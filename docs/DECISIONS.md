# RawNode Architectural Decisions

**Status:** Authoritative decision log.

This file supersedes the historical root-level `DECISIONS.md`. It records durable
decisions, not implementation history.

## Existing durable decisions

The following earlier decisions remain accepted:

- **D001:** Folder-based workflow, no catalogue.
- **D002:** Sidecars are authoritative per-image edit state.
- **D003:** Nodes use a backend-neutral processor abstraction rather than an
  OFX-specific model.
- **D004:** RawNode has one graph-capable processing model; list and graph views
  are views of the same underlying document.
- **D005:** Keep Dear ImGui + GLFW until a concrete blocker appears.
- **D006:** Keep C++ as the application language.
- **D007:** Keep RAW decoding replaceable and separate from RAW development.
- **D008:** Keep the RAW stage minimal; ordinary photographic adjustments belong
  in the processing graph.
- **D009:** Masks/local adjustments are first-class future requirements; AI
  masking should produce masks rather than define a separate architecture.
- **D010:** Cloud, heavyweight DAM/catalogues and project files are non-goals.
- **D011:** Assignable controls target node instances, not processor types.
- **D012:** Prefer reversible staged refactors.
- **D013:** Prefer upstream-friendly infrastructure where practical.
- **D015:** Sidecar V2 is backend-neutral and preserves unavailable processors.
- **D016:** Native Exposure is a reference processor, not a mandate that all
  tools be native.
- **D017:** Standard CTL is the core CTL backend; host-specific conventions are
  adapters.
- **D018:** Plain CTL parameters do not invent UI ranges.

Earlier **D014** allowed speculative graph/node extension fields before they were
executable. The reviewed architecture revises this: topology should be
introduced with the evaluator that actually uses it rather than accumulating a
shadow graph model.

## D019 — Every feature has an obvious home

**Status:** Accepted

Every new RawNode feature should have an obvious architectural home.

If adding a feature requires teaching unrelated parts of the application how
that feature works, the boundary is probably wrong.

Typical homes are:

- image processing -> processor/graph;
- masks/blends/compositing -> graph data/operations;
- execution timing/lifetime -> render runtime;
- monitor/display conversion -> display path;
- file output -> export path;
- durable edit state -> persistence;
- user interaction -> UI/document commands.

## D020 — Staged architectural refactor, not a renderer rewrite

**Status:** Accepted

The audit and independent Claude review found the processor foundation and small
renderer fundamentally sound. RawNode will clean up ownership and boundaries in
small behaviour-preserving steps before masks/compositing.

## D021 — Remove leaked protocol rather than merely moving flags

**Status:** Accepted

The architectural problem is caller knowledge of sequences such as
wait -> mutate -> reschedule.

Current renderer flags may remain internally if they are still the simplest
correct implementation. Encapsulation is useful only when ordinary callers no
longer manage renderer protocol.

## D022 — Document/graph mutation uses one control thread

**Status:** Accepted

Structural document/graph mutation is a UI/control-thread responsibility.
Current APIs do not promise arbitrary concurrent graph mutation. Debug
assertions should enforce this contract where practical.

## D023 — Export has an owned lifecycle

**Status:** Accepted

Export should be owned execution with explicit cleanup/join behaviour, not a
detached thread borrowing application state.

It uses the same document graph/evaluator as preview, at full resolution.

## D024 — Export consistency is simple and conservative

**Status:** Accepted

An export represents the accepted document state when Export is confirmed.
Parameter edits are unavailable while export owns the live processor instances.

Do not add snapshots, queued edits or cloned graphs unless true non-blocking
background export becomes an explicit product requirement.

## D025 — Meaningful operation status survives recovery work

**Status:** Accepted

Export outcomes and operation errors should remain visible. If a failed
operation needs to restore preview work, that recovery should be quiet rather
than overwriting the meaningful status with routine rendering status.

## D026 — Preserve PR #32–#34 guarantees

**Status:** Accepted

The lifetime and scheduling guarantees established by PRs #32, #33 and #34 are
the minimum safe baseline. Refactors may replace mechanisms only while
preserving or strengthening those guarantees.

## D027 — Display and export consume processed results

**Status:** Accepted

Display conversion occurs after the processed-image boundary and should not
rerun processors when the processed result remains valid.

Export is a separate full-resolution evaluation of the same processing model,
not a separate image-processing architecture.

## D028 — Explicit topology plus a simple sequential DAG

**Status:** Accepted

When RawNode moves beyond the linear chain, topology and executable traversal
land together.

The first DAG uses stable node IDs, named ports, simple image/mask roles,
explicit connections, fan-out, cycle rejection, one document output and
sequential dependency evaluation.

## D029 — Whole-graph invalidation first

