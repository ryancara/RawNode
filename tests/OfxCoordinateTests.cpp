#include "OfxCoordinateTests.h"

#include "GraphEvaluator.h"
#include "rawnode/OfxCoordinateMapping.h"
#include "ofx/OfxHostPriv.h"
#include "ofxProperty.h"
#include "processors/NativeExposureProcessor.h"
#include "processors/NativeRadialMaskProcessor.h"
#include "processors/OfxProcessor.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>

#define CHECK(expression) do { if (!(expression)) { \
  std::fprintf(stderr, "OFX coordinate check failed at line %d: %s\n", __LINE__, #expression); return false; \
} } while (false)

namespace {
const OfxPropertySuiteV1 &props() {
  return *static_cast<const OfxPropertySuiteV1 *>(gOfxHost.fetchSuite(gOfxHost.host, kOfxPropertySuite, 1));
}
const OfxImageEffectSuiteV1 &effects() {
  return *static_cast<const OfxImageEffectSuiteV1 *>(gOfxHost.fetchSuite(gOfxHost.host, kOfxImageEffectSuite, 1));
}
Image pattern(int w = 8, int h = 6) {
  Image image; image.w = w; image.h = h;
  image.px.reserve(size_t(w) * h * 4);
  for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x)
    image.px.insert(image.px.end(), {x + .5f, y + .5f, (x + .5f) * (y + .5f), .375f});
  return image;
}
bool near(double actual, double expected, double tolerance = 1e-12) {
  return std::abs(actual - expected) <= tolerance;
}
bool same(OfxRectD a, OfxRectD b) {
  return a.x1 == b.x1 && a.y1 == b.y1 && a.x2 == b.x2 && a.y2 == b.y2;
}

