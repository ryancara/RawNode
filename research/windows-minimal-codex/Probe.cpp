// Comparative native experiments, not a production persistence implementation.
#include "Observe.h"
#include "MinimalWriter.h"
#include "persist/WindowsFile.h"
#include "persist/ProjectPersistPriv.h"
#include "persist/PreferencesPriv.h"
#include "imgio/ImageIO.h"
#include "imgio/ImageIOPriv.h"
#include <functional>
#include <map>
#include <set>
#include <clocale>
#include <mbctype.h>
#include <libraw/libraw.h>
#include <memory>

const std::vector<std::string> modes{"bare", "current", "dacl", "minimal", "inherited"};
const DWORD shareAll = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
const std::string jsonOld = R"({"format":"rawnode","version":1,"metadata":{"mark":"pick"},"grade":{"color":{"rawColorSpace":"rec2020","rawGamma":"linear","outputColorSpace":"rec709","outputGamma":"srgb"},"graph":{"primarySource":"camera","documentOutput":{"node":"camera","port":"image"},"nodes":[{"id":"camera","operation":"source","params":{}}]}}})";
void info(const std::string &key, const std::string &value) {
  std::cout << "{\"type\":\"info\",\"case\":" << quote(current) << ",\"key\":" << quote(key) << ",\"value\":" << quote(value) << "}\n";
}
bool privilege(const wchar_t *name, bool enable) {
  Handle token;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_ADJUST_PRIVILEGES, &token.h)) return false;
  TOKEN_PRIVILEGES p{}; p.PrivilegeCount = 1;
  if (!LookupPrivilegeValueW(nullptr, name, &p.Privileges[0].Luid)) return false;
  p.Privileges[0].Attributes = enable ? SE_PRIVILEGE_ENABLED : 0;
  return AdjustTokenPrivileges(token.h, FALSE, &p, 0, nullptr, nullptr) && GetLastError() == ERROR_SUCCESS;
}
std::string selectedSacl(const fs::path &path, bool full = false) {
  PSECURITY_DESCRIPTOR sd = nullptr;
  const DWORD fields = LABEL_SECURITY_INFORMATION | ATTRIBUTE_SECURITY_INFORMATION | SCOPE_SECURITY_INFORMATION | PROCESS_TRUST_LABEL_SECURITY_INFORMATION | ACCESS_FILTER_SECURITY_INFORMATION | (full ? SACL_SECURITY_INFORMATION : 0);
  DWORD code = GetNamedSecurityInfoW(const_cast<wchar_t *>(path.c_str()), SE_FILE_OBJECT, fields, nullptr, nullptr, nullptr, nullptr, &sd);
  if (code) return "error:" + std::to_string(code);
  const std::string text = securityString(sd, SACL_SECURITY_INFORMATION | fields); LocalFree(sd); return text;
}
std::map<std::string, std::string> streams(const fs::path &path) {
  std::map<std::string, std::string> out; WIN32_FIND_STREAM_DATA data{};
  HANDLE search = FindFirstStreamW(path.c_str(), FindStreamInfoStandard, &data, 0);
  if (search == INVALID_HANDLE_VALUE) { out["enumeration-error"] = std::to_string(GetLastError()); return out; }
  do {
    if (wcscmp(data.cStreamName, L"::$DATA")) {
      std::ifstream in(fs::path(path.wstring() + data.cStreamName), std::ios::binary);
      const std::string content{std::istreambuf_iterator<char>(in), {}};
      out[utf8(data.cStreamName)] = hex(content);
    }
  } while (FindNextStreamW(search, &data));
  FindClose(search); return out;
}
void logStreams(const char *phase, const fs::path &path) {
  for (const auto &[name, content] : streams(path)) info(std::string(phase) + " stream " + name, content);
}
std::set<std::string> names(const fs::path &dir) {
  std::set<std::string> out;
  for (const auto &p : fs::directory_iterator(dir)) out.insert(p.path().filename().u8string());
  return out;
}
void setSystem(const fs::path &path, const std::wstring &sddl, bool label, bool protect = false) {
  PSECURITY_DESCRIPTOR sd = nullptr;
  require(ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &sd, nullptr) != FALSE, "system SDDL");
  PACL acl = nullptr; BOOL p, d; require(GetSecurityDescriptorSacl(sd, &p, &acl, &d) != FALSE, "system ACL");
  const DWORD code = SetNamedSecurityInfoW(const_cast<wchar_t *>(path.c_str()), SE_FILE_OBJECT,
    (label ? LABEL_SECURITY_INFORMATION : SACL_SECURITY_INFORMATION) | (protect ? PROTECTED_SACL_SECURITY_INFORMATION : 0), nullptr, nullptr, nullptr, acl);
  LocalFree(sd);
  info("set-system-error", std::to_string(code)); require(code == 0, "set system ACL");
}
bool runWriter(const fs::path &path, const std::string &mode, std::string &error, experiment::Fault fault = experiment::Fault::None, const std::string &body = NEW) {
  if (mode == "current") return persist_detail::replaceFileAtomically(path.u8string(), body, error);
  if (mode == "bare") {
    const auto temp = path.parent_path() / L"bare-sibling";
    make(temp, body); snapshot("before", "bare temporary", temp, inspect(temp));
    const bool present = GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
    const auto r = result("MoveFileExW bare", MoveFileExW(temp.c_str(), path.c_str(), present ? MOVEFILE_REPLACE_EXISTING : 0));
    if (!r.ok) { error = "MoveFileExW error " + std::to_string(r.error); result("Delete bare temporary", DeleteFileW(temp.c_str())); }
    return r.ok;
  }
  experiment::Policy policy;
  policy.copyLabel = mode != "dacl";
  policy.privateCreation = mode != "inherited";
  return experiment::save(path.u8string(), body, error, policy, fault);
}
struct Case {
  fs::path root, dir, path, backup;
  std::string mode;
  State before, backupBefore;
  std::map<std::string, std::string> streamsBefore;
  std::string saclBefore;
  bool canAudit = false;
  Case(const fs::path &base, const std::string &name, const std::string &mode, bool present = true)
    : root(base), dir(base / wide(name) / wide(mode)), path(dir / L"photo.rawnode.json"),
      backup(base / L"backups" / wide(name + "-" + mode + ".bin")), mode(mode) {
    current = name + "/" + mode; fs::create_directories(dir); fs::create_directories(backup.parent_path());
    if (present && !fs::exists(path)) make(path, OLD);
  }
  void capture() {
    before = inspect(path); snapshot("before", "destination", path, before);
    if (before.exists && !before.readError) { make(backup, before.bytes); backupBefore = inspect(backup); snapshot("before", "backup", backup, backupBefore); }
    streamsBefore = streams(path); logStreams("before", path);
    canAudit = privilege(L"SeSecurityPrivilege", true);
    saclBefore = selectedSacl(path, canAudit); info("before SACL", saclBefore);
    if (canAudit) require(privilege(L"SeSecurityPrivilege", false), "disable fixture audit privilege");
  }
  bool run(experiment::Fault fault = experiment::Fault::None, const std::string &body = NEW) {
    capture(); std::string error; const bool ok = runWriter(path, mode, error, fault, body);
    info("writer return", ok ? "true" : "false"); info("writer error", error);
    const State after = inspect(path); snapshot("after", "destination", path, after); logStreams("after", path);
    if (canAudit) require(privilege(L"SeSecurityPrivilege", true), "enable observer audit privilege");
    const auto saclAfter = selectedSacl(path, canAudit); info("after SACL", saclAfter);
    if (canAudit) require(privilege(L"SeSecurityPrivilege", false), "disable observer audit privilege");
    if (backupBefore.exists) check(unchanged(backupBefore, inspect(backup)), "independent backup unchanged");
    if (ok) check(after.exists && !after.readError && after.bytes == body, "publication exposes complete NEW bytes");
    else {
      check(before.exists ? unchanged(before, after) : !after.exists, "failed writer preserved destination independently");
      check(streamsBefore == streams(path) && saclBefore == saclAfter, "failure preserved streams and observed SACL");
    }
    if (fault != experiment::Fault::Cleanup) check(names(dir) == (after.exists ? std::set<std::string>{"photo.rawnode.json"} : std::set<std::string>{}), "no owned temporary remains");
    return ok;
  }
};
void prepare(const fs::path &root, const std::wstring &tester, const std::wstring &other) {
  fs::create_directories(root);
  require(privilege(L"SeRestorePrivilege", true), "fixture restore privilege");
  require(privilege(L"SeSecurityPrivilege", true), "fixture security privilege");
  const auto dacl = L"D:P(A;;FA;;;" + tester + L")(A;;FA;;;BA)(A;;FA;;;SY)";
  for (const auto &name : {"admin_owner", "foreign_owner", "foreign_modify", "stale_owner", "foreign_group", "audit_explicit", "audit_inherited", "label_confidential", "label_low", "label_inherited", "label_high", "audit_initial_then_save"}) {
    for (const auto &mode : modes) {
      Case c(root, name, mode, false);
      setDacl(c.dir, L"D:PAI(A;OICI;FA;;;" + tester + L")(A;OICI;FA;;;BA)(A;OICI;FA;;;SY)");
      if (std::string(name) == "audit_inherited" || std::string(name) == "audit_initial_then_save") setSystem(c.dir, L"S:(AU;OICISAFA;FW;;;WD)", false);
      if (std::string(name) == "label_inherited") setSystem(c.dir, L"S:(ML;OICI;NW;;;LW)", true);
      if (std::string(name) == "audit_initial_then_save") continue;
      if (!fs::exists(c.path)) make(c.path, OLD); setDacl(c.path, dacl);
      if (std::string(name).find("owner") != std::string::npos || std::string(name) == "foreign_modify" || std::string(name) == "foreign_group") {
        const std::wstring owner = std::string(name) == "admin_owner" ? L"S-1-5-32-544" : std::string(name) == "stale_owner" ? L"S-1-5-21-111111111-222222222-333333333-4444" : other;
        PSID sid = nullptr; require(ConvertStringSidToSidW(owner.c_str(), &sid) != FALSE, "foreign SID");
        const bool group = std::string(name) == "foreign_group";
        const DWORD code = SetNamedSecurityInfoW(const_cast<wchar_t *>(c.path.c_str()), SE_FILE_OBJECT,
          group ? GROUP_SECURITY_INFORMATION : OWNER_SECURITY_INFORMATION, group ? nullptr : sid, group ? sid : nullptr, nullptr, nullptr);
        LocalFree(sid); info("set foreign owner/group error", std::to_string(code)); require(code == 0, "foreign owner/group fixture");
      }
      if (std::string(name) == "foreign_modify") setDacl(c.path,L"D:P(A;;0x1301bf;;;"+tester+L")(A;;FA;;;BA)(A;;FA;;;SY)");
      if (std::string(name) == "audit_explicit") setSystem(c.path, L"S:P(AU;SAFA;FW;;;WD)", false, true);
      if (std::string(name) == "label_confidential") setSystem(c.path, L"S:(ML;;NR;;;ME)", true);
      if (std::string(name) == "label_low") setSystem(c.path, L"S:(ML;;NW;;;LW)", true);
      if (std::string(name) == "label_high") setSystem(c.path, L"S:(ML;;NW;;;HI)", true);
    }
  }
  privilege(L"SeRestorePrivilege", false); privilege(L"SeSecurityPrivilege", false);
}
DWORD lowAccess(const fs::path &path, DWORD access) {
  Handle process, low;
  require(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_DUPLICATE, &process.h) != FALSE, "low token source");
  require(DuplicateTokenEx(process.h, TOKEN_QUERY | TOKEN_IMPERSONATE | TOKEN_ADJUST_DEFAULT, nullptr, SecurityImpersonation, TokenImpersonation, &low.h) != FALSE, "low token duplicate");
  PSID sid = nullptr; require(ConvertStringSidToSidW(L"S-1-16-4096", &sid) != FALSE, "low SID");
  TOKEN_MANDATORY_LABEL label{}; label.Label.Sid = sid; label.Label.Attributes = SE_GROUP_INTEGRITY;
  const BOOL lowered = SetTokenInformation(low.h, TokenIntegrityLevel, &label, sizeof(label) + GetLengthSid(sid)); LocalFree(sid);
  require(lowered != FALSE, "lower fixture token");
  require(ImpersonateLoggedOnUser(low.h) != FALSE, "impersonate low fixture");
  HANDLE h = CreateFileW(path.c_str(), access, shareAll, nullptr, OPEN_EXISTING, 0, nullptr);
  const DWORD error = h == INVALID_HANDLE_VALUE ? GetLastError() : 0;
  if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
  require(RevertToSelf() != FALSE, "restore fixture token"); return error;
}
DWORD openAccess(const fs::path &path,DWORD access){HANDLE h=CreateFileW(path.c_str(),access,shareAll,nullptr,OPEN_EXISTING,0,nullptr);const DWORD code=h==INVALID_HANDLE_VALUE?GetLastError():0;if(h!=INVALID_HANDLE_VALUE)CloseHandle(h);return code;}
void securityCases(const fs::path &root) {
  for (const auto &name : {"admin_owner", "foreign_owner", "foreign_modify", "stale_owner", "foreign_group", "audit_explicit", "audit_inherited", "label_confidential", "label_low", "label_inherited", "label_high"})
    for (const auto &mode : modes) {
      Case c(root, name, mode);
      const bool label = std::string(name) == "label_confidential";
      if (label) info("low read before", std::to_string(lowAccess(c.path, GENERIC_READ)));
      if(std::string(name)=="foreign_modify")info("WRITE_DAC before",std::to_string(openAccess(c.path,WRITE_DAC)));
      const bool ok = c.run();
      if(std::string(name)=="foreign_modify")info("WRITE_DAC after",std::to_string(openAccess(c.path,WRITE_DAC)));
      if (label) {
        const DWORD access = lowAccess(c.path, GENERIC_READ); info("low read after", std::to_string(access));
        if (ok && (mode == "minimal" || mode == "inherited")) check(access == ERROR_ACCESS_DENIED, "copy label retains low-integrity read denial");
      }
      if (ok && mode != "bare") check(inspect(c.path).dacl == c.before.dacl, "DACL/protection retained without owner/group matching");
    }
  for (const auto &mode : modes) {
    Case c(root, "audit_initial_then_save", mode, false);
    // Inherited SACL is installed by the privileged preparation step.
    c.run();
    info("second-save-begins", mode);
    std::string error; const bool ok = runWriter(c.path, mode, error); info("second-save-result", ok ? "true" : "false"); info("second-save-error", error);
  }
}
void ordinary(const fs::path &root) {
  for (const auto &mode : modes) { Case c(root, "create", mode, false); check(c.run(), "new file creation"); }
  for (const auto &mode : modes) { Case c(root, "replace", mode); check(c.run(), "existing ordinary replacement"); }
  for (const auto &mode : modes) for (DWORD attributes : {DWORD(FILE_ATTRIBUTE_NORMAL), DWORD(FILE_ATTRIBUTE_HIDDEN), DWORD(FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_ARCHIVE),
      DWORD(FILE_ATTRIBUTE_NOT_CONTENT_INDEXED | FILE_ATTRIBUTE_ARCHIVE), DWORD(FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_ARCHIVE), DWORD(FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_ARCHIVE)}) {
    Case c(root, "attr_" + std::to_string(attributes), mode);
    require(SetFileAttributesW(c.path.c_str(), attributes) != FALSE, "fixture attributes"); const bool saved=c.run();
    if(attributes & FILE_ATTRIBUTE_READONLY)check(!saved,"read-only destination refused without clearing");
    // Fixture cleanup only; the writer must not clear READONLY.
    if (attributes & FILE_ATTRIBUTE_READONLY) SetFileAttributesW(c.path.c_str(), FILE_ATTRIBUTE_ARCHIVE);
  }
  for (const auto &mode : modes) {
    Case c(root, "hardlink", mode); const auto alias = c.dir / L"retained-link";
    require(CreateHardLinkW(alias.c_str(), c.path.c_str(), nullptr) != FALSE, "hardlink fixture");
    c.capture(); std::string error; const bool ok = runWriter(c.path, mode, error);
    info("writer return", ok ? "true" : "false"); info("writer error", error); snapshot("after", "destination", c.path, inspect(c.path)); snapshot("after", "other link", alias, inspect(alias));
    check(inspect(alias).bytes == OLD && inspect(alias).id == c.before.id, "other hardlink keeps original file");
    if (!ok) check(unchanged(c.before, inspect(c.path)), "hardlink refusal preserves original"); else check(inspect(c.path).bytes == NEW, "replaced hardlink pathname has NEW bytes");
  }
  for (const auto &mode : modes) for (const auto &stream : {L":Zone.Identifier", L":com.dropbox.attrs", L":user-data"}) {
    const std::string name = "ads_" + utf8(std::wstring(stream).substr(1));
    Case c(root, name, mode); make(fs::path(c.path.wstring() + stream), stream == std::wstring(L":Zone.Identifier") ? "[ZoneTransfer]\r\nZoneId=3\r\n" : "opaque additional bytes"); c.run();
  }
  for (bool protect : {false, true}) for (const auto &mode : modes) {
    Case c(root, protect ? "dacl_protected" : "dacl_inherited", mode);
    setDacl(c.path, L"D:(A;;FA;;;" + userSid() + L")(A;;FR;;;BU)(A;;FA;;;SY)", protect); c.run();
    if (mode == "minimal" || mode == "inherited") check(inspect(c.path).dacl == c.before.dacl, "DACL exact including inheritance");
  }
  for (const auto &mode : modes) {
    Case c(root, "write_denied", mode); setDacl(c.path, L"D:P(D;;0x2;;;" + userSid() + L")(A;;FA;;;" + userSid() + L")(A;;FA;;;SY)"); const bool ok = c.run();
    check(mode == "bare" ? ok : !ok, "explicit write preflight prevents rename bypass");
  }
  for (const auto &mode : modes) {
    Case c(root, "callback_dacl", mode);
    const std::wstring sddl = L"D:P(A;;FA;;;" + userSid() + L")(XA;;FR;;;BU;(@User.Title == \"Manager\"))";
    setDacl(c.path, sddl); c.run();
    if (mode == "minimal" || mode == "inherited") check(inspect(c.path).dacl == c.before.dacl, "opaque conditional ACE retained");
  }
}

