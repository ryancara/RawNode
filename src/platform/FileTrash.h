#pragma once

#include <cstdint>
#include <functional>
#include <string>

// Platform capability only; individual volumes/operations may still refuse.
bool fileTrashSupported();

// Recoverable native Trash only. False leaves an explanation; no permanent
// deletion fallback is permitted, including on unsupported volumes.
bool moveFileToTrash(const std::string &path, std::string &error);
using TrashOperation = std::function<bool(const std::string &, std::string &)>;

// A no-follow identity check for the exact directory entry being confirmed.
// Directory timestamps/size are excluded because removing children changes them.
struct TrashFileIdentity {
  uint64_t volume = 0, file = 0, size = 0, modified = 0;
  bool operator==(const TrashFileIdentity &other) const {
    return volume == other.volume && file == other.file && size == other.size && modified == other.modified;
  }
};
bool inspectTrashPath(const std::string &path, bool directory, TrashFileIdentity &identity, std::string &error);
