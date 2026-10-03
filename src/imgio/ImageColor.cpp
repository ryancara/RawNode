#include "imgio/ImageIO.h"
#include "imgio/ImageIOPriv.h"
#include "color/LinearColorTransform.h"
#include "color/TransferFunction.h"
#include "perf.h"

#include <lcms2.h>
#include <zlib.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

static cmsToneCurve *srgbCurve() {
  // Same parametric curve cmsCreate_sRGBProfile uses.
  cmsFloat64Number params[5] = {2.4, 1.0 / 1.055, 0.055 / 1.055, 1.0 / 12.92, 0.04045};
  return cmsBuildParametricToneCurve(nullptr, 4, params);
}

static cmsToneCurve *toneCurve(TransferFunction tf) {
  if (tf == TransferFunction::Linear) return cmsBuildGamma(nullptr, 1.0);
  if (tf == TransferFunction::SRGB) return srgbCurve();

  // lcms has no native DaVinci Intermediate or exact BT.709-camera curve type.
  // Build their decode TRCs from the same TransferFunction implementation used
  // by the CST, so ICC metadata and RawNode maths share one definition.
  constexpr int kSamples = 4096;
  std::array<cmsFloat32Number, kSamples> samples{};
  for (int i = 0; i < kSamples; ++i) {
    const double encoded = (double)i / (kSamples - 1);
    double linear = decodeTransfer(encoded, tf);
    if (!std::isfinite(linear)) linear = 0.0;
    samples[(size_t)i] = (cmsFloat32Number)linear;
  }
  return cmsBuildTabulatedToneCurveFloat(nullptr, kSamples, samples.data());
}

static cmsHPROFILE makeProfile(ColorEncoding encoding) {
  const auto &def = rgbGamutDefinition(encoding.gamut);
  const cmsCIExyY white = {def.whiteX, def.whiteY, 1.0};
  const cmsCIExyYTRIPLE primaries = {
      {def.redX, def.redY, 1.0},
      {def.greenX, def.greenY, 1.0},
      {def.blueX, def.blueY, 1.0},
  };

  cmsToneCurve *trc = toneCurve(encoding.gamma);
  if (!trc) return nullptr;
  cmsToneCurve *curves[3] = {trc, trc, trc};
  cmsHPROFILE profile = cmsCreateRGBProfile(&white, &primaries, curves);
  cmsFreeToneCurve(trc);
  if (!profile) return nullptr;

  const std::string description =
      std::string("RawNode ") + rgbGamutId(encoding.gamut) + " / " + transferFunctionId(encoding.gamma);
  cmsMLU *mlu = cmsMLUalloc(nullptr, 1);
  if (mlu) {
    cmsMLUsetASCII(mlu, "en", "US", description.c_str());
    cmsWriteTag(profile, cmsSigProfileDescriptionTag, mlu);
    cmsMLUfree(mlu);
  }
  return profile;
}

static cmsHPROFILE srgbProfile() {
  static cmsHPROFILE p = cmsCreate_sRGBProfile();
  return p;
}

static cmsHTRANSFORM linearRec709DisplayTransform() {
  static cmsHTRANSFORM transform = [] {
    cmsHPROFILE src = makeProfile({RgbGamut::Rec709, TransferFunction::Linear});
    cmsHPROFILE dst = srgbProfile();
    if (!src || !dst) {
      if (src) cmsCloseProfile(src);
      return (cmsHTRANSFORM)nullptr;
    }
    cmsHTRANSFORM result =
        cmsCreateTransform(src, TYPE_RGBA_FLT, dst, TYPE_RGBA_8,
                           INTENT_RELATIVE_COLORIMETRIC,
                           cmsFLAGS_NOCACHE | cmsFLAGS_COPY_ALPHA);
    cmsCloseProfile(src);
    return result;
  }();
  return transform;
}

bool profileBytes(ColorEncoding encoding, std::vector<uint8_t> &out) {
  out.clear();
  cmsHPROFILE p = makeProfile(encoding);
  if (!p) return false;

  cmsUInt32Number n = 0;
  bool ok = cmsSaveProfileToMem(p, nullptr, &n) && n > 0;
  if (ok) {
    out.resize(n);
    ok = cmsSaveProfileToMem(p, out.data(), &n) != 0;
    if (ok) out.resize(n);
  }
  cmsCloseProfile(p);
  if (!ok) out.clear();
  return ok;
}

static std::string lowerCopy(const char *s) {
  std::string o;
  if (!s) return o;
  for (; *s; ++s) o.push_back((char)tolower((unsigned char)*s));
  return o;
}

