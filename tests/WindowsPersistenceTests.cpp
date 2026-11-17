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
#include "imgio/ImageIO.h"

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
  bool hasAcls = false, protectedDacl = false;
  std::wstring owner;
  std::vector<unsigned char> daclBytes;
  BY_HANDLE_FILE_INFORMATION info{};
  explicit Snapshot(const fs::path &path) : content(bytes(path)) {
    HANDLE h = CreateFileW(path.c_str(), READ_CONTROL | FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (h == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(h, &info)) throw std::runtime_error("snapshot handle");
    DWORD flags = 0;
    if (!GetVolumeInformationW(path.root_path().c_str(), nullptr, 0, nullptr, nullptr, &flags, nullptr, 0)) throw std::runtime_error("snapshot volume");
    hasAcls = (flags & FILE_PERSISTENT_ACLS) != 0;
    if (!hasAcls) { CloseHandle(h); return; }
    PSECURITY_DESCRIPTOR sd = nullptr;
    DWORD code = GetSecurityInfo(h, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                                nullptr, nullptr, nullptr, nullptr, &sd);
    CloseHandle(h);
    wchar_t *text = nullptr;
    if (code || !ConvertSecurityDescriptorToStringSecurityDescriptorW(sd, SDDL_REVISION_1,
        OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, &text, nullptr)) throw std::runtime_error("snapshot security");
    security = text; LocalFree(text);
    PACL acl = nullptr; BOOL present, defaulted;
    GetSecurityDescriptorDacl(sd, &present, &acl, &defaulted);
    if (acl) daclBytes.assign(reinterpret_cast<unsigned char *>(acl), reinterpret_cast<unsigned char *>(acl) + acl->AclSize);
    SECURITY_DESCRIPTOR_CONTROL control = 0; DWORD revision = 0;
    GetSecurityDescriptorControl(sd, &control, &revision); protectedDacl = (control & SE_DACL_PROTECTED) != 0;
    PSID ownerSid = nullptr;
    GetSecurityDescriptorOwner(sd, &ownerSid, &defaulted);
    wchar_t *ownerText = nullptr;
    if (!ConvertSidToStringSidW(ownerSid, &ownerText)) throw std::runtime_error("snapshot owner");
    owner = ownerText; LocalFree(ownerText); LocalFree(sd);
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
  for (DWORD attributes : {DWORD(FILE_ATTRIBUTE_ARCHIVE), DWORD(FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_ARCHIVE),
                           DWORD(FILE_ATTRIBUTE_ARCHIVE | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED),
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
  for (DWORD attributes : {DWORD(FILE_ATTRIBUTE_NORMAL), DWORD(FILE_ATTRIBUTE_HIDDEN), DWORD(FILE_ATTRIBUTE_SYSTEM), DWORD(FILE_ATTRIBUTE_NOT_CONTENT_INDEXED)}) {
    CHECK(SetFileAttributesW(path.c_str(), attributes));
    CHECK(replace(path, "archive becomes set after modification", error));
    const Snapshot after(path);
    CHECK(after.info.dwFileAttributes == ((attributes & ~FILE_ATTRIBUTE_NORMAL) | FILE_ATTRIBUTE_ARCHIVE));
  }
  CHECK(SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_ARCHIVE));
  dacl(path, L"D:P(A;;FA;;;" + sid() + L")(XA;;FR;;;BU;(@User.Title == \"Manager\"))");
  const Snapshot conditional(path);
  CHECK(replace(path, "opaque conditional DACL", error));
  CHECK(Snapshot(path).daclBytes == conditional.daclBytes && Snapshot(path).protectedDacl == conditional.protectedDacl);
  dacl(path, L"D:P(D;;0x2;;;" + sid() + L")(A;;FA;;;" + sid() + L")(A;;FA;;;SY)");
  Snapshot denied(path); const auto names = entries(root);
  CHECK(!replace(path, "must not publish", error) && error.find("not writable") != std::string::npos);
  CHECK(denied.same(path) && entries(root) == names);
  dacl(path, L"D:P(A;;FA;;;" + sid() + L")(A;;FA;;;SY)");
  CHECK(SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_READONLY));
  Snapshot readonly(path);
  CHECK(!replace(path, "must not publish", error) && readonly.same(path));
  CHECK(SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_ARCHIVE));
  for (const auto *name : {L":Zone.Identifier", L":com.dropbox.attrs", L":user-data"}) {
    const auto stream = fs::path(path.wstring() + name); write(stream, "incidental stream data");
    CHECK(bytes(stream) == "incidental stream data");
    CHECK(replace(path, "JSON data stream replacement", error));
    HANDLE missing = CreateFileW(stream.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    const auto code = GetLastError();
    if (missing != INVALID_HANDLE_VALUE) CloseHandle(missing);
    CHECK(bytes(path) == "JSON data stream replacement" && missing == INVALID_HANDLE_VALUE && code == ERROR_FILE_NOT_FOUND);
  }
  const auto other = root / L"other-hard-link";
  CHECK(CreateHardLinkW(other.c_str(), path.c_str(), nullptr));
  const Snapshot linked(path);
  CHECK(replace(path, "new pathname contents", error));
  CHECK(linked.same(other) && bytes(path) == "new pathname contents");
  CHECK(DeleteFileW(other.c_str()));
  const auto inherited = root / L"new-inherited.json", exemplar = root / L"inheritance-control";
  write(exemplar, "control");
  CHECK(replace(inherited, "new folder-inherited file", error));
  CHECK(Snapshot(inherited).security == Snapshot(exemplar).security);
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
  CHECK(SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_HIDDEN));
  for (DWORD share : {DWORD(FILE_SHARE_READ | FILE_SHARE_WRITE), DWORD(FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE)}) {
    HANDLE reader = CreateFileW(path.c_str(), GENERIC_READ, share, nullptr, OPEN_EXISTING, 0, nullptr);
    CHECK(reader != INVALID_HANDLE_VALUE);
    const Snapshot before(path); const auto names = entries(root);
    const bool saved = replace(path, "replacement", error);
    char old[64]{}; DWORD count = 0;
    const BOOL read = ReadFile(reader, old, sizeof(old), &count, nullptr);
    CloseHandle(reader);
    CHECK(read && std::string(old, count) == before.content);
    if (saved) CHECK((share & FILE_SHARE_DELETE) && bytes(path) == "replacement");
    else CHECK((error.find("Windows error 32)") != std::string::npos || error.find("Windows error 5)") != std::string::npos) && before.same(path));
    CHECK(entries(root) == names);
  }
  const Snapshot before(path); const auto names = entries(root);
  CHECK(!replace(path, "partial write material", error, Fault::Cleanup));
  CHECK(before.same(path) && error.find("cannot remove temporary file") != std::string::npos);
  auto after = entries(root);
  CHECK(after.size() == names.size() + 1);
  for (const auto &name : after) if (!names.count(name)) {
    const auto temporary = root / fs::u8path(name);
    CHECK(error.find(temporary.u8string()) != std::string::npos && bytes(temporary) == "partial wri");
    const auto privateAcl = Snapshot(temporary).security;
    CHECK(privateAcl.substr(privateAcl.find(L"D:")) == L"D:P(A;;FA;;;" + sid() + L")");
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
  CHECK(replace(alias / L"file.json", "junction replacement", error));
  CHECK(persist_detail::readGraphFileText((alias / L"file.json").u8string(), text, error, Fault::None) == SidecarReadStatus::Loaded);
  CHECK(text == "junction replacement" && bytes(dir / L"file.json") == text);
  CHECK(!preferences_detail::configPaths("", alias.u8string(), error).preferences.empty());
  CHECK(RemoveDirectoryW(alias.c_str()));
  return true;
}
std::wstring systemAcl(const fs::path &path, bool audit) {
  PSECURITY_DESCRIPTOR sd = nullptr;
  const DWORD code = GetNamedSecurityInfoW(const_cast<wchar_t *>(path.c_str()), SE_FILE_OBJECT,
      audit ? SACL_SECURITY_INFORMATION : LABEL_SECURITY_INFORMATION, nullptr, nullptr, nullptr, nullptr, &sd);
  if (code) throw std::runtime_error("Cannot independently inspect system ACL");
  wchar_t *text = nullptr;
  const BOOL ok = ConvertSecurityDescriptorToStringSecurityDescriptorW(sd, SDDL_REVISION_1, SACL_SECURITY_INFORMATION, &text, nullptr);
  LocalFree(sd);
  if (!ok) throw std::runtime_error("Cannot encode system ACL");
  const std::wstring out(text); LocalFree(text); return out;
}
void systemSecurity(const fs::path &path, const std::wstring &sddl, bool audit = false) {
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr)) throw std::runtime_error("system fixture descriptor");
  PACL acl = nullptr; BOOL present, def;
  GetSecurityDescriptorSacl(descriptor, &present, &acl, &def);
  const auto code = SetNamedSecurityInfoW(const_cast<wchar_t *>(path.c_str()), SE_FILE_OBJECT,
      audit ? SACL_SECURITY_INFORMATION : LABEL_SECURITY_INFORMATION, nullptr, nullptr, nullptr, acl);
  LocalFree(descriptor);
  if (code) throw std::runtime_error("system fixture security: " + std::to_string(code));
}
DWORD lowRead(const fs::path &path) {
  HANDLE process, low;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_DUPLICATE, &process)) throw std::runtime_error("low token source");
  const BOOL copied = DuplicateTokenEx(process, TOKEN_QUERY | TOKEN_IMPERSONATE | TOKEN_ADJUST_DEFAULT, nullptr, SecurityImpersonation, TokenImpersonation, &low);
  CloseHandle(process);
  if (!copied) throw std::runtime_error("low token duplicate");
  PSID integrity = nullptr;
  ConvertStringSidToSidW(L"S-1-16-4096", &integrity);
  TOKEN_MANDATORY_LABEL label{}; label.Label.Sid = integrity; label.Label.Attributes = SE_GROUP_INTEGRITY;
  const BOOL lowered = SetTokenInformation(low, TokenIntegrityLevel, &label, sizeof(label) + GetLengthSid(integrity)); LocalFree(integrity);
  if (!lowered || !ImpersonateLoggedOnUser(low)) throw std::runtime_error("low token impersonation");
  HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
  const DWORD code = file == INVALID_HANDLE_VALUE ? GetLastError() : 0;
  if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
  const BOOL reverted = RevertToSelf(); CloseHandle(low);
  if (!reverted) throw std::runtime_error("restore test token");
  return code;
}
bool integrityAndEncryption(const fs::path &root) {
  std::string error;
  const auto path = root / L"confidential.json";
  write(path, "restricted original");
  systemSecurity(path, L"S:(ML;;NR;;;ME)");
  CHECK(lowRead(path) == ERROR_ACCESS_DENIED);
  const auto label = systemAcl(path, false);
  CHECK(replace(path, "still restricted", error));
  CHECK(systemAcl(path, false) == label && lowRead(path) == ERROR_ACCESS_DENIED);
  const Snapshot before(path); const auto names = entries(root);
  CHECK(!replace(path, "partial confidential contents", error, Fault::Cleanup) && before.same(path));
  for (const auto &name : entries(root)) if (!names.count(name)) {
    const auto temp = root / fs::u8path(name);
    CHECK(lowRead(temp) == ERROR_ACCESS_DENIED && systemAcl(temp, false) == label);
    CHECK(DeleteFileW(temp.c_str()));
  }
  const auto encrypted = root / L"encrypted.json";
  write(encrypted, "encrypted original");
  CHECK(EncryptFileW(encrypted.c_str()));
  const Snapshot efs(encrypted);
  CHECK(!replace(encrypted, "must not become plaintext", error) && efs.same(encrypted));
  const auto encryptedFolder = root / L"encrypted-folder"; fs::create_directory(encryptedFolder);
  CHECK(EncryptFileW(encryptedFolder.c_str()));
  const auto child = encryptedFolder / L"file.json";
  CHECK(replace(child, "inherited encryption", error));
  CHECK(replace(child, "still encrypted", error));
  CHECK(GetFileAttributesW(child.c_str()) & FILE_ATTRIBUTE_ENCRYPTED);
  return true;
}
PersistGrade grade() {
  PersistGrade grade;
  grade.color = {"rec2020", "linear", "rec709", "srgb"};
  grade.graph.primarySource = NodeId{"camera"};
  grade.graph.documentOutput = {NodeId{"camera"}, ports::image};
  PersistNode source; source.id = "camera"; source.operation = NodeOperation::Source;
  grade.graph.nodes.push_back(source);
  return grade;
}
bool photographs(const fs::path &root) {
  std::string error;
  CHECK(persist_detail::windowsUtf8Environment(error));
  const auto folder = root / fs::u8path(u8"caf\u00e9-\u4e2d-\U0001f98a"); fs::create_directory(folder);
  Image image; image.w = image.h = 1; image.px = {0.2f, 0.4f, 0.6f, 1.0f};
  for (const auto *name : {u8"caf\u00e9", u8"caf\u00c3\u00a9", u8"\u65e5\u672c", u8"\U0001f98a"}) {
    const auto png = folder / fs::u8path(std::string(name) + ".png");
    CHECK(writeImage(image, png.u8string()));
    const auto raw = folder / fs::u8path(std::string(name) + ".dng");
    CHECK(CopyFileW(fs::u8path(RAWNODE_TEST_DNG).c_str(), raw.c_str(), TRUE));
    CHECK(updateSidecarGrade(inputSidecarPath(png.u8string()), grade(), error));
    CHECK(updateSidecarMark(inputSidecarPath(png.u8string()), PhotoMark::Pick, error));
    CHECK(updateSidecarGrade(inputSidecarPath(raw.u8string()), grade(), error));
  }
  const auto discovered = listFolderImages(folder.u8string());
  CHECK(discovered.size() == 8);
  for (const auto &path : discovered) {
    Image decoded; ColorEncoding detected; bool raw = false;
    CHECK(loadImage(path, decoded, detected, raw) && !decoded.px.empty());
    CHECK(raw == (fs::u8path(path).extension() == L".dng"));
    PersistFile file;
    CHECK(readGraphSidecar(inputSidecarPath(path), file, error) == SidecarReadStatus::Loaded && file.grade);
    CHECK(updateSidecarMark(inputSidecarPath(path), PhotoMark::Reject, error));
  }
  const auto a = folder / fs::u8path(u8"caf\u00e9.png"), b = folder / fs::u8path(u8"caf\u00c3\u00a9.png");
  const Snapshot before(fs::u8path(inputSidecarPath(a.u8string())));
  const auto bSidecar = fs::u8path(inputSidecarPath(b.u8string()));
  const auto originalB = bytes(bSidecar);
  std::string selected;
  for (const auto &path : discovered) if (fs::equivalent(fs::u8path(path), b)) selected = path;
  CHECK(selected == b.u8string());
  CHECK(updateSidecarMark(inputSidecarPath(selected), PhotoMark::Pick, error));
  CHECK(before.same(fs::u8path(inputSidecarPath(a.u8string()))) && bytes(bSidecar) != originalB);
  PersistFile selectedFile;
  CHECK(readGraphSidecar(bSidecar.u8string(), selectedFile, error) == SidecarReadStatus::Loaded && photoMark(selectedFile) == PhotoMark::Pick);
  std::puts("ok  production discovery -> PNG/DNG decode -> sidecar load/save; Unicode directories, accents, CJK, supplementary and cafe collision identity");
  return true;
}
bool produceTransferredPhoto(const fs::path &root) {
  std::string error;
  fs::create_directories(root);
  const auto photo = root / L"other-account.png";
  Image image; image.w = image.h = 1; image.px = {0.2f, 0.4f, 0.6f, 1.0f};
  CHECK(writeImage(image, photo.u8string()));
  CHECK(updateSidecarGrade(inputSidecarPath(photo.u8string()), grade(), error));
  CHECK(updateSidecarMark(inputSidecarPath(photo.u8string()), PhotoMark::Pick, error));
  return true;
}
void privilege(const wchar_t *name, bool enabled) {
  HANDLE token; TOKEN_PRIVILEGES privileges{};
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) throw std::runtime_error("fixture privilege token");
  privileges.PrivilegeCount = 1;
  LookupPrivilegeValueW(nullptr, name, &privileges.Privileges[0].Luid);
  privileges.Privileges[0].Attributes = enabled ? SE_PRIVILEGE_ENABLED : 0;
  const BOOL ok = AdjustTokenPrivileges(token, FALSE, &privileges, 0, nullptr, nullptr);
  const auto code = GetLastError(); CloseHandle(token);
  if (!ok || code) throw std::runtime_error("fixture privilege: " + std::to_string(code));
}
void prepare(const fs::path &root, const std::wstring &tester, const std::wstring &other) {
  fs::create_directories(root);
  PersistFile portable; portable.grade = grade(); portable.metadataJson = R"({"mark":"pick"})";
  std::string json, error;
  if (!encodeGraphFile(portable, json, error)) throw std::runtime_error(error);
  privilege(L"SeRestorePrivilege", true); privilege(L"SeSecurityPrivilege", true);
  for (const auto &owner : {std::wstring(L"S-1-5-32-544"), other, std::wstring(L"S-1-5-21-111111111-222222222-333333333-4444")}) {
    const auto path = root / (L"owner-" + owner + L".json"); write(path, json);
    dacl(path, L"D:P(A;;0x1301bf;;;" + tester + L")(A;;FA;;;BA)(A;;FA;;;SY)");
    PSID sid = nullptr; ConvertStringSidToSidW(owner.c_str(), &sid);
    const DWORD code = SetNamedSecurityInfoW(const_cast<wchar_t *>(path.c_str()), SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION, sid, nullptr, nullptr, nullptr);
    LocalFree(sid); if (code) throw std::runtime_error("foreign owner fixture");
  }
  const auto folder = root / L"audit-inherited"; fs::create_directory(folder);
  systemSecurity(folder, L"S:(AU;OICISAFA;FW;;;WD)", true);
  const auto path = root / L"audit-explicit.json"; write(path, json);
  systemSecurity(path, L"S:(AU;SAFA;FW;;;WD)", true);
  write(root / L"view.rawnode.json", json);
  privilege(L"SeRestorePrivilege", false); privilege(L"SeSecurityPrivilege", false);
}
bool preparedFixtures(const fs::path &root) {
  std::string error;
  for (const auto &entry : fs::directory_iterator(root)) {
    if (entry.path().extension() != L".json") continue;
    const Snapshot before(entry.path());
    PersistFile imported;
    CHECK(readGraphSidecar(entry.path().u8string(), imported, error) == SidecarReadStatus::Loaded && imported.grade && photoMark(imported) == PhotoMark::Pick);
    CHECK(updateSidecarMark(entry.path().u8string(), PhotoMark::Reject, error));
    PersistFile saved;
    CHECK(readGraphSidecar(entry.path().u8string(), saved, error) == SidecarReadStatus::Loaded && saved.grade && photoMark(saved) == PhotoMark::Reject);
    const Snapshot after(entry.path());
    if (after.security != before.security)
      std::fwprintf(stderr, L"%ls\nbefore %ls\nafter  %ls\n", entry.path().c_str(), before.security.c_str(), after.security.c_str());
    CHECK(after.daclBytes == before.daclBytes && after.protectedDacl == before.protectedDacl);
    if (entry.path().filename().wstring().find(L"owner-") == 0) CHECK(before.owner != sid() && after.owner == sid());
  }
  const auto source = root / L"other-account.png", copied = root / L"copied.png";
  CHECK(CopyFileW(source.c_str(), copied.c_str(), TRUE));
  CHECK(CopyFileW(fs::u8path(inputSidecarPath(source.u8string())).c_str(), fs::u8path(inputSidecarPath(copied.u8string())).c_str(), TRUE));
  Image image; ColorEncoding encoding; bool raw = false;
  CHECK(loadImage(copied.u8string(), image, encoding, raw));
  PersistFile transferred;
  CHECK(readGraphSidecar(inputSidecarPath(copied.u8string()), transferred, error) == SidecarReadStatus::Loaded && transferred.grade && photoMark(transferred) == PhotoMark::Reject);
  CHECK(updateSidecarGrade(inputSidecarPath(copied.u8string()), *transferred.grade, error));
  const auto path = root / L"audit-inherited" / L"new.json";
  CHECK(replace(path, "first inherited-audit save", error));
  CHECK(replace(path, "second inherited-audit save", error));
  std::puts("ok  ordinary-user replacement of admin/other/stale owners and explicit/inherited auditing");
  return true;
}
bool verifyAuditFixtures(const fs::path &root) {
  std::string error;
  privilege(L"SeSecurityPrivilege", true);
  CHECK(systemAcl(root / L"audit-inherited" / L"new.json", true).find(L"AU;") != std::wstring::npos);
  CHECK(systemAcl(root / L"audit-explicit.json", true).find(L"AU;") == std::wstring::npos);
  privilege(L"SeSecurityPrivilege", false);
  std::puts("ok  independent privileged observation: inherited auditing retained; explicit per-file audit removed by ordinary replacement");
  return true;
}
bool volume(const fs::path &base, const fs::path &root) {
  std::string error;
  fs::create_directories(root);
  wchar_t filesystem[64]{}; DWORD flags = 0;
  CHECK(GetVolumeInformationW(root.root_path().c_str(), nullptr, 0, nullptr, nullptr, &flags, filesystem, 64));
  std::printf("Filesystem %ls, persistent ACLs %d, root %ls\n", filesystem, (flags & FILE_PERSISTENT_ACLS) != 0, root.c_str());
  const auto path = root / L"atomic.json";
  CHECK(replace(path, "original", error));
  for (int i = 0; i < 50; ++i) CHECK(replace(path, "complete replacement", error));
  const Snapshot before(path); const auto names = entries(root);
  for (auto fault : {Fault::TempCreate, Fault::Write, Fault::Flush, Fault::Close, Fault::Replace}) {
    CHECK(!replace(path, "never published", error, fault));
    CHECK(before.same(path) && entries(root) == names);
  }
  CHECK(SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_READONLY));
  const Snapshot readonly(path);
  CHECK(!replace(path, "never published", error) && readonly.same(path));
  CHECK(SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_ARCHIVE));
  const auto alias = base / (L"junction-" + std::wstring(filesystem));
  junction(alias, root);
  CHECK(replace(alias / L"atomic.json", "cross-volume junction save", error));
  CHECK(bytes(path) == "cross-volume junction save");
  CHECK(RemoveDirectoryW(alias.c_str()));
  CHECK(photographs(root));
  const auto preset = root / L"preset.rawnode.json";
  CHECK(updateSidecarGrade(preset.u8string(), grade(), error));
  CHECK(updateSidecarGrade(preset.u8string(), grade(), error));
  const auto config = preferences_detail::configPaths("", root.u8string(), error);
  Preferences preferences; preferences.jpegQuality = 73;
  CHECK(savePreferences(config.preferences, preferences, error));
  preferences.jpegQuality = 82;
  CHECK(savePreferences(config.preferences, preferences, error));
  Preferences loaded;
  CHECK(loadPreferences(config.preferences, loaded, error) && loaded.jpegQuality == 82);
  const Snapshot full(path); const auto clean = entries(root);
  CHECK(!replace(path, std::string(160 * 1024 * 1024, 'F'), error));
  std::printf("Genuine full-volume failure: %s\n", error.c_str());
  CHECK(error.find("112") != std::string::npos && full.same(path) && entries(root) == clean);
  std::puts("ok  production creation/replacement, independent failure snapshots, Unicode, resolved cross-volume junction and genuine disk-full preservation");
  return true;
}
}  // namespace

