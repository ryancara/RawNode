# Windows Trash: source and API review

**Research snapshot: 15 November 2026.** PR #116 remains draft and merge-blocked.
Its Windows deletion strategy is unchanged by the corrective pass. Do not use
that adapter on user photographs until the recoverability issue is resolved.

**Recommendation:** after owner agreement, temporarily refuse Windows Trash
operations with a clear unsupported-operation error. Keep the native macOS/Linux
paths. None of the implementations or Microsoft contracts inspected establishes
the required Windows guarantee in every requested configuration. Adding Qt/GIO
or restricting operations to local NTFS does not establish it. This refusal is a
recommendation, **not implemented in this pass**. Do not merge the unchanged
Windows adapter on the strength of this research or passing CI.

## Evidence and Microsoft contract

**D** = official documentation; **S** = inspected source at a pinned commit;
**T** = actual testing; **U** = unverified. No native Windows testing was performed
for this report. Linux/macOS CI, injected tests and Windows cross-compilation do
not establish Windows shell behaviour. No Explorer/Lightroom behaviour is claimed
from observation or proprietary implementation details.

| Microsoft API/flag | Documented meaning (D) and limit of the conclusion |
| --- | --- |
| [DeleteItem][delete] | Queues deletion; its successful HRESULT does not mean a file moved. Execution occurs in PerformOperations. |
| [FOFX_RECYCLEONDELETE][flags] | Windows 8+ flag says to send deleted files to the Recycle Bin rather than delete permanently. This is the documented intent; the page does not specify a complete fail-only contract for disabled/full bins, oversized files or every filesystem/provider. |
| [FOF_ALLOWUNDO][flags] | Preserves undo information **if possible**; fully qualified source names are required. It is not a no-permanent-deletion guarantee. |
| [FOF_WANTNUKEWARNING][flags] | Requests a warning for permanent deletion; partly overrides FOF_NOCONFIRMATION. A warning can still offer permanent deletion, so it is not an enforced refusal. |
| [FOF_NOCONFIRMATION][flags] | Answers Yes to All to dialogs. It is not a safety flag. FOF_NOERRORUI/FOFX_EARLYFAILURE affect reporting/stopping on errors, not proof of recoverability. |
| [PreDeleteItem][pre] / [TSF_DELETE_RECYCLE_IF_POSSIBLE][tsf] | Returning a failing HRESULT cancels the delete and subsequent operations. The flag means recycle **if possible**. Its presence is not a documented final-disposition guarantee. |
| [PostDeleteItem][post] | Reports actual per-item results. Flags can change during deletion; a non-null new item identifies the recycled item, while null can mean permanent deletion. Checking this afterwards cannot prevent already-lost data. |
| [PerformOperations][perform] / [GetAnyOperationsAborted][aborted] | Successful HRESULTs can accompany cancellation. Query aborted status after either success or failure; inspect individual outcomes for partial completion. The [legacy SHFileOperation][legacy] also requires its aborted flag, even after return value zero. |

**Finding:** the reviewed documentation does not establish an end-to-end flag /
validation / callback combination guaranteeing “recycle or leave recoverable”
for all the requested cases. The RECYCLEONDELETE wording is stronger than
ALLOWUNDO, but neither a pre-operation intent flag nor post-operation detection
fills that documentation gap. This is **not** a claim that permanent deletion
was reproduced here, or that every Windows version necessarily falls back.
The current adapter's stronger guarantee was not substantiated.

[ITransferSource::RecycleItem][recycle-item] also describes recycling into a
provided recycle location, with dialog/status results including ignored/pending
work. The reviewed material does not establish a ready replacement covering
per-user/per-volume location acquisition and all failure cases. It remains
unverified, not a selected implementation.

## Established implementations inspected (S)

Links below pin the exact source examined; upstream usage is not a safety proof.
No protective hard-link/backup mechanism was found in these Windows Trash paths.
Qt's documented hard-link-based implementation refers to **Unix**, not Windows.

