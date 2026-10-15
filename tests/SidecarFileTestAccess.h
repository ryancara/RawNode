#pragma once
#include "persist/ProjectPersistPriv.h"

struct SidecarFileTestAccess {
  using Fault = persist_detail::FileFault;
  static bool grade(const std::string &path, const PersistGrade &grade, std::string &error,
                    Fault fault = Fault::None, bool testOperations = false) {
    return persist_detail::updateGraphSidecar(path, &grade, PhotoMark::Unmarked, error, fault, testOperations);
  }
  static bool mark(const std::string &path, PhotoMark mark, std::string &error,
                   Fault fault = Fault::None, bool testOperations = false) {
    return persist_detail::updateGraphSidecar(path, nullptr, mark, error, fault, testOperations);
  }
};
