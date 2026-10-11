# Step 6 Explicit Graph / Sequential DAG Implementation Plan

**Status:** Planned.  
**Step 5 contract:** complete in `docs/STEP5_GRAPH_CONTRACT.md`.  
**Goal:** replace RawNode's implicit serial `App::nodes` execution with one authoritative explicit graph and a sequential requested-endpoint evaluator, while preserving existing runtime/lifetime guarantees and equivalent linear behaviour.

This is an implementation plan, not a new product-design document. If implementation exposes a genuine contradiction with the Step 5 contract, stop and reconcile that contradiction rather than silently inventing a new architecture.

## Core rule

Step 6 must not leave RawNode with two authoritative processing models.

The migration may be implemented as several reviewable commits on one Step 6 feature branch / pull request, but intermediate scaffolding is not a second document model and should not be merged to `main` as a finished state.

The cutover is complete only when preview, inspection and export all evaluate the same explicit graph.

## Required outcome

At Step 6 completion RawNode has:

- one authoritative graph;
- stable Node IDs;
- static named typed Image/Mask ports;
- consumer-owned upstream references;
- ordered node-owned repeated Slots with stable node-local Slot IDs;
- explicit primary Source and authoritative document Image output;
- fan-out and joins;
- cycle rejection across fixed, Slot and Reference dependencies;
- requested Image/Mask endpoint evaluation;
- one shared upstream execution per evaluation;
- per-evaluation intermediate lifetime;
- actual evaluated input dimensions;
- available / unavailable / hard-error / cancelled result states;
- sequential execution under the existing RenderRuntime / DocumentMutation ownership boundary;
- List View operations expressed as graph edits rather than storage-order edits;
- safe refusal of V2 persistence/transfer paths that cannot represent the current graph.

Step 6 does **not** need to ship the first production masking UI or Layer/Parallel Mixer.

## Explicit non-goals

Do not add as part of Step 6:

- automatic intermediate colour-space / transfer-function metadata;
- SpatialDomain, source-coordinate transforms or canvas-offset propagation;
- implicit resize/resample/alignment/reprojection;
- production Qualifier/Radial/Pen tools;
- production generic Mask × Strength UI;
- non-Normal blend maths;
- Layer Mixer or Parallel Mixer arithmetic;
- graph persistence beyond the temporary V2 representability boundary;
- ROI/tiling;
- persistent per-node caches;
- branch parallelism;
- a general job system;
- processor cloning;
- multiple render worker pools;
- a new scheduler for masks or graph features.

## Integration strategy

Use **one Step 6 feature branch / PR** with small reviewable commits.

Each commit should compile and pass the relevant self-tests, but do not merge a half-transition to `main`.

Recommended commit sequence:

1. Graph vocabulary and document state.
2. Topology commands and validation.
3. Sequential requested-endpoint evaluator.
4. Runtime / preview / export cutover.
5. List View and transfer/persistence boundary migration.
6. Remove the old serial authority, harden tests, sanitizer/stress validation.

A code review can occur after each commit. The final PR should receive one independent whole-Step-6 review before merge.

---

# Phase 1 — Graph vocabulary and document state

## Purpose

Introduce the smallest concrete graph data model required by the Step 5 contract without yet changing preview/export execution.

This phase should make the eventual evaluator shape obvious while avoiding a generic node-framework rewrite.

## Required concepts

Use simple C++17 types for the accepted identities:

```text
ValueType
    Image
    Mask

OutputRef
    nodeId
    outputPortId

InputAddress
    nodeId
    optional slotId
    inputPortId

Slot
    stable node-local SlotId
    consumer-owned inputs
    future slot-local settings

Node
    stable NodeId
    graph operation kind
    fixed consumer-owned inputs
    ordered Slots where applicable
    existing processor identity/state where applicable

Graph / document state
    nodes
    primarySource
    documentOutput
```

Exact C++ spelling is implementation work. Keep types strong enough that node indices and Slot IDs cannot be confused accidentally.

## Node operation identity

Do not use `processor == nullptr` as the graph node-kind model.

Step 6 must distinguish at least:

- Source;
- ordinary Processor-backed Image operation;
- graph-native/test operation classes required by evaluator tests.

