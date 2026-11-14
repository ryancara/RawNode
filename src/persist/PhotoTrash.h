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
  size_t moved = 0, failed = 0;
  bool cancelled = false;
  std::vector<std::string> errors;
  std::string message() const;
};
// Caller confirms the captured request and completes current-photo UI edits.
// No confirmation or UI selection is consulted during execution.
PhotoTrashResult trashPhotographs(App &app, const PhotoTrashRequest &request,
                                  const SaveFailurePrompt &prompt = {},
                                  const TrashOperation &move = moveFileToTrash);
