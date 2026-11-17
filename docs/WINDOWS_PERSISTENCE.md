# Native Windows persistence

Photograph grades and Pick/Reject metadata, preset files and global preferences
share `persist_detail::replaceFileAtomically()`. `persist/WindowsFile` owns only
native file I/O and the non-mutating write-capability check. The JSON schema,
untouched-section preservation, processor availability and document lifecycle
remain with their existing owners. The POSIX writer is unchanged.

## Contract and publication

Publish complete JSON contents atomically; preserve the destination's contents
and meaningful permissions on ordinary failure; report a useful error. This is
not a promise to preserve the entire filesystem object, provide a multi-process
transaction, or survive every power loss. Stable directories and the existing
single-session editing contract still apply.

The writer follows the parent directory once and qualifies its real filesystem.
It inspects the final component without following reparse points and checks
`FILE_WRITE_DATA` without truncation: rename permission alone can bypass a file's
write denial. Read-only, non-regular, final-component reparse and offline files
refuse saves. `FileBasicInfo` works on the supported filesystems; NTFS-only
attribute queries are not used for FAT files.

A random sibling is reserved exclusively with `CreateFileW(CREATE_NEW)`. Writes,
`FlushFileBuffers` and handle closure are checked. One `MoveFileExW` publishes
with flags 0 for creation or `MOVEFILE_REPLACE_EXISTING` for replacement. There
is no copy, unlink-first, permission-changing retry or alternate-API fallback.
Failure cleanup touches only the temporary this operation created. A cleanup
failure reports its path and Windows code, without silently retrying removal.
Incompatible open readers can cause safe refusal, including some readers with
delete sharing. Closing a reader is not a reason to weaken the save algorithm.

## Deliberate metadata policy

- **New files inherit ordinary folder permissions.** This Windows product policy
  intentionally differs from POSIX `mkstemp`'s 0600 creation policy. It allows
  ordinary shared folders to work without a second inheritance-management system.
- **Existing DACLs and inheritance protection are copied as opaque ACLs** when
  `FILE_PERSISTENT_ACLS` is available. No owner/group matching or ACE whitelist is
  needed. Replacement ownership follows ordinary Windows creation semantics.
- **Existing restricted contents start in a creator-only protected temporary.**
  The final DACL is applied only after writing completes. Explicit integrity
  labels are applied before the first byte, including to partial leftovers.
  This avoids exposing restricted contents through a more permissive folder.
- **Integrity restrictions are retained.** The research demonstrated a medium
  `NO_READ_UP` label that allowed saving but denied low-integrity reads. DACL-only
  replacement removed that denial. Copying the label preserves it; failure to
  inspect/apply it refuses saving. Other returned specialised enforcement ACEs
  are refused; RawNode does not manage enterprise security policy or privileges.
- **Auditing is not an access-permission contract.** Inherited auditing follows
  normal sibling creation. Explicit per-file audit entries may be lost. Empty
  filtered SACLs are accepted; they no longer cause a first-save/second-save trap.
- **Encryption must not become plaintext.** An encrypted destination in an
  unencrypted folder is refused. For an encrypted folder, the created sibling
  must actually be encrypted before contents are written. No encryption fallback.
- **HIDDEN, SYSTEM and NOT_CONTENT_INDEXED are preserved.** ARCHIVE may become set
  after modification; a cleared ARCHIVE bit is not a reason to refuse a save.
- **Ownership, hard links, timestamps, compression and incidental metadata use
  ordinary replacement semantics.** Other hard-link paths retain the old file.
  Arbitrary alternate streams, including Zone.Identifier and provider metadata,
  are outside this JSON-data contract and may be lost. RawNode does not execute
  sidecar JSON; this decision does not extend to executable or active documents.

The POSIX implementation likewise replaces a pathname with a newly created
sibling, without preserving ownership, hard-link identity, timestamps, ACLs or
extended attributes generally. Windows adds DACL copying and the writable probe
because native experiments demonstrated meaningful access changes without them.
It does not grow into a complete metadata-preservation system.

