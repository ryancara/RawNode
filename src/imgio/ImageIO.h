// Decode RAW (LibRaw) / raster (stb), convert to/from float buffers, export.
#pragma once

#include "color/ColorEncoding.h"

#include <cstdint>
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

const std::vector<std::string> &rawImageExtensions();
bool isRawImagePath(const std::string &path);

// Loads RAW via LibRaw (camera WB/demosaic, then RawNode-owned gamut and
// transfer-function conversion), TIFF via libtiff, or PNG/JPEG/EXR via
// stb/tinyexr. decodedRaw reports what actually decoded the file, independent
// of its filename extension. detectedEncoding describes the pixels in memory.
bool loadImage(const std::string &path, Image &out, ColorEncoding &detectedEncoding,
               bool &decodedRaw,
               ColorEncoding rawWorkingEncoding = {RgbGamut::Rec2020, TransferFunction::Linear});

// maxEdge 0 = full size; otherwise downsamples so longest edge <= maxEdge.
bool makePreview(const Image &src, int maxEdge, Image &out);

// Format from path extension; PNG/JPEG embed an ICC profile describing pixels
// exactly as tagged by the explicit output encoding.
bool writeImage(const Image &img, const std::string &path,
                ColorEncoding encoding = {RgbGamut::Rec709, TransferFunction::SRGB},
                int jpegQuality = 92);

// Top-down 8-bit RGBA for display.
void toDisplayRGBA8(const Image &img, ColorEncoding encoding, std::vector<unsigned char> &out);

// ICC helpers.
bool profileBytes(ColorEncoding encoding, std::vector<uint8_t> &out);

// Small filmstrip preview (downscaled source, sRGB 8-bit RGBA).
bool loadThumbnailRGBA(const std::string &path, int maxEdge, std::vector<unsigned char> &rgba, int &w, int &h);
