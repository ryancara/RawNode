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

- folder browsing and photo sidecars without a central catalogue;
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

D052 sets the product-approval, visible-change reporting and proportionate
architecture-reuse rules for implementers and independent reviewers. Reuse
existing systems by default, but consider justified new modules without
prematurely building speculative infrastructure.

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
- `docs/STEP7_PERSISTENCE_DESIGN.md` — accepted graph-native persistence schema
  and lifecycle; `docs/workspace-format.md` — global settings format.
- `SIDECAR.md` — current persistence overview and labelled historical formats.
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
- graph-native `.rawnode.json` sidecars with stable node IDs, complete topology,
  independent grade/photo metadata and preservation of unavailable processors;
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
uses stable NodeIds. Graph-native sidecar/preset/clipboard capture preserves complete grades;
full-grade application stages graph/colour/source changes before committing.
Linear Append and fragment Paste commit one batch while retaining unrelated
destination branches. Independent whole-Step-6 implementation and architecture
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
or export means. List View was included in the completed independent Step 6
review; Step 7 has replaced its temporary V2 persistence boundary with the
shared graph-native codec.

## Architectural direction

The original RawNode <-> vkdt audit and the post-Step-4 whole-architecture
checkpoint are complete. Codex and Claude independently reviewed the current
implementation and target against RawNode's product goals and the pinned vkdt
reference and found the direction **sound for the next stage**. Step 5 has now
closed after independent final Codex/Claude implementation-readiness review.
No further broad architectural refactor was required before Step 6. The explicit
graph and sequential evaluator are now implemented, independently reviewed and
merged. Step 7 persistence is also complete. The Graph/List editor now includes its
PR #79 foundation, PR #80 contextual Inspector/workspace and PR #82 connection/
deletion editing (D051), all merged and independently reviewed. Owner-reported
native macOS validation of PR #82 is in `docs/GRAPH_VIEW_VALIDATION.md`;
corrupted-sidecar protection and export/edit overlap scenarios, as well
as Windows GUI checks, remain separately tracked. PR 2C's approved contextual
node placement is implemented for independent review/native validation (D051);
the concrete production Mask path follows. PR 2A
historical validation remains in `docs/WORKSPACE_INSPECTOR_VALIDATION.md`.
The approved target is
documented in `docs/ARCHITECTURE.md`; checkpoint
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

PNG export quantises the graph's already encoded values to 8-bit RGBA; Output
remains a manual interpretation/tagging contract, not an automatic conversion
(see `docs/ARCHITECTURE.md`, "Colour semantics"). Tagged PNGs carry one generated
ICC profile immediately after IHDR, before any palette or image data. The PNG
writer adds no sRGB, gAMA or cHRM chunks; its internal untagged path adds no colour
metadata. ICC profiles retain the selected gamut and transfer function. The
existing DaVinci Intermediate limitation remains: ICC cannot fully represent
scene values above 1.0, and export warns that external applications may clip
highlights.

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

Step 7 implements one versioned, human-readable `.rawnode.json` sidecar per
editable source photograph. Sidecars remain authoritative per-photo state;
exported derivatives receive none (D002, D038). Keep the folder-based workflow
without a separate metadata file, project system, catalogue or database
(D001, D010). The following establishes behaviour and scope, not JSON field names.
Persistence must not depend on transient renderer scheduling state.

### Saved grades and future default presets

A saved grade comprises the graph, parameters/connections and photograph's RAW
working/Output settings. Opening a photograph in the editor establishes and
persists its grade even without adjustments, including a legitimate Source-only
grade. This ART/vkdt-inspired lifecycle supersedes the prior editor-browsing rule
(D050). Folder scanning/discovery and thumbnail generation create no sidecars.
Opening a metadata-only photo adds a grade while preserving metadata; restoring
an existing grade needs no immediate rewrite. Folder auto-open affects only the
opened photograph. Create the initial sidecar only after opening succeeds.

