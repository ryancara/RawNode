#include "imgio/ImageIO.h"
#include "color/LinearColorTransform.h"
#include "color/TransferFunction.h"
#include "imgio/ImageIOPriv.h"
#include "perf.h"

#include <libraw/libraw.h>
#include <tiffio.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(__F16C__)
#include <immintrin.h>
#endif

#if defined(__ARM_NEON) && !defined(STBI_NEON)
#define STBI_NEON
#endif

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_HDR
#include "stb_image.h"

#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "stb_image_resize2.h"

#include "tinyexr.h"

namespace fs = std::filesystem;

static std::mutex gLibRawDecodeMutex;

void flipRows(float *px, int w, int h) {
  const size_t row = (size_t)w * 4;
  for (int y = 0; y < h / 2; ++y)
    std::swap_ranges(px + (size_t)y * row, px + (size_t)(y + 1) * row, px + (size_t)(h - 1 - y) * row);
}

bool fromRGBAFloatTopDown(float *src, int w, int h, Image &out) {
  out.w = w;
  out.h = h;
  out.px.assign(src, src + (size_t)w * h * 4);
  flipRows(out.px.data(), w, h);
  return true;
}

void applyCameraMatrix(const float camera[4], int channels, const float matrix[3][4], float rgb[3]) {
  const int n = std::clamp(channels, 0, 4);
  for (int row = 0; row < 3; ++row) {
    float v = 0.0f;
    for (int c = 0; c < n; ++c) v += matrix[row][c] * camera[c];
    rgb[row] = v;
  }
}

bool isRawImagePath(const std::string &path) {
  std::string e = fs::path(path).extension().string();
  for (char &c : e) c = (char)tolower((unsigned char)c);
  return e == ".cr2" || e == ".cr3" || e == ".nef" || e == ".arw" || e == ".dng" ||
         e == ".raf" || e == ".orf" || e == ".rw2" || e == ".pef" || e == ".srw" ||
         e == ".raw";
}

bool makeCameraToWorkingMatrix(const float cameraToRec709[3][4], RgbGamut target, float out[3][4]) {
  double workingFromRec709[3][3] = {};
  if (!linearColorTransformMatrix(RgbGamut::Rec709, target, workingFromRec709)) return false;

  for (int row = 0; row < 3; ++row) {
    for (int c = 0; c < 4; ++c) {
      double v = 0.0;
      for (int k = 0; k < 3; ++k) v += workingFromRec709[row][k] * cameraToRec709[k][c];
      out[row][c] = (float)v;
    }
  }
  return true;
}

bool makeCameraToWorkingMatrix(const float cameraToRec709[3][4], ColorSpace target, float out[3][4]) {
  const ColorEncoding encoding = legacyColorSpaceEncoding(target);
  if (encoding.gamma != TransferFunction::Linear) return false;
  return makeCameraToWorkingMatrix(cameraToRec709, encoding.gamut, out);
}

