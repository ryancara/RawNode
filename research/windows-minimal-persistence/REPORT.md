# RawNode — Windows atomic persistence: minimal implementation research (Claude)

Research only. No production code, PR #117 or merge was changed. Work was independent of Codex's new findings.

**Decision: the simplified writer is ready to proceed into production.** Four owner decisions (§D) shape scope, but none blocks the writer itself:

- new-file permissions;
- filesystem scope;
- the UTF-8 code-page manifest;
- read-only viewing of grades in unsupported locations.

Several PR #117 safeguards are shown natively to be redundant, or to turn ordinary photographer workflows into permanent save failures. The essential protections are few and cheap:

- a write-access probe;
- read-only and final-link refusal;
- DACL copying;
- an encryption check.

---

## B. Native evidence and provenance

| Item | Value |
|---|---|
| Research branch | `claude/windows-minimal-persistence-research` (disposable; based on PR #117 head `5e5aa22145b3cfdb23674f00a991f3d748d62feb`; other CI workflows removed on this branch only) |
| Run 1 | commit `2d5d17e9853d55e81ce530c65a5180aa359462cc`, [Actions run 44100932541](https://github.com/ryancara/RawNode/actions/runs/44100932541): **success**; job log SHA-256 `d9e00993a98605d92ec92cc38804e68058e5684de74a30e053215c7712aec044`; artifact 14336386869 (zip SHA-256 `41aaed90…4fe5`) |
| Run 2 | commit `37491bc`, [Actions run 44101851232](https://github.com/ryancara/RawNode/actions/runs/44101851232): **success**; job log SHA-256 `860519d4b4d80778f988451c0fa0acf393341630250f111bcf884ebdd795c22c`; artifact 14335224358 (zip SHA-256 `c1e60321…3277`) |
| Run 2 changes | The minimal writer reads attributes with `FileBasicInfo` instead of `FileAttributeTagInfo`. The `Cleanup` fault now performs a real partial write. Added an ANSI-only Unicode folder and a protected-DACL test on each filesystem. |
| OS | GitHub-hosted `windows-2022`: Windows Server 2022 Datacenter 10.0.20348 (UBR 5870); ACP 1252 |
| Compiler | MSVC 19.44.35230, C++17, `/W4` |
| Accounts | Runner administrator `…-500` (elevated, High integrity): **privileged fixture setup, EFS and inspection only**. Disposable ordinary user `…-1003` (Users group only): **all saves**. |
| Volumes | `D:` local fixed NTFS; disposable 128 MB VHDs: `Z:` FAT32, `Y:` exFAT; loopback SMB share `\\localhost\RawNodeResearch` (Windows NTFS backing) |
| Writers compared | **bare**: sibling + `MoveFileExW`, no checks. **pr**: the actual PR #117 `persist_detail::replaceWindowsFile` compiled from `5e5aa22`. **minimal**: `MinimalWindowsFile.cpp`. |
| Oracle | Independent before/after snapshots of bytes, owner, group, DACL SDDL, label SDDL, privileged SACL SDDL, attributes, file ID, link count, stream list and directory leftovers. These are taken with separate API calls, not writer return values. |

**Totals.** Run 1 + run 2 contained 495 save observations.

- **Every one of the 265 failed saves left the destination unchanged** in all compared fields: bytes, owner, group, DACL, label, attributes, file ID, links and streams.
- No failure left an unexpected temporary. The only leftovers were from the deliberate cleanup-failure test, and each was reported by exact path.
- Concurrent visibility: 1,000 replacements across five configurations in run 2, with a reader continuously opening and reading the file (share read/write/delete). The reader made 32,007 complete reads. **Zero were partial or unexpected and zero failed to open.** The writer was refused (error 5) whenever the reader held the file open.

**Evidence labels:** **T** native test (these runs); **D** Microsoft documentation; **S** inspected source; **I** inference; **U** untested.

---

## A. Minimum recommended contract

**RawNode guarantees (T on local NTFS; T on FAT32/exFAT where the owner enables them):**

1. A successful save publishes the complete new bytes atomically with a same-directory `MoveFileExW`: flags `0` to create, `MOVEFILE_REPLACE_EXISTING` to replace. It never copies, unlinks first or writes in place.
2. On any ordinary failure the original file is untouched and only RawNode's own sibling is removed. The failures exercised were:
   - write, flush, close and replace errors;
   - a genuine disk-full condition;
   - a held reader;
   - permission denial;
   - a folder that denies file creation.

   If cleanup itself fails, the error names the leftover path.
3. The save is refused, rather than published, when:
   - the destination is read-only;
   - the user may not write the file (the explicit write-access probe);
   - the final component is a directory, symlink, junction or other reparse point;
   - an individually encrypted file would be replaced by plaintext;
   - the location is outside the supported filesystems.
4. The destination's DACL and its inheritance protection carry over when the volume has ACLs. HIDDEN, SYSTEM and NOT_CONTENT_INDEXED carry over.
5. Errors state the operation, path and Windows error. Existing Retry / Discard / Cancel is unchanged.

**RawNode does not guarantee (deliberately, matching POSIX):**

- **Owner and group are not preserved.** They become the saving user's, which is ordinary rename semantics on POSIX too.
- **Several kinds of metadata are not carried over:**
  - alternate data streams;
  - explicit per-file audit entries;
  - explicit integrity labels;
  - per-file compression;
  - the cleared state of ARCHIVE (ARCHIVE becomes set because the file changed);
  - creation time.
- **No replacement while any other program has the file open.** The save fails cleanly and a retry succeeds once the file is closed.
- **No power-loss or media-removal durability.** `FlushFileBuffers` is best effort; this is the existing contract.
- **No defence against concurrent hostile namespace changes.**
- **No network or cloud-provider support** until separately qualified.

---

## Contract comparison with the POSIX writer

POSIX behaviour is from `ProjectPersistSave.cpp:375-446`.

| Aspect | POSIX today | PR #117 Windows | Minimal Windows |
|---|---|---|---|
| Publication | `rename` | `MoveFileExW` | `MoveFileExW` |
| Writability | `access(W_OK)` | write probe | write probe |
| Permissions | `fchmod` copies mode bits | DACL + protection copied | DACL + protection copied |
| New-file permissions | 0600, deliberate and test-enforced | protected user+SYSTEM DACL | inherited from folder *(owner decision)* |
| Owner/group | silently become the writer's | **refuse if different** | become the writer's |
| ACLs/xattrs (incl. macOS tags, quarantine) | not preserved | streams **refused**; SACL/labels **refused** | not preserved |
| Hard links | link broken, other names keep old data | **refused** | link broken (POSIX parity) |
| Symlink final | refused (`lstat`) | refused | refused |
| Timestamps/flags | not preserved (macOS `UF_HIDDEN` lost) | ARCHIVE-cleared **refused**; HIDDEN kept | ARCHIVE set; HIDDEN/SYSTEM/NCI kept |
| Filesystem | any | local fixed NTFS, every ancestor inspected | local NTFS (+ FAT32/exFAT with evidence) |
| Durability | no `fsync` | `FlushFileBuffers` | `FlushFileBuffers` |
| Open readers | allowed | fail cleanly | fail cleanly |

PR #117 is substantially stronger than POSIX in owner/group, streams, SACLs/labels, hard links, ARCHIVE, special attributes and ancestors. **The native evidence below shows none of these is required to prevent data loss or a security regression.** The protections that do matter correspond to POSIX's `access(W_OK)`, `lstat` and `fchmod`.

---

## C. Safeguard inventory

| # | Safeguard (PR #117) | Purpose | Native evidence | Consequence if removed | Cost | Rec. |
|---|---|---|---|---|---|---|
| 1 | Sibling `CREATE_NEW`, random name, share mode 0 | Owned, private sibling | T: a second open of a share-0 file fails with error 32 | Collisions; exposure of partial content | small | **Keep** |
| 2 | Private protected DACL (user+SYSTEM) for the temp and new files | Hide the temp; POSIX 0600 parity | T: new PR files got `D:P(A;;FA;;;U)(A;;FA;;;SY)`, unlike the folder control. **The elevated administrator could not open them.** On later saves the PR re-copied the protected DACL. Share mode 0 already hides the temp during writing (error 32). | New files inherit folder permissions, identical to the control file (T) | ~20 lines (token/SID/SDDL) | **Remove** *(owner policy, §D)* |
| 3 | Write-access probe | Refuse files the user may not write | T: bare **replaced** a write-denied file and swapped its DACL. Bare also replaced a high-integrity-label file in run 2, where the user held delete-child on the folder. The probe refused both, every time. | Rename bypasses deliberate write denial | ~4 lines | **Keep (essential)** |
| 4 | Read-only refusal | Clear refusal | T: bare got error 5 (NTFS, SMB) or 183 (FAT32/exFAT); unchanged | Same refusal, cryptic code | 1 line | **Keep** |
| 5 | Final-component reparse refusal | Do not destroy links | T: bare **replaced the symlink with a regular file**; the target was untouched and the owner changed | Link silently converted to a file | 1 line | **Keep** |
| 6 | DACL + protection copy | Windows analogue of `fchmod` | T: bare widened a protected private DACL to the folder default (Authenticated Users modify, Users read). It dropped an explicit deny (NETWORK) and an explicit allow (Guests). Minimal preserved all of these exactly on NTFS, and a protected DACL **over SMB**. | Meaningful security regression | ~12 lines | **Keep (essential)** |
| 7 | Owner/primary-group matching | Avoid owner change | T: Administrators-owned and foreign-SID-owned files (simulating another PC or a reinstall) were **permanently refused by PR**. Bare/minimal saved them: owner became the user, DACL unchanged. | Owner becomes the saver. The user already held Modify (incl. DELETE + folder create), so could delete and recreate the file anyway; no new capability (I). POSIX does the same. | ~8 lines + test fixture | **Remove** |
| 8 | SACL / label / resource / CAP / trust / filter inspection and refusal; special control flags | Do not drop system protections | T: **PR created a file in a folder with inherited auditing, then refused every later save of it.** Bare/minimal re-applied inherited auditing (`S:AI(AU;IDSA;FA;;;WD)`) on every save. An explicit audit ACE was lost on replacement (setting one needs `SeSecurityPrivilege`; auditing is not access control). A low label was dropped (T): Low-integrity processes then lose write access, a tightening (I). A high label was enforced by the write probe (#3). | Explicit per-file audit/label entries are not carried; inherited ones are | ~35 lines + privileged fixtures | **Remove** |
| 9 | ACE-type / NULL-DACL / owner-group-presence refusal | Exact-copy caution | U: no realistic fixture; `SetSecurityInfo` copies verbatim (D) | None demonstrated | ~12 lines | **Remove** |
| 10 | Alternate-stream refusal | Do not drop stream data | T: `Zone.Identifier`, `com.dropbox.attributes` and an arbitrary `notes` stream were all **refused by PR**. Bare/minimal saved them and the streams were dropped. | MOTW is removed from a JSON sidecar RawNode itself rewrote; JSON has no execute/Protected-View semantics, so no security effect (I). Provider metadata is regenerated (I). Arbitrary stream data would be lost, but nothing writes user data into sidecar streams (I). POSIX drops xattrs likewise. | ~16 lines | **Remove** |
| 11 | Hard-link refusal | Avoid split links | T: bare/minimal gave `v.json` NEW content (links 2→1) while `alias.json` kept OLD; PR refused | POSIX-identical behaviour; no edit loss | 2 lines | **Remove** |
| 12 | ARCHIVE-cleared refusal | "Preserve backup marker" | T: PR refused; bare/minimal saved and ARCHIVE was set (0x80→0x20) | ARCHIVE set is correct after modification | 4 lines | **Remove** |
| 13 | Attribute allow-list (refuse SYSTEM/COMPRESSED/ENCRYPTED/…) | Avoid changing special attributes | T: PR refused SYSTEM, HIDDEN+SYSTEM, compressed file, compressed folder and encrypted folder. Bare dropped HIDDEN, SYSTEM and NCI, and turned an encrypted file into **plaintext**. Minimal kept HIDDEN/SYSTEM/NCI, kept folder compression and encryption naturally, and refused only the plaintext case. Per-file compression was dropped (space only). | — | — | **Simplify:** copy HIDDEN\|SYSTEM\|NCI; refuse only when an encrypted file would become plaintext |
| 14 | Every-ancestor walk + `GetDriveTypeW(root)` + NTFS per ancestor | Detect junctions, cloud folders and other volumes | T: a junction parent on NTFS saved correctly through bare/minimal; PR refused it. A junction to FAT32 was **correctly identified as FAT32 by a single followed-folder volume query** (minimal). | — | ~30 lines + read/preferences callers | **Replace** with one followed-folder volume check |
| 15 | NTFS-only | Qualification | T: FAT32 and exFAT with minimal passed the whole matrix (§E). PR's `FileAttributeTagInfo` returns **error 87 on FAT32/exFAT**, so PR cannot inspect any file there and its error is misleading. | — | — | **Owner decision:** evidence supports local FAT32/exFAT |
| 16 | UNC / network refusal | Unqualified network | T: loopback SMB to a Windows NTFS share passed create/replace, DACL copy, hidden, read-only, readers, faults and visibility. NAS/Samba is U. | — | — | **Defer** (keep refusing until a NAS is qualified) |
| 17 | Read-side location gating | Route unsupported locations to protected mode | Code: avoids a failed-save prompt on every navigation, but hides valid grades | — | — | **Simplify** (§D) |
| 18 | Trailing dot/space check, `GetFileType` check | Path ambiguity | U: no RawNode producer creates such names | none realistic | ~8 lines | **Remove** |
| 19 | `FlushFileBuffers` | Best-effort durability; gives the Flush seam meaning | — | — | 1 line | **Keep** |
| 20 | Cleanup-failure reporting | Never silent debris | T: the cleanup fault left exactly one temp, named in the error | — | small | **Keep** |
| 21 | Preferences ancestor checks (two calls) | Avoid unqualified config folders | — | in-memory warning path remains | ~6 lines | **Remove** |

---

## D. Disputed policy decisions

**Alternate streams — ignore them; do not refuse; do not preserve.**

- Natively, refusal blocks downloaded/ZIP-extracted sidecars (MOTW) and Dropbox-style metadata permanently.
- Losing MOTW on a JSON file RawNode rewrote is not a security regression. MOTW governs execution and Office Protected View, neither of which applies to sidecar JSON.
- Preserving streams would need enumeration plus copy code, a generic stream manager, for no demonstrated benefit.
- Matches POSIX dropping xattrs.

**SACLs and special security — no inspection.**

- Inherited auditing is re-applied by Windows on creation (T). The PR's own policy refuses the second save of a file it created in an audited folder (T).
- Explicit audit entries require `SeSecurityPrivilege` to create, and are not access control.
- Integrity labels that restrict writing are already enforced by the write probe (T); lower labels only become stricter when dropped.
- Smallest sufficient policy: **write probe + DACL copy**.

**New-file permissions — owner decision.**

- **Recommendation:** default inherited security, the same as every other file the user creates in that folder (T: identical to the control).
- **The PR's private protected DACL** excludes Administrators (T: an elevated admin could not open the file). It disables inheritance permanently, because later saves copy it. It breaks shared-folder and multi-account use.
- **"Private temporary, then inherited at publication"** produces the same final result as inheriting from the start. Its privacy during the write is already provided by share mode 0 (T: error 32), so it adds ~15 lines for nothing.
- Choosing inheritance consciously diverges from POSIX's deliberate 0600 for new sidecars. A sidecar is no more sensitive than the photograph beside it. Record the divergence (or revisit the POSIX rule) in `DECISIONS.md`.

**Ownership changes — accept.**

- The saver becomes owner (T), exactly as with POSIX rename.
- No new capability: the user already needed write + delete rights.
- Refusing produces permanent failures for drives kept across reinstalls, files from another PC, and files created while elevated (T).

**Directory qualification — one check.**

- Open the destination's folder *following* links and query the real volume with `GetVolumeInformationByHandleW` + `FileRemoteProtocolInfo` (T). This replaces the every-ancestor walk.
- Junction parents are fine (T).
- Hostile substitution is out of contract.

**Filesystem support — owner decision.**

- Native evidence supports **local NTFS, FAT32 and exFAT** (DACL copy skipped when the volume lacks persistent ACLs). These are the formats of camera cards and most external drives.
- Untested (U):
  - physical removable media and surprise removal;
  - ReFS;
  - NAS/Samba;
  - OneDrive and other cloud providers.
- Crash/removal consistency on FAT/exFAT is not promised, the same as POSIX on exFAT.
- **Network:** keep refused until a NAS is qualified. Loopback SMB evidence is positive.
- **Cloud (OneDrive): U.** The interim must avoid a "saves once" trap: a new sidecar is created, then becomes a placeholder, then is refused. The interim should treat cloud-placeholder files *and* cloud-placeholder folders (reparse or recall attributes on the followed folder) as unsupported locations. Qualify on a real OneDrive client before widening.

**Unicode integration — add a UTF-8 `activeCodePage` application manifest (owner decision, smallest coherent fix).**

T, production `listFolderImages` / `readGraphSidecar` / `updateSidecarMark` at `5e5aa22`, **without a manifest (ACP 1252):**
- A single CJK/emoji filename makes `listFolderImages` **throw**, so the folder lists 0 photos.
- A non-ASCII folder chosen in the dialog is "not a directory".
- In an accented-only folder, `café.nef` decodes and the pre-PR read works, but **PR #117's read fails** ("Invalid UTF-8 persistence path") and mark saves fail. That is a regression for previously functional filenames.
- Scoped `u8string()` listing fixes persistence but **breaks image opening** (narrow `fopen` fails), because LibRaw `char*`, `TIFFOpen`, `stbi_load` and `fopen` use the ANSI code page (S).

**With the manifest (ACP 65001):** folder listing, image open, pre-PR read, PR read and mark save **all succeed** for accented, CJK and supplementary names, in ASCII and non-ASCII folders.

- RawNode's other dependencies already use UTF-8: GLFW drops, pfd dialogs, ImGui ini, tinyexr (S).
- The manifest needs Windows 10 1903+ (D). Full-application validation is still required.

**Reading vs writing — reuse the existing protected state.**

- Remove location qualification from `readGraphFileText`.
- Add one predicate shared with the writer: "can this location be saved?" (POSIX: true).
- In `openPreparedPhoto`:
  - if the sidecar decodes but the location cannot be saved, **restore the grade** and set `sidecarWriteBlockedPath`;
  - skip the initial save.
- The existing protected machinery then blocks edits, departure saves (no prompt storm) and capture, while the graded image displays and exports.
- Reword the four "saved grade was not loaded" messages to "this photo is read-only".
- About 25 lines plus tests. It is a visible behaviour change, so owner approval is needed.
- Smallest acceptable interim: keep today's routing (unsupported → Source-only protected) but use the single predicate, and document it truthfully.

---

## E. Minimal implementation

**Experimental writer:** `research/windows-minimal-persistence/MinimalWindowsFile.cpp` is **187 code lines**, against **306** for PR's `WindowsFile.cpp`. That count includes its own RAII handle, UTF-8 overload and error formatting. In production, sharing PR's existing UTF-8 helpers and adding the location predicate gives roughly **170–190 lines** in total.

Sequence:

1. **Location.** Open the folder following links; `GetVolumeInformationByHandleW` (filesystem name, `FILE_PERSISTENT_ACLS`); `FileRemoteProtocolInfo` (network). Refuse locations outside the agreed set.
2. **Inspect.** No-follow handle opened with `FILE_READ_ATTRIBUTES | READ_CONTROL`. **Use `FileBasicInfo`** (`FileAttributeTagInfo` returns error 87 on FAT32/exFAT, T). Refuse:
   - directory or reparse point;
   - READONLY;
   - failed `FILE_WRITE_DATA` probe.

   Then read the DACL and its control flags when the volume has ACLs.
3. **Create.** `CREATE_NEW` sibling, share 0, **default security**, access `GENERIC_WRITE | FILE_READ_ATTRIBUTES | WRITE_DAC`. If the destination is ENCRYPTED and the sibling is not, refuse.
4. **Write and prepare.** Checked `WriteFile` loop. When present: `SetSecurityInfo` (DACL + protected/unprotected); `FileBasicInfo` with HIDDEN\|SYSTEM\|NCI plus ARCHIVE (skip when zero). Then `FlushFileBuffers` and a checked close.
5. **Publish.** Close the inspection handle; `MoveFileExW(…, present ? MOVEFILE_REPLACE_EXISTING : 0)`.
6. **Clean up.** RAII; on failure delete only the sibling and report a leftover path.

**Deletable from PR #117:**

- `WindowsFile.cpp`:
  - `readSecurity` beyond the DACL;
  - `defaultStreamOnly`;
  - `privateSecurity`;
  - `windowsParents` and its `allowMissing` parameter;
  - the trailing-dot loop;
  - the owner/group comparison;
  - the hard-link, ARCHIVE and attribute-allow-list refusals;
  - the `GetFileType` check;
  - `attributes()` (replaced by `FileBasicInfo`).
- `ProjectPersistLoad.cpp`: `windowsParents` and the reparse refusal in `readGraphFileText`.
- `Preferences.cpp`: both `windowsParents` calls.
- `tests/WindowsPersistenceTests.cpp`:
  - `systemSecurity` and its `SeSecurityPrivilege` handling;
  - the ADS/ARCHIVE/hard-link refusal assertions, converted to success-path assertions;
  - the junction fixture, which instead asserts that saving through a junction parent works.

**Keep / add tests.**

Keep:
- creation and replacement;
- preservation under each fault;
- write denial;
- read-only;
- open readers (both share modes) with retry;
- Unicode;
- DACL protected/explicit preservation;
- new-file DACL equals the control;
- cleanup-failure path reporting;
- final symlink refusal;
- the ordinary-user CI pass.

Add:
- an owner-mismatch save (administrator-created, ordinary-user save);
- the encrypted-file refusal (runner);
- the FAT32/exFAT VHD matrix, if enabled;
- `listFolderImages` → read/mark integration under the manifest.

---

## F. Correction plan

**Essential fixes:**

1. Remove the ARCHIVE, owner/group, alternate-stream, SACL/label and hard-link refusals. They are permanent save failures in realistic workflows, and natively demonstrated.
2. Replace `FileAttributeTagInfo` with `FileBasicInfo`. It is required on any non-NTFS volume, and it fixes PR's misleading "Cannot inspect file attributes (87)" on FAT drives.
3. Resolve the Unicode regression:
   - **preferred:** UTF-8 `activeCodePage` manifest on the Windows executable and the native test target;
   - **otherwise:** do not claim Unicode support, and accept that ANSI-representable names regress for reading.
4. Make the docs truthful: unsupported locations open protected without their grade (until the read/write change), and list the metadata that is not carried over.

**Justified simplifications:**

- Default inherited security for new files (owner decision).
- One followed-folder volume check instead of the ancestor walk.
- Narrow the attribute policy to copying HIDDEN\|SYSTEM\|NCI plus the encryption check.
- Remove the trailing-dot, `GetFileType` and preferences ancestor checks.
- Trim the tests as listed above.

**Documented limitations:**

- Saves fail while another program holds the file open: 15–90% of saves were refused under a tight-loop reader, depending on filesystem (T); this should be rare in normal use (I).
- Owner, streams, explicit audit/labels and per-file compression are not carried over.
- `MAX_PATH`.
- No power-loss promise.
- Network and cloud unsupported.

**Deferred:**

- FAT32/exFAT enablement, if the owner prefers a separate PR. The evidence is ready.
- NAS/Samba qualification.
- OneDrive qualification.
- The read-only-with-grade state.
- A physical SD-card and surprise-removal check.
- Full Windows application/GUI validation.
- Long paths.

---

## Reference implementations (S, from earlier pinned research)

- **Qt `QSaveFile`** uses bare `MoveFileEx(MOVEFILE_REPLACE_EXISTING)` with an inherited temp ACL. On Windows its `setPermissions` is only `_wchmod`, which toggles the read-only bit, so it preserves no DACL.
- **CPython `os.replace` and Go `os.Rename`** use the same primitive.
- **Chromium** uses `ReplaceFile` to preserve metadata, but needs backup and recovery machinery because of `ReplaceFile`'s partial-failure states.

The minimal writer sits between them. It copies the DACL and visible attributes, the security-meaningful parts that Qt drops, without `ReplaceFile`'s failure modes.

---

## G. Verdict

**READY TO PROCEED INTO PRODUCTION** with the minimal writer for local NTFS. The evidence also supports local FAT32/exFAT, subject to the owner's scope decision.

Before Windows persistence is described as generally supported, the owner should decide:

- new-file permissions;
- the UTF-8 manifest;
- read-only viewing of grades.

Network, cloud, ReFS and physical removable-media behaviour remain untested and should stay refused or unclaimed.