Future generator/mixer kinds must have an obvious home without pretending they are missing processors.

Do not build a broad polymorphic graph framework merely to achieve this.

## Static port declarations

Known node kinds expose immutable/static interface declarations.

Examples:

```text
Source
    output image : Image

Processor
    input  image : Image required
    output image : Image

test Image→Mask
    input  image : Image required
    output mask  : Mask

test Mask→Mask
    input  mask : Mask required
    output mask : Mask

test slotted operation
    fixed/repeated inputs as required by tests
```

Production generic processor Mask input may be represented by the topology model, but Step 6 does not need to expose user-facing masking.

Known fixed ports do not need per-instance socket objects.

## Source

Source is a real zero-input graph operation.

Each document has exactly one primary Source corresponding to the current image.

Source:

- cannot be deleted;
- is not duplicated by copy/paste or presets;
- is normally hidden from the existing simple List View;
- supplies `Source.image`;
- is the default `documentOutput` in a source-only document.

## Transitional-field cleanup

The existing reserved/transitional graph fields must not become the new architecture:

- `NodeInput`;
- `NodeInputRole`;
- reserved `Node::opacity`;
- reserved `CompositeMode`.

Replace/remove them as the real graph/node-control model lands rather than extending them.

## Phase 1 tests

Add focused tests for:

- unique stable Node identity;
- static fixed-port declarations;
- input/output direction and Image/Mask type lookup;
- Source interface and Source immutability;
- `documentOutput` always naming an Image output;
- Slot IDs distinct from vector positions;
- deliberately out-of-order Slots `[3, 1, 2]`;
- Slot reorder preserving IDs/settings/references;
- insert/delete Slot without renumbering surviving IDs;
- duplicate Node ID / Slot ID rejection;
- multiple named outputs are addressable even if current production processors remain unary.

## Review gate

Before moving on, verify that this is still a small explicit graph model rather than a generic schema/reflection framework.

---

# Phase 2 — Topology commands and validation

## Purpose

Make topology explicit and editable through one command layer while preserving the existing DocumentMutation execution barrier.

The graph must become capable of representing serial, branched and joined dependency structures even though the current UI will initially exercise mostly a unary Image spine.

## Consumer-owned connections

Each input stores zero or one upstream `OutputRef`.

Outputs fan out because several consumers may hold the same `OutputRef`.

Do not create a second authoritative global edge collection.

A derived consumer/adjacency index is allowed if useful for validation or UI, but it must be reconstructible from consumer-owned inputs.

## Required graph commands

Provide ID-addressed operations for at least:

- find node by NodeId;
- connect;
- reconnect;
- disconnect;
- add node;
- delete node;
- set/change `documentOutput`;
- add Slot;
- delete Slot;
- reorder Slot.

The UI must not mutate topology containers directly.

Compound edits should be staged/validated before commit. `DocumentMutation` gives execution exclusion, not rollback.

## Validation

Validate at edit/load boundaries:

- Node IDs unique;
- fixed port existence;
- Slot ID existence;
- input/output direction;
- exact Image/Mask type compatibility;
- at most one producer per destination input;
- required/optional inputs;
- document output exists and is Image-valued;
- ordinary cycles;
- cycles through Slot inputs;
- cycles through Reference inputs;
- invalid/dangling endpoints.

Bypass/unavailability must not hide structural cycles.

## Unary Image-spine splice semantics

Pin the minimum commands needed by the existing List View:

### Insert

Insert a unary Image node by replacing:

```text
A → B
```

with:

```text
A → New → B
```

or by inserting between Source/document output as appropriate.

### Delete

For a unary Image node with an Image producer:

- reconnect consumers of the deleted node's Image output to that Image producer where the unary splice is well-defined;
- if `documentOutput` pointed at the deleted node, retarget it to the Image producer;
- disconnect consumers of other outputs rather than guessing an alternative;
- remove all incoming references to the deleted node from other consumers as part of the same mutation.

Source is not deletable.

### Move

Move means a topology rewrite of the Image spine, not a vector permutation.

Storage order must remain semantically irrelevant.

## Phase 2 tests

Add deterministic tests for:

