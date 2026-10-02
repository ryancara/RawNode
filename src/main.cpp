// Minimal still-image processor host: decode RAW/raster, process, preview, export.

#include "imgio/ImageIO.h"
#include "NodeGraph.h"
#include "RenderPipeline.h"
#include "ofx/OfxHost.h"
#include "processors/CtlProcessor.h"
#include "processors/OfxProcessor.h"
#include "persist/ProjectPersist.h"
#include "UI.h"

#include <tiffio.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>

namespace fs = std::filesystem;

static int fail(const char *msg) {
  fprintf(stderr, "selftest FAILED: %s\n", msg);
  return 1;
}

static bool writeTinyTiff(const fs::path &p, bool halfFloat) {
  TIFF *tif = TIFFOpen(p.c_str(), "w");
  if (!tif) return false;
  TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, 2);
  TIFFSetField(tif, TIFFTAG_IMAGELENGTH, 2);
  TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, 3);
  TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, 16);
  TIFFSetField(tif, TIFFTAG_SAMPLEFORMAT, halfFloat ? SAMPLEFORMAT_IEEEFP : SAMPLEFORMAT_UINT);
  TIFFSetField(tif, TIFFTAG_ORIENTATION, ORIENTATION_TOPLEFT);
  TIFFSetField(tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
  TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB);
  TIFFSetField(tif, TIFFTAG_ROWSPERSTRIP, 2);
  uint16_t row0[6], row1[6];
  if (halfFloat) {
    // half 1.0 = 0x3c00
    uint16_t one = 0x3c00, z = 0;
    row0[0] = one; row0[1] = z; row0[2] = z; row0[3] = z; row0[4] = one; row0[5] = z;
    row1[0] = z; row1[1] = z; row1[2] = one; row1[3] = one; row1[4] = one; row1[5] = one;
  } else {
    row0[0] = 65535; row0[1] = 0; row0[2] = 0; row0[3] = 0; row0[4] = 65535; row0[5] = 0;
    row1[0] = 0; row1[1] = 0; row1[2] = 65535; row1[3] = 65535; row1[4] = 65535; row1[5] = 65535;
  }
  const bool ok = TIFFWriteScanline(tif, row0, 0, 0) >= 0 && TIFFWriteScanline(tif, row1, 1, 0) >= 0;
  TIFFClose(tif);
  return ok;
}