| Project and pinned source | Actual mechanism and failure/cancellation handling | Bin-unavailable, drive support and limitations |
| --- | --- | --- |
| **darktable** [`da609a5` Windows helper][dt-win], [caller][dt-jobs] | SHFileOperationW, FO_DELETE, ALLOWUNDO + SILENT + NOCONFIRMATION. Checks return status and aborted bit. Caller can explicitly offer permanent deletion after a reported failure. | No strict per-item recycled-object verification or volume allowlist in the helper. Delegates local/external/network decisions to the shell; silent Yes-to-All is not a fail-closed proof. Its explicit permanent-delete fallback is outside RawNode's scope. |
| **digiKam** [`33d0457` DTrash][dk-trash], [job/restore][dk-job] | Collection-owned `.dtrash/files` plus JSON `.dtrashinfo`; QFile::rename moves files. Trash failure reports an error and continues, without entering the separate permanent-delete branch. Cancellation is checked between job items. | OS Recycle Bin settings are irrelevant. Collection/root must support the required directory/metadata writes and movement; external/network support depends on collection/filesystem setup. Metadata creation and movement are separate operations. Reusing this would introduce the custom trash/catalogue machinery RawNode excludes. |
| **RawTherapee** [`64c2d90` browser][rt-browser], [thumbnail flag][rt-thumb], [empty/delete][rt-catalog] | “Trash” sets a persisted virtual flag; it does not move the original file to the Windows bin. Empty Trash invokes a separately confirmed g_remove-based permanent-delete path; return values are not checked in the inspected deletion loop. | Original bytes stay in place while merely marked. No OS-bin capability/volume handling exists for that gesture. This is a different workflow, not an implementation of RawNode's filesystem Trash action. |
| **Qt QFile::moveToTrash** [`619152d` Windows engine][qt-win], [QFile wrapper/docs][qt-file] | IFileOperation with ALLOWUNDO, RECYCLEONDELETE, NOCONFIRMATION, SILENT, NOERRORUI. PreDeleteItem rejects absent recycle intent; PostDeleteItem records HRESULT/new location. | Closely resembles RawNode's approach, not independent protection. The inspected method does not query aborted state or require a non-null recycled item for success; SetOperationFlags' result is unchecked. QFile::supportsMoveToTrash expressly does not check disabled settings. External/network and capacity decisions remain shell-dependent and untested here. |
| **GLib/GIO Windows** [`75329cd` glocalfile.c][gio-win] | SHFileOperationW, FO_DELETE, ALLOWUNDO. No silent/auto-confirm flags in this path; checks return/aborted status and maps cancellation. | May show shell dialogs; no recycled-object verification or unavailable-bin refusal guarantee. No special external/network allowlist. Linux g_file_trash behaviour does not establish the Windows implementation's safety. |
| **Explorer++** [`94ca535` FileOperations::DeleteFiles][explorer] | IFileOperation/DeleteItems; non-permanent mode sets ALLOWUNDO. Interactive mode adds WANTNUKEWARNING; silent mode adds NOCONFIRMATION/SILENT/NOERRORUI. This helper returns PerformOperations' HRESULT without an aborted/per-item check. | General file manager deliberately supports permanent deletion and shell warning choices. Delegates drive/bin policy to Windows; its helper alone cannot prove cancellation or strict recycle-only success. |
| **Send2Trash** [`a0b1625` modern backend][s2t-modern], [sink][s2t-sink], [legacy backend][s2t-legacy] | Modern IFileOperation checks global result and aborted state; uses recycle/undo flags and an intent-checking sink. Legacy SHFileOperation checks nonzero return, but not the aborted bit on zero. | The Python sink itself notes that returning an error may not stop the operation through its binding; do not generalise that binding limitation to native C++. Neither path establishes strict unsupported-volume protection. No independent backup. |

