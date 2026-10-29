# RawNode - Claude Code Project Instructions

## Project

RawNode is a lightweight, modular RAW photo editor written in C++17 and derived
from ofxrawhost.

It supports one explicit processing graph containing native processors, OpenFX
plugins and CTL processors, with a dockable Nodes panel and switchable List/Graph
presentation. D051's workspace and graph editing stages are merged as PR #80
and PR #82. PR 2C's contextual placement was merged as PR #87; Step 8
non-AI masking is in progress. Future plans include DCTL, LUTs, compositing
and local adjustments.

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
- `docs/STEP7_PERSISTENCE_DESIGN.md` — accepted graph-native persistence schema
  and lifecycle; `docs/workspace-format.md` — global settings format.
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

## Continuous PR quality review

Every implementation PR review includes a proportionate readability check of
changed and directly affected code. Ask whether a competent C++ developer
unfamiliar with the implementation can follow its names, coherent file/function
responsibilities, ownership, locking, mutation and execution boundaries, and
failure paths. Check that features fit the architecture and abstractions serve
established requirements. Raise meaningful maintainability issues while the
feature is under review; use the severity guidance in `docs/CODE_STYLE.md`.
Minor stylistic preferences should not block a PR or prompt unrelated cleanup.

For substantial features, challenge the agreed acceptance criteria, expected
results and test oracles independently of the implementation. Values copied
from the implementation's own output are not independent evidence. Prefer
analytical or mathematical reference values, independently validated libraries,
established reference images/results, implementation-independent
invariants, and equivalence to an independently validated baseline,
exercised through the production path. Check failure/boundary cases and whether
a plausible incorrect implementation would fail the tests. See the existing
review standard below for regression-test mutation checks.

Reference implementations must be independent of the specific code path under
test. Appropriately configured lcms2 can independently reference native
colour-space conversion maths, but is not an independent oracle for RawNode's
ICC-processing path that already uses lcms2. Extended-range tests need
floating-point, unclipped reference transforms; integer or clipping transforms
cannot establish preservation of negative RGB or values above 1.0.

Tests should explain the behaviour they establish, why expected results are
correct and justify numerical tolerances for the behaviour under test. For
colour/image work, check floating-point precision, negative and above-1 RGB
values, and alpha preservation. Keep test support proportionate and understandable.

Milestone checkpoint timing and post-Step-6 infrastructure tasks are tracked in
`docs/PROJECT_CONTEXT.md`. The whole-Step-6 review is complete; the focused
Step 7 design checkpoint and implementation are complete and do not reopen it.

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

For comparable behaviour, inspect how ART approaches RAW editor workflows,
sidecars and navigation, and how vkdt approaches graph data flow and rendering.
Use their actual implementations as references where relevant, not as
blueprints. Do not import unrelated infrastructure. See `docs/CODE_STYLE.md`
for the comparison and complexity standard.

Before adding special machinery for a possible edge case, identify a credible
failure mode, its impact and why a narrower solution or documented limitation
would be inadequate. Protect real user-data, graph and concurrency invariants;
do not expand PR scope to pre-solve hypothetical future cases.

Correctness and simplicity are more important than premature optimisation.

Human readability and maintainability are development requirements, applied as
features are designed and implemented. An experienced C++ developer should be
able to understand, review and maintain RawNode independently of Codex or Claude.
See `docs/CODE_STYLE.md` for the source and interface guidance.

The post-Step-4 readability pass is complete. Keep future cleanup
behaviour-preserving and surface architecture or product changes separately.
See `docs/CODE_STYLE.md`.

Keep refactoring focused on demonstrated correctness, maintenance or
architectural problems; avoid broad unrelated cleanup in bug-fix or feature PRs.
Keep substantial refactors in their own focused PR unless required for
correctness. Address small, directly relevant maintainability improvements during
feature development and review. Do not postpone genuine correctness or important
maintainability problems solely to avoid necessary changes. Cosmetic restructuring
of understandable code and speculative future-proofing do not justify broad
refactoring or new abstractions.

## Architectural direction

