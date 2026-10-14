# RawNode Project Context

This document is the durable onboarding summary for RawNode. It exists so a new
ChatGPT, Codex, Claude Code, contributor, or future development session can
understand the project's direction without relying on old chat history.

Read this document before making broad product or architecture decisions.

This is **not** a changelog. Git history and pull requests record implementation
history. Keep this document focused on the current product direction, important
settled decisions, current development phase, and information that would be
expensive to reconstruct.

## What RawNode is

RawNode is a lightweight, modular RAW photo editor written in C++17 and forked
from ofxrawhost.

The project is evolving from a simple still-image OpenFX host into a general
non-destructive photo editor built around a processor graph.

RawNode currently supports native processors, OpenFX processors and CTL. Planned
processor capabilities include DCTL and LUTs, followed later by masks,
compositing, local adjustments and a more capable branching graph.

macOS is the primary development platform today. The codebase should remain
cross-platform for macOS, Windows and Linux.

The repository's inherited README still contains substantial upstream
ofxrawhost-era material. Treat this document and current source code as more
authoritative for RawNode's present direction until the README is rewritten.

## Product direction

RawNode should remain fast, lightweight and understandable.

It is not intended to become a cloud photo service or a heavyweight catalogue
application. The current direction is:

- minimal DAM/workspace functionality rather than a central catalogue;
- image edits stored in sidecars rather than project/catalogue databases;
- a modular processor model that can host native, OFX, CTL and future DCTL/LUT
  processing;
- a node/graph model that can eventually support masks, compositing and local
  adjustments;
- serious colour-managed photographic workflows;
- cross-platform operation without making the architecture depend on one OS.

The core should stay simple enough that new image-processing features plug into
general graph/rendering mechanisms instead of creating feature-specific
subsystems.

Human readability and maintainability are development requirements. An
experienced C++ developer should be able to understand, review and maintain
RawNode independently, without relying on Codex or Claude to explain the code.
Source and interfaces should expose responsibilities and ownership; see
`docs/CODE_STYLE.md` for the detailed guidance.

## Development workflow

The preferred development workflow is:

1. Ryan and ChatGPT discuss product behaviour, features and architecture.
2. Once the behaviour/design is sufficiently clear, Codex implements a focused
   task and creates or updates a pull request.
3. Claude Code independently reviews the resulting implementation, normally in
   a fresh chat.
4. Ryan and ChatGPT interpret the review and decide what should change.
5. Codex implements required fixes in the same task/PR conversation when they
   belong to that PR.
6. Claude may perform a focused final verification.
7. Merge only when the implementation and review are satisfactory.
8. If a durable project decision changed, update the relevant project
   documentation before considering the work complete.

Before implementing a substantial feature, establish a proportionate design:

- fit with the existing architecture and ownership of state/behaviour;
- important interfaces, data flows and interactions with existing features;
- resource lifetime and concurrency implications where relevant;
- understandable failure handling;
- acceptance criteria and independently verifiable expected results, with their
  source or rationale;
- whether coupling, duplicated responsibilities or speculative abstractions can
  be avoided.

Capture this in the task/PR or relevant document; update `docs/DECISIONS.md` or
the authoritative architecture document for substantial architectural decisions.
Small local fixes need no formal design document or extra approval process.
Reviews should verify the intended behaviour rather than merely confirm the
implementation's own output.

ChatGPT is the long-running product/architecture discussion partner. Codex is the
primary implementation agent. Claude Code is primarily the independent,
adversarial reviewer.

A fresh Codex chat is preferred for a new coherent PR/task. Review-driven fixes
to the same PR should normally stay in that PR's existing Codex chat.

A fresh Claude Code chat is preferred for each independent review. Persistent
Claude review rules live in the repository root `CLAUDE.md`; do not depend on
one indefinitely long Claude conversation.

## Documentation is project memory

The repository, not chat history, should be the durable source of important
RawNode knowledge.

When a significant architectural, product, workflow or roadmap decision is
settled, ask whether project documentation needs to change before the work is
considered complete.

Document responsibilities are:

- `docs/PROJECT_CONTEXT.md` — fast onboarding, product direction, current state,
  priorities, current development phase and near-term roadmap.
