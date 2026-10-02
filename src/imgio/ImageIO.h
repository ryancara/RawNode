// Decode RAW (LibRaw) / raster (stb), convert to/from OFX float buffers, export.
#pragma once

#include <string>
#include <utility>
#include <vector>

struct Image {
  std::vector<float> px;  // bottom-up float RGBA
  int w = 0, h = 0;
  void swap(Image &o) noexcept {
    px.swap(o.px);
    std::swap(w, o.w);
    std::swap(h, o.h);
  }
};

// Matches the UI "Output tag" combo. Plugin pixels are assumed already in this space;
// we only embed the matching ICC (and convert for on-screen preview).
enum class ColorSpace {
  sRGB = 0,
  DisplayP3,
  LinearRec709,
  LinearRec2020,
  ACES2065_1,
};

const char *colorSpaceName(ColorSpace cs);
bool colorSpaceFromName(const std::string &name, ColorSpace &cs);
bool isRawWorkingSpace(ColorSpace cs);
bool isRawImagePath(const std::string &path);

// Loads RAW via LibRaw (camera WB/demosaic, then a RawNode-owned camera -> working-space
// matrix), TIFF via libtiff, or PNG/JPEG/EXR via stb/tinyexr.
// rawWorkingSpace currently supports Linear Rec.709, Linear Rec.2020 and ACES2065-1.
// Raster inputs ignore rawWorkingSpace and are not converted.
bool loadImage(const std::string &path, Image &out, ColorSpace &detected,
               ColorSpace rawWorkingSpace = ColorSpace::LinearRec2020);
// maxEdge 0 = full size; otherwise downsamples so longest edge <= maxEdge.
bool makePreview(const Image &src, int maxEdge, Image &out);
// Format from path extension; PNG/JPEG embed ICC.
bool writeImage(const Image &img, const std::string &path, ColorSpace space = ColorSpace::sRGB, int jpegQuality = 92);
// Top-down 8-bit RGBA for display (lcms2 transform into sRGB).
void toDisplayRGBA8(const Image &img, ColorSpace space, std::vector<unsigned char> &out);
// Small filmstrip preview (downscaled source, sRGB 8-bit RGBA).
bool loadThumbnailRGBA(const std::string &path, int maxEdge, std::vector<unsigned char> &rgba, int &w, int &h);