bool extractPngIcc(const std::string &path, std::vector<uint8_t> &icc) {
  icc.clear();
  FILE *f = fopen(path.c_str(), "rb");
  if (!f) return false;
  uint8_t sig[8];
  if (fread(sig, 1, 8, f) != 8 || std::memcmp(sig, "\x89PNG\r\n\x1a\n", 8) != 0) {
    fclose(f);
    return false;
  }
  bool ok = false;
  for (;;) {
    uint8_t lenb[4], type[4];
    if (fread(lenb, 1, 4, f) != 4 || fread(type, 1, 4, f) != 4) break;
    const uint32_t len = ((uint32_t)lenb[0] << 24) | ((uint32_t)lenb[1] << 16) | ((uint32_t)lenb[2] << 8) | lenb[3];
    if (std::memcmp(type, "IEND", 4) == 0) break;
    if (std::memcmp(type, "iCCP", 4) == 0 && len > 2 && len < 64u * 1024u * 1024u) {
      std::vector<uint8_t> chunk(len);
      if (fread(chunk.data(), 1, len, f) != len) break;
      fseek(f, 4, SEEK_CUR);  // CRC
      size_t i = 0;
      while (i < chunk.size() && chunk[i]) ++i;
      if (i + 2 >= chunk.size() || chunk[i + 1] != 0) break;
      const uint8_t *comp = chunk.data() + i + 2;
      const uLong compLen = (uLong)(chunk.size() - (i + 2));
      uLongf destLen = compLen * 4 + 65536;
      for (int attempt = 0; attempt < 8; ++attempt) {
        icc.resize(destLen);
        const int z = uncompress(icc.data(), &destLen, comp, compLen);
        if (z == Z_OK) {
          icc.resize(destLen);
          ok = !icc.empty();
          break;
        }
        if (z != Z_BUF_ERROR) {
          icc.clear();
          break;
        }
        destLen *= 2;
      }
      break;
    }
    if (fseek(f, (long)len + 4, SEEK_CUR) != 0) break;
  }
  fclose(f);
  if (!ok) icc.clear();
  return ok;
}

bool extractJpgIcc(const std::string &path, std::vector<uint8_t> &icc) {
  icc.clear();
  FILE *f = fopen(path.c_str(), "rb");
  if (!f) return false;
  if (fseek(f, 0, SEEK_END) != 0) {
    fclose(f);
    return false;
  }
  const long sz = ftell(f);
  if (sz < 4 || sz > 256L * 1024L * 1024L) {
    fclose(f);
    return false;
  }
  if (fseek(f, 0, SEEK_SET) != 0) {
    fclose(f);
    return false;
  }
  std::vector<uint8_t> data((size_t)sz);
  if (fread(data.data(), 1, data.size(), f) != data.size()) {
    fclose(f);
    return false;
  }
  fclose(f);
  if (data[0] != 0xff || data[1] != 0xd8) return false;

  std::vector<std::vector<uint8_t>> parts;
  int expected = -1;
  size_t i = 2;
  while (i + 4 <= data.size()) {
    if (data[i] != 0xff) {
      ++i;
      continue;
    }
    while (i < data.size() && data[i] == 0xff) ++i;
    if (i >= data.size()) break;
    const uint8_t marker = data[i++];
    if (marker == 0xd9 || marker == 0xda) break;  // EOI / SOS
    if (marker >= 0xd0 && marker <= 0xd7) continue;  // RSTn
    if (i + 2 > data.size()) break;
    const uint16_t seglen = (uint16_t)((data[i] << 8) | data[i + 1]);
    if (seglen < 2 || i + seglen > data.size()) break;
    if (marker == 0xe2 && seglen >= 16) {
      const uint8_t *p = data.data() + i + 2;
      if (std::memcmp(p, "ICC_PROFILE\0", 12) == 0) {
        const int seq = p[12], cnt = p[13];
        if (seq >= 1 && cnt >= 1) {
          if (expected < 0) {
            expected = cnt;
            parts.assign((size_t)cnt, {});
          }
          if (cnt == expected && seq <= expected)
            parts[(size_t)seq - 1].assign(p + 14, p + seglen - 2);
        }
      }
    }
    i += seglen;
  }
  if (expected <= 0) return false;
  size_t total = 0;
  for (const auto &p : parts) {
    if (p.empty()) return false;
    total += p.size();
  }
  icc.reserve(total);
  for (const auto &p : parts) icc.insert(icc.end(), p.begin(), p.end());
  return !icc.empty();
}

