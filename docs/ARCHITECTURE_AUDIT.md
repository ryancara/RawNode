# RawNode Architecture Audit and Refactor Plan

**Status:** Original audit and post-Step-4 whole-architecture checkpoint
complete, independently reviewed and reconciled. Step 5 is current
product-contract decision work. The approved target architecture lives in
`docs/ARCHITECTURE.md` and accepted decisions live in `docs/DECISIONS.md`.

**RawNode audit baseline:** `07808451f72bba9bc59a6e9a6889ec5a4f7c07de`

**vkdt reference used by the audit:** `hanatos/vkdt @ e2ebdd3e65ab39f8f7e9d30030f299fd725d0f2a`

This document records the evidence and recommendations from the RawNode <-> vkdt
architecture audit, the independent Claude plan review, and the completed
post-Step-4 Codex and Claude architecture checkpoint.

It is retained as the audit trail rather than the authoritative architecture.
Where this file differs from `docs/ARCHITECTURE.md` or `docs/DECISIONS.md`,
those reviewed documents take precedence.

The independent review agreed with the core diagnosis and destination but judged
the original ten-step migration mildly too large. The reconciled plan keeps the
same architectural direction while removing speculative intermediate
abstractions.

## Original audit conclusion

At the original audit baseline, the recommendation was a **staged architectural
refactor before masks and compositing**, not a rendering-engine rewrite.
These findings describe the original audit baseline. Some were resolved by
Steps 1–4 and the stabilisation work; others intentionally remain as target
architecture or later product-contract work, including the serial chain and
incomplete branch image/spatial/colour semantics. The completed checkpoint below
records their current disposition.

The original baseline's foundations were broadly healthy:

- the processor abstraction already supports native, OFX and CTL backends;
- nodes have clear unique ownership of processors;
- preview and export use the same evaluator;
- the processed floating-point image is retained separately from display bytes;
- Sidecar V2 preserves stable node identities and missing processors;
- PRs #32-#34 established valuable renderer lifetime and scheduling guarantees;
- there is no large cache/tile/job architecture that needs to be dismantled.

The original problems were architectural boundaries rather than a broken core:

- the current "graph" is still an ordered vector chain;
- renderer safety depends on callers knowing the wait/mutate/reschedule protocol;
- export protects structural lifetime but does not define a fully frozen
  parameter/document state;
- image values do not yet carry enough colour, alpha and spatial semantics for
  independently processed branches to combine safely;
- export borrows live application state from a detached thread;
- cancellation, freshness, execution ownership and status policy are related but
  distinct concerns and should not be collapsed carelessly.

The existing pending/busy/mutation flags are not evidence that the renderer
needs to be thrown away. Most protect real invariants. The goal is to hide and
simplify their use before deciding whether their representation should change.

## Architectural direction

vkdt is useful as a reference for:

- explicit graph connections;
- named inputs and outputs;
- fan-out and dependency-based evaluation;
- sink-oriented processing;
- clear graph/resource ownership;
- separation between processing results and display presentation.

RawNode should **not** copy vkdt's Vulkan-specific machinery, two-layer execution
model, run-flag system, allocator complexity or export job architecture unless a
future RawNode requirement independently justifies them.

The working target remains conceptually:

    Image source
         |
         v
    Processing graph
         |
         v
     Graph result
       /      \
      v        v
   Display    Export
    sink       sink
      |
      v
    Viewer

This is a responsibility model, not a requirement for a large class hierarchy.

The desired property is that features state what changed while graph/rendering
infrastructure owns mutation safety, lifetime, invalidation and scheduling.

## Correctness baseline that must survive refactoring

The behaviour established by PRs #32-#34 is the minimum safe baseline.

A refactor must preserve, or deliberately replace with stronger guarantees:

- preview rendering never using processors that are concurrently destroyed or
  replaced;
- graph mutation waiting for active processor use to finish;
- full-resolution export safely owning processor execution;
- preview and export not unsafely using the same mutable processor instances;
- display-only refresh not consuming or cancelling required processor work;
- processor rendering taking priority over display-only refresh when both are
  requested;
- idle/cancellation barriers actually cancelling the work they promise to
  cancel and waiting for active ownership to drain;
- export completion restoring a current preview;
- export success/warning/error status surviving the restored preview;
- failed or cancelled work not resurrecting obsolete requests;
- shutdown draining real execution owners before application/processor state is
  destroyed.

