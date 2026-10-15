#pragma once

#include "persist/ProjectPersistPriv.h"

// No flag on production codecs accepts test kinds. This seam runs the exact
// same schema/validation implementation with the existing test interfaces.
struct GraphPersistenceTestAccess {
  static bool decode(const std::string &json, PersistFile &out, std::string &error) {
    return persist_detail::decodeGraphFile(json, out, error, nullptr, true);
  }
  static bool encode(const PersistFile &file, std::string &json, std::string &error) {
    return persist_detail::encodeGraphFile(file, json, error, nullptr, true);
  }
};
