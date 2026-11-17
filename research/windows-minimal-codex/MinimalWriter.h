#pragma once
#include <filesystem>
#include <string>

namespace experiment {
enum class Fault { None, Write, Flush, Close, Rename, Cleanup };
struct Policy {
  bool privateCreation = true;
  bool copyDacl = true;
  bool copyLabel = true;
  bool qualifyNtfs = true;
};
bool save(const std::string &path, const std::string &body, std::string &error,
          Policy policy = {}, Fault fault = Fault::None);
}