Save the complete current editable grade before leaving, same-photo reopening
and closing, before live replacement or editor/runtime teardown, even if nothing
changed. Explicit Save Grade remains. Reuse the open path/protection state;
do not add live hasGrade/gradeDirty flags, comparison snapshots, per-edit
notifications or persistence revisions. Initial-save failure can leave the
opened grade editable with a persistent non-modal warning, separate from render
status; clear it after a successful grade save to its sidecar or actual photo
switch.
It is presentation text, not save tracking; save again at the next boundary.
Document actions refuse departure after failure unless Discard was explicit.
Unwritable folders may prompt on navigation/close even with no adjustments.

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
Failure handling for that future default-preset application remains deferred to
its implementation milestone.

### Independent grade and photo metadata

The same sidecar supports logically independent processing-grade data and
minimal photo metadata. Grade data includes the graph, parameters, RAW/Output
colour settings and optional Graph View node positions (D048). Photo metadata
includes future Pick/Reject/Unmarked status and other photo-specific information
only when needed. Grade-only, metadata-only and combined sidecars are all valid.
An absent grade remains valid for unopened photos; absent metadata.mark is the
canonical Unmarked representation. Metadata-only updates do not open a photo.
Metadata updates preserve the grade; grade updates preserve photo metadata.
Prefer retaining untouched section JSON exactly where practical, with precision
and opaque-data preservation mandatory. Metadata changes construct no processors,
decode no photo and build no live graph. External modification/conflict tracking
is outside Step 7: the live editor is authoritative while a photo is open, and
concurrent external sidecar edits are unsupported. Destination validation and
atomic writes remain required.

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

Opening a photograph establishes/restores its grade and metadata. Saved presets
have two application modes, both preserving destination photo metadata:

- **Replace:** replace the processing graph and apply the preset's RAW working
  and Output colour settings. Rebind document Source to the destination photo;
  retain valid incoming NodeIds where appropriate for whole-grade replacement.
  Validate and apply transactionally, preserving the original grade on failure.
- **Append:** retain the existing graph and destination RAW/Output settings;
  insert incoming nodes with fresh NodeIds and consistently remapped internal
  connections. Connect or rebind Source/external references only through explicit,
  validated rules. Differing preset colour settings do not cause automatic
  colour-space conversion.

Initially Append requires a supported linear incoming chain. A branched
destination is allowed at a valid main-spine insertion point: after the selected
node, or at Output when unselected. Rewire only that successor/Output, preserving
all other branches. Refuse unsupported attachment rather than flattening or
guessing. General branched incoming attachment remains deferred. Preset saves
use ordinary confirmed overwrites, including valid photograph-sidecar paths,
with destination metadata preservation, strict validation and atomic replacement.

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

Step 7 replaced the linear preset/clipboard paths with this shared
representation and reusable graph validation, identity and reconstruction
mechanisms for Replace/Append. It added no preset-management UI. The later
Preset Panel can
expose both actions. Branched incoming Append can be staged separately if
existing graph commands cannot support it safely; refuse unsupported operations
without speculative graph-editing machinery. D045's no-flattening requirement
continues to apply to the graph-native paths.

Historical Sidecar V1/V2 files are protected without migration; live legacy
preset/clipboard readers and `.rawnodepreset` workflows are retired. Do not add
legacy migration readers or
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
defaults and default RAW working/Output settings. Future default-preset choice,
CTL/DCTL/OFX/preset locations, shortcuts and input mappings belong to this same
scope when implemented. There are no competing folder-level RAW defaults.

Step 7 PR 5 implements global `preferences.json` and ImGui `layout.ini` in
`~/Library/Application Support/RawNode/` on macOS and `$XDG_CONFIG_HOME/RawNode/`
(or `~/.config/RawNode/`) on Linux. Windows uses roaming application data when
supported; checked atomic writes remain unsupported there. Paths resolve once
before UI/photo startup. Unavailable locations warn and keep settings in memory,
never falling back to CWD or photograph folders. Preferences save at normal
shutdown even with no folder; cancelled photo departure keeps the app usable.
Theme, visibility, fallback sizes, preview/export choices and both default colour
pairs are retained. ImGui owns docking/window geometry. Optional fields and
sensible defaults allow later implemented preferences without new infrastructure.

