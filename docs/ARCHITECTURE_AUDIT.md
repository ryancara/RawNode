# RawNode Architecture Audit and Refactor Plan

**Status:** Proposed direction, pending independent Claude review and Ryan + ChatGPT approval.

**RawNode audit baseline:** `07808451f72bba9bc59a6e9a6889ec5a4f7c07de`

**vkdt reference used by the audit:** `hanatos/vkdt @ e2ebdd3e65ab39f8f7e9d30030f299fd725d0f2a`

This document records the durable conclusions and proposed migration plan from
the RawNode <-> vkdt architecture audit. It is intentionally shorter than the
full audit report and is meant to guide future project conversations.

Nothing in this file is approved architecture merely because it appears here.
The current next step is independent adversarial review by Claude Code. After
that review, Ryan + ChatGPT will decide which recommendations become accepted
architecture and record those decisions in the authoritative architecture and
decision documents.

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

## Proposed staged migration

The migration is intentionally incremental. Do not combine these steps into one
large renderer rewrite.

### Step 1 - Record contracts and resolve documentation authority

**Type:** Documentation only.

Document the current renderer guarantees, control-thread assumptions, current
output-tag semantics and known consistency limitations.

Resolve the current documentation ambiguity: root `ARCHITECTURE.md` and
`DECISIONS.md` contain older/aspirational material, while durable project
context expects future authoritative documents under `docs/`.

No production behaviour changes.

### Step 2 - Encapsulate renderer state without changing policy

**Goal:** Give the existing renderer state one owner/coordinator.

Move or hide the current pending/owner/gate/condition-variable state behind one
rendering boundary while preserving the current state machine exactly.

Examples of current internals that should stop leaking outward include:

    renderPending
    displayRecolorPending
    renderQuietPending
    renderBusy
    exportBusy
    renderMutationDepth
    gLatestGen

Do not introduce a revision system in this step.
Do not change scheduling policy.
Do not add DAG behaviour.

A key acceptance criterion is that all #32-#34 regression behaviour remains
unchanged.

### Step 3 - Centralize graph edit transactions

**Goal:** UI/document code no longer performs renderer protocol manually.

Route structural operations such as add, delete, reorder, bypass and chain
restore through graph-owner operations.

Ordinary callers should not need to implement:

    wait for renderer
    mutate live graph
    schedule preview

The graph/execution boundary should perform the complete safe operation.

Failure paths matter. If an operation cancels valid preview work and then fails,
it must preserve or deliberately restore the appropriate preview demand.

This is the natural place to address existing stale-preview cases around failed
node addition and failed RAW reload, rather than creating more feature-specific
scheduler state.

### Step 4 - Make processed-result publication explicit

**Goal:** Separate processed image results from presentation results clearly.

The current float `App.display` is really the last successful processed preview
result, not a display-encoded image.

Introduce a clear processed-result concept while preserving current behaviour.

A result should eventually be able to identify at least:

- which accepted image/document state produced it;
- requested output;
- resolution/quality;
- pixel interpretation/semantics.

Keep display bytes as a derived presentation result.

Do not build a per-node cache.

The audit identified a possible late-publication window when supersession occurs
during display conversion. Reproduce it deterministically before changing
behaviour.

### Step 5 - Own the export lifecycle

**Goal:** Remove detached borrowing of live application state.

Encapsulate export acquisition, execution, cleanup, preview restoration and
status handling in an owned lifecycle with scope-based cleanup.

Preserve current export pixels and status behaviour.

This step is about ownership and cleanup, not yet about creating cloned processor
graphs or parallel preview/export execution.

### Step 6 - Decide evaluation/export consistency

**Goal:** Make parameter consistency an explicit product contract.

This is a decision point, not just mechanical refactoring.

Questions include:

- What document state does an export promise to represent?
- Are parameter edits allowed to take effect during export?
- Should edits block, queue, or be staged while export owns processors?
- Should future evaluations use immutable parameter snapshots?
- Must export pixels and the captured sidecar always describe the same accepted
  state?

Do not silently inherit inconsistent backend behaviour.

A conservative first policy may be to keep processor state stable for a complete
evaluation and delay conflicting edits while export owns the instances. This
must be approved before implementation.

### Step 7 - Introduce explicit topology while preserving linear behaviour

**Goal:** Establish graph data structures before changing graph execution.

Add stable named endpoints/connections and topology validation while keeping the
current unary, list-oriented editing experience and equivalent linear execution.

Required concepts include:

- stable node IDs;
- named input/output ports;
- explicit upstream endpoint references;
- fan-out;
- cycle rejection;
- required/optional input validation;
- explicit selected result endpoint.

Existing native, CTL and OFX processors may initially remain unary adapters.

### Step 8 - Replace chain traversal with a simple sequential DAG evaluator

**Goal:** Enable branching/multiple inputs without adding scheduler complexity.

Evaluate only nodes reachable from the requested output, in dependency order.

A shared upstream node should execute once per evaluation.

Keep intermediate buffers only as long as downstream consumers need them.

This is ordinary per-evaluation buffer lifetime, not persistent per-node caching.

Keep whole-graph invalidation initially.

Do not add parallel branch execution, tiling or fine-grained cache invalidation.

### Step 9 - Add versioned graph persistence

**Goal:** Persist the topology safely.

Extend/migrate the sidecar representation to store:

- explicit connections;
- source/output port identity;
- selected result endpoint;
- future multi-input relationships.

Preserve stable node identity and missing-processor representation.

Older readers must not silently reinterpret a branching graph as a linear chain.

Copy/paste and presets will need explicit topology/ID-remapping semantics.

### Step 10 - Approve alpha/compositing contract, then begin feature work

Only after the architectural foundation is accepted should RawNode add its first
mask/compositor path.

The audit did **not** approve a permanent global alpha model.

The leading low-change candidate is:

- straight/unassociated RGBA at general processor boundaries;
- premultiplied arithmetic where compositing/filtering requires it;
- explicit adaptation at OFX and I/O boundaries.

Before approving that model, verify:

- TIFF associated/unassociated alpha behaviour;
- EXR conventions;
- representative OFX transparency behaviour and clip preferences;
- zero/near-zero alpha hidden-colour policy;
- JPEG flattening behaviour;
- viewer background/checkerboard compositing;
- fractional-alpha CST/LUT/CTL behaviour;
- transparent-edge filtering.

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

## Independent review checkpoint

Before Step 1 turns into implementation work, a fresh Claude Code session should
adversarially review this audit and plan.

That review should specifically challenge:

- whether a refactor is actually justified;
- whether the migration order is safe and minimal;
- whether any step merely moves complexity instead of reducing coupling;
- whether concurrency/lifetime guarantees are preserved;
- whether any proposed abstraction is premature;
- whether steps should be merged, split, reordered or dropped;
- whether the proposed DAG direction is appropriate for RawNode and OFX;
- which open questions truly block implementation;
- which "required before masks" items can safely be deferred.

After Claude's review, Ryan + ChatGPT should reconcile the findings. Only then
should approved architecture be written into the authoritative architecture and
decision documents and implementation begin.