At the original baseline, these limits were recorded. Steps 1–2 have since
replaced detached export lifetime with owned execution and frozen parameter
edits during export; the control-thread and cooperative supersession limits
remain. Do not read this historical list as the current export contract:

- parameter edits can occur while processing is active;
- export currently has structural exclusivity, not an immutable document
  snapshot;
- supersession is cooperative rather than immediate;
- the current mutation APIs assume a serialized control-thread calling pattern;
- export execution is detached and does not have one encompassing RAII
  ownership/exception guard.

## Reconciled staged migration after independent review

Claude independently verified the renderer, graph, export, OFX, persistence and
test paths and agreed that a refactor is justified but that the original plan
could be smaller.

The key refinement is:

> The primary architectural debt is the caller-managed renderer protocol, not
> the mere existence of renderer flags.

Moving flags into a coordinator before reducing that protocol would mostly move
complexity rather than remove it.

The reconciled sequence is therefore:

### Step 1 - Own the export job

Replace detached export lifetime with owned execution and scope-based cleanup.

Split UI/dialog concerns from a synchronous production export-job body so tests
exercise the real path rather than hand-reimplementing it.

Preserve #34 preview restoration and status behaviour.

Do not add snapshots, cloned processors or background-export concurrency.

### Step 2 - Enforce export consistency simply

An export represents the accepted document state when Export is confirmed.

While export owns the live processor instances, parameter editing is
unavailable.

This avoids introducing a snapshot/staging system merely to solve consistency
that can currently be handled by serialization.

### Step 3 - Centralize graph-edit transactions

Remove ordinary caller responsibility for:

    wait for renderer
    mutate graph/document
    schedule preview

Add/delete/reorder/bypass/restore operations should cross one document/graph
mutation boundary.

Nested operations should produce one final preview request.

Failure paths that must restore a valid preview should do so quietly so the
meaningful error/status remains visible.

This is the right boundary for the known failed-add and failed-RAW-reload stale
preview cases.

### Step 4 - Encapsulate renderer ownership

Only after the external protocol has shrunk should renderer state move behind a
dedicated runtime/owner.

That boundary should own worker lifecycle, execution exclusivity, image/display
demand, cancellation, shutdown and safe publication.

Internal pending/busy flags may remain if they are still the simplest correct
representation.

A deliberate test observation seam should replace tests reaching directly into
private renderer fields.

### Post-Step-4 stabilisation gate

Implementation/review evidence from Step 4 added a useful checkpoint before
graph expansion.

The independent PR #45 review found the RenderRuntime ownership model sound and
also exposed two pre-existing OpenFX host multithread lifetime bugs unrelated to
the renderer refactor:

- host `multiThread()` can return before every scheduled slice has completed,
  allowing stale workers to outlive the render action;
- the global OpenFX host worker pool is not joined before static destruction.

These were corrected in focused PR #48 rather than folded into renderer
ownership work. Independent review found the generation/acknowledgement model
sound, and macOS validation passed before merge.

The behaviour-preserving human-readability/source-organisation pass is
complete in PR #50 after independent review and macOS validation. Its candidate
questions were examined by the completed checkpoint below. This gate does not
add numbered architecture steps.

### Completed post-Step-4 whole-architecture checkpoint

**Reviewed architecture basis:** the post-#50 state used for the independent
checkpoint, before the immediate follow-ups landed.

**Checkpoint closure state:** `fd0f1a750d8ce84981839ef074bc1fead5aad410`
includes follow-ups #53 and #54; it is not the exact revision inspected by both
architecture reviewers.

**Pinned conceptual reference:**
`hanatos/vkdt @ e2ebdd3e65ab39f8f7e9d30030f299fd725d0f2a`.

Codex and Claude independently reviewed RawNode's post-#50 implementation and
approved target against its actual product goals, production code and
renderer/lifetime constraints, and the pinned vkdt reference:

- **Codex independent verdict: SOUND FOR THE NEXT STAGE.**
- **Claude independent verdict: SOUND FOR THE NEXT STAGE.**
- **Shared conclusion:** RawNode's architectural direction is sound for the
  next stage. No further broad architectural refactor is justified before
  Step 5. Small focused implementation follow-ups remain, without an
  architecture blocker.

The checkpoint found no evidence that another broad subsystem must first be
moved, split or redesigned merely to resemble vkdt more closely. Designing
RawNode today for its actual goals would lead to substantially the same
high-level responsibilities:

    UI -> Document / Graph API
             |             |
       Persistent       Render runtime
        document           |
             +-------------+
                    |
             Graph evaluator
                    |
             Processed result
                /       \
             Display   Export

