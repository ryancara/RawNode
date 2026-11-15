#include "persist/PhotoTrash.h"

#include "persist/DocumentActionsPriv.h"
#include "ui/Filmstrip.h"

#include <cassert>
#include <filesystem>

namespace fs = std::filesystem;
using document_detail::DocumentMutation;

namespace {
bool currentPhoto(const App &app, const std::string &path) {
  if (app.path.empty()) return false;
  std::error_code ec;
  // Compare spellings too: a missing/replaced current source must still resolve
  // departure, rather than being mistaken for an unrelated unopened photo.
  if (fs::path(app.path).lexically_normal() == fs::path(path).lexically_normal()) return true;
  const auto current = fs::weakly_canonical(app.path, ec);
  if (ec) return false;
  const auto selected = fs::weakly_canonical(path, ec);
  return !ec && current == selected;
}

bool sameIdentity(const std::string &path, bool directory, const TrashFileIdentity &expected, std::string &error) {
  TrashFileIdentity actual;
  if (!inspectTrashPath(path, directory, actual, error)) return false;
  if (actual == expected) return true;
  error = "Path changed since confirmation";
  return false;
}

bool eligiblePhoto(const App &app, const PhotoTrashRequest &request, const PhotoTrashTarget &photo, std::string &error) {
  if (!photo.error.empty()) { error = photo.error; return false; }
  if (app.folderDir != request.folder || !sameIdentity(request.folder, true, request.folderIdentity, error)) {
    if (error.empty()) error = "The current folder changed";
    return false;
  }
  if (fs::path(photo.path).parent_path() != fs::path(request.folder) || !isSupportedImagePath(photo.path) ||
      std::none_of(app.filmstrip.begin(), app.filmstrip.end(), [&](const auto &entry) { return entry.path == photo.path; })) {
    error = "Not a photograph in the current filmstrip folder";
    return false;
  }
  return sameIdentity(photo.path, false, photo.identity, error);
}

void movePhoto(App &app, const PhotoTrashRequest &request, const PhotoTrashTarget &photo,
               const TrashOperation &move, PhotoTrashResult &result) {
  std::string error;
  if (!eligiblePhoto(app, request, photo, error) || !move(photo.path, error)) {
    ++result.failed;
    result.errors.push_back(photo.path + ": " + error);
    return;
  }
  ++result.moved;
  app.filmstripSelection.erase(photo.path);
  // Only the two exact appended names belong to this photo, including the
  // historical contract. Contents are never parsed or rewritten for Trash.
  for (const auto &sidecar : {inputSidecarPath(photo.path), legacyInputSidecarPath(photo.path)}) {
    std::error_code ec;
    const auto status = fs::symlink_status(sidecar, ec);
    if (status.type() == fs::file_type::not_found && (!ec || ec == std::errc::no_such_file_or_directory)) continue;
    TrashFileIdentity identity;
    error.clear();
    if (ec) error = ec.message();
    else if (sameIdentity(request.folder, true, request.folderIdentity, error) &&
             inspectTrashPath(sidecar, false, identity, error) && move(sidecar, error)) continue;
    result.errors.push_back("Photograph moved; sidecar remains at " + sidecar + ": " + error);
  }
}
}  // namespace

PhotoTrashRequest capturePhotoTrashRequest(const App &app) {
  PhotoTrashRequest request;
  request.folder = app.folderDir;
  std::string error;
  if (request.folder.empty() || !inspectTrashPath(request.folder, true, request.folderIdentity, error)) return request;
  // Use filmstrip order, including filtered-out entries. Stale selection paths
  // outside this folder are not filesystem authority.
  for (const auto &entry : app.filmstrip) {
    if (!app.filmstripSelection.count(entry.path)) continue;
    PhotoTrashTarget target;
    target.path = entry.path;
    if (fs::path(entry.path).parent_path() != fs::path(request.folder) || !isSupportedImagePath(entry.path))
      target.error = "Not a photograph in the current folder";
    else inspectTrashPath(entry.path, false, target.identity, target.error);
    if (target.error.empty() && !filmstripEntryVisible(app, entry)) ++request.hiddenCount;
    request.photographs.push_back(std::move(target));
  }
  for (const auto &path : app.filmstripSelection) {
    if (std::any_of(request.photographs.begin(), request.photographs.end(), [&](const auto &photo) { return photo.path == path; })) continue;
    request.photographs.push_back({path, {}, "Not a photograph in the current filmstrip folder"});
  }
  return request;
}

bool photoTrashIncludesCurrent(const App &app, const PhotoTrashRequest &request) {
  return std::any_of(request.photographs.begin(), request.photographs.end(),
      [&](const auto &photo) { return currentPhoto(app, photo.path); });
}

static std::string photographCount(size_t count) {
  return std::to_string(count) + (count == 1 ? " photograph" : " photographs");
}

