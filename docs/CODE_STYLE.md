# RawNode Code Style and Source Organisation

**Status:** Durable human-readability guidance.

This document is intentionally small. It does not try to replace normal C++ best
practice or prescribe every formatting choice. Its purpose is to keep RawNode's
source understandable to a human programmer as the editor grows.

Read it together with `docs/ARCHITECTURE.md`: source organisation should make
that architecture easier to recover from the code.

## Primary rule

Human readability and maintainability are development requirements from the
outset. Optimise source for an experienced C++ developer who must understand,
review and maintain RawNode independently, without Codex or Claude explaining
its implementation.

Prefer straightforward, idiomatic C++ and conventional patterns. Code should
explain itself primarily through structure, descriptive names and interfaces;
documentation explains decisions that are not obvious from the implementation.
The goal is the simplest implementation that can be confidently understood and
verified, rather than the fewest lines. Avoid duplicated state and abstractions
or frameworks created solely for hypothetical future features.

Formatting consistency matters, but conceptual order matters more.

## Reference implementations and proportional complexity

When a design or review concerns behaviour established photo editors have already
solved, compare the relevant ART and vkdt code or documentation where practical.
ART is a useful reference for RAW editing workflows, photo navigation, sidecars
and editor lifecycle; vkdt for graph data flow, connections, evaluation and
rendering. Check the actual implementation rather than guessing, and identify
the concrete lesson or tradeoff when it materially influences a decision.

Use these projects as references, not templates. RawNode's requirements and
existing boundaries take precedence. Do not import another editor's
implementation or complexity merely because it exists there, particularly
vkdt-specific Vulkan infrastructure.

Prefer the smallest understandable solution that covers actual requirements
and credible failure modes. Before adding state flags, fallback paths, caches,
watchers, compatibility layers or frameworks to handle a possible edge case,
explain how the case could occur, its consequences and why a simpler guard,
explicit failure, focused test or documented deferral is insufficient. Do not
build machinery solely for hypothetical future needs.

This is not permission to ignore correctness: silent data loss, damaged
sidecars, invalid graph state, use-after-free and real concurrency failures
must still be prevented. Address them proportionately without inventing
unnecessary recovery or coordination systems.

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

The dedicated readability/source-organisation pass after architecture Step 4 is
complete. The following behaviour-preserving constraints also apply to future
cleanup.

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

If future readability work exposes a genuine architecture problem, record it
and address it deliberately in a focused change. The post-Step-4 architecture
checkpoint is complete; broad cleanup is no longer the current task.

## Continuous and milestone review

Include a proportionate readability check with correctness review in every
implementation PR. Review changed and closely affected names, conceptual
organisation, ownership/locking and responsibility boundaries. Address important
maintainability problems during the relevant feature review: unclear ownership,
excessive coupling, duplicated state/execution logic, misleading interfaces,
unnecessarily complicated control flow or abstractions that obscure behaviour.

Optional stylistic preferences are non-blocking: alternative names that are not
materially clearer, equally reasonable file organisation, cosmetic restructuring
and unrelated cleanup. Raise readability concerns early; do not wait for a later
cleanup pass to address a demonstrated problem.

Recommend fixing small relevant issues in the current PR. Track substantial
risks intentionally deferred beyond that PR in `docs/PROJECT_CONTEXT.md`
(Known deferred issues); minor cleanup can wait for the next relevant milestone
review. A formatter passing is not a readability review.

At architectural milestones, inspect the affected subsystems for accumulated
complexity and responsibility drift. Use the checkpoint schedule in
`docs/PROJECT_CONTEXT.md`, and keep any cleanup focused and
behaviour-preserving.

Refactoring remains appropriate for demonstrated correctness, maintenance or
architectural issues. Keep it proportional, and record deliberate behaviour or
architecture changes rather than hiding them inside readability cleanup.

## Review standard

A readability PR should still build and pass the full self-test suite. For
moved/reordered concurrency code, retain sanitizer coverage where practical.

Review should ask:

- Can a new programmer infer the subsystem's responsibility quickly?
- Does top-to-bottom reading reveal the important operations before mechanics?
- Are ownership and failure paths obvious?
- Did cleanup accidentally alter behaviour?
- Did the change reduce cognitive load without inventing unnecessary layers?