`workspace.ofxrawhost.json`, `.ofxrawhost-layout.ini` and CWD `ofxrawhost.ini` are
retired without readers, writers, migration or automatic deletion. Folder browsing,
scanning and first-photo auto-open remain. There is no replacement workspace JSON.
See [application settings](workspace-format.md) for format/error details.

The selected photograph/folder may remain transient session state. Remembering
the last opened folder or photograph can be a later global preference. Global
RAW/Output defaults are read and validated from global preferences; valid stored
pairs initialise new grades, with sensible built-in fallback for absent/invalid
pairs. The future Settings UI configures these same fields; Step 7 adds no
temporary controls. Photo controls require an editable current photo and never
change defaults, nor does opening/applying a grade.

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

A known application-shutdown issue remains: OpenFX support-library descriptors
allocated during Describe/DescribeInContext require ActionUnload (independently
reproduced as 3,186 bytes in 36 allocations). Full self-tests now send Unload after
test-owned instances are destroyed; CI keeps leak detection enabled without leak
suppressions. The normal application shutdown still needs a focused correction.

## Known deferred issues

These are known but are not reasons to expand unrelated focused PRs:

- the production OpenFX plugin descriptor shutdown leak described above;
- JPEG XL DNG decoding is not yet supported in the current image-loading path;
- TIFF SubIFD handling is incomplete for some RAW-like TIFF structures;
- export UX can block/wait on a non-cancellable full-resolution export;
- thumbnail worker exceptional-unwind lifetime, the macOS pthread-to-joiner
  exception gap, and `makePreview` partial helper-thread launch remain focused
  lifetime/exception-safety follow-ups; normal thumbnail shutdown is safe.

Address remaining renderer-related deferred issues within the approved staged
architecture rather than layering more scheduler state onto the design without
need.

## Current development phase

**Step 8 in progress (2026-10-29):** D054 and [STEP8_MASKING_CONTRACT.md](STEP8_MASKING_CONTRACT.md) record Inactive Mask bypass, unary generic Opacity and explicit binary Combine direction. PRs #97–#102 are merged, including native Radial Mask → Exposure and Mask Only preview, with owner-reported independent Claude review and native macOS GUI validation for #102. Photo remains the default; selection can retarget active Mask inspection or restore Photo, and photo/grade replacement resets the transient view. Viewer mode stays outside grades, history and export. Non-AI masking workflows remain the priority; Mask Overlay, on-image handles, other geometry and freeform generators, Combine/modifiers and basic qualifiers/conversion follow as independently scoped features. CTL/future DCTL remain Image-producing. Step 9's approved whole-codebase audit follows agreed core non-AI masking completion; SAM/AI masking is deferred without a scheduled milestone.

Undo/Redo design and behaviour are approved in D053. Stage 1 was independently
reviewed, approved and merged in PR #92 at
`c2624989213bb656f9552b3c52c7146dbfd432c2`. Its staged reconstruction and
logical-node identity rules remain the replay boundary.

Stage 2 was independently reviewed, approved and merged in PR #93 at
`342fd2f1b97994931a4a27806348162ebe05793d`: one App-owned in-memory snapshot
history/cursor, 100 undo steps and a 64 MiB accounted-memory cap. Successful photo
switches/reopening reset it; failed/cancelled navigation and Save Grade preserve
it. Completed graph/document edits reuse existing mutation ownership. Replay uses
one staged restoration without post-replay capture. The accepted OFX initialization
apparent-no-op limitation remains documented in [UNDO_HISTORY.md](UNDO_HISTORY.md).

