#include "SidecarFileTests.h"
#include "SidecarFileTestAccess.h"
#include "GraphPersistenceTestAccess.h"
#include "persist/Json.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <set>
#include <stdexcept>

#define CHECK(expression) do { \
  if (!(expression)) { \
    std::fprintf(stderr, "sidecar file check failed at line %d: %s\n", __LINE__, #expression); \
    return false; \
  } \
} while (false)

namespace {
namespace fs = std::filesystem;
using Fault = SidecarFileTestAccess::Fault;

#ifndef _WIN32
struct TestDirectory {
  fs::path path;
  TestDirectory() {
    const auto pattern = (fs::temp_directory_path() / "rawnode-file-test-XXXXXX").string();
    std::vector<char> name(pattern.begin(), pattern.end());
    name.push_back('\0');
    if (!mkdtemp(name.data())) throw std::runtime_error("Cannot create sidecar test directory");
    path = name.data();
  }
  ~TestDirectory() { std::error_code ignored; fs::remove_all(path, ignored); }
};
#endif

void write(const fs::path &path, const std::string &text) {
  std::ofstream stream(path, std::ios::binary);
  stream << text;
  stream.close();
  if (!stream) throw std::runtime_error("Cannot write sidecar fixture: " + path.string());
}
std::string bytes(const fs::path &path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) throw std::runtime_error("Cannot read sidecar fixture: " + path.string());
  return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}
std::set<std::string> entries(const fs::path &path) {
  std::set<std::string> result;
  for (const auto &entry : fs::directory_iterator(path)) result.insert(entry.path().filename().string());
  return result;
}
std::string rawSection(const fs::path &path, const char *key) {
  persist_json::Value root;
  std::string error;
  if (!persist_json::read(bytes(path), root, error)) throw std::runtime_error(error);
  const auto found = root.object.find(key);
  return found == root.object.end() ? "" : found->second.raw;
}
std::string file(const std::string &grade = "", const std::string &metadata = "") {
  return std::string(R"({"format":"rawnode","version":1)") +
      (grade.empty() ? "" : ",\"grade\":" + grade) +
      (metadata.empty() ? "" : ",\"metadata\":" + metadata) + "}\n";
}
const std::string colors = R"("color":{"rawWorking":{"colorSpace":"rec2020","gamma":"linear"},"output":{"colorSpace":"rec709","gamma":"srgb"}})";
const std::string sourceGrade = "{" + colors + R"(,"graph":{"primarySource":"camera","documentOutput":{"node":"camera","port":"image"},"nodes":[{"id":"camera","kind":"source"}]}})";
// Shuffled records, explicit connections and output, unavailable backend,
// opaque precise values and a known-but-backend-invalid parameter. File-level
// metadata work must not restore or normalise any of this processor state.
const std::string opaqueGrade = "{\n " + colors + R"(,
 "graph": {"primarySource":"camera", "documentOutput":{"node":"known","port":"image"},
 "nodes":[
 {"id":"known","kind":"image-processor","backend":"native","identifier":"rawnode.native.exposure","enabled":false,"params":{"exposure":true,"future":{"integer":9007199254740993,"decimal":0.123456789012345678901234567890,"huge":1e9999,"text":"caf\u00e9 \ud83e\udd8a"}},"inputs":{"image":{"node":"missing","port":"image"}}},
 {"id":"missing","kind":"image-processor","backend":"ctl","identifier":"/unavailable/pr2-script.ctl","enabled":true,"params":{"gain":0.100000000000000000000000000001},"inputs":{"image":{"node":"camera","port":"image"}}},
 {"id":"camera","kind":"source"}]},
 "positions":{"known":{"x":-12.5,"y":8.25},"camera":{"x":0,"y":100}}
})";
PersistGrade replacementGrade() {
  PersistFile parsed;
  std::string error;
  if (!decodeGraphFile(file(sourceGrade), parsed, error)) throw std::runtime_error(error);
  return *parsed.grade;
}