The original RawNode <-> vkdt audit and the post-Step-4 whole-architecture
checkpoint and Step 5 product-contract work are complete. Codex and Claude
independently found the direction **sound for the next stage** and again found
the final Step 5 contract sound after small clarifications. Step 6 is complete
and merged in PR #58 (`6294a034d30168063e2aa92e984a51666f56ca22`): the explicit
graph, sequential DAG evaluator and topology List View are implemented. Preview,
inspection and export share the evaluator. Step 7 PRs 1–5 are merged (#63, #65,
#66, #68, #69), replacing the temporary V2 boundary with graph-native persistence.
Independent whole-Step-6 review is complete; no further broad architecture
audit was required **before Step 8**. The owner has separately approved
Step 9 (D055): a whole-codebase readability, simplicity and architecture
quality audit **after the agreed non-AI Step 8 exit gate**. Each subsystem
receives separate Codex and Claude audits, reconciled by the owner; every
implementation PR still follows focused scope, tests, independent review
and explicit owner approval for significant architectural, behavioural or
persistence changes.
The authoritative architecture is in `docs/ARCHITECTURE.md`; Step 9's
canonical process is in `ROADMAP.md`; milestone and validation status are
in `docs/PROJECT_CONTEXT.md`.

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

**Step 5 is complete.** The accepted graph/mask contract is detailed in
`docs/STEP5_GRAPH_CONTRACT.md` and summarized in D039–D046.

**Step 6 is complete, independently reviewed and merged.** Explicit topology
and the sequential evaluator use
static named Image/Mask ports, consumer-owned upstream references,
ordered stable Slots, explicit Source/document output, requested Image/Mask
endpoints, fan-out, cycle rejection, shared-upstream evaluation and
per-evaluation value lifetime.

Do not preserve list/vector order as hidden execution semantics. A simple List
View is derived from graph topology. Keep the existing RenderRuntime /
DocumentMutation ownership boundary; do not add feature-specific schedulers,
parallel branches, persistent caches or processor cloning.

Unavailable Mask data propagates through required Mask-processing inputs and
fail-opens to full coverage only at optional effect/layer coverage use, with a
persistent warning. Under Step 8 D054, a bypassed Mask generator without a
same-type pass-through returns raster-free Inactive (not white), and an
Inactive optional Mask provides full coverage without a warning.
See `docs/STEP8_MASKING_CONTRACT.md`; the old Step 5 white-bypass rule is
superseded.

Step 7 graph-native persistence and all four targeted post-Step-7 corrections
are complete (see "Quality and verification roadmap" in
`docs/PROJECT_CONTEXT.md`). The Graph/List visual foundation (PR #79),
workspace/Inspector (PR #80) and graph editing (PR #82) are merged.
PR 2C's approved contextual node placement was merged as #87; the first
production Mask path (#101) and Mask Only preview (#102) are also merged.
See `docs/GRAPH_VIEW_VALIDATION.md`
for focused tests, owner-reported macOS results and pending native edge-case
and Windows GUI checks.
Both editor views share the authoritative document graph and NodeId selection.
Step 6 proves typed Mask/evaluation contracts with tests; user-facing masking
remains Step 8 work.

The candidate dispositions are recorded in `docs/ARCHITECTURE_AUDIT.md`. App
remains a practical composition root. Leaf App locks must not call back into
RenderRuntime while held; no pre-Step-5 lock redesign is required.
Thumbnail exceptional lifetime, the macOS pthread handoff and `makePreview`
partial thread-launch safety remain focused follow-ups, not architecture
blockers or reasons to reopen the audit. Dead `uploadTexture` cleanup is complete
in Step 6; Step 7 completed checked atomic persistence and global settings
ownership. Outstanding platform checks remain in `docs/STEP6_MANUAL_VALIDATION.md`.

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

The processing graph is persistent image state.

The live photo editor, presets and clipboard share the strict graph-native codec.
Historical V1/V2 sidecars remain protected without migration; legacy transfers
are retired. Complete grades preserve supported branching, colour pairs and
positions. Append and compatible fragment Paste use one staged linear batch;
copy eligibility is independent of attachment support. No path may silently
flatten or corrupt the authoritative graph.

Processor/node identity must remain stable enough for persistence, presets and
copy/paste.

Unavailable processors should remain represented in persisted graphs rather
than silently disappearing.

Do not make persistence behaviour depend on transient renderer state.

## Scope is an acceptance criterion

A change can be correct, well-tested and architecturally reasonable while still
being unnecessary.

Every independent PR review must assess compliance with the approved scope, in
addition to correctness, readability, architecture and test coverage.

Identify features, abstractions, dependencies and refactoring that are not required
by the agreed acceptance criteria. Flag scope expansion independently of functional
defects and recommend removing unnecessary changes before merge.

Passing tests do not justify additional complexity. Optional improvements should
be proposed separately rather than incorporated without approval.

## Product decisions and architecture reuse in PR reviews

For any PR with user-visible changes, enumerate every actual behaviour or
appearance change, not just the headline features. Consider, for example,
controls, menus, labels, icons, port shapes, colours, layout, shortcuts,
selection, warnings, defaults and workflows. Distinguish minimal feedback
required for approved interactions from discretionary choices. Flag unapproved
choices
as scope findings even if the implementation is small, useful and correct.
Do not assume that a roadmap idea authorises implementation without a focused
owner-approved acceptance contract.

For architecture, explicitly map new behaviour to the existing graph/model,
document commands, processor interface, evaluator, render runtime, persistence
and UI responsibilities. Look for duplicated state, hidden dependencies,
new lifecycle protocols and feature-specific subsystems; prefer reuse where
it keeps boundaries clear. A new file or class is not automatically a new
system. Conversely, a *justified* new module or system is welcome when it
demonstrably improves modularity, separation of responsibilities, maintainability
or extensibility for a concrete requirement. Speculative infrastructure is
not justified by possible future uses. Significant architectural changes
(e.g. a new subsystem, changed ownership/execution protocol or persistence
schema) require owner approval. Judge proportionately, not by counting files.

Review **scope**, **architecture fit** and **correctness** as separate
acceptance criteria; tests alone cannot establish approval. Recommend narrowly
scoped corrections and keep optional improvements outside the PR. If the
implementer has identified useful unapproved improvements, they should appear
as unimplemented suggestions, not silent additional functionality.

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

Run the complete self-test suite before declaring implementation or source-code
review validation complete. Documentation-only changes need proportionate
consistency, scope and whitespace checks rather than a local rebuild.

For renderer, lifetime or concurrency work, also use ASan and TSan where the
environment supports them.

## Known baseline issues

Full self-tests now send OpenFX ActionUnload after test-owned instances are
destroyed and pass leak-enabled ASan without a descriptor-leak suppression.
Normal application shutdown still omits Unload; its descriptor leak remains
unresolved (historically 3,186 bytes in 36 allocations). See the quality roadmap
in `docs/PROJECT_CONTEXT.md` for the narrow JPEG UBSan exception.

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

A successful build and passing self-test are necessary but not sufficient for
implementation PRs.

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