static bool loadRaw(const std::string &path, Image &out, RgbGamut workingGamut, TransferFunction workingGamma) {
  std::lock_guard<std::mutex> lock(gLibRawDecodeMutex);
  LibRaw raw;
  if (raw.open_file(path.c_str()) != LIBRAW_SUCCESS) return false;
  if (raw.unpack() != LIBRAW_SUCCESS) return false;

  // Keep LibRaw responsible for the existing RAW-development stages (including
  // camera WB and demosaic), but stop before its output-colour conversion.
  // RawNode owns the camera-RGB -> working-RGB matrix application below.
  raw.imgdata.params.output_bps = 16;
  raw.imgdata.params.gamm[0] = 1.0;
  raw.imgdata.params.gamm[1] = 1.0;
  raw.imgdata.params.no_auto_bright = 1;
  raw.imgdata.params.use_camera_wb = 1;
  raw.imgdata.params.output_color = 0;

  if (raw.dcraw_process() != LIBRAW_SUCCESS) return false;

  // rgb_cam is the matrix LibRaw would otherwise use for camera RGB -> linear
  // sRGB/Rec.709. Copy it after dcraw_process(): almost all cameras have their
  // final matrix earlier, but a few legacy paths may update it during processing.
  // RawNode then composes this with the selected working-space transform and
  // applies one camera -> working-space matrix in float.
  float cameraToRec709[3][4] = {};
  bool haveMatrix = false;
  for (int row = 0; row < 3; ++row) {
    for (int c = 0; c < 4; ++c) {
      cameraToRec709[row][c] = raw.imgdata.color.rgb_cam[row][c];
      haveMatrix = haveMatrix || cameraToRec709[row][c] != 0.0f;
    }
  }
  // Unknown cameras can have no usable matrix. Preserve LibRaw's effective
  // fallback by treating the first three camera channels as RGB.
  if (!haveMatrix)
    for (int c = 0; c < 3; ++c) cameraToRec709[c][c] = 1.0f;

  float cameraToWorking[3][4] = {};
  if (!makeCameraToWorkingMatrix(cameraToRec709, workingGamut, cameraToWorking)) return false;

  libraw_processed_image_t *img = raw.dcraw_make_mem_image();
  if (!img || img->type != LIBRAW_IMAGE_BITMAP || img->colors < 3 || img->colors > 4) {
    if (img) LibRaw::dcraw_clear_mem(img);
    return false;
  }

  const int w = img->width, h = img->height;
  out.w = w;
  out.h = h;
  out.px.resize((size_t)w * h * 4);

  const auto convertPixel = [&](const auto *src, float scale, float *dst) {
    float camera[4] = {};
    for (int c = 0; c < img->colors; ++c) camera[c] = (float)src[c] * scale;
    float rgb[3];
    applyCameraMatrix(camera, img->colors, cameraToWorking, rgb);
    // Deliberately do not clamp here. The old LibRaw output-colour stage used
    // unsigned 16-bit storage and clipped matrix-created negatives/highlights.
    // Applying the matrix in RawNode float preserves those values.
    dst[0] = (float)encodeTransfer(rgb[0], workingGamma);
    dst[1] = (float)encodeTransfer(rgb[1], workingGamma);
    dst[2] = (float)encodeTransfer(rgb[2], workingGamma);
    dst[3] = 1.0f;
  };

  if (img->bits == 16) {
    const uint16_t *p = reinterpret_cast<const uint16_t *>(img->data);
    const float scale = 1.0f / 65535.0f;
    for (int y = 0; y < h; ++y) {
      const uint16_t *src = p + (size_t)(h - 1 - y) * w * img->colors;
      float *dst = out.px.data() + (size_t)y * w * 4;
      for (int x = 0; x < w; ++x) {
        convertPixel(src, scale, dst);
        src += img->colors;
        dst += 4;
      }
    }
  } else {
    const uint8_t *p = img->data;
    const float scale = 1.0f / 255.0f;
    for (int y = 0; y < h; ++y) {
      const uint8_t *src = p + (size_t)(h - 1 - y) * w * img->colors;
      float *dst = out.px.data() + (size_t)y * w * 4;
      for (int x = 0; x < w; ++x) {
        convertPixel(src, scale, dst);
        src += img->colors;
        dst += 4;
      }
    }
  }

  LibRaw::dcraw_clear_mem(img);
  return true;
}

static bool loadExr(const std::string &path, Image &out) {
  float *rgba = nullptr;
  int w = 0, h = 0;
  const char *err = nullptr;
  if (LoadEXR(&rgba, &w, &h, path.c_str(), &err) != TINYEXR_SUCCESS) {
    if (err) FreeEXRErrorMessage(err);
    return false;
  }
  fromRGBAFloatTopDown(rgba, w, h, out);
  free(rgba);
  return true;
}

static float halfToFloat(uint16_t h) {
#if defined(__F16C__)
  return _cvtsh_ss(h);
#else
  const uint32_t sign = (uint32_t)(h >> 15) << 31;
  uint32_t exp = (h >> 10) & 0x1f;
  uint32_t mant = h & 0x3ff;
  uint32_t bits;
  if (exp == 0) {
    if (mant == 0) {
      bits = sign;
    } else {
      exp = 127 - 15 + 1;
      while ((mant & 0x400) == 0) {
        mant <<= 1;
        --exp;
      }
      bits = sign | (exp << 23) | ((mant & 0x3ff) << 13);
    }
  } else if (exp == 31) {
    bits = sign | 0x7f800000u | (mant << 13);
  } else {
    bits = sign | ((exp + 127 - 15) << 23) | (mant << 13);
  }
  float f;
  std::memcpy(&f, &bits, sizeof f);
  return f;
#endif
}