- serial `Source → A → B`;
- one output feeding several consumers;
- reconnect retaining destination identity;
- Image→Mask type mismatch rejection;
- duplicate destination producer impossible/rejected;
- fixed-input cycle rejection;
- Reference cycle rejection;
- Slot-input cycle rejection;
- bypassed/unavailable edges still counted for cycle validation;
- delete producer with multiple consumers;
- delete current document output;
- insert/move/delete unary-spine operations;
- storage-vector permutation does not change graph meaning;
- Node/Slot command APIs work with deliberately non-positional IDs.

## Review gate

Review specifically for hidden uses of vector index as durable identity or execution order.

---

# Phase 3 — Sequential requested-endpoint evaluator

## Purpose

Introduce the real executable DAG.

Do not optimize it beyond what is needed for correctness.

## Evaluation API

Conceptually:

```text
evaluate(
    graph,
    requested OutputRef,
    EvaluationContext,
    cancellation
)
```

The exact C++ form may differ.

`EvaluationContext` carries only evaluation-wide state required by the accepted contract, including:

- purpose: preview / export / inspection;
- full Source width/height;
- evaluated Source width/height.

Do not attach this metadata to every graph value.

## Graph values

Evaluator values support:

```text
Image
    RGBA float raster
    width
    height

Mask
    scalar float raster
    width
    height
```

Mask storage should be single-channel.

## Result state

Evaluation must distinguish:

- available value;
- unavailable value plus diagnostics;
- hard error;
- cancelled/obsolete.

A warning is not itself an unavailable state.

Examples:

- missing unary processor with defined pass-through → available Image + warning;
- unavailable External Mask → unavailable Mask + warning;
- valid Mask with wrong dimensions at a required pixelwise use → hard error;
- cancellation → cancelled, never "missing".

## Dependency traversal

For the requested endpoint:

- traverse only reachable dependencies;
- validate/resolve required inputs;
- evaluate shared upstream work once;
- retain each result until its consumers for this evaluation are done;
- support fan-out and joins;
- never use node storage order as execution order.

A simple memo table keyed by output endpoint is sufficient. This is per-evaluation state, not a persistent cache.

## Source

`Source.image` returns the evaluation's Source image.

Source-only evaluation must therefore be an ordinary evaluator result, not an `nodes.empty()` special renderer architecture.

## Ordinary processor adapter

Production native / CTL / OFX processors remain ordinary unary Image operations initially.

For each evaluation:

- obtain actual evaluated Image input;
- configure/render the backend using that input's actual dimensions;
- preserve current cancellation behavior;
- produce the processor's Image output.

Do not broadcast document Source dimensions as the semantic input size.

Backend-specific limitations remain inside the adapter.

## Bypass

The evaluator should execute only dependencies needed for the bypass result:

- unary Image bypass → Image input only;
- unary Mask bypass → Mask input only;
- mixer bypass later → base/background only;
- source-like Mask generator bypass later → Reference frame only.

Structural cycle validation still includes all stored dependencies.

## Availability propagation

Implement the Step 5 rule exactly:

- unavailable required input → dependent output unavailable unless that operation defines a narrower fallback;
- unavailable Mask therefore propagates through required Mask→Mask operations;
- fail-open full coverage is **not** implemented by manufacturing an upstream white Mask;
- the optional coverage-use rule belongs to the eventual generic adjustment/layer consumer;
- requesting an unavailable Mask for inspection returns unavailable + diagnostics.

Step 6 should prove this with test-only Mask operations even though the production Mask application path waits until Step 8.

## Mask-generator bypass test operation

A test-only source-like Mask generator is sufficient to prove:

- valid/absent Reference frame → real white Mask;
- unavailable connected Reference → unavailable Mask;
- white through Invert → black;
- unavailable through Invert → unavailable.

Do not ship Radial merely to satisfy this test.

## Actual dimensions

Every evaluated raster carries its actual dimensions.

Add a deterministic size-changing test operation.

Verify downstream operations receive the changed dimensions.

No implicit resampling or alignment.

## Phase 3 tests

Minimum evaluator tests:

1. **Linear equivalence**
   - equivalent linear graph produces the same pixels as the old serial path for current unary processors;
   - disabled/missing unary behavior remains equivalent.

2. **Storage-order independence**
   - shuffle node storage and obtain identical result.

