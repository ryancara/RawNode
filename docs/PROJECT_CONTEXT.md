# RawNode Project Context

This document is the durable onboarding summary for RawNode. It exists so a new
ChatGPT, Codex, Claude Code, contributor, or future development session can
understand the project's direction without relying on old chat history.

Read this document before making broad product or architecture decisions.

This is **not** a changelog. Git history and pull requests record implementation
history. Keep this document focused on the current product direction, important
settled decisions, current development phase, and information that would be
expensive to reconstruct.

## What RawNode is

RawNode is a lightweight, modular RAW photo editor written in C++17 and forked
from ofxrawhost.

The project is evolving from a simple still-image OpenFX host into a general
non-destructive photo editor built around a processor graph.

RawNode currently supports native processors, OpenFX processors and CTL. Planned
processor capabilities include DCTL and LUTs, followed later by masks,
compositing, local adjustments and a more capable branching graph.

macOS is the primary development platform today. The codebase should remain
cross-platform for macOS, Windows and Linux.

The repository's inherited README still contains substantial upstream
ofxrawhost-era material. Treat this document and current source code as more
authoritative for RawNode's present direction until the README is rewritten.

## Product direction

RawNode should remain fast, lightweight and understandable.

It is not intended to become a cloud photo service or a heavyweight catalogue
application. The current direction is:

- minimal DAM/workspace functionality rather than a central catalogue;
- image edits stored in sidecars rather than project/catalogue databases;
- a modular processor model that can host native, OFX, CTL and future DCTL/LUT
  processing;
- a node/graph model that can eventually support masks, compositing and local
  adjustments;
- serious colour-managed photographic workflows;
- cross-platform operation without making the architecture depend on one OS.

The core should stay simple enough that new image-processing features plug into
general graph/rendering mechanisms instead of creating feature-specific
subsystems.

## Development workflow

The preferred development workflow is:

1. Ryan and ChatGPT discuss product behaviour, features and architecture.
2. Once the behaviour/design is sufficiently clear, Codex implements a focused
   task and creates or updates a pull request.
3. Claude Code independently reviews the resulting implementation, normally in
   a fresh chat.
4. Ryan and ChatGPT interpret the review and decide what should change.
5. Codex implements required fixes in the same task/PR conversation when they
   belong to that PR.
6. Claude may perform a focused final verification.
7. Merge only when the implementation and review are satisfactory.
8. If a durable project decision changed, update the relevant project
   documentation before considering the work complete.

ChatGPT is the long-running product/architecture discussion partner. Codex is the
primary implementation agent. Claude Code is primarily the independent,
adversarial reviewer.

A fresh Codex chat is preferred for a new coherent PR/task. Review-driven fixes
to the same PR should normally stay in that PR's existing Codex chat.

A fresh Claude Code chat is preferred for each independent review. Persistent
Claude review rules live in the repository root `CLAUDE.md`; do not depend on
one indefinitely long Claude conversation.

## Documentation is project memory

The repository, not chat history, should be the durable source of important
RawNode knowledge.

When a significant architectural, product, workflow or roadmap decision is
settled, ask whether project documentation needs to change before the work is
considered complete.

Document responsibilities are:

- `docs/PROJECT_CONTEXT.md` — fast onboarding, product direction, current state,
  priorities, current development phase and near-term roadmap.
- `docs/ARCHITECTURE_AUDIT.md` — evidence, proposed staged refactor plan and
  unresolved architecture questions from the RawNode <-> vkdt audit. It is not
  approved architecture by itself.
- `docs/ARCHITECTURE.md` — authoritative technical architecture once the
  reviewed target architecture is approved.
- `docs/DECISIONS.md` — significant approved design decisions and the reasons
  behind them.
- `CLAUDE.md` — Claude Code's persistent review/development instructions.
- Git history and pull requests — implementation history and per-change detail.

Do not turn `PROJECT_CONTEXT.md` into a chronological diary. Do not copy every
PR into it. Update it when the information a future project conversation needs
has materially changed.

`docs/ARCHITECTURE.md` and `docs/DECISIONS.md` remain intentionally deferred
until the audit recommendations have been independently reviewed and Ryan +
ChatGPT approve the architecture we actually want to keep.

The audit also found older root-level `ARCHITECTURE.md` and `DECISIONS.md`
material that is partly aspirational/stale. Do not treat those root files as the
new authoritative architecture until this documentation ambiguity is explicitly
resolved.

## Current capabilities and baseline

RawNode has already moved substantially beyond the original ofxrawhost design.

Important implemented foundations include:

- generic processor abstraction and parameter API;
- Sidecar V2 with migration from V1, persistent node IDs and preservation of
  unavailable processors;
