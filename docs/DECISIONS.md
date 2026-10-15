# RawNode Architectural Decisions

**Status:** Authoritative decision log.

This file supersedes the historical root-level `DECISIONS.md`. It records durable
decisions, not implementation history.

## Existing durable decisions

The following earlier decisions remain accepted:

- **D001:** Folder-based workflow, no catalogue.
- **D002:** Sidecars are authoritative per-image edit state.
- **D003:** Nodes use a backend-neutral processor abstraction rather than an
  OFX-specific model.
- **D004:** RawNode has one graph-capable processing model; list and graph views
  are views of the same underlying document.
- **D005:** Keep Dear ImGui + GLFW until a concrete blocker appears.
- **D006:** Keep C++ as the application language.
- **D007:** Keep RAW decoding replaceable and separate from RAW development.
- **D008:** Keep the RAW stage minimal; ordinary photographic adjustments belong
  in the processing graph.
- **D009:** Masks/local adjustments are first-class future requirements; AI
  masking should produce masks rather than define a separate architecture.
- **D010:** Cloud, heavyweight DAM/catalogues and project files are non-goals.
- **D011:** Assignable controls target node instances, not processor types.
- **D012:** Prefer reversible staged refactors.
- **D013:** Prefer upstream-friendly infrastructure where practical.
- **D015:** Sidecar V2 is backend-neutral and preserves unavailable processors.
- **D016:** Native Exposure is a reference processor, not a mandate that all
  tools be native.
- **D017:** Standard CTL is the core CTL backend; host-specific conventions are
  adapters.
- **D018:** Plain CTL parameters do not invent UI ranges.

Earlier **D014** allowed speculative graph/node extension fields before they were
executable. The reviewed architecture revises this: topology should be
introduced with the evaluator that actually uses it rather than accumulating a
shadow graph model.

## D019 — Every feature has an obvious home

**Status:** Accepted

Every new RawNode feature should have an obvious architectural home.

If adding a feature requires teaching unrelated parts of the application how
that feature works, the boundary is probably wrong.

Typical homes are:

- image processing -> processor/graph;
- masks/blends/compositing -> graph data/operations;
- execution timing/lifetime -> render runtime;
- monitor/display conversion -> display path;
- file output -> export path;
- durable edit state -> persistence;
- user interaction -> UI/document commands.

## D020 — Staged architectural refactor, not a renderer rewrite

**Status:** Accepted

The audit and independent Claude review found the processor foundation and small
renderer fundamentally sound. RawNode will clean up ownership and boundaries in
small behaviour-preserving steps before masks/compositing.

## D021 — Remove leaked protocol rather than merely moving flags

**Status:** Accepted

The architectural problem is caller knowledge of sequences such as
wait -> mutate -> reschedule.

Current renderer flags may remain internally if they are still the simplest
correct implementation. Encapsulation is useful only when ordinary callers no
longer manage renderer protocol.

## D022 — Document/graph mutation uses one control thread

**Status:** Accepted

Structural document/graph mutation is a UI/control-thread responsibility.
Current APIs do not promise arbitrary concurrent graph mutation. Debug
assertions should enforce this contract where practical.

## D023 — Export has an owned lifecycle

**Status:** Accepted

Export should be owned execution with explicit cleanup/join behaviour, not a
detached thread borrowing application state.

It uses the same document graph/evaluator as preview, at full resolution.

## D024 — Export consistency is simple and conservative

**Status:** Accepted

An export represents the accepted document state when Export is confirmed.
Parameter edits are unavailable while export owns the live processor instances.

Do not add snapshots, queued edits or cloned graphs unless true non-blocking
background export becomes an explicit product requirement.

## D025 — Meaningful operation status survives recovery work

**Status:** Accepted

Export outcomes and operation errors should remain visible. If a failed
operation needs to restore preview work, that recovery should be quiet rather
than overwriting the meaningful status with routine rendering status.

## D026 — Preserve PR #32–#34 guarantees

**Status:** Accepted

The lifetime and scheduling guarantees established by PRs #32, #33 and #34 are
the minimum safe baseline. Refactors may replace mechanisms only while
preserving or strengthening those guarantees.

