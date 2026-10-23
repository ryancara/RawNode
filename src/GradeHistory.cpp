#include "GradeHistory.h"

#include "AppState.h"
#include "persist/DocumentActionsPriv.h"

#include <algorithm>
#include <stdexcept>
#include <type_traits>

namespace {
size_t stringBytes(const std::string &value) { return value.capacity() + 1; }
template<class Map, class Values>
size_t mapBytes(const Map &map, Values values) {
  size_t bytes = map.size() * (sizeof(typename Map::value_type) + 3 * sizeof(void *));
  for (const auto &entry : map) bytes += values(entry);
  return bytes;
}
size_t outputBytes(const OutputRef &output) {
  return stringBytes(output.nodeId.value) + stringBytes(output.outputPortId.value);
}
size_t bindingsBytes(const InputBindings &inputs) {
  return mapBytes(inputs, [](const auto &entry) {
    return stringBytes(entry.first.value) + (entry.second ? outputBytes(*entry.second) : 0);
  });
}
std::string gradeKey(const PersistGrade &grade) {
  // Node-array order has no logical meaning. Keep opaque parameter tokens and
  // Slot order intact; the existing codec supplies deterministic field ordering.
  PersistFile file; file.grade = grade;
  auto &nodes = file.grade->graph.nodes;
  std::sort(nodes.begin(), nodes.end(), [](const auto &a, const auto &b) { return a.id < b.id; });
  std::string key, error;
  if (!encodeGraphFile(file, key, error)) throw std::runtime_error(error);
  return key;
}
}

size_t GradeHistory::snapshotBytes(const document_detail::GradeSnapshot &snapshot) {
  const auto &grade = snapshot.grade;
  size_t bytes = sizeof(snapshot) + stringBytes(grade.color.rawColorSpace) + stringBytes(grade.color.rawGamma) +
      stringBytes(grade.color.outputColorSpace) + stringBytes(grade.color.outputGamma) +
      stringBytes(grade.graph.primarySource.value) + outputBytes(grade.graph.documentOutput);
  bytes += grade.graph.nodes.capacity() * sizeof(PersistNode);
  for (const auto &node : grade.graph.nodes) {
    bytes += stringBytes(node.id) + stringBytes(node.backend) + stringBytes(node.identifier) + stringBytes(node.label);
    bytes += bindingsBytes(node.inputs) + node.slots.capacity() * sizeof(Slot);
    for (const auto &slot : node.slots) bytes += bindingsBytes(slot.inputs);
    bytes += mapBytes(node.paramsJson, [](const auto &entry) { return stringBytes(entry.first) + stringBytes(entry.second); });
    bytes += mapBytes(node.groupOpen, [](const auto &entry) { return stringBytes(entry.first); });
  }
  bytes += mapBytes(grade.positions, [](const auto &entry) { return stringBytes(entry.first); });
  bytes += mapBytes(snapshot.workingProcessors, [](const auto &entry) { return stringBytes(entry.first); });
  return bytes;
}

void GradeHistory::clear() noexcept {
  entries_.clear();
  replayKey_.reset();
  cursor_ = bytes_ = 0;
  unrecorded_ = false;
}

bool GradeHistory::reset(App &app) {
  document_detail::DocumentMutation::assertControlThread(app);
  if (!app.path.empty() && !app.isPhotoProtected() && !app.renderer.canCaptureSnapshot()) return false;
  clear();
  if (app.path.empty() || app.isPhotoProtected()) return false;
  unrecorded_ = true; // A new photograph awaits its initial baseline.
  return recordCompletedEdit(app);
}

bool GradeHistory::recordCompletedEdit(App &app) {
  document_detail::DocumentMutation::assertControlThread(app);
  if (app.path.empty() || app.isPhotoProtected()) return false;
  // Only successful photo opening/reset activates collection. An empty,
  // unrecorded history is a failed baseline that a later capture can retry.
  if (entries_.empty() && !unrecorded_) return false;
  // Failure retains the live edit and every stored entry/cursor, but traversal
  // must not skip this unrecorded state or follow its obsolete redo branch.
  unrecorded_ = true;
  try {
    document_detail::GradeSnapshot captured;
    if (!document_detail::captureGradeSnapshot(app, captured)) return false;
    const auto key = gradeKey(captured.grade);
    if (!entries_.empty() && captured.workingProcessors == entries_[cursor_].snapshot.workingProcessors &&
        (replayKey_ ? key == *replayKey_ : key == gradeKey(entries_[cursor_].snapshot.grade))) {
      unrecorded_ = false;
      return true;
    }
    const auto bytes = snapshotBytes(captured);
    if (bytes > memoryLimit_) throw std::runtime_error("snapshot exceeds the history memory limit");
    // Allocate before discarding redo. Every operation after reserve is noexcept.
    static_assert(std::is_nothrow_move_constructible_v<Entry> && std::is_nothrow_move_assignable_v<Entry>);
    const auto retained = entries_.empty() ? 0 : cursor_ + 1;
    entries_.reserve(retained + 1);
    for (size_t i = retained; i < entries_.size(); ++i) bytes_ -= entries_[i].bytes;
    entries_.erase(entries_.begin() + retained, entries_.end());
    entries_.push_back({std::move(captured), bytes});
    bytes_ += bytes;
    replayKey_.reset();
    while (entries_.size() > undoSteps_ + 1 || bytes_ > memoryLimit_) {
      bytes_ -= entries_.front().bytes;
      entries_.erase(entries_.begin());
    }
    cursor_ = entries_.size() - 1;
    unrecorded_ = false;
    return true;
  } catch (const std::exception &failure) {
    app.setStatus("Could not record Undo history: " + std::string(failure.what()));
    return false;
  }
}

bool GradeHistory::replay(App &app, bool forward) {
  document_detail::DocumentMutation::assertControlThread(app);
  if (app.path.empty() || !app.renderer.canCaptureSnapshot()) return false;
  if (unrecorded_) {
    app.setStatus("Undo/Redo unavailable: the current edit has not been recorded");
    return false;
  }
  if (forward ? !canRedo() : !canUndo()) return false;
  const auto target = forward ? cursor_ + 1 : cursor_ - 1;
  // Replay uses the Stage 1 internal path, never the ordinary Replace command
  // that records completed edits. OFX creation may legitimately adjust values.
  if (!document_detail::restoreGradeSnapshot(app, entries_[target].snapshot)) return false;
  cursor_ = target;
  replayKey_.reset();
  // Capture only comparison metadata after releasing restoration ownership.
  // OFX may have changed parameters during creation; comparing the next edit
  // against its initial values would create a false step and discard redo.
  // This never replaces an entry or demands exact post-creation equality.
  unrecorded_ = true;
  try {
    document_detail::GradeSnapshot actual;
    if (!document_detail::captureGradeSnapshot(app, actual)) return true;
    auto key = gradeKey(actual.grade);
    if (key != gradeKey(entries_[cursor_].snapshot.grade)) {
      if (key.capacity() + 1 > memoryLimit_ - bytes_)
        throw std::runtime_error("replay comparison exceeds the history memory limit");
      replayKey_ = std::move(key);
    }
    unrecorded_ = false;
  } catch (const std::exception &failure) {
    app.setStatus("Replay succeeded; could not refresh history comparison: " + std::string(failure.what()));
  }
  return true;
}
bool GradeHistory::undo(App &app) { return replay(app, false); }
bool GradeHistory::redo(App &app) { return replay(app, true); }
