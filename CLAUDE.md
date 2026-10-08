# RawNode - Claude Code Project Instructions

## Project

RawNode is a lightweight, modular RAW photo editor written in C++17 and derived
from ofxrawhost.

It supports a processing chain containing native processors, OpenFX plugins and
CTL processors. Future plans include DCTL, LUTs, masks, compositing, local
adjustments and a more capable node graph.

macOS is currently the primary development platform, but the architecture should
remain cross-platform for macOS, Windows and Linux.

## Project documentation

The repository is the durable source of project context. Do not rely on chat
history for important architectural knowledge.

Before broad architecture or product work, read `docs/PROJECT_CONTEXT.md`,
`docs/ARCHITECTURE.md` and `docs/DECISIONS.md`.

Read `docs/ARCHITECTURE_AUDIT.md` when the audit evidence, vkdt comparison or
migration rationale is relevant.

Document responsibilities:

- `docs/PROJECT_CONTEXT.md` — project goals, current state, priorities and
  current development phase.
- `docs/ARCHITECTURE.md` — authoritative technical architecture.
- `docs/DECISIONS.md` — significant durable decisions and their rationale.
- `docs/CODE_STYLE.md` — human-readability and source-organisation guidance.
- `CLAUDE.md` — Claude Code working and review instructions.
- Git history and pull requests — implementation history.

When work results in a significant architectural, product, workflow or roadmap
decision, identify whether the project documentation needs updating before the
work is considered complete.

Do not turn `PROJECT_CONTEXT.md` into a changelog. Update it when information a
future project session needs has materially changed. Implementation history
belongs in Git and pull requests.

For implementation PRs, update architecture/decision documentation only when the
PR actually changes the documented architecture or settles a durable decision.

## Claude's role

Claude Code is primarily used as an independent reviewer for RawNode.

The normal development workflow is:

1. Ryan and ChatGPT discuss features, behaviour and architecture.
2. Codex implements the agreed task and creates or updates a focused PR.
3. Claude independently reviews the resulting PR.
4. Review findings go back to Ryan and ChatGPT to decide what should change.
5. Codex implements any required review fixes.
6. Claude may perform a focused final verification before merge.

Unless explicitly asked to implement something:

- Do not modify repository files.
- Do not create commits.
- Do not merge PRs.
- Do not broaden the requested review into unrelated cleanup.
- Treat the submitted implementation as something to challenge, not defend.

Review the actual code rather than relying on the PR description or previous
conversation.

For PR reviews, inspect the exact requested head commit and compare it with its
base.

Clearly distinguish:

- blockers;
- important issues;
- minor issues;
- test gaps;
- pre-existing issues not introduced by the PR.

Finish with a clear judgement about whether the PR is safe to merge.

## Development philosophy

RawNode should remain lightweight and architecturally simple.

Prefer general mechanisms over feature-specific systems.

New features should integrate with existing graph, rendering and invalidation
infrastructure rather than create independent schedulers, queues, state machines
or duplicated processing paths.

Avoid accumulating feature-specific renderer state such as separate pending
systems for masks, compositing, LUTs, exports, etc.

When additional complexity appears necessary, first ask whether the same
behaviour can be represented by the existing graph or invalidation model.

Correctness and simplicity are more important than premature optimisation.

Human readability is also an explicit maintenance goal. Source organisation
should make architectural responsibilities, ownership and control flow legible
to a competent C++ programmer. Prefer conceptual/top-down function order,
coherent file responsibilities, domain-oriented names, visible lock scopes and
comments that explain invariants/why rather than narrating syntax.

The post-Step-4 readability pass is complete. Keep future cleanup
behaviour-preserving and surface architecture or product changes separately.
See `docs/CODE_STYLE.md`.

Do not perform large refactors inside narrowly scoped bug-fix or feature PRs
unless they are required for correctness.

## Architectural direction

The original RawNode <-> vkdt audit and the post-Step-4 whole-architecture
checkpoint are complete. Codex and Claude independently found the direction
**sound for the next stage**. No further broad architecture cleanup is justified
before Step 5. This does not mean the explicit graph is implemented: current
`App::nodes` execution is still serial. The authoritative target is in
`docs/ARCHITECTURE.md`.

