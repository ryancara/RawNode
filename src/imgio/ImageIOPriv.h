#pragma once

#include "imgio/ImageIO.h"

#include <cstdint>
#include <string>
#include <vector>

// Shared between ImageLoad / ImageColor / ImageWrite translation units.
bool fromRGBAFloatTopDown(float *src, int w, int h, Image &out);
void flipRows(float *px, int w, int h);
bool extractPngIcc(const std::string &path, std::vector<uint8_t> &icc);
bool extractJpgIcc(const std::string &path, std::vector<uint8_t> &icc);

// Encode with the supplied profile (empty means untagged). writeImage supplies
// the generated profile; self-tests also exercise the untagged encoder branches.
bool writeImageWithIcc(const Image &img, const std::string &path, const std::vector<uint8_t> &icc, int jpegQuality);

// RawNode-authored ICC profiles carry exact stable IDs and can stay in their
// native gamut. Other valid RGB ICC profiles are converted through lcms into
// RawNode's canonical raster working encoding: Linear Rec.2020.
bool rawNodeIccEncoding(const std::vector<uint8_t> &icc, ColorEncoding &encoding);
bool convertIccToLinearRec2020(Image &img, const std::vector<uint8_t> &icc);

// Camera-space float RGB -> target RGB using a 3x4 matrix. Kept internal so
// decoder tests can verify that matrix-created negative/highlight values survive.
void applyCameraMatrix(const float camera[4], int channels, const float matrix[3][4], float rgb[3]);

// Compose LibRaw's camera -> Linear Rec.709 matrix with RawNode's selected
// initial working-space transform. Returns false for unsupported targets.
bool makeCameraToWorkingMatrix(const float cameraToRec709[3][4], RgbGamut target, float out[3][4]);
