#include "OfxCoordinateTests.h"

#include "GraphEvaluator.h"
#include "ofx/OfxCoordinateMapping.h"
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
    CHECK(effects().clipGetRegionOfDefinition(output, 0, &actualRod) == kOfxStatOK && same(actualRod, rod));
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
  const struct { OfxRectD rod; OfxRectI bounds; } cases[] = {
      {{0,0,8,6}, {0,0,8,6}}, {{2,1,5,3}, {2,1,5,3}},
      {{-3,-2,1,1}, {-3,-2,1,1}}, {{-2.25,1.5,1.25,3.125}, {-3,1,2,4}},
      {{2,-1,10,5}, {2,-1,10,5}}, // Same dimensions do not prove a transform.
      {{.2,-.8,.3,-.7}, {0,-1,1,0}},
  };
  for (const auto &test : cases) {
    plugin.rod = test.rod; plugin.expected = test.bounds;
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
  plugin.rod = {2,1,5,3}; plugin.expected = {2,1,5,3}; plugin.identity = true;
  CHECK(processor->render(source, output, {}).ok);
  for (int y = 0; y < 2; ++y) for (int x = 0; x < 3; ++x)
    for (int c = 0; c < 4; ++c) CHECK(output.px[(y * 3 + x) * 4 + c] == source.px[((y + 1) * 8 + x + 2) * 4 + c]);
  plugin.identity = false;
  for (const auto &bad : {OfxRectD{0,0,0,1}, OfxRectD{1,0,0,1}, OfxRectD{0,0,INFINITY,1},
                          OfxRectD{0,0,NAN,1}, OfxRectD{0,0,32769,1}, OfxRectD{1e20,0,1e20+1e6,1}}) {
    plugin.rod = bad; CHECK(!processor->render(source, output, {}).ok);
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
    // [2.5,2.25,6.5,5.25] encloses 5x4, rather than old round(size)=4x3.
    {0,50,25,50, {2.9,3.7,4.5,5.3,6.1}, {2.625,3.375,4.125,4.875}},
    {0,0,-100,0, {-3.5,-2.5,-1.5,-.5,.5,1.5,2.5,3.5}, {.5,1.5,2.5,3.5,4.5,5.5}},
    {0,0,100,0, {4.5,5.5,6.5,7.5,8.5,9.5,10.5,11.5}, {.5,1.5,2.5,3.5,4.5,5.5}},
    // Subpixel RoD [3.92,2.94,4.08,3.06] encloses 2x2.
    {0,100,0,0, {3.96,4.04}, {2.97,3.03}},
    // Infinitesimal pan across integer edges changes enclosing width 4 -> 5.
    {0,50,0,0, {2.5,3.5,4.5,5.5}, {1.875,2.625,3.375,4.125}},
    {0,50,.00001,0, {2.4000002,3.2000002,4.0000002,4.8000002,5.6000002}, {1.875,2.625,3.375,4.125}},
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
  return true;
}

bool testSequentialCropGraph() {
  std::vector<Node> nodes;
  const NodeId source{"source"}, effect{"effect"}, mask{"mask"}, a{"crop-a"}, b{"crop-b"};
  Node root(NodeOperation::Source); root.id = source; nodes.push_back(std::move(root));
  Node radial(NodeOperation::NativeRadialMask); radial.id = mask;
  radial.processor = std::make_unique<NativeRadialMaskProcessor>(); nodes.push_back(std::move(radial));
  Node adjustment; adjustment.id = effect; adjustment.processor = std::make_unique<NativeExposureProcessor>();
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
  // First [3,0,7,3] -> 4x3, then local [0,.75,2,2.25] -> 2x3.
  // Composed x'=4x-1.5, y'=4y-.5, verified against original image fields.
  const auto mapping = result.maskPlacements[0].maskToImage;
  CHECK(samples(**std::get_if<ImageValue>(&result.value), {3.5,4.5}, {1,1.5,2}, mapping));
  const auto mapped = mapping.map(.5,.25); CHECK(mapped && near(mapped->first,.5) && near(mapped->second,.5));
  return true;
}
}

bool testOfxCoordinates() {
  if (!testRegions() || !testCropPixels() || !testSequentialCropGraph()) return false;
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