std::string photoTrashConfirmationText(const PhotoTrashRequest &request) {
  const auto total = request.photographs.size();
  const size_t ineligible = std::count_if(request.photographs.begin(), request.photographs.end(),
                                         [](const auto &photo) { return !photo.error.empty(); });
  const auto eligible = total - ineligible;
  std::string details = ineligible
      ? std::to_string(total) + (total == 1 ? " selected item: " : " selected items: ") + std::to_string(eligible) +
          (eligible == 1 ? " eligible photograph" : " eligible photographs")
      : std::to_string(total) + " selected" + (total == 1 ? " photograph" : " photographs");
  if (request.hiddenCount) details += ", including " + std::to_string(request.hiddenCount) + " hidden by current filters";
  if (ineligible) details += "; " + std::to_string(ineligible) + " already ineligible";
  return "Move " + photographCount(eligible) + " to Trash?\n\n" + details +
      ". Associated RawNode sidecars will also be moved.";
}

std::string PhotoTrashResult::message() const {
  std::string text = "Moved " + photographCount(moved) + " to Trash; " + std::to_string(failed) + " failed.";
  for (const auto &error : errors) text += "\n" + error;
  return text;
}

PhotoTrashResult document_detail::trashPhotographs(App &app, const PhotoTrashRequest &request,
    const SaveFailurePrompt &prompt, const ActionTestOptions &options) {
  DocumentMutation::assertControlThread(app);
  PhotoTrashResult result;
  if (request.photographs.empty()) return result;
  const auto current = std::find_if(request.photographs.begin(), request.photographs.end(),
      [&](const auto &photo) { return currentPhoto(app, photo.path); });
  const bool includesCurrent = current != request.photographs.end();
  if (includesCurrent && app.renderer.exportInProgress()) {
    result.failed = request.photographs.size();
    result.errors.push_back("Wait for the current export to finish before moving its source to Trash.");
    app.setStatus(result.message());
    return result;
  }
  std::string departureError;
  if (includesCurrent && !eligiblePhoto(app, request, *current, departureError)) {
    result.failed = request.photographs.size();
    result.errors.push_back(current->path + ": " + departureError + "; no photographs moved");
    app.setStatus(result.message());
    return result;
  }
  // The save-failure dialog can remain open while a file is replaced externally.
  // Retry must not save the old live grade onto that replacement's sidecar.
  const auto guardedPrompt = [&](const std::string &error) {
    const auto decision = prompt ? prompt(error) : SaveFailureDecision::Cancel;
    if (decision == SaveFailureDecision::Retry && !eligiblePhoto(app, request, *current, departureError))
      return SaveFailureDecision::Cancel;
    return decision;
  };
  if (includesCurrent && !leavePhoto(app, guardedPrompt, options)) {
    result.cancelled = true;
    if (!departureError.empty()) {
      result.failed = request.photographs.size();
      result.errors.push_back(current->path + ": " + departureError + "; no photographs moved");
      app.setStatus(result.message());
    }
    return result;
  }
  // Only current-photo removal needs execution exclusion. No unrelated document
  // capture, preview demand or history change occurs for unopened photographs.
  std::unique_ptr<DocumentMutation> mutation;
  if (includesCurrent) mutation = std::make_unique<DocumentMutation>(app);
  bool movedCurrent = false;
  for (const auto &photo : request.photographs) {
    const bool wasCurrent = currentPhoto(app, photo.path);
    const auto movedBefore = result.moved;
    movePhoto(app, request, photo, options.moveToTrash, result);
    std::error_code ec;
    const bool currentMissing = wasCurrent && fs::symlink_status(photo.path, ec).type() == fs::file_type::not_found &&
        (!ec || ec == std::errc::no_such_file_or_directory);
    if (wasCurrent && (result.moved != movedBefore || currentMissing)) {
      // Retire the save identity immediately, including when a sidecar failed.
      assert(mutation && mutation->owns(app));
      clearPhoto(app, *mutation);
      movedCurrent = true;
    }
  }
  refreshFilmstrip(app, true);
  if (movedCurrent) {
    // Reuse the prepared-open path: the outgoing photo was already saved or
    // explicitly discarded. Never save it again during this transition.
    // openPreparedPhoto intentionally nests DocumentMutation: mutation depth
    // keeps preview dispatch closed until this outer removal/transition ends.
    for (const auto &entry : app.filmstrip) {
      if (openPreparedPhoto(app, entry.path, options)) break;
    }
  }
  app.setStatus(result.message());
  return result;
}

PhotoTrashResult trashPhotographs(App &app, const PhotoTrashRequest &request,
                                  const SaveFailurePrompt &prompt, const TrashOperation &move) {
  document_detail::ActionTestOptions options;
  options.moveToTrash = move;
  return document_detail::trashPhotographs(app, request, prompt, options);
}