## D027 — Display and export consume processed results

**Status:** Accepted

Display conversion occurs after the processed-image boundary and should not
rerun processors when the processed result remains valid.

Export is a separate full-resolution evaluation of the same processing model,
not a separate image-processing architecture.

## D028 — Explicit topology plus a simple sequential DAG

**Status:** Accepted

When RawNode moves beyond the linear chain, topology and executable traversal
land together.

The first DAG uses stable node IDs, named ports, simple image/mask roles,
explicit connections, fan-out, cycle rejection, one document output and
sequential dependency evaluation.

## D029 — Whole-graph invalidation first

**Status:** Accepted

The first DAG rerenders the whole reachable graph when image state changes.
Persistent node caches, selective branch invalidation, parallel branches,
dependency hashes and broad ROI/tile scheduling are deferred.

## D030 — No feature-specific schedulers

**Status:** Accepted

Masks, LUTs, DCTL, compositing, opacity and other processing features use the
same graph/render infrastructure. They do not create their own workers, queues
or pending-state domains without a strong independent reason.

## D031 — OpenFX remains conservative initially

**Status:** Accepted

The first graph implementation may keep OpenFX processors unary. Preview/export
use of shared instances remains serialized. Parallel graph execution and
processor cloning are not assumed safe.

Before any future parallelism, re-audit OpenFX host state, honour relevant
thread-safety declarations and validate representative plugins.

## D032 — Supersession promises eventual replacement

**Status:** Accepted

Newer preview work must eventually replace obsolete work. RawNode does not
currently promise that an obsolete frame can never transiently publish.

Do not add machinery for a stronger guarantee without an observable need.

## D033 — Revisions are not an architecture goal

**Status:** Accepted

Revision IDs/evaluation keys may be added later if they simplify a concrete
requirement. They are not required simply because they are useful concepts.

Freshness, demand, cancellation, execution ownership and status remain distinct
concerns.

## D034 — Masks are distinct from image alpha

**Status:** Accepted

A local-adjustment mask is a separate scalar/coverage concept from image alpha.
The graph must be able to represent mask data without creating a separate
rendering architecture.

The Step 5 graph/mask product contract is settled. Mask remains a distinct typed graph value; the first concrete production mask path remains Step 8.

## D035 — Global alpha contract remains pending

**Status:** Accepted deferral

Do not declare a permanent global alpha association yet.

Straight/unassociated processor boundaries remain the leading candidate, with
premultiplied arithmetic inside operations that need it, but TIFF, EXR and
OpenFX boundaries require validation before approval.

## D036 — docs/ is authoritative

**Status:** Accepted

The authoritative project documents are:

- `docs/PROJECT_CONTEXT.md`;
- `docs/ARCHITECTURE.md`;
- `docs/DECISIONS.md`;
- `docs/ARCHITECTURE_AUDIT.md` for audit evidence/reconciliation;
- `CLAUDE.md` for Claude workflow/review instructions.

Old root-level architecture/decision files are historical only.

## D037 — Stabilise and make the source human-readable before graph expansion

**Status:** Accepted

After renderer ownership encapsulation and before mask/topology feature work,
RawNode will:

1. fix the pre-existing OpenFX host multithread lifetime bugs discovered during
   Step 4 review in a separate correctness PR;
2. perform a behaviour-preserving human-readability/source-organisation pass;
3. perform the planned whole-architecture checkpoint against the pinned vkdt
   reference and RawNode's actual product goals.

The readability pass should make source layout reflect architectural
responsibilities: clear file ownership, top-down reading order, conceptually
grouped functions, domain-oriented names, visible locking/ownership and comments
that explain invariants and rationale.

It is not an excuse for hidden redesign. If cleanup reveals a genuine
architectural problem, record it for the checkpoint and address it deliberately.

These are stabilisation gates between Steps 4 and 5, not additional numbered
architecture-migration steps.