- `docs/ARCHITECTURE.md` — authoritative target technical architecture.
- `docs/DECISIONS.md` — authoritative durable design decisions and rationale.
- `docs/ARCHITECTURE_AUDIT.md` — audit evidence and the Codex/Claude
  reconciliation that led to the approved architecture.
- `docs/CODE_STYLE.md` — human-readability and source-organisation principles.
- `CLAUDE.md` — Claude Code's persistent review/development instructions.
- `AGENTS.md` — Codex discovery/pointer to authoritative project documents.
- `ROADMAP.md` — feature and product roadmap; detailed quality follow-ups live
  in this document's "Quality and verification roadmap".
- Git history and pull requests — implementation history and per-change detail.

Do not turn `PROJECT_CONTEXT.md` into a chronological diary. Do not copy every
PR into it. Update it when the information a future project conversation needs
has materially changed.

`docs/ARCHITECTURE.md` and `docs/DECISIONS.md` now contain the reviewed,
approved architecture and decision log.

Older root-level `ARCHITECTURE.md` and `DECISIONS.md` are historical pointers
only. The authoritative documents live under `docs/`.

For human onboarding, use this document for goals, current state and known
limitations; the architecture and decision log for subsystem responsibilities
and rationale; `CLAUDE.md` for build/self-test commands and review expectations;
and `docs/CODE_STYLE.md` for contribution standards. Keep each authoritative
explanation in its existing home rather than duplicating it across guides.

## Current capabilities and baseline

RawNode has already moved substantially beyond the original ofxrawhost design.

Important implemented foundations include:

- generic processor abstraction and parameter API;
- Sidecar V2 with migration from V1, persistent node IDs and preservation of
  unavailable processors;
- native Exposure processor;
- CTL backend using the CTL reference runtime;
- CTL scalar parameters;
- unified colour encoding/registry work used by RAW decode and native colour
  transforms;
- native CST processor with colour space and transfer-function concepts;
- lcms-based colour conversion and associated tests;
- node and full-grade copy/paste plus basic preset support;
- preview pan/zoom and normal photo-editor interaction improvements;
- render lifetime/mutation synchronisation;
- distinct processor-render and display-conversion refresh behaviour;
- preview restoration after full-resolution export.

Since Step 6 merged in PR #58, explicit consumer-owned topology and a sequential
requested-endpoint evaluator now drive preview, Image/Mask inspection and export.
List View derives topology order and keeps off-spine nodes accessible; selection
uses stable NodeIds. Unsupported linear sidecar/clipboard/preset capture refuses
before writing, and full-grade application stages graph/colour/source changes
before committing. Independent whole-Step-6 implementation and architecture
review is complete.

The renderer/concurrency baseline established by PRs #32-#34 is important:

- PR #32 fixed processor lifetime/use-after-free races between rendering and
  graph mutation/export.
- PR #33 prevented display-only refreshes from swallowing required processor
  renders and centralised display conversion from the cached processed image.
- PR #34 restored a current preview after full-resolution export while
  preserving export result/warning/error status.

These fixes are the safe baseline for the architecture audit. A refactor may
replace their implementation, but it must preserve their behavioural and
lifetime guarantees.

## Current renderer shape

Step 4, renderer ownership encapsulation, is complete. PR #45 was independently
reviewed as **SAFE TO MERGE**, validated on Linux with Release/Debug, ASan and
TSan stress coverage, then manually validated on macOS before merge.

RawNode now has one App-owned `RenderRuntime` whose private state owns
preview/export worker lifetime, demand, cancellation, mutation gating, execution
exclusivity and shutdown. Ordinary UI/document/export callers use semantic
requests rather than renderer mutexes, pending/busy flags or
wait/mutate/reschedule protocol.

Preview, inspection and export share the sequential GraphEvaluator. Typed
publication retains request generation,
endpoint, value kind, state and diagnostics. Preview Source frames use shared
ownership; per-evaluation consumer counts bound intermediate raster lifetime.
Presentation buffers remain in the display path and file production in export.
The runtime owns **when** evaluation may execute safely, not what the document
or export means. List View and the minimal V2 safety boundary are implemented
and included in the completed independent whole-Step-6 review and merged code.

## Architectural direction

