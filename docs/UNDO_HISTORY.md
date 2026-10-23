# In-memory grade history (D053)

Stage 2 uses one `App::history` (`GradeHistory`) for the current photograph.
Entries own complete detached `GradeSnapshot` values, including processor
availability, never processors or image buffers. The cursor names the current
entry. Undo/Redo call Stage 1 restoration and move the cursor only on success;
replay never calls the ordinary Replace command or records itself.

Opening establishes a baseline after graph/source/colour publication and after
opening/initial-save mutation scopes have ended. Successful photo switches and
same-photo reopening reset history, including when the incoming photo is
protected (which has no editable history). Failed/cancelled navigation retains
it. Save Grade never changes history, including an unrecorded-state marker.
History never determines whether, when or what to save (D050).

## Recording and failure policy

Completed discrete operations explicitly call `recordCompletedEdit()` after
releasing their document mutation. Capture synchronises processors through the
existing Stage 1 API. The runtime's capture eligibility query checks existing
export/mutation/protection state; capture cannot nest a mutation or wait for an
active export. No new scheduler, worker or runtime state is introduced.

An unchanged logical snapshot adds nothing and preserves redo. Comparison uses
the existing codec with canonical NodeId order, retaining Slot order and opaque
parameter tokens; availability participates too. New entries truncate redo only
after capture, comparison, size checks and allocation succeed. Oldest entries
are then evicted to meet both limits.

Parameter writes and node movement/materialisation only mark the current state
unrecorded: they never capture per frame. Stage 3 will call the same completion
API once per gesture. Until then, traversal pauses after these unwired changes;
it cannot silently undo past them. A later successful completed-edit capture
records the current logical state and safely truncates any obsolete redo branch.

Failed capture (including private-state sync failure, allocation/codec failure or
an oversized snapshot) keeps the live edit, all stored entries and the cursor.
It marks the current state unrecorded and disables both traversal directions.
Retry `recordCompletedEdit()` after the cause is resolved. If the state is equal
to the current entry, recording simply clears the marker and preserves redo.
If initial baseline capture fails, the new photograph has no traversable history;
a later successful capture establishes its baseline. No old-photo history survives.

Failed replay preserves the live document, processor ownership, entries and
cursor, and leaves the Stage 1 error visible. A missing CTL file/plugin can be
restored and replay retried. After successful restoration moves the cursor, an ownership-safe capture refreshes
no-op comparison metadata. OFX may legitimately adjust parameters during
CreateInstance: when actual logical values differ, keep one optional comparison
key without replacing the original restoration snapshot or adding a step. This
prevents an unchanged completion from incorrectly truncating redo. The key counts
against the same memory cap. If this refresh fails or cannot fit, the replay
itself succeeded; retain its selected cursor and pause further traversal under
the capture-failure policy until a successful completed-edit capture.

## Limits and measurements

The default retains a baseline plus **100 undo steps** (101 entries), with a
**64 MiB accounted-memory limit**. Either limit can evict oldest entries. A single
snapshot over the memory limit is refused under the capture-failure policy.

`snapshotBytes()` counts snapshot structures, vector capacity, string capacity,
map values/keys and a conventional three-pointer tree-node overhead. Counting
inline string capacity again is conservative. This is a simple retained-history
budget, not an allocator/RSS promise: outer-vector spare capacity, allocator
metadata and temporary capture/codec/reconstruction allocations are excluded.
The latter can exceed the retained budget temporarily. An optional current-state
comparison key is retained only for permitted backend initialization differences;
its capacity participates in the same cap, not a separate cache budget.

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

Stage 3 still owns gesture completion/grouping for shared parameter widgets
(including text, sliders, choices/reset/activation) and node drags, plus keyboard
shortcuts and menus/visible availability. These interactions currently mark
unrecorded state without collecting steps. There is no pending-work scheduler,
per-frame capture or history-driven persistence. Inspector expansion, selection,
inspection and other view-only changes are not history steps.