// A cooperating synthetic plugin goes through the real adapter, region query,
// image suites and Render action. Its expected bounds are literal test data.
struct RegionPlugin {
  OfxPlugin plugin{};
  int index = int(gPlugins.size());
  OfxRectD rod{0, 0, 8, 6};
  std::optional<OfxRectD> resolvedRod;
  OfxRectI expected{0, 0, 8, 6};
  OfxStatus queryStatus = kOfxStatOK;
  enum class Declaration { None, Affine, Unknown, Invalid } declaration = Declaration::None;
  bool identity = false, valid = true, failRender = false, throwRender = false;
  RegionPlugin() {
    plugin.pluginIdentifier = "org.rawnode.selftest.coordinate-region";
    plugin.mainEntry = [](const char *action, const void *handle, OfxPropertySetHandle in, OfxPropertySetHandle out) {
      auto *effect = static_cast<Effect *>(const_cast<void *>(handle));
      auto &self = *static_cast<RegionPlugin *>(effect->props.m.at("selftest-coordinate-probe")[0].p);
      if (std::strcmp(action, kOfxImageEffectActionGetRegionOfDefinition) == 0) {
        if (self.queryStatus != kOfxStatOK) return self.queryStatus;
        const double edges[4] = {self.rod.x1, self.rod.y1, self.rod.x2, self.rod.y2};
        return propSetN<double, propSetDouble>(out, kOfxImageEffectPropRegionOfDefinition, 4, edges);
      }
      if (std::strcmp(action, kOfxImageEffectActionRender) != 0) return kOfxStatReplyDefault;
      self.valid = self.render(reinterpret_cast<OfxImageEffectHandle>(effect), in);
      const auto &coordinates = *static_cast<const RawNodeOfxCoordinateMappingSuiteV1 *>(
          gOfxHost.fetchSuite(gOfxHost.host, kRawNodeOfxCoordinateMappingSuite, 1));
      if (self.declaration == Declaration::Affine) {
        const double matrix[6] = {1, 0, .25, 0, 1, -.125};
        coordinates.setInputToOutput(reinterpret_cast<OfxImageEffectHandle>(effect), matrix);
      } else if (self.declaration == Declaration::Unknown) {
        coordinates.setInputToOutput(reinterpret_cast<OfxImageEffectHandle>(effect), nullptr);
      } else if (self.declaration == Declaration::Invalid) {
        const double invalid[6] = {1,0,std::numeric_limits<double>::infinity(),0,1,0};
        coordinates.setInputToOutput(reinterpret_cast<OfxImageEffectHandle>(effect), invalid);
      }
      if (self.throwRender) throw std::runtime_error("coordinate render exception");
      return self.valid && !self.failRender ? kOfxStatOK : kOfxStatFailed;
    };
    PluginEntry entry; entry.plugin = &plugin; entry.label = "Coordinate test"; entry.descriptor = std::make_unique<Effect>();
    propSetPointer(H(&entry.descriptor->props), "selftest-coordinate-probe", 0, this);
    effects().clipDefine(reinterpret_cast<OfxImageEffectHandle>(entry.descriptor.get()), kOfxImageEffectSimpleSourceClipName, nullptr);
    effects().clipDefine(reinterpret_cast<OfxImageEffectHandle>(entry.descriptor.get()), kOfxImageEffectOutputClipName, nullptr);
    gPlugins.push_back(std::move(entry));
  }
  ~RegionPlugin() { gPlugins.resize(index); }
  bool render(OfxImageEffectHandle effect, OfxPropertySetHandle args) {
    const int want[4] = {expected.x1, expected.y1, expected.x2, expected.y2};
    int window[4];
    CHECK(props().propGetIntN(args, kOfxImageEffectPropRenderWindow, 4, window) == kOfxStatOK);
    CHECK(std::equal(window, window + 4, want));
    OfxImageClipHandle source, output;
    CHECK(effects().clipGetHandle(effect, kOfxImageEffectSimpleSourceClipName, &source, nullptr) == kOfxStatOK);
    CHECK(effects().clipGetHandle(effect, kOfxImageEffectOutputClipName, &output, nullptr) == kOfxStatOK);
    OfxRectD actualRod{};
    CHECK(effects().clipGetRegionOfDefinition(output, 0, &actualRod) == kOfxStatOK && same(actualRod, resolvedRod.value_or(rod)));
    CHECK(effects().clipGetRegionOfDefinition(source, 0, &actualRod) == kOfxStatOK && same(actualRod, {0, 0, 8, 6}));
    OfxPropertySetHandle src = nullptr, dst = nullptr;
    CHECK(effects().clipGetImage(source, 0, nullptr, &src) == kOfxStatOK);
    CHECK(effects().clipGetImage(output, 0, nullptr, &dst) == kOfxStatOK);
    struct Release { OfxPropertySetHandle a, b; ~Release() { effects().clipReleaseImage(a); effects().clipReleaseImage(b); } } release{src, dst};
    int bounds[4], pixelRod[4], stride = 0;
    void *srcData = nullptr, *dstData = nullptr;
    CHECK(props().propGetIntN(dst, kOfxImagePropBounds, 4, bounds) == kOfxStatOK && std::equal(bounds, bounds + 4, want));
    CHECK(props().propGetIntN(dst, kOfxImagePropRegionOfDefinition, 4, pixelRod) == kOfxStatOK && std::equal(pixelRod, pixelRod + 4, want));
    CHECK(props().propGetInt(dst, kOfxImagePropRowBytes, 0, &stride) == kOfxStatOK && stride == (expected.x2 - expected.x1) * 16);
    CHECK(props().propGetPointer(src, kOfxImagePropData, 0, &srcData) == kOfxStatOK);
    CHECK(props().propGetPointer(dst, kOfxImagePropData, 0, &dstData) == kOfxStatOK);
    for (int y = expected.y1; y < expected.y2; ++y) {
      auto *row = reinterpret_cast<float *>(static_cast<char *>(dstData) + (y - expected.y1) * stride);
      for (int x = expected.x1; x < expected.x2; ++x) {
        float *pixel = row + (x - expected.x1) * 4;
        if (identity) std::copy_n(static_cast<float *>(srcData) + (y * 8 + x) * 4, 4, pixel);
        else { pixel[0] = float(x); pixel[1] = float(y); pixel[2] = float(100 * y + x); pixel[3] = .375f; }
      }
    }
    return true;
  }
};