App remains a practical composition root. The document/graph API owns edit
meaning and topology; backend-neutral processors remain image operations.
RenderRuntime owns execution lifetime, demand, cancellation and
preview/export/edit exclusion. Display and export consume processed results.

**Current implementation is not the final graph architecture.** Today
`App::nodes` is still evaluated as an ordered serial chain. Step 5 settles the
first mask/graph product contract; Step 6 deliberately introduces explicit
topology and executable evaluation together. This verdict is scoped to the
next stage, not a claim that the architecture is finished or permanently valid.

#### vkdt reconciliation

vkdt is a conceptual reference for graph/data-flow structure, not an
implementation blueprint to copy wholesale. The approved target adopts one real
processing graph, stable node identity, named endpoints/ports, explicit upstream
connections, typed/role-aware image and mask data, fan-out, dependency-ordered
evaluation, shared upstream evaluation once per result, graph-native mask/blend/
compositing operations, and display/export consumers.

Several of these remain target capabilities. Step 6 must implement explicit
topology, named image/mask ports and upstream endpoint references, cycle
rejection, required/optional input validation, one authoritative document
output, sequential topological/shared-upstream evaluation and per-evaluation
intermediate-buffer lifetime. These are the generic topology/evaluator
capabilities required by the Step 5 contract. Any concrete multi-input mix/blend
operation belongs in the implementation step selected by that contract, not
automatically in Step 6. The first DAG retains whole-graph invalidation.

Deliberately deferred unless a concrete requirement or measurement justifies
them: Vulkan as an application architecture requirement, GPU-resident graph
execution, persistent per-node caches, branch-specific invalidation, parallel
branches, tile/ROI schedulers, general job systems, multiple renderer worker
pools, automatic processor cloning, simultaneous exports, temporal/feedback
graphs and speculative revision frameworks. RawNode should remain smaller where
its product does not need vkdt's execution complexity.

#### Candidate dispositions and focused follow-ups

These reconcile the formerly open PR #50 candidates with the completed reviews.
They do not introduce new architecture decisions beyond the accepted target and
D019–D038.

| Candidate | Completed checkpoint disposition |
| --- | --- |
| App ownership | Keep App as an intentional lightweight composition root. Extract ownership only for a concrete feature/lifetime benefit, not architectural purity. |
| Document edit boundary | Structural edits correctly use `DocumentMutation`. Parameter/output edits intentionally differ today. Move UI intent toward ID-addressed document APIs in Step 6; no preliminary refactor is required. |
| Thumbnail worker lifetime | Normal UI shutdown is safe; exceptional unwind has a latent lifetime issue. Future focused work should provide explicit RAII stop/join ownership. Not fixed and not a Step 5 blocker. |
| macOS thumbnail pthread handoff | The pthread-to-`std::thread` joiner handoff has a narrow exception ownership gap. Direct pthread handle ownership/joining is a possible focused fix; still open and non-blocking. |
| `makePreview` helper-thread launch | If a later `std::thread` launch throws, earlier joinable threads can cause termination. Partial-launch exception safety remains focused work, not an architecture blocker. These three lifetime items may share one future PR. |
| Evaluator/display separation | Make the graph evaluator an explicit seam when topology lands in Step 6. Display remains after the processed-image boundary; no separate preliminary refactor. |
| `DocumentActions` / UI coupling | Acknowledged placement debt. Revisit when persistence/document APIs consolidate, probably Step 7; do not move files solely for cleanliness. |
| Runtime/display/colour locking | No lock-order cycle or deadlock was found in the reviewed paths. Some critical sections could be shorter; no pre-Step-5 lock redesign. Leaf App locks must not call back into RenderRuntime while held. |
| Reserved/shadow graph fields | Transitional only. Do not extend them into a second graph model. Remove or redefine unused fields as real topology and evaluation land together in Step 6. |
| Source/export sidecar question | Superseded by D038 and PR #53: sidecars belong only to editable source documents; exported derivatives receive none. There is no future source/export capture-convergence task. |
| `runExportJob` test seam | Retain the useful synchronous production export test seam; no present reason to remove or redesign it. |
| Single-level mutation-completion regression | Complete in PR #54. Changed edits queue normal demand and interrupted unchanged edits queue quiet recovery before the mutation gate opens; deterministic coverage now protects both. |
| `uploadTexture` | Dead path remains implementation cleanup. Remove when Step 6 touches the evaluator/display boundary unless a use appears earlier; not a Step 5 blocker. |
| Alpha | Global alpha association remains deliberately unresolved; masks are not image alpha. Source alpha differs across formats. Revisit TIFF, EXR, OpenFX and JPEG flattening before transparent compositing/alpha-carrying workflows, not as an invented Step 5 global contract. |
| Spatial semantics | Preview/full-resolution pixel-unit and render-scale differences need a contract before spatial masks/native operations. If the first mask is geometric, Step 5 should decide coordinates, preferably resolution-independent document semantics. |
| Persistence reliability | Source sidecars still use in-place writes, not atomic replacement. Persistence hardening belongs with Step 7 and does not block Step 5; not fixed. |
| Per-image GUI preferences | Sidecar V2 still contains theme/panel/preview preferences. Step 7 should move toward document state rather than unrelated application presentation state; no format change here. |
| Export over an editable raster source | PR #53 review identified the separate pre-existing hazard that export to the source's exact path can overwrite the source image. This remains an export-safety follow-up, unrelated to sidecars; no export redesign here. |

