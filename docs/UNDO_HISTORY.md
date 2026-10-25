# In-memory grade history (D053)

Stages 2–3 use one `App::history` (`GradeHistory`) for the current photograph.
Entries own complete detached `GradeSnapshot` values, including processor
availability, never processors or image buffers. The cursor names the current
entry. Undo/Redo call Stage 1 restoration and move the cursor only on success;
replay never calls the ordinary Replace command or records itself.

Opening establishes a baseline after graph/source/colour publication and any
initial save, within the same opening execution-exclusion scope. Its completion
requests the preview once. Successful photo switches and
same-photo reopening reset history, including when the incoming photo is
protected (which has no editable history). Failed/cancelled navigation retains
it. Save Grade never changes history, including an unrecorded-state marker.
History never determines whether, when or what to save (D050).

## Recording and failure policy

Completed discrete operations explicitly call `recordCompletedEdit()` with their
existing `DocumentMutation` scope, after publishing the complete edit. The capture
API verifies that the supplied scope owns the same App and reuses its exclusion;
it neither creates a nested mutation nor requests an intermediate preview.
Standalone capture (including Stage 3 gesture completion) acquires one scope after
checking existing export/mutation/protection state. Processor synchronisation is
unchanged. No history capture waits for active export. No new scheduler, worker
or runtime state is introduced.

Output-only edits use `recordMetadataChange()`: when no other edit is pending, copy
the current entry, update its colours and current positions, then use the same
comparison, retention and redo-truncation code. No processor access, private sync,
mutation, cancellation or full preview request occurs. Display refresh remains
permitted during export and preserves export status. If another edit is pending
(or the baseline is absent), keep that state pending for the next full completed-
edit capture; never copy a stale entry while omitting unrecorded changes. A copy/
retention failure uses the ordinary pending-state policy without overwriting
active export status.

An unchanged logical snapshot adds nothing and preserves redo. Comparison uses
the existing codec with canonical NodeId order, retaining Slot order and opaque
parameter tokens; availability participates too. New entries truncate redo only
after capture, comparison, size checks and allocation succeed. Oldest entries
are then evicted to meet both limits.

Parameter writes and manual node movement mark the current state pending without
capturing per frame. The marker distinguishes metadata from a full logical grade:
position completion may copy the current entry only when no processor edit awaits
capture. Both pending kinds disable traversal. Metadata capture failures remain
pending and cannot be erased by an unrelated unchanged drag. Automatic Graph View materialisation of
missing positions is housekeeping: it persists through ordinary saving but does
not mark history pending or disable Redo. The UI calls the same completion API
once per gesture. A later successful completed-edit capture records the actual
logical state and safely truncates any obsolete redo branch.

Failed capture (including private-state sync failure, allocation/codec failure or
an oversized snapshot) keeps the live edit, all stored entries and the cursor.
It marks the current state unrecorded and disables both traversal directions.
Retry `recordCompletedEdit()` after the cause is resolved. If the state is equal
to the current entry, recording simply clears the marker and preserves redo.
If initial baseline capture fails, the new photograph has no traversable history;
a later successful capture establishes its baseline. No old-photo history survives.

Failed replay preserves the live document, processor ownership, entries and
cursor, and leaves the Stage 1 error visible. A missing CTL file/plugin can be
restored and replay retried. Successful replay performs exactly one restoration
mutation, with no subsequent capture or private-state sync, and reports success
immediately. There is no separate replay comparison metadata or failure state.
An accepted narrow trade-off: after replaying an OFX plugin that changes persistent
parameters during CreateInstance, an apparent no-op full capture may record its
effective live grade as an additional step and truncate Redo. The live document
is retained and restoration success is never reclassified as failure. Output-only
entries keep the original processor initial values, so their reconstruction
preserves those same permitted creation-time differences.

## Limits and measurements

The default retains a baseline plus **100 undo steps** (101 entries), with a
**64 MiB accounted-memory limit**. Either limit can evict oldest entries. A single
snapshot over the memory limit is refused under the capture-failure policy.

`snapshotBytes()` counts snapshot structures, vector capacity, string capacity,
map values/keys and a conventional three-pointer tree-node overhead. Counting
inline string capacity again is conservative. This is a simple retained-history
budget, not an allocator/RSS promise: outer-vector spare capacity, allocator
metadata and temporary capture/codec/reconstruction allocations are excluded.
The latter can exceed the retained budget temporarily.

Measured by deterministic self-tests on Linux/libstdc++:

| Complete snapshot | Accounted bytes |
| --- | ---: |
| Source only | 840 |
| One native Exposure | 1,784 |
| 16 native processors | 19,424 |
| 100 native processors | 95,168 |
| Native plus 2 MiB opaque preserved parameter | 2,099,043 |
| OFX plus 2 MiB persistent Custom parameter | 2,099,180 |

Thus 101 of the 100-processor snapshots account for about 9.2 MiB; the 64 MiB
cap leaves room for ordinary graphs while retaining only about 31 of the large
OFX snapshots (30 undo steps). Measurements depend on platform/container
capacities; tests check invariants and payload scale rather than these exact
implementation sizes.

## Integration and remaining work

Integrated: native/OFX/CTL Add Processor; deletion, enable/disable and Clear;
App graph commands (connections, Output rewiring, structural/Slot changes);
fragment/node paste, grade Replace and preset Append; discrete RAW and Output
colour changes. Each completed batch records once. Legacy V1/V2 fixture-only
transfer helpers are outside the live history workflow.

## Gesture and UI boundaries (Stage 3)

Shared parameter widgets use ImGui activation/deactivation, with one optional
Inspector edit record: item ID, photograph, logical node/parameter identity and
an optional deferred numeric value. Sliders, vector drags and strings keep their
existing live writes/preview behaviour. Numeric text remains deferred until Enter
or focus loss. Escape follows the actual ImGui-restored value. Resets, steps,
choices, booleans and representable push-button changes complete immediately.
Unchanged interactions preserve Redo through the existing snapshot comparison.

After drawing, a missing/deactivated item completes the same gesture. This covers
selection/filter changes, hidden/collapsed Inspector, disabled controls and focus
loss. Unapplied numeric text is committed only while the original node/parameter
is still editable; otherwise it is discarded and the actual live grade is kept.
UI navigation explicitly completes edits before ordinary departure/save; successful
photo opening still resets history. Export defers completion until execution is
available, without capture attempts or status changes each frame. A real capture
failure clears the UI gesture but leaves history paused; a later completed edit
can recover. There is no retry scheduler or per-frame snapshot collection.

Graph Canvas latches movement within its existing drag state. Release, focus loss,
List switch and hidden/collapsed Nodes complete it. Colours and positions share
metadata-only recording: no processor access, private sync, mutation, preview
request or sidecar write. If a processor edit is pending, completion waits until
full capture is safe and includes both changes. A clean drag returning to its
starting position clears only its metadata marker, including generated positions
absent from the Source-only baseline; existing failed captures remain pending.
Panning, zooming, framing, selection and inspection are not history steps.

Edit-menu availability and keyboard actions share document/export eligibility,
history endpoint/pending checks and refusal while a UI gesture is active. Undo is
Cmd+Z on macOS and Ctrl+Z elsewhere; Redo is Cmd+Shift+Z on macOS and Ctrl+Shift+Z
or Ctrl+Y elsewhere, using ConfigMacOSXBehaviors. Active text inputs retain their
own shortcuts. A shortcut during an uncommitted gesture is refused; it does not
silently finish and traverse in one action. Restoration/capture errors use the
existing status path. No toolbar or history panel is added.
