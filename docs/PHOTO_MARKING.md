# In-editor photo marking

The initial Pick / Unmarked / Reject milestone extends the existing document
and filmstrip paths. It starts from main `5316a03` (merged PR #113).

## Behaviour and visible changes

- A compact Filmstrip row provides Pick, Unmarked and Reject for the current
  photograph, including one opened directly without a folder. Controls disable
  for no photo or an actively protected photo. This implements current-photo
  marking, without adding another selection model.
- Thumbnail footers show P / U / R; hover text gives the full mark. A `?` footer
  and an explanatory tooltip distinguish unreadable/unsupported sidecars from
  Unmarked. Indicators use existing theme text/background colours.
- Mark filter offers All / Pick / Unmarked / Reject alongside the existing
  All / RAW / Compressed tabs. Both predicates must match. All includes unknown
  marks; specific mark filters exclude them. Filters remain transient.
- Marking persists immediately and updates matching filmstrip entries only
  after success. If a new mark excludes the current photo, its thumbnail hides
  while the document stays open. The marking controls still address that photo.
- Thumbnail clicks retain `finishUiEdits` and `openPath` with the existing
  save-failure prompt. Marking never advances selection or alters source files.
- Failed marking leaves the last confirmed displayed mark intact and shows a
  persistent Filmstrip warning. A successful mark save or successful photo open
  clears that warning. Grade-save warnings remain independent.

These are the requested actions, indicators, filtering and minimum failure
feedback. No substantial workspace redesign or additional interaction is included.

## Responsibility and lifecycle

`assignPhotoMark(App&, photoPath, PhotoMark)` is the single public, control-thread
application action. UI and future assignable shortcuts can invoke it directly;
it accepts no widget or input event. It checks photo identity and active source
protection, then reuses `updateSidecarMark`. Test faults use the existing private
atomic-writer seam. It never captures a grade, enters `DocumentMutation`, changes
history or requests rendering. Marking an unopened photo constructs no processors
and does not decode it.

`readGraphSidecar` and the existing strict codec validate sidecars. `photoMark`
extracts a mark from that validated temporary record using the same JSON reader,
including escaped string values; absent metadata/mark means Unmarked. No second
format or serialization path exists. Atomic metadata writes preserve the exact
complete grade JSON, including opaque parameters and precise numeric tokens.
Metadata-only photos remain without grades until ordinary editor opening.

Each existing `FilmstripEntry` stores an optional mark and error text, solely on
the control thread. Unknown is represented by a disengaged optional. Folder
refresh discards old entries and reloads sidecars once; successful document
opening and mark actions synchronize the relevant entry. Per-frame filtering and
drawing use these snapshots. The thumbnail worker does not access mark state.
There is no document-level duplicate mark, worker, watcher or metadata service.
External concurrent editing remains outside D050's single-session contract.

Assignable bindings, hard-coded marking keys, multi-photo selection, batch
marking/export and rejected-file operations remain separate milestones.

## Acceptance evidence

The existing `--selftest` photo-document suite adds three focused groups:

- Public action calls on deliberately undecodable unopened RAW fixtures prove
  all three marks persist and restore without opening a document or creating a
  grade. A hand-authored grade checks exact section bytes across transitions,
  including nested opaque values, >53-bit integers, decimal precision, colours,
  topology and positions. Unsaved live grade edits and history size/cursor/bytes
  stay unchanged, as do runtime epoch, pending demand and preview ownership.
- Existing atomic fault injection covers read/permission/temp-create/write/
  flush/permission-copy/close/replace failures. Original bytes, directory entries,
  displayed marks, current selection and grade warnings remain intact. Invalid,
  newer-version, unsupported metadata, active protection, legacy-only,
  nonregular and read-only sidecars refuse marking.
- Mixed Pick/Unmarked/Reject/unknown entries exercise every mark/type filter
  combination through the production predicate. Ordinary document opening
  preserves mark metadata and save-failure navigation refusal. Explicit refresh,
  removed sidecars, new folders and empty folders reload the correct snapshot.

Expected marks come from explicit fixture literals and requested enum values;
untouched grade bytes are compared directly with the hand-authored input. The
runtime/history invariants are observed with existing test access rather than
inferred from successful serialization. Existing sidecar/lifecycle suites remain
part of every full run.

Local Release and sanitizer results and exact-head CI are recorded in the PR.

## Required macOS GUI checks

These require the native macOS application; Linux self-tests do not establish them.

1. Open a mixed RAW/compressed folder. Verify readable P/U/R footers and full
   mark/error tooltips in the normal docked filmstrip, Retina scaling and a
   resized/floating filmstrip. Confirm both filter controls remain usable.
2. Mark the current photograph Pick → Reject → Unmarked. Verify immediate
   sidecar metadata and restoration after folder reopening/application restart,
   with grade parameters, colours and node positions intact.
3. Combine each mark filter with RAW/Compressed. Click remaining thumbnails and
   verify normal photo loading. Change the current photo's mark so it disappears
   from the active filter: the viewer must retain that photo and the buttons must
   still target it. Restore All and confirm its mark.
4. Open an image directly without a folder. Verify all three marking buttons
   work and have no effect on grade Undo/Redo or the processed preview.
5. Exercise malformed/newer/legacy-only sidecars and protected source viewing.
   Confirm error indication, useful tooltip/warning and refusal without changing
   bytes. Use a read-only sidecar/folder to confirm a failed mark never displays
   success. Repair/reopen and retry successfully.
6. Retain an unsaved grade while marking; navigate, reopen and quit. Verify normal
   grade saving and native Retry / Discard / Cancel behaviour after a grade-save
   failure, including usable editing after Cancel. Reject must never delete,
   move or trash a photo or sidecar.