PR #53 and PR #54 closed the two immediate checkpoint follow-ups. The remaining
lifetime/exception-safety findings are focused implementation work, not reasons
to delay Step 5 or reopen broad architecture cleanup.

### Step 5 - Settle the first mask/graph product contract

**Current task: decision work before implementation.** Settle the first
mask/graph product contract before DAG feature work; the pending topics and
graph-model guardrails are in `docs/PROJECT_CONTEXT.md`.

Step 5 settles the product/architectural contract needed before topology work:
the user-visible mask model and its list/graph attachment, ownership/meaning of
opacity and effect strength, multi-input behaviour, coordinate semantics if the
first mask is geometric, and enough mixing semantics to define the required
graph shape. D034's mask/image-alpha separation is already accepted; their
interaction remains open. No actual contract answer is chosen here.

The internal architecture must remain graph-capable and must not create a
feature-specific mask scheduler.

Graph-native masks remain the preferred direction for eventual compositing, but
the exact first mask UX is not yet an approved implementation contract.

### Step 6 - Introduce topology and sequential DAG evaluation together

Do not introduce topology that nothing executes.

Add the smallest useful graph model and evaluator in the same architectural
step:

- stable node IDs;
- named ports;
- simple image/mask roles;
- explicit connections;
- fan-out;
- cycle rejection;
- one document output;
- sequential dependency evaluation.

Preserve bit/behaviour equivalence for linear graphs first.

Keep OpenFX unary initially.

### Step 7 - Add versioned graph persistence

Persist explicit topology only when the runtime can execute it.

Older builds must never silently reinterpret branching documents as a linear
chain.

Preserve stable identities, missing processors and copy/paste compatibility.

### Step 8 - Settle remaining concrete mask details and implement the first mask path

Implement the first mask path under the Step 5 product/architectural contract.
Settle remaining lower-level details that were not needed before topology work:
concrete mask representation, range, filtering/sampling, concrete blend encoding
where not already required by Step 5, and implementation details of the selected
path. Do not postpone Step 5's required ownership, graph-shape or geometric
coordinate decisions to this step.

A global image-alpha contract remains separate and may wait until transparent
compositing or alpha-carrying I/O requires it.

### Changes from the original Codex plan

The independent review specifically recommended:

- moving export ownership first;
- merging export consistency into the early export work rather than designing a
  snapshot system;
- moving renderer encapsulation until after graph-edit and export call protocols
  shrink;
- deferring processed-result identity/revision work until a concrete need exists;
- merging topology and executable DAG traversal;
- treating the broad revision/evaluation-key model as analysis vocabulary, not
  a target architecture;
- keeping whole-graph invalidation and sequential execution;
- deciding the first mask model before DAG feature work.

The destination remains the same: one processing graph, simple execution,
display/export consumers, and feature code that does not know renderer
internals.

## What is required before masks/compositing

Before branching, masks and compositing become production features, RawNode
should have explicit decisions or infrastructure for:

- executable topology with named endpoints and cycle rejection;
- colour-image versus mask port semantics;
- image extent/origin/dimension semantics;
- processor lifetime and graph mutation ownership;
- evaluation/parameter consistency;
- branch colour compatibility;
- alpha and opacity semantics;
- topology persistence/migration;
- failure/bypass behaviour for missing multi-input nodes;
- per-evaluation shared-buffer lifetime.

