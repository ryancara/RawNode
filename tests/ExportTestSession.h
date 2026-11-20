#pragma once

#include "AppState.h"

namespace test_export {

// Drive the same control-thread session as the dialog. Some graph fixtures
// install synthetic live pixels without going through photo opening.
inline bool start(App &app, const std::string &destination) {
  if (app.exportDialog.session && app.exportDialog.session->running()) return false;
  if (app.path.empty()) app.path = "self-test-source.png";
  auto session = std::make_unique<ExportSession>(app, ExportScope::Current, destination, ExistingExport::Overwrite);
  if (!session->running()) return false;
  app.exportDialog.session = std::move(session);
  app.exportDialog.session->advance();
  return true;
}

inline void finish(App &app, ExportSession &session) {
  while (session.running()) {
    app.renderer.joinExport();
    session.advance();
  }
}

inline bool finish(App &app) {
  if (!app.exportDialog.session) return false;
  auto &session = *app.exportDialog.session;
  finish(app, session);
  return session.results().size() == 1 && session.results().front().outcome == ExportOutcome::Exported;
}

inline bool run(App &app, const std::string &destination) {
  return start(app, destination) && finish(app);
}

} // namespace test_export
