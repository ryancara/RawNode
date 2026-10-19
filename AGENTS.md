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
