#pragma once

#include <cstdint>
#include <functional>
#include <string>

// Platform capability only; individual volumes/operations may still refuse.
bool fileTrashSupported();

enum class TrashOutcome { Trashed, PermanentlyDeleted, Cancelled, Failed };
// Request native recycling. Windows may offer permanent deletion in its own
// confirmation; RawNode never approves it automatically. Failed sets error.
TrashOutcome moveFileToTrash(const std::string &path, std::string &error);
using TrashOperation = std::function<TrashOutcome(const std::string &, std::string &)>;

// A no-follow identity check for the exact directory entry being confirmed.
// Directory timestamps/size are excluded because removing children changes them.
struct TrashFileIdentity {
  uint64_t volume = 0, file = 0, size = 0, modified = 0;
  bool operator==(const TrashFileIdentity &other) const {
    return volume == other.volume && file == other.file && size == other.size && modified == other.modified;
  }
};
bool inspectTrashPath(const std::string &path, bool directory, TrashFileIdentity &identity, std::string &error);