bool testRegions() {
  RegionPlugin plugin;
  auto processor = OfxProcessor::create(plugin.index); CHECK(processor);
  const Image source = pattern(); Image output;
  const auto *coordinates = static_cast<const RawNodeOfxCoordinateMappingSuiteV1 *>(
      gOfxHost.fetchSuite(gOfxHost.host, kRawNodeOfxCoordinateMappingSuite, 1));
  CHECK(coordinates && !gOfxHost.fetchSuite(gOfxHost.host, kRawNodeOfxCoordinateMappingSuite, 2));
  CHECK(coordinates->setInputToOutput(reinterpret_cast<OfxImageEffectHandle>(processor->effect()), nullptr) == kOfxStatErrBadHandle);
  const struct { OfxRectD rod; OfxRectI bounds; std::optional<OfxRectD> resolved = {}; } cases[] = {
      {{0,0,8,6}, {0,0,8,6}}, {{2,1,5,3}, {2,1,5,3}},
      {{-3,-2,1,1}, {-3,-2,1,1}}, {{-2.25,1.5,1.25,3.125}, {-3,1,2,4}},
      {{2,-1,10,5}, {2,-1,10,5}}, // Same dimensions do not prove a transform.
      {{.2,-.8,.3,-.7}, {0,-1,1,0}},
      {{kOfxFlagInfiniteMin,kOfxFlagInfiniteMin,kOfxFlagInfiniteMax,kOfxFlagInfiniteMax}, {0,0,8,6}, OfxRectD{0,0,8,6}},
      {{kOfxFlagInfiniteMin,1,5,kOfxFlagInfiniteMax}, {0,1,5,6}, OfxRectD{0,1,5,6}},
      {{-2,kOfxFlagInfiniteMin,kOfxFlagInfiniteMax,3}, {-2,0,8,3}, OfxRectD{-2,0,8,3}},
  };
  for (const auto &test : cases) {
    plugin.rod = test.rod; plugin.expected = test.bounds;
    plugin.resolvedRod = test.resolved;
    const auto result = processor->render(source, output, {});
    CHECK(result.ok && plugin.valid && !result.inputToOutput);
    CHECK(output.w == test.bounds.x2 - test.bounds.x1 && output.h == test.bounds.y2 - test.bounds.y1);
    for (int y = 0; y < output.h; ++y) for (int x = 0; x < output.w; ++x) {
      const auto *p = &output.px[(size_t(y) * output.w + x) * 4];
      CHECK(p[0] == x + test.bounds.x1 && p[1] == y + test.bounds.y1);
      CHECK(p[2] == 100 * (y + test.bounds.y1) + x + test.bounds.x1 && p[3] == .375f);
    }
    CHECK(result.resolvedMapping(8,6,output.w,output.h).kind() ==
          (output.w == 8 && output.h == 6 ? CoordinateMapping::Kind::Identity : CoordinateMapping::Kind::Unknown));
  }
  plugin.resolvedRod.reset();
  plugin.rod = {2,1,5,3}; plugin.expected = {2,1,5,3}; plugin.identity = true;
  CHECK(processor->render(source, output, {}).ok);
  for (int y = 0; y < 2; ++y) for (int x = 0; x < 3; ++x)
    for (int c = 0; c < 4; ++c) CHECK(output.px[(y * 3 + x) * 4 + c] == source.px[((y + 1) * 8 + x + 2) * 4 + c]);
  plugin.identity = false;
  for (const auto &bad : {OfxRectD{0,0,0,1}, OfxRectD{1,0,0,1}, OfxRectD{0,0,INFINITY,1},
                          OfxRectD{0,0,NAN,1}, OfxRectD{0,0,32769,1}, OfxRectD{1e20,0,1e20+1e6,1},
                          // Small extents isolate coordinate range from the allocation limit.
                          OfxRectD{3e9,0,3e9+4,1}, OfxRectD{-3e9,0,-3e9+4,1},
                          // Only exact, correctly positioned OFX sentinels get resolved.
                          OfxRectD{double(kOfxFlagInfiniteMin)+1,0,4,6},
                          OfxRectD{0,0,double(kOfxFlagInfiniteMax)-1,6},
                          OfxRectD{kOfxFlagInfiniteMin,7,kOfxFlagInfiniteMax,kOfxFlagInfiniteMax}}) {
    plugin.rod = bad;
    const auto result = processor->render(source, output, {});
    CHECK(!result.ok && result.backendCode == kOfxStatErrValue && result.message.find("Invalid OFX output region") != std::string::npos);
  }
  plugin.rod = {0,0,8,6}; plugin.expected = {0,0,8,6};
  for (auto status : {kOfxStatReplyDefault, kOfxStatErrUnsupported}) {
    plugin.queryStatus = status; CHECK(processor->render(source, output, {}).ok && output.w == 8 && output.h == 6);
  }
  plugin.queryStatus = kOfxStatFailed;
  CHECK(!processor->render(source, output, {}).ok);
  plugin.queryStatus = kOfxStatOK;
  for (auto declaration : {RegionPlugin::Declaration::Affine, RegionPlugin::Declaration::None,
                           RegionPlugin::Declaration::Unknown, RegionPlugin::Declaration::Invalid}) {
    plugin.declaration = declaration;
    const auto result = processor->render(source, output, {}); CHECK(result.ok);
    const auto mapping = result.resolvedMapping(8,6,8,6);
    if (declaration == RegionPlugin::Declaration::Affine) {
      const auto p = mapping.map(.2,.3); CHECK(p && near(p->first,.45) && near(p->second,.175));
    } else CHECK(mapping.kind() == (declaration == RegionPlugin::Declaration::None ? CoordinateMapping::Kind::Identity : CoordinateMapping::Kind::Unknown));
    CHECK(processor->effect()->renderMapping == nullptr);
  }
  plugin.declaration = RegionPlugin::Declaration::Affine; plugin.throwRender = true;
  try { processor->render(source, output, {}); CHECK(false); } catch (const std::runtime_error &) {}
  CHECK(processor->effect()->renderMapping == nullptr);
  plugin.throwRender = false; plugin.failRender = true;
  CHECK(!processor->render(source, output, {}).ok);
  CHECK(processor->effect()->renderMapping == nullptr);
  return true;
}

