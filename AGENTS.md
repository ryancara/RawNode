# RawNode AI Contributor Guide

Read `docs/PROJECT_CONTEXT.md` before planning work, then consult
`docs/ARCHITECTURE.md` and `docs/DECISIONS.md` for boundaries and accepted
decisions. Follow `docs/CODE_STYLE.md` for maintainability and readability.
See `ROADMAP.md` for features and the quality checklist in
`docs/PROJECT_CONTEXT.md` for post-Step-6 follow-ups.

For substantial changes, agree verifiable acceptance criteria and independent
expected results and a proportionate design **before** implementation. Include
that evidence in focused PRs. Respect the existing render-lifetime, graph and
persistence contracts; avoid speculative subsystem proliferation or unrelated
cleanup.

Human readability is a development requirement; code and interfaces should be
understandable to an experienced C++ developer without AI explanations.
See `CLAUDE.md` for build/self-test commands and independent review standards,
and `docs/PROJECT_CONTEXT.md` for known limitations and deferred work.

Codex normally implements changes; Claude normally reviews. Do not infer that
a reviewer should alter the repo unless explicitly instructed.

## Scope discipline

Implement only the explicitly approved functionality. Do not introduce additional
features, refactoring or architectural changes simply because they seem useful.
Propose optional improvements separately and obtain approval before implementing them.

Every production-code change must be justified by the agreed acceptance criteria.

Before opening a PR, review the complete diff against the base branch and remove
unnecessary changes. Correctness, passing tests and potential future usefulness
do not independently justify scope expansion.

## Product and UI decisions

Preserve existing visible behaviour and appearance unless an approved task
requires changing them. Codex may choose ordinary implementation details and
the minimal interaction feedback necessary for an approved feature. Optional
choices involving user-visible elements (e.g. controls, menus, labels, icons,
port shapes, colours, layout, shortcuts, selection or workflows) are product
decisions: propose them for owner discussion rather than silently implementing
them, however minor or reasonable they seem.

Include a short inventory of **all user-visible behaviour and appearance
changes** in every PR that changes the user experience. Identify their approved requirement or explain why
they are strictly necessary. Record other worthwhile ideas as
"Unimplemented suggestions" in the implementation report, not in the code.

## Reuse architecture without prohibiting improvements

Before adding functionality, identify its home in the existing UI, graph
commands, processors, evaluator, render runtime, export and persistence
boundaries. Prefer extending and composing established mechanisms rather than
adding parallel state, protocols or subsystems.

A new cohesive module or system is appropriate when a concrete requirement
justifies greater modularity, responsibility separation, maintainability or
extensibility relative to its integration and maintenance costs. A new
file/class alone is not a new subsystem. A significant architectural departure
(e.g. a new subsystem, changed ownership/execution protocol or persistence
schema) requires explicit owner discussion and agreement before implementation;
do not introduce one incidentally in unrelated work. Do not build speculative
infrastructure solely for hypothetical future features.

Review every change for **scope, architecture fit and correctness** separately.