**Current progress:** the full stabilisation gate is complete: PR #48 fixed
OpenFX multithread lifetime, PR #50 completed readability/source organisation
after independent review and macOS validation, and Codex and Claude independently
completed the whole-architecture checkpoint against RawNode's product goals and
`hanatos/vkdt @ e2ebdd3e65ab39f8f7e9d30030f299fd725d0f2a`.
Both found the architectural direction **sound for the next stage**, with no
further broad refactor justified before Step 5. Immediate checkpoint follow-ups
#53 (D038 source-only sidecars) and #54 (single-level mutation-completion ordering
regression) are complete. Remaining lifetime/exception-safety findings are
focused, non-blocking follow-ups; see `docs/ARCHITECTURE_AUDIT.md`.

**Step 5 is complete:** the first mask/graph product contract was independently
reviewed by Codex and Claude and closed without another broad architecture
refactor. Step 6's explicit topology/evaluator transition and independent
whole-Step-6 review are complete and merged in PR #58 (`6294a034`). Complete
per-check macOS/native-dialog and NEF validation evidence is not recorded; see
`docs/STEP6_MANUAL_VALIDATION.md`.

## D038 — Sidecars belong to editable source documents only

**Status:** Accepted

RawNode sidecars represent editable per-source document state. Exported JPEG,
PNG, TIFF and other rendered derivatives do not receive RawNode sidecars.
Export consumes the evaluated document; it is not a second persistence path.
Successful export means the rendered derivative was written successfully.

This also removes a data-integrity hazard: the legacy export-sidecar naming
rule could resolve an export such as `DSC_0001.NEF.png` to the existing source
sidecar `DSC_0001.NEF.rawnode.json` and overwrite its grade, including when an
export was named after another source image.

Source Sidecar V2 semantics remain unchanged. Legacy export sidecars may remain
on disk, but are not part of the product model and require no migration or
deletion. Source-sidecar write reliability/atomic replacement remains separate
Step 7 work.

## D039 — Raster-local Image/Mask graph values

**Status:** Accepted

Graph values are deliberately minimal:

- `Image` is floating-point RGBA raster data plus width/height.
- `Mask` is scalar coverage in `[0,1]` plus width/height.
- Mask is distinct from image alpha.

The first DAG does not automatically propagate colour encoding, source-coordinate
provenance, canvas offsets or a `SpatialDomain`. Operations that need colour
interpretation declare it explicitly. Pixelwise operations use raster-coordinate
correspondence and require compatible dimensions. No implicit resize, alignment
or reprojection occurs.

A small per-evaluation context carries render purpose and Source full/evaluated
dimensions. Each operation receives the actual dimensions of its evaluated
inputs.

## D040 — Named typed ports, consumer-owned connections and stable Slots

**Status:** Accepted

Node kinds declare static named Image/Mask input/output ports with required or
optional input status.

Each single-producer input stores its upstream `OutputRef`; outputs fan out
because several consumers may reference the same output. There is no second
authoritative global edge list.

Repeated/variadic inputs use node-owned ordered Slots. A Slot owns a stable
node-local `SlotId`, its incoming references and its slot-local settings.
Vector position is semantic processing order; SlotId is identity and must not be
derived from position or producer. No persisted `GroupId` is required for the
currently planned graph.

## D041 — Explicit Source, document output and requested endpoint evaluation

**Status:** Accepted

Each document has one primary Source and an explicit authoritative Image
`documentOutput`. Output is never inferred from node storage/list order.

The sequential evaluator may request any named Image or Mask output for preview
or inspection while normal preview/export requests `documentOutput`. Shared
upstream work executes once per evaluation and intermediate values live only as
long as required by that evaluation.

## D042 — Fail-open Mask availability and numerical generator bypass

**Status:** Accepted

Unavailable Mask data propagates through required Mask-processing inputs unless
an operation defines a narrower fallback. RawNode does not manufacture an
upstream white raster for missing data.

At an optional Mask coverage input, an unavailable Mask is treated as absent:
the effect/layer uses full coverage, the connection is preserved, and a
persistent warning is reported.

A bypassed source-like Mask generator produces a real full-coverage Mask when
its Reference frame resolves. If a connected Reference is unavailable, the
generator output remains unavailable and the downstream coverage consumer uses
the fail-open rule.

Valid-but-incompatible raster dimensions remain a hard evaluation error and are
never converted into fail-open behaviour.

