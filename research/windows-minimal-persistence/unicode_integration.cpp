// Research-only: actual RawNode photo discovery -> sidecar read -> mark save on
// Windows, linked against the production persistence sources at 5e5aa22.
// Built twice: with the process ANSI code page, and with a UTF-8
// activeCodePage manifest. Image decoders open narrow paths (LibRaw
// open_file(char*), TIFFOpen, stbi_load, fopen), so std::fopen stands in for them.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "persist/ProjectPersist.h"

#include <cstdio>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace {
std::string utf8(const std::wstring &w) {
  const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
  std::string s(size_t(n > 0 ? n : 0), '\0');
  if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
  return s;
}
std::string esc(const std::string &s) {
  std::string o;
  for (unsigned char c : s) {
    if (c == '"' || c == '\\') { o += '\\'; o += char(c); }
    else if (c < 0x20 || c >= 0x7f) { char b[8]; std::snprintf(b, sizeof b, "\\x%02x", c); o += b; }
    else o += char(c);
  }
  return o;
}
void writeWide(const std::wstring &path, const std::string &bytes) {
  HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  DWORD n = 0;
  WriteFile(h, bytes.data(), DWORD(bytes.size()), &n, nullptr);
  CloseHandle(h);
}
std::string readWide(const std::wstring &path) {
  HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
  if (h == INVALID_HANDLE_VALUE) return "<missing>";
  char buffer[4096];
  DWORD n = 0;
  ReadFile(h, buffer, sizeof buffer, &n, nullptr);
  CloseHandle(h);
  return std::string(buffer, n);
}
std::set<std::wstring> namesIn(const std::wstring &dir) {
  std::set<std::wstring> out;
  WIN32_FIND_DATAW data;
  HANDLE find = FindFirstFileW((dir + L"\\*").c_str(), &data);
  if (find == INVALID_HANDLE_VALUE) return out;
  do { out.insert(data.cFileName); } while (FindNextFileW(find, &data));
  FindClose(find);
  return out;
}
const char *statusName(SidecarReadStatus s) {
  return s == SidecarReadStatus::Loaded ? "loaded" : s == SidecarReadStatus::Absent ? "absent" : "error";
}

// One photograph path as the application would hold it, through the steps
// that touch the filesystem: extension filter, image open, sidecar read, mark save.
void followPhoto(const char *flow, const std::string &photo, const std::wstring &groundTruthSidecar,
                 const std::wstring &dir) {
  const auto namesBefore = namesIn(dir);
  std::string supported, imageOpen, preRead, read, mark, markError;
  try { supported = isSupportedImagePath(photo) ? "yes" : "no"; } catch (const std::exception &e) { supported = std::string("throws: ") + e.what(); }
  if (std::FILE *f = std::fopen(photo.c_str(), "rb")) { imageOpen = "ok"; std::fclose(f); } else imageOpen = "fails";
  try {
    // Pre-PR readGraphFileText converted with std::filesystem::path(std::string).
    std::FILE *f = _wfopen(fs::path(inputSidecarPath(photo)).c_str(), L"rb");
    preRead = f ? "ok" : "fails";
    if (f) std::fclose(f);
  } catch (const std::exception &e) { preRead = std::string("throws: ") + e.what(); }
  PersistFile file;
  std::string error;
  const auto status = readGraphSidecar(inputSidecarPath(photo), file, error);
  read = statusName(status);
  try { if (status == SidecarReadStatus::Loaded) read += photoMark(file) == PhotoMark::Pick ? "/pick" : "/other"; } catch (...) { read += "/mark-throws"; }
  const std::string readError = error;
  mark = updateSidecarMark(inputSidecarPath(photo), PhotoMark::Reject, markError) ? "ok" : "fails";
  const bool rejected = readWide(groundTruthSidecar).find("reject") != std::string::npos;
  const bool sameNames = namesIn(dir) == namesBefore;
  std::printf("{\"flow\":\"%s\",\"pathBytes\":\"%s\",\"supported\":\"%s\",\"imageOpenNarrow\":\"%s\",\"preRead\":\"%s\","
              "\"read\":\"%s\",\"readError\":\"%s\",\"markSave\":\"%s\",\"markError\":\"%s\",\"realSidecarRejected\":%s,"
              "\"noStrayFiles\":%s}\n",
              flow, esc(photo).c_str(), esc(supported).c_str(), imageOpen.c_str(), esc(preRead).c_str(), read.c_str(),
              esc(readError).c_str(), mark.c_str(), esc(markError).c_str(), rejected ? "true" : "false",
              sameNames ? "true" : "false");
  writeWide(groundTruthSidecar, "{\"format\":\"rawnode\",\"version\":1,\"metadata\":{\"mark\":\"pick\"}}\n");
}
}  // namespace