Stage 3 is complete and merged in PR #94 (`e2e4c4f`): shared parameter widgets complete
meaningful gestures through ImGui lifecycle, and Graph Canvas completes metadata-
only drags without processor execution. One Inspector edit record handles deferred
numeric values and disappearing controls; existing canvas state handles drag
interruptions. UI completion defers during export; failed capture preserves the
live edit and pauses history until recovery. Pending metadata is distinguished
from processor edits so position-only copying cannot omit them. Automatic layout
positions remain housekeeping. The existing Edit menu and platform shortcuts share
history/document/export eligibility and preserve active text-input shortcuts.

No history state controls sidecar saving. Ordinary replacement/sidecar policies,
full-graph reconstruction and document/runtime ownership remain unchanged. This
stage was independently reviewed before merge.

**Step 6 is complete, independently reviewed and merged in PR #58 as
`6294a034d30168063e2aa92e984a51666f56ca22`.** Step 5's product/architecture
contract is complete. The full graph transition, NodeId selection, topology List
View and minimal linear capture / transactional grade safety are implemented.
**Step 7 is complete: PRs 1–5 merged as #63, #65, #66, #68 and #69, ending at
`69572b265075cc44cc9b06ee52ad1c803263f345`.**
[STEP7_PERSISTENCE_DESIGN.md](STEP7_PERSISTENCE_DESIGN.md) records the historical
pre-implementation inventory, accepted schema, safe reconstruction/write contracts
and five completed implementation PRs. Claude's final independent verification
of PR #62's design, D050 lifecycle and final corrections at
`75e7affd4014f090a90293d69b6a157050576864` returned **SAFE TO MERGE**
(result supplied by the user).
Graph-native records, the strict codec and atomic file updates now support live
photo opening/saving. Editable opening establishes a grade; document departure,
same-photo reopening and shutdown save it through checked POSIX atomic
replacement; Windows replacement remains unsupported.
Protected photographs remain viewable but noneditable. Photo colours are separate
from defaults, and save warnings remain separate from render/export status.
Presets and clipboard now use the shared graph-native grade/node codec. Unified
Save Preset supports complete Source-only/branched grades and validated metadata-
preserving overwrites. Replace retains incoming identity and applies colours;
Append and fragment Paste stage one linear splice into eligible branched destinations
with fresh IDs and position remapping. Fragment capture retains internal links and
detaches/reports external Mask/Reference links independently of attachment support.
Legacy transfers and folder workspace/layout persistence are retired. Global
preferences/layout complete the final Step 7 PR 5 scope. No dirty-state or
background-save system is introduced.

The owner confirms that independent post-Step-7 Codex and Claude architecture
audits both concluded **HEALTHY — TARGETED FIXES RECOMMENDED**. Both found RawNode
ready for the Graph/List editor without another major architecture refactor.
The targeted post-Step-7 correction gate is complete; remaining findings are
tracked under "Quality and verification roadmap" below.

**Owner-reported macOS evidence, 2026-10-17:** all five basic PR #69 checks passed
before merge (reviewed head `6dec6e4848f039cbb8528ebda71c4636893000bb`):

1. After Quit, `~/Library/Application Support/RawNode/` contained both
   `preferences.json` and `layout.ini`.
2. Docking changes survived restart.
3. Two photograph folders retained the same global layout and preferences.
4. Neither folder acquired new `workspace.ofxrawhost.json` or
   `.ofxrawhost-layout.ini` files.
5. A Nikon NEF opened and was edited; its grade and colour settings restored
   after switching photographs and restarting.

