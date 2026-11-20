# RawNode AI Contributor Guide

Read `docs/PROJECT_CONTEXT.md` before planning work. Read
[the shared development and review standard](docs/CODE_STYLE.md) before
implementation or review: its simplicity, human readability, proportionate
robustness, cross-platform reuse, scope and review rules govern both Codex and
Claude.

Consult `docs/ARCHITECTURE.md` and `docs/DECISIONS.md` for boundaries and accepted
decisions, including D052's product-approval rules. Keep common guidance in
`docs/CODE_STYLE.md` instead of maintaining parallel policies in agent files.

Codex normally implements the agreed task; Claude normally reviews it
independently. A request to review does not authorise repository changes.
See `CLAUDE.md` for role-specific review guidance and build/self-test commands.

Respect the existing render-lifetime, graph and persistence contracts. Current
priorities and verification follow-ups live in `ROADMAP.md` and
`docs/PROJECT_CONTEXT.md`.