int cropIndex() {
  for (size_t i = 0; i < gPlugins.size(); ++i) if (gPlugins[i].label == "Crop") return int(i);
  return -1;
}
bool configure(Processor &crop, int aspect, double amount, double x, double y) {
  return crop.setParameterValue("aspect", aspect) && crop.setParameterValue("crop", amount) &&
         crop.setParameterValue("offsetX", x) && crop.setParameterValue("offsetY", y);
}

// R/G encode input pixel-centre edge coordinates, B their product. Bilinear
// interpolation reproduces all three fields exactly (within float rounding).
// The literal sample tables below are independent of Crop's implementation.
bool samples(const Image &image, const std::vector<double> &xs, const std::vector<double> &ys,
             const CoordinateMapping &mapping, int inputW = 8, int inputH = 6) {
  CHECK(image.w == int(xs.size()) && image.h == int(ys.size()));
  for (int y = 0; y < image.h; ++y) for (int x = 0; x < image.w; ++x) {
    const auto *p = &image.px[(size_t(y) * image.w + x) * 4];
    const double sx = xs[x], sy = ys[y];
    if (sx < .5 || sx > inputW - .5 || sy < .5 || sy > inputH - .5) {
      CHECK(p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 0); continue;
    }
    CHECK(near(p[0], sx, 2e-5) && near(p[1], sy, 2e-5) && near(p[2], sx * sy, 2e-5) && p[3] == .375f);
    const auto mapped = mapping.map(p[0] / double(inputW), p[1] / double(inputH));
    CHECK(mapped && near(mapped->first, (x+.5)/image.w, 3e-6) && near(mapped->second, (y+.5)/image.h, 3e-6));
    const auto recovered = mapping.inverse().map((x+.5)/image.w, (y+.5)/image.h);
    CHECK(recovered && near(recovered->first, sx/inputW) && near(recovered->second, sy/inputH));
  }
  return true;
}

