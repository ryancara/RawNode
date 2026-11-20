#pragma once

#include "persist/DocumentActions.h"
#include "platform/FileTrash.h"

// Value snapshot captured before confirmation, independent of subsequent UI
// selection/filter changes. File identity prevents substituting a replaced photo.
struct PhotoTrashTarget {
  std::string path;
  TrashFileIdentity identity;
  std::string error;
};
struct PhotoTrashRequest {
  std::string folder;
  TrashFileIdentity folderIdentity;
  std::vector<PhotoTrashTarget> photographs;
  size_t hiddenCount = 0;
};
PhotoTrashRequest capturePhotoTrashRequest(const App &app);
bool photoTrashIncludesCurrent(const App &app, const PhotoTrashRequest &request);
std::string photoTrashConfirmationText(const PhotoTrashRequest &request);
using PhotoTrashConfirmation = std::function<bool(const std::string &message)>;

struct PhotoTrashResult {
  // Photograph outcomes only; sidecar failures do not count as failed photos.
  size_t moved = 0, deleted = 0, failed = 0;
  // Stops remaining work. Earlier completed operations are retained.
  bool cancelled = false;
  // Photo/sidecar errors and whole-batch refusal reasons, not one entry per failure.
  std::vector<std::string> errors;
  // Exact photo/sidecar paths permanently deleted after native confirmation.
  std::vector<std::string> deletedPaths;
  std::string message() const;
};
// Caller confirms the captured request and completes current-photo UI edits.
// Native filesystem operations may show their own confirmations during execution.
PhotoTrashResult trashPhotographs(App &app, const PhotoTrashRequest &request,
                                  const SaveFailurePrompt &prompt = {},
                                  const TrashOperation &move = moveFileToTrash);