3. **Explicit output**
   - `documentOutput` may point to an intermediate;
   - unreachable stored nodes do not execute.

4. **Requested endpoints**
   - evaluate Image output;
   - evaluate Mask output;
   - neither changes `documentOutput`.

5. **Fan-out / shared upstream**
   - counting node feeding two consumers executes once.

6. **Multi-output identity**
   - test node with two named outputs proves endpoint identity matters.

7. **Slot dependency**
   - test slotted operation traverses repeated Slot inputs in semantic vector order.

8. **Availability**
   - unavailable Mask direct to test optional coverage consumer;
   - unavailable Mask through Mask→Mask operation;
   - shared unavailable Mask;
   - missing unary Image processor pass-through remains available + warning.

9. **Generator bypass**
   - real white with resolved frame;
   - white through Invert becomes black;
   - unavailable Reference produces unavailable Mask.

10. **Dimensions**
    - size-changing output supplies actual dimensions downstream;
    - valid wrong-sized Mask at a test pixelwise consumer hard-fails;
    - preview-compatible/export-incompatible fixture validates independently.

11. **Lifetime**
    - shared values remain alive through all consumers;
    - values are releasable after their last consumer.

12. **Cancellation**
    - cancellation between graph operations suppresses further work and publication.

## Review gate

At this point there must be only one generic evaluator concept. Do not retain `renderChain` as a second independent execution model.

---

# Phase 4 — Runtime, preview, inspection and export cutover

## Purpose

Make RenderRuntime, preview/display and export consume the new evaluator while preserving the renderer-ownership work from Steps 1–4.

## RenderRuntime

Keep existing ownership:

- one preview worker;
- export lease/exclusion;
- DocumentMutation drain/gate behavior;
- cancellation/supersession;
- shutdown/join;
- display-refresh ownership.

Do not add graph-specific workers or queues.

## Preview demand

Preview requests normally evaluate `documentOutput`.

Inspection may request another Image/Mask `OutputRef`.

The requested endpoint is demand state and must be captured consistently for one evaluation/publication.

## Publication identity

Publish together:

- requested endpoint identity;
- value kind: Image or Mask;
- processed value/result;
- diagnostics;
- request/supersession identity already needed to avoid stale publication.

A stale Mask result must not be presented/recoloured as though it were the currently requested Image, and vice versa.

## Display

Image display continues through the existing output/display conversion boundary.

For Step 6:

- `Source.image` uses the document input interpretation;
- other processed Image endpoints use the existing output interpretation unless/until a later product decision changes that;
- Mask inspection uses presentation-only grayscale and never Image colour conversion.

Do not introduce per-value automatic colour metadata.

## Source-only behavior

Preserve the current product behavior during the architecture migration:

- source-only viewer works;
- source-only export remains disabled.

This is no longer implemented through `nodes.empty()`; it follows explicit Source / requested endpoint state.

## Export

Export:

- requests full-resolution `documentOutput`;
- uses the same evaluator;
- refuses hard graph/value errors;
- may succeed with reachable fail-open/pass-through warnings;
- reports those warnings in final status;
- does not scan every stored node indiscriminately for warnings.

## Node-owned mutable controls

New graph-owned mutable state must be race-free.

Structural edits use DocumentMutation.

Parameter-like node state such as future Strength/Blend/slot settings:

- remains blocked during export under the existing editability policy;
- triggers preview;
- is read using atomics/locking or a consistent per-evaluation snapshot.

Do not invent feature-specific locking.

## Phase 4 tests

Add/adapt tests for:

- preview evaluates explicit `documentOutput`;
- inspection Image and Mask endpoints use the same runtime;
- switching requested endpoint while older work is active cannot publish/present stale kind/context;
- display refresh does not rerun processors when the processed requested result remains valid;
- source-only preview remains correct;
- source-only export remains disabled;
- export uses full-resolution document output;
- fail-open/pass-through warnings survive successful evaluation/export;
- warnings include reachable dependencies only;
- hard errors refuse export;
- graph mutation waits for active evaluation;
- cancellation before downstream work;
- export exclusion;
- shutdown with evaluator work outstanding;
- exception/unwind behavior remains safe.

## Review gate