bool testCropPixels() {
  auto crop = OfxProcessor::create(cropIndex()); CHECK(crop);
  const Image source = pattern(); Image output;
  CHECK(configure(*crop, 0, 50, 50, -100));
  auto result = crop->render(source, output, {}); CHECK(result.ok && result.inputToOutput);
  // Window [3,0,7,3], output 4x3: these samples coincide with source
  // pixel centres, so also check exact independent original pixel indices.
  CHECK(samples(output, {3.5,4.5,5.5,6.5}, {.5,1.5,2.5}, *result.inputToOutput));
  for (int y = 0; y < 3; ++y) for (int x = 0; x < 4; ++x) for (int c = 0; c < 4; ++c)
    CHECK(output.px[(y * 4 + x) * 4 + c] == source.px[(y * 8 + x + 3) * 4 + c]);
  const auto p = result.inputToOutput->map(.5,.25); CHECK(p && near(p->first,.25) && near(p->second,.5));
  const struct { int aspect; double amount, x, y; std::vector<double> xs, ys; } cases[] = {
    // Square window [1,0,7,6].
    {1,0,0,0, {1.5,2.5,3.5,4.5,5.5,6.5}, {.5,1.5,2.5,3.5,4.5,5.5}},
    // Continuous [2.5,2.25,6.5,5.25], reported [2,2,6,5], legacy raster 4x3.
    {0,50,25,50, {3,4,5,6}, {2.75,3.75,4.75}},
    {0,0,-100,0, {-3.5,-2.5,-1.5,-.5,.5,1.5,2.5,3.5}, {.5,1.5,2.5,3.5,4.5,5.5}},
    {0,0,100,0, {4.5,5.5,6.5,7.5,8.5,9.5,10.5,11.5}, {.5,1.5,2.5,3.5,4.5,5.5}},
    // Subpixel sampling window retains legacy Source-sized raster (round -> 0).
    {0,100,0,0, {3.93,3.95,3.97,3.99,4.01,4.03,4.05,4.07}, {2.95,2.97,2.99,3.01,3.03,3.05}},
    // The legacy fallback used continuous extent < 1, even when round -> 1.
    {0,90,0,0, {3.65,3.75,3.85,3.95,4.05,4.15,4.25,4.35}, {2.75,2.85,2.95,3.05,3.15,3.25}},
    {0,85,0,0, {3.475,3.625,3.775,3.925,4.075,4.225,4.375,4.525}, {2.625,2.775,2.925,3.075,3.225,3.375}},
    // Infinitesimal pan across integer edges must not add a column or row.
    {0,50,0,0, {2.5,3.5,4.5,5.5}, {2,3,4}},
    {0,50,.00001,0, {2.5000002,3.5000002,4.5000002,5.5000002}, {2,3,4}},
  };
  for (const auto &test : cases) {
    CHECK(configure(*crop,test.aspect,test.amount,test.x,test.y));
    result = crop->render(source, output, {}); CHECK(result.ok && result.inputToOutput);
    CHECK(samples(output, test.xs, test.ys, *result.inputToOutput));
  }
  for (const auto &size : {std::pair<int,int>{8,6}, {13,7}, {7,13}}) {
    const auto odd = pattern(size.first,size.second);
    CHECK(configure(*crop,0,0,0,0)); result = crop->render(odd,output,{});
    CHECK(result.ok && output.w == odd.w && output.h == odd.h && output.px == odd.px);
    CHECK(result.inputToOutput && result.inputToOutput->kind() == CoordinateMapping::Kind::Identity);
  }
  // 9:16 at 30% has exact window [44.25,12,75.75,68]. Legacy edge
  // subtraction rounds its 31.5 width to 32, despite w being just below 31.5.
  // Preserve that behaviour without altering the continuous sampling calculation.
  CHECK(configure(*crop,4,30,0,0));
  result = crop->render(pattern(120,80),output,{}); CHECK(result.ok && result.inputToOutput);
  std::vector<double> xs, ys;
  for (int x = 0; x < 32; ++x) xs.push_back(44.25 + (x+.5) * 63/64);
  for (int y = 0; y < 56; ++y) ys.push_back(12.5 + y);
  CHECK(samples(output,xs,ys,*result.inputToOutput,120,80));
  return true;
}

