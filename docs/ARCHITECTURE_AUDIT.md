# RawNode Architecture Audit and Refactor Plan

**Status:** Audit complete, independently reviewed and reconciled. The approved
target architecture now lives in `docs/ARCHITECTURE.md` and accepted decisions
live in `docs/DECISIONS.md`.

**RawNode audit baseline:** `07808451f72bba9bc59a6e9a6889ec5a4f7c07de`

**vkdt reference used by the audit:** `hanatos/vkdt @ e2ebdd3e65ab39f8f7e9d30030f299fd725d0f2a`

This document records the evidence and recommendations from the RawNode <-> vkdt
architecture audit and the later independent Claude review.

It is retained as the audit trail rather than the authoritative architecture.
Where this file differs from `docs/ARCHITECTURE.md` or `docs/DECISIONS.md`,
those reviewed documents take precedence.

The independent review agreed with the core diagnosis and destination but judged
the original ten-step migration mildly too large. The reconciled plan keeps the
same architectural direction while removing speculative intermediate
abstractions.

## Executive conclusion

The audit recommends a **staged architectural refactor before masks and
compositing**, not a rendering-engine rewrite.

RawNode's current foundations are broadly healthy:

- the processor abstraction already supports native, OFX and CTL backends;
- nodes have clear unique ownership of processors;
- preview and export use the same evaluator;
- the processed floating-point image is retained separately from display bytes;
- Sidecar V2 preserves stable node identities and missing processors;
- PRs #32-#34 established valuable renderer lifetime and scheduling guarantees;
- there is no large cache/tile/job architecture that needs to be dismantled.

The main problems are architectural boundaries rather than a broken core:

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

The current implementation also has limits that should not be accidentally
described as stronger guarantees than they are:

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

The next stabilisation task is the behaviour-preserving human-readability and
source-organisation pass. The goal is to make the physical code communicate the
approved architecture to a human reader before topology/mask complexity arrives.
It should improve file responsibility, conceptual/top-down function order,
naming, comments and visibility of ownership/locking without changing
architecture under the label of cleanup.

Then perform the planned whole-architecture checkpoint against RawNode's product
goals and the pinned vkdt reference.

This gate does not add architecture steps; it reduces avoidable implementation
and comprehension debt before Step 5.

### Step 5 - Settle the first mask/graph product contract

Before DAG feature work, decide the first mask UX/graph model.

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

### Step 8 - Define the first mask contract and implement the first mask path

Mask representation, range, filtering, coordinate space, blend encoding and
opacity semantics should be settled with the feature.

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

## Major open questions

### Must settle before or during early refactoring

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

## Independent review result

The independent Claude review is complete.

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