void newPermissions(const fs::path &root) {
  const auto base=root/L"new_permissions";fs::create_directories(base);
  const auto permissions=L"D:PAI(A;OICI;FA;;;"+userSid()+L")(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;FR;;;BU)";
  for(const auto &mode:{std::string("minimal"),std::string("inherited")}){
    Case c(base,"new",mode,false);setDacl(c.dir,permissions);check(c.run(),"shared-folder new creation");
  }
  current="new_permissions/staged";const auto dir=base/L"staged";fs::create_directory(dir);setDacl(dir,permissions);
  const auto exemplar=dir/L"empty-inherited-exemplar",temporary=dir/L"private-completed-sibling",path=dir/L"photo.rawnode.json";
  make(exemplar,"");const auto inherited=inspect(exemplar);std::string error;
  require(persist_detail::replaceFileAtomically(temporary.u8string(),NEW,error),"private completed sibling");snapshot("private","staged sibling",temporary,inspect(temporary));
  PSECURITY_DESCRIPTOR sd=nullptr;PACL dacl=nullptr;
  Handle h(CreateFileW(exemplar.c_str(),READ_CONTROL,shareAll,nullptr,OPEN_EXISTING,0,nullptr));
  require(GetSecurityInfo(h.h,SE_FILE_OBJECT,DACL_SECURITY_INFORMATION,nullptr,nullptr,&dacl,nullptr,&sd)==0,"inherited exemplar DACL");
  const DWORD copied=SetNamedSecurityInfoW(const_cast<wchar_t*>(temporary.c_str()),SE_FILE_OBJECT,DACL_SECURITY_INFORMATION|UNPROTECTED_DACL_SECURITY_INFORMATION,nullptr,nullptr,dacl,nullptr);LocalFree(sd);
  require(!copied,"stage final inherited DACL");require(closeChecked(h),"close exemplar inspection");
  require(DeleteFileW(exemplar.c_str())!=FALSE,"remove empty exemplar");require(MoveFileExW(temporary.c_str(),path.c_str(),0)!=FALSE,"publish staged new file");
  snapshot("after","staged publication",path,inspect(path));check(inspect(path).dacl==inherited.dacl&&inspect(path).bytes==NEW,"private-to-inherited final DACL matches native inheritance");
}
void readCreated(const fs::path &root){
  for(const auto &mode:{L"minimal",L"inherited",L"staged"}){
    current="other_account_new_permissions/"+utf8(mode);
    const auto path=std::wstring(mode)==L"staged"?root/L"new_permissions"/mode/L"photo.rawnode.json":root/L"new_permissions"/L"new"/mode/L"photo.rawnode.json";
    HANDLE h=CreateFileW(path.c_str(),GENERIC_READ,shareAll,nullptr,OPEN_EXISTING,0,nullptr);const DWORD error=h==INVALID_HANDLE_VALUE?GetLastError():0;
    info("different-account read error",std::to_string(error));if(h!=INVALID_HANDLE_VALUE)CloseHandle(h);
    check(std::wstring(mode)==L"minimal"?error==ERROR_ACCESS_DENIED:error==0,"private versus inherited shared-folder access");
  }
}

