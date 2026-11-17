# Codex: minimum Windows atomic persistence research

17 November 2026. Independent research, based on reviewed revision
`5e5aa22145b3cfdb23674f00a991f3d748d62feb`. Production sources and PR #117 are
unchanged. No merge or implementation PR. No consultation of Claude's new work.

**Recommendation:** proceed with a smaller, bounded correction after the owner
settles the metadata/default-permission policy. Do not merge #117 unchanged:
the native discovery-to-save experiment demonstrated a successful write to the
wrong photograph's sidecar under mixed ACP/UTF-8 paths.

## A. Minimum recommended contract

RawNode should promise complete, atomic publication of its **JSON data stream**
at the selected pathname on qualified storage; discovery, reading and writing
must agree on that pathname; original JSON and live edits
survive ordinary preparation/publication failure. Respect read-only state and
actual write access. Preserve ordinary access permissions without weakening
known enforcement protections. Report the failing operation and native error;
report the exact owned temporary when cleanup fails. Keep existing validation,
untouched grade/metadata preservation and Retry / Discard / Cancel.

This is not a promise to preserve file identity, owner/group, all ACL/security
metadata, arbitrary streams/xattrs, timestamps, compression state or the relation
between hard-link aliases. It is not a crash/power-loss transaction, a defense
against hostile concurrent directory substitution, or cloud-sync conflict
management. Native results do not prove every late rename I/O failure harmless.
No in-place, copy-over, unlink-first or permission-clearing fallback is proposed.

### What POSIX actually does

Verified in `src/persist/ProjectPersistSave.cpp:376–444` at the reviewed commit:

| Property | Current macOS/Linux implementation |
|---|---|
| Publication | Sibling `mkstemp`, checked stdio write/flush/close, then same-filesystem `rename`; no fallback. `fflush` is not `fsync`; no directory sync or power-loss promise. |
| Write permission | `lstat` regular-file check and `access(W_OK)` on the old destination. Parent permission is exercised by creation/rename. |
| New permissions | `mkstemp`'s 0600. |
| Existing permissions | `fchmod(old.st_mode & 07777)` on the completed sibling; common mode bits retained. |
| Owner/group | No `chown`; replacement gets the creator/directory's natural ownership/group. |
| ACLs/xattrs | No explicit old-file ACL or xattr copying. Parent defaults may affect the sibling; copying mode bits is not full ACL preservation. |
| Hard links | No link-count restriction: replace one directory entry; other aliases keep the old inode. |
| Symlinks | Refuse a final symlink. Parent symlinks are followed; no ancestor walk. |
| Identity/times | New inode and ordinary creation/write times; old timestamps are not copied. |
| Cleanup/failures | Only the owned sibling is removed; checked error path. Cleanup failure is reported. Existing FileFault seam writes real partial temporary data. |

`PosixProbe.cpp` calls the actual unchanged writer. On Linux overlayfs as uid/gid
1000: mode 0664 survived; user xattr disappeared; the other hard link retained OLD
while the chosen path became NEW; final symlink was refused; new file was 0600;
a write fault left NEW unchanged. See `POSIX-EVIDENCE.txt`. A valid named-user ACL
fixture was rejected with EINVAL on this environment, so ACL loss is a **source
finding**, not a claimed native ACL experiment. No new native macOS experiment.

Windows DACL copying is nevertheless justified: DACLs are Windows' ordinary
access-permission model, analogous to the POSIX mode policy. The fact that POSIX
does not preserve extended ACLs is not a reason to discard basic Windows access
restrictions. Requiring identical owners, zero streams, exactly supported
incidental attributes, one hard link and reparse-free ancestors is substantially
stronger than the existing POSIX contract.

## B. Native evidence and scope