The architectural north star is:

> Every new RawNode feature should have an obvious home. If adding a feature
> requires teaching unrelated parts of the application how that feature works,
> the boundary is probably wrong.

vkdt remains the primary conceptual reference for graph data flow, explicit
connections and ownership. Do not port its Vulkan-specific machinery or assume
its complexity is required.

The high-level model remains:

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

Implementation responsibilities are:

- UI expresses user intent;
- the document/graph API owns the edit and graph state;
- processors own image operations;
- the graph evaluator executes dependencies;
- the render runtime owns execution lifetime, demand, cancellation and
  preview/export/edit exclusivity;
- display owns presentation conversion;
- export owns file production;
- persistence owns durable document state.

Do not solve a new feature by teaching unrelated subsystems about it.

Image-processing features belong in the graph. Display/monitor conversion belongs
after the processed-image boundary. Export and display consume graph results
rather than becoming independent image-processing architectures.

The first DAG should remain sequential with whole-graph invalidation. Caches,
parallel branches, general job systems, broad ROI/tile scheduling and speculative
revision frameworks are deliberately deferred.

## Renderer principles

Renderer lifetime and concurrency correctness are critical.

Preserve these invariants:

- Preview rendering must never use processors that are being destroyed or
  replaced.
- Graph mutation must not race active rendering.
- Full-resolution export must safely own/use the processor graph.
- Preview and export must not concurrently access processors when doing so is
  unsafe.
- Display-only work must not cancel, consume or replace required
  processor-chain work.
- Explicit idle/cancellation barriers must return with the work they cancel no
  longer pending.
- Shutdown must not leave rendering threads using App or processor state.
- Failed or cancelled work must not accidentally resurrect obsolete work.
- Do not introduce a new rendering/scheduling domain without strong
  architectural justification.
- Preserve the concurrency and scheduling guarantees established by PRs
  #32-#34 unless a deliberate architecture change replaces them safely.

When reviewing concurrency changes, actively look for:

- use-after-free;
- ownership/lifetime errors;
- data races;
- lost wakeups;
- cancellation races;
- stale work resurrection;
- deadlocks;
- unsafe shutdown;
- generation/order errors.

Use ASan and TSan where practical for changes involving renderer lifetime,
threading or graph mutation.

## Feature boundaries

Processors should not know how RawNode schedules rendering.

A processor should perform image processing and expose its parameters. Rendering
policy, worker scheduling and UI behaviour belong outside the processor.

Long term, feature/UI code should describe what changed rather than directly
manage renderer mechanics.

Conceptually:

    processor parameter / mask / graph change
        -> image result invalidated

    display / monitor transform change
        -> display result invalidated

Do not introduce new invalidation categories casually.

The graph/rendering architecture has been audited against vkdt. Before masks
and compositing are implemented, follow the approved staged migration in
`docs/ARCHITECTURE.md` and settle the remaining mask/alpha decisions recorded
in `docs/DECISIONS.md`.

