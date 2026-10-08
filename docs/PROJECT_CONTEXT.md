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
- `docs/ARCHITECTURE.md` — authoritative target technical architecture.
- `docs/DECISIONS.md` — authoritative durable design decisions and rationale.
- `docs/ARCHITECTURE_AUDIT.md` — audit evidence and the Codex/Claude
  reconciliation that led to the approved architecture.
- `docs/CODE_STYLE.md` — human-readability and source-organisation principles.
- `CLAUDE.md` — Claude Code's persistent review/development instructions.
- Git history and pull requests — implementation history and per-change detail.

Do not turn `PROJECT_CONTEXT.md` into a chronological diary. Do not copy every
PR into it. Update it when the information a future project conversation needs
has materially changed.

`docs/ARCHITECTURE.md` and `docs/DECISIONS.md` now contain the reviewed,
approved architecture and decision log.

Older root-level `ARCHITECTURE.md` and `DECISIONS.md` are historical pointers
only. The authoritative documents live under `docs/`.

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

Step 4, renderer ownership encapsulation, is complete. PR #45 was independently
reviewed as **SAFE TO MERGE**, validated on Linux with Release/Debug, ASan and
TSan stress coverage, then manually validated on macOS before merge.

RawNode now has one App-owned `RenderRuntime` whose private state owns
preview/export worker lifetime, demand, cancellation, mutation gating, execution
exclusivity and shutdown. Ordinary UI/document/export callers use semantic
requests rather than renderer mutexes, pending/busy flags or
wait/mutate/reschedule protocol.

Presentation buffers remain part of the display path, export file production
remains in the export path, and `renderChain()` still evaluates `App::nodes`
as an ordered serial chain. Explicit topology and graph evaluation remain
Step 6 work. The runtime owns **when** evaluation may execute safely, not what
the document or export means.

## Architectural direction

The original RawNode <-> vkdt audit and the post-Step-4 whole-architecture
checkpoint are complete. Codex and Claude independently reviewed the current
implementation and target against RawNode's product goals and the pinned vkdt
reference and found the direction **sound for the next stage**. No further broad
architectural refactor is justified before Step 5. This does not mean the final
graph architecture is implemented. The approved target remains documented in
`docs/ARCHITECTURE.md`; checkpoint evidence is in `docs/ARCHITECTURE_AUDIT.md`.

The architectural north star is:

> Every new RawNode feature should have an obvious home. If adding a feature
> requires teaching unrelated parts of the application how that feature works,
> the boundary is probably wrong.

The high-level target remains:

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
    path       path
      |
      v
    Viewer

The implementation model is slightly more explicit:

    UI
     |
     v
    Document / Graph API
     |
     +---- persistent document state
     |
     +---- render runtime
             |
             v
        graph evaluator
             |
             v
       processed result
          /       \
         v         v
      display     export

The important approved principles are:

- one image-processing graph rather than feature-specific processing systems;
- UI/document code expresses edits instead of managing renderer sequencing;
- the render runtime owns worker lifetime, execution exclusivity, demand,
  cancellation and shutdown;
- display and export consume processed graph results;
- masks, opacity, blending, LUTs, CTL, DCTL, OFX and native processors belong to
  image processing rather than creating their own schedulers;
- renderer flags may remain internally if they are the simplest correct
  implementation;
- whole-graph rerendering and sequential execution are preferred before
  fine-grained caches/parallelism;
- explicit topology and the evaluator that uses it should arrive together;
- vkdt is a conceptual reference for graph data flow and ownership, not a system
  to port or a Vulkan requirement.

A useful conceptual distinction remains:

    source / processor / parameter / graph / mask / blend change
        -> image processing work

    display / monitor conversion change
        -> display-only work

Do not create additional invalidation/scheduling domains casually.

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

Masks are not image alpha. Global alpha association remains deliberately
unresolved and is not a prerequisite for Step 5 mask-contract discussion.
Before transparent compositing or alpha-carrying workflows, explicitly revisit
TIFF, EXR, OpenFX and JPEG flattening semantics; current source alpha behaviour
is inconsistent across formats.

## Persistence direction

RawNode uses sidecar files as persistent per-image edit state.

Important persistence principles:

- sidecars belong to editable source documents only; exported derivatives do
  not receive RawNode sidecars (D038);
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
- JPEG XL DNG decoding is not yet supported in the current image-loading path;
- TIFF SubIFD handling is incomplete for some RAW-like TIFF structures;
- export UX can block/wait on a non-cancellable full-resolution export;
- thumbnail worker exceptional-unwind lifetime, the macOS pthread-to-joiner
  exception gap, and `makePreview` partial helper-thread launch remain focused
  lifetime/exception-safety follow-ups; normal thumbnail shutdown is safe;
- the dead `uploadTexture` path should be removed when the evaluator/display
  boundary is touched in Step 6 unless a use appears earlier;