The original RawNode <-> vkdt audit and the post-Step-4 whole-architecture
checkpoint are complete. Codex and Claude independently reviewed the current
implementation and target against RawNode's product goals and the pinned vkdt
reference and found the direction **sound for the next stage**. Step 5 has now
closed after independent final Codex/Claude implementation-readiness review.
No further broad architectural refactor was required before Step 6. The explicit
graph and sequential evaluator are now implemented, independently reviewed and
merged; persistence and concrete production Mask features remain later
steps. The approved target is documented in `docs/ARCHITECTURE.md`; checkpoint
evidence is in `docs/ARCHITECTURE_AUDIT.md`.

The architectural north star is:

> Every new RawNode feature should have an obvious home. If adding a feature
> requires teaching unrelated parts of the application how that feature works,
> the boundary is probably wrong.

The high-level target remains:

    Image source
         |
         v
    Processing graph
         |
         v
     Graph result
       /      \
      v        v
   Display    Export
    path       path
      |
      v
    Viewer

The implementation model is slightly more explicit:

    UI
     |
     v
    Document / Graph API
     |
     +---- persistent document state
     |
     +---- render runtime
             |
             v
        graph evaluator
             |
             v
       processed result
          /       \
         v         v
      display     export

The important approved principles are:

- one image-processing graph rather than feature-specific processing systems;
- UI/document code expresses edits instead of managing renderer sequencing;
- the render runtime owns worker lifetime, execution exclusivity, demand,
  cancellation and shutdown;
- display and export consume processed graph results;
- masks, opacity, blending, LUTs, CTL, DCTL, OFX and native processors belong to
  image processing rather than creating their own schedulers;
- renderer flags may remain internally if they are the simplest correct
  implementation;
- whole-graph rerendering and sequential execution are preferred before
  fine-grained caches/parallelism;
- explicit topology and the evaluator that uses it should arrive together;
- vkdt is a conceptual reference for graph data flow and ownership, not a system
  to port or a Vulkan requirement.

A useful conceptual distinction remains:

    source / processor / parameter / graph / mask / blend change
        -> image processing work

    display / monitor conversion change
        -> display-only work

Do not create additional invalidation/scheduling domains casually.

## Colour and image-processing direction

RawNode is intended for serious colour-managed photographic workflows.

Unless an operation explicitly requires otherwise:

- preserve floating-point precision;
- preserve negative RGB values;
- preserve values above 1.0;
- avoid unintended clipping;
- preserve alpha;
- keep colour-space/primaries and transfer-function/gamma concepts distinct;
- do not silently introduce display-referred behaviour into scene-referred
  processing.

Colour transforms should have explicit source and destination encodings.

RAW decode and native CST should share colour definitions/math where practical
instead of maintaining duplicate matrices or transfer functions.

The UI direction is to treat colour space and gamma/transfer function as
separate concepts. With the architecture audit/checkpoint complete, further
RAW working-space design may proceed independently when appropriate, provided
it respects the accepted graph/mask contract and does not introduce conflicting
architecture.

Display/monitor conversion conceptually occurs after the processed-image
boundary. A display conversion change should not require rerunning expensive
image processors when the processed image is still valid.

Masks are not image alpha. Global alpha association remains deliberately
unresolved and is not a prerequisite for Step 5 mask-contract discussion.
Before transparent compositing or alpha-carrying workflows, explicitly revisit
TIFF, EXR, OpenFX and JPEG flattening semantics; current source alpha behaviour
is inconsistent across formats.

## Persistence direction

Step 7 should use one versioned, human-readable `.rawnode.json` sidecar per
editable source photograph. Sidecars remain authoritative per-photo state;
exported derivatives receive none (D002, D038). Keep the folder-based workflow
without a separate metadata file, project system, catalogue or database
(D001, D010). The following establishes behaviour and scope, not JSON field names.
Persistence must not depend on transient renderer scheduling state.

### Saved grades and future default presets

A saved grade comprises the processing graph (nodes, parameters and connections)
plus the photograph's RAW working and Output colour settings. A processing or
colour adjustment establishes a grade, as does successful application of a
preset, pasted grade or future default preset. An intentionally saved empty
grade is still a saved grade. Browsing or navigating away from an unedited photo,
or changing only photo metadata, must not manufacture one; a metadata-only
sidecar does not count as a saved grade.

The future default preset is selected from the user's preset folder.
When opening a photograph, use this precedence:

1. Restore an existing saved grade, including an intentionally empty one.
2. Otherwise, apply the configured default preset, if any, using Replace
   semantics, including its RAW and Output colour settings.