- Branch: `codex/research-windows-minimal-20261117`, from the exact reviewed head.
- Initial complete comparison: commit `ccd1875ba304c676d78743341f133e5be689d2e0`,
  [run 44101637565](https://github.com/ryancara/RawNode/actions/runs/44101637565),
  success. 615 checks per account, zero failures; initial PNG/manifest tests passed.
  FAT/exFAT setup was unavailable because the fixture label exceeded its accepted
  length. These filesystems were not qualified by that run.
- Expanded DNG/permissions comparison: commit
  `1508760a7d562256e29fc6c7755bd13a0a0dc95c`,
  [run 44103843164](https://github.com/ryancara/RawNode/actions/runs/44103843164).
  Both account matrices passed 627 checks and both other-account permission
  comparisons passed. The run **failed** three real manifest-only non-ASCII DNG
  pipeline assertions; FAT32 then exposed an unsupported observer query, error 87.
  These failures are retained evidence, not production data corruption.
- Final tested source: [`0b6912455e6afcddd1e43b8f64e832a7d78e79fb`](https://github.com/ryancara/RawNode/commit/0b6912455e6afcddd1e43b8f64e832a7d78e79fb),
  [run 44109872879](https://github.com/ryancara/RawNode/actions/runs/44109872879),
  **SUCCESS**. Runner and ordinary-user matrices: **676 checks each**; manifest
  and manifest+CRT controls 31 each; scoped-stb control 3; other-account access
  3 each; NTFS/FAT32/exFAT VHDs 18/24/24. **1,489 checks, zero failed checks**.
  Passing controls deliberately demonstrate unsafe unchanged-baseline behavior;
  they do not mean the existing application has no Unicode bug.
- [Raw final artifact](https://github.com/ryancara/RawNode/actions/runs/44109872879/artifacts/14340871601),
  SHA-256 `d771e973c1dc2d3df93a8244ae7bfabafbf95826fa85233e1149f47af5b5f982`,
  expires 8 December 2026. Independently recomputed all **1,180 complete snapshot
  hashes** in Python. `RESULTS.json` retains durable case summaries;
  `Analyze.py` reproduces their verification from the downloaded artifact.
- Two intermediate diagnostic runs are retained: [44106307424](https://github.com/ryancara/RawNode/actions/runs/44106307424)
  (`8a8d2a63049669c701c642952ba25c0dce3649de`) failed after a second large
  stack-local LibRaw diagnostic object was added; moving that diagnostic to
  heap allocation restored the matrix. No writer failure is inferred from it.
  Its independent VHD step passed all three filesystems. [44108461041](https://github.com/ryancara/RawNode/actions/runs/44108461041)
  (`297d0903d38a420b635d79438a6ef83bed6a76f0`) established that UTF-8 CRT
  character-type locale also fails the three non-ASCII narrow-LibRaw cases;
  the wide overload opened, unpacked and processed each successfully.
- Native environment: x64 GitHub `windows-2022`, Windows Server 2022 Datacenter
  10.0.20348.5870; MSVC 19.44.35230.0, SDK 10.0.26100.0. D: was fixed NTFS;
  separate disposable 128-MB VHDs provided additional volume experiments.
- A real second local account created foreign-owner files. The matrix also ran
  under an ordinary Users-only account. Privileges were enabled only to construct
  owner/audit fixtures or observe audit ACLs, then disabled before writer calls.
  Low-integrity access was tested with an actual lowered impersonation token.
  The foreign-group fixture changes the primary-group SID field; it does not
  simulate joining an old Windows domain.
- Actual production persistence, discovery, image loading, ICC extraction and
  image-writing sources were compiled unchanged. Existing pinned LibRaw 0.22.3,
  stb/tinyexr, GLFW 3.4 and ImGui v1.91.8-docking were used. Existing image
  dependencies were built with vcpkg: lcms 2.19.1#1, TIFF 4.7.2, JPEG-turbo 3.2.1,
  zlib 1.3.2#3. This is headless component integration, not full Windows GUI or
  OpenFX/JXL/camera-format compatibility qualification.

`Observe.h` mechanically reuses measurement helpers from original probe
`20f820ecafa305c843c0c9ca3dc0846acade1897`. It records native returns/errors,
paths, exact bytes/hex, SHA-256, attributes, identity, times and security. Backups
are separate files; failure checks inspect the destination independently.
FAT lacks some NTFS observation APIs; unavailable fields are logged as such.

The modes are **bare**, **current** (unchanged PR backend), **dacl** (experimental
DACL transfer without label transfer), **minimal** (private creation plus DACL
and label transfer), and **inherited** (same smaller writer with ordinary
creation inheritance). Comparisons remove safeguards rather than merely test
that the current guards reject fixtures.

### Observed results

The table describes both accounts unless explicitly noted. Guard-generated
refusals (for example unqualified-filesystem code 50) are policy outcomes, not
claims that MoveFileExW failed with that code. Errors 5/32/112 at the documented
API failure points are genuine native failures. Error 0 is a
normalized success value, not a claim about successful APIs' GetLastError.

| Experiment | Bare/current | Smaller variants and independently observed consequence |
|---|---|---|
| Ordinary creation/replacement | Succeeded. | Complete NEW bytes; owned sibling absent after successful publication. |
| Write-denied old file, directory permits replacement | Bare replacement succeeded despite data-write denial. Current refused, error 5. | All smaller variants refused at FILE_WRITE_DATA preflight, error 5; old bytes/identity/compared metadata retained. Keep preflight. |
| Read-only | Native bare failure; current refused. | Refused without clearing READONLY; original retained. |
| Readers, with/without delete sharing | Native MoveFileExW returned 5 on this OS. | Safe failure; original and compared metadata retained, owned temporary removed. Delete sharing did not guarantee success. |
| Real locked source/cleanup | Rename and deletion returned 32. | Original retained; exact temporary remained until the fixture released its handle, then deletion succeeded. |
| Partial-write/flush/close/rename fault seams | Not a simulated internal filesystem rename failure. | Original retained and only owned sibling cleaned. Cleanup-fault case retained an independently inspected partial sibling and reported its exact path. |
| Foreign/stale owner and foreign group | Current refused owner/group mismatch, error 50. | Successful replacement; ordinary ACEs and protection retained. New owner/group follow ordinary creation. Stale SID is a deliberate synthetic stand-in for an unavailable old account. |
| Protected/unprotected DACL | Bare used the temporary's DACL. | Small GetSecurityInfo/SetSecurityInfo transfer retained tested ACEs and protection, including inherited entries. |
| Conditional callback DACL | Current refused ACE type. | Opaque DACL copied exactly and accepted by Windows; claim-dependent access combinations were not exhaustively evaluated. No need to parse each ordinary DACL ACE. |
| Inherited auditing | Current refused. | New sibling inherited the same directory auditing. No audit privilege was required by the writer. |
| Explicit per-file auditing | Current refused. | Explicit audit entries/protection were lost; directory defaults still apply. This is a real auditing-policy change, not access-control enforcement. |
| Audited directory, initial and subsequent save | Current initial save succeeded, subsequent save refused. | Both saves succeeded with inherited auditing. Current policy can create its own unsavable sidecar. |
| Medium label with NO_READ_UP | Low token read denied before replacement. Bare/DACL-only replacement then allowed the low token to read (5 → 0). | Label-copy variants kept denial (5 → 5). Current-writer write preflight alone does not preserve future access restrictions. |
| High NO_WRITE_UP label | Elevated runner could write it; ordinary account could not. | Ordinary account preflight refused, error 5. Label copy did not elevate privileges. |
| Low/inherited integrity labels | Current refused existing file. | Tested labels copied without enabling privileges; no blanket rejection of an audited or labeled file. |
| ARCHIVE cleared | Current refused. | Successful edits set ARCHIVE, as expected and now explicitly accepted. No post-publication restoration. |
| Hidden/SYSTEM/not-content-indexed | Bare could drop flags; current refused SYSTEM and some combinations. | One small FileBasicInfo mask preserved these tested flags; no exhaustive attribute allow-list. |
| Explicit file compression | Current refused. | Replacement could become uncompressed; bytes remained complete. Compression is not confidentiality. |
| Compressed parent | Current refused ancestor attributes. | Sibling inherited compression; replacement stayed compressed. |
| Explicit EFS encryption | Bare replacement changed 16416 (ENCRYPTED|ARCHIVE) to 32. | EFS refusal preserved old bytes and encryption. This is meaningful at-rest security, unlike compression. |
| Hard links | Current refused. | Chosen pathname became NEW; other alias retained OLD with original identity. Same ordinary semantics as POSIX. |
| Zone.Identifier, Dropbox-style and arbitrary ADS | Current refused each. | Ordinary replacement dropped each extra stream. JSON NEW remained complete. No provider installation or provider-compatibility inference. |
| Junction to NTFS directory | Current refused ancestor. | Resolved-parent qualification allowed it and published to the intended target. |
| Junction to separate NTFS VHD | Current refused ancestor. | Qualified actual resolved NTFS parent and published successfully; no cross-volume copy flag. |
| Final symlink | Bare replaced the link itself, leaving target unchanged. | Current and smaller writers refused; link/target preserved. Keep final-link refusal. |
| FAT32/exFAT and cross-filesystem junctions | Bare replacement succeeded. Current rejected direct paths with 87 and junction ancestors. | Smaller writer explicitly identified the resolved non-NTFS filesystem and refused with 50; original retained. Independent decode recovered the valid grade that current reading hid. These administrator-run VHD observations do not qualify general FAT/exFAT atomicity/permissions. |
| Genuine full NTFS VHD | Write failed, error 112. | Smaller writer retained original and removed its partial sibling. This is preparation failure, not late rename failure. |

No observed **failed publication** damaged original JSON. A **successful**
mixed-encoding baseline save did modify the wrong photograph's sidecar; that is
a separate caller/path-identity violation detailed below. This is a finite test
result, not an exhaustive claim about NTFS/media failure states. The original
[22,853-read visibility experiment](https://github.com/ryancara/RawNode/actions/runs/43985082057)
remains the atomic visibility evidence; an unchanged publication primitive does
not justify rerunning every original experiment. This round targets removed
safeguards and genuine changed-writer preparation/sharing failures.

## C. Safeguard inventory

Likelihood is qualitative for sidecars, not a measured deployment frequency.

| Safeguard / purpose | Realistic case and consequence if removed | Native/source evidence and cost | Recommendation |
|---|---|---|---|
| Completed sibling + one native rename: avoid partial publication | Universal; in-place writes can destroy edits. | Existing native visibility evidence plus new complete-byte/failure checks. Small core. | **Keep**. |
| CREATE_NEW and ownership: do not overwrite unrelated temporary names | Universal implementation requirement. | Owned cleanup and original unchanged; OS exclusive creation. Random names plus checked reservation suffice. | **Keep**. |
| Cryptographic temporary names | Collisions or guessed filenames; no need for a new naming subsystem. | BCrypt already OS-provided; fixed-size sibling names work. Main integrity guarantee is CREATE_NEW. | **Keep** existing small primitive. |
| Checked writes, flush, close, publication, cleanup | Ordinary disk/sharing failures. | Native error 112, sharing failures; seam cleanup/partial-write tests. | **Keep**, with RAII. No post-publication fallible fix. |
| Writable-file preflight | Real write-denied file in writable folder; raw rename bypasses it. | Native bare succeeds while preflight fails 5. One non-truncating open. | **Keep**; FILE_WRITE_DATA suffices, APPEND is unnecessary for replacement. |
| Read-only guard | Common user protection. | Native refusal and unchanged original. One attribute bit. | **Keep**. |
| DACL + protected/unprotected inheritance transfer | Common Windows permissions/shared folder. | Bare DACL loss; exact tested transfer. Two security calls/control bit. | **Keep**. |
| Owner/group equality | Transferred, reinstalled or other-user files. | Current rejects writable fixtures; smaller writer succeeds with same DACL. Adds temp inspection and usability refusals. | **Remove** equality; document normal creator ownership. |
| Allow/deny-only DACL ACE whitelist | Uncommon conditional ACL. | Opaque callback ACE copied exactly; parsing adds complexity and blocks usable file. | **Remove** whitelist; copy DACL as an OS-owned object. |
| All non-null/protected SACL refusal | Inherited auditing in managed folders is credible. | Current first-save/second-save bug; directory auditing naturally inherited. Filtered empty SACL does not imply lost enforcement. | **Remove** blanket refusal. |
| Enforcement label/special-metadata query | Explicit integrity policies uncommon but real confidentiality. | DACL-only permitted formerly forbidden low-integrity read. Small label copy or targeted refusal earns its place. | **Simplify** to known enforcement handling; no audit manager. |
| Full audit SACL preservation | Enterprise per-file logging, outside ordinary editing contract. | Explicit audits dropped, inherited audits retained. Copy requires privileges/extra machinery. | **Defer** preservation; owner must approve contract exclusion. |
| Resource-manager descriptor-control refusal | No demonstrated ordinary-sidecar case. | No test demonstrating benefit; do not infer enforcement from every descriptor bit. | **Remove/Defer** broad rule; keep targeted unknown-enforcement refusal. |
| Private protected creation DACL | Consistent with POSIX 0600, but blocks new-file collaboration. | Actual other-account read 5 versus 0 with inheritance. ~14 helper lines plus token/SID operations. | **Policy choice**, see D. |
| Zero ADS | Downloaded/provider-tagged files realistic; arbitrary user streams possible. | Tested streams dropped by ordinary replacement. Enumeration blocks legitimate JSON saves. | **Remove** blanket refusal under an explicit JSON-stream contract. |
| ARCHIVE-set prerequisite | Common backup-managed files. | Ordinary replacement sets ARCHIVE; clearing it is not required. Restriction makes legitimate saves fail. | **Remove**. |
| Hidden/NCI/SYSTEM preservation | Hidden files common enough; NCI/SYSTEM less common. | Native mask retained flags at negligible extra complexity. | **Keep** small copy mask. |
| Exhaustive allowed-attribute mask | Unknown/incidental flags, compressed files/folders. | Compression changes storage, not access; blanket policy over-restrictive. | **Remove**; exclude meaningful unsafe states. |
| EFS exclusion | Less common but real encryption. | Bare removal proved at-rest downgrade. One bit plus parent boundary. | **Keep refusal**, defer EFS/key management. |
| Link-count==1 | Hard-linked catalogs/sidecars possible. | Other link retained old file, exactly POSIX semantics. | **Remove**. |
| Every-ancestor walk | Ordinary NTFS junctions/mounts. | Correct resolved-parent publication; cross-filesystem test checks actual target. O(depth) repeated metadata I/O and many refusals. | **Simplify** to resolved parent. |
| DRIVE_FIXED-only | External/removable media normal for photographers. | A bus type alone does not determine NTFS rename semantics; no physical removable fixture available. | **Remove as security rule**; document hardware qualification limits. |
| Filesystem qualification | Remote/non-NTFS behavior not established by local NTFS. | VHD results below; filesystem check must follow redirection. | **Keep** one resolved local-NTFS boundary initially. |
| Final reparse/nonregular refusal | Symlink publication would replace link rather than viewed target. | Native target/link comparison. Few bits/type check. | **Keep**. |
| Cloud/offline special-state refusal | Provider placeholder behavior unqualified. | No installed-provider qualification; NTFS name alone cannot establish compatibility. | **Keep targeted refusals**, defer provider support. |
| Write-location gate before read | Valid saved grades on unsupported locations. | Source open path substitutes Source-only when read returns Error; independent grade decoding on other volumes. | **Remove from reading**. |
| Strict UTF-8 persistence conversion | Required, but ACP discovery can produce non-UTF-8. | Real discovery/decoder failures; direct writer tests missed them. | **Keep**, integrate coherently across actual input paths. |
| Hard-coded ordinary length/device/path checks | Some protect against ADS/device paths; others overstate Unicode support. | Bounds tested only ordinary lengths. | **Simplify**; retain NUL/invalid-UTF8/stream/device refusal, defer long-path promises. |

## D. Disputed policy recommendations

**Streams.** Define sidecars/presets/preferences as the unnamed JSON stream.
Recommend ordinary replacement semantics, including dropping extra streams,
without a generic stream copier or stream-name whitelist. Zone.Identifier is
real provenance, but this JSON is not an executable/active Office document and
RawNode has no MOTW enforcement path that this change bypasses. Do not generalize
that conclusion to arbitrary executables or active content. Dropbox-style bytes
were synthetic: their actual provider semantics were not qualified; blindly
copying an old provider identifier onto a new identity is not proven correct.
Arbitrary streams can contain real user data, so exclusion must be explicit and
approved rather than described as universally harmless. This parallels the
current absence of POSIX xattr preservation.

**SACLs.** Preserve access-enforcement integrity labels, or refuse when a small
checked transfer fails. Writable preflight protects today's caller, not future
readers. Allow inherited auditing and let the sibling inherit directory policy;
do not preserve privileged per-file auditing by default. The audit tests compare descriptor entries, not
verified event generation in a configured enterprise audit system. Refuse detected other
enforcement metadata that the small writer cannot transfer. Do not parse or
manage every enterprise configuration. Explicit audit loss needs owner approval;
proof that auditing is not access enforcement does not make audit loss imaginary.

**New files.** Recommend retaining private creation by default for consistency
with 0600 until the owner deliberately chooses Windows directory inheritance.
Inheritance is also a coherent product policy: it respects intentional shared
folders, is what established Windows libraries use, and removes token/SID/DACL
construction. In owner-only directories both are normally private. Native tests
show the actual collaboration difference. Private creation followed by inherited
publication worked using an empty native-inheritance exemplar, but needs extra
creation, security query/set and cleanup. That extra machinery is not justified
for ordinary JSON sidecars. Existing files retain their DACL regardless of the
chosen new-file policy. No approach is inherently the only correct policy.

**Ownership.** Recommend normal replacement ownership; do not match or restore
arbitrary old owners/groups. Explicit SID-based data access remains in the copied
DACL. Ownership itself confers administrative capabilities and owner-relative
ACEs can change meaning; do not claim identical security semantics for every
owner-dependent policy. In the ordinary-user `foreign_modify/minimal` case,
FILE_WRITE_DATA was allowed but WRITE_DAC open failed with 5 before replacement;
after normal creator ownership, WRITE_DAC succeeded (0), although the DACL was
copied. This administrative capability change is real. The parent already
permitted creating/replacing files; POSIX similarly gives the replacing user
owner/chmod authority. Accept it deliberately rather than claim zero security
consequence. This is the same deliberate ownership trade-off present
on POSIX, not a proof that ownership is meaningless. Accepting it is a product
contract decision; an ownership-preserving subsystem is not justified here.

**Directories/filesystems.** Resolve and inspect the parent once; qualify its
actual local NTFS filesystem, reject unsupported final reparse/provider/offline
states, and refuse a final file symlink. A directory junction is a path redirect,
not inherently an unsafe file. Sibling creation through that parent stays on its
resolved filesystem under the existing stable-directory contract. No malicious
substitution defenses. No name-based OneDrive/Dropbox blocklists. Cloud locations
without observable special metadata and arbitrary external synchronization cannot
be classified comprehensively by this check: they remain unqualified, not
silently claimed supported. Physical removable NTFS, ReFS, SMB/NAS and installed
cloud providers were not tested. FAT/exFAT observations alone do not establish
an equivalent permission/durability contract.

**Unicode.** Strict UTF-8 persistence must be paired with coherent discovery and
decoder conventions. Portable-file-dialogs already converts CP_UTF8 explicitly;
GLFW/ImGui use UTF-8. Current `listFolderImages()` and extension/path code use
narrow filesystem conversions; ICC extraction uses `fopen`, TIFF uses TIFFOpen,
and LibRaw uses its narrow `open_file` overload. Enabling only STBI_WINDOWS_UTF8
left discovery broken and profile extraction wrong, and can break an accented
path that the old ACP decoder could read. A UTF-8 manifest fixed actual PNG
integration, but failed non-ASCII DNG through LibRaw in the expanded run.
The CRT experiment set LC_CTYPE to `English_United States.utf8` and still
failed non-ASCII narrow LibRaw with IO error **-100009**. The wide overload
returned **0** for open/unpack/process for accented, CJK and supplementary DNG.
Recommend the UTF-8 process manifest (minimum Windows 10 1903, with an explicit
startup GetACP check) plus scoped Windows wide-path calls at the three LibRaw
open sites in ImageLoad.cpp (initial RAW, JXL reopen, embedded thumbnail).
The candidate expression is `raw.open_file(std::filesystem::u8path(path).c_str())`: the existing Windows wide overload; no general path manager is needed.
The latter is a demonstrated dependency-boundary technique; the production
RawNode source remains unchanged, so its post-correction integrated result
must be tested before merge. Do not add an ACP guessing/fallback writer.

A more serious native counterexample used two distinct disposable photographs,
`café.png` (U+00E9) and `cafÃ©.png` (U+00C3, U+00A9). Under ACP 1252, actual discovery emitted C3 A9 for
photograph B's name; those bytes are also valid UTF-8 for photograph A.
The unchanged strict-UTF8 read loaded A's Pick instead of B's Reject, and a
successful actual mark save for B modified **A's sidecar**, leaving B unchanged.
This happened under both accounts. A UTF-8 process emitted distinct correct
names and changed only B. See the `encoding_collision` cases, exact before/after
paths and hashes in the raw artifact/RESULTS.json. Strict conversion at the
writer boundary alone cannot establish file identity. No full GUI, OFX plugin, every RAW format, old Windows
version or full Unicode path-length compatibility claim.

Genuine owner decisions: unnamed-stream/audit exclusions; private versus
inherited new files; normal replacement ownership; initial local-NTFS/provider
boundary and minimum Windows/runtime Unicode policy. These recommendations do
not authorize production changes during this research task.

## E. Smallest reasonable implementation

`MinimalWriter.cpp` is a **174-line research prototype**, including headers,
RAII, strict conversion, diagnostics, policy comparison switches and fault seams;
`MinimalWriter.h` is 15 lines. The reviewed backend is 328 lines plus its header.
This is a comparison of responsibilities, not a target line count or a claim
that the prototype should be copied verbatim.

Keep: checked CREATE_NEW sibling publication, write access, read-only/final-link
checks, opaque DACL/inheritance transfer, targeted integrity handling, EFS
refusal, small visible-attribute mask, resolved-parent qualification and checked
owned cleanup. Keep existing codecs/callers and document lifecycle.

Delete owner/group queries/comparison/temp-security inspection, DACL ACE-type
whitelisting, stream enumeration, ARCHIVE refusal, hard-link refusal, the broad
attribute allow-list and the ancestor vector/walk. Replace blanket SACL rejection
with a short enforcement policy. Private creation is isolated and optional by
product policy. No new dependency is needed for the writer: advapi32 and bcrypt
are Windows APIs. The image dependencies in this research already belong to
RawNode; Qt/Rust are reference implementations, not proposed dependencies.

For production adaptation, expand terse experimental formatting, keep the exact
private/label/DACL policy readable, preserve meaningful native diagnostics (including a nonzero diagnostic for
nonregular types or a zero-progress write) and
reuse the existing FileFault tests. Do not transplant the comparative harness.

### Mature references

- Qt 6.10.0, commit `5a8637e4516bc48a0b3f4b5ec3b18618b92e7222`:
  [`src/corelib/io/qsavefile.cpp`](https://github.com/qt/qtbase/blob/5a8637e4516bc48a0b3f4b5ec3b18618b92e7222/src/corelib/io/qsavefile.cpp) checks writability and commits a temporary;
  [`qfilesystemengine_win.cpp:1775`](https://github.com/qt/qtbase/blob/5a8637e4516bc48a0b3f4b5ec3b18618b92e7222/src/corelib/io/qfilesystemengine_win.cpp#L1775) uses MoveFileEx(REPLACE_EXISTING).
  `setPermissions()` uses `_wchmod`, not a comprehensive DACL/SACL/ADS system.
  `qtemporaryfile.cpp` uses CREATE_NEW with ordinary Windows inheritance.
  Useful: small checked lifecycle and explicit direct-write fallback policy.
  RawNode must keep direct-write fallback disabled and add its justified DACL
  policy; mature library use is not proof of RawNode's stronger access contract.
- Rust tempfile 3.23.0, commit `fe9f4a305b785abd1f4f912c8304bcc90024f695`,
  [`src/file/imp/windows.rs`](https://github.com/Stebalien/tempfile/blob/fe9f4a305b785abd1f4f912c8304bcc90024f695/src/file/imp/windows.rs): exclusive creation, ordinary inheritance and
  MoveFileExW with optional REPLACE_EXISTING; no generic old-file metadata copier.
  Useful: exclusive ownership and a small publication operation. Its metadata
  contract alone does not replace RawNode's write preflight/DACL requirement.
- ART `40ec1cd38a11e13883d8f6159ce7017aa97e988a` and vkdt
  `aa9329fcc04b666f0f4b1cf5961835b6e56ac636` direct-writing findings from the
  original report remain relevant examples of simplicity with a different
  failure contract. Their in-place writes remain rejected.

## F. Correction plan, without making those changes

**Essential correctness:** separate read from write-location qualification;
fix coherent Unicode discovery/decoding/persistence, including the demonstrated
wrong-photograph save; retain writable preflight,
DACL/inheritance and meaningful integrity/EFS policy; retain checked data I/O,
rename and owned cleanup. Update actual native production regressions when a
correction implementation is later approved.

**Justified simplification:** accept ARCHIVE setting, normal ownership and
hard-link replacement; copy opaque DACLs; accept ordinary junctions and compressed
locations; use a resolved-parent check; drop indiscriminate streams/auditing/
metadata refusals under the explicitly approved minimum contract.

**Documented limitations:** local NTFS qualification; provider/remote/other-FS
and physical removable hardware coverage; supported Windows/runtime versions;
owner-dependent ACL semantics; unpreserved incidental metadata; stable directory
assumption; non-exhaustive late I/O/crash/power-loss evidence.

**Deferred:** per-file audit/stream preservation, EFS key management, CAP/resource
policy support, installed-provider qualification, long paths, broader codecs/
plugins/GUI validation. No recovery journal, lock manager or fallback writer.

### Read/view and departure behavior

At `DocumentActions.cpp:394–450`, a sidecar read Error sets protectedSource,
skips the valid saved grade and installs Source-only fallback. Departure skips
saving protected photos. Merely loosening the reader does not change this
architecture: a successfully decoded grade follows the existing restore branch;
an unsupported write still fails in the writer and existing departure prompts
retain live edits. This is the smallest interim fix, consistent with D050's
existing behavior for a readable but unwritable photo. It permits viewing and
editing while saving remains unsupported; it must not promise that edits can be
committed there. If the owner wants known-unsupported locations read-only before
editing, reuse `sidecarWriteBlockedPath` **after** restoring the grade and adjust
the warning which currently says the grade was not loaded. That is a small,
separately reviewed lifecycle/message change, not a new document architecture.
No native GUI behavior was inferred from a standalone writer test.

## G. Verdict

**READY TO PROCEED TO A FOCUSED CORRECTION IMPLEMENTATION, after owner policy
approval; not ready to merge PR #117 unchanged.** The bounded writer's core
behavior is supported. The integrated Windows save path still requires the
essential Unicode corrections and read/write separation, followed by actual
production regression tests. No broader security/metadata framework or more
speculative metadata preservation is justified by this evidence. A smaller writer is supported by the comparative
results; deleting every security operation is not. The concrete low-integrity
read and EFS regressions justify targeted protection. The remaining policy
choices belong to the owner. PR #117 should not be merged unchanged, and this
research does not modify it.

Primary API references: [MoveFileExW](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-movefileexw),
[security-information access rights](https://learn.microsoft.com/en-us/windows/win32/secauthz/security-information),
[mandatory integrity](https://learn.microsoft.com/en-us/windows/win32/secauthz/mandatory-integrity-control),
[UTF-8 process code page](https://learn.microsoft.com/en-us/windows/apps/design/globalizing/use-utf8-code-page).
API documentation supports API interpretation; the tables above identify actual
native observations. Product recommendations and parity judgments are inferences.

Documentation is published separately from the exact tested source. Changes
against the reviewed revision are confined to this research directory and its
new workflow; no production, existing test, roadmap or PR #117 mutation.
The PR branch was independently checked at its original reviewed SHA.