Re-run the established Step 1–4 runtime regression/stress tests. Any regression in lifetime/exclusion/cancellation is a Step 6 blocker.

---

# Phase 5 — List View, ID migration and persistence/transfer boundary

## Purpose

Move the current user-facing linear editing workflow onto explicit topology without pretending Step 6 has solved arbitrary-DAG UI.

## List View

Derive the main Image spine from `documentOutput` upstream through unary `image` inputs toward Source.

The row list is presentation.

Use NodeId for:

- selection;
- ImGui IDs;
- drag payloads;
- commands.

Do not use vector index as document identity.

Nodes outside the main Image spine may be shown separately/minimally or remain inaccessible from the current simple UI. Their storage order is never execution order.

## Current editing commands

Translate current actions into topology operations:

- add node → insert/splice;
- paste node → insert/splice;
- apply linear preset/grade → construct/rebind a graph;
- move row → topology rewrite;
- delete row → unary-spine delete semantics;
- bypass/enable → node state, with dependencies evaluated according to bypass rules.

## V2 representability boundary

Sidecar V2 remains usable temporarily **only** when the current document state is losslessly representable by V2.

Define one explicit predicate, for example:

```text
isV2Representable(graph)
```

A V2-representable document should require at least:

- exactly one primary Source;
- one simple unary Image chain from Source;
- no branch/fan-out that changes document semantics;
- no Mask values/connections;
- no repeated Slots;
- no graph-native operation V2 cannot encode;
- `documentOutput` is the end of that serial chain;
- no generic graph control/state whose meaning V2 would lose.

When representable:

- serialize processors in derived spine order.

When not representable:

- sidecar save/autosave refuses without truncating/replacing an existing file;
- grade copy refuses;
- preset save refuses;
- any other V2-based transfer refuses;
- UI/status explains why rather than silently flattening topology.

Step 7 replaces this temporary boundary with graph-native persistence.

## V2 load

Loading an existing V2 chain synthesizes:

```text
Source → node1 → node2 → ... → documentOutput
```

Node-array order is used only to interpret the historical V2 file at load time.

Once loaded, the in-memory document is the explicit graph.

## Copy/paste within Step 6

Do not build the complete Step 7 subgraph serialization system prematurely.

Existing V2-backed node/full-grade/preset workflows may continue for V2-representable graph states only.

Where Step 6 implements an in-memory node duplication/splice, use fresh Node IDs and graph commands.

## Phase 5 tests

Add tests for:

- V2 linear load builds correct explicit topology;
- V2 linear save writes derived spine order;
- node storage order does not affect V2 output;
- non-linear graph refuses V2 save;
- Mask graph refuses V2 save;
- non-terminal `documentOutput` with stored downstream nodes refuses V2 save if V2 would lose meaning;
- refusal leaves existing sidecar/file unchanged;
- autosave obeys the same guard;
- clipboard/full-grade/preset paths obey the same guard;
- List View selection survives storage reorder;
- row move changes graph topology/result rather than node storage semantics;
- row delete correctly retargets `documentOutput`;
- Source cannot be deleted/duplicated;
- node IDs, not indices, drive UI commands.

## Review gate

Search for remaining uses of list/vector index that still affect processing semantics.

---

# Phase 6 — Remove the old serial authority and harden Step 6

## Purpose

Finish the transition cleanly.

## Required cleanup

By the final Step 6 commit:

- preview no longer executes `App::nodes` in vector order;
- export no longer executes `App::nodes` in vector order;
- `nodes.empty()` is not the semantic Source/output switch;
- `processor == nullptr` is not the node-kind test;
- Source dimensions are not broadcast as though every intermediate has Source size;
- List View move/delete/insert do not directly define render order;
- transitional `NodeInput` / `NodeInputRole` / reserved opacity/composite placeholders are gone or fully superseded;
- old `renderChain` is removed, renamed/reimplemented as the graph evaluator, or retained only as a thin compatibility helper that delegates to the one authoritative evaluator.

There must be no second hidden serial processing model.

## Final regression matrix

At minimum the Step 6 PR should cover:

### Graph / identity
- serial graph;
- fan-out;
- join;
- fixed + Slot consumer;
- cycles;
- Reference cycle;
- explicit output;
- storage-order independence;
- Slot IDs `[3,1,2]`.