3. Otherwise, use the normal global processing/RAW/Output defaults.

A preset applied with Replace supplies its colour settings; global defaults
must not override them. Failed default loading or application must not be
recorded as successful application.
Unreadable, invalid, unsupported or incompatible sidecars are errors, not
unedited photographs; they must not trigger a default preset. Allow source
viewing with a persistent warning, but disable grade editing, paste and preset
application while protected. Ordinary save failures retain unsaved edits through
a simple retry/discard/cancel interaction. Changing the selected default never
alters saved grades. On editable photos users may manually apply another preset;
future multi-photo application extends that action to a selection.
Do not track which default was previously used. Default selection UI and
automatic application are later workflow features, not Step 7 implementations.

### Independent grade and photo metadata

The same sidecar supports logically independent processing-grade data and
minimal photo metadata. Grade data includes the graph, parameters, RAW/Output
colour settings and optional Graph View node positions (D048). Photo metadata
includes future Pick/Reject/Unmarked status and other photo-specific information
only when needed. Grade-only, metadata-only and combined sidecars are all valid.
Metadata updates preserve the grade; grade updates preserve photo metadata.
Prefer retaining untouched section JSON exactly where practical, with precision
and opaque-data preservation mandatory. Metadata changes construct no processors,
decode no photo and build no live graph. External modification/conflict tracking
is outside Step 7; destination validation and atomic writes remain required.

Provide one coherent read/write path with safe atomic file replacement, usable
by future batch operations without opening the photograph in the editor.
Preserve unknown/unavailable processors and information that must survive a
rewrite where safely representable. Unreadable, invalid, unsupported or otherwise
protected sidecars must not be overwritten. If safe interpretation or preservation
is impossible, warn the user, leave the original untouched and do not discard data
or apply a default. No elaborate automatic repair system is needed.
Step 7 establishes minimal metadata persistence support; the Pick/Reject UI is
later work.

### Unified presets and clipboard graph transfers

Use one unified, versioned `.rawnode.json` preset format, identical in structure
to the grade portion of a photograph's sidecar. A preset contains one processor
or an entire connected graph: nodes, parameters, connections, RAW working and
Output colour settings, and optional node layout. It excludes Pick/Reject and
other photo-specific metadata. One Save Preset action saves the current complete
grade, including a one-processor or Source-only grade; retire Save Node Preset.
Node copy/paste remains a distinct editing action. Do not add a preset wrapper
format or second graph implementation. The preset folder contains ordinary
`.rawnode.json` files; another photograph's sidecar can supply its grade data.

Opening a photograph restores its own grade and metadata. Saved presets have
two application modes, both preserving destination photo metadata:

- **Replace:** replace the processing graph and apply the preset's RAW working
  and Output colour settings. Rebind document Source to the destination photo;
  retain valid incoming NodeIds where appropriate for whole-grade replacement.
  Validate and apply transactionally, preserving the original grade on failure.
- **Append:** retain the existing graph and destination RAW/Output settings;
  insert incoming nodes with fresh NodeIds and consistently remapped internal
  connections. Connect or rebind Source/external references only through explicit,
  validated rules. Differing preset colour settings do not cause automatic
  colour-space conversion.

For an ordinary linear chain, Append inserts the incoming chain after the
selected main-spine node, or at the end when none is selected, matching current
node-paste insertion. Branching or incompatible structures require graph validation
and explicit connection rules; refuse unsafe operations with a useful error.
Never guess, silently discard connections or flatten the graph. Detailed
attachment/rebinding policies remain design work, not new machinery to invent
for future UI.

Clipboard transfers share graph serialization, validation and reconstruction,
but a copied node is a fragment with its relevant data, parameters and internal
connections, without requiring complete document Source/output or document-level
colour settings. Node paste allocates a fresh NodeId and preserves destination
RAW/Output settings. Complete-grade copy/paste includes its colours and follows
whole-grade replacement semantics. Payloads need not be literal files on disk.
Capturing a valid supported fragment does not depend on attachment eligibility;
retain internal references and report omitted external Mask/Reference links.
Paste separately validates the destination and refuses unsupported attachments.
Parameter-group expansion is transient and excluded from sidecars, presets and
all clipboard payloads; optional node X/Y positions remain graph layout.