- native Exposure processor;
- CTL backend using the CTL reference runtime;
- CTL scalar parameters;
- unified colour encoding/registry work used by RAW decode and native colour
  transforms;
- native CST processor with colour space and transfer-function concepts;
- lcms-based colour conversion and associated tests;
- node and full-grade copy/paste plus basic preset support;
- preview pan/zoom and normal photo-editor interaction improvements;
- render lifetime/mutation synchronisation;
- distinct processor-render and display-conversion refresh behaviour;
- preview restoration after full-resolution export.

The renderer/concurrency baseline established by PRs #32-#34 is important:

- PR #32 fixed processor lifetime/use-after-free races between rendering and
  graph mutation/export.
- PR #33 prevented display-only refreshes from swallowing required processor
  renders and centralised display conversion from the cached processed image.
- PR #34 restored a current preview after full-resolution export while
  preserving export result/warning/error status.

These fixes are the safe baseline for the architecture audit. A refactor may
replace their implementation, but it must preserve their behavioural and
lifetime guarantees.

## Current renderer shape

The current renderer is still largely a linear-chain architecture inherited
from RawNode's earlier evolution.

Current machinery includes concepts such as:

    renderPending
    displayRecolorPending
    renderQuietPending
    renderBusy
    exportBusy
    renderMutationDepth
    gLatestGen

and APIs such as:

    scheduleRender()
    scheduleDisplayRecolor()
    waitRenderIdle()
    beginRenderMutation()
    endRenderMutation()
    beginFullResolutionRender()
    endFullResolutionRender()

These mechanisms currently solve real correctness problems. They are **not**
automatically the desired long-term public architecture.

Do not add more feature-specific pending domains merely by copying this pattern.
The architecture audit exists partly to determine which of these concepts should
remain, which should be hidden, which can be collapsed, and which can eventually
be removed.

## Architectural direction

vkdt is the primary architectural reference for the next phase of RawNode.

The reason is conceptual rather than implementation-specific: vkdt is lightweight,
node/graph oriented, separates processing from display concerns cleanly, and is
closer to the kind of application RawNode is becoming than a large catalogue
editor.

Do not mechanically port vkdt or assume Vulkan is required. RawNode has its own
constraints, especially OpenFX hosting, CTL, future DCTL support and
cross-platform CPU/GPU processing.

The working conceptual target is:

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

The important principles behind this model are:

- one image-processing graph rather than separate processing systems for each
  feature;
- display and export are consumers/sinks of graph results;
- masks, opacity, blending, LUTs, CTL, DCTL, OFX and native processors belong to
  image processing rather than creating their own schedulers;
- feature/UI code should report that image or display state changed rather than
  manipulate renderer internals;
- renderer lifetime, mutation, cancellation and scheduling details should be
  hidden behind a small infrastructure boundary;
- prefer re-rendering slightly too much over prematurely creating many granular
  invalidation/cache domains.

A useful conceptual distinction is currently:

    processor / graph / mask / blend change -> image result invalidated
    display / monitor conversion change     -> display result invalidated

Do not create a third or fourth invalidation category casually.

## Colour and image-processing direction

RawNode is intended for serious colour-managed photographic workflows.

Unless an operation explicitly requires otherwise:

- preserve floating-point precision;
- preserve negative RGB values;
- preserve values above 1.0;
- avoid unintended clipping;
- preserve alpha;
- keep colour-space/primaries and transfer-function/gamma concepts distinct;
- do not silently introduce display-referred behaviour into scene-referred
  processing.

Colour transforms should have explicit source and destination encodings.

RAW decode and native CST should share colour definitions/math where practical
instead of maintaining duplicate matrices or transfer functions.

The UI direction is to treat colour space and gamma/transfer function as
separate concepts. Further RAW working-space design is deliberately postponed
until after the architecture audit.

Display/monitor conversion conceptually occurs after the processed-image
boundary. A display conversion change should not require rerunning expensive
image processors when the processed image is still valid.

Before masks/compositing are implemented, RawNode also needs an explicit
decision about internal alpha representation (for example straight/unassociated
versus premultiplied) and how that maps to OFX and compositing.

## Persistence direction

RawNode uses sidecar files as persistent per-image edit state.

Important persistence principles:

- avoid a central project/catalogue database;
- maintain backwards compatibility where practical;
- processor/node identity must remain stable enough for persistence, presets and
  copy/paste;
- unavailable/missing processors should remain represented rather than silently
  disappearing;
- persistence must not depend on transient renderer scheduling state.

## Testing and review philosophy

Passing builds and self-tests are necessary but not sufficient for core changes.

For renderer, graph, lifetime and concurrency work, review should actively
consider:

- use-after-free and processor lifetime;
- data races and deadlocks;
- lost wakeups;
- cancellation and supersession;
- stale-work resurrection;
- mutation while rendering;
- export/preview interaction;
- shutdown;
- failure paths;
- whether regression tests actually fail when the protected behaviour is
  removed.

Prefer deterministic barriers/observers to timing-dependent sleeps.

Use ASan and TSan where practical for renderer/lifetime/concurrency changes.

A known baseline issue exists: leak-enabled ASan reports an OpenFX plugin
descriptor leak during plugin loading (3,186 bytes in 36 allocations when last
measured). Do not use that known leak to dismiss unrelated sanitizer findings.

## Known deferred issues

These are known but are not reasons to expand unrelated focused PRs:

- the OpenFX plugin descriptor leak described above;
- an idle/cancel caller racing export completion can occasionally wait for the
  restored preview to finish; this is currently a latency issue, not a lifetime
  or correctness failure;
- failed node addition can cancel an existing preview and fail to restore it;
- failed RAW reload can similarly leave a stale preview;
- JPEG XL DNG decoding is not yet supported in the current image-loading path;
- TIFF SubIFD handling is incomplete for some RAW-like TIFF structures;
- export UX can block/wait on a non-cancellable full-resolution export.

Re-evaluate renderer-related deferred issues after the architecture audit rather
than layering more scheduler state onto the current design without need.

## Current development phase

**The RawNode <-> vkdt architecture audit is complete. No architectural
implementation has begun.**

The durable audit summary and proposed staged migration plan are in:

    docs/ARCHITECTURE_AUDIT.md

The audit's main conclusion is that RawNode should undergo a **staged
architectural refactor before masks/compositing**, not a renderer rewrite.

It found the processor foundation and current small renderer broadly sound.
The highest-value cleanup is to remove renderer sequencing obligations from
ordinary UI/document code, clarify ownership/lifetime boundaries, own export
execution more directly, and only then introduce explicit graph topology.

The proposed sequence deliberately keeps the first refactor steps
behaviour-preserving:

    Independent Claude architecture review
          |
          v
    Ryan + ChatGPT reconcile findings
          |
          v
    Approve target direction
          |
          v
    Resolve documentation/contracts
          |
          v
    Encapsulate existing renderer state
          |
          v
    Centralize graph-edit transactions
          |
          v
    Make processed-result publication explicit
          |
          v
    Own export lifecycle
          |
          v
    Decide evaluation/export consistency
          |
          v
    Introduce explicit topology in linear mode
          |
          v
    Sequential DAG evaluation
          |
          v
    Versioned graph persistence
          |
          v
    Approve alpha/compositing contract
          |
          v
    Begin masks/compositing feature work

The immediate next action is a **fresh Claude Code adversarial review of the
audit and migration plan**. Claude should challenge whether the refactor is
justified, whether the proposed ordering is minimal/safe, whether any
abstractions are premature, and which open questions truly block
implementation.

Do not begin refactoring until Ryan + ChatGPT review Claude's findings.

Important audit conclusions currently treated as proposals rather than approved
decisions include:

- keep current #32-#34 correctness guarantees;
- hide renderer state behind one owner/coordinator before changing its policy;
- centralize wait/mutate/reschedule sequencing behind graph operations;
- avoid a revision-only scheduler model;
- introduce explicit ports/topology before masks/compositing;
- keep whole-graph rerendering initially;
- defer per-node caches, parallel branches, tile/ROI scheduling and general job
  systems;
- investigate alpha/OFX/I/O behaviour before committing to a global alpha model.

Exact implementation details can change after independent review.

## Near-term roadmap

The architecture audit deliberately separates cleanup required now from later
feature work.

Near term:

- independently review the audit with Claude;
- settle the small number of blocking architecture/product questions;
- document approved architecture/decisions;
- carry out small behaviour-preserving refactor PRs with sanitizer and focused
  independent review;
- establish a clean graph/execution baseline.

After that baseline:

- resume colour-management design;
- continue DCTL/LUT and processor work;
- establish the approved alpha/compositing contract;
- add masks/compositing/branching incrementally.

Do not introduce speculative caches, schedulers or broad optimisation
infrastructure merely because a future DAG could use them.

## Starting a future project conversation

A future ChatGPT or other project-planning session should first read:

1. `docs/PROJECT_CONTEXT.md`;
2. `docs/ARCHITECTURE_AUDIT.md` while the staged refactor is being reviewed or
   implemented;
3. `docs/ARCHITECTURE.md`, once the reviewed architecture exists;
4. `docs/DECISIONS.md`, once approved decisions have been recorded there;
5. relevant current pull requests/issues for the task at hand.

Then inspect current source where needed rather than assuming this summary
contains implementation details.

If this document conflicts with newer architecture/decision documentation, use
the newer explicit decision and update this file so the conflict does not
persist.
