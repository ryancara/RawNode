#include "MaskOverlayTests.h"

#include "RenderPipeline.h"
#include "ofx/OfxHost.h"
#include "processors/NativeExposureProcessor.h"
#include "processors/OfxProcessor.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

#define CHECK(expression) do { if (!(expression)) { \
  std::fprintf(stderr, "Mask Overlay check failed at line %d: %s\n", __LINE__, #expression); return false; \
} } while (false)

namespace {
const OutputRef maskOutput{NodeId{"coverage"}, ports::mask};
const InputAddress consumer{NodeId{"adjustment"}, std::nullopt, ports::mask};

EvaluationResult photo(int w, int h) {
  auto image = std::make_shared<Image>(); image->w = w; image->h = h;
  for (int i = 0; i < w * h; ++i) image->px.insert(image->px.end(), {0, 0, 0, .375f});
  return {{NodeId{"photo"}, ports::image}, ValueType::Image, EvaluationState::Available, ImageValue(image), {}};
}
EvaluationResult coverage(int w, int h, std::vector<float> samples) {
  auto mask = std::make_shared<Mask>(); mask->w = w; mask->h = h; mask->px = std::move(samples);
  return {maskOutput, ValueType::Mask, EvaluationState::Available, MaskValue(mask), {}};
}

// Expected coverage is a literal bottom-up table derived outside the sampler.
// A black photograph makes its red bytes round(255 * .4 * coverage), G/B=0;
// alpha .375 rounds to 96. Check top-down presentation independently.
bool pixels(const EvaluationResult &image, const EvaluationResult &mask, const std::vector<double> &expected) {
  App app;
  app.outputEncoding = {RgbGamut::Rec709, TransferFunction::SRGB};
  const auto output = display_detail::preparePresentation(app, image, mask);
  CHECK(expected.size() == size_t(output.w) * output.h && output.rgba.size() == expected.size() * 4);
  for (int y = 0; y < output.h; ++y) for (int x = 0; x < output.w; ++x) {
    const auto i = (size_t(output.h - 1 - y) * output.w + x) * 4;
    const auto c = expected[size_t(y) * output.w + x];
    CHECK(output.rgba[i] == std::lround(102 * c));
    CHECK(output.rgba[i + 1] == 0 && output.rgba[i + 2] == 0 && output.rgba[i + 3] == 96);
  }
  return true;
}

bool sampling() {
  const auto plane = coverage(4, 3, {0,.25f,.5f,.75f, .125f,.375f,.625f,.875f, .25f,.5f,.75f,1});
  auto image = photo(4, 3);
  const auto check = [&](CoordinateMapping mapping, const std::vector<double> &expected) {
    image.maskPlacements = {{maskOutput, consumer, mapping}};
    return pixels(image, plane, expected);
  };
  CHECK(check(CoordinateMapping::identity(), {0,.25,.5,.75, .125,.375,.625,.875, .25,.5,.75,1}));
  // A half-Mask-pixel translation: inverse sample columns are -.5,.5,1.5,2.5.
  CHECK(check(CoordinateMapping::affine(1,0,.125,0,1,0),
              {0,.125,.375,.625, .0625,.25,.5,.75, .125,.375,.625,.875}));
  // Two whole columns move right; no edge extrusion into the empty half.
  CHECK(check(CoordinateMapping::affine(1,0,.5,0,1,0),
              {0,0,0,.25, 0,0,.125,.375, 0,0,.25,.5}));
  image = photo(4, 4);
  CHECK(check(CoordinateMapping::affine(2,0,-.5,0,.5,.25),
              {0,0,0,0, .21875,.34375,.46875,.59375, .40625,.53125,.65625,.78125, 0,0,0,0}));

  const auto grid = coverage(3, 3, {0,.125f,.25f, .375f,.5f,.625f, .75f,.875f,1});
  image = photo(3, 3);
  image.maskPlacements = {{maskOutput, consumer, CoordinateMapping::affine(0,-1,1,1,0,0)}};
  CHECK(pixels(image, grid, {.75,.375,0, .875,.5,.125, 1,.625,.25}));
  image.maskPlacements[0].maskToImage = CoordinateMapping::affine(-1,0,1,0,1,0);
  CHECK(pixels(image, grid, {.25,.125,0, .625,.5,.375, 1,.875,.75}));
  image.maskPlacements[0].maskToImage = CoordinateMapping::affine(1,1,-.5,0,1,0);
  CHECK(pixels(image, grid, {.125,.25,0, .375,.5,.625, 0,.75,.875}));

  // Identity relates normalized domains even when the two rasters differ.
  // Zero padding (not edge clamping) contributes at the outer quarter pixels.
  image = photo(4, 4);
  image.maskPlacements = {{maskOutput, consumer, CoordinateMapping::identity()}};
  CHECK(pixels(image, coverage(2,2,{0,.5f,.5f,1}),
      {0,.09375,.28125,.28125, .09375,.25,.5,.46875,
       .28125,.5,.75,.65625, .28125,.46875,.65625,.5625}));

  // Entirely outside the domain, including extreme finite inverse coordinates.
  image.maskPlacements[0].maskToImage = CoordinateMapping::affine(1,0,2,0,1,0);
  CHECK(pixels(image, plane, std::vector<double>(16,0)));
  image.maskPlacements[0].maskToImage = CoordinateMapping::affine(1e-308,0,-1,0,1,0);
  CHECK(pixels(image, plane, std::vector<double>(16,0)));

  // Exact legacy bytes on valid Identity coverage, including feather gradients
  // and arbitrary photograph/alpha bytes (no round trip through coordinates).
  image = photo(7,5);
  auto raster = std::make_shared<Image>(*std::get<ImageValue>(image.value));
  std::vector<float> samples;
  for (size_t i = 0; i < 35; ++i) {
    samples.push_back(float(i) / 34);
    for (size_t c = 0; c < 4; ++c) raster->px[i*4+c] = float((i*17+c*31)%256)/255;
  }
  image.value = ImageValue(raster);
  image.maskPlacements = {{maskOutput,consumer,CoordinateMapping::identity()}};
  const auto mask = coverage(7,5,samples);
  App app; const auto background = display_detail::preparePresentation(app,image);
  auto expected = background.rgba;
  for (int y = 0; y < 5; ++y) for (int x = 0; x < 7; ++x) {
    const float amount = .4f * samples[size_t(4-y)*7+x];
    const size_t i = (size_t(y)*7+x)*4;
    for (int c = 0; c < 3; ++c) {
      const float target = c == 0 ? 255.0f : 0.0f;
      expected[i+c] = static_cast<unsigned char>(expected[i+c]+(target-expected[i+c])*amount+.5f);
    }
  }
  CHECK(display_detail::preparePresentation(app,image,mask).rgba == expected);
  return true;
}

bool resolutionAndInvalidData() {
  auto image = photo(2,2);
  const auto identity = CoordinateMapping::identity(), moved = CoordinateMapping::affine(1,0,.25,0,1,0);
  const auto resolve = [&](std::vector<CoordinateMapping> mappings) {
    image.maskPlacements.clear();
    // An unrelated endpoint/port must never supply the selected Mask's position.
    image.maskPlacements.push_back({{maskOutput.nodeId,ports::image},consumer,CoordinateMapping::unknown()});
    for (auto mapping : mappings) image.maskPlacements.push_back({maskOutput,consumer,mapping});
    return resolveMaskPlacement(image,maskOutput);
  };
  CHECK(resolve({identity}).state == MaskPlacementState::Identity);
  CHECK(resolve({moved}).state == MaskPlacementState::Affine);
  CHECK(resolve({moved,moved}).maskToImage == moved);
  CHECK(resolve({identity,identity}).state == MaskPlacementState::Identity);
  CHECK(resolve({identity,moved}).state == MaskPlacementState::Ambiguous);
  CHECK(resolve({moved,identity}).state == MaskPlacementState::Ambiguous);
  const auto singular = CoordinateMapping::affine(1,2,0,2,4,0);
  const auto malformed = CoordinateMapping::affine(1,0,NAN,0,1,0);
  const auto overflow = CoordinateMapping::affine(1e308,0,0,0,1e308,0);
  for (auto invalid : {CoordinateMapping::unknown(),singular,malformed,overflow}) {
    for (auto mappings : {std::vector<CoordinateMapping>{invalid}, {identity,invalid}, {invalid,identity},
                          {identity,moved,invalid}, {invalid,moved,identity}}) {
      CHECK(resolve(mappings).state == MaskPlacementState::Unknown);
      CHECK(pixels(image,coverage(2,2,{1,1,1,1}),{0,0,0,0}));
    }
  }
  CHECK(resolve({identity,moved}).state == MaskPlacementState::Ambiguous);
  CHECK(pixels(image,coverage(2,2,{1,1,1,1}),{0,0,0,0}));
  CHECK(resolve({}).state == MaskPlacementState::Absent);
  CHECK(pixels(image,coverage(2,2,{1,1,1,1}),{0,0,0,0}));
  CHECK(resolve({identity}).state == MaskPlacementState::Identity);
  auto valid = std::get<ImageValue>(image.value);
  for (const auto &size : {std::pair<int,int>{0,2},{-1,2},{2,0},{2,-1},{3,2}}) {
    auto bad = std::make_shared<Image>(*valid); bad->w = size.first; bad->h = size.second;
    image.value = ImageValue(bad);
    CHECK(resolveMaskPlacement(image,maskOutput).state == MaskPlacementState::Unknown);
  }
  image.value = valid;
  for (auto mask : {coverage(0,2,{}),coverage(-1,2,{}),coverage(2,0,{}),coverage(2,2,{1}),coverage(2,2,{1,1,1,1,1})})
    CHECK(pixels(image,mask,{0,0,0,0}));
  CHECK(pixels(image,coverage(2,2,{NAN,INFINITY,-1,2}),{0,0,0,1}));
  for (auto state : {EvaluationState::Inactive,EvaluationState::Unavailable,EvaluationState::Error}) {
    auto mask = coverage(2,2,{1,1,1,1}); mask.state = state;
    CHECK(pixels(image,mask,{0,0,0,0})); // Even a stale value cannot tint.
  }
  return true;
}

class PlaneMask final : public graph_test::Executor {
 public:
  graph_test::OperationResult execute(const Node &, const EvaluationContext &,
      const graph_test::InputValues &inputs, const RenderCancellation &) override {
    auto mask = std::make_shared<Mask>(); mask->w = inputs.reference->w; mask->h = inputs.reference->h;
    for (int y = 0; y < mask->h; ++y) for (int x = 0; x < mask->w; ++x)
      mask->px.push_back(float(x+3*y)/32);
    return {EvaluationState::Available,{{ports::mask,MaskValue(mask)}},{}};
  }
};
}