Step 7 replaces the existing linear preset/clipboard paths with this shared
representation and reusable graph validation, identity and reconstruction
mechanisms for Replace/Append. Focused implementation PRs may stage the work;
Step 7 does not require a new preset-management UI. The later Preset Panel can
expose both actions. General branched-graph Append can be staged separately if
existing graph commands cannot support it safely; refuse unsupported operations
without speculative graph-editing machinery. Until replacement, preserve the
D045 safeguards against silently flattening unsupported graphs.

Historical Sidecar V1/V2 and `.rawnodepreset` compatibility is not required;
the old preset format can be retired. Do not add legacy migration readers or
compatibility frameworks solely for development-era files, and do not
automatically delete existing files. Protection from accidental destruction
remains required independently of backwards compatibility. Preset browsing,
folder selection, defaults and multi-photo workflows remain later features.

### Preserve existing node identity behaviour

Opening an existing sidecar preserves valid saved NodeIds. Replacing an entire
grade may retain valid incoming NodeIds because photographs own separate graphs.
Inserting, pasting or duplicating nodes allocates fresh NodeIds.
Connected-fragment insertion follows the Step 5 contract, remapping NodeIds and
internal references consistently. Rebind document Source references to the
destination photograph. Extend existing restoration/insertion behaviour to
arbitrary connections without unnecessary ID regeneration or another identity
management system.

### Two persistent settings scopes

Photo sidecars own each photograph's complete grade, colour settings and photo
metadata. Global application preferences own theme, dock/window layout, export
defaults, default RAW working and Output settings, the selected default grade
preset, CTL/DCTL/OFX/preset folder locations, keyboard shortcuts and future input
mappings. There are no competing folder-level RAW defaults.

As focused Step 7-related persistence cleanup with its own tests, retire
per-folder `workspace.ofxrawhost.json` and `.ofxrawhost-layout.ini` persistence,
and the working-directory `ofxrawhost.ini` fallback wherever it conflicts with
global settings ownership. Move appropriate session/UI preferences, dock/window
layout and defaults to the global settings location; inventory and verify that
essential preferences remain available. Actual photograph processing state stays
in sidecars. No replacement workspace file or folder-specific settings system
is needed. No migration readers are required for these development-era files,
and existing files must not be automatically deleted. Removal and relocation
belong to Step 7-related implementation work.

The selected photograph/workspace may remain transient session state. Remembering
the last opened folder or photograph can be a later global preference. Global
RAW/Output defaults use sensible built-ins until the future Settings UI provides
their exclusive configuration. Keep their stable colour pairs in global
preferences for that future UI. Photo controls require an editable current photo
and never change defaults. Opening/applying a grade
never changes defaults. Step 7 adds neither Settings nor temporary default controls.

## Testing and review philosophy

Passing builds and self-tests are necessary but not sufficient for core changes.

For renderer, graph, lifetime and concurrency work, review should actively
consider:

- use-after-free and processor lifetime;
- data races and deadlocks;
- lost wakeups;
- cancellation and supersession;
- stale-work resurrection;
- mutation while rendering;
- export/preview interaction;
- shutdown;
- failure paths;
- whether regression tests actually fail when the protected behaviour is
  removed.

Prefer deterministic barriers/observers to timing-dependent sleeps.

Use ASan and TSan where practical for renderer/lifetime/concurrency changes.

A known baseline issue exists: leak-enabled ASan reports an OpenFX plugin
descriptor leak during plugin loading (3,186 bytes in 36 allocations when last
measured). Do not use that known leak to dismiss unrelated sanitizer findings.

## Known deferred issues

These are known but are not reasons to expand unrelated focused PRs:

- the OpenFX plugin descriptor leak described above;
- JPEG XL DNG decoding is not yet supported in the current image-loading path;
- TIFF SubIFD handling is incomplete for some RAW-like TIFF structures;
- export UX can block/wait on a non-cancellable full-resolution export;
- thumbnail worker exceptional-unwind lifetime, the macOS pthread-to-joiner
  exception gap, and `makePreview` partial helper-thread launch remain focused
  lifetime/exception-safety follow-ups; normal thumbnail shutdown is safe;
- source sidecar writes remain in-place rather than atomic, and Sidecar V2 still
  includes some GUI preferences; persistence hardening/document-state cleanup
  belongs with Step 7;
