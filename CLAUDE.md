# RawNode - Claude Code Project Instructions

## Project

RawNode is a lightweight, modular RAW photo editor written in C++17 and derived
from ofxrawhost.

It supports a processing chain containing native processors, OpenFX plugins and
CTL processors. Future plans include DCTL, LUTs, masks, compositing, local
adjustments and a more capable node graph.

macOS is currently the primary development platform, but the architecture should
remain cross-platform for macOS, Windows and Linux.

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

Do not perform large refactors inside narrowly scoped bug-fix or feature PRs
unless they are required for correctness.

## Architectural direction

vkdt is the primary architectural reference for future RawNode graph and
rendering design because its lightweight node/graph approach is close to
RawNode's goals.

Use vkdt as a reference for concepts and architecture, not as code that must be
copied literally.

RawNode has different constraints, particularly:

- OpenFX hosting;
- native C++ processors;
- CTL;
- future DCTL support;
- cross-platform CPU/GPU operation.

The intended high-level model is approximately:

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

Image-processing features belong in the processing graph.

Display/monitor conversion belongs after the processed image boundary.

Export and display should be consumers of graph output rather than independent
image-processing architectures.

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

Before masks and compositing are implemented, RawNode's graph/rendering
architecture will be audited against vkdt and simplified where appropriate.

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

## Known baseline issue

Leak-enabled ASan currently reports a known OpenFX plugin descriptor leak during
plugin loading: 3,186 bytes in 36 allocations at the time this file was added.

Do not attribute this leak to an unrelated PR unless the allocation behaviour
actually changes.

Do not disable or dismiss other sanitizer findings merely because this known
leak exists.

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
