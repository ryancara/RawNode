# RawNode Code Style and Source Organisation

**Status:** Durable human-readability guidance.

This document is intentionally small. It does not try to replace normal C++ best
practice or prescribe every formatting choice. Its purpose is to keep RawNode's
source understandable to a human programmer as the editor grows.

Read it together with `docs/ARCHITECTURE.md`: source organisation should make
that architecture easier to recover from the code.

## Primary rule

Optimise source for a competent programmer trying to build a correct mental
model of RawNode.

Formatting consistency matters, but conceptual order matters more.

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

Prefer domain-oriented names that communicate what an operation means.

Use early exits and explicit state transitions when they make control flow easier
to inspect. Avoid clever compression when a few extra lines make ownership or
failure behaviour obvious.

## Ownership, threading and locks

Concurrency code should make ownership visible.

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

## Review standard

A readability PR should still build and pass the full self-test suite. For
moved/reordered concurrency code, retain sanitizer coverage where practical.

Review should ask:

- Can a new programmer infer the subsystem's responsibility quickly?
- Does top-to-bottom reading reveal the important operations before mechanics?
- Are ownership and failure paths obvious?
- Did cleanup accidentally alter behaviour?
- Did the change reduce cognitive load without inventing unnecessary layers?