The post-Step-4 stabilisation gate is complete: OpenFX multithread lifetime
(#48), readability/source organisation (#50), and the independent Codex and
Claude whole-architecture checkpoint. PR #53 settled the source-only sidecar
boundary (D038); PR #54 deterministically protects single-level mutation
completion demand before gate opening. Neither remains an open checkpoint item.

**Step 5 is current: settle the first mask/graph product contract.** This is
product/architecture decision work. Use the open topics and graph-model
guardrails in `docs/PROJECT_CONTEXT.md`; do not silently settle them in unrelated
implementation work.

Do not start DAG implementation until that contract is settled. Step 6 introduces
explicit topology and the sequential evaluator together, including image/mask
ports and shared upstream evaluation. A simple list UI must preserve a real
internal graph: masks and effect strength use graph-native operations, not
masked-processor renderer state, a mask scheduler, mask fields that bypass the
graph or mandatory opacity implemented internally by every processor backend.
Generic graph/evaluator-level opacity is not ruled out; exact ownership remains
a Step 5 decision. Step 5 settles the product/architectural contract, including
opacity/effect-strength meaning and enough mixing semantics to define graph
shape. Step 8 implements the first mask path and settles remaining concrete
representation/range/filtering and blend-encoding details not needed in Step 5.
No concrete mix/blend operation is automatically a Step 6 deliverable.
D034 already separates masks from image alpha; coverage/alpha interaction
remains open, and global alpha association remains separately deferred.

The candidate dispositions are recorded in `docs/ARCHITECTURE_AUDIT.md`. App
remains a practical composition root. Leaf App locks must not call back into
RenderRuntime while held; no pre-Step-5 lock redesign is required.
Thumbnail exceptional lifetime, the macOS pthread handoff and `makePreview`
partial thread-launch safety remain focused follow-ups, not architecture
blockers or reasons to reopen the audit. Dead `uploadTexture` cleanup belongs
with the Step 6 evaluator/display boundary; persistence atomicity and GUI-state
cleanup belong with Step 7.

## Colour and image-processing principles

RawNode is intended for serious photographic and colour-managed workflows.

Unless an operation explicitly requires otherwise:

- preserve floating-point precision;
- preserve negative RGB values;
- preserve RGB values above 1.0;
- avoid unintended clipping;
- preserve alpha;
- keep colour-space and transfer-function concepts distinct;
- do not silently introduce display-referred behaviour into scene-referred
  processing.

Colour transforms should have clearly defined source and destination encodings.

RAW decoding, native CST processing and other colour-management components
should use shared colour definitions where possible rather than duplicate
matrices or transfer functions.

Changes involving colour transforms should be checked numerically, not only
visually.

## Graph and persistence principles

The processing chain is persistent image state.

Sidecars must remain backwards compatible unless a deliberate migration is
implemented.

Processor/node identity must remain stable enough for persistence, presets and
copy/paste.

Unavailable processors should remain represented in persisted chains rather
than silently disappearing.

Do not make persistence behaviour depend on transient renderer state.

## Scope discipline

Keep PRs focused.

When reviewing a PR:

- identify whether a problem was introduced by that PR;
- report significant pre-existing problems separately;
- do not require unrelated cleanup before merge;
- recommend follow-up work when appropriate rather than expanding the PR.

Do not opportunistically redesign the renderer during a narrow bug fix.

Conversely, when explicitly performing an architecture audit or refactor, be
willing to question and remove existing machinery rather than preserving it
only because it already exists.

## Build

The normal Release build is:

    ./build.sh

`build.sh` configures CMake with `CMAKE_BUILD_TYPE=Release` and builds using
the available CPU cores.

Optional host-specific CPU optimisation:

    OFX_NATIVE_ARCH=1 ./build.sh

On macOS the packaged application is:

    build/OfxRawHost.app

On other platforms the executable is normally:

    build/OfxRawHost

## Self-test

macOS:

    build/OfxRawHost.app/Contents/MacOS/OfxRawHost --selftest

Linux / Windows-style build:

    build/OfxRawHost --selftest

Run the complete self-test suite before declaring an implementation or review
validation complete.

For renderer, lifetime or concurrency work, also use ASan and TSan where the
environment supports them.

## Known baseline issues

Leak-enabled ASan currently reports a known OpenFX plugin descriptor leak during
plugin loading: 3,186 bytes in 36 allocations at the time this file was added.

The two OpenFX host multithread lifetime bugs found during Step-4 review were
fixed in PR #48 and are no longer baseline issues. If reviewing related code,
preserve the guarantees that `multiThread()` does not return before its callbacks
finish and that persistent host workers are stopped/joined before their owner is
destroyed.

Do not attribute the remaining baseline leak to unrelated changes unless
allocation behaviour actually changes.

Do not disable or dismiss other sanitizer findings merely because a known
baseline issue exists.

## Review standard

A successful build and passing self-test are necessary but not sufficient.

For substantial changes, inspect:

- behaviour at boundaries and failure paths;
- ownership and lifetime;
- persistence compatibility;
- cancellation and supersession;
- shutdown;
- interactions with existing processor types;
- whether tests actually fail when the intended fix is removed.

Prefer deterministic regression tests over timing-dependent sleeps.

When a proposed regression test passes even after the behaviour it is intended
to protect is removed, call out the test gap.

Do not assume a change is correct simply because its author reports sanitizer
or test success.
