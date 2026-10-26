#include "OfxImageHandleTests.h"
#include "ofx/OfxHost.h"
#include "ofx/OfxHostPriv.h"

#include "ofxMultiThread.h"
#include "ofxProperty.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

// A test filter plugin fetches and releases images through the real host suites
// inside the production renderEffect Render action. OpenFX requires each
// clipGetImage to return a separate handle, valid until its own release, even
// for repeated requests of the same image. Expected properties come from the
// buffers and sizes this test passes to renderEffect, not from host output.

namespace {
// Distinct Source and Output sizes show that each handle describes its own clip.
constexpr int kSourceW = 3, kSourceH = 2, kOutputW = 5, kOutputH = 4;

template <class Suite>
const Suite &suite(const char *name) {
  return *static_cast<const Suite *>(gOfxHost.fetchSuite(gOfxHost.host, name, 1));
}
const OfxPropertySuiteV1 &props() { return suite<OfxPropertySuiteV1>(kOfxPropertySuite); }
const OfxImageEffectSuiteV1 &effects() { return suite<OfxImageEffectSuiteV1>(kOfxImageEffectSuite); }
const OfxMultiThreadSuiteV1 &threads() { return suite<OfxMultiThreadSuiteV1>(kOfxMultiThreadSuite); }

OfxImageClipHandle clip(OfxImageEffectHandle effect, const char *name) {
  OfxImageClipHandle handle = nullptr;
  return effects().clipGetHandle(effect, name, &handle, nullptr) == kOfxStatOK ? handle : nullptr;
}

struct Expected {
  const char *clip;
  const void *data;
  int w, h;
};

// A fetched image plus property storage retained from it, as plugins keep
// propGetString results. Both must remain valid until this handle's release,
// whatever other images are fetched or released meanwhile.
struct Held {
  OfxPropertySetHandle handle = nullptr;
  const char *uniqueId = nullptr;
};

Held fetch(OfxImageClipHandle clip) {
  Held held;
  char *id = nullptr;
  if (effects().clipGetImage(clip, 0, nullptr, &held.handle) != kOfxStatOK ||
      props().propGetString(held.handle, kOfxImagePropUniqueIdentifier, 0, &id) != kOfxStatOK) return {};
  held.uniqueId = id;
  return held;
}
OfxStatus release(const Held &held) { return effects().clipReleaseImage(held.handle); }

// The image must still describe the buffer this test supplied for its clip:
// packed float RGBA with its origin at zero and width * 4 floats per row.
bool describes(const Held &held, const Expected &want) {
  if (!held.handle || !held.uniqueId || std::strcmp(held.uniqueId, want.clip) != 0) return false;
  const auto &ps = props();
  char *type = nullptr, *depth = nullptr, *components = nullptr;
  void *data = nullptr;
  int bounds[4] = {}, rod[4] = {}, rowBytes = 0;
  double scale[2] = {};
  if (ps.propGetDoubleN(held.handle, kOfxImageEffectPropRenderScale, 2, scale) || scale[0] != 1 || scale[1] != 1) return false;
  if (ps.propGetString(held.handle, kOfxPropType, 0, &type) ||
      ps.propGetString(held.handle, kOfxImageEffectPropPixelDepth, 0, &depth) ||
      ps.propGetString(held.handle, kOfxImageEffectPropComponents, 0, &components) ||
      ps.propGetPointer(held.handle, kOfxImagePropData, 0, &data) ||
      ps.propGetIntN(held.handle, kOfxImagePropBounds, 4, bounds) ||
      ps.propGetIntN(held.handle, kOfxImagePropRegionOfDefinition, 4, rod) ||
      ps.propGetInt(held.handle, kOfxImagePropRowBytes, 0, &rowBytes)) return false;
  const int expected[4] = {0, 0, want.w, want.h};
  return !std::strcmp(type, kOfxTypeImage) && !std::strcmp(depth, kOfxBitDepthFloat) &&
         !std::strcmp(components, kOfxImageComponentRGBA) && data == want.data &&
         std::equal(bounds, bounds + 4, expected) && std::equal(rod, rod + 4, expected) &&
         rowBytes == want.w * 4 * (int)sizeof(float);
}

// Plugin-side view of one Render action.
struct Render {
  OfxImageEffectHandle effect;
  OfxImageClipHandle source, output;
  Expected sourceImage, outputImage;
};

// Records the first unmet expectation so a failing case names its cause. Later
// expectations still run, so sanitizers observe every access a case makes.
struct Checks {
  std::string first;
  bool ok = true;
  void expect(bool condition, const char *what) {
    if (!condition && ok) first = what;
    ok &= condition;
  }
};
using Scenario = void (*)(const Render &, Checks &);

// The plugin runs the current scenario inside each Render action. The
// descriptor carries the fixture pointer; there is no global test token.
struct ImagePluginFixture {
  OfxPlugin plugin{};
  PluginEntry entry{};
  std::unique_ptr<Effect> instance;
  std::vector<float> src = std::vector<float>((size_t)kSourceW * kSourceH * 4, 0.5f);
  std::vector<float> dst = std::vector<float>((size_t)kOutputW * kOutputH * 4, 0.0f);
  Scenario scenario = nullptr;
  Checks checks;