// Renders a gray ramp through every installed filter plugin and writes export formats.
static int selfTest() {
  for (bool half : {false, true}) {
    const fs::path p = fs::temp_directory_path() / (half ? "ofxrawhost-selftest-half.tif" : "ofxrawhost-selftest.tif");
    if (!writeTinyTiff(p, half)) return fail(half ? "tiff write half" : "tiff write");
    Image img;
    ColorSpace cs = ColorSpace::sRGB;
    if (!loadImage(p.string(), img, cs) || img.w != 2 || img.h != 2) return fail(half ? "tiff load half" : "tiff load");
    if (img.px[(size_t)1 * 2 * 4 + 0] < 0.9f) return fail(half ? "tiff pixels half" : "tiff pixels");
    // Untagged float TIFF → Rec.2020; untagged 16-bit int → sRGB.
    if (half && cs != ColorSpace::LinearRec2020) return fail("tiff half colorspace");
    if (!half && cs != ColorSpace::sRGB) return fail("tiff uint colorspace");
    fs::remove(p);
  }

  {
    // Minimal 1x1 RGB PNG, no iCCP (untagged LDR → sRGB).
    static const unsigned char kPng[] = {
        0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52, 0x00, 0x00,
        0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x08, 0x02, 0x00, 0x00, 0x00, 0x90, 0x77, 0x53, 0xde, 0x00, 0x00, 0x00,
        0x0c, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9c, 0x63, 0xf8, 0xcf, 0xc0, 0x00, 0x00, 0x03, 0x01, 0x01, 0x00, 0xc9,
        0xfe, 0x92, 0xef, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82};
    const fs::path p = fs::temp_directory_path() / "ofxrawhost-selftest-cs.png";
    FILE *f = fopen(p.c_str(), "wb");
    if (!f || fwrite(kPng, 1, sizeof kPng, f) != sizeof kPng) {
      if (f) fclose(f);
      return fail("png write");
    }
    fclose(f);
    Image img;
    ColorSpace cs = ColorSpace::LinearRec2020;
    if (!loadImage(p.string(), img, cs) || cs != ColorSpace::sRGB) return fail("png colorspace");
    fs::remove(p);
  }

  {
    // Sidecar V2 round-trip: IDs/backend identity and opaque future parameter
    // JSON must survive even when this build cannot interpret the processor.
    const fs::path source = fs::temp_directory_path() / "rawnode-selftest-source.nef";
    PersistGui gui;
    PersistChain chain;
    chain.selectedNodeId = "node-future";

    PersistNode node;
    node.id = "node-future";
    node.backend = "dctl";
    node.identifier = "FutureTransform.dctl";
    node.label = "Future Transform";
    node.enabled = true;
    node.groupOpen["params"] = true;  // Must not shadow the sibling params object.
    node.paramsJson["amount"] = "0.75";
    node.paramsJson["futureData"] = "{\"curve\":[0,0.5,1],\"mode\":\"test\"}";
    node.paramsJson["unicodeText"] = "\"Caf\\u00e9 \\ud83c\\udf9e\"";
    chain.nodes.push_back(node);

    if (!saveInputSidecar(source.string(), ColorSpace::LinearRec2020, gui, chain))
      return fail("sidecar v2 save");

    PersistSidecar loaded;
    const std::string sidecar = inputSidecarPath(source.string());
    if (!loadSidecarFile(sidecar, loaded)) return fail("sidecar v2 load");
    if (loaded.format != "rawnode-sidecar" || loaded.version != 2) return fail("sidecar v2 version");
    if (loaded.chain.selectedNodeId != "node-future" || loaded.chain.nodes.size() != 1)
      return fail("sidecar v2 node identity");
    const PersistNode &loadedNode = loaded.chain.nodes[0];
    if (loadedNode.backend != "dctl" || loadedNode.identifier != "FutureTransform.dctl" ||
        loadedNode.paramsJson.at("futureData") != "{\"curve\":[0,0.5,1],\"mode\":\"test\"}" ||
        loadedNode.paramsJson.at("amount") != "0.75" || !loadedNode.groupOpen.at("params"))
      return fail("sidecar v2 opaque state");

    std::string decodedUnicode;
    const std::string expectedUnicode = "Caf\xC3\xA9 \xF0\x9F\x8E\x9E";
    if (!parseJsonStringValue(loadedNode.paramsJson.at("unicodeText"), decodedUnicode) ||
        decodedUnicode != expectedUnicode)
      return fail("sidecar v2 unicode string");
    const std::string controlString = std::string("line1\nline2\t") + char(1);
    std::string decodedControl;
    if (!parseJsonStringValue(jsonStringValue(controlString), decodedControl) || decodedControl != controlString)
      return fail("sidecar v2 control string");

    App placeholderApp;
    applyChain(placeholderApp, loaded.chain);
    if (placeholderApp.nodes.size() != 1 || placeholderApp.nodes[0].processor ||
        placeholderApp.nodes[0].id != "node-future" ||
        placeholderApp.nodes[0].storedBackend != "dctl")
      return fail("sidecar v2 missing processor placeholder");
    const PersistChain recaptured = captureChain(placeholderApp);
    if (recaptured.nodes.size() != 1 || recaptured.nodes[0].id != "node-future" ||
        recaptured.nodes[0].paramsJson.at("futureData") != "{\"curve\":[0,0.5,1],\"mode\":\"test\"}")
      return fail("sidecar v2 missing processor preservation");

    fs::remove(sidecar);

    // V1 remains readable and is normalised into the generic persistence model.
    const fs::path legacy = fs::temp_directory_path() / "rawnode-selftest-v1.ofxrawhost.json";
    static const char kV1[] =
        "{\"format\":\"ofxrawhost-sidecar\",\"version\":1,\"kind\":\"input\","
        "\"sourcePath\":\"old.nef\",\"inputColorSpace\":\"Linear Rec.2020\","
        "\"chain\":{\"selectedNode\":0,\"nodes\":[{\"pluginIdentifier\":\"example.ofx\","
        "\"pluginLabel\":\"Example\",\"enabled\":true,\"groupOpen\":{},"
        "\"params\":{\"gain\":1.25}}]}}";
    {
      std::ofstream legacyFile(legacy.string(), std::ios::binary);
      if (!legacyFile) return fail("sidecar v1 test write");
      legacyFile.write(kV1, sizeof(kV1) - 1);
      if (!legacyFile.good()) return fail("sidecar v1 test write");
    }

    PersistSidecar migrated;
    if (!loadSidecarFile(legacy.string(), migrated) || migrated.chain.nodes.size() != 1)
      return fail("sidecar v1 migration");
    if (migrated.chain.nodes[0].backend != "ofx" ||
        migrated.chain.nodes[0].identifier != "example.ofx" ||
        migrated.chain.nodes[0].paramsJson.at("gain") != "1.25")
      return fail("sidecar v1 normalisation");

    // Pre-V2 writers escaped only '"' and '\\' in string parameters, so legacy
    // multi-line values contain raw control characters. They must load intact,
    // including every parameter that follows them.
    {
      std::ofstream legacyFile(legacy.string(), std::ios::binary);
      legacyFile << "{\"format\":\"ofxrawhost-sidecar\",\"version\":1,\"kind\":\"input\","
                    "\"chain\":{\"selectedNode\":0,\"nodes\":[{\"pluginIdentifier\":\"example.ofx\","
                    "\"pluginLabel\":\"Example\",\"enabled\":true,\"groupOpen\":{},"
                    "\"params\":{\"a\":1,\"notes\":\"line1\nline2\tend\",\"z\":0.5}}]}}";
      if (!legacyFile.good()) return fail("sidecar v1 multiline test write");
    }
    PersistSidecar multiline;
    std::string notes;
    if (!loadSidecarFile(legacy.string(), multiline) || multiline.chain.nodes.size() != 1 ||
        multiline.chain.nodes[0].paramsJson.size() != 3 ||
        multiline.chain.nodes[0].paramsJson.at("z") != "0.5" ||
        !parseJsonStringValue(multiline.chain.nodes[0].paramsJson.at("notes"), notes) ||
        notes != "line1\nline2\tend")
      return fail("sidecar v1 legacy multiline string");

    // A malformed params object rejects the sidecar rather than restoring a partial node.
    {
      std::ofstream legacyFile(legacy.string(), std::ios::binary);
      legacyFile << "{\"format\":\"ofxrawhost-sidecar\",\"version\":1,\"kind\":\"input\","
                    "\"chain\":{\"selectedNode\":0,\"nodes\":[{\"pluginIdentifier\":\"example.ofx\","
                    "\"params\":{\"a\":1 \"b\":2}}]}}";
      if (!legacyFile.good()) return fail("sidecar v1 malformed test write");
    }
    PersistSidecar malformed;
    if (loadSidecarFile(legacy.string(), malformed)) return fail("sidecar malformed params rejection");
    fs::remove(legacy);

    const fs::path future = fs::temp_directory_path() / "rawnode-selftest-v3.rawnode.json";
    {
      std::ofstream futureFile(future.string(), std::ios::binary);
      futureFile << "{\"format\":\"rawnode-sidecar\",\"version\":3,\"graph\":{\"nodes\":[]}}";
      if (!futureFile.good()) return fail("sidecar v3 test write");
    }
    PersistSidecar futureSidecar;
    if (loadSidecarFile(future.string(), futureSidecar) || futureSidecar.version != 3)
      return fail("sidecar future version rejection");
    fs::remove(future);

    printf("ok  Sidecar V2\n");
  }

  Image src;
  src.w = 64;
  src.h = 48;
  src.px.assign((size_t)src.w * src.h * 4, 1.0f);
  for (int y = 0; y < src.h; ++y)
    for (int x = 0; x < src.w; ++x)
      for (int c = 0; c < 3; ++c) src.px[((size_t)y * src.w + x) * 4 + c] = 0.18f * std::exp2((x - src.w / 2) / 8.0f);

  // Row-order check: bottom-up means index 0 is the bottom row.
  Image order;
  order.w = 8;
  order.h = 8;
  order.px.assign(8 * 8 * 4, 0.0f);
  for (int x = 0; x < 8; ++x) {
    order.px[((size_t)7 * 8 + x) * 4 + 0] = 1.0f;  // top row in display = last bottom-up row
    order.px[((size_t)7 * 8 + x) * 4 + 3] = 1.0f;
  }
  if (order.px[0] > 0.1f || order.px[(size_t)7 * 8 * 4] < 0.5f) return fail("source rows are not bottom-up");

  loadPlugins();
  if (gPlugins.empty()) return fail("no OFX filter plugins found");

  // Standard CTL backend: load a real .ctl with a sibling import, render it
  // through Processor, persist/restore it through Sidecar V2, then verify a
  // missing script degrades to the normal preserved placeholder.
  {
    const fs::path ctlDir = fs::temp_directory_path() / "rawnode-selftest-ctl";
    fs::create_directories(ctlDir);
    const fs::path libPath = ctlDir / "GainLib.ctl";
    const fs::path scriptPath = ctlDir / "DoubleRGB.ctl";

    {
      std::ofstream lib(libPath.string(), std::ios::binary);
      lib << "float applyGain(float x, float gain) { return x * gain; }\n";
      if (!lib.good()) return fail("ctl library test write");
    }
    {
      std::ofstream script(scriptPath.string(), std::ios::binary);
      script <<
          "import \"GainLib\";\n"
          "void main(\n"
          "  input varying float rIn, input varying float gIn, input varying float bIn,\n"
          "  output varying float rOut, output varying float gOut, output varying float bOut,\n"
          "  output varying float aOut, input varying float aIn = 1.0,\n"
          "  input uniform float gain = 2.0, input uniform int mode = 0,\n"
          "  input uniform bool enabled = true)\n"
          "{\n"
          "  if (!enabled) { rOut = rIn; gOut = gIn; bOut = bIn; }\n"
          "  else if (mode == 1) { rOut = rIn; gOut = applyGain(gIn, gain); bOut = bIn; }\n"
          "  else { rOut = applyGain(rIn, gain); gOut = applyGain(gIn, gain); bOut = applyGain(bIn, gain); }\n"
          "  aOut = aIn;\n"
          "}\n";
      if (!script.good()) return fail("ctl script test write");
    }

    App ctlApp;
    if (!addCtlNode(ctlApp, scriptPath.string()) || ctlApp.nodes.size() != 1 ||
        !ctlApp.nodes[0].processor || ctlApp.nodes[0].processor->backend() != ProcessorBackend::CTL)
      return fail("ctl processor creation");

    Image ctlOut;
    ProcessorResult ctlResult = renderChain(ctlApp, src, ctlOut, 0);
    if (!ctlResult.ok || ctlOut.w != src.w || ctlOut.h != src.h || ctlOut.px.size() != src.px.size())
      return fail("ctl processor render");

    for (size_t i = 0; i + 3 < src.px.size(); i += 4) {
      for (int c = 0; c < 3; ++c)
        if (std::fabs(ctlOut.px[i + c] - src.px[i + c] * 2.0f) > 1e-6f)
          return fail("ctl RGB result");
      if (ctlOut.px[i + 3] != src.px[i + 3]) return fail("ctl alpha result");
    }

    const auto ctlParams = ctlApp.nodes[0].processor->parameters();
    const auto findCtlParam = [&](const char *id) -> const ProcessorParameter * {
      for (const ProcessorParameter &param : ctlParams)
        if (param.id == id) return &param;
      return nullptr;
    };
    const ProcessorParameter *gainParam = findCtlParam("gain");
    const ProcessorParameter *modeParam = findCtlParam("mode");
    const ProcessorParameter *enabledParam = findCtlParam("enabled");
    if (!gainParam || gainParam->type != ParameterType::Double || gainParam->hasRange ||
        !std::get_if<double>(&gainParam->defaultValue) || *std::get_if<double>(&gainParam->defaultValue) != 2.0 ||
        !modeParam || modeParam->type != ParameterType::Integer || modeParam->hasRange ||
        !std::get_if<int>(&modeParam->defaultValue) || *std::get_if<int>(&modeParam->defaultValue) != 0 ||
        !enabledParam || enabledParam->type != ParameterType::Boolean ||
        !std::get_if<bool>(&enabledParam->defaultValue) || !*std::get_if<bool>(&enabledParam->defaultValue))
      return fail("ctl parameter discovery/defaults");

    if (!ctlApp.nodes[0].processor->setParameterValue("gain", 3.0) ||
        !ctlApp.nodes[0].processor->setParameterValue("mode", 1) ||
        !ctlApp.nodes[0].processor->setParameterValue("enabled", true))
      return fail("ctl parameter set");

    Image ctlParamOut;
    if (!renderChain(ctlApp, src, ctlParamOut, 0).ok) return fail("ctl parameter render");
    for (size_t i = 0; i + 3 < src.px.size(); i += 4) {
      if (std::fabs(ctlParamOut.px[i + 0] - src.px[i + 0]) > 1e-6f ||
          std::fabs(ctlParamOut.px[i + 1] - src.px[i + 1] * 3.0f) > 1e-6f ||
          std::fabs(ctlParamOut.px[i + 2] - src.px[i + 2]) > 1e-6f ||
          ctlParamOut.px[i + 3] != src.px[i + 3])
        return fail("ctl parameter values");
    }

    if (!ctlApp.nodes[0].processor->setParameterValue("enabled", false))
      return fail("ctl bool parameter set");
    Image ctlDisabledOut;
    if (!renderChain(ctlApp, src, ctlDisabledOut, 0).ok || ctlDisabledOut.px != src.px)
      return fail("ctl bool parameter render");

    if (!ctlApp.nodes[0].processor->resetParameter("gain") ||
        !ctlApp.nodes[0].processor->resetParameter("mode") ||
        !ctlApp.nodes[0].processor->resetParameter("enabled"))
      return fail("ctl parameter reset");
    Image ctlResetOut;
    if (!renderChain(ctlApp, src, ctlResetOut, 0).ok || ctlResetOut.px != ctlOut.px)
      return fail("ctl parameter reset render");

    // Values are validated against CTL's 32-bit float storage and exact types,
    // and a rejected write leaves the current value untouched.
    Processor &ctlProc = *ctlApp.nodes[0].processor;
    const auto ctlGain = [&]() {
      for (const ProcessorParameter &param : ctlProc.parameters())
        if (param.id == "gain") return std::get<double>(param.value);
      return -1.0;
    };
    if (ctlProc.setParameterValue("gain", std::numeric_limits<double>::infinity()) ||
        ctlProc.setParameterValue("gain", std::numeric_limits<double>::quiet_NaN()) ||
        ctlProc.setParameterValue("gain", 1e39) ||               // overflows 32-bit float
        ctlProc.setParameterValue("gain", 2) ||                  // int for a float input
        ctlProc.setParameterValue("mode", 1.0) ||                // double for an int input
        ctlProc.setParameterValue("enabled", 1) ||               // int for a bool input
        ctlProc.setParameterValue("noSuchParameter", 1.0) || ctlGain() != 2.0)
      return fail("ctl parameter validation");
    if (!ctlProc.setParameterValue("gain", (double)std::numeric_limits<float>::max()) ||
        ctlGain() != (double)std::numeric_limits<float>::max() || !ctlProc.resetParameter("gain"))
      return fail("ctl parameter float range");

    // More pixels than one SIMD chunk (maxSamples() = 4096), so the parameter
    // snapshot written before rendering must hold for every chunk.
    Image ctlWide;
    ctlWide.w = 101;
    ctlWide.h = 77;  // 7777 pixels
    ctlWide.px.resize((size_t)ctlWide.w * ctlWide.h * 4);
    for (size_t i = 0; i < ctlWide.px.size(); ++i) ctlWide.px[i] = (i % 4 == 3) ? 0.5f : 0.001f * (float)(i % 997);
    if (!ctlProc.setParameterValue("gain", 3.0) || !ctlProc.setParameterValue("mode", 1))
      return fail("ctl multi-chunk parameter set");
    Image ctlWideOut;
    if (!ctlProc.render(ctlWide, ctlWideOut, 0).ok || ctlWideOut.px.size() != ctlWide.px.size())
      return fail("ctl multi-chunk render");
    for (size_t i = 0; i + 3 < ctlWide.px.size(); i += 4) {
      if (ctlWideOut.px[i + 0] != ctlWide.px[i + 0] ||
          std::fabs(ctlWideOut.px[i + 1] - ctlWide.px[i + 1] * 3.0f) > 1e-6f ||
          ctlWideOut.px[i + 2] != ctlWide.px[i + 2] || ctlWideOut.px[i + 3] != ctlWide.px[i + 3])
        return fail("ctl multi-chunk parameter values");
    }
    if (!ctlProc.resetParameter("gain") || !ctlProc.resetParameter("mode"))
      return fail("ctl multi-chunk parameter reset");

    // Only defaulted scalar uniform float/int/bool inputs are exposed. An
    // unqualified input is uniform in CTL; varying, half, unsigned int and
    // array inputs stay hidden but their defaults must still apply.
    {
      const fs::path exposurePath = ctlDir / "ExposureRules.ctl";
      {
        std::ofstream script(exposurePath.string(), std::ios::binary);
        script <<
            "void main(\n"
            "  input varying float rIn, input varying float gIn, input varying float bIn,\n"
            "  output varying float rOut, output varying float gOut, output varying float bOut,\n"
            "  input float unqualified = 2.0, input varying float varyingGain = 3.0,\n"
            "  input uniform half halfGain = 0.5, input uniform unsigned int uintGain = 4,\n"
            "  input uniform float arrayGain[2] = {5.0, 7.0})\n"
            "{\n"
            "  rOut = rIn * unqualified * varyingGain * halfGain * uintGain * arrayGain[1];\n"
            "  gOut = gIn; bOut = bIn;\n"
            "}\n";
        if (!script.good()) return fail("ctl exposure rules test write");
      }
      std::string exposureError;
      auto exposure = CtlProcessor::create(exposurePath.string(), &exposureError);
      if (!exposure) return fail(("ctl exposure rules load: " + exposureError).c_str());
      const auto exposed = exposure->parameters();
      if (exposed.size() != 1 || exposed[0].id != "unqualified" || exposed[0].type != ParameterType::Double ||
          exposed[0].hasRange || std::get<double>(exposed[0].defaultValue) != 2.0)
        return fail("ctl exposure rules parameters");

      const auto checkRed = [&](float factor) {
        Image out;
        if (!exposure->render(ctlWide, out, 0).ok || out.px.size() != ctlWide.px.size()) return false;
        for (size_t i = 0; i + 3 < ctlWide.px.size(); i += 4) {
          const float want = ctlWide.px[i] * factor;
          if (std::fabs(out.px[i] - want) > 1e-5f * std::max(1.0f, std::fabs(want))) return false;
        }
        return true;
      };
      if (!checkRed(2.0f * 3.0f * 0.5f * 4.0f * 7.0f)) return fail("ctl hidden input defaults");
      if (!exposure->setParameterValue("unqualified", 1.0) || !checkRed(3.0f * 0.5f * 4.0f * 7.0f))
        return fail("ctl unqualified uniform parameter");
      exposure.reset();
      fs::remove(exposurePath);
    }

    // Persist non-default values so Sidecar V2 proves CTL parameter state is
    // restored through the same backend-neutral path as OFX/native controls.
    if (!ctlApp.nodes[0].processor->setParameterValue("gain", 1.5) ||
        !ctlApp.nodes[0].processor->setParameterValue("mode", 1) ||
        !ctlApp.nodes[0].processor->setParameterValue("enabled", false))
      return fail("ctl persistence parameter setup");
    Image ctlSavedOut;
    if (!renderChain(ctlApp, src, ctlSavedOut, 0).ok) return fail("ctl persistence parameter render");

    const PersistChain ctlSaved = captureChain(ctlApp);
    if (ctlSaved.nodes.size() != 1 || ctlSaved.nodes[0].backend != "ctl" ||
        fs::path(ctlSaved.nodes[0].identifier).filename() != scriptPath.filename() ||
        std::strtod(ctlSaved.nodes[0].paramsJson.at("gain").c_str(), nullptr) != 1.5 ||
        std::strtol(ctlSaved.nodes[0].paramsJson.at("mode").c_str(), nullptr, 10) != 1 ||
        ctlSaved.nodes[0].paramsJson.at("enabled") != "false")
      return fail("ctl persistence capture");

    App ctlRestored;
    applyChain(ctlRestored, ctlSaved);
    if (ctlRestored.nodes.size() != 1 || !ctlRestored.nodes[0].processor ||
        ctlRestored.nodes[0].processor->backend() != ProcessorBackend::CTL)
      return fail("ctl persistence restore");

    const auto restoredCtlParams = ctlRestored.nodes[0].processor->parameters();
    bool restoredGain = false, restoredMode = false, restoredEnabled = false;
    for (const ProcessorParameter &param : restoredCtlParams) {
      if (param.id == "gain") {
        const double *v = std::get_if<double>(&param.value);
        restoredGain = v && *v == 1.5;
      } else if (param.id == "mode") {
        const int *v = std::get_if<int>(&param.value);
        restoredMode = v && *v == 1;
      } else if (param.id == "enabled") {
        const bool *v = std::get_if<bool>(&param.value);
        restoredEnabled = v && !*v;
      }
    }
    if (!restoredGain || !restoredMode || !restoredEnabled)
      return fail("ctl restored parameter values");

    Image ctlRestoredOut;
    if (!renderChain(ctlRestored, src, ctlRestoredOut, 0).ok || ctlRestoredOut.px != ctlSavedOut.px)
      return fail("ctl restored parameter render");

    auto cropIt = std::find_if(gPlugins.begin(), gPlugins.end(),
                               [](const PluginEntry &pe) { return pe.label == "Crop"; });
    if (cropIt == gPlugins.end()) return fail("bundled Crop plugin not found (ctl mixed)");
    const int cropIndex = (int)std::distance(gPlugins.begin(), cropIt);

    App ctlMixed;
    if (!addNode(ctlMixed, cropIndex) || !addCtlNode(ctlMixed, scriptPath.string()) ||
        !addNode(ctlMixed, cropIndex))
      return fail("OFX/CTL mixed node creation");
    if (!ctlMixed.nodes[0].processor->setParameterValue("crop", 40.0) ||
        !ctlMixed.nodes[2].processor->setParameterValue("crop", 40.0))
      return fail("OFX/CTL mixed crop setup");

    App ctlReference;
    if (!addNode(ctlReference, cropIndex) || !addNode(ctlReference, cropIndex) ||
        !ctlReference.nodes[0].processor->setParameterValue("crop", 40.0) ||
        !ctlReference.nodes[1].processor->setParameterValue("crop", 40.0))
      return fail("OFX/CTL mixed reference setup");

    Image ctlMixedOut, ctlReferenceOut;
    if (!renderChain(ctlMixed, src, ctlMixedOut, 0).ok ||
        !renderChain(ctlReference, src, ctlReferenceOut, 0).ok ||
        ctlMixedOut.w != ctlReferenceOut.w || ctlMixedOut.h != ctlReferenceOut.h)
      return fail("OFX/CTL mixed render");

    for (size_t i = 0; i + 3 < ctlReferenceOut.px.size(); i += 4) {
      for (int c = 0; c < 3; ++c)
        if (std::fabs(ctlMixedOut.px[i + c] - ctlReferenceOut.px[i + c] * 2.0f) > 1e-6f)
          return fail("OFX/CTL mixed RGB");
      if (ctlMixedOut.px[i + 3] != ctlReferenceOut.px[i + 3])
        return fail("OFX/CTL mixed alpha");
    }

    fs::remove(scriptPath);
    App ctlMissing;
    applyChain(ctlMissing, ctlSaved);
    if (ctlMissing.nodes.size() != 1 || ctlMissing.nodes[0].processor ||
        ctlMissing.nodes[0].storedBackend != "ctl" ||
        ctlMissing.nodes[0].storedIdentifier != ctlSaved.nodes[0].identifier)
      return fail("ctl missing script placeholder");

    // Syntax and import errors must surface CTL's own diagnostics rather than
    // its generic exception text ('Failed to load CTL module "module.<id>"',
    // 'Cannot find CTL function main.') or a stderr-only message.
    const fs::path badSyntaxPath = ctlDir / "BadSyntax.ctl";
    const fs::path badImportPath = ctlDir / "BadImport.ctl";
    {
      std::ofstream badSyntax(badSyntaxPath.string(), std::ios::binary);
      badSyntax << "void main(input varying float rIn {\n}\n";
      std::ofstream badImport(badImportPath.string(), std::ios::binary);
      badImport <<
          "import \"NoSuchModule\";\n"
          "void main(\n"
          "  input varying float rIn, input varying float gIn, input varying float bIn,\n"
          "  output varying float rOut, output varying float gOut, output varying float bOut)\n"
          "{\n"
          "  rOut = rIn; gOut = gIn; bOut = bIn;\n"
          "}\n";
      if (!badSyntax.good() || !badImport.good()) return fail("ctl error script test write");
    }
    std::string ctlError;
    if (CtlProcessor::create(badSyntaxPath.string(), &ctlError) || ctlError.rfind("BadSyntax.ctl:1: ", 0) != 0 ||
        ctlError.find("module.") != std::string::npos)
      return fail(("ctl syntax error message: " + ctlError).c_str());
    if (CtlProcessor::create(badImportPath.string(), &ctlError) ||
        ctlError.find("Cannot find CTL module \"NoSuchModule\"") == std::string::npos)
      return fail(("ctl import error message: " + ctlError).c_str());
    fs::remove(badSyntaxPath);
    fs::remove(badImportPath);

    fs::remove(libPath);
    fs::remove(ctlDir);
    printf("ok  Standard CTL processor\n");
  }

  // ART compatibility is an adapter on top of the standard CTL runtime. This
  // first seam proves ART_main execution, positional RGB channels, sibling
  // _artlib imports, zero fallback for scalar parameters without CTL defaults,
  // and normal Sidecar V2 persistence. @ART-param presentation metadata is a
  // separate follow-up.
  {
    const fs::path artDir = fs::temp_directory_path() / "rawnode-selftest-art-ctl";
    fs::create_directories(artDir);
    const fs::path artLibPath = artDir / "_artlib.ctl";
    const fs::path artScriptPath = artDir / "ArtCompat.ctl";

    {
      std::ofstream lib(artLibPath.string(), std::ios::binary);
      lib << "float artScale(float x, float gain) { return x * gain; }\n";
      if (!lib.good()) return fail("ART CTL library test write");
    }
    {
      std::ofstream script(artScriptPath.string(), std::ios::binary);
      script <<
          "import \"_artlib\";\n"
          "void ART_main(\n"
          "  varying float R, varying float G, varying float B,\n"
          "  output varying float RR, output varying float GG, output varying float BB,\n"
          "  float gain, int mode, bool enabled)\n"
          "{\n"
          "  if (!enabled) { RR = R; GG = G; BB = B; }\n"
          "  else if (mode == 1) { RR = artScale(R, gain); GG = G; BB = B; }\n"
          "  else { RR = artScale(R, gain); GG = artScale(G, gain); BB = artScale(B, gain); }\n"
          "}\n";
      if (!script.good()) return fail("ART CTL script test write");
    }

    App artApp;
    if (!addCtlNode(artApp, artScriptPath.string()) || artApp.nodes.size() != 1 ||
        !artApp.nodes[0].processor || artApp.nodes[0].processor->backend() != ProcessorBackend::CTL)
      return fail("ART CTL processor creation");

    const auto artParams = artApp.nodes[0].processor->parameters();
    if (artParams.size() != 3 || artParams[0].id != "gain" || artParams[0].type != ParameterType::Double ||
        std::get<double>(artParams[0].defaultValue) != 0.0 ||
        artParams[1].id != "mode" || artParams[1].type != ParameterType::Integer ||
        std::get<int>(artParams[1].defaultValue) != 0 ||
        artParams[2].id != "enabled" || artParams[2].type != ParameterType::Boolean ||
        std::get<bool>(artParams[2].defaultValue))
      return fail("ART CTL scalar parameter fallback");

    Image artDefault;
    if (!renderChain(artApp, src, artDefault, 0).ok || artDefault.px != src.px)
      return fail("ART CTL default render");

    if (!artApp.nodes[0].processor->setParameterValue("gain", 2.0) ||
        !artApp.nodes[0].processor->setParameterValue("mode", 1) ||
        !artApp.nodes[0].processor->setParameterValue("enabled", true))
      return fail("ART CTL parameter set");

    Image artOut;
    if (!renderChain(artApp, src, artOut, 0).ok || artOut.px.size() != src.px.size())
      return fail("ART CTL render");
    for (size_t i = 0; i + 3 < src.px.size(); i += 4) {
      if (std::fabs(artOut.px[i + 0] - src.px[i + 0] * 2.0f) > 1e-6f ||
          artOut.px[i + 1] != src.px[i + 1] || artOut.px[i + 2] != src.px[i + 2] ||
          artOut.px[i + 3] != src.px[i + 3])
        return fail("ART CTL RGB/alpha result");
    }

    const PersistChain artSaved = captureChain(artApp);
    App artRestored;
    applyChain(artRestored, artSaved);
    if (artRestored.nodes.size() != 1 || !artRestored.nodes[0].processor ||
        artRestored.nodes[0].processor->backend() != ProcessorBackend::CTL)
      return fail("ART CTL persistence restore");
    Image artRestoredOut;
    if (!renderChain(artRestored, src, artRestoredOut, 0).ok || artRestoredOut.px != artOut.px)
      return fail("ART CTL restored render");

    // @ART-param metadata supplies defaults with ART's documented precedence:
    // metadata default, then the CTL default, then zero. Each layer is used by
    // one parameter here, and gain's metadata default must beat its CTL one.
    const fs::path artMetaPath = artDir / "ArtMeta.ctl";
    {
      std::ofstream script(artMetaPath.string(), std::ios::binary);
      script <<
          "// @ART-param: [\"gain\", \"Gain\", 0.0, 4.0, 1.5, 0.01]\n"
          "// @ART-param: [\"mode\", \"Mode\", [\"All\", \"Red only\"], 1]\n"
          "// @ART-param: [\"enabled\", \"Enabled\", true]\n"
          "// @ART-param: [\"bias\", \"Bias\", -1.0, 1.0]\n"
          "// @ART-param: [\"steps\", \"Steps\", 0, 10]\n"
          "void ART_main(\n"
          "  varying float r, varying float g, varying float b,\n"
          "  output varying float ro, output varying float go, output varying float bo,\n"
          "  int mode, bool enabled, float bias, float gain = 9.0, int steps = 4)\n"
          "{\n"
          "  if (!enabled) { ro = r; go = g; bo = b; }\n"
          "  else {\n"
          "    ro = r * gain + bias; go = g * gain; bo = b * steps / 4.0;\n"
          "    if (mode == 1) go = g;\n"
          "  }\n"
          "}\n";
      if (!script.good()) return fail("ART metadata script test write");
    }
    App artMeta;
    if (!addCtlNode(artMeta, artMetaPath.string())) return fail("ART metadata script load");
    {
      bool ok = true;
      int seen = 0;
      for (const ProcessorParameter &param : artMeta.nodes[0].processor->parameters()) {
        ++seen;
        if (param.id == "gain") ok = ok && std::get<double>(param.defaultValue) == 1.5 && std::get<double>(param.value) == 1.5;
        else if (param.id == "mode") ok = ok && std::get<int>(param.defaultValue) == 1;
        else if (param.id == "enabled") ok = ok && std::get<bool>(param.defaultValue);
        else if (param.id == "bias") ok = ok && std::get<double>(param.defaultValue) == 0.0;
        else if (param.id == "steps") ok = ok && std::get<int>(param.defaultValue) == 4;
        else ok = false;
      }
      if (!ok || seen != 5) return fail("ART @ART-param default precedence");
    }
    Image artMetaOut;
    if (!renderChain(artMeta, src, artMetaOut, 0).ok) return fail("ART metadata render");
    for (size_t i = 0; i + 3 < src.px.size(); i += 4) {
      if (std::fabs(artMetaOut.px[i] - src.px[i] * 1.5f) > 1e-6f || artMetaOut.px[i + 1] != src.px[i + 1] ||
          std::fabs(artMetaOut.px[i + 2] - src.px[i + 2]) > 1e-6f)
        return fail("ART metadata default render");
    }

    // Untouched parameters reach Sidecar V2 with ART's defaults, not zeros.
    const PersistChain artMetaSaved = captureChain(artMeta);
    const auto &metaJson = artMetaSaved.nodes[0].paramsJson;
    if (metaJson.at("gain") != "1.5" || metaJson.at("mode") != "1" || metaJson.at("enabled") != "true" ||
        metaJson.at("bias") != "0" || metaJson.at("steps") != "4")
      return fail("ART metadata defaults in Sidecar V2");
    App artMetaRestored;
    applyChain(artMetaRestored, artMetaSaved);
    Image artMetaRestoredOut;
    if (artMetaRestored.nodes.size() != 1 || !artMetaRestored.nodes[0].processor ||
        !renderChain(artMetaRestored, src, artMetaRestoredOut, 0).ok || artMetaRestoredOut.px != artMetaOut.px)
      return fail("ART metadata restored render");
    fs::remove(artMetaPath);

    // Entry-point selection and ART contract errors.
    const auto artLoadError = [&](const char *name, const std::string &source) {
      const fs::path path = artDir / name;
      {
        std::ofstream script(path.string(), std::ios::binary);
        script << source;
      }
      std::string error;
      const bool loaded = CtlProcessor::create(path.string(), &error) != nullptr;
      fs::remove(path);
      return loaded ? std::string("<loaded>") : error;
    };
    const auto contains = [](const std::string &text, const char *part) { return text.find(part) != std::string::npos; };
    const std::string artRgb =
        "varying float r, varying float g, varying float b, "
        "output varying float ro, output varying float go, output varying float bo";
    {
      // A script defining both entry points uses standard main().
      const fs::path bothPath = artDir / "Both.ctl";
      {
        std::ofstream script(bothPath.string(), std::ios::binary);
        script << "void main(input varying float rIn, input varying float gIn, input varying float bIn,\n"
                  "  output varying float rOut, output varying float gOut, output varying float bOut)\n"
                  "{ rOut = rIn * 2.0; gOut = gIn; bOut = bIn; }\n"
                  "void ART_main(" << artRgb << ") { ro = r * 10.0; go = g; bo = b; }\n";
      }
      auto both = CtlProcessor::create(bothPath.string());
      Image bothOut;
      if (!both || !both->render(src, bothOut, 0).ok || std::fabs(bothOut.px[0] - src.px[0] * 2.0f) > 1e-6f)
        return fail("CTL main() preferred over ART_main()");
      fs::remove(bothPath);
    }
    if (artLoadError("Neither.ctl", "float f(float x) { return x; }\n") !=
        "CTL script defines neither main() nor ART_main()")
      return fail("CTL missing entry point error");
    if (!contains(artLoadError("FourOut.ctl", "void ART_main(" + artRgb + ", output varying float extra)"
                                              " { ro = r; go = g; bo = b; extra = r; }\n"),
                  "exactly three varying float RGB outputs"))
      return fail("ART exactly three outputs");
    if (!contains(artLoadError("TwoIn.ctl", "void ART_main(varying float r, varying float g,"
                                            " output varying float ro, output varying float go, output varying float bo)"
                                            " { ro = r; go = g; bo = g; }\n"),
                  "three varying float RGB inputs"))
      return fail("ART three inputs");
    if (!contains(artLoadError("Curve.ctl", "void ART_main(" + artRgb + ", float curve[4]) { ro = r * curve[0]; go = g; bo = b; }\n"),
                  "is a curve (float array); ART curve parameters are not supported yet"))
      return fail("ART curve parameter error");
    if (!contains(artLoadError("VaryingParam.ctl", "void ART_main(" + artRgb + ", varying float k) { ro = r * k; go = g; bo = b; }\n"),
                  "must be uniform, not varying"))
      return fail("ART varying parameter error");
    if (!contains(artLoadError("UnknownMeta.ctl", "// @ART-param: [\"nope\", \"Nope\", 0.0, 1.0, 0.5]\n"
                                                  "void ART_main(" + artRgb + ") { ro = r; go = g; bo = b; }\n"),
                  "@ART-param refers to unknown ART_main parameter nope"))
      return fail("ART unknown @ART-param error");
    if (!contains(artLoadError("BadMeta.ctl", "// @ART-param: [\"k\", \"K\", 0.0, 1.0, \"high\"]\n"
                                              "void ART_main(" + artRgb + ", float k) { ro = r * k; go = g; bo = b; }\n"),
                  "invalid @ART-param definition for k"))
      return fail("ART malformed @ART-param error");
    if (!contains(artLoadError("NoLib.ctl", "import \"_artlib_missing\";\n"
                                            "void ART_main(" + artRgb + ") { ro = r; go = g; bo = b; }\n"),
                  "Cannot find CTL module \"_artlib_missing\""))
      return fail("ART missing library import error");

    fs::remove(artScriptPath);
    fs::remove(artLibPath);
    fs::remove(artDir);
    printf("ok  ART CTL entry point\n");
  }

  // Phase 4 mixed-backend seam: OFX -> native Exposure -> OFX must render,
  // persist, restore, and render identically through the generic interfaces.
  {
    auto cropIt = std::find_if(gPlugins.begin(), gPlugins.end(),
                               [](const PluginEntry &pe) { return pe.label == "Crop"; });
    if (cropIt == gPlugins.end()) return fail("bundled Crop plugin not found (mixed)");
    const int cropIndex = (int)std::distance(gPlugins.begin(), cropIt);

    // Crop changes the image size on both sides of the native node, so the
    // native processor must handle an input that is not the source size.
    App mixed;
    if (!addNode(mixed, cropIndex) || !addNativeExposureNode(mixed) || !addNode(mixed, cropIndex))
      return fail("mixed processor node creation");
    if (mixed.nodes.size() != 3 || !mixed.nodes[1].processor ||
        mixed.nodes[1].processor->backend() != ProcessorBackend::Native)
      return fail("mixed processor backend");

    const double exposureEv = 1.0 / 3.0;  // not representable in 6 decimals
    if (!mixed.nodes[0].processor->setParameterValue("crop", 40.0) ||
        !mixed.nodes[1].processor->setParameterValue("exposure", exposureEv) ||
        !mixed.nodes[2].processor->setParameterValue("crop", 40.0))
      return fail("mixed processor parameter set");

    // Reference: the same two crops without the native node.
    App cropOnly;
    if (!addNode(cropOnly, cropIndex) || !addNode(cropOnly, cropIndex) ||
        !cropOnly.nodes[0].processor->setParameterValue("crop", 40.0) ||
        !cropOnly.nodes[1].processor->setParameterValue("crop", 40.0))
      return fail("mixed reference chain");
    Image cropOut;
    if (!renderChain(cropOnly, src, cropOut, 0).ok || cropOut.w >= src.w || cropOut.h >= src.h)
      return fail("mixed reference render");

    Image mixedOut;
    ProcessorResult mixedResult = renderChain(mixed, src, mixedOut, 0);
    if (!mixedResult.ok || mixedOut.w != cropOut.w || mixedOut.h != cropOut.h ||
        mixedOut.px.size() != cropOut.px.size())
      return fail("mixed processor render size");

    const float gain = (float)std::exp2(exposureEv);
    for (size_t i = 0; i + 3 < cropOut.px.size(); i += 4) {
      for (int c = 0; c < 3; ++c) {
        const float want = cropOut.px[i + c] * gain;
        if (std::fabs(mixedOut.px[i + c] - want) > 1e-6f * std::max(1.0f, std::fabs(want)))
          return fail("native exposure gain");
      }
      if (mixedOut.px[i + 3] != cropOut.px[i + 3]) return fail("native exposure alpha");
    }

    const PersistChain saved = captureChain(mixed);
    if (saved.nodes.size() != 3 || saved.nodes[1].backend != "native" ||
        saved.nodes[1].identifier != "rawnode.native.exposure")
      return fail("native exposure persistence capture");
    if (std::strtod(saved.nodes[1].paramsJson.at("exposure").c_str(), nullptr) != exposureEv)
      return fail("native exposure full-precision capture");

    App restored;
    applyChain(restored, saved);
    if (restored.nodes.size() != 3 || !restored.nodes[0].processor || !restored.nodes[1].processor ||
        !restored.nodes[2].processor || restored.nodes[1].processor->backend() != ProcessorBackend::Native)
      return fail("native exposure persistence restore");

    Image restoredOut;
    ProcessorResult restoredResult = renderChain(restored, src, restoredOut, 0);
    if (!restoredResult.ok || restoredOut.w != mixedOut.w || restoredOut.h != mixedOut.h ||
        restoredOut.px != mixedOut.px)
      return fail("mixed processor restored render");

    printf("ok  Mixed OFX/Native processors\n");
  }

  // Generic processor/parameter seam: exercise the same Crop plugin through
  // Processor rather than touching OFX Param/Effect objects directly.
  {
    auto it = std::find_if(gPlugins.begin(), gPlugins.end(),
                           [](const PluginEntry &pe) { return pe.label == "Crop"; });
    if (it == gPlugins.end()) return fail("bundled Crop plugin not found (generic)");
    const int pluginIndex = (int)std::distance(gPlugins.begin(), it);
    auto processor = OfxProcessor::create(pluginIndex);
    if (!processor) return fail("OfxProcessor::create: Crop");

    bool foundCrop = false;
    for (const ProcessorParameter &param : processor->parameters()) {
      if (param.id != "crop") continue;
      foundCrop = param.type == ParameterType::Double;
      break;
    }
    if (!foundCrop) return fail("generic crop parameter missing");

    if (!processor->setParameterValue("crop", 80.0)) return fail("generic crop parameter set");
    Image out;
    ProcessorResult result = processor->render(src, out, 0);
    if (!result.ok || out.w >= src.w || out.h >= src.h) return fail("generic Crop render");

    if (!processor->resetParameter("crop")) return fail("generic crop parameter reset");
    result = processor->render(src, out, 0);
    if (!result.ok || out.w != src.w || out.h != src.h || out.px != src.px)
      return fail("generic Crop reset/render");

    printf("ok  Generic processor parameters\n");
  }

  // Bundled Crop plugin: defaults must be an identity pass-through with a
  // full-size RoD; the crop slider must shrink the RoD and change the rendered
  // output. Checked first so a flaky third-party plugin later in the list
  // cannot mask a regression here.
  {
    auto it = std::find_if(gPlugins.begin(), gPlugins.end(),
                           [](const PluginEntry &pe) { return pe.label == "Crop"; });
    if (it == gPlugins.end()) return fail("bundled Crop plugin not found");
    auto e = createInstance(*it);
    if (!e) return fail("createInstance: Crop");

    // At default (crop=0): RoD matches source size and render is identity.
    int ow = src.w, oh = src.h;
    queryOutputSize(it->plugin, e.get(), src.w, src.h, &ow, &oh);
    if (ow != src.w || oh != src.h) return fail("crop RoD at default != source size");
    Image out;
    out.w = src.w;
    out.h = src.h;
    out.px.assign(src.px.size(), -1.0f);
    if (renderEffect(it->plugin, e.get(), src.px.data(), out.px.data(), src.w, src.h, src.w, src.h, 0) != kOfxStatOK)
      return fail("render: Crop (defaults)");
    if (out.px != src.px) return fail("crop defaults are not identity");

    // At crop=80: RoD should shrink and render to the smaller output should differ.
    Param *crop = findParam(e.get(), "crop");
    if (!crop || crop->v.empty()) return fail("crop param missing");
    crop->v[0] = 80;
    ow = src.w; oh = src.h;
    queryOutputSize(it->plugin, e.get(), src.w, src.h, &ow, &oh);
    if (ow >= src.w || oh >= src.h) return fail("crop RoD did not shrink at 80%");
    out.w = ow;
    out.h = oh;
    out.px.assign((size_t)ow * oh * 4, -1.0f);
    if (renderEffect(it->plugin, e.get(), src.px.data(), out.px.data(), src.w, src.h, ow, oh, 0) != kOfxStatOK)
      return fail("render: Crop (zoomed)");
    bool finite = true, changed = false;
    for (float v : out.px) {
      finite &= std::isfinite(v);
      changed |= v != -1.0f;
    }
    if (!finite || !changed) return fail("crop zoom output");
    if (out.px == src.px) return fail("crop slider had no effect");

    // At crop=0 the window fills the source, so both pan ranges must fall back
    // to half the crop size (not zero). Panning ±100 should slide the window
    // past the source edge, producing black where no source data exists.
    crop->v[0] = 0;
    Param *offsetX = findParam(e.get(), "offsetX");
    Param *offsetY = findParam(e.get(), "offsetY");
    if (!offsetX || offsetX->v.empty()) return fail("offsetX param missing");
    if (!offsetY || offsetY->v.empty()) return fail("offsetY param missing");
    ow = src.w; oh = src.h;
    queryOutputSize(it->plugin, e.get(), src.w, src.h, &ow, &oh);
    out.w = ow; out.h = oh;
    out.px.assign((size_t)ow * oh * 4, -1.0f);
    offsetX->v[0] = 0; offsetY->v[0] = 0;
    if (renderEffect(it->plugin, e.get(), src.px.data(), out.px.data(), src.w, src.h, ow, oh, 0) != kOfxStatOK)
      return fail("render: Crop (centered)");
    for (float v : out.px)
      if (v == 0.0f) return fail("centered crop should have no black pixels");
    offsetY->v[0] = 100;
    std::fill(out.px.begin(), out.px.end(), -1.0f);
    if (renderEffect(it->plugin, e.get(), src.px.data(), out.px.data(), src.w, src.h, ow, oh, 0) != kOfxStatOK)
      return fail("render: Crop (Y offset)");
    changed = false;
    for (float v : out.px)
      changed |= v == 0.0f;
    if (!changed) return fail("Y offset produced no black fill");
    offsetX->v[0] = 100; offsetY->v[0] = 0;
    std::fill(out.px.begin(), out.px.end(), -1.0f);
    if (renderEffect(it->plugin, e.get(), src.px.data(), out.px.data(), src.w, src.h, ow, oh, 0) != kOfxStatOK)
      return fail("render: Crop (X offset)");
    changed = false;
    for (float v : out.px)
      changed |= v == 0.0f;
    if (!changed) return fail("X offset produced no black fill");
    callAction(it->plugin, kOfxActionDestroyInstance, e.get());
    printf("ok  Crop zoom\n");
  }

  for (auto &pe : gPlugins) {
    auto e = createInstance(pe);
    if (!e) return fail(("createInstance: " + pe.label).c_str());
    int ow = src.w, oh = src.h;
    queryOutputSize(pe.plugin, e.get(), src.w, src.h, &ow, &oh);
    Image out;
    out.w = ow;
    out.h = oh;
    out.px.assign((size_t)ow * oh * 4, -1.0f);
    const OfxStatus st = renderEffect(pe.plugin, e.get(), src.px.data(), out.px.data(), src.w, src.h, ow, oh, 0);
    callAction(pe.plugin, kOfxActionDestroyInstance, e.get());
    if (st != kOfxStatOK) return fail(("render: " + pe.label).c_str());
    bool finite = true, touched = false;
    for (float v : out.px) {
      finite &= std::isfinite(v);
      touched |= v != -1.0f;
    }
    if (!finite || !touched) return fail(("output: " + pe.label).c_str());
    const fs::path dir = fs::temp_directory_path();
    bool written = true;
    for (const char *ext : {"png", "jpg"}) {
      const fs::path p = dir / ("ofxrawhost-selftest." + std::string(ext));
      written &= writeImage(out, p.string());
      fs::remove(p);
    }
    if (!written) return fail("export");
    printf("ok  %s\n", pe.label.c_str());
  }
  return 0;
}

int main(int argc, char **argv) {
  if (argc > 1 && !strcmp(argv[1], "--selftest")) return selfTest();
  const std::string path = (argc > 1 && argv[1][0] != '-') ? argv[1] : "";
  return runApp(path);
}
