# RawNode Standing Development Principles

This is the canonical entry point for RawNode's shared development principles.
These are standing requirements for all future planning, implementation, testing
and review, including Codex implementation and Claude independent review. The
owner does not need to repeat them for each task.

Before planning, read [PROJECT_CONTEXT.md](docs/PROJECT_CONTEXT.md) for current
state and priorities. Use [CODE_STYLE.md](docs/CODE_STYLE.md) for detailed coding
guidance, [ARCHITECTURE.md](docs/ARCHITECTURE.md) for system responsibilities and
contracts, and [DECISIONS.md](docs/DECISIONS.md) for accepted decisions. Keep the
shared principles here rather than duplicating them in those documents.

Codex normally implements approved work; Claude normally reviews independently.
A review request does not authorise repository changes. [CLAUDE.md](CLAUDE.md)
retains role-specific review guidance and build/self-test commands.

## Simplicity first

Prefer the simplest coherent implementation that satisfies approved requirements.
Unnecessary complexity is grounds for rejecting an implementation even when it
is correct and well-tested. Simplicity does not mean minimising line count:
keep the code needed to make responsibilities, ownership and failure paths clear.
Correctness and simplicity take priority over premature optimisation.

## Human readability

Human readability is a primary acceptance criterion. An experienced C++ developer
must be able to understand, maintain and debug RawNode independently of AI
assistance. Prefer conventional C++, descriptive names, clear responsibilities,
straightforward control flow and explicit ownership and lifetimes.

Consider readability during planning, implementation, testing and review.
Address directly relevant maintainability problems while developing the feature;
do not defer readability to a cleanup milestone. Cosmetic uniformity alone does
not establish readability.

## Proportionate robustness

Handle realistic usage, ordinary failures and consequential correctness risks.
Preserve meaningful user-data and sidecar integrity, image correctness, graph
validity, memory safety and concurrency guarantees. Accept reasonable documented
limitations and user responsibility within the approved contract.

Before adding defensive state, fallback paths, compatibility layers, recovery
systems or other infrastructure, identify a credible trigger, its consequences
and why a narrow guard, explicit failure or documented limitation is inadequate.
Avoid disproportionate machinery for obscure or hypothetical scenarios.
Distinguish demonstrated defects, required validation gaps and accepted limits.
Missing hardware alone does not justify new infrastructure; an owner-accepted
limitation is not a merge blocker without new material evidence.
Do not require universal compatibility proofs for an accepted native API contract.

## Shared cross-platform implementation

RawNode is one application across macOS, Windows and Linux. Share workflows,
application logic, state, result models and architecture wherever practical.
Confine platform-specific code to genuinely necessary differences, including
native operations, path encoding, permission semantics and native UI behaviour.

Existing platform implementations may be revised or simplified to produce a
better shared solution. Do not preserve older code merely because it exists or
add a platform as a parallel subsystem. Keep changes within approved behaviour.

## Reuse before introducing

Extend existing mechanisms and state owners before creating parallel systems.
Prefer established libraries and native operating-system facilities where
appropriate. Delegate filesystem compatibility, permissions, capacity and native
confirmations to the OS where those are its responsibility.

New abstractions, modules and infrastructure require demonstrated benefits for
actual requirements that justify their implementation, integration and
maintenance cost. Speculative future usefulness is insufficient. A new file or
class alone is not a new subsystem; justified improvements remain welcome.

## Scope discipline

Implement approved functionality and necessary supporting changes. Preserve
owner approval of product decisions and significant architectural changes under
[D052](docs/DECISIONS.md#d052--explicit-product-choices-and-proportionate-architecture-reuse).
Ordinary technical details and minimum feedback for an approved feature remain
the implementer's responsibility. Allow directly relevant simplifications that
improve the solution without introducing unapproved behaviour or appearance.

Before substantial work, establish proportionate acceptance criteria, a design
and independently verifiable expected results in the PR or an appropriate
existing document. Explain relevant boundaries, data flows, ownership and failure
handling. Small fixes need no formal design document or extra approval process.

Every production-code change must be justified by the approved requirements.
Inspect the complete diff before opening a PR and remove unnecessary changes.
Report every user-visible change against its approved requirement. Record
optional ideas as "Unimplemented suggestions" without implementing them.
Passing tests do not authorise extra functionality or unrelated cleanup.

## Proportionate testing

Focus tests on realistic photography workflows, meaningful invariants,
independently justified numerical expectations and credible regressions.
Exercise the relevant production boundary. Test fixtures, helpers and harnesses
must meet the same simplicity and human-readability standards as production code.
Avoid exhaustive combinations and elaborate harnesses that add little confidence;
justify extra test infrastructure by the risk and evidence it addresses.

Run required repository checks and focused tests appropriate to the change.
Retain meaningful regression and sanitizer coverage for data integrity, image
correctness, memory safety and concurrency. Reuse valid CI evidence for the
reviewed revision; repeat or broaden completed checks when changes, failures or
new concerns warrant it. Distinguish what unit tests, native CI and owner desktop
validation establish. Documentation-only changes need consistency, scope,
link/anchor and whitespace checks instead of an application rebuild. Detailed
numerical/reference-oracle review guidance is in CLAUDE.md.

## Proportionate independent review

Assess simplicity, readability, scope, architecture fit and correctness as
separate acceptance criteria. Reviewers must weigh the implementation and
maintenance cost of their recommendations against the practical benefit.

Distinguish blocking correctness defects, important maintainability problems and
optional improvements. For a required correction, state the conditions, concrete
consequence, evidence and smallest reasonable fix. Separate introduced defects,
pre-existing issues, required test gaps and accepted limitations. Do not make
hypothetical failures or stylistic preferences mandatory corrections without a
concrete justification. Keep suggestions distinct from merge requirements.

After a minor correction, verify the fix and directly affected behaviour. A
complete independent re-review is not required after every minor change; broaden
review when the change or new evidence exposes meaningful additional risk.
Once acceptance criteria are satisfied and meaningful risks are resolved,
conclude the review. Broader audits remain separately agreed milestones.

## Independent judgement

Challenge proposals constructively, including those made by the owner,
implementer or reviewer. Do not agree merely to please another contributor.
Present meaningful tradeoffs and simpler alternatives where appropriate, and
revise judgement when evidence warrants it. The owner retains final
decision-making authority.