  ImagePluginFixture() {
    plugin.pluginIdentifier = "org.rawnode.selftest.image-handles";
    plugin.mainEntry = [](const char *action, const void *handle, OfxPropertySetHandle, OfxPropertySetHandle) {
      if (std::strcmp(action, kOfxImageEffectActionRender) != 0) return kOfxStatReplyDefault;
      const auto effect = static_cast<OfxImageEffectHandle>(const_cast<void *>(handle));
      OfxPropertySetHandle effectProps = nullptr;
      void *self = nullptr;
      effects().getPropertySet(effect, &effectProps);
      props().propGetPointer(effectProps, "selftest-probe", 0, &self);
      auto &fixture = *static_cast<ImagePluginFixture *>(self);
      const Render render{effect,
                          clip(effect, kOfxImageEffectSimpleSourceClipName),
                          clip(effect, kOfxImageEffectOutputClipName),
                          {kOfxImageEffectSimpleSourceClipName, fixture.src.data(), kSourceW, kSourceH},
                          {kOfxImageEffectOutputClipName, fixture.dst.data(), kOutputW, kOutputH}};
      fixture.scenario(render, fixture.checks);
      return kOfxStatOK;
    };
    entry.plugin = &plugin;
    entry.descriptor = std::make_unique<Effect>();
    // As in a filter plugin's describe-in-context action.
    const auto descriptor = reinterpret_cast<OfxImageEffectHandle>(entry.descriptor.get());
    effects().clipDefine(descriptor, kOfxImageEffectOutputClipName, nullptr);
    effects().clipDefine(descriptor, kOfxImageEffectSimpleSourceClipName, nullptr);
    OfxPropertySetHandle maskProps = nullptr;
    effects().clipDefine(descriptor, "Mask", &maskProps);
    props().propSetInt(maskProps, kOfxImageClipPropOptional, 0, 1);
    propSetPointer(H(&entry.descriptor->props), "selftest-probe", 0, this);
    instance = createInstance(entry);
  }
  ~ImagePluginFixture() {
    if (instance) callAction(&plugin, kOfxActionDestroyInstance, instance.get());
  }
  ImagePluginFixture(const ImagePluginFixture &) = delete;
  ImagePluginFixture &operator=(const ImagePluginFixture &) = delete;

  bool render(Scenario next) {
    scenario = next;
    checks = {};
    checks.expect(instance != nullptr, "createInstance");
    if (instance) {
      int width = 0, height = 0;
      queryOutputSize(&plugin, instance.get(), kSourceW, kSourceH, &width, &height);
      checks.expect(width == kSourceW && height == kSourceH, "default RoD uses evaluated input dimensions");
      checks.expect(renderEffect(&plugin, instance.get(), src.data(), dst.data(), kSourceW, kSourceH,
                                 kOutputW, kOutputH) == kOfxStatOK, "render action");
    }
    return checks.ok;
  }