## Filesystems, paths and viewing

Local **NTFS, FAT32 and exFAT** are qualified by native production regressions.
ACL operations are skipped on volumes without persistent ACLs. Ordinary directory
junctions are allowed when their followed target is qualified, including junctions
to another supported local filesystem. Fixed/removable classification is not a
restriction; disposable VHD tests do not establish physical-device reliability.

Network/NAS/SMB persistence, ReFS and provider-specific cloud semantics remain
unqualified. Network handles and reparse/offline endpoints are refused. There is
no directory-name blacklist or cloud-provider detection framework: an ordinary
local file can pass these checks even in a synchronised folder, without making
any promise about the provider's synchronisation, hydration or remote replicas.
Long-path and unusual device-namespace compatibility is not independently
qualified. Native failures remain errors, with no destructive fallback.

Reading does not require a writable or qualified location. A valid saved grade
can be restored and viewed there using the existing protected-document state;
existing restrictions on editing, capture/export and saving apply; no initial
sidecar is created, and departure
does not repeatedly prompt. Known Pick/Reject metadata stays visible. Invalid,
incompatible and legacy sidecars retain their existing source-only protection.

Application paths use a **UTF-8 activeCodePage manifest**, verified at startup
with `GetACP() == CP_UTF8` (Windows 10 1903 or later). Without it RawNode refuses
to run with mixed path encodings. Persistence converts strict UTF-8 to UTF-16;
preferences read the wide Windows environment. Discovery emits UTF-8; all three
LibRaw entry points use its native wide-path overload on Windows. A manifest
alone did not fix LibRaw's narrow RAW opening. Invalid UTF-8 and embedded NULs
are refused, as are named-stream destinations.

Photographs and version-1 sidecars remain portable between directories, machines
and user accounts subject to actual permissions and processor availability. No
machine identifiers or schema changes were added. Missing processors retain
opaque parameters and remain unavailable; CTL asset relocation is outside scope.

## Evidence and limits

The original feasibility probe/report are pinned at `20f820ec` / `f93327a5`, run
[43985082057](https://github.com/ryancara/RawNode/actions/runs/43985082057).
The simplification follows independent second-round reports:

- [Codex, 5231dec9](https://github.com/ryancara/RawNode/blob/5231dec9f695dc16c49e6f1a4d0561d398807193/research/windows-minimal-codex/REPORT.md),
  [native run 44109872879](https://github.com/ryancara/RawNode/actions/runs/44109872879).
- [Claude, f56cad8](https://github.com/ryancara/RawNode/blob/f56cad8/research/windows-minimal-persistence/REPORT.md).

Research probes are evidence, not substituted production implementations.
`tests/windows-persistence` compiles actual production persistence, codecs,
preferences, discovery and PNG/RAW decoding with MSVC C++17, the pinned LibRaw,
and the application's UTF-8 manifest. Native tests include an ordinary user,
foreign/stale owners, ACL/integrity/encryption, sharing failures, cleanup,
independently inspected failure snapshots, real disk-full volumes, and accented,
CJK and supplementary PNG/DNG paths. The permanent café/cafÃ© collision checks
that only the selected photograph's sidecar changes. The headless component
suite does not establish full Windows application/GUI compatibility.

Linux Release and sanitizer suites and macOS CI retain full application self-test
coverage, including transferred grades, missing processors and protected viewing.
The PR validation record identifies exact commits, native runs and any full
Windows build/integration limits. Finite tests cannot establish exhaustive rename
failure or power-loss safety. Windows Trash remains separately disabled.

A full MSVC application build was attempted at `13c91ba3` in
[run 44197564348](https://github.com/ryancara/RawNode/actions/runs/44197564348)
(artifact `windows-full-application-attempt`). It configured and built dependencies,
but application compilation found existing graph/history `std::map` no-throw
assertion failures, Windows min/max macro and OpenGL declaration issues, and a
surrogate-escape test-literal portability error. These require a separate Windows
application-port review; this PR does not weaken lifetime assertions to obtain a
build. No full Windows application or GUI validation is claimed.
