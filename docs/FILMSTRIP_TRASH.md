# Move selected photographs to Trash

macOS/Linux native Trash was introduced in merged PR #116. Windows interactive
recycling is implemented in [draft PR #122](https://github.com/ryancara/RawNode/pull/122),
with Claude's independent review reported **SAFE TO MERGE**. The PR is open and
unmerged pending the owner's manual validation of the updated build. All three
platforms share selection, confirmation, sidecars, document lifecycle and refresh. There
is no catalogue, culling mode, filesystem Undo or new filmstrip control.
Windows may offer explicit permanent-deletion confirmation; RawNode never
approves it automatically. See [implementation and native evidence](WINDOWS_TRASH_RESEARCH.md).
The native macOS GUI/restore checklist below remains separate from selection validation.

## Interaction and visible changes

Only unmodified Delete or Backspace, without key repetition, requests Trash in
the focused Filmstrip window. Shift, Control, Command/Super and Alt combinations
do not trigger photo Trash. GLFW maps the physical macOS Delete key to Backspace; both keys
are accepted. Graph/List retain their node-deletion bindings. Active controls,
text capture, open popups/modals and absent/invalid selection block the action.

On supported platforms, one concise native confirmation uses singular/plural
wording and shows the eligible photograph count, total selection and known-ineligible count when
nonzero, eligible photographs hidden by mark/type filters, and sidecar inclusion.
Stale, missing, invalid and unrelated paths are not counted as hidden photographs.
For a mixed selection it distinguishes selected items from eligible photographs;
those already known to be ineligible remain in the snapshot for failure reporting.
On Windows, the existing confirmation also warns that native permanent-deletion
prompts may follow when recycling is unavailable. Cancelling this application
confirmation, dismissing it or an unavailable dialog helper performs
no edit completion, grade save, refresh or Trash operation. A value snapshot of
paths and non-symlink file identities is captured before opening confirmation;
subsequent selection/filter changes cannot substitute other photographs.

Confirm processes that complete snapshot, including hidden selections. The final
status reports recycled photographs, explicitly confirmed permanent deletions,
selected-item failures and cancellation separately. Errors or permanent deletions
also open a result dialog, with exact permanently deleted/remaining-sidecar paths
and native errors. Sidecar failures do not count as failed photograph moves.
This feedback is necessary to expose
non-atomic outcomes; no toolbar/layout/theme changes are included.

## Filesystem and sidecars

Each source must still be a supported regular, non-symlink photograph directly
in the same filmstrip folder, with the identity/size/modification time captured
at confirmation. The folder identity must still match. Replaced, missing,
symlink/reparse and unrelated paths are refused. There is no recursive traversal
or similarly-named-file search. Native path APIs cannot provide a transaction
against an uncooperative external process replacing files between the final
check and the native operation. Concurrent external editing remains outside the
single-session contract; these checks reject observed changes, not promise a
filesystem-wide lock.

Request native recycling of the photograph first. If it fails, leave both
sidecars alone and continue with independent photographs. On native cancellation,
stop remaining Trash operations. Only after recycling or explicitly confirmed
permanent deletion, consider the two existing naming helpers' exact appended paths:

- `photograph.ext.rawnode.json`;
- `photograph.ext.ofxrawhost.json` (the legacy contract also appends to the full
  source filename, so it is unambiguous even with same-stem RAW/JPEG pairs).

Missing sidecars are normal. Regular sidecars move as opaque bytes, including
malformed, newer-version, protected and read-only files. Trash does not parse,
migrate, overwrite or repair them. Symlink/reparse/nonregular sidecars are left
in place with their exact path reported. A native failure after photo success
also leaves and reports that sidecar. Later saves/navigation never overwrite or
recreate an orphan for the removed source. Photo/sidecar pairs and batches are
explicitly not atomic; there is no rollback framework. Cancellation during
sidecars leaves and reports the remaining exact sidecar paths. Completed
removals retain their outcomes; removed current sources still finish the normal
document retirement/refresh transition. A permanent-deletion choice for a photo
never authorizes automatic permanent deletion of its sidecars.

## Document, render and thumbnail ownership

Unopened-photo batches never complete current-photo UI edits, capture/save its
grade, change graph/history or request rendering. The current document and
preview owners remain intact.

When the snapshot includes the current photo, UI completion precedes the existing
photo-departure save policy. Retry / Discard / Cancel keeps its established
meaning; save Cancel aborts the whole batch. Protected viewing skips saving.
A changed current source is rejected before saving onto a substituted source,
including a replacement while the save-failure dialog is open before Retry.
An active export refuses the whole current-photo batch; unrelated photos can
still move during export.

The current-photo batch enters the existing `DocumentMutation` boundary to drain
preview and hold execution exclusion across native movement and transition.
A successfully removed current source is cleared through DocumentActions inside
that same ownership contract: path/save protection, pixels, graph/history,
selection/inspection and presentation are retired. Its save identity is never
retained for shutdown or navigation. The existing prepared-open operation opens
the first decodable remaining photograph in folder order, independently of
filters, without another outgoing save. Its nested DocumentMutation is intentional:
mutation depth keeps preview dispatch closed until the outer Trash scope ends;
current-source clearing asserts that the outer mutation owns this App. If none opens, the Source-only empty
editor remains usable. A failed current-photo move normally retains its grade,
history and preview; if the native call reports failure but the source has
actually disappeared, detach it conservatively and report the failure.

Refresh uses the existing scan/pruning path, preserves mark/type filters and
surviving thumbnail textures/state, removes moved paths from selection, retains
failed paths still present, and clears missing anchors. Clearing thumbnail jobs
advances their generation under the queue mutex; obsolete workers publish
nothing, including cancellation notifications for a new same-path job.

## Native support and build requirements

`platform/FileTrash` exposes `fileTrashSupported()`, `moveFileToTrash(path, error)`
and no-follow identity inspection. `TrashOutcome` is Trashed, PermanentlyDeleted,
Cancelled or Failed. Native recycling is requested on all platforms. Windows
may offer its own permanent-deletion warning; RawNode never answers Yes for the
user. The capability is true on macOS/Linux/Windows and false on fallback
platforms, without promising that every volume accepts recycling. Unsupported
platforms still refuse before confirmation, UI completion or departure.

- macOS: Foundation `NSFileManager trashItemAtURL:resultingItemURL:error:` in an
  Objective-C++ source, compiled only on Apple. Existing Cocoa linkage supplies
  Foundation. Unsupported volumes return the native error.
- Windows: `IFileOperation` with recycle/warning flags and no automatic
  confirmation. A small sink observes actual item disposition and refuses when
  Windows supplies no recycle intent, because disabled bins can otherwise delete
  silently. The native API owns permissions, storage compatibility and capacity.
  The control thread uses an STA COM apartment; Windows SDK COM/Shell linkage is
  the only build addition. See [configuration and evidence](WINDOWS_TRASH_RESEARCH.md).
- Linux: GIO `g_file_trash()`, with required `gio-2.0` through pkg-config
  (`libglib2.0-dev` on Debian/Ubuntu, added to Linux CI only). GIO mount policy,
  filesystem permissions and desktop Trash availability may refuse a move;
  the error is reported. No GIO dependency is added to macOS or Windows.

API references checked before implementation:
[Apple Trash](https://developer.apple.com/documentation/foundation/filemanager/trashitem(at:resultingitemurl:)),
[Microsoft flags](https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nf-shobjidl_core-ifileoperation-setoperationflags),
[pre-delete veto](https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nf-shobjidl_core-ifileoperationprogresssink-predeleteitem),
[per-item results](https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nf-shobjidl_core-ifileoperationprogresssink-postdeleteitem),
[cancellation](https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nf-shobjidl_core-ifileoperation-performoperations),
[GIO Trash](https://docs.gtk.org/gio/method.File.trash.html).

## Acceptance and automated evidence

The supplied feature request defines acceptance. Independent expected results
are literal path sets/order, byte-identical sidecars, zero native operations
after cancellation (none at all for application-confirmation Cancel), unchanged
document/preview/history/runtime identities for
non-current batches, and no remaining save target for a removed current source.
These expectations and the proportionate design were recorded before coding.

`--selftest-trash` covers controlled batch/native-failure seams;
`--selftest-photo` adds real ImGui input, confirmation and pending-edit completion.
Both are included in full `--selftest`. Test moves only rename test-owned files
inside their own temporary fixtures; self-tests never call the real OS Trash.
Barrier tests exercise actual preview/thumbnail workers without sleeps.
`tests/WindowsTrashTests.cpp` is a separate native Windows fixture driver,
compiled with the production adapter by Windows Trash safety CI. Its script
checks real bin entries, native restore and hashes, disabled bins, oversized
warnings, No/dismissal, locked files and missing paths on disposable virtual
volumes. Its dialog monitor never approves a prompt. Full Windows application
CI also runs shared lifecycle and ImGui keyboard tests; native GUI checks remain
separate. The shared regressions simulate all four outcomes using test-owned
renames, including cancellation after photo removal and during sidecars. They
check literal call order, preserved unprocessed files, separate permanent-delete
reporting and retirement of a removed current source after cancellation.

Coverage includes single/multiple and hidden selection; immutable confirmation
and Cancel; real Graph/List/text/active-control/modal focus; exact current/legacy,
missing, malformed/newer/protected/read-only/symlink sidecars; photo failure and
post-photo sidecar failure; missing/replaced sources/folders; current versus
non-current transitions; Retry / Discard / Cancel; export source refusal;
last-photo/failed replacement opening; history/save/presentation cleanup;
retained thumbnails, queue/anchor/selection cleanup and stale worker rejection. Modifier regressions exercise every nonempty
combination of Shift/Ctrl/Alt/Super with both keys under both ImGui native
mappings. Exact-text cases distinguish eligible/hidden/ineligible counts and
singular/plural wording while preserving the execution snapshot. The obsolete
cancelled-result enum/consumer was removed: no producer remains; generation
mismatches already discard results before publication.

Exact-head Release/sanitizer/CI outcomes and the scope/readability audit are
recorded in the draft PR and implementation report. Native GUI verification
below is separate from headless tests and CI compilation.

## Remaining native macOS checklist

Use only disposable photographs and sidecars in a temporary folder. Keep original
copies for comparison; never test on a user's photo collection.

1. Verify unmodified physical Delete/Backspace, held-key behaviour, refusal for
   Shift/Ctrl/Command/Alt combinations and one confirmation in
   docked/floating filmstrips. Check total/hidden counts under both filters.
   Check mixed invalid/stale selections are identified separately and excluded
   from hidden-photo counts. Cancel must preserve files, selection, current
   document and pending edits.
2. Move one and multiple photographs, with missing/current/legacy sidecars,
   malformed/newer/read-only sidecars, Unicode/spaced names and same-stem RAW/JPEG
   pairs. Inspect the native Trash, original-path information and
   exact contents; restore every disposable file successfully.
3. With photo selection retained, delete nodes via both keys in Graph and List,
   edit text, use search/filter controls and another modal. No photo confirmation
   or filesystem operation may occur.
4. Trash a non-current selection while grading/exporting the current photo:
   preserve document, history, preview and export. Include the current photo:
   refuse during export; after export, complete pending numeric/handle edits,
   save once via Retry / Discard / Cancel, then transition safely. Save Cancel
   must prevent every batch move.
5. Inject an unwritable sidecar and native photograph failure. A failed photo
   retains its sidecars/selection. After a successful photo move with sidecar
   failure, verify the exact remaining path in the error report and unchanged
   orphan bytes through navigation, explicit Save, reopening and shutdown.
6. Remove the last photo, and remove a current photo whose remaining neighbours
   cannot decode. Verify an empty usable editor, cleared preview/history and no
   recreated source sidecar after navigation/restart. Refresh during active
   thumbnails and verify no removed/stale results return.
7. Check native Save Retry / Discard / Cancel, unsupported/read-only/external
   volumes, Retina/narrow/floating filmstrips and theme selection visibility.
   Confirm recovery in Finder. The completed PR #115 selection checks do not
   establish new Trash/lifecycle behaviour.

Windows native configuration, evidence and remaining desktop checks are recorded
in [Windows Trash research](WINDOWS_TRASH_RESEARCH.md) and
[the Windows validation record](WINDOWS_VALIDATION.md). A runner pass does not
complete independent review, Windows 10/11 desktop or physical external/card checks.