  // Outside renderEffect the instance has no pixels for an image to describe.
  bool fetchOutsideRender() {
    checks = {};
    OfxPropertySetHandle image = nullptr;
    const OfxStatus status = instance
        ? effects().clipGetImage(clip(reinterpret_cast<OfxImageEffectHandle>(instance.get()),
                                      kOfxImageEffectSimpleSourceClipName), 0, nullptr, &image)
        : kOfxStatErrBadHandle;
    checks.expect(status == kOfxStatFailed && !image, "kOfxStatFailed without a handle");
    return checks.ok;
  }
};

// Two requests for each clip's same image are outstanding together. Releasing
// one, in any order, leaves every other handle and its retained property
// storage intact; a repeated release is rejected.
void independentOutstandingHandles(const Render &r, Checks &c) {
  const Held s1 = fetch(r.source), s2 = fetch(r.source);
  const Held o1 = fetch(r.output), o2 = fetch(r.output);
  const OfxPropertySetHandle handles[] = {s1.handle, s2.handle, o1.handle, o2.handle};
  bool distinct = true;
  for (int i = 0; i < 4; ++i) {
    distinct &= handles[i] != nullptr;
    for (int j = 0; j < i; ++j) distinct &= handles[i] != handles[j];
  }
  c.expect(distinct, "each request returns a separate handle");
  c.expect(describes(s1, r.sourceImage) && describes(s2, r.sourceImage),
           "earlier Source handle survives a repeated request");
  c.expect(describes(o1, r.outputImage) && describes(o2, r.outputImage), "Output handles describe Output");
  c.expect(release(s1) == kOfxStatOK, "release first Source");
  c.expect(describes(s2, r.sourceImage) && describes(o1, r.outputImage) && describes(o2, r.outputImage),
           "releasing one Source handle keeps the others");
  c.expect(release(o2) == kOfxStatOK, "release later Output first");
  c.expect(release(s1) == kOfxStatErrBadHandle, "repeated release is rejected");
  c.expect(describes(s2, r.sourceImage) && describes(o1, r.outputImage),
           "out-of-order and repeated releases keep the others");
  c.expect(release(s2) == kOfxStatOK && describes(o1, r.outputImage), "release second Source");
  c.expect(release(o1) == kOfxStatOK, "release first Output last");
}

void optionalMaskIsDisconnected(const Render &r, Checks &c) {
  const auto mask = clip(r.effect, "Mask");
  OfxPropertySetHandle properties = nullptr, image = nullptr;
  effects().clipGetPropertySet(mask, &properties);
  int connected = 1;
  c.expect(props().propGetInt(properties, kOfxImageClipPropConnected, 0, &connected) == kOfxStatOK && !connected,
           "optional plugin Mask is disconnected");
  c.expect(effects().clipGetImage(mask, 0, nullptr, &image) == kOfxStatFailed && !image,
           "optional plugin Mask cannot fetch Source RGB");
}

// Many fetch/release cycles of the same images never reuse or disturb a handle
// that is still outstanding, and each release is final.
void repeatedRequests(const Render &r, Checks &c) {
  const Held kept = fetch(r.source);
  bool cycles = true;
  for (int i = 0; i < 1000; ++i) {
    const bool source = i % 2 == 0;
    const Held image = fetch(source ? r.source : r.output);
    cycles &= image.handle && image.handle != kept.handle &&
              describes(image, source ? r.sourceImage : r.outputImage);
    cycles &= release(image) == kOfxStatOK && release(image) == kOfxStatErrBadHandle;
  }
  c.expect(cycles, "each cycle has its own handle, released exactly once");
  c.expect(describes(kept, r.sourceImage), "held handle survives repeated requests");
  c.expect(release(kept) == kOfxStatOK && release(kept) == kOfxStatErrBadHandle, "held handle released once");
}

// Property sets that did not come from clipGetImage are rejected and left intact.
void invalidReleases(const Render &r, Checks &c) {
  OfxPropertySetHandle clipProps = nullptr, effectProps = nullptr;
  effects().clipGetPropertySet(r.source, &clipProps);
  effects().getPropertySet(r.effect, &effectProps);
  c.expect(effects().clipReleaseImage(nullptr) == kOfxStatErrBadHandle, "null handle is rejected");
  c.expect(effects().clipReleaseImage(clipProps) == kOfxStatErrBadHandle &&
           effects().clipReleaseImage(effectProps) == kOfxStatErrBadHandle, "host property sets are rejected");
  char *name = nullptr, *type = nullptr;
  c.expect(props().propGetString(clipProps, kOfxPropName, 0, &name) == kOfxStatOK &&
           !std::strcmp(name, kOfxImageEffectSimpleSourceClipName) &&
           props().propGetString(effectProps, kOfxPropType, 0, &type) == kOfxStatOK &&
           !std::strcmp(type, kOfxTypeImageEffectInstance), "rejected property sets are untouched");
}

// Each slice holds one Source image while it repeatedly fetches and releases
// both clips, so plugin render threads keep handles outstanding together.
// There is no repeated release: another thread may own a recycled address.
struct ConcurrentFetches {
  const Render &render;
  std::atomic<bool> failed{false};
  std::atomic<unsigned> slices{0};
  explicit ConcurrentFetches(const Render &r) : render(r) {}