static bool loadTiffScanline(TIFF *tif, uint32_t w, uint32_t h, uint16_t bps, uint16_t spp, uint16_t sf,
                             Image &out) {
  const tsize_t rowBytes = TIFFScanlineSize(tif);
  if (rowBytes <= 0) return false;
  std::vector<uint8_t> row((size_t)rowBytes);
  out.w = (int)w;
  out.h = (int)h;
  out.px.assign((size_t)w * h * 4, 0.0f);
  for (uint32_t y = 0; y < h; ++y) {
    if (TIFFReadScanline(tif, row.data(), y, 0) < 0) return false;
    float *dst = out.px.data() + (size_t)(h - 1 - y) * w * 4;
    if (bps == 8 && sf == SAMPLEFORMAT_UINT) {
      const uint8_t *src = row.data();
      const float s = 1.0f / 255.0f;
      for (uint32_t x = 0; x < w; ++x) {
        dst[0] = src[0] * s;
        dst[1] = (spp > 1 ? src[1] : src[0]) * s;
        dst[2] = (spp > 2 ? src[2] : src[0]) * s;
        dst[3] = spp > 3 ? src[3] * s : 1.0f;
        src += spp;
        dst += 4;
      }
    } else if (bps == 16 && sf == SAMPLEFORMAT_UINT) {
      const uint16_t *src = reinterpret_cast<const uint16_t *>(row.data());
      const float s = 1.0f / 65535.0f;
      for (uint32_t x = 0; x < w; ++x) {
        dst[0] = src[0] * s;
        dst[1] = (spp > 1 ? src[1] : src[0]) * s;
        dst[2] = (spp > 2 ? src[2] : src[0]) * s;
        dst[3] = spp > 3 ? src[3] * s : 1.0f;
        src += spp;
        dst += 4;
      }
    } else if (bps == 16 && sf == SAMPLEFORMAT_IEEEFP) {
      const uint16_t *src = reinterpret_cast<const uint16_t *>(row.data());
      for (uint32_t x = 0; x < w; ++x) {
        dst[0] = halfToFloat(src[0]);
        dst[1] = halfToFloat(spp > 1 ? src[1] : src[0]);
        dst[2] = halfToFloat(spp > 2 ? src[2] : src[0]);
        dst[3] = spp > 3 ? halfToFloat(src[3]) : 1.0f;
        src += spp;
        dst += 4;
      }
    } else if (bps == 32 && sf == SAMPLEFORMAT_IEEEFP) {
      const float *src = reinterpret_cast<const float *>(row.data());
      for (uint32_t x = 0; x < w; ++x) {
        dst[0] = src[0];
        dst[1] = spp > 1 ? src[1] : src[0];
        dst[2] = spp > 2 ? src[2] : src[0];
        dst[3] = spp > 3 ? src[3] : 1.0f;
        src += spp;
        dst += 4;
      }
    } else {
      return false;
    }
  }
  return true;
}

static bool loadTiffRgba(TIFF *tif, uint32_t w, uint32_t h, Image &out) {
  std::vector<uint32_t> raster((size_t)w * h);
  if (!TIFFReadRGBAImageOriented(tif, w, h, raster.data(), ORIENTATION_TOPLEFT, 0)) return false;
  out.w = (int)w;
  out.h = (int)h;
  out.px.resize((size_t)w * h * 4);
  const float s = 1.0f / 255.0f;
  for (uint32_t y = 0; y < h; ++y) {
    float *dst = out.px.data() + (size_t)(h - 1 - y) * w * 4;
    const uint32_t *src = raster.data() + (size_t)y * w;
    for (uint32_t x = 0; x < w; ++x) {
      const uint32_t p = src[x];
      dst[0] = TIFFGetR(p) * s;
      dst[1] = TIFFGetG(p) * s;
      dst[2] = TIFFGetB(p) * s;
      dst[3] = TIFFGetA(p) * s;
      dst += 4;
    }
  }
  return true;
}