bool rawNodeIccEncoding(const std::vector<uint8_t> &icc, ColorEncoding &encoding) {
  if (icc.empty()) return false;
  cmsHPROFILE profile = cmsOpenProfileFromMem(icc.data(), (cmsUInt32Number)icc.size());
  if (!profile) return false;

  char desc[256] = {};
  cmsGetProfileInfoASCII(profile, cmsInfoDescription, "en", "US", desc, sizeof desc);
  const std::string description = lowerCopy(desc);

  bool ok = false;
  if (description.rfind("rawnode ", 0) == 0) {
    const std::string payload = description.substr(8);
    const size_t sep = payload.find(" / ");
    if (sep != std::string::npos && payload.find(" / ", sep + 3) == std::string::npos) {
      const std::string gamutId = payload.substr(0, sep);
      const std::string transferId = payload.substr(sep + 3);
      ok = colorEncodingFromIds(gamutId, transferId, encoding);
    }
  }

  cmsCloseProfile(profile);
  return ok;
}

bool convertIccToLinearRec2020(Image &img, const std::vector<uint8_t> &icc) {
  if (icc.empty() || img.w <= 0 || img.h <= 0 || img.px.empty()) return false;

  cmsHPROFILE source = cmsOpenProfileFromMem(icc.data(), (cmsUInt32Number)icc.size());
  if (!source) return false;
  if (cmsGetColorSpace(source) != cmsSigRgbData) {
    cmsCloseProfile(source);
    return false;
  }

  cmsHPROFILE target = makeProfile({RgbGamut::Rec2020, TransferFunction::Linear});
  if (!target) {
    cmsCloseProfile(source);
    return false;
  }

  cmsHTRANSFORM transform =
      cmsCreateTransform(source, TYPE_RGBA_FLT,
                         target, TYPE_RGBA_FLT,
                         INTENT_RELATIVE_COLORIMETRIC,
                         cmsFLAGS_NOCACHE | cmsFLAGS_COPY_ALPHA);
  cmsCloseProfile(target);
  cmsCloseProfile(source);
  if (!transform) return false;

  std::vector<float> converted(img.px.size());
  cmsDoTransform(transform, img.px.data(), converted.data(),
                 (cmsUInt32Number)((size_t)img.w * img.h));
  cmsDeleteTransform(transform);

  img.px.swap(converted);
  return true;
}

static void linearRec709ToDisplayRGBA8(const Image &img, std::vector<unsigned char> &out) {
  PerfScope _ps("toDisplayRGBA8");
  out.assign((size_t)img.w * img.h * 4, 0);
  if (img.w <= 0 || img.h <= 0) return;

  cmsHTRANSFORM xform = linearRec709DisplayTransform();
  const int rowFloats = img.w * 4;
  if (xform) {
    for (int y = 0; y < img.h; ++y) {
      const float *src = img.px.data() + (size_t)(img.h - 1 - y) * rowFloats;
      unsigned char *dst = out.data() + (size_t)y * rowFloats;
      cmsDoTransform(xform, src, dst, (cmsUInt32Number)img.w);
    }
    return;
  }

  for (int y = 0; y < img.h; ++y) {
    const float *src = img.px.data() + (size_t)(img.h - 1 - y) * rowFloats;
    unsigned char *dst = out.data() + (size_t)y * rowFloats;
    for (int x = 0; x < img.w; ++x) {
      for (int channel = 0; channel < 3; ++channel) {
        const double encoded = encodeTransfer(src[channel], TransferFunction::SRGB);
        const double clamped = std::clamp(encoded, 0.0, 1.0);
        dst[channel] = (unsigned char)std::lround(clamped * 255.0);
      }
      const double alpha = std::clamp((double)src[3], 0.0, 1.0);
      dst[3] = (unsigned char)std::lround(alpha * 255.0);
      src += 4;
      dst += 4;
    }
  }
}

void toDisplayRGBA8(const Image &img, ColorEncoding encoding,
                    std::vector<unsigned char> &out) {
  if (img.w <= 0 || img.h <= 0 || img.px.empty()) {
    out.clear();
    return;
  }

  double toRec709[3][3] = {};
  if (!linearColorTransformMatrix(encoding.gamut, RgbGamut::Rec709, toRec709)) {
    out.clear();
    return;
  }

  Image rec709 = img;
  for (size_t i = 0; i + 3 < rec709.px.size(); i += 4) {
    const float linear[3] = {
        (float)decodeTransfer(img.px[i + 0], encoding.gamma),
        (float)decodeTransfer(img.px[i + 1], encoding.gamma),
        (float)decodeTransfer(img.px[i + 2], encoding.gamma),
    };
    float converted[3] = {};
    applyLinearColorMatrix(toRec709, linear, converted);
    rec709.px[i + 0] = std::isfinite(converted[0]) ? converted[0] : 0.0f;
    rec709.px[i + 1] = std::isfinite(converted[1]) ? converted[1] : 0.0f;
    rec709.px[i + 2] = std::isfinite(converted[2]) ? converted[2] : 0.0f;
  }

  linearRec709ToDisplayRGBA8(rec709, out);
}