  static void slice(unsigned, unsigned, void *arg) {
    auto &self = *static_cast<ConcurrentFetches *>(arg);
    const Render &r = self.render;
    const Held kept = fetch(r.source);
    bool ok = describes(kept, r.sourceImage);
    for (int i = 0; i < 200; ++i) {
      const Held source = fetch(r.source), output = fetch(r.output);
      ok &= source.handle != kept.handle && source.handle != output.handle &&
            describes(source, r.sourceImage) && describes(output, r.outputImage);
      ok &= release(source) == kOfxStatOK && release(output) == kOfxStatOK;
    }
    ok &= describes(kept, r.sourceImage) && release(kept) == kOfxStatOK;
    if (!ok) self.failed = true;
    ++self.slices;
  }
};

void concurrentRenderThreads(const Render &r, Checks &c) {
  constexpr unsigned kSlices = 8;
  ConcurrentFetches fetches(r);
  c.expect(threads().multiThread(ConcurrentFetches::slice, kSlices, &fetches) == kOfxStatOK &&
           fetches.slices == kSlices, "every render-thread slice ran");
  c.expect(!fetches.failed, "render threads fetch and release independent handles");
}
} // namespace

bool testOfxImageHandles() {
  ImagePluginFixture fixture;
  auto check = [&](bool ok, const char *name) {
    if (ok) std::fprintf(stderr, "ok  OFX image handles: %s\n", name);
    else std::fprintf(stderr, "FAIL OFX image handles: %s (%s)\n", name, fixture.checks.first.c_str());
    return ok;
  };
  // Every single-threaded case runs, so a failure lists each broken contract.
  bool ok = check(fixture.render(independentOutstandingHandles),
                  "simultaneous Source/Output handles, out-of-order and repeated release");
  ok &= check(fixture.render(optionalMaskIsDisconnected), "optional Mask stays disconnected from Source RGB");
  ok &= check(fixture.render(repeatedRequests), "repeated requests keep a held handle; each release is final");
  ok &= check(fixture.render(invalidReleases), "invalid releases are rejected without side effects");
  ok &= check(fixture.fetchOutsideRender(), "fetch outside a render returns no handle");
  // Shared, unsynchronised image state can corrupt memory under concurrent
  // fetches, so this case runs only once the single-threaded contract holds.
  return ok && check(fixture.render(concurrentRenderThreads), "concurrent render-thread fetch/release");
}
