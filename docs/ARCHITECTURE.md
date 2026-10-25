# RawNode Architecture

**Status:** Authoritative target architecture.

This document records the architecture approved after the RawNode <-> vkdt
architecture audit and independent Claude review. Steps 1–4 and the
post-Step-4 stabilisation/checkpoint gate and Step 5 product-contract work are
complete. Step 6 implementation and independent whole-Step-6 review are complete
and merged in PR #58 (`6294a034`). Preview, inspection, export and List View use
one explicit document graph. Complete per-check macOS/native-dialog and NEF
manual-validation evidence is not recorded; see `docs/STEP6_MANUAL_VALIDATION.md`.
Step 7 PRs 1–5 are merged (#63, #65, #66, #68, #69), completing graph-native
photo/preset/clipboard persistence and global preferences/workspace retirement.
The targeted post-Step-7 graph safety and PNG colour correction gate is complete
(see `docs/PROJECT_CONTEXT.md`). The switchable Graph/List editor (D047)
has its visual foundation (#79), workspace/Inspector (#80) and focused graph
editing (#82) merged. PR 2C's approved contextual node placement is implemented
for independent review/native validation before Step 8 masking (D051).
These fixes do not add architecture steps.

Read this together with:

- `docs/PROJECT_CONTEXT.md` for current project state and roadmap;
- `docs/DECISIONS.md` for accepted durable design decisions;
- `docs/CODE_STYLE.md` for coding guidance and
  `docs/STEP7_PERSISTENCE_DESIGN.md` for the accepted persistence specification;
- `docs/ARCHITECTURE_AUDIT.md` for the audit evidence and reconciliation that
  led here.

## Architectural north star

Every new RawNode feature should have an obvious home.

If adding a feature requires teaching unrelated parts of the application how
that feature works, the boundary is probably wrong.

This principle is deliberately stronger than "use abstractions". RawNode should
not accumulate feature-specific schedulers, queues, pending flags, persistence
paths, export paths or UI-to-render protocols simply because a new feature was
added.

A feature should normally fit one of the established responsibilities below.

## High-level model

The target model is:

    UI
     |
     v
    Document / Graph API
     |
     +----------------------+----------------------+
     |                                             |
     v                                             v
    Persistent document                        Render runtime
    -------------------                        --------------
    Source                                     Worker lifetime
    Graph topology                             Execution ownership
    Node parameters                            Edit/export exclusion
    Colour semantics                           Cancellation
    Sidecar state                              Preview/display demand
     |                                             |
     +----------------------+----------------------+
                            |
                            v
                     Graph evaluator
                            |
                            v
                     Processed result
                       /          \
                      v            v
                 Display path    Export path
                      |            |
                      v            v
                   Viewer         File

This diagram expresses responsibility boundaries, not a requirement for a large
class hierarchy.

The implementation should remain as small as possible while keeping these
boundaries clear.

## Responsibility boundaries

### UI

The UI expresses user intent.

Examples:

- enable or disable a node;
- change a parameter;
- add, remove or reorder a node;
- start export;
- change a display setting.

The UI should not know how to cancel a render, acquire graph ownership, wait for
an export, manipulate renderer pending flags or decide which worker must wake.

UI code should not implement sequences such as:

    wait for renderer
    mutate document/graph
    schedule preview

Those sequencing obligations belong below the UI boundary.

### Document / graph API

The document/graph layer owns what the edit means.

It owns or coordinates:

- source/document state;
- node identity;
- graph topology;
- node enabled/bypass state;
- parameter changes at the document level;
- complete graph-edit transactions;
- colour interpretation state;
- persistence-facing state.

Structural edits must go through this boundary rather than directly mutating a
live node vector from arbitrary UI code.

A complete graph edit should be applied as one operation and should produce one
final invalidation/request decision.

### Processors

A processor owns how an image operation is performed.

Processor backends include:

- native C++;
- OpenFX;
- CTL;
- future DCTL;
- future LUT/CLF-backed processing.

Processors expose parameters and image-processing behaviour. They should not
know how RawNode schedules preview work, restores export previews, updates UI
status or persists the document.

Backend-specific synchronization remains inside the backend adapter where
required.

### Graph evaluator

The evaluator owns how processing dependencies are executed for one requested
result.

The target evaluator is intentionally simple:

- sequential;
- dependency ordered;
- evaluates only nodes reachable from the requested Image/Mask endpoint;
- executes a shared upstream node once per evaluation;
- retains intermediate buffers only while downstream consumers need them;
- distinguishes available values, unavailable values, hard errors and cancellation;
- propagates unavailable required inputs unless an operation defines a fallback;
- checks cooperative cancellation between operations where practical.

This is not a persistent node-cache system.

### Render runtime

The render runtime owns execution policy and lifetime safety.

Its responsibilities should include:

- preview worker lifetime;
- start/stop/join lifecycle;
- preview/export/edit execution exclusivity;
- image-render demand;
- display-refresh demand;
- cooperative cancellation/supersession;
- shutdown draining;
- safe result publication;
- owned export execution/lifecycle.

The current internal flags may continue to exist if they remain the simplest
correct implementation. They are not part of the feature API.

Encapsulation is only useful if callers no longer have to understand the
underlying wait/mutate/reschedule protocol.

The render runtime must not become a general-purpose job system.

### Display path

The display path consumes the published requested result. Available Image and
Mask values are converted for presentation; unavailable/error results retain
their identity and diagnostics without presenting a stale successful raster.

Display/monitor changes should not rerun expensive image processors when the
processed result itself is still valid.

The viewer owns view-only behaviour such as pan/zoom when that behaviour does
not require a different processed preview.

### Export path

Export consumes the same document graph/evaluator at full resolution, but it is
a separate evaluation from the preview.

Initially, export remains serialized against preview execution because processor
instances, especially OpenFX instances, are not assumed to be safely reusable
concurrently.

Export execution must have an owned lifecycle rather than a detached thread
borrowing application state indefinitely.

An export represents the accepted document state when Export is confirmed.
Parameter edits are disabled while the export owns the live processor instances.

Do not introduce parameter snapshots, queued edits or cloned processor graphs
unless non-blocking background export becomes a deliberate product requirement.

### Persistence

Persistence serializes document state, not renderer state.

It owns:

- stable node identity;
- processor identity/backend;
- parameters;
- enabled/bypass state;
- colour/document settings;
- explicit graph topology;
- primary Source and authoritative document Image output;
- ordered repeated Slot identity/settings once used by a graph operation.

Renderer pending flags, busy flags, cancellation tokens and worker state must
never become persistent edit state.

Application preferences have a separate lightweight path/load/save implementation
in `persist/Preferences`. It resolves one platform user configuration directory
before UI initialisation. `preferences.json` owns theme, visibility/fallback
sizes, List/Graph view, preview/export choices and RAW/Output defaults. Optional fields default
independently for additive extension. `layout.ini` remains ImGui-owned, configured
before its first frame and never switched by folder opening. No folder workspace
persistence remains; historical files are ignored and left untouched. Folder/photo
selection stays transient. Preferences save only after photo departure succeeds
or Discard is explicit, before teardown; Cancel leaves the app operational.
Configuration errors warn without selecting another directory. Historical V1/V2
helpers serve self-tests only, outside live photo/settings paths.

## Current safe renderer baseline

PRs #32-#34 established behaviour that must survive the architectural
transition unless a later change deliberately replaces it with a stronger
contract.

Required invariants include:

- preview rendering must not use processors being destroyed or replaced;
- graph mutation must not race active processor execution;
- export must safely own the live processor instances it uses;
- preview and export must not concurrently access those instances when unsafe;
- display-only refresh must not consume or cancel required processor work;
- explicit cancellation/idle barriers must actually drain the work they promise
  to cancel;
- export completion must restore a current preview;
- restored preview work must not overwrite export success/warning/error status;
- failed or cancelled work must not resurrect obsolete work;
- shutdown must drain execution owners before graph/application destruction.

These invariants are more important than preserving the current implementation
shape.

## Completed graph and persistence implementation (Steps 6–7)

`App::nodes`, including the primary Source, is the authoritative graph storage.
Static named Image/Mask ports and consumer-owned upstream references define
topology; node storage order has no processing meaning. Ordered node-owned Slots
retain stable SlotIds. Validation covers every stored connection and cycle,
including disabled, unavailable and unreachable nodes.
Structural validation uses iterative traversal and limits each stored dependency
chain to 128 nodes (127 edges), bounding subsequent readiness/evaluator recursion.
See `docs/GRAPH_DEPTH_SAFETY.md` for the gate audit and stack-safety rationale.

`GraphEvaluator` borrows that graph without depending on App, UI or scheduling.
One sequential requested-endpoint evaluation owns its memo and consumer-counted
intermediate lifetime. Preview, inspection and full-resolution export use this
evaluator under the existing RenderRuntime execution ownership. Publication
retains endpoint, generation, value kind and diagnostics; display-only refresh
converts the retained result without rerunning processors.

Graph commands stage and validate before acquiring DocumentMutation for an
actual commit. Rejected/unchanged commands preserve active preview/export work.
Source replacement drains the same ownership before replacing full/preview
pixels. Backend parameter edits retain their synchronization and export exclusion.

UI selection has one authoritative transient variant: no selection, a real NodeId
(Source or processor), or virtual Output. `selectedNodeId()` derives a real ID;
`selectedNode()` deliberately excludes Source for processor-only callers. List
View derives its main unary Image spine from documentOutput; other nodes remain visible and accessible.
Splice/move controls use stable identities and validated graph commands. The
projection neither changes connections nor supplies another execution order.

The dockable Nodes panel switches that List presentation with a Graph canvas.
Graph drawing borrows the same nodes, declares ports through `nodeInterface()`
and reads consumer-owned bindings and documentOutput directly. Its visual Output
endpoint is neither a stored node nor executable; its position is automatic.
Simple dependency columns supply missing coordinates. Add Processor stages
existing coordinates before insertion and publishes them only on success,
placing the new node right of its upstream Image predecessor in both views.
Surviving generated or manually moved positions never change automatically
during additions, deletion or reconnection; overlap is allowed without collision
avoidance. Protected photos derive placement without modifying the grade.
Source and processor header drags change only `App::nodePositions`,
saved by the existing grade lifecycle. Canvas pan/zoom and drag state are transient,
and visual interactions never acquire DocumentMutation or request rendering.
Inspector displays Source input/current-photo RAW controls, the existing processor
parameters, or Output grade colours/application export preferences. Fixed Source
and Output List entries sit outside the processor reorder list. Selection does
not change the requested render endpoint or documentOutput. Photo/grade replacement
clears it; graph commits clear a removed real ID without discarding Source or
virtual Output. Nothing persists this selection.

The compact top toolbar owns file actions and status/persistence warnings. Preview
owns resolution, Nodes owns the shared processor picker and List/Graph switch.
The retired Left Panel has no window, visibility or width preference. Fresh default
docking is Preview above Nodes, Inspector right and Filmstrip across the bottom;
ImGui retains ordinary resizing/floating/custom arrangement persistence. Historical
UI layouts have no migration/alias path; user configuration is never deleted.
Connection editing and deletion are delivered by merged PR #82 under D051.

Live photo capture/restoration preserves every stored graph node, connection,
endpoint, Slot allocator and optional position through the shared strict codec.
Opening stages source/graph/colours/preview before publication. Checked grade saves
run at editable open and document boundaries, independently of render demand.
Sidecars, presets and clipboard grades share that codec. Full-grade Replace
prepares incoming nodes, both colour pairs and any required RAW decode/preview
before replacement; selection/filter and inspection reset even when IDs are reused.
D053 history replay borrows this same staged path with complete, in-memory
processor availability checks. A recorded working processor must reconstruct with
its backend/identifier before publication; recorded missing processors stay missing.
This information is absent from persistent records. Replay retains valid selection
and inspection plus surviving Inspector expansion; ordinary Replace still resets
selection/filter and inspection. Stage 2 adds one App-owned GradeHistory of
logical snapshots and a cursor, explicitly recorded within completed document/
graph operations' existing exclusion scopes, without nested mutations. Standalone
capture checks existing export/mutation state before acquiring one scope. Output-
only recording copies the current entry when no edit is pending, preserving
processor-free display refresh even during export. Replay performs one restoration
without post-replay capture. Shared parameter widgets identify gesture completion
with ImGui lifecycle; one transient Inspector item record retains deferred numeric
text and logical identity across interruptions. Existing canvas state completes
node drags through the same colours/positions metadata recording path. Pending
metadata and full-grade changes remain distinct; layout completion cannot omit
processor edits. UI menu/shortcut eligibility includes active gestures, text focus,
export/protection and history availability. No scheduler or worker is added.
Failed captures pause traversal until a successful completion capture.
See `UNDO_HISTORY.md` for bounds, measurements and interaction/failure policies.
Linear Append and fragment Paste construct detached nodes, remap IDs/positions and
commit one StagedEdit batch. Only the chosen main-spine successor or Output changes;
unrelated branches remain. Fragment capture retains internal links independently
of attachment eligibility and detaches external inputs without matching foreign IDs.

Accepted transition limitations:

- Processor creation failures may conservatively enter DocumentMutation and
  restore interrupted preview work quietly.
- Graph staging assumes control-thread-only topology mutation and immutable
  port interfaces; it is not a concurrent editing protocol.
- No-op comparison must match committed fields, including ordered SlotIds,
  bindings, ID high-water marks and every future SlotSettings field.
- Arbitrary-graph editing still needs a suitable UX for disconnected required
  dependencies. The current List View exposes nodes without guessing repairs.
- Photograph sidecars, presets and global preferences use checked atomic replacement.
  ImGui retains its native ini persistence.

## Control-thread contract

Document and graph mutation are controlled from one UI/control thread.

This is an explicit architectural contract for the current application.

Debug builds should assert the contract at important mutation boundaries where
practical.

Do not design current graph APIs as though arbitrary concurrent mutation is
supported.

Worker/export threads may execute processing but must not initiate document
mutation transactions that would wait on their own execution ownership.

## Graph target

The explicit graph adds only the topology needed for future masks/compositing.
Storage order has no execution meaning; the current simple List View projects
the main unary Image spine. Arbitrary-DAG presentation remains deferred.

The first graph-capable representation should provide:

- stable node IDs;
- named input/output ports;
- simple port roles such as image and mask;
- explicit upstream endpoint references;
- fan-out;
- cycle rejection;
- required/optional input validation;
- one authoritative document output.

The initial UI may remain a list.

Graph/list views, if both exist, must represent the same underlying graph rather
than maintain separate processing models.

Existing native, CTL and OpenFX processors may remain unary processors at first.
RawNode graph support does not require immediate support for arbitrary OpenFX
multi-clip effects.

### Evaluation

Topology and executable evaluation should land together.

Do not create a second speculative graph structure that the renderer ignores.

The first DAG evaluator should:

- remain sequential;
- preserve current linear output when given an equivalent linear graph;
- execute dependencies in topological order;
- avoid repeated evaluation of shared upstream nodes;
- keep whole-reachable-graph invalidation;
- use simple per-evaluation consumer/buffer lifetime.

Do not add parallel branch execution, persistent node caches, fine-grained
dependency hashes, general ROI scheduling or a job system as part of this work.

## Masks, opacity and compositing

Masks are image-processing data, not a new renderer/scheduler domain.

The graph must be capable of representing a mask as an explicit input to an
operation rather than requiring renderer-specific mask state.

The UI may still present a mask as belonging to an adjustment even if the
internal graph represents mask generation and mixing explicitly.

Keep these concepts distinct:

- local-adjustment/effect strength;
- scalar mask/coverage;
- image alpha;
- layer compositing.

A node-opacity UI must not force every processor backend to implement masking
internally.

The Step 5 mask/graph contract is settled. Mask is a typed scalar-coverage
graph value, distinct from image alpha. Optional Mask coverage is fail-open on
unavailability with a warning; unavailable Mask data otherwise propagates
through required Mask-processing inputs.

Generic Mask/Strength/Blend ownership belongs to graph/node evaluation rather
than individual backends. The first concrete user-facing mask path remains Step
8.

## Colour semantics

RawNode is intended for serious colour-managed photographic workflows.

Unless an operation explicitly requires otherwise:

- preserve floating-point precision;
- preserve negative RGB;
- preserve RGB above 1.0;
- avoid unintended clipping;
- preserve alpha;
- keep colour-space/primaries and transfer-function/gamma concepts distinct;
- avoid silently introducing display-referred behaviour into scene-referred
  processing.

The first DAG deliberately keeps Image values numeric and raster-local:
floating-point RGBA plus dimensions. It does not automatically propagate
colour-space/transfer-function metadata, source-coordinate provenance or a
spatial transform.

Operations that require colour interpretation declare it explicitly. RawNode
permits arbitrary numerical branch combinations rather than automatically
inferring or converting unknown OFX/CTL/DCTL/LUT colour meaning.

The current output-tag behaviour remains a manual interpretation contract until
a deliberate colour-management change replaces it. Do not silently turn it into
a conversion operation during architecture refactors.

Monitor/display conversion belongs after the processed-image boundary.

## Alpha

No permanent global alpha representation is approved yet.

Existing processing often behaves like straight/unassociated RGB with alpha
preserved separately, but TIFF/EXR/OpenFX boundaries require focused validation
before this becomes a formal contract.

The leading candidate remains:

- straight/unassociated RGBA at general processor boundaries;
- premultiplied arithmetic inside operations that require it, such as
  compositing/filtering;
- explicit adaptation at I/O and OpenFX boundaries.

This remains pending until transparent compositing or alpha-carrying I/O makes
the decision necessary.

Mask representation is a separate decision from image alpha.

## Invalidation and scheduling

Do not create a scheduler domain per feature.

The broad conceptual distinction remains:

    source / processor / parameter / topology / mask / blend change
        -> processed image work required

    display / monitor interpretation change
        -> display work required

Viewer-only changes may require neither.

Whole-graph rerendering is acceptable initially.

A revision/key system is not an architectural goal by itself. Introduce result
identity or revision machinery only when a concrete requirement makes it
simpler than the existing demand/cancellation model.

Freshness, work demand, cancellation, execution ownership and status policy are
different concerns. Do not collapse them into one "stale" flag if doing so
recreates lost-work bugs.

## Supersession and publication

Interactive supersession promises eventual replacement of obsolete preview
work.

RawNode does not currently promise the stronger property that an obsolete frame
can never transiently publish after a later edit.

Do not add machinery to enforce that stronger contract unless it becomes a real
observable problem.

## Status policy

Operation outcomes such as export success/failure or node-creation failure
should remain visible.

If a failed operation must restore preview work, that restoration should be
quiet so routine preview progress does not erase the meaningful operation
status.

Quiet suppresses progress and Available-result messages only. A current Error,
Unavailable result or unexpected preview exception reports its failure and
clears stale presentation. Cancelled or superseded work publishes neither status
nor a result.

Status ownership should remain separate from image freshness.

## OpenFX constraints

OpenFX instances are mutable host/plugin objects and must be treated
conservatively.

For now:

- preview/export execution remains serialized for shared processor instances;
- OpenFX processors remain unary in the first graph implementation;
- no parallel branch rendering is assumed safe;
- no processor cloning is required for export;
- parameter/change-action concurrency should be validated with TSan and
  representative real plugins.

Before any future parallel rendering or preview/export concurrency, RawNode must
honour relevant OpenFX render-thread-safety declarations and re-audit host-side
shared state.

## Persistence migration for topology

Photo sidecars now use the graph-native format; historical V1/V2 files are protected without migration.

It is not a backwards-compatibility constraint on the explicit graph. RawNode has
no external user base requiring the new topology model to preserve V2 execution
semantics.

The graph-native format must preserve at least:

- stable Node IDs and node/backend identity;
- static/saved interfaces sufficient to preserve unavailable nodes;
- parameters and generic node controls;
- consumer-owned upstream OutputRefs;
- ordered Slot arrays with stable node-local SlotIds and slot-local settings;
- primary Source and authoritative document output;
- external asset references where relevant;
- safe copy/paste ID remapping.

Presets and clipboard now use the same graph-native records and strict codec.
Incompatible attachments refuse without mutating the destination or clipboard;
explicit topology is never flattened into V2 list order.

## Deliberately deferred complexity

The following are not part of the approved near-term architecture work:

- persistent per-node caches;
- branch-specific invalidation;
- parallel branch execution;
- tile schedulers;
- comprehensive ROI scheduling;
- general-purpose job systems;
- multiple renderer worker pools;
- automatic processor cloning for preview/export;
- multiple simultaneous exports;
- temporal/feedback graphs;
- Vulkan as an application architecture requirement;
- GPU-native graph-result storage;
- automatic colour-space inference for arbitrary OFX/CTL/DCTL code;
- OpenFX multi-clip support;
- a dedicated status subsystem;
- a broad revision/evaluation-key framework.

Prefer re-rendering slightly too much before adding another invalidation or cache
domain.

## Approved migration sequence

The architecture should be reached through small behaviour-preserving changes.

1. **Own the export job.**
   Replace detached export lifetime with owned execution and RAII cleanup. Make
   production export directly testable.

2. **Freeze parameter edits during export.**
   Enforce the approved export consistency contract without snapshots or queues.

3. **Centralize graph-edit transactions.**
   Remove caller-managed wait/mutate/reschedule sequences and fix failure
   restoration at the same boundary.

4. **Encapsulate renderer ownership.**
   Once the public protocol has shrunk, own worker lifecycle, execution
   exclusivity, demand, cancellation and shutdown behind one runtime boundary.

### Stabilisation gate after Step 4

The full gate is complete:

1. OpenFX host multithread lifetime correction (#48);
2. behaviour-preserving readability/source organisation (#50);
3. independent Codex and Claude whole-architecture checkpoint against RawNode's
   product goals and the pinned vkdt reference.

Both reviews found the direction sound for the next stage, with no broad
architectural blocker before Step 5. Immediate follow-ups #53 (source-only
sidecars) and #54 (mutation-completion ordering regression) are complete.
See `docs/ARCHITECTURE_AUDIT.md` for evidence and non-blocking dispositions.

These are stabilisation/review gates, **not additional numbered architecture
steps**. Steps 1–4 are complete. Step 6 implementation and independent
whole-Step-6 review are complete and merged in PR #58. Outstanding manual
validation evidence is tracked separately in `docs/PROJECT_CONTEXT.md`.

5. **Settle the first mask/graph product contract — complete.**
   The accepted contract is summarized in `docs/DECISIONS.md` and detailed in
   `docs/STEP5_GRAPH_CONTRACT.md`.

6. **Introduce explicit topology and sequential DAG evaluation together — complete, independently reviewed and merged.**
   One authoritative graph has static named typed ports,
   consumer-owned input references, ordered stable Slots, explicit Source and
   document output, requested Image/Mask endpoint evaluation, fan-out, joins,
   cycle rejection, shared-upstream evaluation and per-evaluation value lifetime.
   Equivalent linear behaviour is preserved. The first concrete production
   Mask application remains Step 8.

7. **Add versioned graph persistence — complete; PRs #63, #65, #66, #68, #69.**

   The post-Step-7 correction gate is complete. Graph/List visual foundation
   (#79), workspace (#80) and editing (#82) are merged; PR 2C's approved contextual
   placement is implemented for independent review/native validation before Step 8.
   This is a UI milestone (D047), not another numbered architecture step. Both views edit the
   same authoritative document graph and share NodeId selection; no additional renderer or
   execution model is introduced.

8. **Settle remaining concrete mask details and add the first mask path.**
   Implement the Step 5 contract; settle remaining representation, range,
   filtering/sampling and concrete blend encoding where not already needed in
   Step 5. Global alpha association remains separate until the corresponding
   transparent-compositing or alpha-carrying feature requires it.

Each step should remain independently reviewable.

## Source organisation and human readability

Source layout should help a human programmer recover the architecture from the
code rather than requiring knowledge of the project's implementation history.

Prefer:

- one obvious responsibility per file or tightly related file group;
- top-down implementation order: public/domain operations before lower-level
  mechanics where practical;
- related lifecycle, request, mutation, export and worker operations grouped
  conceptually rather than ordered by when they were added;
- domain-oriented names over names that expose incidental implementation
  machinery;
- small, visually obvious lock scopes and comments that state ownership/thread
  invariants;
- comments that explain **why**, invariants and non-obvious constraints rather
  than restating C++ syntax;
- source structure that mirrors the UI/document/graph/runtime/display/export/
  persistence boundaries in this document.

Do not pursue readability by inventing layers, managers or tiny wrappers without
a real responsibility boundary. Formatting is useful but is not a substitute
for conceptual reading order.

The durable source-style guidance lives in `docs/CODE_STYLE.md`.

## Testing requirements

Core architecture changes require tests that exercise production paths, not only
hand-reimplemented equivalents.

For renderer/export/graph work:

- retain deterministic #32-#34 regression coverage;
- prefer barriers/observers over timing sleeps;
- provide a deliberate test observation seam when internals become private;
- run the full self-test suite;
- use ASan/TSan where practical;
- validate failure paths and exception cleanup;
- ensure tests fail when the intended guarantee is deliberately removed;
- test real OpenFX interaction when concurrency behaviour is involved.

## Final design rule

When adding a feature, ask:

    Where does this belong?

Typical answers should be obvious:

    DCTL / LUT / Exposure / CTL / OFX
        -> processor + graph

    Mask
        -> graph data / graph operation

    Blend / compositing
        -> graph operation

    Display profile / monitor transform
        -> display path

    Export format / file encoding
        -> export path

    Sidecar format
        -> persistence

If the answer is instead "add knowledge of this feature to UI, renderer,
persistence, export and several global flags", stop and re-examine the boundary.
