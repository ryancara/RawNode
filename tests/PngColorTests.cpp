#include "PngColorTests.h"

#include "imgio/ImageIOPriv.h"

#include <lcms2.h>
#include <zlib.h>

#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>

#define CHECK(expression) do { if (!(expression)) { \
  std::fprintf(stderr, "PNG colour check failed at line %d: %s\n", __LINE__, #expression); return false; \
} } while (false)

namespace {
namespace fs = std::filesystem;
using Bytes = std::vector<uint8_t>;
using Profile = std::unique_ptr<void, decltype(&cmsCloseProfile)>;

struct Files {
  fs::path dir = fs::temp_directory_path() /
      ("rawnode-png-colour-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  Files() { fs::create_directory(dir); }
  ~Files() { std::error_code ignored; fs::remove_all(dir, ignored); }
};

uint32_t be32(const uint8_t *p) {
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

struct Png {
  uint32_t w = 0, h = 0;
  Bytes profile, idat;
};

// Inspect bytes directly: no RawNode ICC extractor or stb PNG reader. The PNG
// specification requires iCCP before both PLTE and IDAT, and consecutive IDATs.
bool inspect(const fs::path &path, bool tagged, Png &png) {
  std::ifstream in(path, std::ios::binary);
  CHECK(in);
  const Bytes bytes{std::istreambuf_iterator<char>(in), {}};
  CHECK(bytes.size() >= 8 && std::memcmp(bytes.data(), "\x89PNG\r\n\x1a\n", 8) == 0);
  bool header = false, palette = false, data = false, dataEnded = false, end = false;
  unsigned profiles = 0;
  for (size_t offset = 8; offset < bytes.size();) {
    CHECK(bytes.size() - offset >= 12);
    const uint32_t length = be32(bytes.data() + offset);
    CHECK(length <= 0x7fffffffu && length <= bytes.size() - offset - 12);
    const uint8_t *type = bytes.data() + offset + 4;
    const uint8_t *payload = type + 4;
    for (int i = 0; i < 4; ++i)
      CHECK((type[i] >= 'A' && type[i] <= 'Z') || (type[i] >= 'a' && type[i] <= 'z'));
    CHECK(type[2] >= 'A' && type[2] <= 'Z'); // Reserved bit.
    const auto crc = crc32(0, type, length + 4);
    CHECK(crc == be32(payload + length));
    const std::string name(reinterpret_cast<const char *>(type), 4);
    CHECK(header || name == "IHDR");
    if (data && name != "IDAT") dataEnded = true;
    if (name == "IHDR") {
      CHECK(!header && offset == 8 && length == 13);
      png.w = be32(payload); png.h = be32(payload + 4);
      CHECK(png.w > 0 && png.h > 0);
      CHECK(payload[8] == 8 && payload[9] == 6); // RGBA8.
      CHECK(payload[10] == 0 && payload[11] == 0 && payload[12] == 0);
      header = true;
    } else if (name == "iCCP") {
      CHECK(tagged && ++profiles == 1);
      CHECK(!data && !palette);
      // PNG keyword, NUL separator, then compression method 0 (zlib/deflate).
      constexpr char prefix[] = "ICC Profile\0";
      CHECK(length > sizeof prefix && std::memcmp(payload, prefix, sizeof prefix) == 0);
      // Profiles in these deterministic fixtures are small; bound decompression.
      Bytes recovered(64 * 1024);
      uLongf size = recovered.size();
      uLong compressed = length - sizeof prefix;
      CHECK(uncompress2(recovered.data(), &size, payload + sizeof prefix, &compressed) == Z_OK);
      CHECK(compressed == length - sizeof prefix && size >= 128);
      recovered.resize(size);
      CHECK(be32(recovered.data()) == size && std::memcmp(recovered.data() + 36, "acsp", 4) == 0);
      png.profile = std::move(recovered);
    } else if (name == "PLTE") {
      CHECK(!palette && !data && length > 0 && length <= 768 && length % 3 == 0);
      CHECK(!tagged || profiles == 1);
      palette = true;
    } else if (name == "IDAT") {
      CHECK(!dataEnded);
      CHECK(!tagged || profiles == 1);
      data = true;
      png.idat.insert(png.idat.end(), payload, payload + length);
    } else if (name == "IEND") {
      CHECK(data && length == 0 && offset + 12 == bytes.size());
      end = true;
    } else {
      // Current writer's policy: ICC only for tagged output, no colour chunks
      // at all for untagged output. In particular, never imply sRGB or gamma.
      CHECK(name != "sRGB" && name != "gAMA" && name != "cHRM" && name != "cICP");
      CHECK(type[0] >= 'a' && type[0] <= 'z'); // No unknown critical chunks.
    }
    offset += size_t(length) + 12;
  }
  CHECK(header && data && end && profiles == unsigned(tagged));
  return true;
}

// Independent PNG scanline reconstruction from zlib data and the five filters
// defined in the PNG specification (4 bytes/pixel, non-interlaced RGBA8).
bool decodePixels(const Png &png, Bytes &pixels) {
  const size_t stride = size_t(png.w) * 4;
  Bytes rows((stride + 1) * png.h);
  uLongf size = rows.size();
  uLong compressed = png.idat.size();
  CHECK(uncompress2(rows.data(), &size, png.idat.data(), &compressed) == Z_OK);
  CHECK(size == rows.size() && compressed == png.idat.size());
  pixels.assign(stride * png.h, 0);
  for (size_t y = 0; y < png.h; ++y) {
    const unsigned filter = rows[y * (stride + 1)];
    CHECK(filter <= 4);
    for (size_t x = 0; x < stride; ++x) {
      const int left = x >= 4 ? pixels[y * stride + x - 4] : 0;
      const int up = y ? pixels[(y - 1) * stride + x] : 0;
      const int upperLeft = y && x >= 4 ? pixels[(y - 1) * stride + x - 4] : 0;
      int prediction = 0;
      if (filter == 1) prediction = left;
      if (filter == 2) prediction = up;
      if (filter == 3) prediction = (left + up) / 2;
      if (filter == 4) {
        const int p = left + up - upperLeft;
        const int a = std::abs(p - left), b = std::abs(p - up), c = std::abs(p - upperLeft);
        prediction = a <= b && a <= c ? left : (b <= c ? up : upperLeft);
      }
      pixels[y * stride + x] = uint8_t(rows[y * (stride + 1) + x + 1] + prediction);
    }
  }
  return true;
}

Image referenceImage() {
  Image image;
  image.w = 4; image.h = 2;
  const float nan = std::numeric_limits<float>::quiet_NaN();
  const float inf = std::numeric_limits<float>::infinity();
  // Already in the declared output encoding, as required by the manual Output
  // interpretation contract. Export must not apply an extra transfer function.
  image.px = {
      -0.1f, 0.18f, 1.1f, 0.0f,   0.5f, 0.0031308f, 1.0f, 0.5f,
      nan, inf, -inf, 1.0f,        0.25f, 0.75f, 0.999f, 1.1f,
      1.0f, 0.0f, 0.0f, 1.0f,     0.0f, 1.0f, 0.0f, 0.25f,
      0.0f, 0.0f, 1.0f, 0.75f,    0.499f / 255, 0.5f / 255, 0.501f / 255, -0.1f,
  };
  return image;
}

// Hand-calculated top-down bytes: nearest 8-bit code, clamped to [0,255];
// non-finite values become zero. These are not generated by RawNode helpers.
const Bytes kPixels = {
    255, 0, 0, 255,   0, 255, 0, 64,   0, 0, 255, 191,   0, 1, 1, 0,
    0, 46, 255, 0,    128, 1, 255, 128, 0, 0, 0, 255,     64, 191, 255, 255,
};

struct EncodingCase {
  ColorEncoding encoding;
  const char *description;
  cmsCIExyYTRIPLE primaries;
};

bool checkProfile(const Bytes &bytes, const EncodingCase &expected) {
  Profile profile(cmsOpenProfileFromMem(bytes.data(), cmsUInt32Number(bytes.size())), cmsCloseProfile);
  CHECK(profile && cmsGetColorSpace(profile.get()) == cmsSigRgbData);
  CHECK(cmsGetPCS(profile.get()) == cmsSigXYZData);
  char description[128] = {};
  CHECK(cmsGetProfileInfoASCII(profile.get(), cmsInfoDescription, "en", "US", description, sizeof description));
  CHECK(std::string(description) == expected.description);

  // Independent reference primaries (ITU-R BT.709/BT.2020 and Display P3),
  // D65 white. Do not obtain expectations from RawNode's gamut registry or ICC
  // generator. lcms adapts this reference to the ICC D50 PCS for comparison.
  const cmsCIExyY white{0.3127, 0.3290, 1.0};
  cmsToneCurve *linear = cmsBuildGamma(nullptr, 1.0);
  CHECK(linear);
  cmsToneCurve *curves[3] = {linear, linear, linear};
  Profile reference(cmsCreateRGBProfile(&white, &expected.primaries, curves), cmsCloseProfile);
  cmsFreeToneCurve(linear);
  CHECK(reference);
  for (auto tag : {cmsSigRedColorantTag, cmsSigGreenColorantTag, cmsSigBlueColorantTag, cmsSigMediaWhitePointTag}) {
    const auto *actual = static_cast<const cmsCIEXYZ *>(cmsReadTag(profile.get(), tag));
    const auto *target = static_cast<const cmsCIEXYZ *>(cmsReadTag(reference.get(), tag));
    CHECK(actual && target);
    CHECK(std::abs(actual->X - target->X) < 3e-5 && std::abs(actual->Y - target->Y) < 3e-5 &&
          std::abs(actual->Z - target->Z) < 3e-5); // ICC s15Fixed16 rounding.
  }
  for (auto tag : {cmsSigRedTRCTag, cmsSigGreenTRCTag, cmsSigBlueTRCTag}) {
    const auto *curve = static_cast<const cmsToneCurve *>(cmsReadTag(profile.get(), tag));
    CHECK(curve);
    for (double code : {0.0, 1.0 / 255, 0.02, 0.04045, 0.081, 0.18, 0.5, 0.75, 1.0}) {
      double decoded = code;
      if (expected.encoding.gamma == TransferFunction::SRGB)
        decoded = code <= 0.04045 ? code / 12.92 : std::pow((code + 0.055) / 1.055, 2.4);
      if (expected.encoding.gamma == TransferFunction::Rec709)
        decoded = code < 4.5 * 0.018053968510807 ? code / 4.5 :
            std::pow((code + 0.09929682680944) / 1.09929682680944, 1.0 / 0.45);
      CHECK(std::abs(cmsEvalToneCurveFloat(curve, float(code)) - decoded) < 3e-4);
    }
  }
  return true;
}
} // namespace

bool testPngColor() {
  Files files;
  const Image image = referenceImage();
  const std::array<EncodingCase, 4> cases{{
      {{RgbGamut::Rec709, TransferFunction::SRGB}, "RawNode rec709 / srgb",
       {{0.64, 0.33, 1}, {0.30, 0.60, 1}, {0.15, 0.06, 1}}},
      {{RgbGamut::Rec2020, TransferFunction::Linear}, "RawNode rec2020 / linear",
       {{0.708, 0.292, 1}, {0.170, 0.797, 1}, {0.131, 0.046, 1}}},
      {{RgbGamut::DisplayP3, TransferFunction::SRGB}, "RawNode display-p3 / srgb",
       {{0.680, 0.320, 1}, {0.265, 0.690, 1}, {0.150, 0.060, 1}}},
      {{RgbGamut::Rec2020, TransferFunction::Rec709}, "RawNode rec2020 / rec709-camera",
       {{0.708, 0.292, 1}, {0.170, 0.797, 1}, {0.131, 0.046, 1}}},
  }};
  for (const auto &expected : cases) {
    const auto path = files.dir / "output.png";
    CHECK(writeImage(image, path.string(), expected.encoding));
    Png png;
    CHECK(inspect(path, true, png));
    CHECK(png.w == 4 && png.h == 2 && checkProfile(png.profile, expected));
    Bytes decoded;
    CHECK(decodePixels(png, decoded) && decoded == kPixels);

    // Supplied-profile branch must preserve the entire profile byte-for-byte,
    // including header and tags, rather than just a recognisable description.
    const auto supplied = png.profile;
    CHECK(writeImageWithIcc(image, path.string(), supplied, 92));
    Png copied;
    CHECK(inspect(path, true, copied) && copied.profile == supplied);
    CHECK(copied.idat == png.idat);
    CHECK(writeImageWithIcc(image, path.string(), {}, 92));
    Png untagged;
    CHECK(inspect(path, false, untagged) && untagged.profile.empty());
    CHECK(untagged.w == png.w && untagged.h == png.h && untagged.idat == png.idat);
    CHECK(decodePixels(untagged, decoded) && decoded == kPixels);
  }
  std::printf("ok  PNG colour: independent chunk/CRC/zlib/filter checks, ICC primaries/TRCs, exact RGBA8, tagged/untagged (4 encodings)\n");
  return true;
}