bool testMaskOverlay() {
  if (!sampling() || !resolutionAndInvalidData()) return false;
  std::printf("ok  Mask Overlay independent pixel-centre/bilinear/affine/boundary oracles and shared placement policy\n");
  return true;
}

bool testMaskOverlayCrop() {
  int cropIndex = -1;
  for (size_t i = 0; i < gPlugins.size(); ++i) if (gPlugins[i].label == "Crop") cropIndex = int(i);
  CHECK(cropIndex >= 0);
  const NodeId source{"source"}, adjustment{"adjustment"}, a{"crop-a"}, b{"crop-b"};
  std::vector<Node> nodes;
  Node root(NodeOperation::Source); root.id = source; nodes.push_back(std::move(root));
  Node mask(NodeOperation::TestReferenceMask); mask.id = maskOutput.nodeId;
  mask.fixedInputs[ports::reference] = OutputRef{source,ports::image}; nodes.push_back(std::move(mask));
  Node effect; effect.id = adjustment; effect.processor = std::make_unique<NativeExposureProcessor>();
  effect.fixedInputs[ports::image] = OutputRef{source,ports::image}; effect.fixedInputs[ports::mask] = maskOutput;
  nodes.push_back(std::move(effect));
  for (auto id : {a,b}) {
    Node crop; crop.id = id; crop.processor = OfxProcessor::create(cropIndex); CHECK(crop.processor);
    CHECK(crop.processor->setParameterValue("crop",50.0));
    CHECK(crop.processor->setParameterValue("offsetX",id == a ? 50.0 : -100.0));
    CHECK(crop.processor->setParameterValue("offsetY",id == a ? -100.0 : 0.0));
    crop.fixedInputs[ports::image] = OutputRef{id == a ? adjustment : a,ports::image};
    nodes.push_back(std::move(crop));
  }
  PlaneMask executor;
  const auto sourceValue = std::get<ImageValue>(photo(8,6).value);
  const auto evaluate = [&](const NodeId &id) {
    const OutputRef output{id,ports::image};
    return evaluateGraphPair({nodes,source,output},output,maskOutput,
        {EvaluationPurpose::Preview,8,6,8,6},sourceValue,{},&executor);
  };
  auto pair = evaluate(a);
  CHECK(pair.first.state == EvaluationState::Available && pair.second.state == EvaluationState::Available);
  CHECK(std::get<MaskValue>(pair.second.value)->w == 8 && std::get<ImageValue>(pair.first.value)->w == 4);
  CHECK(pixels(pair.first,pair.second,{3/32.,4/32.,5/32.,6/32., 6/32.,7/32.,8/32.,9/32., 9/32.,10/32.,11/32.,12/32.}));
  pair = evaluate(b);
  // Composed original sample centres: X=3.5,4.5; Y=1.125,1.875.
  CHECK(pixels(pair.first,pair.second,{4.875/32,5.875/32,7.125/32,8.125/32}));

  // Full-size Crop pans half a frame. Matching dimensions cannot imply Identity.
  CHECK(nodes[3].processor->setParameterValue("crop",0.0));
  CHECK(nodes[3].processor->setParameterValue("offsetX",100.0));
  CHECK(nodes[3].processor->setParameterValue("offsetY",0.0));
  pair = evaluate(a);
  CHECK(std::get<ImageValue>(pair.first.value)->w == 8 && std::get<MaskValue>(pair.second.value)->w == 8);
  CHECK(resolveMaskPlacement(pair.first,maskOutput).state == MaskPlacementState::Affine);
  std::vector<double> expected;
  for (int y = 0; y < 6; ++y) for (int x = 0; x < 8; ++x)
    expected.push_back(x < 4 ? (x+4+3*y)/32. : 0);
  // Crop's existing out-of-bounds alpha is zero; use its photograph alpha as
  // the independent unchanged background check, then check all tint bytes.
  App app; const auto background = display_detail::preparePresentation(app,pair.first);
  const auto tinted = display_detail::preparePresentation(app,pair.first,pair.second);
  for (int y = 0; y < 6; ++y) for (int x = 0; x < 8; ++x) {
    const size_t i = (size_t(5-y)*8+x)*4;
    CHECK(tinted.rgba[i] == std::lround(102*expected[size_t(y)*8+x]));
    CHECK(tinted.rgba[i+1] == 0 && tinted.rgba[i+2] == 0 && tinted.rgba[i+3] == background.rgba[i+3]);
  }
  std::printf("ok  Mask Overlay real single/composed/same-size Crop and differing Reference/Image dimensions\n");
  return true;
}

int runMaskOverlayTests() {
  loadPlugins();
  const bool ok = testMaskOverlay() && testMaskOverlayCrop();
  bool unloaded = true;
  for (auto &entry : gPlugins) {
    const auto status = callAction(entry.plugin,kOfxActionUnload,nullptr);
    unloaded &= status == kOfxStatOK || status == kOfxStatReplyDefault;
  }
  gPlugins.clear();
  return ok && unloaded ? 0 : 1;
}