bool independentSections(const fs::path &dir) {
  const auto path = (dir / "photo.nef.rawnode.json").string();
  PersistFile loaded;
  loaded.metadataJson = R"({"mark":"reject"})";
  std::string error;
  CHECK(readGraphSidecar(path, loaded, error) == SidecarReadStatus::Absent && error.empty());
  CHECK(loaded.metadataJson == R"({"mark":"reject"})");
  CHECK(updateSidecarMark(path, PhotoMark::Pick, error));
  CHECK(readGraphSidecar(path, loaded, error) == SidecarReadStatus::Loaded);
  CHECK(!loaded.grade && loaded.metadataJson == R"({"mark":"pick"})");
  CHECK(updateSidecarMark(path, PhotoMark::Reject, error));
  CHECK(rawSection(path, "metadata") == R"({"mark":"reject"})");
  CHECK(updateSidecarMark(path, PhotoMark::Unmarked, error));
  CHECK(rawSection(path, "metadata") == "{}" && rawSection(path, "grade").empty());
  write(path, file("", R"({"mark":"unmarked"})"));
  CHECK(updateSidecarMark(path, PhotoMark::Unmarked, error));
  CHECK(rawSection(path, "metadata") == "{}");

  write(path, file(opaqueGrade, R"({"mark":"pick"})") + std::string(20000, ' '));
  for (const auto mark : {PhotoMark::Reject, PhotoMark::Unmarked, PhotoMark::Pick}) {
    CHECK(updateSidecarMark(path, mark, error));
    CHECK(rawSection(path, "grade") == opaqueGrade);
  }
  CHECK(readGraphSidecar(path, loaded, error) == SidecarReadStatus::Loaded);
  CHECK(loaded.grade && loaded.grade->graph.nodes.size() == 3);
  CHECK(loaded.grade->graph.nodes[0].id == "known");
  CHECK(loaded.grade->graph.nodes[0].paramsJson.at("exposure") == "true");
  CHECK(loaded.grade->graph.nodes[1].paramsJson.at("gain") == "0.100000000000000000000000000001");
  CHECK(loaded.grade->graph.documentOutput.nodeId == NodeId{"known"});
  CHECK(loaded.grade->positions.at("known").x == -12.5);

  const auto grade = replacementGrade();
  for (const auto &metadata : {std::string(""), std::string("{}"), std::string("{ \n \"mark\" : \"unmarked\" \n}"),
                               std::string(R"({ "mark" : "\u0075nmarked" })")}) {
    write(path, file(opaqueGrade, metadata));
    CHECK(updateSidecarGrade(path, grade, error));
    CHECK(rawSection(path, "metadata") == metadata);
    CHECK(readGraphSidecar(path, loaded, error) == SidecarReadStatus::Loaded);
    CHECK(loaded.grade && loaded.grade->graph.nodes.size() == 1);
    CHECK(loaded.grade->graph.primarySource == NodeId{"camera"});
  }
  fs::remove(path);
  CHECK(updateSidecarGrade(path, grade, error));
  CHECK(readGraphSidecar(path, loaded, error) == SidecarReadStatus::Loaded);
  CHECK(loaded.grade && !loaded.metadataJson);
  fs::remove(path);
  CHECK(updateSidecarMark(path, PhotoMark::Unmarked, error));
  CHECK(readGraphSidecar(path, loaded, error) == SidecarReadStatus::Loaded);
  CHECK(!loaded.grade && loaded.metadataJson == "{}");
  return true;
}

bool slotSection(const fs::path &dir) {
  const std::string grade = "{" + colors + R"(,"graph":{"primarySource":"camera","documentOutput":{"node":"mix","port":"image"},"nodes":[{"id":"mix","kind":"test-slotted","enabled":true,"inputs":{"reference":null},"slots":[{"id":"3","settings":{},"inputs":{"image":{"node":"camera","port":"image"}}},{"id":"1","settings":{"enabled":false},"inputs":{"image":null,"mask":null}}],"nextSlotId":"5"},{"id":"camera","kind":"source"}]}})";
  const auto path = (dir / "slots.rawnode.json").string();
  const auto original = file(grade, R"({ "mark" : "unmarked" })");
  write(path, original);
  std::string error;
  CHECK(!updateSidecarMark(path, PhotoMark::Pick, error)); // Production still refuses test kinds.
  CHECK(bytes(path) == original);
  CHECK(SidecarFileTestAccess::mark(path, PhotoMark::Pick, error, Fault::None, true));
  CHECK(rawSection(path, "grade") == grade);
  PersistFile parsed;
  CHECK(GraphPersistenceTestAccess::decode(bytes(path), parsed, error));
  const auto &mix = parsed.grade->graph.nodes[0];
  CHECK(mix.nextSlotId == 5 && mix.slots[0].id == SlotId{3} && mix.slots[1].id == SlotId{1});
  CHECK(!mix.slots[1].settings.enabled && !mix.slots[1].inputs.at(ports::mask));
  CHECK(SidecarFileTestAccess::grade(path, *parsed.grade, error, Fault::None, true));
  CHECK(rawSection(path, "metadata") == R"({"mark":"pick"})");
  return true;
}

