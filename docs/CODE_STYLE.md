# RawNode Development Principles and Code Style

**Status:** Permanent standard for implementation and independent review.

These principles govern Codex implementation, Claude review and other RawNode
contributions. Keep the shared rules here; `AGENTS.md` and `CLAUDE.md` provide
role-specific instructions and entry points.

Read this with `docs/ARCHITECTURE.md` for system responsibilities and
`docs/DECISIONS.md` for accepted decisions. Current project state belongs in
`docs/PROJECT_CONTEXT.md`; persistence contracts remain in
`docs/STEP7_PERSISTENCE_DESIGN.md`.

## Simplicity and human readability

Human readability and maintainability are development requirements from the
outset. An experienced C++ developer should be able to understand, review and
maintain RawNode independently, without Codex or Claude explaining its
implementation.

Prefer the smallest coherent solution to the agreed requirement. Use
straightforward, idiomatic C++ and conventional patterns. Names, interfaces,
responsibilities, ownership and failure paths should explain how the code works.
Keep the lines needed to make those relationships clear. Documentation records
non-obvious contracts and tradeoffs.

Conceptual order matters more than cosmetic uniformity. Correctness and
simplicity take priority over premature optimisation.

## Approved scope and reuse

Implement only agreed functionality. Every production-code change must be
justified by the acceptance criteria. Before substantial work, establish a
proportionate design and independently verifiable expected results, recorded in
the PR or an appropriate existing document. Explain responsibility boundaries,
important data flows, ownership and failure handling at the level needed to
verify the change. Small fixes need no formal design document or extra approval
process.

Find the feature's home in the existing architecture and compose the established
commands, state owners and lifecycle. Avoid parallel state, workflows, protocols
and feature-specific subsystems. A cohesive new module is appropriate when a
concrete requirement demonstrates a maintenance or responsibility benefit that
justifies its integration cost. A new file or class alone is not a new subsystem.

[D052](DECISIONS.md#d052--explicit-product-choices-and-proportionate-architecture-reuse)
governs product choices and significant architectural departures. Ordinary
technical details and minimum feedback for an approved feature remain the
implementer's responsibility. Preserve existing behaviour and appearance unless
an approved task requires changes. Report every user-visible change against its approved requirement in
the PR; record optional ideas as "Unimplemented suggestions" without implementing
them.

Before opening a PR, inspect the complete diff against its base and remove
unnecessary changes. Passing tests or possible future usefulness do not
authorise scope expansion.

## Shared cross-platform implementations

Keep one shared policy, workflow and result model across macOS, Windows and
Linux. Confine platform-specific code to native operations and genuinely
necessary OS differences, such as path encoding, permission semantics and
native UI behaviour. Avoid adding a platform as a separate subsystem.

Review existing platform implementations when adding support elsewhere. Simplify
or consolidate older code when it improves the shared solution; an existing
implementation is not immutable. Keep such changes within the agreed task.

Prefer standard native APIs and established libraries. Let the operating system
handle filesystem compatibility, permissions, capacity and native confirmations
where those are its responsibility. Add application-specific policy or recovery
mechanisms only when a concrete accepted requirement justifies them. Report
native failures and limitations through the shared workflow.

## Proportionate robustness and validation

Protect user data, sidecar integrity, graph validity and real lifetime/concurrency
guarantees. Address credible failure modes with the smallest understandable
solution. Before adding defensive state, fallback paths, caches, watchers,
compatibility layers, recovery systems or test frameworks, identify the trigger,
its consequences and why a narrow guard, explicit failure, focused test or
documented limitation would be insufficient.

Use realistic photography workflows and meaningful boundary cases at the
relevant production boundary. Scale fixtures and test support to the change's
consequences. Run required repository checks and use independent expected
results; distinguish what unit tests, native CI and owner desktop validation
actually establish. Documentation-only changes need consistency, scope and
link/whitespace checks.

Distinguish a demonstrated defect, a required validation gap and an explicitly
accepted limitation. Missing hardware or an unverified hypothetical case does
not by itself justify new infrastructure. Do not silently promote an
owner-accepted limitation into a merge blocker without new material evidence.

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

## Proportionate implementation and review

Apply the same simplicity, readability, robustness, reuse and platform standards
during design, implementation and independent review. Review scope, architecture
fit and correctness separately. Inspect the actual diff and directly affected
code. Passing checks provide evidence; they do not approve extra functionality
or complexity.

Check whether a competent C++ developer unfamiliar with the change can follow
its names, responsibilities, ownership, locking and failure paths. Raise material
maintainability issues while the feature is under review. Fix small directly
relevant problems; propose substantial unrelated work separately.

For each finding, explain the conditions, practical consequence, evidence and
smallest reasonable correction. Separate introduced defects, pre-existing issues,
required test gaps and optional improvements. Mandatory fixes should address
credible correctness, data-integrity, maintainability or agreed-scope failures.
Cosmetic preferences, alternative equally clear organisation and speculative
future-proofing are non-blocking.

Reviewers must not require extra abstractions, defensive state or test hooks
solely to manufacture coverage for hypothetical cases. Check meaningful failure
paths and challenge weak evidence proportionately to the risk. Respect explicit
owner decisions about accepted limitations; keep suggestions distinct from merge
requirements.

Use the repository's required checks and focused regression tests appropriate to
the changed behaviour. Keep role-specific numerical/reference-oracle guidance in
`CLAUDE.md`. Do not present CI coverage as proof of untested native hardware or
desktop behaviour, and do not require universal compatibility proofs for an
otherwise accepted native API contract.

At explicitly agreed architectural milestones or audits, inspect accumulated
complexity and responsibility drift, including existing machinery. Follow the
checkpoint schedule in `docs/PROJECT_CONTEXT.md`. Routine feature reviews should
stay focused on changed and closely affected code.

A readability refactor still builds and passes the full self-test suite.
Preserve sanitizer coverage for moved/reordered concurrency code where practical.
Document intentional behaviour or architecture changes separately from cleanup.
