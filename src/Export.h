#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

struct App;

struct ExportSelection {
  std::vector<std::string> paths;
  std::size_t hidden = 0;
};

ExportSelection selectedPhotographsForExport(const App &app);
bool canExportCurrent(const App &app);

enum class ExportScope { Current, Selected };
enum class ExistingExport { Skip, Overwrite };
enum class ExportOutcome { Exported, Skipped, Failed };

struct ExportPhotoResult {
  ExportOutcome outcome = ExportOutcome::Failed;
  std::string message;
  std::vector<std::string> warnings;
};

struct ExportTarget {
  std::string source;
  std::string destination;
};

// Control-thread owner of one confirmed request and its transient results.
// Each tick takes a finished item or dispatches the next one. Destruction
// drains an in-flight item before releasing the render lease.
class ExportSession {
 public:
  ExportSession(App &app, ExportScope scope, const std::string &destination, ExistingExport existing);
  ~ExportSession();
  void advance();
  void cancel() { cancelRequested_ = true; }
  bool cancelling() const { return cancelRequested_; }
  bool running() const;
  const std::vector<ExportTarget> &targets() const;
  const std::vector<ExportPhotoResult> &results() const;

 private:
  struct State;
  std::unique_ptr<State> state_;
  bool cancelRequested_ = false;
};
