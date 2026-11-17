#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winioctl.h>
#include <aclapi.h>
#include <sddl.h>

#include "SidecarFileTests.h"
#include "SidecarFileTestAccess.h"
#include "persist/PreferencesPriv.h"
#include "persist/WindowsFile.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <set>
#include <stdexcept>
#include <vector>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "Windows persistence line %d: %s (%lu): %s\n", __LINE__, #x, GetLastError(), error.c_str()); return false; } } while (false)
namespace {
namespace fs = std::filesystem;
using Fault = persist_detail::FileFault;
struct Directory {
  fs::path path = fs::temp_directory_path() / (L"rawnode-native-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
      std::to_wstring(std::chrono::steady_clock::now().time_since_epoch().count()));
  Directory() { if (!fs::create_directory(path)) throw std::runtime_error("Cannot create test directory"); }
  ~Directory() { std::error_code ignored; fs::remove_all(path, ignored); }
};
void write(const fs::path &path, const std::string &bytes) {
  std::ofstream out(path, std::ios::binary); out << bytes; out.close();
  if (!out) throw std::runtime_error("Cannot prepare fixture");
}
std::string bytes(const fs::path &path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) throw std::runtime_error("Cannot independently read fixture");
  return {std::istreambuf_iterator<char>(in), {}};
}
std::set<std::string> entries(const fs::path &path) {
  std::set<std::string> out;
  for (const auto &p : fs::directory_iterator(path)) out.insert(p.path().filename().u8string());
  return out;
}
std::wstring sid() {
  HANDLE token; if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) throw std::runtime_error("token");
  DWORD n = 0; GetTokenInformation(token, TokenUser, nullptr, 0, &n);
  std::vector<unsigned char> data(n);
  if (!GetTokenInformation(token, TokenUser, data.data(), n, &n)) throw std::runtime_error("token user");
  CloseHandle(token);
  wchar_t *text = nullptr;
  if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER *>(data.data())->User.Sid, &text)) throw std::runtime_error("SID");
  std::wstring out(text); LocalFree(text); return out;
}
void dacl(const fs::path &path, const std::wstring &text, bool protect = true) {
  PSECURITY_DESCRIPTOR sd = nullptr;
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(text.c_str(), SDDL_REVISION_1, &sd, nullptr)) throw std::runtime_error("parse ACL");
  PACL acl = nullptr; BOOL p, d;
  GetSecurityDescriptorDacl(sd, &p, &acl, &d);
  DWORD code = SetNamedSecurityInfoW(const_cast<wchar_t *>(path.c_str()), SE_FILE_OBJECT,
      DACL_SECURITY_INFORMATION | (protect ? PROTECTED_DACL_SECURITY_INFORMATION : UNPROTECTED_DACL_SECURITY_INFORMATION), nullptr, nullptr, acl, nullptr);
  LocalFree(sd);
  if (code) throw std::runtime_error("set ACL");
}
struct Snapshot {
  std::string content;
  std::wstring security;
  BY_HANDLE_FILE_INFORMATION info{};
  explicit Snapshot(const fs::path &path) : content(bytes(path)) {
    HANDLE h = CreateFileW(path.c_str(), READ_CONTROL | FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (h == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(h, &info)) throw std::runtime_error("snapshot handle");
    PSECURITY_DESCRIPTOR sd = nullptr;
    DWORD code = GetSecurityInfo(h, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                                nullptr, nullptr, nullptr, nullptr, &sd);
    CloseHandle(h);
    wchar_t *text = nullptr;
    if (code || !ConvertSecurityDescriptorToStringSecurityDescriptorW(sd, SDDL_REVISION_1,
        OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, &text, nullptr)) throw std::runtime_error("snapshot security");
    security = text; LocalFree(text); LocalFree(sd);
  }
  bool same(const fs::path &path) const {
    const Snapshot now(path);
    return content == now.content && security == now.security && info.dwFileAttributes == now.info.dwFileAttributes &&
      info.dwVolumeSerialNumber == now.info.dwVolumeSerialNumber && info.nFileIndexHigh == now.info.nFileIndexHigh && info.nFileIndexLow == now.info.nFileIndexLow &&
      CompareFileTime(&info.ftCreationTime, &now.info.ftCreationTime) == 0 && CompareFileTime(&info.ftLastWriteTime, &now.info.ftLastWriteTime) == 0;
  }
};
bool replace(const fs::path &path, const std::string &body, std::string &error, Fault fault = Fault::None) {
  return persist_detail::replaceFileAtomically(path.u8string(), body, error, fault);
}
bool unicodeAndCallers(const fs::path &root) {
  std::string error;
  const auto path = root / fs::u8path(u8"caf\u00e9_\u65e5\u672c_\U0001f98a.rawnode.json");
  CHECK(updateSidecarMark(path.u8string(), PhotoMark::Pick, error));
  PersistFile file;
  CHECK(readGraphSidecar(path.u8string(), file, error) == SidecarReadStatus::Loaded && photoMark(file) == PhotoMark::Pick);
  PersistGrade grade;
  grade.color = {"rec2020", "linear", "rec709", "srgb"};
  grade.graph.primarySource = NodeId{"camera"};
  grade.graph.documentOutput = {NodeId{"camera"}, ports::image};
  PersistNode source; source.id = "camera"; source.operation = NodeOperation::Source;
  grade.graph.nodes.push_back(source);
  CHECK(updateSidecarGrade(path.u8string(), grade, error));
  CHECK(updateSidecarMark(path.u8string(), PhotoMark::Reject, error));
  CHECK(readGraphSidecar(path.u8string(), file, error) == SidecarReadStatus::Loaded && file.grade && photoMark(file) == PhotoMark::Reject);
  // Preset saving reaches this same grade update after capture/confirmation.
  const auto preset = root / fs::u8path(u8"\u9884\u8bbe.rawnode.json");
  CHECK(updateSidecarGrade(preset.u8string(), grade, error));
  CHECK(readGraphSidecar(preset.u8string(), file, error) == SidecarReadStatus::Loaded && file.grade && !file.metadataJson);
  const auto config = preferences_detail::configPaths("", (root / fs::u8path(u8"\u00e9\u4e2d")).u8string(), error);
  CHECK(!config.preferences.empty() && error.empty());
  Preferences preferences; preferences.jpegQuality = 73;
  CHECK(savePreferences(config.preferences, preferences, error));
  Preferences loaded;
  CHECK(loadPreferences(config.preferences, loaded, error) && loaded.jpegQuality == 73);
  const wchar_t *appData = _wgetenv(L"APPDATA");
  const std::wstring previousAppData = appData ? appData : L"";
  const auto unicodeAppData = root / fs::u8path(u8"\u73af\u5883-\U0001f98a");
  _wputenv_s(L"APPDATA", unicodeAppData.c_str());
  const auto environmentConfig = applicationConfigPaths(error);
  _wputenv_s(L"APPDATA", previousAppData.c_str());
  CHECK(environmentConfig.preferences == (unicodeAppData / L"RawNode" / L"preferences.json").u8string());
  CHECK(savePreferences(environmentConfig.preferences, preferences, error));
  const auto original = bytes(fs::u8path(config.preferences));
  for (auto fault : {Fault::PermissionStat, Fault::PermissionCheck, Fault::TempCreate, Fault::Write, Fault::Flush,
                     Fault::PermissionCopy, Fault::Close, Fault::Replace}) {
    CHECK(!preferences_detail::save(config.preferences, preferences, error, fault));
    CHECK(bytes(fs::u8path(config.preferences)) == original && entries(fs::u8path(config.preferences).parent_path()) == std::set<std::string>{"preferences.json"});
  }
  const auto before = entries(root);
  const std::string invalid = root.u8string() + "/bad-" + char(0xff) + ".json";
  CHECK(!persist_detail::replaceFileAtomically(invalid, "body", error) && error.find("UTF-8") != std::string::npos);
  std::string untouched = "unchanged";
  CHECK(persist_detail::readGraphFileText(invalid, untouched, error, Fault::None) == SidecarReadStatus::Error && untouched == "unchanged");
  CHECK(entries(root) == before);
  CHECK(preferences_detail::configPaths("", invalid, error).preferences.empty());
  CHECK(!persist_detail::replaceFileAtomically(path.u8string() + std::string("\0extra", 6), "x", error));
  return true;
}
bool permissionsAndRefusal(const fs::path &root) {
  std::string error;
  const auto path = root / L"security.json";
  CHECK(replace(path, "original", error));
  for (bool protect : {false, true}) {
    dacl(path, L"D:(A;;FA;;;" + sid() + L")(A;;FR;;;BU)(A;;FA;;;SY)", protect);
    const Snapshot before(path);
    CHECK(SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_ARCHIVE));
    CHECK(replace(path, "replacement", error));
    const Snapshot after(path);
    CHECK(after.security == before.security && after.info.dwFileAttributes == (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_ARCHIVE));
  }
  for (DWORD attributes : {DWORD(FILE_ATTRIBUTE_NORMAL), DWORD(FILE_ATTRIBUTE_HIDDEN), DWORD(FILE_ATTRIBUTE_ARCHIVE),
                           DWORD(FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_ARCHIVE | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED)}) {
    CHECK(SetFileAttributesW(path.c_str(), attributes));
    const Snapshot before(path); const auto names = entries(root);
    for (int i = 0; i < 4; ++i) {
      const std::string content = "replacement " + std::to_string(i);
      CHECK(replace(path, content, error));
      const Snapshot after(path);
      CHECK(after.content == content && after.security == before.security && after.info.dwFileAttributes == before.info.dwFileAttributes);
      CHECK(entries(root) == names);
    }
  }
  dacl(path, L"D:P(D;;0x6;;;" + sid() + L")(A;;FA;;;" + sid() + L")(A;;FA;;;SY)");
  Snapshot denied(path); const auto names = entries(root);
  CHECK(!replace(path, "must not publish", error) && error.find("not writable") != std::string::npos);
  CHECK(denied.same(path) && entries(root) == names);
  dacl(path, L"D:P(A;;FA;;;" + sid() + L")(A;;FA;;;SY)");
  CHECK(SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_READONLY));
  Snapshot readonly(path);
  CHECK(!replace(path, "must not publish", error) && readonly.same(path));
  // Restore only the disposable fixture after checking refusal; never a save fallback.
  CHECK(SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL));
  const auto stream = fs::path(path.wstring() + L":extra"); write(stream, "retain alternate bytes");
  Snapshot streams(path);
  CHECK(!replace(path, "must not publish", error) && error.find("streams") != std::string::npos);
  CHECK(streams.same(path) && bytes(stream) == "retain alternate bytes");
  CHECK(DeleteFileW(stream.c_str()));
  const auto folder = root / L"directory.json"; fs::create_directory(folder);
  CHECK(!replace(folder, "x", error) && fs::is_empty(folder));
  CHECK(!replace(root / L"missing" / L"file.json", "x", error) && !fs::exists(root / L"missing"));
  CHECK(!persist_detail::replaceFileAtomically("\\\\localhost\\C$\\never-created.json", "x", error));
  return true;
}
bool readersAndCleanup(const fs::path &root) {
  const auto path = root / L"readers.json";
  std::string error;
  CHECK(replace(path, "original", error));
  for (DWORD share : {DWORD(FILE_SHARE_READ | FILE_SHARE_WRITE), DWORD(FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE)}) {
    HANDLE reader = CreateFileW(path.c_str(), GENERIC_READ, share, nullptr, OPEN_EXISTING, 0, nullptr);
    CHECK(reader != INVALID_HANDLE_VALUE);
    const Snapshot before(path); const auto names = entries(root);
    const bool saved = replace(path, "replacement", error);
    CloseHandle(reader);
    CHECK(!saved && before.same(path) && entries(root) == names);
  }
  const Snapshot before(path); const auto names = entries(root);
  CHECK(!replace(path, "partial write material", error, Fault::Cleanup));
  CHECK(before.same(path) && error.find("cannot remove temporary file") != std::string::npos);
  auto after = entries(root);
  CHECK(after.size() == names.size() + 1);
  for (const auto &name : after) if (!names.count(name)) {
    const auto temporary = root / fs::u8path(name);
    CHECK(error.find(temporary.u8string()) != std::string::npos && bytes(temporary) == "partial wri");
    CHECK(DeleteFileW(temporary.c_str()));
  }
  CHECK(replace(path, "retry succeeds", error) && error.empty());
  return true;
}
void junction(const fs::path &link, const fs::path &target) {
  fs::create_directory(link);
  struct Mount { DWORD tag; WORD size, reserved, so, sl, po, pl; wchar_t names[2048]; } data{};
  data.tag = IO_REPARSE_TAG_MOUNT_POINT;
  const auto sub = L"\\??\\" + target.wstring(), print = target.wstring();
  data.sl = WORD(sub.size() * 2); data.po = WORD((sub.size() + 1) * 2); data.pl = WORD(print.size() * 2);
  memcpy(data.names, sub.c_str(), data.sl + 2); memcpy(reinterpret_cast<char *>(data.names) + data.po, print.c_str(), data.pl + 2);
  data.size = WORD(8 + data.po + data.pl + 2);
  HANDLE h = CreateFileW(link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
  DWORD n = 0; const BOOL ok = DeviceIoControl(h, FSCTL_SET_REPARSE_POINT, &data, 8 + data.size, nullptr, 0, &n, nullptr);
  CloseHandle(h); if (!ok) throw std::runtime_error("Cannot create junction fixture");
}
bool links(const fs::path &root) {
  std::string error, text;
  const auto target = root / L"target.json", link = root / L"link.json";
  write(target, "preserve target");
  CHECK(CreateSymbolicLinkW(link.c_str(), target.c_str(), SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE));
  const Snapshot before(target); const auto names = entries(root);
  CHECK(!replace(link, "no", error));
  CHECK(persist_detail::readGraphFileText(link.u8string(), text, error, Fault::None) == SidecarReadStatus::Error);
  CHECK(before.same(target) && entries(root) == names && fs::is_symlink(fs::symlink_status(link)));
  CHECK(DeleteFileW(link.c_str()));
  const auto dir = root / L"real-folder", alias = root / L"junction";
  fs::create_directory(dir); write(dir / L"file.json", "keep"); junction(alias, dir);
  CHECK(!replace(alias / L"file.json", "no", error));
  CHECK(persist_detail::readGraphFileText((alias / L"file.json").u8string(), text, error, Fault::None) == SidecarReadStatus::Error);
  CHECK(bytes(dir / L"file.json") == "keep");
  CHECK(preferences_detail::configPaths("", alias.u8string(), error).preferences.empty());
  CHECK(!fs::exists(dir / L"RawNode"));
  CHECK(RemoveDirectoryW(alias.c_str()));
  return true;
}
bool systemSecurity(const fs::path &root, bool createAuditFixture) {
  std::string error;
  for (bool audit : {false, true}) {
    if (audit && !createAuditFixture) {
      std::puts("audit SACL fixture creation is covered by the privileged runner; production never enables privileges");
      continue;
    }
    const auto path = root / (audit ? L"audit.json" : L"integrity.json");
    CHECK(replace(path, "retain protected contents", error));
    HANDLE token = nullptr;
    TOKEN_PRIVILEGES privilege{};
    if (audit) {
      CHECK(OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token));
      privilege.PrivilegeCount = 1;
      CHECK(LookupPrivilegeValueW(nullptr, L"SeSecurityPrivilege", &privilege.Privileges[0].Luid));
      privilege.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
      CHECK(AdjustTokenPrivileges(token, FALSE, &privilege, 0, nullptr, nullptr) && GetLastError() == ERROR_SUCCESS);
    }
    const std::wstring sddl = audit ? L"S:(AU;SAFA;FW;;;" + sid() + L")" : L"S:(ML;;NW;;;LW)";
    PSECURITY_DESCRIPTOR sd = nullptr;
    CHECK(ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &sd, nullptr));
    PACL sacl = nullptr; BOOL present, def;
    CHECK(GetSecurityDescriptorSacl(sd, &present, &sacl, &def));
    const DWORD code = SetNamedSecurityInfoW(const_cast<wchar_t *>(path.c_str()), SE_FILE_OBJECT,
        audit ? SACL_SECURITY_INFORMATION : LABEL_SECURITY_INFORMATION, nullptr, nullptr, nullptr, sacl);
    LocalFree(sd);
    if (audit) {
      privilege.Privileges[0].Attributes = 0;
      CHECK(AdjustTokenPrivileges(token, FALSE, &privilege, 0, nullptr, nullptr));
      CloseHandle(token);
    }
    CHECK(code == ERROR_SUCCESS);
    const Snapshot before(path); const auto names = entries(root);
    CHECK(!replace(path, "must not remove system security", error));
    CHECK(before.same(path) && entries(root) == names);
  }
  return true;
}
}  // namespace

int wmain(int argc, wchar_t **argv) {
  try {
    const bool ordinary = argc == 3 && std::wcscmp(argv[1], L"--ordinary-user") == 0;
    if (ordinary) {
      _wputenv_s(L"TEMP", argv[2]);
      _wputenv_s(L"TMP", argv[2]);
    } else if (argc != 1) throw std::runtime_error("Unexpected native test arguments");
    Directory dir;
    if (!unicodeAndCallers(dir.path) || !testSidecarFiles() || !permissionsAndRefusal(dir.path) || !readersAndCleanup(dir.path) ||
        !links(dir.path) || !systemSecurity(dir.path, !ordinary)) return 1;
    std::puts("ok  MSVC production persistence: UTF-8, sidecars/presets/preferences, ACLs/attributes, streams/reparse refusal, reader failures and cleanup");
    return 0;
  } catch (const std::exception &failure) { std::fprintf(stderr, "%s\n", failure.what()); return 2; }
}