- source sidecar writes remain in-place rather than atomic, and Sidecar V2 still
  includes some GUI preferences; persistence hardening/document-state cleanup
  belongs with Step 7;
- exporting over the exact path of an editable raster source can overwrite the
  source image itself; this pre-existing hazard is separate from export sidecars.

Address remaining renderer-related deferred issues within the approved staged
architecture rather than layering more scheduler state onto the design without
need.

## Current development phase

**Step 5 — Settle the first mask/graph product contract — is current.**
This is product/architecture decision work before graph implementation.

Steps 1–4 are complete: owned export execution, frozen parameter edits during
export, centralized graph-edit transactions and encapsulated renderer ownership.
Ordinary structural UI/document callers no longer manage renderer sequencing.

The full post-Step-4 stabilisation gate is complete: the OpenFX multithread
lifetime fix (#48), the behaviour-preserving readability/source-organisation
pass (#50), and the independently completed Codex and Claude architecture
checkpoint. The two immediate checkpoint follow-ups are also complete: #53
implemented D038's source-only sidecar rule and #54 deterministically protects
single-level mutation-completion ordering.

The checkpoint found no broad architecture blocker before Step 5. App remains
a practical composition root; the remaining thread-launch/lifetime findings are
focused implementation follow-ups, not reasons to reopen the architecture audit.
See `docs/ARCHITECTURE_AUDIT.md` for the candidate dispositions.

Current execution still uses a serial node chain. The remaining sequence is:

    Step 5: First mask/graph product contract       [current; decisions]
          |
          v
    Step 6: Explicit topology + sequential DAG evaluator
          |
          v
    Step 7: Versioned graph persistence
          |
          v
    Step 8: First mask contract + mask path

The completed gates are stabilisation/review work, not extra numbered
architecture steps. The checkpoint validates the direction for the next stage,
not a permanent or finished architecture.

The architectural goal is to make future changes cleaner across the application:
new features should plug into the graph, render runtime, display path, export
path or persistence rather than spread knowledge of themselves across unrelated
subsystems.

Important accepted implementation constraints:

- preserve all #32-#34 renderer/lifetime guarantees;
- export represents the accepted document state when Export is confirmed;
- parameter editing is unavailable while export owns live processor instances;
- structural graph mutation remains a single UI/control-thread contract;
- failure-triggered preview restoration should not overwrite meaningful
  operation status;
- renderer encapsulation follows protocol simplification rather than preceding
  it;
- no revision/key framework is required in the near term;
- the first DAG is sequential and uses whole-graph invalidation;
- OpenFX remains unary and serialized initially;
- caches, parallel branches, ROI/tile scheduling and job systems remain
  deliberately deferred.

Step 5 must settle at least:

- the first mask type and real use case;
- how list view maps to topology and whether a masked adjustment is one compound
  row/block;
- mask versus image alpha, RGB mixing and base-input alpha handling;
- where opacity belongs;
- multi-input dimension mismatch policy;
- required/optional inputs and missing-input behaviour;
- branch colour policy;
- coordinate space if the first mask is geometric, preferably using
  resolution-independent document semantics;
- mask visualization/inspection;
- empty-graph interpretation behaviour.

These remain decision topics, not contracts chosen by checkpoint closure.
Simple UI must not imply a weaker internal graph model: a masked adjustment may
be one logical editing unit while the document remains a genuine graph. Masks
and effect strength belong in graph-native operations rather than requiring
every processor backend to understand masking. Do not add masked-processor
renderer state, a special mask scheduler, mask fields that bypass the graph, or
universal processor opacity merely because the list places opacity beside an
adjustment. Explicit mix/blend ownership of opacity/coverage is a likely
direction to settle in Step 5, not a newly accepted contract here.

Do not start DAG implementation until this first mask/graph contract is settled.
Global alpha association remains separately deferred until transparent
compositing or alpha-carrying I/O requires it.

## Near-term roadmap

Immediate work:

- Step 5: decide the first mask/graph product contract using the topics above.

The checkpoint and immediate follow-ups are complete. No additional broad
architecture cleanup is required before this discussion.

After the Step 5 contract is settled:

- resume colour-management design;
- continue DCTL/LUT and other processor work;
- introduce the simple executable DAG and versioned topology persistence;
- add masks/compositing incrementally.

Do not introduce speculative caches, schedulers or broad optimisation
infrastructure merely because a future DAG could use them.

## Starting a future project conversation

A future ChatGPT or other project-planning session should first read:

1. `docs/PROJECT_CONTEXT.md`;
2. `docs/ARCHITECTURE.md`;
3. `docs/DECISIONS.md`;
4. `docs/ARCHITECTURE_AUDIT.md` when audit rationale or the migration
   reconciliation matters;
5. relevant current pull requests/issues for the task at hand.

Then inspect current source where needed rather than assuming this summary
contains implementation details.

If this document conflicts with newer architecture/decision documentation, use
the newer explicit decision and update this file so the conflict does not
persist.