static bool loadTiff(const std::string &path, Image &out, std::vector<uint8_t> &icc, bool &isFloat) {
  icc.clear();
  isFloat = false;
  TIFF *tif = TIFFOpen(path.c_str(), "r");
  if (!tif) return false;
  uint32_t w = 0, h = 0;
  uint16_t bps = 8, spp = 3, sf = SAMPLEFORMAT_UINT, planar = PLANARCONFIG_CONTIG, orient = ORIENTATION_TOPLEFT;
  TIFFGetField(tif, TIFFTAG_IMAGEWIDTH, &w);
  TIFFGetField(tif, TIFFTAG_IMAGELENGTH, &h);
  TIFFGetFieldDefaulted(tif, TIFFTAG_BITSPERSAMPLE, &bps);
  TIFFGetFieldDefaulted(tif, TIFFTAG_SAMPLESPERPIXEL, &spp);
  TIFFGetFieldDefaulted(tif, TIFFTAG_SAMPLEFORMAT, &sf);
  TIFFGetFieldDefaulted(tif, TIFFTAG_PLANARCONFIG, &planar);
  TIFFGetFieldDefaulted(tif, TIFFTAG_ORIENTATION, &orient);
  uint32_t iccLen = 0;
  void *iccPtr = nullptr;
  if (TIFFGetField(tif, TIFFTAG_ICCPROFILE, &iccLen, &iccPtr) && iccPtr && iccLen > 0)
    icc.assign((const uint8_t *)iccPtr, (const uint8_t *)iccPtr + iccLen);
  isFloat = (sf == SAMPLEFORMAT_IEEEFP);
  bool ok = false;
  if (w && h && !TIFFIsTiled(tif) && planar == PLANARCONFIG_CONTIG && spp >= 1 && spp <= 4 &&
      orient == ORIENTATION_TOPLEFT &&
      ((bps == 8 && sf == SAMPLEFORMAT_UINT) || (bps == 16 && sf == SAMPLEFORMAT_UINT) ||
       (bps == 16 && sf == SAMPLEFORMAT_IEEEFP) || (bps == 32 && sf == SAMPLEFORMAT_IEEEFP))) {
    ok = loadTiffScanline(tif, w, h, bps, spp, sf, out);
  }
  if (!ok && w && h) {
    out = {};
    ok = loadTiffRgba(tif, w, h, out);
    isFloat = false;  // RGBA path is 8-bit
  }
  TIFFClose(tif);
  if (!ok) {
    out = {};
    icc.clear();
  }
  return ok;
}

static bool loadStb(const std::string &path, Image &out) {
  int w = 0, h = 0, n = 0;
  float *data = stbi_loadf(path.c_str(), &w, &h, &n, 4);
  if (!data) return false;
  fromRGBAFloatTopDown(data, w, h, out);
  stbi_image_free(data);
  return true;
}

static bool downscaleRGBA8(const unsigned char *src, int sw, int sh, int maxEdge, std::vector<unsigned char> &dst,
                           int &dw, int &dh) {
  if (sw <= 0 || sh <= 0) return false;
  const int longE = std::max(sw, sh);
  if (maxEdge <= 0 || longE <= maxEdge) {
    dw = sw;
    dh = sh;
    dst.assign(src, src + (size_t)sw * sh * 4);
    return true;
  }
  const double scale = (double)maxEdge / longE;
  dw = std::max(1, (int)std::floor(sw * scale));
  dh = std::max(1, (int)std::floor(sh * scale));
  dst.resize((size_t)dw * dh * 4);
  stbir_resize_uint8_linear(src, sw, sh, sw * 4, dst.data(), dw, dh, dw * 4, STBIR_RGBA);
  return true;
}

static bool loadStbThumbRGBA(const std::string &path, int maxEdge, std::vector<unsigned char> &rgba, int &w, int &h) {
  int iw = 0, ih = 0, n = 0;
  unsigned char *data = stbi_load(path.c_str(), &iw, &ih, &n, 4);
  if (!data) return false;
  const bool ok = downscaleRGBA8(data, iw, ih, maxEdge, rgba, w, h);
  stbi_image_free(data);
  return ok;
}

