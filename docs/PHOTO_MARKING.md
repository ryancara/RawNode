# In-editor photo marking

The initial Pick / Unmarked / Reject milestone extends the existing document
and filmstrip paths. It starts from main `5316a03` (merged PR #113).

## Behaviour and visible changes

- One borderless table row contains the existing All / RAW / Compressed tabs,
  current-photo Pick / Unmarked / Reject buttons and the separate mark filter.
  The default filmstrip height remains 96. Native tab scrolling handles narrow
  tab space without wrapping the toolbar. Thumbnails use the remaining height.
- The current photo's active mark uses the theme HeaderActive colour on its button,
  including direct-image opening and a photo hidden by the active filter. Unknown
  has no selected button. Controls disable for no photo or a protected photo.
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
  persistent inline `!` indicator with the full error in a hover tooltip; even a
  long error consumes no extra row. A successful mark save or successful photo
  open clears it. Grade-save warnings remain independent.

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
App also holds one transient `currentPhotoMark` optional for the current
photograph's controls. Successful document publication initializes it, including
Unknown for a protected source; a successful mark action changes it only when
the target identifies that current photograph. Failed opening/navigation/marking
leaves the current display intact. This presentation value is independent of
folder snapshots and never governs saves or grade history. No worker, watcher or
metadata service is added. External concurrent editing remains outside D050's
single-session contract.

Assignable bindings, hard-coded marking keys, multi-photo selection, batch
marking/export and rejected-file operations remain separate milestones.

## Acceptance evidence

The existing `--selftest-photo` and full `--selftest` suites cover the following
behaviours; `--selftest-sidecar` runs the existing sidecar suite directly:

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

- Opening tests deliberately stale the scanned entry, then prove that successful
  opening synchronizes it and the current mark, clears a previous mark warning,
  and leaves incompatible grades Unknown even with a known stored mark. Failed
  decoding/departure preserves the outgoing selection and warning. Direct-image,
  equivalent path spellings and directory symlinks exercise the production action
  and opening paths. Nonexistent, unsupported and directory targets create no
  stray sidecar. The read-only action test follows SidecarFileTests: a root runner
  drops privileges in a child with its own fixtures rather than skipping refusal.
- Real ImGui frames check 360/800/1280 logical-pixel widths, 96/160 heights,
  moved floating windows, framebuffer scales 1/2, every existing theme and both
  absent/long warnings. Assertions bound the toolbar to one row, keep thumbnail
  footers within the clip rectangle and reject vertical scrolling at default
  height. Real input still selects the type tabs and assigns marks through the
  semantic action, including failed-save selection retention. Draw vertices prove
  the correct active button colour, including direct opening, a filtered-out
  current photo and Unknown. Native macOS testing remains
  separate from these headless ImGui checks.

Expected marks come from explicit fixture literals and requested enum values;
untouched grade bytes are compared directly with the hand-authored input. The
runtime/history invariants are observed with existing test access rather than
inferred from successful serialization. Existing sidecar/lifecycle suites remain
part of every full run.

Targeted mutations removed opening/current-mark/entry synchronization, Unknown
protection, equivalent-path matching, warning clearing, target validation, the
success-only state update, active styling, the single toolbar row and compact
warning feedback (including separate nonexistent/unsupported target guards). All 15 variants failed a regression assertion; the unchanged
baseline passed. Detached source copies reused the existing compiler and test
facilities without modifying production files.

Full Release, focused photo/sidecar and full leak-enabled ASan/UBSan self-tests
passed on Linux. Exact-head CI results are recorded in the PR. The audit keeps the approved action/writer/worker boundaries.

## Required macOS GUI checks

These require the native macOS application; Linux self-tests do not establish them.

1. Open a mixed RAW/compressed folder. Verify readable P/U/R footers and full
   mark/error tooltips in the normal docked filmstrip, Retina scaling and a
   resized/floating filmstrip. At the unchanged default height, verify the single
   toolbar row, unclipped thumbnail footers and usable native tab scrolling in a
   narrow window. Confirm the separate mark filter remains usable.
2. Mark the current photograph Pick → Reject → Unmarked. Verify immediate
   sidecar metadata and restoration after folder reopening/application restart,
   with grade parameters, colours and node positions intact.
3. Combine each mark filter with RAW/Compressed. Click remaining thumbnails and
   verify normal photo loading. Change the current photo's mark so it disappears
   from the active filter: the viewer must retain that photo and the buttons must
   still target it and highlight its current mark. Restore All and confirm it.
4. Open an image directly without a folder. Verify all three marking buttons
   work, highlight the current mark without filmstrip entries, and have no effect
   on grade Undo/Redo or the processed preview.
5. Exercise malformed/newer/legacy-only sidecars and protected source viewing.
   Confirm error indication, useful tooltip/warning and refusal without changing
   bytes. Use a read-only sidecar/folder to confirm a failed mark never displays
   success or changes the active marking button. Verify the persistent inline `!`
   and full error tooltip without reducing thumbnail space. Protected opening
   must show no active marking button. Repair/reopen and retry successfully;
   successful opening must clear a previous mark warning.
6. Retain an unsaved grade while marking; navigate, reopen and quit. Verify normal
   grade saving and native Retry / Discard / Cancel behaviour after a grade-save
   failure, including usable editing after Cancel. Reject must never delete,
   move or trash a photo or sidecar.