- exporting over the exact path of an editable raster source can overwrite the
  source image itself; this pre-existing hazard is separate from export sidecars.

Address remaining renderer-related deferred issues within the approved staged
architecture rather than layering more scheduler state onto the design without
need.

## Current development phase

**Step 6 is complete, independently reviewed and merged in PR #58 as
`6294a034d30168063e2aa92e984a51666f56ca22`.** Step 5's product/architecture
contract is complete. The full graph transition, NodeId selection, topology List
View and minimal linear capture / transactional grade safety are implemented.
Step 7 graph-native persistence is at its focused design checkpoint.
[STEP7_PERSISTENCE_DESIGN.md](STEP7_PERSISTENCE_DESIGN.md) records the current-code
inventory, simplified proposed schema, safe reconstruction/write contracts and
five focused implementation PRs. The revised PR #62 proposal awaits independent
Claude review; Step 7 production implementation has not begun.

Reported macOS testing found normal operation. No complete macOS/NEF
manual-validation results are recorded for `docs/STEP6_MANUAL_VALIDATION.md`,
including native-dialog and successful Nikon NEF full-grade re-decode checks
across differing RAW colour settings. Record per-check evidence and remaining
unverified checks through the quality checklist below; completed Step 6
implementation and independent architecture review remain closed.

Steps 1–4 are complete: owned export execution, frozen parameter edits during
export, centralized graph-edit transactions and encapsulated renderer ownership.
Ordinary structural UI/document callers no longer manage renderer sequencing.