Node "opacity" must also be defined carefully. Effect strength and layer
compositing are not the same operation.

## What should explicitly be deferred

Do not build these as part of the architectural cleanup unless new evidence
shows they are needed:

- persistent per-node caches;
- branch-specific invalidation;
- dependency hashes for every parameter;
- parallel branch execution;
- tile schedulers;
- comprehensive ROI scheduling;
- a general-purpose job system;
- multiple renderer worker pools;
- automatic cloning of all processors for export;
- multiple simultaneous exports;
- temporal/feedback graphs;
- Vulkan as an architectural requirement;
- GPU-native graph-result storage;
- automatic colour-space inference for arbitrary OFX/CTL/DCTL code;
- a LibRaw/developer rewrite.

Prefer whole reachable-graph rerendering until profiling and real workflows prove
that finer-grained machinery is worth its complexity.

## Revision/invalidation note

The audit found revisions potentially useful but **not sufficient by themselves**.

Freshness, work demand, cancellation, execution ownership and status policy are
different concepts.

A future model may distinguish:

- image/document revision;
- evaluation key (state + output + resolution/quality);
- presentation key;
- cancellation epoch/token;
- request/status policy.

Do not replace the current scheduler with "result is stale, therefore render"
without handling failure, cancellation, export restoration and quiet status
semantics.

A stale last-good result may intentionally remain displayed without creating an
automatic retry loop.

## Question reconciliation

### Early-refactoring questions — settled

The original questions below are answered by D022 (one control thread), D024
(accepted export state and frozen parameter edits), D032 (eventual-replacement
supersession), manual output interpretation, D025 (quiet recovery/status
preservation), and D036 (authoritative document paths). They are
historical audit prompts, not open checkpoint prerequisites.

- Is the serialized control-thread assumption an explicit RawNode contract?
- What accepted state does export promise to represent?
- May parameter edits block during export, or must they be staged?
- Does supersession merely promise eventual replacement, or must obsolete work
  never publish after supersession?
- Does the current output tag remain a manual interpretation control?
- How should export status and routine preview progress be owned/separated?
- Which architecture/decision document paths are authoritative?

### Must settle before masks/compositing

- What is RawNode's alpha contract, including hidden colour at zero alpha?
  This remains separate from masks and is needed before transparent compositing
  or alpha-carrying workflows.
- What exactly does node opacity mean?
- In which encoding do blend modes operate?
- How are colour-encoding mismatches between branches handled?
- What are image extent/origin/preview-scale semantics?
- What are mask range/filtering semantics?
- What does bypass mean for a multi-input node?
- What happens if a mask/compositor backend is unavailable?
- Are OFX multi-clip effects an initial requirement?
- How are topology and asset references represented in sidecars and copied
  subgraphs?

### Safe to defer

- per-node caches;
- selective branch invalidation;
- parallel branches;
- separate preview/export processor instances;
- tile/ROI scheduling;
- backend fusion;
- multiple simultaneous exports;
- temporal graphs;
- broader plugin capability negotiation.

## Original independent plan review result

The original independent Claude plan review is complete. Its migration-plan
verdict below predates the completed post-Step-4 architecture checkpoint and
should not be confused with the shared current verdict above.

Its overall verdict was **TOO LARGE (mildly)**: the audit's diagnosis and target
were judged correct, but several intermediate steps were unnecessary.

The review confirmed these core conclusions:

- no renderer rewrite is needed;
- caller-managed wait/mutate/reschedule sequencing is the highest-value coupling
  to remove;
- export ownership and production-path test coverage should be fixed first;
- renderer encapsulation should follow, not precede, protocol simplification;
- a simple sequential DAG is sufficient for the first branching architecture;
- vkdt is useful for explicit topology and graph-mask concepts, not as a system
  to port wholesale;
- caches, ROI/tile systems, parallel branches and job systems remain premature;
- alpha should remain a separate pending decision from local-adjustment masks.

Ryan + ChatGPT reconciled the audit and review and approved the architecture in
`docs/ARCHITECTURE.md` and `docs/DECISIONS.md`.

No production architecture refactor had begun when the original audit was
approved. Subsequent implementation should be judged against the authoritative
architecture/decision documents plus the post-Step-4 stabilisation evidence
recorded above.