See [the dated manual record](STEP6_MANUAL_VALIDATION.md#owner-reported-pr-69-macos-results--2026-10-17).
This establishes basic NEF opening/editing/restoration, not full historical T3,
native-dialog, preset or failure-path coverage. Those remaining checks, including
invalid/unwritable settings, quit-time save failures and real Linux desktop
dialogs, remain outstanding. The 2026-10-16 PR 3 prerequisite waiver remains
historical and is not passing evidence. Completed Step 6 implementation and
independent architecture review remain closed.

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
    Step 7: Versioned graph persistence              [complete; PRs 1–5 merged]
          |
          v
    Targeted graph safety + PNG colour corrections   [complete; independently reviewed]
          |
          v
    Switchable Graph/List editor                   [#79/#80/#82/#87 merged, D047/D051]
          |
          v
    Step 8: Practical non-AI masking workflows        [in progress]
          |
          v
    Step 9: Whole-codebase readability/simplicity/architecture audit [planned]

The completed gates are stabilisation/review work, not extra numbered
architecture migration steps. Step 9 is an independently numbered **quality
milestone**, not a change to the original graph/render migration sequence.
The earlier checkpoint validated the direction at that time, not a permanent
or finished architecture; D055 deliberately authorises a later full review.

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

- The visual Graph/List foundation (#79), contextual Inspector/workspace (#80)
  and focused connection/deletion editing (#82) are merged after independent
  review. Native macOS Graph View testing was reported successful; the
  corrupted-sidecar protection, export/edit overlap and Windows GUI checks
  remain separately tracked.
- PR 2C's approved contextual placement was merged as #87 before Step 8.
  New processors appear right of their upstream Image predecessor with an
  80-unit gap in both views, even before Graph View has opened. Preserve
  existing positions through graph edits; permit overlap without collision
  avoidance. Source stays draggable; virtual Output stays automatic and
  non-draggable. Any outstanding native validation remains tracked separately.
- Add Processor now follows the shared selection in List and Graph View:
  Source/main-chain processors insert before their next main-chain consumer;
  off-chain Image processors start another downstream branch without changing
  document Output or existing consumers. No selection, virtual Output and the
  final main-chain processor append at Output. Invalid Image selections refuse;
  successful insertion selects the new processor. This separate approval (D051)
  changes connections only. PR 2C's visual placement is separately approved;
  Insert Before/After and Auto Arrange still require separate decisions.
  Empty nodes and processor assignment/replacement
  remain deferred, without blocking PR 2C or Step 8. Clipboard/preset policies remain.
  Keep graph NodeId/connections distinct from processor instances.
- Preview owns resolution; Nodes owns Add Processor and List/Graph; Inspector
  follows Source/processor/virtual Output/no selection. The top toolbar owns file
  actions and status/warnings, retiring Left Panel. Historical UI-layout migration
  is waived, while new layouts persist normally and photo/grade contracts remain.
- Both editor views share the authoritative document graph and NodeId selection;
  switching views introduces no additional execution model.

The checkpoint, immediate follow-ups, Step 5 contract and whole-Step-6
implementation/architecture review are complete. These focused corrections do
not reopen the architecture audit or add numbered steps. Unrelated image-format
and lifetime findings stay tracked separately and need not delay Graph View.

Colour-management design and DCTL/LUT or other processor work may continue
independently when appropriate. Step 5 is not a prerequisite for that unrelated
work, provided it respects the accepted graph/mask contract and does not introduce
conflicting architecture.

The Step 6 implementation retains the existing ownership boundary and sequential
whole-graph invalidation. Processor creation may enter a mutation even on failure;
staging assumes one control thread and fixed port interfaces. Future SlotSettings
fields must be included in no-op comparison, and arbitrary editing needs a UX for
disconnected required inputs. Step 7 added staged sidecar loading and checked
atomic persistence. No further compatibility infrastructure is required for the
historical linear format.

Do not introduce speculative caches, schedulers or broad optimisation
infrastructure merely because a future DAG could use them.

## Quality and verification roadmap

**Continuous practice:** apply proportionate readability and independent
verification checks to implementation PRs. Follow the design/acceptance workflow
above, `CLAUDE.md` for review expectations and `docs/CODE_STYLE.md` for source
guidance.

**Completed post-Step-7 safety corrections:** merged PR #71 fixed OFX outstanding
image-handle lifetime: each fetched handle owns its property set until release,
while pixel buffers remain borrowed. Export now checks both byte counts and
stream-close results for tagged/untagged PNG and JPEG. Both export entry points
refuse destinations identifying the currently opened source, including relative,
symlink and hardlink aliases. Other photographs and existing derivatives remain
valid targets; exports remain sidecar-free. Linux Release self-tests cover these
behaviours and failure recovery. Derivative writes are not atomic or crash-safe;
a failed write can leave incomplete output.

**Completed post-Step-7 correction gate:** Copy Node ownership/capture,
parameter-edit preview scheduling, graph traversal depth and PNG colour
metadata/ICC placement are corrected and independently reviewed. PNG regression
coverage independently checks chunks, CRCs, profiles and pixels.

**Completed Graph/List UI delivery (D047):** visual foundation (#79),
workspace/Inspector (#80), wiring/deletion (#82) and contextual placement
(#87, PR 2C) are merged; outstanding native/platform checks remain tracked
separately. **Step 8 masking is in progress** (first production Mask path and
Mask Only preview merged as #101–#102). Those earlier corrections were a
preparatory gate, not extra architecture migration steps. The newly approved
Step 9 whole-codebase quality audit (D055) is a distinct future milestone.

**Tracked separately; not prerequisites for Graph View:** TIFF
photometric/extra-channel handling, EXR chromaticities, and the existing JPEG
writer undefined behaviour. Give them focused correctness PRs and appropriate
tests/sanitizer evidence when scheduled; address a concrete user-data or safety
regression earlier if discovered. TIFF photometric/extra-channel handling
becomes a Step 8 prerequisite if the first Mask path decodes external TIFF
mask files. Other documented lifetime/exception-safety follow-ups remain
tracked, not silently resolved by this plan.

**Completed Step 7 checkpoint and implementation — retained verification criteria:**
The criteria below describe the implemented contract and remain useful for
regression review. They do not claim completion of outstanding manual checks.
[STEP7_PERSISTENCE_DESIGN.md](STEP7_PERSISTENCE_DESIGN.md) records how
the existing graph model, identifiers, interfaces and ownership can be persisted
reliably and understood without unnecessary architectural changes. The completed
work used the accepted Step 5 contract and completed Step 6 review findings,
including the transition limitations in `docs/ARCHITECTURE.md`. Use `docs/STEP5_GRAPH_CONTRACT.md` ("Step 7 graph
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
Editor opening establishes a grade; unopened metadata-only photos remain valid.
No live dirty/presence tracking is required for lifecycle saves (D050).
Step 7 completed the preset/clipboard transition and workspace-persistence
cleanup. Default-preset UI/application, culling UI, multi-photo operations
and batch/workspace export remain separate focused workflow milestones.

**Accepted sidecar/layout direction (D048):** each editable source photograph
uses one sidecar for the authoritative graph and optional compact X/Y positions
keyed by NodeId. Step 7 provides a home for layout without requiring Graph View
or saved positions; a node without a position remains valid. Keep layout separate
from executable data. Repositioning must not affect connections, graph validation,
rendering or graph evaluation; reopening the editable source photograph should
restore any saved arrangement.

The Nodes panel remembers List/Graph globally, defaulting to List. Graph View
derives typed ports and connections from the live graph, shares transient
Source/processor/virtual Output selection with the contextual Inspector, and uses `app.nodePositions` for generated
and dragged coordinates. Add Processor stages missing coordinates from the
pre-insertion layout and publishes them only after successful insertion, so
List and Graph additions agree. Surviving positions never automatically move
on Add, deletion or reconnection; overlap is permitted (D051).
Normal grade-save boundaries persist these positions;
protected photos only derive drawing coordinates. Canvas pan/zoom remain transient
and do not expand Step 7.

Keep one authoritative graph shared by List View, Graph View, preview and export.
Do not add a separate project file, catalogue/database, duplicate graph topology
or unnecessary repeated data. Do not persist rendered images, intermediates,
caches or unnecessary transient UI state. Keep the format simple and readable:
a little readable metadata is preferable to complexity introduced merely to
minimise file size. Binary serialization, compression or an elaborate
persistence framework needs a demonstrated requirement.

The reviewed serialization schema, field names and optional-field handling are
recorded in the checkpoint and implemented by the five merged Step 7 PRs.
D048 in `docs/DECISIONS.md` records the accepted sidecar/layout
direction; D049 records independent metadata, shared grades and global preferences;
D050 records the editor-opening/save lifecycle. Implementation is complete;
manual platform evidence remains a separate requirement below.
Step 7 supports the subsequent Graph/List milestone (D047), without implementing
Graph View as part of Step 7.
This design checkpoint remains focused on persistence; the completed
whole-Step-6 review remains closed.

The remaining quality improvements are tracked work, not automatic blockers for
unrelated features; tooling can proceed alongside the next milestone where practical.

- [x] Complete the whole-Step-6 independent implementation/architecture review.
- [x] Record the five owner-reported PR #69 macOS checks from 2026-10-17:
      configuration-file presence, docking restart/folder continuity, no new
      folder settings files and Nikon NEF grade/colour restoration.
- [ ] Complete the remaining manual checks in `docs/STEP6_MANUAL_VALIDATION.md`:
      historical native dialogs and comprehensive Nikon NEF/T3 re-decode cases,
      PR 4 presets, remaining PR 5 settings/failure cases and real Linux desktop
      dialogs. The 2026-10-16 PR 3 prerequisite waiver is not passing evidence.
- [x] Final independent Claude verification of the design, D050 lifecycle and
      remaining corrections at `75e7affd4014f090a90293d69b6a157050576864`:
      **SAFE TO MERGE** (result supplied by the user).
- [x] Extend automatic push/PR CI: Apple Silicon macOS Release build, arm64
      verification and full self-test; separate Linux Debug ASan/UBSan full
      self-test alongside the existing GCC Release job. Independent fault probes
      check fatal diagnostics and leak detection. The sole temporary UBSan
      exception is `shift-base` in `stbiw__jpg_writeBits` (see
      `.github/sanitizers/ubsan.supp`); the JPEG correction remains deferred.
      Keep TSan/stress manual initially; Windows and extra macOS architectures
      remain later milestones. Execution evidence belongs in the CI PR/runs.
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

**Step 9 — comprehensive whole-codebase quality audit (owner-approved; D055):**
Begin when the agreed **Step 8 exit gate** for practical non-AI masking is met;
record and approve that gate in `docs/STEP8_MASKING_CONTRACT.md` while planning
the remaining work. SAM/AI masks are not prerequisites. Step 9 does **not**
replace ongoing per-PR readability and correctness reviews during Step 8.
The original narrower post-first-Mask checkpoint is superseded by Step 9;
avoid a premature redesign, but review architectural risk in each masking PR
so high-cost defects are not deferred unnoticed.

**Canonical phases, subsystem coverage and completion criteria are in the
Step 9 section of `ROADMAP.md`.** Each subsystem receives separate Codex
and Claude audits before findings are compared; the owner reconciles the
recommendations. At Step 9 kickoff create `docs/STEP9_AUDIT.md` to record
evidence, severity, viable alternatives, owner decisions and verified
dispositions, following `docs/ARCHITECTURE_AUDIT.md` as a precedent.
This Project Context records milestone progress only, not an audit changelog.

D055 authorises **proposals** for substantial rewrites or architectural
reassessment. Every Step 9 implementation PR still needs an agreed focused
scope, independent review, tests and normal safeguards; architecture,
behavioural and persistence changes require explicit owner approval.
Maintain correctness, user-data integrity, colour fidelity and concurrency
guarantees. The open quality checklist above (including reference-image
tests, warnings, performance baselines and human-developer onboarding)
informs Step 9 baseline and closure; it is not automatically completed
or erased when the audit starts.

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