bool testCropIdentityDeclaration() {
  auto crop = OfxProcessor::create(cropIndex()); CHECK(crop);
  // These default full-frame windows acquire a near-Identity affine from legacy
  // floating-point arithmetic. Metadata must be exact Identity, independently
  // of any pre-existing boundary sampling errors (pixels must not be repaired).
  for (const auto &size : {std::pair<int,int>{40,53}, {6048,4024}}) {
    const Image source = pattern(size.first,size.second); Image output;
    CHECK(configure(*crop,0,0,0,0));
    const auto result = crop->render(source,output,{});
    CHECK(result.ok && output.w == source.w && output.h == source.h);
    CHECK(result.inputToOutput && result.inputToOutput->kind() == CoordinateMapping::Kind::Identity);
    const auto p = result.inputToOutput->map(.25,.75);
    CHECK(p && p->first == .25 && p->second == .75);
    // Interior default samples still come from the original pixel centres.
    const size_t centre = (size_t(source.h/2) * source.w + source.w/2) * 4;
    for (int c = 0; c < 4; ++c) CHECK(output.px[centre+c] == source.px[centre+c]);
  }
  const Image source = pattern(); Image output;
  // Same raster size is insufficient: tiny real pans and a lower-left-anchored
  // resize must retain their affine, even though all dimensions round unchanged.
  const struct { double amount, x, y, u, v, expectedU, expectedV; } cases[] = {
      {0,1e-8,0,0,0,-5e-11,0}, {0,0,1e-8,0,0,0,-5e-11},
      {1e-8,-100,-100,1,1,1.0000000001,1.0000000001},
  };
  for (const auto &test : cases) {
    CHECK(configure(*crop,0,test.amount,test.x,test.y));
    const auto result = crop->render(source,output,{});
    CHECK(result.ok && output.w == source.w && output.h == source.h);
    CHECK(result.inputToOutput && result.inputToOutput->kind() == CoordinateMapping::Kind::Affine);
    const auto p = result.inputToOutput->map(test.u,test.v);
    CHECK(p && near(p->first,test.expectedU) && near(p->second,test.expectedV));
  }
  return true;
}

bool testLegacyCropSizes() {
  auto crop = OfxProcessor::create(cropIndex()); CHECK(crop);
  // These integer windows have unit sample spacing in the legacy raster.
  // Literal offsets/dimensions are independent expectations, including the
  // photographic sizes that floating-point enclosure previously enlarged.
  const struct { int w, h; double amount; int outW, outH, left, bottom; } cases[] = {
      {120,80,95,6,4,57,38}, {6000,4000,18,4920,3280,540,360},
      {6000,4000,41,3540,2360,1230,820},
  };
  for (const auto &test : cases) {
    const Image source = pattern(test.w,test.h); Image output;
    CHECK(configure(*crop,0,test.amount,0,0));
    const auto result = crop->render(source,output,{});
    CHECK(result.ok && result.inputToOutput && output.w == test.outW && output.h == test.outH);
    for (int y = 0; y < output.h; ++y) for (int x = 0; x < output.w; ++x) {
      const auto dst = (size_t(y) * output.w + x) * 4;
      const auto src = (size_t(y + test.bottom) * source.w + x + test.left) * 4;
      // Exact original samples, including alpha and the independent B field.
      for (int c = 0; c < 4; ++c) CHECK(output.px[dst+c] == source.px[src+c]);
      const auto mapped = result.inputToOutput->map(output.px[dst]/double(source.w), output.px[dst+1]/double(source.h));
      CHECK(mapped && near(mapped->first,(x+.5)/output.w) && near(mapped->second,(y+.5)/output.h));
    }
  }
  return true;
}