### Evaluation
- serial pixel equivalence;
- shared upstream once;
- actual intermediate dimensions;
- Image and Mask requested endpoints;
- availability propagation;
- generator bypass;
- hard dimension mismatch;
- preview/export per-resolution validation;
- cancellation;
- intermediate lifetime.

### Runtime
- DocumentMutation exclusion;
- export exclusion;
- stale-request publication;
- display refresh;
- shutdown;
- exception cleanup.

### UI
- NodeId selection;
- graph-splice insert/move/delete;
- Source invariants;
- `documentOutput` retargeting.

### Persistence boundary
- V2 load → graph;
- safe linear V2 save;
- unsupported graph refusal;
- autosave / clipboard / preset guard consistency.

## Validation run

Before merge:

- build Release;
- full `--selftest`;
- repeat self-test enough times to expose nondeterministic scheduling/lifetime issues;
- ASan;
- TSan where supported and meaningful;
- existing render/export stress harness;
- macOS functional pass;
- Linux CI.

If sanitizer noise is pre-existing and known, separate it from new Step 6 findings explicitly.

## Final independent review

Before merge, ask a fresh reviewer to inspect:

- one-authoritative-graph guarantee;
- runtime/lifetime safety;
- evaluator result lifetime;
- cancellation/publication;
- List View not retaining hidden execution order;
- V2 refusal boundary;
- no accidental SpatialDomain / colour-metadata / cache / parallelism scope creep.

The review should answer whether Step 6 can merge, not reopen Step 5 product decisions without a concrete contradiction.

---

# Codex task sizing

Do not hand all of Step 6 to Codex in one implementation prompt.

Recommended task sequence on the same Step 6 branch:

## Task 6A — graph vocabulary

Implement Phase 1 only.

Stop after:

- types/interfaces/document graph state;
- Source/documentOutput;
- Slot identity;
- transitional-field cleanup necessary for this phase;
- focused tests.

No evaluator cutover.

## Task 6B — topology commands

Implement Phase 2 only.

Stop after:

- consumer-owned connections;
- graph commands;
- validation/cycles;
- unary-spine splice semantics;
- tests.

No preview/export cutover.

## Task 6C — evaluator

Implement Phase 3 only.

Stop after:

- requested-endpoint sequential evaluator;
- typed Image/Mask values;
- availability/error/cancel states;
- shared upstream;
- actual dimensions;
- test-only Mask/slot operations;
- tests.

The evaluator may still be invoked primarily by tests at this commit.

## Task 6D — runtime cutover

Implement Phase 4.

This is the decisive execution cutover:

- preview and export use the DAG evaluator;
- inspection endpoint demand/publication;
- display Image/Mask handling;
- diagnostics/warnings;
- runtime regression tests.

After this commit there must not be a second independent serial evaluator.

## Task 6E — List View + V2 boundary

Implement Phase 5.

Migrate existing UI actions to graph commands and enforce the V2 representability/refusal rule consistently.

## Task 6F — cleanup / hardening

Implement Phase 6.

Remove obsolete serial assumptions, run sanitizer/stress validation, and prepare the whole Step 6 PR for independent review.

---

# Review workflow

For each Codex task:

1. Codex implements only that task and reports exact files/tests.
2. Build/self-test before review.
3. Claude independently reviews the diff against this plan and the Step 5 contract.
4. Fix concrete correctness/architecture findings in the same task branch/chat.
5. Continue to the next task only when the previous task's boundary is clean.

For the final Step 6 PR, perform one fresh whole-PR review rather than assuming the sum of per-task reviews proves the complete transition.

## Stop conditions

Pause Step 6 and return to architecture discussion only if implementation reveals one of:

- a required graph cannot be represented by the accepted Node/Port/Slot model;
- a settled Step 5 rule produces contradictory evaluator behavior;
- the sequential DAG cannot fit safely inside current RenderRuntime/DocumentMutation ownership;
- preserving one authoritative graph would require a materially different migration;
- a supposedly deferred decision is genuinely required for generic DAG correctness.

Ordinary code movement, test harness work, UI rewiring, persistence refusal, or backend adapter fixes are not reasons to reopen Step 5.
