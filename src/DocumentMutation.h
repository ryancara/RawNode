#pragma once

#include "AppState.h"

namespace document_detail {

// Internal to document/graph operations. Implementation helpers run inside
// their operation's scope; they neither acquire ownership nor request work.
class DocumentMutation {
 public:
  explicit DocumentMutation(App &app);
  ~DocumentMutation();
  DocumentMutation(const DocumentMutation &) = delete;
  DocumentMutation &operator=(const DocumentMutation &) = delete;

  void changed() { changed_ = true; }
  bool owns(const App &app) const { return &app_ == &app; }
  // Validate control-thread intent before staging an edit that may not commit.
  static void assertControlThread(const App &app);
  // Rebuild source pixels inside this same boundary, without a second barrier
  // or an intermediate preview request (used by RAW encoding reload).
  void rebuildPreview();

 private:
  App &app_;
  bool interrupted_ = false;
  bool changed_ = false;
};

// Prepare source pixels without changing live state. Source replacement can
// allocate/convert first, then commit the completed preview within its scope.
std::shared_ptr<const Image> makePreviewSource(const Image &full, ColorEncoding encoding, int previewRes);

}  // namespace document_detail

// Source/document operation; pixel rebuilding is not renderer execution policy.
void rebuildPreview(App &app);