int wmain(int argc, wchar_t **argv) {
  try {
    std::string error;
    if (argc == 2 && std::wcscmp(argv[1], L"--check-codepage") == 0) {
      const bool coherent = persist_detail::windowsUtf8Environment(error);
      std::printf("Unmanifested ACP %u; startup check %s; %s\n", GetACP(), coherent ? "accepted" : "refused", error.c_str());
      if (GetACP() == CP_UTF8) return coherent ? 0 : 1; // System-wide UTF-8 is already coherent.
      return !coherent && error.find("UTF-8") != std::string::npos ? 0 : 1;
    }
    if (!persist_detail::windowsUtf8Environment(error)) throw std::runtime_error(error);
    if (argc == 3 && std::wcscmp(argv[1], L"--produce-photo") == 0) return produceTransferredPhoto(argv[2]) ? 0 : 1;
    if (argc == 5 && std::wcscmp(argv[1], L"--prepare") == 0) { prepare(argv[2], argv[3], argv[4]); return 0; }
    if (argc == 3 && std::wcscmp(argv[1], L"--verify-audits") == 0) return verifyAuditFixtures(argv[2]) ? 0 : 1;
    if (argc >= 3) { _wputenv_s(L"TEMP", argv[2]); _wputenv_s(L"TMP", argv[2]); }
    if (argc == 3 && std::wcscmp(argv[1], L"--readonly-location") == 0) {
      const auto path = fs::path(argv[2]) / L"view.rawnode.json";
      PersistFile file;
      CHECK(readGraphSidecar(path.u8string(), file, error) == SidecarReadStatus::Loaded && file.grade);
      const Snapshot before(path);
      CHECK(!persist_detail::canSaveFile(path.u8string(), error));
      CHECK(!replace(path, "unsupported write", error) && before.same(path));
      std::puts("ok  actual unsupported network location: saved grade readable; writes refused and bytes/security/identity preserved");
      return 0;
    }
    Directory dir;
    if (argc == 4 && std::wcscmp(argv[1], L"--volume") == 0) return volume(dir.path, argv[3]) ? 0 : 1;
    if (argc == 4 && std::wcscmp(argv[1], L"--ordinary-user") == 0 && !preparedFixtures(argv[3])) return 1;
    if (!unicodeAndCallers(dir.path) || !testSidecarFiles() || !permissionsAndRefusal(dir.path) || !readersAndCleanup(dir.path) ||
        !links(dir.path) || !integrityAndEncryption(dir.path) || !photographs(dir.path)) return 1;
    std::puts("ok  MSVC actual production persistence: sidecars/presets/preferences, ACLs/integrity/EFS, readers, failure preservation, cleanup and Unicode photograph identity");
    return 0;
  } catch (const std::exception &failure) { std::fprintf(stderr, "%s (Windows error %lu)\n", failure.what(), GetLastError()); return 2; }
}