int wmain(int argc, wchar_t **argv) {
  if (argc < 2) return 2;
  const std::wstring root = argv[1];
  CreateDirectoryW(root.c_str(), nullptr);
  std::printf("{\"acp\":%u,\"oemcp\":%u}\n", GetACP(), GetOEMCP());
  const std::vector<std::wstring> stems = {L"plain", L"café", L"日本語", L"\U0001F98A-fox"};
  // "ansi-only" holds names representable in code page 1252, isolating the
  // pre-PR comparison from CJK/emoji names that break listing outright.
  const std::vector<std::wstring> folders = {root + L"\\ansi-only", root + L"\\ascii", root + L"\\Été 写真"};
  for (const auto &dir : folders) {
    CreateDirectoryW(dir.c_str(), nullptr);
    for (const auto &stem : stems) {
      if (dir == folders[0] && stem != L"plain" && stem != L"café") continue;
      writeWide(dir + L"\\" + stem + L".nef", "raw");
      writeWide(dir + L"\\" + stem + L".nef.rawnode.json", "{\"format\":\"rawnode\",\"version\":1,\"metadata\":{\"mark\":\"pick\"}}\n");
    }
  }
  for (const auto &dir : folders) {
    // Folder workflow: the dialog returns UTF-8; openFolder canonicalises with
    // path::string(); the filmstrip lists with listFolderImages().
    const std::string dialogFolder = utf8(dir);
    std::string canonical, listingError;
    std::vector<std::string> listed;
    bool isDirectory = false;
    try {
      std::error_code ec;
      isDirectory = fs::is_directory(dialogFolder, ec);
      canonical = fs::weakly_canonical(fs::path(dialogFolder), ec).string();
      listed = listFolderImages(canonical);
    } catch (const std::exception &e) { listingError = e.what(); }
    std::printf("{\"flow\":\"open-folder\",\"folder\":\"%s\",\"isDirectory\":%s,\"canonicalBytes\":\"%s\",\"listed\":%zu,"
                "\"listingError\":\"%s\"}\n",
                esc(dialogFolder).c_str(), isDirectory ? "true" : "false", esc(canonical).c_str(), listed.size(),
                esc(listingError).c_str());
    for (const auto &photo : listed) {
      // Map the listed narrow string back to the fixture it names.
      std::wstring truth;
      for (const auto &stem : stems) {
        try {
          if (fs::path(dir + L"\\" + stem + L".nef").string() == photo) truth = dir + L"\\" + stem + L".nef.rawnode.json";
        } catch (const std::exception &) {}
      }
      followPhoto("folder-listing", photo, truth.empty() ? dir + L"\\unknown" : truth, dir);
    }
    // Scoped conversion alternative: list with u8string() instead of string().
    std::error_code ec;
    for (const auto &entry : fs::directory_iterator(dir, ec)) {
      if (entry.path().extension() != L".nef") continue;
      followPhoto("scoped-u8-listing", entry.path().u8string(), entry.path().wstring() + L".rawnode.json", dir);
    }
    // Single photo from the open dialog or a drop: GLFW and pfd supply UTF-8.
    for (const auto &stem : stems)
      if (GetFileAttributesW((dir + L"\\" + stem + L".nef").c_str()) != INVALID_FILE_ATTRIBUTES)
        followPhoto("dialog-or-drop", utf8(dir + L"\\" + stem + L".nef"), dir + L"\\" + stem + L".nef.rawnode.json", dir);
  }
  return 0;
}
