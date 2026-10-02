// Decode RAW (LibRaw) / raster (stb), convert to/from OFX float buffers, export.
#pragma once

#include "color/ColorEncoding.h"

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

// Loads RAW via LibRaw (camera WB/demosaic, then RawNode-owned gamut and
// transfer-function conversion), TIFF via libtiff, or PNG/JPEG/EXR via
// stb/tinyexr. Raster inputs ignore the RAW encoding arguments.
bool loadImage(const std::string &path, Image &out, ColorSpace &detected,
               RgbGamut rawGamut = RgbGamut::Rec2020,
               TransferFunction rawGamma = TransferFunction::Linear);
// Compatibility overload for the three legacy scene-linear RAW choices.
bool loadImage(const std::string &path, Image &out, ColorSpace &detected, ColorSpace rawWorkingSpace);
// maxEdge 0 = full size; otherwise downsamples so longest edge <= maxEdge.
bool makePreview(const Image &src, int maxEdge, Image &out);
// Format from path extension; PNG/JPEG embed ICC.
bool writeImage(const Image &img, const std::string &path, ColorSpace space = ColorSpace::sRGB, int jpegQuality = 92);
// Top-down 8-bit RGBA for display.
void toDisplayRGBA8(const Image &img, ColorSpace space, std::vector<unsigned char> &out);
// Explicit source gamut + transfer function, used by RAW source preview.
void toDisplayRGBA8(const Image &img, RgbGamut gamut, TransferFunction gamma,
                    std::vector<unsigned char> &out);
// Small filmstrip preview (downscaled source, sRGB 8-bit RGBA).
bool loadThumbnailRGBA(const std::string &path, int maxEdge, std::vector<unsigned char> &rgba, int &w, int &h);