The full post-Step-4 stabilisation gate is complete: the OpenFX multithread
lifetime fix (#48), the behaviour-preserving readability/source-organisation
pass (#50), and the independently completed Codex and Claude architecture
checkpoint. The two immediate checkpoint follow-ups are also complete: #53
implemented D038's source-only sidecar rule and #54 deterministically protects
single-level mutation-completion ordering.

The checkpoint found no broad architecture blocker before Step 5. App remains
a practical composition root; the remaining thread-launch/lifetime findings are
focused implementation follow-ups, not reasons to reopen the architecture audit.
See `docs/ARCHITECTURE_AUDIT.md` for the candidate dispositions.

The remaining sequence is:

    Step 5: First mask/graph product contract       [complete]
          |
          v
    Step 6: Explicit topology + sequential DAG evaluator [complete; reviewed and merged]
          |
          v
    Step 7: Versioned graph persistence
          |
          v
    Switchable Graph/List editor                   [UI milestone, D047]
          |
          v
    Step 8: Concrete first mask path + remaining implementation details

The completed gates are stabilisation/review work, not extra numbered
architecture steps. The checkpoint validates the direction for the next stage,
not a permanent or finished architecture.

The architectural goal is to make future changes cleaner across the application:
new features should plug into the graph, render runtime, display path, export
path or persistence rather than spread knowledge of themselves across unrelated
subsystems.

Important accepted implementation constraints:

- preserve all #32-#34 renderer/lifetime guarantees;
- export represents the accepted document state when Export is confirmed;
- parameter editing is unavailable while export owns live processor instances;
- structural graph mutation remains a single UI/control-thread contract;
- failure-triggered preview restoration should not overwrite meaningful
  operation status;
- renderer encapsulation follows protocol simplification rather than preceding
  it;
- no revision/key framework is required in the near term;
- the first DAG is sequential and uses whole-graph invalidation;
- OpenFX remains unary and serialized initially;
- caches, parallel branches, ROI/tile scheduling and job systems remain
  deliberately deferred.

Step 5 has settled the graph-shape/product questions that must precede
topology implementation. The accepted contract is detailed in
`docs/STEP5_GRAPH_CONTRACT.md` and D039–D046.

In particular:

- Mask is a typed scalar-coverage graph value, distinct from image alpha;
- generic Mask/Strength/Blend ownership belongs to graph/node evaluation rather
  than every processor backend;
- List View is a topology view, with a derived main Image spine for the Step 6
  UI subset;
- required/optional inputs, unavailable propagation, fail-open optional Mask
  coverage and hard dimension incompatibility are defined;
- branch colour is deliberately numerical unless an operation explicitly
  declares colour interpretation;
- geometric Reference is raster-frame context without automatic transform
  propagation;
- Image/Mask requested-endpoint inspection is part of Step 6;
- Source/document-output and source-only behaviour are explicit.

Step 8 implements the first concrete production Mask path and settles remaining
feature details such as exact geometric coordinates, Mask combine equations,
non-Normal blend maths and detailed Layer/Parallel Mixer arithmetic.

Do not add masked-processor renderer state, a special mask scheduler, mask fields
that bypass the graph or mandatory opacity inside every processor backend.

Global alpha association remains separately deferred until transparent
compositing or alpha-carrying I/O requires it.

## Near-term roadmap

Next architecture/product work:

- Step 7 graph-native persistence, then the switchable Graph/List editor (D047),
  then Step 8's first concrete production Mask path.
- Both editor views share the authoritative document graph and NodeId selection;
  switching views introduces no additional execution model.

The checkpoint, immediate follow-ups, Step 5 contract and whole-Step-6
implementation/architecture review are complete. No additional broad audit or
cleanup is required before the next stage.

Colour-management design and DCTL/LUT or other processor work may continue
independently when appropriate. Step 5 is not a prerequisite for that unrelated
work, provided it respects the accepted graph/mask contract and does not introduce
conflicting architecture.

The Step 6 implementation retains the existing ownership boundary and sequential
whole-graph invalidation. Processor creation may enter a mutation even on failure;
staging assumes one control thread and fixed port interfaces. Future SlotSettings
fields must be included in no-op comparison, and arbitrary editing needs a UX for
disconnected required inputs. Sidecar-loading exceptions and persistence atomicity
remain Step 7 work. No further compatibility infrastructure is required for the
historical linear format.

Do not introduce speculative caches, schedulers or broad optimisation
infrastructure merely because a future DAG could use them.

## Quality and verification roadmap

**Continuous practice:** apply proportionate readability and independent
verification checks to implementation PRs. Follow the design/acceptance workflow
above, `CLAUDE.md` for review expectations and `docs/CODE_STYLE.md` for source
guidance.

**Step 7 design checkpoint — simplified draft, independent review pending:**
[STEP7_PERSISTENCE_DESIGN.md](STEP7_PERSISTENCE_DESIGN.md) investigates whether
the existing graph model, identifiers, interfaces and ownership can be persisted
reliably and understood without unnecessary
architectural changes. Begin with the accepted Step 5 contract and completed
Step 6 review findings, including the transition limitations in
`docs/ARCHITECTURE.md`. Use `docs/STEP5_GRAPH_CONTRACT.md` ("Step 7 graph
persistence target"), D045, D047 and D048 in `docs/DECISIONS.md`, and the minimal
layout-persistence requirement under "Graph / List interface" in `ROADMAP.md`.

**Persistent identity and reconstruction:** verify the expected handling of:

- stable NodeIds and node-local SlotIds, including allocation/high-water marks
  across save/reopen cycles so later allocation cannot reuse preserved identities;
- stable node-kind/backend identities and typed port identifiers under one file
  schema version. Additional interface/backend versions require an actual
  implementation need; display-name changes must not break saved photographs.
  Identifier renames need explicit alias/version handling, never positional
  guessing; no legacy migration reader is required now;
- document Source/output endpoints, fixed input references and ordered Slot
  arrays with their slot-local settings. Storage order is not execution order;
- processor parameters and implemented node controls such as enabled state;
- missing/unavailable processor state, using existing static interfaces where
  sufficient. Current `nodeInterface()` declarations need no per-node snapshot;
  unsupported graph semantics protect the file instead;
- external references required by actual processors, such as CTL's script-path
  identity, without a generic asset-management system.

**Persistence/workflow checkpoint:** verify the saved-grade distinction, safe
independent grade/metadata updates, shared JSON reconstruction, existing identity
behaviour and photo/global settings ownership in "Persistence direction" above.
Include existing preset/clipboard transition and workspace-persistence cleanup
in Step 7 scope. Default-preset UI/application, culling UI, multi-photo operations
and batch/workspace export remain separate focused workflow milestones.

**Accepted sidecar/layout direction (D048):** each editable source photograph
uses one sidecar for the authoritative graph and optional compact X/Y positions
keyed by NodeId. Step 7 provides a home for layout without requiring Graph View
or saved positions; a node without a position remains valid. Keep layout separate
from executable data. Repositioning must not affect connections, validation,
rendering or graph evaluation; reopening the editable source photograph should
restore any saved arrangement.

Preferred default Graph/List View remains a global preference. Decide graph
viewport zoom/pan persistence with Graph View; temporary viewport state should
not expand Step 7.

Keep one authoritative graph shared by List View, Graph View, preview and export.
Do not add a separate project file, catalogue/database, duplicate graph topology
or unnecessary repeated data. Do not persist rendered images, intermediates,
caches or unnecessary transient UI state. Keep the format simple and readable:
a little readable metadata is preferable to complexity introduced merely to
minimise file size. Binary serialization, compression or an elaborate
persistence framework needs a demonstrated requirement.

The proposed serialization schema, field names and optional-field handling are
recorded in the checkpoint for review before establishing the format and beginning
implementation. D048 in `docs/DECISIONS.md` records the accepted sidecar/layout
direction; D049 records PR #61's independent metadata, shared-grade and global
preferences ownership. Neither entry implies that Step 7 is implemented.
Step 7 supports the subsequent Graph/List milestone (D047), without implementing
Graph View as part of Step 7.
Keep this design checkpoint focused on persistence; the completed
whole-Step-6 review remains closed. If the existing design is sound, record that
conclusion and proceed without refactoring.

The remaining quality improvements are tracked work, not automatic blockers for
unrelated features; tooling can proceed alongside Step 7 where practical.

- [x] Complete the whole-Step-6 independent implementation/architecture review.
- [ ] Record macOS manual-validation results from
      `docs/STEP6_MANUAL_VALIDATION.md`, confirming native-dialog behaviour and
      successful Nikon NEF full-grade re-decode across differing RAW colour
      settings; identify individual checks still unverified. Before Step 7
      replaces the relevant capture/sidecar paths, record the outstanding
      results or an explicit, documented waiver for the unverified checks.
- [ ] Complete independent review of the focused pre-persistence checkpoint in
      `docs/STEP7_PERSISTENCE_DESIGN.md` before Step 7 finalises the graph format.
- [ ] Extend CI where practical (Linux build + self-test already implemented):
      add a macOS PR build + self-test and a separate ASan/UBSan job. Keep
      TSan/stress manual initially where automation is impractical.
- [ ] Enable meaningful compiler warnings for RawNode-owned sources first
      (not vendored `third_party`), then trial a narrow clang-tidy profile.
      Establish a baseline and prioritise real defects over cosmetic churn.
- [ ] Expand independent numerical/reference-image tests **beyond Step 6's
      existing graph-equivalence tests** for colour transforms, exposure,
      preservation of precision/alpha and negative or >1 RGB values, and
      relevant export paths. Define numerical tolerances for compiler/platform
      differences rather than demanding byte-identical output.
- [ ] Consider focused fuzz/property testing for graph validation and the new
      graph-native persistence reader, and other untrusted file-input boundaries
      when practical. Historical Sidecar V1/V2 compatibility testing is optional;
      it does not constrain Step 7 or require a migration reader.
- [ ] Establish lightweight image-processing performance/memory baselines as
      useful.
- [ ] Improve human-developer onboarding and the inherited README when useful,
      using the authoritative documentation above rather than duplicating it.

**After Step 8's first concrete production Mask path:** perform another focused
readability/architecture checkpoint for the affected graph, processor and UI
boundaries. Do not default to a whole-repository pass after every feature.

**Before a stable public release:** review cross-platform builds, input/file
errors, recovery, dependency/licensing obligations, performance and remaining
critical test gaps. An independent paid C++ audit may be worthwhile when
affordable but is optional, not a development gate.

Keep the above checklist current as work lands in focused PRs. GitHub Issues
are currently disabled, so this document is the follow-up tracker.

## Starting a future project conversation

A future ChatGPT or other project-planning session should first read:

1. `docs/PROJECT_CONTEXT.md`;
2. `docs/ARCHITECTURE.md`;
3. `docs/DECISIONS.md`;
4. `docs/ARCHITECTURE_AUDIT.md` when audit rationale or the migration
   reconciliation matters;
5. relevant current pull requests/issues for the task at hand.

Then inspect current source where needed rather than assuming this summary
contains implementation details.

If this document conflicts with newer architecture/decision documentation, use
the newer explicit decision and update this file so the conflict does not
persist.
