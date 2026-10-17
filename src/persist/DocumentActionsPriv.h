#pragma once

#include "persist/DocumentActions.h"
#include "persist/ProjectPersistPriv.h"

namespace document_detail {
// Operation-local self-test seams, never stored in App or installed globally.
// Production passes defaults. File faults use the existing atomic writer;
// synthetic RAW decoding exercises colour publication without claiming NEF coverage.
struct ActionTestOptions {
  bool testOperations = false;
  persist_detail::FileFault readFault = persist_detail::FileFault::None;
  std::function<persist_detail::FileFault()> beforeSave;
  decltype(&loadImage) decode = loadImage;
};
void setRawWorkingEncoding(App &app, RgbGamut gamut, TransferFunction gamma,
                           const ActionTestOptions &options);
bool applyGrade(App &app, const PersistGrade &grade,
                const ActionTestOptions &options);
bool savePreset(App &app, const std::string &path, const OverwritePrompt &confirm,
                const ActionTestOptions &options);
bool savePhoto(App &app, std::string &error, const ActionTestOptions &options);
bool leavePhoto(App &app, const SaveFailurePrompt &prompt, const ActionTestOptions &options);
bool openPhoto(App &app, const std::string &path, const SaveFailurePrompt &prompt,
               const ActionTestOptions &options);
bool openFolder(App &app, const std::string &dir, const SaveFailurePrompt &prompt,
                const ActionTestOptions &options);
}  // namespace document_detail