The listed limits are observations of those source paths, not reproduced Windows
bugs or a claim to have audited each whole application. For all shell-based rows,
behaviour with disabled/full bins, oversized files, removable media and UNC/
mapped shares remains **U** here. No reviewed implementation supplies a portable
recoverability guarantee for those cases that RawNode can simply inherit.

## Options for RawNode

| Option | Recoverability / disabled bin | External/network support | Dependencies and maintenance |
| --- | --- | --- | --- |
| **A. Adjust native shell flags/callbacks** | Good intent and outcome reporting; strict fallback prevention remains U. WANTNUKEWARNING is a prompt, not refusal; post-delete checks are too late. | Requires proof for each supported provider/volume/policy. | No new dependency; small code change, but the central safety claim remains unresolved. Do not approve merely from passing normal-bin tests. |
| **B. Adopt Qt or Windows GIO** | Inspected implementations do not add the missing guarantee. | Shell-dependent; not established here for unavailable bins or remote/removable files. | QtCore is a substantial new dependency; Windows GIO is also new platform baggage. Neither is justified by a proven safety improvement. digiKam/RawTherapee workflows are outside scope. |
| **C. Only local NTFS** | NTFS supports required filesystem primitives, but users/policy can still disable the bin or impose capacity limits. Restriction alone does not guarantee recycling or refusal. | Excludes UNC/mapped shares and non-NTFS media; NTFS on an external drive is not proof of bin availability. | Small volume check, insufficient safety by itself. Registry/bin-capacity preflight is not an atomic reservation and should not become a new policy subsystem. |
| **D. Protective hard link or full backup** | A retained NTFS hard link can preserve bytes after the original name is deleted (D), but the shell result still needs verification before releasing it. A copy can protect other filesystems at significant I/O/storage cost. Neither has been designed/tested for RawNode. | Hard links require same-volume NTFS files; do not promise network/removable support. A copy still requires writable space and durable recovery rules. | Collision-safe creation, identity checks, failed restore, crashes, orphan cleanup, preserved metadata/streams and user-visible recovery all need an approved contract. This can grow into the recovery machinery we are trying to avoid. |
| **Preferred interim policy: refuse Windows Trash** | No destructive OS call, so disabled/full/unavailable bins cannot cause data loss. | Clear refusal on every Windows volume until a supported case is justified. | No new dependency or filesystem-management state; smallest safe interim behaviour. Requires owner agreement and a later implementation; unchanged adapter still blocks this PR. |

[CreateHardLinkW][hardlink] and [Hard Links and Junctions][hardlinks-overview]
document NTFS file links, same-volume constraints and retention until links are
removed. A hard link is **not an independent
content backup**: other writers can change the shared data. Crash before result
verification must retain protection; restoring the original name must never
overwrite a file created in the meantime. Failed cleanup must report the retained
path. Those obligations make D more than an extra create/delete pair; do not
prototype it in production without an agreed recovery design.

## Required native Windows evidence before enabling an implementation

Use disposable fixtures, saved byte/stream/metadata references and temporary
volumes/shares only. Record Windows build, filesystem, drive type and bin policy.

1. Enabled bin: small/large RAW + current/legacy sidecars, Unicode/long names,
   actual bin entry/original path, and successful byte-identical restore.
2. Disabled bin via per-drive setting/policy, full/tiny bin, a single oversized
   file, unavailable/corrupt bin, disk full, denied/locked/read-only files.
   A rejected operation must leave each original or an explicitly retained,
   verified recovery copy; no silent permanent deletion is acceptable.
3. Fixed and external NTFS, FAT/exFAT/removable media, UNC and mapped SMB shares,
   disconnected shares, reparse points and cloud placeholders. Unsupported cases
   must refuse before risking the source.
4. Cancel/skip and partial completion, including successful aggregate HRESULTs
   with aborted work; record pre/post flags and actual per-file disposition.
   Post-operation observation is evidence, not prevention.
5. If D is ever approved, inject crashes at every protection/OS-call/verification/
   restore/cleanup boundary, filename collisions and changing originals. Verify
   recovery without overwriting unrelated files and that no protection is removed
   before verified recoverability. No such strategy was implemented/tested here.