**Status:** Accepted

The first DAG rerenders the whole reachable graph when image state changes.
Persistent node caches, selective branch invalidation, parallel branches,
dependency hashes and broad ROI/tile scheduling are deferred.

## D030 — No feature-specific schedulers

**Status:** Accepted

Masks, LUTs, DCTL, compositing, opacity and other processing features use the
same graph/render infrastructure. They do not create their own workers, queues
or pending-state domains without a strong independent reason.

## D031 — OpenFX remains conservative initially

**Status:** Accepted

The first graph implementation may keep OpenFX processors unary. Preview/export
use of shared instances remains serialized. Parallel graph execution and
processor cloning are not assumed safe.

Before any future parallelism, re-audit OpenFX host state, honour relevant
thread-safety declarations and validate representative plugins.

## D032 — Supersession promises eventual replacement

**Status:** Accepted

Newer preview work must eventually replace obsolete work. RawNode does not
currently promise that an obsolete frame can never transiently publish.

Do not add machinery for a stronger guarantee without an observable need.

## D033 — Revisions are not an architecture goal

**Status:** Accepted

Revision IDs/evaluation keys may be added later if they simplify a concrete
requirement. They are not required simply because they are useful concepts.

Freshness, demand, cancellation, execution ownership and status remain distinct
concerns.

## D034 — Masks are distinct from image alpha

**Status:** Accepted

A local-adjustment mask is a separate scalar/coverage concept from image alpha.
The graph must be able to represent mask data without creating a separate
rendering architecture.

The exact first mask UX/graph contract is still pending.

## D035 — Global alpha contract remains pending

**Status:** Accepted deferral

Do not declare a permanent global alpha association yet.

Straight/unassociated processor boundaries remain the leading candidate, with
premultiplied arithmetic inside operations that need it, but TIFF, EXR and
OpenFX boundaries require validation before approval.

## D036 — docs/ is authoritative

**Status:** Accepted

The authoritative project documents are:

- `docs/PROJECT_CONTEXT.md`;
- `docs/ARCHITECTURE.md`;
- `docs/DECISIONS.md`;
- `docs/ARCHITECTURE_AUDIT.md` for audit evidence/reconciliation;
- `CLAUDE.md` for Claude workflow/review instructions.

Old root-level architecture/decision files are historical only.

## D037 — Stabilise and make the source human-readable before graph expansion

**Status:** Accepted

After renderer ownership encapsulation and before mask/topology feature work,
RawNode will:

1. fix the pre-existing OpenFX host multithread lifetime bugs discovered during
   Step 4 review in a separate correctness PR;
2. perform a behaviour-preserving human-readability/source-organisation pass;
3. perform the planned whole-architecture checkpoint against the pinned vkdt
   reference and RawNode's actual product goals.

The readability pass should make source layout reflect architectural
responsibilities: clear file ownership, top-down reading order, conceptually
grouped functions, domain-oriented names, visible locking/ownership and comments
that explain invariants and rationale.

It is not an excuse for hidden redesign. If cleanup reveals a genuine
architectural problem, record it for the checkpoint and address it deliberately.

These are stabilisation gates between Steps 4 and 5, not additional numbered
architecture-migration steps.

**Current progress:** the first D037 stabilisation item was completed in PR #48.
The next task is the behaviour-preserving human-readability/source-organisation
pass, followed by the whole-architecture checkpoint.

## Pending decisions

Before DAG/mask feature work:

- graph-native masks versus the exact first per-node mask UX;
- multi-input dimension mismatch policy;
- missing/bypassed multi-input node semantics;
- graph-sidecar version policy;
- subgraph copy/paste topology rules.

Before masks/compositing:

- mask representation/range/filtering/coordinate-space contract;
- blend encoding;
- opacity semantics;
- branch colour mismatch policy;
- unavailable mask/compositor behaviour.

Before transparent compositing or alpha-carrying I/O:

- formal alpha association;
- hidden colour at zero alpha;
- TIFF/EXR/OpenFX adaptation;
- JPEG flattening/background policy where relevant.

## Approved near-term migration

1. Own the export job.
2. Make parameter editing unavailable during export.
3. Centralize graph-edit transactions.
4. Encapsulate renderer ownership once the public protocol has shrunk.

Then complete the D037 stabilisation gate:

- fix the OpenFX host multithread lifetime bugs;
- perform the behaviour-preserving human-readability/source-organisation pass;
- perform the whole-architecture checkpoint against vkdt and RawNode's product
  goals.

5. Settle the first mask/graph product contract.
6. Introduce topology and sequential DAG evaluation together.
7. Add versioned graph persistence.
8. Define the first mask contract and add the first mask path.

If implementation evidence changes this direction, update this log rather than
letting the architecture drift silently.