## D043 — Generic Image-node strength/blend ownership

**Status:** Accepted

Generic Mask, Strength/Mix and Blend are graph/node concerns, not mandatory
features inside every processor backend.

Normal is the first executable blend mode. Non-Normal maths remains feature work.

Full-strength Normal processing with no available Mask may change dimensions.
If pixelwise self-mixing is required by an available Mask, Strength < 1 or a
non-Normal blend, the participating rasters must have compatible dimensions.

Step 6 builds the typed graph/evaluator and proves these contracts with test
operations; the first concrete user-facing Mask application path remains Step 8.

## D044 — Step 6 uses the existing runtime ownership boundary

**Status:** Accepted

The first DAG remains sequential and runs under the existing `RenderRuntime`,
`DocumentMutation`, export exclusion, cancellation and shutdown ownership.

Structural edits are staged/validated and committed through
`DocumentMutation`. Node-owned mutable controls must be read race-free by the
worker through atomics/locking or a consistent per-evaluation snapshot.

Evaluation diagnostics travel with the requested result. Successful fail-open or
pass-through fallbacks may export with warnings; hard graph/value errors refuse
export. Warnings are derived from reachable dependencies.

## D045 — Sidecar V2 does not constrain graph persistence

**Status:** Accepted

RawNode has no external user base requiring Sidecar V2 backwards compatibility
to constrain the explicit graph.

Step 7 may replace V2 with a graph-native versioned format. Until then, any
save/autosave/clipboard/preset path that cannot represent the Step 6 graph must
refuse or be disabled rather than silently flattening topology.

V2 source sidecars are now historical and protected without migration. Legacy
clipboard/preset capture remains guarded until its graph-native replacement.

## D046 — Step 6 List View is a topology view, not execution order

**Status:** Accepted

For the Step 6 UI subset, List View derives the main Image spine from
`documentOutput` upstream through unary Image inputs.

Insert/paste/move/delete are explicit topology-splice operations addressed by
stable IDs and committed as document mutations. Storage/list order has no
independent processing semantics.

Arbitrary-DAG List View presentation remains deferred.

## D047 — Switchable Graph/List editor follows graph-native persistence

**Status:** Accepted

After Step 7 graph-native persistence and before Step 8's first concrete
production Mask path, add a switchable Graph/List editor. This is a UI milestone
between existing architecture steps, not a ninth architecture step.

Both views edit the same authoritative document graph and share stable NodeId
selection. Switching views introduces no additional renderer or execution model.

## D048 — Graph and minimal node layout share one source sidecar

**Status:** Accepted

Use one versioned, human-readable sidecar per editable source photograph to
persist the authoritative processing graph, parameters, stable identities and
connections once; derived presentations such as List View order are not stored
separately. Store minimal node X/Y layout positions keyed by NodeId in that same
sidecar, logically separate from executable graph data. Layout is optional: Step
7 provides its home without requiring positions before Graph View exists, and a
node without a saved position remains valid. Positions must not affect
connections, graph validation, rendering or graph evaluation; reopening the
editable source photograph restores any saved arrangement.

Do not persist rendered images, intermediate buffers, caches or unnecessary
transient UI state. Keep the format simple and readable. Separate project files
and catalogue databases remain non-goals (D001, D010); compression or binary
serialization requires a demonstrated need.

Exact schema details, field names and optional-field handling remain Step 7
design decisions. The Graph View itself remains the UI milestone between Steps
7 and 8 (D047), sharing the authoritative graph and NodeId selection with List
View.

## D049 — Independent photo metadata, shared grades and global preferences

**Status:** Accepted; Claude's final independent verification returned
**SAFE TO MERGE** at `75e7affd4014f090a90293d69b6a157050576864`
(result supplied by the user).

One photo sidecar independently owns an optional grade and photo metadata.
An intentionally empty saved grade is still a grade. Unopened photos can have
metadata only; editor opening establishes a grade (D050). Updating either section
preserves the other. Absent metadata.mark is canonical Unmarked.
Prefer exact untouched-section JSON where practical; numerical precision and
opaque processor data must survive. Metadata updates construct no processors.
Step 7 assumes one editing session and defers external-file conflict tracking.

