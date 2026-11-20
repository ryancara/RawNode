# RawNode Code Style and Source Organisation

**Status:** Detailed coding guidance under the standing principles in
[AGENTS.md](../AGENTS.md). Those principles govern all planning, implementation,
testing and review by Codex, Claude and other contributors.

This document keeps source-organisation and C++ guidance in one place. It also
applies to test code and support infrastructure. System responsibilities and
architectural contracts remain in [ARCHITECTURE.md](ARCHITECTURE.md); current
project state belongs in [PROJECT_CONTEXT.md](PROJECT_CONTEXT.md).

## Reference implementations

When a design or review concerns behaviour established photo editors have already
solved, compare relevant ART and vkdt code or documentation where practical.
ART is useful for RAW editing workflows, navigation, sidecars and editor
lifecycle; vkdt for graph data flow, connections, evaluation and rendering.
Inspect the actual implementation and identify the concrete lesson or tradeoff
when it materially influences a decision.

Use these projects as references, not templates. RawNode's requirements and
existing boundaries take precedence. Do not import another editor's
infrastructure merely because it exists, particularly vkdt-specific Vulkan
machinery.

## File responsibility

A file should have an obvious purpose.

Split files when there is a real responsibility boundary, not merely because a
file is long. Do not create managers/controllers/helpers solely to make files
smaller.

Physical source structure should broadly reflect the project architecture:

- UI expresses intent;
- document/graph code owns edit meaning and persistent graph state;
- processors own image operations;
- render runtime owns execution lifetime/policy;
- evaluator executes processing dependencies;
- display owns presentation conversion;
- export owns file production;
- persistence owns durable state;
- backend adapters contain backend-specific integration.

## Reading order

Prefer top-down conceptual order inside implementation files.

A reader should normally encounter:

1. construction/lifecycle and major public/domain operations;
2. related semantic operations grouped together;
3. internal state transitions and policy;
4. worker/backend mechanics;
5. small implementation utilities.

Do not alphabetise functions when that destroys conceptual reading order. Avoid
letting years of feature chronology become the file structure.

## Names and control flow

Use clear, domain-oriented names for types, functions, variables and files.
Keep functions manageable, with coherent responsibilities; split them when
doing so clarifies an operation or ownership boundary, without imposing arbitrary
line limits or adding layers merely to shorten code.

Use early exits and explicit state transitions when they make control flow easier
to inspect. Avoid clever compression when a few extra lines make ownership or
failure behaviour obvious.

## Ownership, threading and locks

Make resource ownership, lifetime expectations and concurrency boundaries
explicit in interfaces and in the code that depends on them.

- Keep lock scopes as small and visually obvious as correctness allows.
- State important lock-order, thread-affinity and lifetime invariants near the
  code that relies on them.
- Prefer RAII for ownership and cleanup.
- Do not expose synchronization primitives merely to make tests convenient.
- Tests should observe behaviour through narrow seams rather than becoming a
  second control API.

## Comments

Comments should explain:

- why a non-obvious rule exists;
- ownership/lifetime assumptions;
- thread/lock invariants;
- failure/recovery semantics;
- important tradeoffs and architectural reasoning;
- deliberate limitations.

Avoid comments that merely translate the next line of C++ into English.

Historical PR references can be useful in project documentation and tests, but
production code should normally state the invariant directly so future readers
do not need Git history to understand correctness.

## Behaviour-preserving cleanup

Readability cleanup preserves behaviour unless the approved task explicitly
changes it.

It may:

- reorder functions into conceptual reading order;
- move code to a more appropriate file;
- improve names;
- remove obsolete/historical comments;
- clarify ownership and lock scopes;
- split or combine files where an existing responsibility boundary is clearer;
- apply consistent formatting.

It should not quietly:

- change product behaviour;
- redesign architecture;
- add feature abstractions;
- introduce a job system/cache/revision framework;
- change persistence semantics;
- change renderer guarantees.

If readability work exposes a genuine architecture problem, record it and
address it deliberately in a focused change. A cleanup task does not implicitly
authorise product or architecture changes.

Apply AGENTS.md's separate acceptance criteria and proportionate review rules
to readability work. A production-code readability refactor still builds and
passes the full self-test suite; preserve sanitizer coverage for moved/reordered
concurrency code where practical. Documentation-only edits use the checks in
AGENTS.md. Record intentional behaviour or architecture changes separately from
cleanup.