void failuresCases(const fs::path &root) {
  for (DWORD sharing : {DWORD(FILE_SHARE_READ | FILE_SHARE_WRITE), DWORD(shareAll)}) for (const auto &mode : modes) {
    Case c(root, "reader_" + std::to_string(sharing), mode);
    Handle reader(CreateFileW(c.path.c_str(), GENERIC_READ, sharing, nullptr, OPEN_EXISTING, 0, nullptr)); require(reader.h != INVALID_HANDLE_VALUE, "reader fixture"); check(!c.run(), "native sharing failure refused");
  }
  for (auto fault : {experiment::Fault::Write, experiment::Fault::Flush, experiment::Fault::Close, experiment::Fault::Rename, experiment::Fault::Cleanup}) {
    Case c(root, "fault_" + std::to_string(int(fault)), "minimal"); check(!c.run(fault), "fault abort refused");
    if (fault == experiment::Fault::Cleanup) {
      for (const auto &p : fs::directory_iterator(c.dir)) if (p.path().filename().wstring().find(L".rawnode-research-") == 0) {
        snapshot("after", "owned partial temporary", p.path(), inspect(p.path()));
        check(inspect(p.path()).bytes == NEW.substr(0, NEW.size()/2), "cleanup failure leaves only owned partial temporary"); result("manual fixture cleanup", DeleteFileW(p.path().c_str()));
      }
    }
  }
  for(const auto &mode:modes){Case c(root,"directory_destination",mode,false);fs::create_directory(c.path);check(!c.run(),"directory destination refused");}
  for(const auto &mode:{std::string("current"),std::string("minimal")}){current="missing_parent/"+mode;const auto before=names(root);std::string error;check(!runWriter(root/(L"missing-"+wide(mode))/L"file.json",mode,error),"missing parent refused");info("writer error",error);check(names(root)==before,"missing parent creates no files");}
  current = "real_source_cleanup_conflict"; const auto dir = root / L"source-conflict"; fs::create_directory(dir);
  const auto destination = dir / L"old", source = dir / L"temp"; make(destination, OLD); make(source, NEW); const auto before = inspect(destination);
  { Handle held(CreateFileW(source.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr));
    check(!result("MoveFileExW locked source", MoveFileExW(source.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING)).ok, "real source conflict");
    check(!result("DeleteFileW locked source", DeleteFileW(source.c_str())).ok, "real cleanup failure"); check(unchanged(before, inspect(destination)), "real conflict preserves destination"); }
  result("cleanup after reader close", DeleteFileW(source.c_str()));
}
void junction(const fs::path &link, const fs::path &target) {
  fs::create_directory(link);
  struct Data { DWORD tag; WORD size, reserved, so, sl, po, pl; wchar_t names[2048]; } data{};
  data.tag = IO_REPARSE_TAG_MOUNT_POINT;
  const auto substitute = L"\\??\\" + target.wstring(), print = target.wstring();
  data.sl = WORD(substitute.size()*2); data.po = WORD((substitute.size()+1)*2); data.pl = WORD(print.size()*2);
  memcpy(data.names, substitute.c_str(), data.sl+2); memcpy(reinterpret_cast<char*>(data.names)+data.po, print.c_str(), data.pl+2);
  data.size = WORD(8+data.po+data.pl+2);
  Handle h(CreateFileW(link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_BACKUP_SEMANTICS, nullptr));
  DWORD n; require(DeviceIoControl(h.h, FSCTL_SET_REPARSE_POINT, &data, 8+data.size, nullptr, 0, &n, nullptr) != FALSE, "junction fixture");
}
void pathCases(const fs::path &root) {
  for (const auto &mode : modes) {
    Case c(root, "junction_ntfs", mode); const auto real = c.dir / L"target", alias = c.dir / L"alias"; fs::create_directory(real); junction(alias, real);
    const auto p = alias / L"photo.rawnode.json"; make(real / L"photo.rawnode.json", OLD); std::string error;
    const bool ok = runWriter(p, mode, error); info("writer return", ok ? "true" : "false"); info("writer error", error); snapshot("after", "junction target", real / L"photo.rawnode.json", inspect(real / L"photo.rawnode.json"));
    check(inspect(real/L"photo.rawnode.json").bytes == (ok ? NEW : OLD), "junction resolves intended actual file");
    if (mode == "minimal") check(ok, "resolved NTFS parent permits ordinary junction"); require(RemoveDirectoryW(alias.c_str()) != FALSE, "remove junction fixture");
  }
  for (const auto &mode : modes) {
    Case c(root, "final_symlink", mode); const auto target = c.dir / L"target"; make(target, OLD); DeleteFileW(c.path.c_str());
    require(CreateSymbolicLinkW(c.path.c_str(), target.c_str(), SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE) != FALSE, "symlink fixture");
    const auto before = inspect(target); std::string error; const bool ok = runWriter(c.path, mode, error); info("writer return", ok ? "true" : "false"); info("writer error", error);
    check(unchanged(before, inspect(target)), "final symlink target never changed"); if (mode != "bare") check(!ok, "final link refused");
  }
  for (const auto &mode : modes) {
    Case c(root, "compressed_file", mode);
    { Handle file(CreateFileW(c.path.c_str(), GENERIC_READ|GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr)); USHORT compression = COMPRESSION_FORMAT_DEFAULT; DWORD n;
      require(DeviceIoControl(file.h, FSCTL_SET_COMPRESSION, &compression, sizeof(compression), nullptr, 0, &n, nullptr) != FALSE, "compress file fixture"); }
    c.run();
  }
  for (const auto &mode : modes) {
    Case c(root, "compressed_parent", mode, false);
    { Handle dir(CreateFileW(c.dir.c_str(), GENERIC_READ|GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr)); USHORT compression = COMPRESSION_FORMAT_DEFAULT; DWORD n;
      require(DeviceIoControl(dir.h, FSCTL_SET_COMPRESSION, &compression, sizeof(compression), nullptr, 0, &n, nullptr) != FALSE, "compress directory fixture"); }
    make(c.path, OLD); c.run();
  }
  for (const auto &mode : modes) {
    Case c(root, "encrypted_file", mode);
    const auto r = result("EncryptFileW fixture", EncryptFileW(c.path.c_str()));
    if (r.ok) c.run(); else info("unverified", "EFS fixture unavailable error " + std::to_string(r.error));
  }
}
PersistGrade grade() {
  PersistGrade g; g.color = {"rec2020", "linear", "display-p3", "linear"};
  g.graph.primarySource = NodeId{"camera"}; g.graph.documentOutput = {NodeId{"camera"}, ports::image};
  PersistNode source; source.id = "camera"; source.operation = NodeOperation::Source; g.graph.nodes.push_back(source); return g;
}
void unicodePipeline(const fs::path &root, bool ctype = false) {
  if(ctype) require(std::setlocale(LC_CTYPE,".UTF8") != nullptr,"UTF-8 character-type locale");
  const auto images = root / L"pipeline"; fs::create_directories(images); current = "unicode_pipeline";
  info("ACP", std::to_string(GetACP())); info("OEMCP", std::to_string(GetOEMCP())); info("CRT locale",std::setlocale(LC_CTYPE,nullptr));info("CRT mbcp",std::to_string(_getmbcp()));
  Image authored; authored.w = authored.h = 1; authored.px = {0.25f,0.5f,0.75f,1.0f};
  const auto seed = images / L"seed.png"; require(writeImage(authored, seed.u8string(), {RgbGamut::DisplayP3,TransferFunction::Linear}), "actual production tagged image fixture");
  PersistFile f; f.grade = grade(); f.metadataJson = R"({"mark":"pick"})"; std::string json, error; require(encodeGraphFile(f,json,error), "grade fixture encode");
  for (const auto &name : {L"ascii", L"caf\u00e9", L"\u65e5\u672c", L"photo_\U0001f98a"}) for (const auto &extension : {L".png", L".dng"}) {
    current = "pipeline/" + utf8(name) + utf8(extension);
    const bool dng = std::wstring(extension)==L".dng";
    const ColorEncoding expected = dng ? ColorEncoding{RgbGamut::Rec2020,TransferFunction::Linear} : ColorEncoding{RgbGamut::DisplayP3,TransferFunction::Linear};
    const auto dir = images / (L"case_" + std::to_wstring(std::hash<std::wstring>{}(name)) + std::wstring(extension)); fs::create_directory(dir);
    const auto path = dir / (std::wstring(name) + extension); require(CopyFileW(dng ? fs::path(RESEARCH_DNG).c_str() : seed.c_str(), path.c_str(), TRUE) != FALSE, "Unicode image fixture copy");
    make(fs::path(path.wstring()+L".rawnode.json"), json);
    std::vector<std::string> discovered;
    try { discovered = listFolderImages(dir.u8string()); info("actual discovery count", std::to_string(discovered.size())); }
    catch (const std::exception &e) { info("actual discovery exception", e.what()); }
    if (!discovered.empty()) {
      const auto p = discovered.front(); info("discovered filename hex", hex(p)); info("expected UTF-8 hex", hex(path.u8string()));
      Image decoded; ColorEncoding detected; bool raw;
      const bool loaded = loadImage(p, decoded, detected, raw); info("actual loadImage", loaded ? "true" : "false");
      info("profile correct", loaded && detected == expected ? "true" : "false");
      PersistFile loadedSidecar; const auto status = readGraphSidecar(inputSidecarPath(p), loadedSidecar, error);
      info("actual sidecar status", std::to_string(int(status))); info("actual sidecar error", error);
      const bool currentSaved = updateSidecarMark(inputSidecarPath(p), PhotoMark::Reject, error); info("actual current save discovered path", currentSaved ? "true" : "false"); info("current save error", error);
      if (GetACP()==CP_UTF8) check(currentSaved,"manifest actual production mark save");
      const bool saved = experiment::save(inputSidecarPath(p), json, error); info("experimental save discovered path", saved ? "true" : "false"); info("save error", error);
      if (GetACP() == CP_UTF8) {
        if(!dng || std::wstring(name)==L"ascii" || ctype) check(p == path.u8string() && loaded && loadedSidecar.grade && saved && detected == expected, "UTF-8 coherent actual discovery/decode/profile/grade/save pipeline");
        else check(!loaded && loadedSidecar.grade && saved,"manifest-only LibRaw narrow path remains broken for non-ASCII DNG");
      }
    } else if (GetACP() == CP_UTF8) check(false, "UTF-8 manifest must discover photograph");
    // Scoped conversion fixes discovery; it does not change narrow decoder/ICC opens.
    std::vector<std::string> explicitPaths;
    for (const auto &p : fs::directory_iterator(fs::u8path(dir.u8string()))) if (p.path().extension() == extension) explicitPaths.push_back(p.path().u8string());
    Image decoded; ColorEncoding detected; bool raw;
    const bool loaded = loadImage(explicitPaths.front(), decoded, detected, raw);
    if(dng){auto rawProbe=std::make_unique<LibRaw>();const auto input=explicitPaths.front();info("LibRaw object size",std::to_string(sizeof(LibRaw)));info("LibRaw narrow open status",std::to_string(rawProbe->open_file(input.c_str())));rawProbe->recycle();const int wideStatus=rawProbe->open_file(fs::u8path(input).c_str());info("LibRaw wide open status",std::to_string(wideStatus));if(!wideStatus){info("LibRaw wide unpack status",std::to_string(rawProbe->unpack()));info("LibRaw wide process status",std::to_string(rawProbe->dcraw_process()));}}
    info("explicit UTF-8 discovery loadImage", loaded ? "true" : "false"); info("explicit UTF-8 profile correct", loaded && detected == expected ? "true" : "false");
    const auto unicodeDir = images / (std::wstring(name) + std::wstring(extension) + L"_folder"); fs::create_directory(unicodeDir); require(CopyFileW(seed.c_str(), (unicodeDir/L"photo.png").c_str(), TRUE) != FALSE, "Unicode directory image");
    try { const auto paths = listFolderImages(unicodeDir.u8string()); info("Unicode folder actual discovery count", std::to_string(paths.size())); if (GetACP()==CP_UTF8) check(paths.size()==1,"manifest Unicode folder discovery"); }
    catch(const std::exception &e) { info("Unicode folder exception",e.what()); if(GetACP()==CP_UTF8)check(false,"manifest Unicode folder exception"); }
  }
  current = "invalid_utf8"; const auto before = names(images); check(!experiment::save(images.u8string()+"/invalid-"+std::string(1,char(0xff)),NEW,error),"invalid UTF-8 refusal"); check(before==names(images),"invalid UTF-8 changes no directory entries");
}
void volume(const fs::path &root, const fs::path &volumeRoot) {
  fs::create_directories(volumeRoot); current="volume"; wchar_t filesystem[64]; GetVolumeInformationW(volumeRoot.root_path().c_str(),nullptr,0,nullptr,nullptr,nullptr,filesystem,64); info("filesystem",utf8(filesystem));
  for(const auto &mode:modes){Case c(volumeRoot,"volume_replace",mode);c.run();}
  for(const auto &mode:modes){
    Case c(root,"junction_external_"+utf8(filesystem),mode); const auto target=volumeRoot/wide(mode);fs::create_directories(target);junction(c.dir/L"alias",target);
    const auto path=c.dir/L"alias"/L"sidecar.json";make(target/L"sidecar.json",OLD);const auto before=inspect(target/L"sidecar.json");std::string error;
    const bool ok=runWriter(path,mode,error);info("external junction return",ok?"true":"false");info("external junction error",error);snapshot("after","external target",target/L"sidecar.json",inspect(target/L"sidecar.json"));
    if(!ok)check(unchanged(before,inspect(target/L"sidecar.json")),"cross-filesystem junction refusal preserves actual target"); require(RemoveDirectoryW((c.dir/L"alias").c_str())!=FALSE,"remove cross filesystem junction");
  }
  current="read_write_separation"; const auto path=volumeRoot/L"grade.rawnode.json";PersistFile f;f.grade=grade();std::string json,error;require(encodeGraphFile(f,json,error),"grade encode");make(path,json);
  std::string loaded;const auto status=persist_detail::readGraphFileText(path.u8string(),loaded,error,persist_detail::FileFault::None);info("current read status",std::to_string(int(status)));info("current read error",error);
  std::ifstream in(path,std::ios::binary);const std::string direct{std::istreambuf_iterator<char>(in),{}};PersistFile decoded;check(decodeGraphFile(direct,decoded,error)&&decoded.grade.has_value(),"valid saved grade independently decodes on write-unqualified filesystem");
  if(wcscmp(filesystem,L"NTFS")==0){current="minimal_disk_full";const auto full=volumeRoot/L"full.json";make(full,OLD);const auto before=inspect(full);std::string error;const bool ok=experiment::save(full.u8string(),std::string(160*1024*1024,'F'),error);info("disk full result",ok?"true":"false");info("disk full error",error);check(!ok&&unchanged(before,inspect(full)),"genuine disk-full failure preserves original");}
}
int wmain(int argc,wchar_t **argv){try{
  if(argc>=3&&std::wstring(argv[1])==L"--read-created"){readCreated(argv[2]);return failures?1:0;}
  if(argc>=3&&std::wstring(argv[1])==L"--create-other"){for(const auto &mode:modes){const auto dir=fs::path(argv[2])/L"foreign_owner"/wide(mode);fs::create_directories(dir);make(dir/L"photo.rawnode.json",OLD);}return 0;}
  if(argc>=5&&std::wstring(argv[1])==L"--prepare"){prepare(argv[2],argv[3],argv[4]);return failures?1:0;}
  if(argc>=4&&std::wstring(argv[1])==L"--volume"){volume(argv[2],argv[3]);return failures?1:0;}
  if(argc<3)throw std::runtime_error("root and mode required"); const fs::path root=argv[1];fs::create_directories(root);
  current="environment";info("account SID",utf8(userSid()));info("ACP",std::to_string(GetACP()));
  if(std::wstring(argv[2])==L"--pipeline" || std::wstring(argv[2])==L"--pipeline-ctype"){unicodePipeline(root,std::wstring(argv[2])==L"--pipeline-ctype");}
  else{ordinary(root);newPermissions(root);failuresCases(root);pathCases(root);securityCases(root);unicodePipeline(root);}
  std::cout<<"{\"type\":\"summary\",\"failed_checks\":"<<failures<<"}\n";return failures?1:0;
}catch(const std::exception &e){std::cerr<<"case "<<current<<": "<<e.what()<<" ("<<GetLastError()<<")\n";return 2;}}