Presets share the sidecar grade representation in `.rawnode.json`, with Replace
and Append modes. One Save Preset action saves the complete current grade,
including a Source-only grade. Copy Node / Paste Node remain distinct editing
actions sharing graph serialization; fragment capture is independent of whether
a destination supports attachment. There is no separate node-preset workflow.

Unreadable/unsupported/incompatible sidecars permit read-only source viewing
with a persistent warning, never grade editing or automatic default presets.
Ordinary save failures preserve unsaved edits until retry or explicit discard;
cancel leaves the current document available.

Only photo state and global application preferences persist; retire folder
workspace/layout files without a replacement workspace or catalogue. Photo
colour controls affect the photo only. Read/validate stored global colour
defaults with built-in fallback; the future Settings UI configures these same
fields, without temporary default controls.
The default grade preset is separate and remains later work. Parameter-group
expansion is transient in every scope/transfer; optional node positions remain
photo graph layout (D048).

See [PROJECT_CONTEXT.md](PROJECT_CONTEXT.md#persistence-direction) for accepted
workflow semantics and [STEP7_PERSISTENCE_DESIGN.md](STEP7_PERSISTENCE_DESIGN.md)
for the proposed schema and implementation boundaries.

## D050 — Editor opening establishes a grade; save at document boundaries

**Status:** Accepted; lifecycle independently reviewed in PR #62.

Successfully opening an editable photo establishes and saves its complete grade,
including Source-only. Restore valid existing grades without an immediate rewrite;
add a grade to metadata-only sidecars while preserving metadata. Folder discovery
and thumbnails create none. This replaces the prior no-grade-on-editor-browsing
policy; future default changes never replace an existing saved grade.

Save the current complete grade on departure, same-photo reopening and close,
before replacement/teardown, whether adjustments changed or not. Explicit Save
Grade remains. Reuse open-path/protection state without live grade-presence/dirty
flags, per-edit notifications, comparison snapshots, revisions or an event bus.
Persistence is independent of render demand.

Initial-save failure leaves the photo editable with a persistent non-modal
warning, cleared by a successful grade save to its sidecar or photo switch;
no tracking flag. Retry at departure/close. Unwritable folders can prompt even
with no edits.
Failed saves retain the document through Retry / Discard / Cancel; cancelled
shutdown leaves the editor running. Protected views remain read-only and never
overwrite sidecars (D049); exports remain sidecar-free (D038).

The live editor is authoritative while a photograph is open; concurrent external
sidecar edits are outside the supported workflow. Keep destination validation
and atomic replacement. Future default-preset application failure policy remains
deferred to that feature's milestone.

## Pending decisions

The following remain intentionally deferred until the corresponding feature:

- exact Qualifier algorithm;
- exact geometric-mask coordinate convention before geometric parameters are
  persisted;
- fractional Add / Intersect / Subtract Mask maths;
- non-Normal blend formulas/domain;
- Parallel Mixer arithmetic;
- detailed Layer Mixer compositing and alpha interaction;
- arbitrary-DAG List View presentation;
- global alpha association and transparent compositing policy;
- OFX RoD/render-scale/project-size/internal-mask policy before generic
  Mask/Strength/Blend is exposed on OFX;
- External Mask decoding/filtering/asset details;
- ROI, tiling, caches and graph parallelism.

## Approved near-term migration

1. Own the export job — complete.
2. Make parameter editing unavailable during export — complete.
3. Centralize graph-edit transactions — complete.
4. Encapsulate renderer ownership once the public protocol has shrunk — complete.

The D037 stabilisation gate and immediate checkpoint follow-ups are complete.

5. Settle the first mask/graph product contract — complete.
6. Introduce topology and sequential DAG evaluation together — implementation and
   independent whole-Step-6 review complete and merged in PR #58.
7. Add versioned graph persistence.

   Then add the switchable Graph/List editor UI milestone (D047), before Step 8.

8. Settle remaining concrete mask details and add the first mask path under the
   Step 5 contract.

If implementation evidence changes this direction, update this log rather than
letting the architecture drift silently.