bool protectedFiles(const fs::path &dir) {
  const auto path = (dir / "protected.rawnode.json").string();
  const auto grade = replacementGrade();
  std::string error;
  for (const auto &original : {std::string("{"), std::string(R"({"format":"other","version":1})"),
      std::string(R"({"format":"rawnode","version":2})"), std::string(R"({"format":"rawnode-sidecar","version":2,"graph":{}})"),
      std::string(R"({"format":"rawnode","version":1,"version":1})"), file() + "junk",
      file("null"), file("{}"), file(sourceGrade, R"({"mark":"unknown"})"),
      file(sourceGrade.substr(0, sourceGrade.size() - 1) + R"(,"unknown":0})"),
      file(std::string(sourceGrade).replace(sourceGrade.find("rec2020"), 7, "unknown")),
      file(std::string(sourceGrade).replace(sourceGrade.find("camera"), 6, "absent")),
      file(sourceGrade, std::string("{\"mark\":\"") + char(0xff) + "\"}")}) {
    write(path, original);
    const auto before = entries(dir);
    PersistFile untouched;
    untouched.metadataJson = "{}";
    CHECK(readGraphSidecar(path, untouched, error) == SidecarReadStatus::Error);
    CHECK(untouched.metadataJson == "{}" && !untouched.grade && error.find(path) != std::string::npos);
    CHECK(!updateSidecarMark(path, PhotoMark::Pick, error));
    CHECK(!updateSidecarGrade(path, grade, error));
    CHECK(error.find(path) != std::string::npos);
    CHECK(bytes(path) == original && entries(dir) == before);
  }
  fs::remove(path);
  fs::create_directory(path);
  CHECK(!updateSidecarMark(path, PhotoMark::Pick, error));
  CHECK(fs::is_directory(path) && fs::is_empty(path));
  fs::remove(path);
#ifndef _WIN32
  const auto target = dir / "target";
  write(target, file());
  fs::create_symlink(target, path);
  CHECK(!updateSidecarMark(path, PhotoMark::Pick, error));
  CHECK(fs::is_symlink(path) && bytes(target) == file());
  fs::remove(path);
  fs::create_symlink(dir / "missing-target", path);
  CHECK(!updateSidecarGrade(path, grade, error));
  CHECK(fs::is_symlink(path) && !fs::exists(dir / "missing-target"));
  fs::remove(path);
#endif
  const auto parentFile = dir / "not-a-directory";
  write(parentFile, "keep");
  PersistFile unused;
  CHECK(readGraphSidecar((parentFile / "child").string(), unused, error) == SidecarReadStatus::Error);
  CHECK(!updateSidecarMark((parentFile / "child").string(), PhotoMark::Pick, error));
  CHECK(bytes(parentFile) == "keep");
  CHECK(!updateSidecarMark((dir / "absent-parent" / "child").string(), PhotoMark::Pick, error));
  CHECK(!fs::exists(dir / "absent-parent")); // No implicit directory creation.

  const auto legacy = dir / "old.nef.ofxrawhost.json";
  const auto next = (dir / "old.nef.rawnode.json").string();
  write(legacy, "legacy bytes, deliberately not parsed");
  const auto before = entries(dir);
  CHECK(readGraphSidecar(next, unused, error) == SidecarReadStatus::Error);
  CHECK(!updateSidecarGrade(next, grade, error) && !updateSidecarMark(next, PhotoMark::Pick, error));
  CHECK(!updateSidecarGrade(legacy.string(), grade, error));
  CHECK(!fs::exists(next) && entries(dir) == before);
  CHECK(bytes(legacy) == "legacy bytes, deliberately not parsed");
  write(next, file()); // A valid new-format file takes precedence over legacy state.
  CHECK(updateSidecarGrade(next, grade, error));
  CHECK(updateSidecarMark(next, PhotoMark::Reject, error));
  CHECK(bytes(legacy) == "legacy bytes, deliberately not parsed");
  return true;
}

