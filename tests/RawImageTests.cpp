#include "RawImageTests.h"
#include "imgio/ImageIO.h"

#define LIBRAW_DNGSDK_CONFLICT 1
#include <libraw/libraw.h>
#if RAWNODE_ENABLE_JXL_DNG
#include <jxl/encode.h>
#include <jxl/encode_cxx.h>
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <vector>

namespace {
using Bytes = std::vector<uint8_t>;
namespace fs = std::filesystem;
void require(bool ok, const char *message) {
  if (!ok) throw std::runtime_error(message);
}
struct Files {
  fs::path dir =
      fs::temp_directory_path() /
      ("rawnode-raw-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  Files() { require(fs::create_directory(dir), "create temporary RAW fixture directory"); }
  ~Files() {
    std::error_code ignored;
    fs::remove_all(dir, ignored);
  }
};
struct Fixture {
  bool jxl = false, floating = false, unequal = false, opcode2 = false, opcode3 = false;
  bool active = false, trim = false, lossy = false;
  uint16_t orientation = 1;
};
uint32_t white(const Fixture &f, int c) { return f.lossy ? 65535 : (f.unequal ? (4096u << c) - 1 : 4095); }
uint16_t sample(const Fixture &f, int x, int y, int c) {
  // Keep a white sample inside every ActiveArea so LibRaw does not infer a lower maximum.
  if (x == 32 && y == 24) return uint16_t(white(f, c));
  // Distinct RGB ramps identify channel swaps and every spatial position.
  const int ramp[] = {256 + 7 * x + 3 * y, 1300 + 2 * x + 11 * y, 2400 + 13 * x + 5 * y};
  if (!f.lossy) return uint16_t(ramp[c] * (f.unequal ? (1 << c) : 1));
  // Saturated Rec.2020 colours exercise XYB conversion, with a spatial pattern.
  const double colours[4][3] = {{.88, .02, .05}, {.03, .85, .06}, {.02, .04, .90}, {.76, .60, .02}};
  return uint16_t(colours[(x / 16 + y / 12) % 4][c] * 60000 + x * 13 + y * 7 + c * 3);
}
Bytes imageData(const Fixture &f) {
  std::vector<uint16_t> pixels(64 * 48 * 3);
  for (int y = 0; y < 48; ++y)
    for (int x = 0; x < 64; ++x)
      for (int c = 0; c < 3; ++c) pixels[(y * 64 + x) * 3 + c] = sample(f, x, y, c);
  if (!f.jxl) {
    Bytes bytes;
    for (uint16_t v : pixels) {
      bytes.push_back(uint8_t(v));
      bytes.push_back(uint8_t(v >> 8));
    }
    return bytes;
  }
#if RAWNODE_ENABLE_JXL_DNG
  auto encoder = JxlEncoderMake(nullptr);
  JxlBasicInfo info;
  JxlEncoderInitBasicInfo(&info);
  info.xsize = 64;
  info.ysize = 48;
  info.num_color_channels = 3;
  info.bits_per_sample = 16;
  info.exponent_bits_per_sample = f.floating ? 5 : 0;
  info.uses_original_profile = f.lossy ? JXL_FALSE : JXL_TRUE;
  require(JxlEncoderSetBasicInfo(encoder.get(), &info) == JXL_ENC_SUCCESS, "JXL basic info");
  JxlColorEncoding colour;
  JxlColorEncodingSetToLinearSRGB(&colour, JXL_FALSE);
  colour.primaries = JXL_PRIMARIES_2100; // Rec.2020 gamut, linear transfer.
  require(JxlEncoderSetColorEncoding(encoder.get(), &colour) == JXL_ENC_SUCCESS,
          "JXL linear Rec.2100 colour");
  auto *settings = JxlEncoderFrameSettingsCreate(encoder.get(), nullptr);
  require(JxlEncoderSetFrameLossless(settings, f.lossy ? JXL_FALSE : JXL_TRUE) == JXL_ENC_SUCCESS,
          "JXL mode");
  if (f.lossy) require(JxlEncoderSetFrameDistance(settings, .01f) == JXL_ENC_SUCCESS, "JXL distance");
  std::vector<float> floats(pixels.size());
  for (size_t i = 0; i < floats.size(); ++i) floats[i] = .125f + (i % 3) * .0625f;
  JxlPixelFormat format{3, f.floating ? JXL_TYPE_FLOAT : JXL_TYPE_UINT16, JXL_NATIVE_ENDIAN, 0};
  require(JxlEncoderAddImageFrame(settings, &format,
                                  f.floating ? static_cast<void *>(floats.data())
                                             : static_cast<void *>(pixels.data()),
                                  f.floating ? floats.size() * sizeof(float)
                                             : pixels.size() * sizeof(uint16_t)) == JXL_ENC_SUCCESS,
          "JXL encode frame");
  JxlEncoderCloseInput(encoder.get());
  Bytes bytes(65536);
  uint8_t *next = bytes.data();
  size_t available = bytes.size();
  require(JxlEncoderProcessOutput(encoder.get(), &next, &available) == JXL_ENC_SUCCESS, "JXL output");
  bytes.resize(bytes.size() - available);
  return bytes;
#else
  throw std::runtime_error("JXL fixture encoder unavailable");
#endif
}

Bytes little(std::initializer_list<uint32_t> values, int n) {
  Bytes b;
  for (auto v : values)
    for (int i = 0; i < n; ++i) b.push_back(uint8_t(v >> (8 * i)));
  return b;
}
void big(Bytes &b, uint32_t v) {
  for (int i = 3; i >= 0; --i) b.push_back(uint8_t(v >> (8 * i)));
}
Bytes polynomial(double slope) {
  Bytes b;
  for (auto v : {1u, 8u, 0x01030000u, 0u, 52u, 0u, 0u, 48u, 64u, 0u, 3u, 1u, 1u, 1u}) big(b, v);
  for (double v : {0., slope}) {
    uint64_t bits;
    std::memcpy(&bits, &v, 8);
    for (int i = 7; i >= 0; --i) b.push_back(uint8_t(bits >> (8 * i)));
  }
  return b;
}
// A small TIFF/DNG wrapper authored here, independent of the SDK image writer.
// No photograph, converter, checked-in binary fixture or image export is used.
std::string writeFixture(const Files &files, const char *name, const Fixture &f) {
  struct Tag {
    uint16_t id, type;
    uint32_t count;
    Bytes data;
  };
  std::vector<Tag> tags;
  auto tag = [&](uint16_t id, uint16_t type, uint32_t count, Bytes b) {
    tags.push_back({id, type, count, std::move(b)});
  };
  const Bytes image = imageData(f);
  tag(254, 4, 1, little({0}, 4));
  tag(256, 4, 1, little({64}, 4));
  tag(257, 4, 1, little({48}, 4));
  tag(258, 3, 3, little({16, 16, 16}, 2));
  tag(259, 3, 1, little({f.jxl ? 52546u : 1u}, 2));
  tag(262, 3, 1, little({34892}, 2));
  tag(273, 4, 1, little({0}, 4));
  tag(274, 3, 1, little({f.orientation}, 2));
  tag(277, 3, 1, little({3}, 2));
  tag(278, 4, 1, little({48}, 4));
  tag(279, 4, 1, little({uint32_t(image.size())}, 4));
  tag(284, 3, 1, little({1}, 2));
  const uint32_t format = f.floating ? 3u : 1u;
  tag(339, 3, 3, little({format, format, format}, 2));
  tag(50706, 1, 4, {1, 7, 1, 0});
  tag(50707, 1, 4, f.jxl ? Bytes{1, 7, 0, 0} : Bytes{1, 3, 0, 0});
  const char model[] = "RawNode synthetic linear Rec2020";
  tag(50708, 2, sizeof model, Bytes(model, model + sizeof model));
  tag(50714, 4, 1, little({0}, 4));
  tag(50717, 4, 3, little({white(f, 0), white(f, 1), white(f, 2)}, 4));
  // Deliberately different from ActiveArea: production does not request
  // DefaultCrop.
  tag(50719, 4, 2, little({3, 2}, 4));
  tag(50720, 4, 2, little({44, 34}, 4));
  // Published XYZ(D65)->Rec.2020 matrix, not a matrix read back from LibRaw.
  // Thus camera space is linear Rec.2020 and the default working matrix is
  // identity.
  tag(50721, 10, 9,
      little({1716651188u, 1000000000u, uint32_t(-355670784), 1000000000u, uint32_t(-253366281), 1000000000u,
              uint32_t(-666684352), 1000000000u, 1616481237u, 1000000000u, 15768546u, 1000000000u, 17639857u,
              1000000000u, uint32_t(-42770613), 1000000000u, 942103121u, 1000000000u},
             4));
  tag(50728, 5, 3, little({1, 1, 1, 1, 1, 1}, 4));
  tag(50778, 3, 1, little({21}, 2));
  if (f.active) tag(50829, 4, 4, little({4, 6, 44, 58}, 4));
  if (f.trim) {
    Bytes b;
    for (auto v : {1u, 6u, 0x01030000u, 0u, 16u, 6u, 8u, 42u, 56u}) big(b, v);
    const auto n = uint32_t(b.size());
    tag(51008, 7, n, std::move(b));
  }
  for (const auto stage : {2, 3})
    if (stage == 2 ? f.opcode2 : f.opcode3) {
      auto b = polynomial(stage == 2 ? .5 : .25);
      const auto n = uint32_t(b.size());
      tag(stage == 2 ? 51009 : 51022, 7, n, std::move(b));
    }
  std::sort(tags.begin(), tags.end(), [](const auto &a, const auto &b) { return a.id < b.id; });
  Bytes bytes(8 + 2 + 12 * tags.size() + 4, 0);
  bytes[0] = bytes[1] = 'I';
  bytes[2] = 42;
  bytes[4] = 8;
  auto put = [&](size_t pos, uint32_t v, int n) {
    for (int i = 0; i < n; ++i) bytes.at(pos + i) = uint8_t(v >> (8 * i));
  };
  put(8, uint32_t(tags.size()), 2);
  size_t strip = 0;
  for (size_t i = 0; i < tags.size(); ++i) {
    const auto &t = tags[i];
    const size_t p = 10 + i * 12;
    put(p, t.id, 2);
    put(p + 2, t.type, 2);
    put(p + 4, t.count, 4);
    if (t.id == 273) strip = p + 8;
    if (t.data.size() <= 4)
      std::copy(t.data.begin(), t.data.end(), bytes.begin() + p + 8);
    else {
      put(p + 8, uint32_t(bytes.size()), 4);
      bytes.insert(bytes.end(), t.data.begin(), t.data.end());
      if (bytes.size() % 2) bytes.push_back(0);
    }
  }
  put(strip, uint32_t(bytes.size()), 4);
  bytes.insert(bytes.end(), image.begin(), image.end());
  const auto path = files.dir / (std::string(name) + ".dng");
  std::ofstream stream(path, std::ios::binary);
  stream.write(reinterpret_cast<const char *>(bytes.data()), std::streamsize(bytes.size()));
  require(bool(stream), "write temporary DNG");
  return path.string();
}

void checkPixels(const Image &image, const Fixture &f, RgbGamut gamut) {
  const int width = f.active ? 52 : 64, height = f.active ? 40 : 48;
  const bool rotated = f.orientation == 6;
  require(image.w == (rotated ? height : width) && image.h == (rotated ? width : height),
          "RAW dimensions/orientation/ActiveArea");
  require(image.px.size() == size_t(image.w) * image.h * 4, "RAW buffer size");
  // Published linear Rec.2020 -> Rec.709 transform; independent of RawNode's
  // registry.
  const double to709[3][3] = {
      {1.660491, -.587641, -.072850}, {-.124550, 1.132900, -.008349}, {-.018151, -.100579, 1.118730}};
  double maximum = 0;
  for (int y = 0; y < image.h; ++y)
    for (int x = 0; x < image.w; ++x) {
      const int sx = (f.active ? 6 : 0) + (rotated ? y : x);
      const int sy = (f.active ? 4 : 0) + (rotated ? height - 1 - x : y);
      double expected[3];
      for (int c = 0; c < 3; ++c)
        expected[c] = double(sample(f, sx, sy, c)) / white(f, c) * (f.opcode2 ? .5 : 1.);
      const float *actual = &image.px[(size_t(image.h - 1 - y) * image.w + x) * 4]; // bottom-up storage
      for (int c = 0; c < 3; ++c) {
        const double target =
            gamut == RgbGamut::Rec709
                ? to709[c][0] * expected[0] + to709[c][1] * expected[1] + to709[c][2] * expected[2]
                : expected[c];
        require(std::isfinite(actual[c]), "non-finite RAW pixel");
        maximum = std::max(maximum, std::abs(actual[c] - target));
      }
      require(actual[3] == 1, "RAW alpha");
    }
  // Quantization plus published matrix rounding (LibRaw's XYZ->709 constants
  // have ~4 decimal digits). The lossy bound also covers distance .01 XYB
  // compression.
  // This fixture measures <.000346 in Rec.2020 and <.000573 in Rec.709.
  // Allow modest compiler/SIMD variation, not a generic one-percent lossy bound.
  const double tolerance = f.lossy ? (gamut == RgbGamut::Rec709 ? .0008 : .0005) : .0001;
  std::fprintf(stderr, "RAW oracle max_error=%.9g bound=%.9g\n", maximum, tolerance);
  require(maximum <= tolerance, "independent RAW colour/pixel oracle");
}
Image decodeRawImage(const std::string &path,
                     ColorEncoding encoding = {RgbGamut::Rec2020, TransferFunction::Linear}) {
  Image out;
  ColorEncoding detected;
  bool raw = false;
  require(loadImage(path, out, detected, raw, encoding), "production RAW loadImage failed");
  require(raw && detected == encoding, "RAW dispatch/working encoding");
  return out;
}
} // namespace

bool testRawImages() {
  try {
    Files files;
    auto test = [&](const char *name, const Fixture &fixture, RgbGamut gamut = RgbGamut::Rec2020) {
      const auto image =
          decodeRawImage(writeFixture(files, name, fixture), {gamut, TransferFunction::Linear});
      checkPixels(image, fixture, gamut);
      std::fprintf(stderr, "PASS RAW %s\n", name);
    };
    test("ordinary", {});
    Fixture ordinary;
    ordinary.active = true;
    ordinary.orientation = 6;
    test("ordinary-active-rotated", ordinary);
    test("ordinary-colour-709", {}, RgbGamut::Rec709);
#if RAWNODE_ENABLE_JXL_DNG
    Fixture f;
    f.jxl = true;
    test("integer-jxl", f);
    f.unequal = true;
    test("unequal-whites-no-op2", f);
    f.opcode2 = true;
    test("unequal-whites-op2", f);
    f.unequal = false;
    test("op2", f);
    f.opcode3 = true;
    test("op2-and-op3", f);
    f.opcode2 = false;
    test("op3-only", f);
    f.opcode3 = false;
    f.active = true;
    test("active-no-op2", f);
    f.opcode2 = true;
    test("active-op2", f);
    f.orientation = 6;
    test("active-op2-rotated", f);
    f = Fixture{};
    f.jxl = f.lossy = true;
    test("lossy-saturated-linear-rec2100", f);
    test("lossy-colour-709", f, RgbGamut::Rec709);
    f = Fixture{};
    f.jxl = true;
    const auto path = writeFixture(files, "no-host", f);
    LibRaw raw;
    require(raw.open_file(path.c_str()) == LIBRAW_SUCCESS, "missing-host identification");
    require(raw.unpack() == LIBRAW_FILE_UNSUPPORTED, "JXL without SDK host must fail");
    std::fprintf(stderr, "PASS RAW missing-host failure\n");
    for (bool op2 : {false, true}) {
      f = Fixture{};
      f.jxl = f.floating = true;
      f.opcode2 = op2;
      Image image;
      ColorEncoding detected;
      bool decodedRaw = false;
      require(!loadImage(writeFixture(files, "float", f), image, detected, decodedRaw) && image.px.empty(),
              "floating-point JXL must be rejected");
      std::fprintf(stderr, "PASS RAW float%s rejection\n", op2 ? "-op2" : "");
    }
    f = Fixture{};
    f.jxl = f.opcode2 = f.trim = true;
    Image image;
    ColorEncoding detected;
    bool decodedRaw = false;
    require(!loadImage(writeFixture(files, "trim", f), image, detected, decodedRaw) && image.px.empty(),
            "unmapped Stage 1 geometry must be rejected");
    std::fprintf(stderr, "PASS RAW TrimBounds rejection\n");
#endif
    // Optional private fixtures exercise the same public loader without
    // committing photos.
    for (const char *key : {"RAWNODE_TEST_NEF", "RAWNODE_TEST_DNG", "RAWNODE_TEST_JXL"})
      if (const char *path = std::getenv(key)) {
        const auto begin = std::chrono::steady_clock::now();
        const auto image = decodeRawImage(path);
        require(image.w > 0 && image.h > 0 && !image.px.empty(), "real RAW dimensions");
        double sum = 0;
        float low = 1, high = 0;
        uint64_t fingerprint = 14695981039346656037ull;
        for (size_t i = 0; i < image.px.size(); ++i) {
          const float v = image.px[i];
          require(std::isfinite(v), "non-finite real RAW");
          if (i % 4 != 3) {
            sum += v;
            low = std::min(low, v);
            high = std::max(high, v);
          }
          uint32_t bits;
          std::memcpy(&bits, &v, sizeof bits);
          for (int byte = 0; byte < 4; ++byte) {
            fingerprint ^= uint8_t(bits >> (byte * 8));
            fingerprint *= 1099511628211ull;
          }
        }
        require(high > low && sum > 0, "real RAW numerical sanity");
        std::vector<unsigned char> thumb;
        int w = 0, h = 0;
        require(loadThumbnailRGBA(path, 128, thumb, w, h) && w > 0 && h > 0 && w <= 128 && h <= 128,
                "RAW thumbnail");
        const double seconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
        std::fprintf(stderr,
                     "PASS %s dimensions=%dx%d range=[%g,%g] sum=%.17g fnv64=%016llx "
                     "load+thumbnail_seconds=%.3f\n",
                     key, image.w, image.h, low, high, sum, static_cast<unsigned long long>(fingerprint),
                     seconds);
      }
    return true;
  } catch (const std::exception &e) {
    std::fprintf(stderr, "RAW image test failed: %s\n", e.what());
    return false;
  }
}
