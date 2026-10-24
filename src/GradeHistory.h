#pragma once

#include "persist/ProjectPersist.h"

#include <cstddef>
#include <vector>

struct App;

namespace document_detail {
class DocumentMutation;
// Detached logical state only; availability never enters the sidecar schema.
// Every ImageProcessor has an entry: true was working, false was missing.
struct GradeSnapshot {
  PersistGrade grade;
  std::map<std::string, bool> workingProcessors;
};
}

// Current-photo history, owned by App and used only on its control thread.
// No processor instances, image buffers, scheduling or persistence policy.
class GradeHistory {
 public:
  static constexpr size_t defaultUndoSteps = 100;
  static constexpr size_t defaultMemoryLimit = 64 * 1024 * 1024;
  explicit GradeHistory(size_t undoSteps = defaultUndoSteps, size_t memoryLimit = defaultMemoryLimit)
      : undoSteps_(undoSteps), memoryLimit_(memoryLimit) {}

  void clear() noexcept;
  // An optional scope reuses the completed edit's existing execution exclusion.
  bool reset(App &app, const document_detail::DocumentMutation *mutation = nullptr);
  bool recordCompletedEdit(App &app, const document_detail::DocumentMutation *mutation = nullptr);
  bool recordOutputChange(App &app);    // Colours/layout only; never synchronises processors.
  void noteUnrecordedEdit() noexcept { if (!entries_.empty()) unrecorded_ = true; }
  bool undo(App &app);
  bool redo(App &app);

  bool canUndo() const { return !unrecorded_ && !entries_.empty() && cursor_ > 0; }
  bool canRedo() const { return !unrecorded_ && cursor_ + 1 < entries_.size(); }
  bool hasUnrecordedEdit() const { return unrecorded_; }
  size_t size() const { return entries_.size(); }
  size_t cursor() const { return cursor_; }
  size_t bytes() const { return bytes_; }
  // Retained allocations plus a conventional tree-node overhead estimate;
  // excludes temporary capture/codec/reconstruction work and allocator metadata.
  static size_t snapshotBytes(const document_detail::GradeSnapshot &snapshot);

 private:
  struct Entry { document_detail::GradeSnapshot snapshot; size_t bytes = 0; };
  void recordSnapshot(document_detail::GradeSnapshot snapshot);
  bool replay(App &app, bool forward);
  std::vector<Entry> entries_;
  size_t cursor_ = 0, bytes_ = 0;
  size_t undoSteps_, memoryLimit_;
  bool unrecorded_ = false;
};