static bool loadRawEmbeddedThumbRGBA(const std::string &path, int maxEdge, std::vector<unsigned char> &rgba, int &w,
                                     int &h) {
  std::lock_guard<std::mutex> lock(gLibRawDecodeMutex);
  LibRaw raw;
  if (raw.open_file(path.c_str()) != LIBRAW_SUCCESS) return false;
  if (raw.unpack_thumb() != LIBRAW_SUCCESS) return false;
  const libraw_thumbnail_t &t = raw.imgdata.thumbnail;
  if (t.tlength <= 0 || !t.thumb) return false;

  std::vector<unsigned char> decoded;
  int tw = 0, th = 0;
  if (t.tformat == LIBRAW_THUMBNAIL_JPEG) {
    int n = 0;
    unsigned char *jd =
        stbi_load_from_memory(reinterpret_cast<const unsigned char *>(t.thumb), t.tlength, &tw, &th, &n, 4);
    if (!jd) return false;
    decoded.assign(jd, jd + (size_t)tw * th * 4);
    stbi_image_free(jd);
  } else if (t.tformat == LIBRAW_THUMBNAIL_BITMAP) {
    tw = t.twidth;
    th = t.theight;
    const int tc = t.tcolors >= 3 ? t.tcolors : 3;
    if (tw <= 0 || th <= 0 || tc > 4) return false;
    if ((size_t)t.tlength < (size_t)tw * th * (size_t)tc) return false;
    decoded.resize((size_t)tw * th * 4);
    const unsigned char *src = reinterpret_cast<const unsigned char *>(t.thumb);
    for (int y = 0; y < th; ++y) {
      for (int x = 0; x < tw; ++x) {
        const int si = (y * tw + x) * tc;
        const int di = (y * tw + x) * 4;
        decoded[(size_t)di] = src[si];
        decoded[(size_t)di + 1] = src[si + 1];
        decoded[(size_t)di + 2] = src[si + 2];
        decoded[(size_t)di + 3] = tc >= 4 ? src[si + 3] : 255;
      }
    }
  } else if (t.tformat == LIBRAW_THUMBNAIL_BITMAP16) {
    tw = t.twidth;
    th = t.theight;
    const int tc = t.tcolors >= 3 ? t.tcolors : 3;
    if (tw <= 0 || th <= 0 || tc > 4) return false;
    if ((size_t)t.tlength < (size_t)tw * th * (size_t)tc * 2) return false;
    decoded.resize((size_t)tw * th * 4);
    const uint16_t *src = reinterpret_cast<const uint16_t *>(t.thumb);
    for (int y = 0; y < th; ++y) {
      for (int x = 0; x < tw; ++x) {
        const int si = (y * tw + x) * tc;
        const int di = (y * tw + x) * 4;
        for (int c = 0; c < 3; ++c) decoded[(size_t)di + c] = (unsigned char)(src[si + c] >> 8);
        decoded[(size_t)di + 3] = tc >= 4 ? (unsigned char)(src[si + 3] >> 8) : 255;
      }
    }
  } else {
    return false;
  }
  return downscaleRGBA8(decoded.data(), tw, th, maxEdge, rgba, w, h);
}

bool makePreview(const Image &src, int maxEdge, Image &out) {
  PerfScope _ps("makePreview");
  if (src.w <= 0 || src.h <= 0 || src.px.empty()) return false;
  const int longEdge = std::max(src.w, src.h);
  if (maxEdge <= 0 || longEdge <= maxEdge) {
    out = src;
    return true;
  }
  const double scale = (double)maxEdge / longEdge;
  const int w = std::max(1, (int)std::floor(src.w * scale));
  const int h = std::max(1, (int)std::floor(src.h * scale));
  out.w = w;
  out.h = h;
  out.px.resize((size_t)w * h * 4);

  const unsigned int nThreads = std::min(std::max(1u, std::thread::hardware_concurrency()), 4u);
  if (nThreads > 1 && longEdge >= 512) {
    STBIR_RESIZE rs;
    stbir_resize_init(&rs, src.px.data(), src.w, src.h, 0, out.px.data(), w, h, 0, STBIR_RGBA, STBIR_TYPE_FLOAT);
    if (stbir_build_samplers_with_splits(&rs, (int)nThreads)) {
      std::vector<std::thread> threads;
      threads.reserve(nThreads);
      for (unsigned int i = 0; i < nThreads; ++i)
        threads.emplace_back([&rs, i] { stbir_resize_extended_split(&rs, (int)i, 1); });
      for (auto &t : threads) t.join();
      stbir_free_samplers(&rs);
    } else {
      stbir_resize_float_linear(src.px.data(), src.w, src.h, 0, out.px.data(), w, h, 0, STBIR_RGBA);
    }
  } else {
    stbir_resize_float_linear(src.px.data(), src.w, src.h, 0, out.px.data(), w, h, 0, STBIR_RGBA);
  }
  return true;
}