bool testPartialStrengthIdentityCropGraph() {
  std::vector<Node> nodes;
  const NodeId source{"source"}, effect{"effect"}, mask{"mask"}, cropId{"crop"};
  Node root(NodeOperation::Source); root.id = source; nodes.push_back(std::move(root));
  Node radial(NodeOperation::NativeRadialMask); radial.id = mask;
  radial.processor = std::make_unique<NativeRadialMaskProcessor>(); nodes.push_back(std::move(radial));
  Node adjustment; adjustment.id = effect; adjustment.processor = std::make_unique<NativeExposureProcessor>();
  CHECK(adjustment.processor->setParameterValue("exposure",1.0));
  adjustment.fixedInputs[ports::image] = OutputRef{source,ports::image};
  adjustment.fixedInputs[ports::mask] = OutputRef{mask,ports::mask}; nodes.push_back(std::move(adjustment));
  Node crop; crop.id = cropId; crop.processor = OfxProcessor::create(cropIndex()); CHECK(crop.processor);
  crop.strength = .5;
  crop.fixedInputs[ports::image] = OutputRef{effect,ports::image}; nodes.push_back(std::move(crop));
  const OutputRef output{cropId,ports::image};
  const auto result = evaluateGraph({nodes,source,output},output,{EvaluationPurpose::Inspection,40,53,40,53},
                                    std::make_shared<Image>(pattern(40,53)));
  CHECK(result.state == EvaluationState::Available && result.maskPlacements.size() == 1);
  const auto &placement = result.maskPlacements[0];
  CHECK(placement.mask.nodeId == mask && placement.consumer.nodeId == effect);
  CHECK(placement.maskToImage.kind() == CoordinateMapping::Kind::Identity);
  const auto *image = std::get_if<ImageValue>(&result.value); CHECK(image && *image);
  CHECK((*image)->w == 40 && (*image)->h == 53);
  // Centre lies wholly inside the default Mask. +1 EV doubles RGB; the default
  // Crop at half Strength preserves this interior sample and its original alpha.
  const auto *p = &(*image)->px[(26 * 40 + 20) * 4];
  CHECK(p[0] == 41 && p[1] == 53 && p[2] == 1086.5f && p[3] == .375f);
  return true;
}

