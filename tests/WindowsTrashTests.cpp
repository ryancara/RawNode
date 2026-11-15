// Standalone native Windows regression; links the real platform/FileTrash.cpp.
#ifndef _WIN32
#error This test must run on Windows.
#endif
#include "platform/FileTrash.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>

namespace fs = std::filesystem;

static void require(bool passed, const char *message) {
  if (!passed) throw std::runtime_error(message);
}

static std::string bytes(const fs::path &path) {
  std::ifstream in(path, std::ios::binary);
  require(bool(in), "Fixture missing or unreadable");
  return {std::istreambuf_iterator<char>(in), {}};
}

int main() {
  const auto dir = fs::temp_directory_path() /
      ("rawnode-windows-trash-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  try {
    require(fs::create_directory(dir), "Fixture directory must be new");
    // Only this directory is test-owned. Original bytes are also retained in
    // independent fixture copies; a regression must never risk user files.
    const std::string contents = "disposable photograph or opaque sidecar\n";
    for (const auto *name : {"photo.dng", "photo.dng.rawnode.json", "photo.dng.ofxrawhost.json"}) {
      const auto source = dir / name;
      const auto backup = dir / (std::string(name) + ".backup");
      std::ofstream(source, std::ios::binary) << contents;
      fs::copy_file(source, backup);
      require(bytes(backup) == contents, "Independent fixture copy must verify");

      TrashFileIdentity before, after;
      std::string error;
      require(inspectTrashPath(source.string(), false, before, error), "Cannot inspect disposable fixture");
      for (int attempt = 0; attempt < 2; ++attempt) {
        error = attempt ? "stale error" : "";
        require(!moveFileToTrash(source.string(), error), "Windows Trash must refuse every move");
        require(error.find("unsupported") != std::string::npos && error.find("Windows") != std::string::npos,
            "Refusal must explain that Windows Trash is unsupported");
        require(bytes(source) == contents && bytes(backup) == contents, "Refusal must preserve original bytes");
        require(inspectTrashPath(source.string(), false, after, error) && after == before,
            "Refusal must preserve the original file identity");
      }
    }
    const auto missing = dir / "missing.dng";
    std::string error;
    require(!moveFileToTrash(missing.string(), error) && !error.empty() && !fs::exists(missing),
        "Missing paths must also be refused without creating a file");
    fs::remove_all(dir);
    std::puts("PASS Windows Trash: unsupported; photographs and sidecars retain bytes and identity");
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "FAIL Windows Trash: %s\n", error.what());
    // Retain failed fixtures and independent copies for inspection.
    std::fprintf(stderr, "Disposable fixture directory: %s\n", dir.string().c_str());
    return 1;
  }
}