**Merge blocker:** resolve Windows recoverability by an approved, validated
implementation or an approved explicit refusal. The current Windows strategy
remains unchanged. Part 1 tests/CI validate only the corrections and established
macOS/Linux/shared behaviour.

[delete]: https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nf-shobjidl_core-ifileoperation-deleteitem
[flags]: https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nf-shobjidl_core-ifileoperation-setoperationflags
[tsf]: https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/ne-shobjidl_core-_transfer_source_flags
[pre]: https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nf-shobjidl_core-ifileoperationprogresssink-predeleteitem
[post]: https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nf-shobjidl_core-ifileoperationprogresssink-postdeleteitem
[perform]: https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nf-shobjidl_core-ifileoperation-performoperations
[aborted]: https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nf-shobjidl_core-ifileoperation-getanyoperationsaborted
[legacy]: https://learn.microsoft.com/en-us/windows/win32/api/shellapi/nf-shellapi-shfileoperationw
[hardlink]: https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-createhardlinkw
[recycle-item]: https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nf-shobjidl_core-itransfersource-recycleitem
[dt-win]: https://github.com/darktable-org/darktable/blob/da609a5b641c4af4dca52277e14dd62069d89b44/src/win/dtwin.c#L348-L387
[dt-jobs]: https://github.com/darktable-org/darktable/blob/da609a5b641c4af4dca52277e14dd62069d89b44/src/control/jobs/control_jobs.c#L1311-L1407
[dk-trash]: https://github.com/KDE/digikam/blob/33d0457e20adda97c003f3dee652a1749406ff9f/core/libs/dtrash/dtrash.cpp#L44-L110
[dk-job]: https://github.com/KDE/digikam/blob/33d0457e20adda97c003f3dee652a1749406ff9f/core/libs/iojobs/iojob.cpp#L190-L248
[rt-browser]: https://github.com/RawTherapee/RawTherapee/blob/64c2d90a3248c7ad41842d9162368858b61aac6a/rtgui/filebrowser.cc#L1626-L1650
[rt-thumb]: https://github.com/RawTherapee/RawTherapee/blob/64c2d90a3248c7ad41842d9162368858b61aac6a/rtgui/thumbnail.cc#L1286-L1294
[rt-catalog]: https://github.com/RawTherapee/RawTherapee/blob/64c2d90a3248c7ad41842d9162368858b61aac6a/rtgui/filecatalog.cc#L1033-L1079
[qt-win]: https://github.com/qt/qtbase/blob/619152dc9f9825c9bcd49b633c820c3a9e351a98/src/corelib/io/qfilesystemengine_win.cpp#L1830-L1877
[qt-file]: https://github.com/qt/qtbase/blob/619152dc9f9825c9bcd49b633c820c3a9e351a98/src/corelib/io/qfile.cpp#L490-L565
[gio-win]: https://github.com/GNOME/glib/blob/75329cdee80aeca525b50f88ef1dcab070f8b95d/gio/glocalfile.c#L2590-L2635
[explorer]: https://github.com/derceg/explorerplusplus/blob/94ca53592de1c60fea76713ded62545bffec31c4/Explorer++/Helper/FileOperations.cpp#L48-L124
[s2t-modern]: https://github.com/arsenetar/send2trash/blob/a0b16254fdb56d3df7363f52ea47f345e7a25ebd/send2trash/win/modern.py
[s2t-sink]: https://github.com/arsenetar/send2trash/blob/a0b16254fdb56d3df7363f52ea47f345e7a25ebd/send2trash/win/IFileOperationProgressSink.py
[s2t-legacy]: https://github.com/arsenetar/send2trash/blob/a0b16254fdb56d3df7363f52ea47f345e7a25ebd/send2trash/win/legacy.py#L139-L190
[hardlinks-overview]: https://learn.microsoft.com/en-us/windows/win32/fileio/hard-links-and-junctions