bool atomicFailures(const fs::path &dir) {
  const auto path = (dir / "atomic.rawnode.json").string();
  const auto original = file(opaqueGrade, R"({ "mark" : "unmarked" })");
  const auto grade = replacementGrade();
  write(path + ".tmp-keep", "unrelated temporary-looking file");
  std::string error;
  write(path, original);
  std::string unchanged = "unchanged";
  CHECK(persist_detail::readGraphFileText(path, unchanged, error, Fault::Read) == SidecarReadStatus::Error);
  CHECK(unchanged == "unchanged");
  for (bool present : {false, true}) for (bool gradeUpdate : {false, true}) {
    for (const auto &test : {std::make_pair(Fault::TempCreate, "create"), std::make_pair(Fault::Write, "write"),
         std::make_pair(Fault::Flush, "flush"), std::make_pair(Fault::Close, "close"), std::make_pair(Fault::Replace, "replace")}) {
      if (present) write(path, original); else fs::remove(path);
      const auto before = entries(dir);
      const bool success = gradeUpdate ? SidecarFileTestAccess::grade(path, grade, error, test.first)
          : SidecarFileTestAccess::mark(path, PhotoMark::Pick, error, test.first);
      CHECK(!success && error.find(path) != std::string::npos && error.find(test.second) != std::string::npos);
      CHECK(entries(dir) == before && (present ? bytes(path) == original : !fs::exists(path)));
      CHECK(bytes(path + ".tmp-keep") == "unrelated temporary-looking file");
      CHECK(gradeUpdate ? updateSidecarGrade(path, grade, error) : updateSidecarMark(path, PhotoMark::Pick, error));
      CHECK(error.empty());
      if (present && !gradeUpdate) CHECK(rawSection(path, "grade") == opaqueGrade);
      if (present && gradeUpdate) CHECK(rawSection(path, "metadata") == R"({ "mark" : "unmarked" })");
    }
  }
  write(path, original);
  const auto before = entries(dir);
  CHECK(!SidecarFileTestAccess::mark(path, PhotoMark::Pick, error, Fault::Read));
  CHECK(!SidecarFileTestAccess::grade(path, grade, error, Fault::Read));
  CHECK(bytes(path) == original && entries(dir) == before);
  auto invalid = grade;
  invalid.color.rawColorSpace = "unknown";
  CHECK(!SidecarFileTestAccess::grade(path, invalid, error, Fault::TempCreate));
  CHECK(error.find("colour") != std::string::npos && error.find("create") == std::string::npos);
  invalid = grade;
  invalid.positions["camera"] = {std::numeric_limits<double>::infinity(), 0};
  CHECK(!SidecarFileTestAccess::grade(path, invalid, error, Fault::TempCreate));
  CHECK(error.find("Non-finite") != std::string::npos);
  PersistFile opaque;
  CHECK(decodeGraphFile(file(opaqueGrade), opaque, error));
  invalid = *opaque.grade;
  invalid.graph.nodes[0].paramsJson["bad"] = "0,\"injected\":1";
  CHECK(!SidecarFileTestAccess::grade(path, invalid, error, Fault::TempCreate));
  CHECK(error.find("Invalid parameter JSON") != std::string::npos);
  CHECK(!updateSidecarMark(path, static_cast<PhotoMark>(99), error));
  CHECK(bytes(path) == original && entries(dir) == before);
  // An already-open reader must retain the complete original inode after a
  // successful replacement. A truncate-and-write implementation fails this.
  std::ifstream held(path, std::ios::binary);
  CHECK(held.good());
  CHECK(updateSidecarMark(path, PhotoMark::Pick, error));
  const std::string oldBytes{std::istreambuf_iterator<char>(held), std::istreambuf_iterator<char>()};
  CHECK(oldBytes == original && bytes(path) != original && entries(dir) == before);
  return true;
}
}  // namespace

bool testSidecarFiles() {
#ifdef _WIN32
  std::puts("skip graph-native atomic sidecar tests: Windows replacement is not implemented");
#else
  TestDirectory directory;
  CHECK(independentSections(directory.path));
  CHECK(slotSection(directory.path));
  CHECK(protectedFiles(directory.path));
  CHECK(atomicFailures(directory.path));
  std::puts("ok  Step 7 safe sidecar files: exact independent sections, protected destinations, checked atomic failures/cleanup");
#endif
  return true;
}
