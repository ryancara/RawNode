# RawNode AI Contributor Guide

Read `docs/PROJECT_CONTEXT.md` before planning work, then consult
`docs/ARCHITECTURE.md` and `docs/DECISIONS.md` for boundaries and accepted
decisions. Follow `docs/CODE_STYLE.md` for maintainability and readability.
See `ROADMAP.md` for features and the quality checklist in
`docs/PROJECT_CONTEXT.md` for post-Step-6 follow-ups.

For substantial changes, agree verifiable acceptance criteria and independent
expected results **before** implementation. Include that evidence in focused
PRs. Respect the existing render-lifetime, graph and persistence contracts;
avoid speculative subsystem proliferation or unrelated cleanup.

`CLAUDE.md` documents the independent reviewer role and review expectations.
Codex normally implements changes; Claude normally reviews. Do not infer that
a reviewer should alter the repo unless explicitly instructed.