bool testSequentialCropGraph() {
  std::vector<Node> nodes;
  const NodeId source{"source"}, effect{"effect"}, mask{"mask"}, a{"crop-a"}, b{"crop-b"};
  Node root(NodeOperation::Source); root.id = source; nodes.push_back(std::move(root));
  Node radial(NodeOperation::NativeRadialMask); radial.id = mask;
  radial.processor = std::make_unique<NativeRadialMaskProcessor>();
  // Hard ellipse centred on original pixel (4,1). On this 8x6 frame it covers
  // exactly (3,1), (4,0), (4,1), (4,2), (5,1). The two Crops retain varying
  // coverage, so a missing mask, missing Exposure or wrong sampling all fail.
  CHECK(radial.processor->setParameterValue("centreX",.5625));
  CHECK(radial.processor->setParameterValue("centreY",.25));
  CHECK(radial.processor->setParameterValue("radiusX",.15625));
  CHECK(radial.processor->setParameterValue("radiusY",.2));
  CHECK(radial.processor->setParameterValue("feather",0.0));
  nodes.push_back(std::move(radial));
  Node adjustment; adjustment.id = effect; adjustment.processor = std::make_unique<NativeExposureProcessor>();
  CHECK(adjustment.processor->setParameterValue("exposure",1.0));
  adjustment.fixedInputs[ports::image] = OutputRef{source,ports::image};
  adjustment.fixedInputs[ports::mask] = OutputRef{mask,ports::mask}; nodes.push_back(std::move(adjustment));
  for (const auto &id : {a,b}) {
    Node crop; crop.id = id; crop.processor = OfxProcessor::create(cropIndex()); CHECK(crop.processor);
    CHECK(configure(*crop.processor,0,50,id == a ? 50 : -100,id == a ? -100 : 0));
    crop.fixedInputs[ports::image] = OutputRef{id == a ? effect : a,ports::image}; nodes.push_back(std::move(crop));
  }
  const OutputRef output{b,ports::image};
  const auto result = evaluateGraph({nodes,source,output}, output, {EvaluationPurpose::Inspection,8,6,8,6}, std::make_shared<Image>(pattern()));
  CHECK(result.state == EvaluationState::Available && result.maskPlacements.size() == 1);
  CHECK(result.maskPlacements[0].mask.nodeId == mask && result.maskPlacements[0].consumer.nodeId == effect);
  // First [3,0,7,3] -> 4x3, then local [0,.75,2,2.25] -> legacy 2x2.
  // Sample original X [3.5,4.5], Y [1.125,1.875]. Linear interpolation of
  // the explicitly enumerated masked +1 EV source pixels yields this table.
  // Rows: coverage [.625,1] at both rows; R, G and B also include interpolation
  // of the image multiplied by coverage (not a new analytic mask at the sample).
  const float expected[] = {5.6875f,2.0625f,7.21875f,.375f, 9,2.25f,10.125f,.375f,
                            5.6875f,2.8125f,9.84375f,.375f, 9,3.75f,16.875f,.375f};
  const auto *image = std::get_if<ImageValue>(&result.value); CHECK(image && *image);
  CHECK((*image)->w == 2 && (*image)->h == 2 && (*image)->px.size() == 16);
  for (size_t i = 0; i < 16; ++i) CHECK(near((*image)->px[i],expected[i],2e-5));
  const auto mapping = result.maskPlacements[0].maskToImage;
  // Independent sample locations establish composed placement even though
  // Exposure has changed RGB and those channels no longer encode coordinates.
  const double xs[] = {3.5,4.5}, ys[] = {1.125,1.875};
  for (int y = 0; y < 2; ++y) for (int x = 0; x < 2; ++x) {
    const auto mapped = mapping.map(xs[x]/8,ys[y]/6);
    CHECK(mapped && near(mapped->first,(x+.5)/2) && near(mapped->second,(y+.5)/2));
    const auto original = mapping.inverse().map((x+.5)/2,(y+.5)/2);
    CHECK(original && near(original->first,xs[x]/8) && near(original->second,ys[y]/6));
  }
  const auto mapped = mapping.map(.5,.25); CHECK(mapped && near(mapped->first,.5) && near(mapped->second,.5));
  return true;
}
}

bool testOfxCoordinates() {
  if (!testRegions() || !testCropPixels() || !testCropIdentityDeclaration() ||
      !testLegacyCropSizes() || !testPartialStrengthIdentityCropGraph() || !testSequentialCropGraph()) return false;
  std::printf("ok  OFX canonical/pixel bounds, declarations and independent Crop pixel/mapping correspondence\n");
  return true;
}

int runOfxCoordinateTests() {
  loadPlugins();
  const bool ok = testOfxCoordinates();
  bool unloaded = true;
  for (auto &entry : gPlugins) unloaded &= ofxActionOk(callAction(entry.plugin,kOfxActionUnload,nullptr));
  return ok && unloaded ? 0 : 1;
}
