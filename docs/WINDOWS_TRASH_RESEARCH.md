# Windows Trash: implementation and native evidence

## Current contract

[PR #122](https://github.com/ryancara/RawNode/pull/122) implements the
owner-approved interactive contract from main `167888a`: request native recycling;
Windows may offer its own explicit permanent-deletion warning. RawNode never
approves that warning automatically. A user who explicitly chooses permanent
deletion can lose the file irreversibly. This supersedes the earlier requirement
that even offering permanent deletion was forbidden. The owner reports Claude's
independent review as **SAFE TO MERGE** and successful Windows desktop validation,
including multi-photo Trash, external HDD/SSD, dialog ownership and cancellation.
The owner authorized merge. SD card testing is unverified due to unavailable
hardware and is explicitly not a merge blocker; see the
[validation record and build provenance](WINDOWS_VALIDATION.md).

Selection, application confirmation, departure saving, photo-first sidecars,
current-document retirement and refresh remain one shared workflow. The shared
`TrashOutcome` distinguishes Trashed, PermanentlyDeleted, Cancelled and Failed.
Results count recycled and permanently deleted photographs separately, identify
all permanently deleted photo/sidecar paths, and report remaining sidecars.
Cancellation stops further native operations without rolling back completed work.
See [the shared behavior contract](FILMSTRIP_TRASH.md).

## Minimal native implementation

Windows uses `IFileOperation` on the control thread in a single-threaded COM
apartment. A UTF-8 path becomes a wide filesystem path and Shell item. Each call
operates on one exact file; the shared workflow retains photo-before-sidecar
ordering. The adapter sets:

- `FOFX_RECYCLEONDELETE`: request recycling;
- `FOF_WANTNUKEWARNING`: request explicit permanent-deletion warnings;
- `FOF_NO_CONNECTED_ELEMENTS | FOF_NORECURSION`: act on the supplied file only;
- `FOF_SILENT`: hide progress UI, which does not suppress confirmation;
- `FOF_NOERRORUI | FOFX_EARLYFAILURE`: return ordinary errors to shared reporting.

**Never set `FOF_NOCONFIRMATION`.** It answers Yes to dialogs. Native No/dismissal
returns Cancelled. A small progress sink observes the per-item HRESULT and
recycled Shell item; it accepts successful HRESULTs other than `S_OK`. An absent
recycled item is classified as permanent deletion only after successful item
completion and observed removal of the source. Explicit per-item skip/cancellation
is handled first; verified permanent deletion takes precedence over aggregate
cancellation. Aggregate status and `GetAnyOperationsAborted` are also collected;
ordinary errors are not all labelled user cancellation.

One pre-delete refusal is necessary: if Windows does not supply
`TSF_DELETE_RECYCLE_IF_POSSIBLE`, return an unsupported-operation error. The first
exact native run demonstrated that a disabled bin bypassed even WANTNUKEWARNING
and permanently deleted its disposable fixture without UI. The refusal uses
Windows' own operation flags, with no registry inspection or capacity/volume
preflight in production. Oversized files can retain recycle intent and therefore
still reach Windows' native warning; the callback is not a capacity guarantee.

macOS retains Foundation `trashItemAtURL:resultingItemURL:error:`. Linux retains
GIO `g_file_trash()`. Both map native success/failure to the shared outcome. No
new dependency, custom Trash, backup, hard link, allowlist, rollback or recovery
system is introduced. `ITransferSource::RecycleItem` is not used: supplying a
recycle destination adds work without a demonstrated benefit for this contract.

## Native validation

`Windows Trash safety` compiles `tests/WindowsTrashTests.cpp` with the exact
production `FileTrash.cpp` using native MSVC. The existing full Windows application
CI separately runs the shared lifecycle/UI regressions and produces the portable
Release ZIP. The ordinary application self-tests never call real Trash.

The native fixture script creates three disposable 128 MiB fixed NTFS VHDs:
enabled bin (64 MiB quota), disabled bin, and a 1 MiB quota. Settings are written
only for those newly created, identity-checked volume GUIDs, then removed during
cleanup. This is test setup, not an application policy subsystem. Fixture bytes
are synthetic; a 12 MiB `.dng` exercises photography-sized native filesystem work
without introducing another RAW decoder test.

Independent checks cover:

1. A 12 MiB source and current/legacy sidecar names, including Unicode paths:
   source disappears, one native bin entry has the original folder, Shell
   `undelete` restores to the original path with identical SHA-256, bin entry gone.
2. Disabled bin: adapter fails, original hash unchanged, no bin entry.
3. Oversized file: explicit permanent-deletion warning captured; No and dialog
   dismissal each cancel, preserving the original hash and producing no bin entry.
4. Locked source: failed outcome and unchanged hash. Missing source: failed
   outcome and no created file.

The driver can only click No/Cancel or close a recognized dialog belonging to
its own process. It never clicks Yes, OK or a default button. Unknown dialogs
or timeouts fail the test. Current-run evidence and portable build provenance
are recorded in the PR and [Windows validation record](WINDOWS_VALIDATION.md).

The first run on `62a18b0` exposed the disabled-bin omission described above:
[run 44650904869](https://github.com/ryancara/RawNode/actions/runs/44650904869).
Its other seven cases passed. It is regression evidence, not a passing build.
The corrected adapter at `718981ad7642cf47b131f523e3bcc5b80e8dc249` passed all
eight native cases on Windows Server 2022 build 20348:
[run 44651079955](https://github.com/ryancara/RawNode/actions/runs/44651079955),
[raw observations artifact](https://github.com/ryancara/RawNode/actions/runs/44651079955/artifacts/14591117345).
The artifact is retained for 14 days. It contains the adapter source hash,
environment, exact captured warning text, results, restore/source hashes and
test-volume settings. Later final-head runs are linked in the PR.

## Validation limits

Runner evidence uses Windows Server 2022 and fixed virtual NTFS disks. The owner
separately reports successful Windows desktop, physical external HDD/SSD, native
dialog ownership and cancellation checks. SD cards remain unverified because
hardware was unavailable; this is an accepted non-blocking limitation. Neither
result establishes network-share or provider/cloud behavior. Windows decides
storage compatibility, permissions and capacity; unsupported operations may fail.
Existing shared symlink/reparse/nonregular-file refusal remains. Concurrent
external replacement and atomic photo/sidecar groups remain outside the contract.

Independent review is reported complete. Remaining non-blocking implementation
findings are accepted without additional defensive state or test infrastructure.
Owner-reported manual validation and the supplied portable build identity are
recorded in [Windows validation](WINDOWS_VALIDATION.md). Explicit Yes is never
automated; permanent-deletion reporting is exercised through controlled shared
test outcomes. SD card tests can follow when hardware is available.

## Earlier research retained

The [2026-11-15 diagnostic report](https://github.com/ryancara/RawNode/blob/b6cbafdd3d0255897417ca898a6eb8b2975aa6e4/REPORT.md)
and linked raw traces document the old adapter's oversized-file silent deletion,
the separate successful-HRESULT reporting defect, and the No-response experiment.
The [source/API comparison at the previous main](https://github.com/ryancara/RawNode/blob/167888a6f5f9af37257d44df930391a4d01a15f4/docs/WINDOWS_TRASH_RESEARCH.md)
retains the pinned darktable, digiKam, RawTherapee, Qt, GIO, Explorer++ and
Send2Trash review. Its strict recycle-or-preserve policy is historical.

Microsoft references: [operation flags](https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nf-shobjidl_core-ifileoperation-setoperationflags),
[pre-delete callback](https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nf-shobjidl_core-ifileoperationprogresssink-predeleteitem),
[per-item results](https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nf-shobjidl_core-ifileoperationprogresssink-postdeleteitem),
[aborted operations](https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nf-shobjidl_core-ifileoperation-getanyoperationsaborted).