static ColorEncoding rasterBufferEncoding(ColorSpace tag) {
  ColorEncoding encoding = legacyColorSpaceEncoding(tag);
  // Raster loaders feed processing buffers as linear float. Preserve the
  // detected primaries while describing the actual pixels in memory.
  encoding.gamma = TransferFunction::Linear;
  return encoding;
}

bool loadImage(const std::string &path, Image &out, ColorEncoding &detectedEncoding,
               bool &decodedRaw, ColorEncoding rawWorkingEncoding) {
  PerfScope _ps("loadImage");
  out = {};
  decodedRaw = false;
  detectedEncoding = {RgbGamut::Rec709, TransferFunction::Linear};

  std::string e = fs::path(path).extension().string();
  for (char &ch : e) ch = (char)tolower((unsigned char)ch);

  if (e == ".exr") {
    if (!loadExr(path, out)) return false;
    detectedEncoding = {RgbGamut::Rec2020, TransferFunction::Linear};
    return true;
  }

  if (e == ".tif" || e == ".tiff") {
    std::vector<uint8_t> icc;
    bool isFloat = false;
    if (!loadTiff(path, out, icc, isFloat)) return false;
    const ColorSpace tag =
        !icc.empty() ? classifyIcc(icc)
                     : (isFloat ? ColorSpace::LinearRec2020 : ColorSpace::sRGB);
    detectedEncoding = rasterBufferEncoding(tag);
    return true;
  }

  std::vector<uint8_t> icc;
  if (e == ".png") extractPngIcc(path, icc);
  else if (e == ".jpg" || e == ".jpeg") extractJpgIcc(path, icc);

  if (loadStb(path, out)) {
    const ColorSpace tag = !icc.empty() ? classifyIcc(icc) : ColorSpace::sRGB;
    detectedEncoding = rasterBufferEncoding(tag);
    return true;
  }

  // LibRaw is the final decoder fallback regardless of filename extension.
  // The successful decoder, not an extension allow-list, determines RAW state.
  if (loadRaw(path, out, rawWorkingEncoding.gamut, rawWorkingEncoding.gamma)) {
    decodedRaw = true;
    detectedEncoding = rawWorkingEncoding;
    return true;
  }

  return false;
}

bool loadImage(const std::string &path, Image &out, ColorSpace &detected,
               RgbGamut rawGamut, TransferFunction rawGamma) {
  ColorEncoding encoding;
  bool decodedRaw = false;
  if (!loadImage(path, out, encoding, decodedRaw, {rawGamut, rawGamma})) return false;

  // Legacy callers cannot describe every encoding. Return an exact legacy tag
  // where possible; otherwise retain the historical Linear Rec.2020 stand-in.
  if (!legacyColorSpaceFromEncoding(encoding, detected))
    detected = ColorSpace::LinearRec2020;
  return true;
}

bool loadImage(const std::string &path, Image &out, ColorSpace &detected, ColorSpace rawWorkingSpace) {
  const ColorEncoding rawEncoding = legacyColorSpaceEncoding(rawWorkingSpace);
  if (rawEncoding.gamma != TransferFunction::Linear) return false;
  return loadImage(path, out, detected, rawEncoding.gamut, rawEncoding.gamma);
}

bool loadThumbnailRGBA(const std::string &path, int maxEdge, std::vector<unsigned char> &rgba, int &w, int &h) {
  if (maxEdge <= 0) maxEdge = 128;
  std::string e = fs::path(path).extension().string();
  for (char &c : e) c = (char)tolower((unsigned char)c);

  if (isRawImagePath(path)) return loadRawEmbeddedThumbRGBA(path, maxEdge, rgba, w, h);
  if (e == ".png" || e == ".jpg" || e == ".jpeg") return loadStbThumbRGBA(path, maxEdge, rgba, w, h);
  return false;
}
