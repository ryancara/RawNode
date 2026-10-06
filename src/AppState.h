#pragma once

#include "imgio/ImageIO.h"
#include "processors/Processor.h"
#include "RenderRuntime.h"

#include <GLFW/glfw3.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

enum class NodeInputRole {
  Image,
  Mask,
};

struct NodeInput {
  NodeInputRole role = NodeInputRole::Image;
  std::string sourceNodeId;
};

enum class CompositeMode {
  Normal,
};

// One entry in App::nodes; vector order is the current processing order.
// The node owns its processor, whose lifetime is protected by DocumentMutation.
struct Node {
  std::string id;
  bool enabled = true;
  std::unique_ptr<Processor> processor;

  // Persistence identity is retained even when a processor is unavailable.
  // For live processors these mirror the processor; for missing processors
  // they let Sidecar V2 round-trip the node without deleting it.
  std::string storedBackend;
  std::string storedIdentifier;
  std::string storedLabel;
  std::map<std::string, std::string> preservedParamsJson;

  // Reserved by the graph-capable data model. The current renderer still
  // evaluates a simple serial chain and leaves these at their defaults.
  std::vector<NodeInput> inputs;
  float opacity = 1.0f;
  CompositeMode compositeMode = CompositeMode::Normal;

  // Expanded parameter groups, keyed by the backend-neutral parameter ID.
  std::map<std::string, bool> groupOpen;
};

struct ThumbReady {
  std::string path;
  std::vector<unsigned char> rgba;
  int w = 0, h = 0;
  int edge = 0;  // long-edge cap the job was rendered at
  enum class Kind { Ok, Fail, Canceled } kind = Kind::Fail;
};

struct FilmstripEntry {
  std::string path;
  unsigned int tex = 0;
  int tw = 0, th = 0;
  bool thumbPending = true;
  bool thumbLoading = false;
  bool thumbFailed = false;
  int thumbLru = 0;
};

// Long-edge caps for 16:9 frames; 0 = no downscale.
inline constexpr struct {
  const char *label;
  int maxEdge;
} kPreviewRes[] = {
    {"720p", 1280},
    {"1080p", 1920},
    {"1440p", 2560},
    {"Full res", 0},
};
inline constexpr int kPreviewResCount = 4;

// Shared application state. UI.cpp owns the application loop and thumbnail
// teardown; RenderRuntime owns preview/export execution. Document edits use
// DocumentMutation, while the dedicated mutexes below guard shared snapshots.
struct App {
  // Preview/export workers borrow document/display/status fields. Stop them in
  // the destructor body while all members are alive, regardless of member order.
  ~App() noexcept { renderer.shutdown(); }

  GLFWwindow *window = nullptr;
  unsigned int tex = 0;
  int texW = 0, texH = 0;

  Image full, preview;
  std::string path, status = "Open an image. Source is fed to the plugin as scene-linear.";
  // If a sidecar exists but cannot be safely read, never overwrite it on
  // automatic image switching. Reopening after the file is fixed/removed
  // clears this protection.
  std::string sidecarWriteBlockedPath;
  // Colour state is read by the render worker and written by the UI/document
  // thread. Guard snapshots/updates so gamut+gamma pairs remain coherent.
  mutable std::mutex colorMutex;

  // Canonical colour state. inputEncoding describes the pixels actually in
  // memory, independent of the file's original tag. inputIsRaw is determined
  // by the decoder that successfully opened the current source.
  ColorEncoding inputEncoding{RgbGamut::Rec2020, TransferFunction::Linear};
  bool inputIsRaw = false;

  // Session/default preference for RAWs that do not yet have an explicit
  // per-image setting. Sidecars may override the current image without changing
  // this default; an explicit UI change updates both the image and the default.
  ColorEncoding rawWorkingEncoding{RgbGamut::Rec2020, TransferFunction::Linear};

  // Explicit output tag. It is never inferred from processors or automatically
  // changed by the CST.
  ColorEncoding outputEncoding{RgbGamut::Rec709, TransferFunction::SRGB};
  int exportFormat = 1;  // JPEG
  int jpegQuality = 92;
  int previewRes = 1;  // 1080p
  bool showLeft = true;
  bool showRight = true;
  // Legacy layout sizes (read from old JSON; DockBuilder uses them once if no .ini).
  float leftW = 280.0f;
  float rightW = 420.0f;
  char paramFilter[128] = {};
  char pluginFilter[128] = {};
  float previewZoom = 1.0f;  // 1 = fit in view
  int themeIndex = 2;        // Photoshop
  float previewPanX = 0.0f;
  float previewPanY = 0.0f;
  std::vector<Node> nodes;
  int selectedNode = -1;

  std::string workspaceDir;
  // Protect a workspace file containing future colour IDs this build cannot
  // interpret, just as we protect forward-versioned per-image sidecars.
  bool workspaceWriteBlocked = false;
  std::vector<FilmstripEntry> filmstrip;
  int filmstripIndex = -1;
  bool showFilmstrip = true;
  float filmstripH = 96.0f;
  int filmstripTab = 0;  // 0=All, 1=RAW, 2=Compressed
  bool showAbout = false;
  bool showDonate = false;

  // The thumbnail worker exchanges jobs/results under thumbMutex; GL textures
  // and filmstrip entries stay on the UI thread. runApp joins it before teardown.
  std::atomic<int> filmstripGen{0};
  std::atomic<int> filmstripThumbEdge{256};  // snapped long-edge cap (see kFilmstripThumbEdges)
  int thumbLruTick = 0;
  std::thread thumbThread;
  std::mutex thumbMutex;
  std::condition_variable thumbCv;
  std::deque<std::string> thumbQueue;
  std::unordered_set<std::string> thumbQueued;
  std::deque<ThumbReady> thumbReady;

  std::string pendingWorkspaceDir;
  bool themeApplyPending = false;
  bool layoutApplyPending = false;

  // Application/thumbnail intent is independent of renderer shutdown.
  std::atomic<bool> quit{false};
  // Presentation storage: only display_detail/pumpDisplayUpload publish/consume.
  Image display;  // latest rendered (bottom-up float), guarded by displayMutex
  std::vector<unsigned char> displayRGBA;  // sRGB8 top-down, ready for GL upload
  std::mutex displayMutex;
  bool displayDirty = false;
  int displayGen = 0;

  std::mutex statusMutex;
  void setStatus(const std::string &s) {
    std::lock_guard<std::mutex> lock(statusMutex);
    status = s;
  }
  std::string getStatus() {
    std::lock_guard<std::mutex> lock(statusMutex);
    return status;
  }
  // Declared last as a second lifetime safeguard; explicit destruction above
  // remains authoritative if future App members are added after this one.
  RenderRuntime renderer{*this};
};
