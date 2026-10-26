# RawNode Roadmap

This roadmap is intentionally staged so major architectural assumptions are proven before more complex features are added.

## Current foundation

The original backend-neutral architecture work is now in place:

- generic processor/node model;
- generic parameters;
- graph-native photo sidecars with independent grade and metadata sections;
- native processors;
- CTL backend;
- shared colour architecture based on `ColorEncoding`;
- native CST with separate gamut and transfer-function controls;
- RAW working gamut + transfer function;
- explicit Output gamut + transfer function;
- stable persisted choice IDs;
- third-party RGB ICC input converted through Little CMS into Linear Rec.2020;
- Linux CI and extracted self-tests;
- node copy / paste;
- full-grade copy / paste;
- unified `.rawnode.json` grade presets and graph-native clipboard transfers;
- global `preferences.json` and ImGui `layout.ini`; folder workspace persistence
  is retired.

The current codebase is a clean base for feature development. New work should return to small, focused PRs.

## Current architecture/stabilisation sequence

The approved eight-step architecture migration is in progress. Steps 1–7 are
complete. Step 4 (renderer ownership encapsulation) landed in PR #45 after
independent review, sanitizer/stress validation and routine macOS validation.

The full post-Step-4 stabilisation gate is complete:

1. OpenFX host multithread lifetime correction (#48);
2. behaviour-preserving readability/source organisation (#50), independently
   reviewed and validated on macOS;
3. the whole-architecture checkpoint against RawNode's product goals and the
   pinned vkdt reference.

Codex and Claude independently found the direction sound for the next stage;
no further broad refactor is justified before the next stage. Immediate
follow-ups #53 (source-only sidecars) and #54 (mutation-completion ordering
regression) are complete. Remaining findings in `docs/ARCHITECTURE_AUDIT.md`
are non-blocking follow-ups.

- **Step 5 — complete:** the first mask/graph product contract is settled and
  independently reviewed; see `docs/STEP5_GRAPH_CONTRACT.md`.
- **Step 6 — complete and independently reviewed:** explicit topology and one
  sequential requested-endpoint DAG evaluator, merged in PR #58
  (`6294a034d30168063e2aa92e984a51666f56ca22`).
- **Step 7 — complete:** PRs 1–5 merged as #63, #65, #66, #68 and #69
  (final merge `69572b265075cc44cc9b06ee52ad1c803263f345`). Graph-native sidecars,
  unified presets, shared clipboard serialization, independent grade/photo
  metadata, optional node layout, checked atomic writes and global preferences
  are implemented; folder workspace persistence is retired.
- **Post-Step-7 correction gate — complete:** Copy Node ownership/capture,
  parameter-edit preview scheduling, graph traversal depth and PNG colour
  metadata/ICC placement are corrected and independently reviewed.
- **Graph View UI milestone (between Steps 7 and 8; D047), in progress:**
  PR #79 added the dockable List/Graph foundation, PR #80 the contextual
  Inspector/workspace, and PR #82 the focused wiring/deletion interactions.
  All three are merged. PR 2C's contextual Add Processor placement is approved
  and implemented for independent review; native validation remains pending.
- **Step 8:** first concrete mask path and remaining implementation details
  under the Step 5 product/architectural contract. TIFF and EXR format
  correctness and JPEG encoder UB remain independently tracked follow-ups,
  not prerequisites for Graph View, nor for masks unless the first Mask path
  decodes external TIFF files.

The completed stabilisation tasks are not extra migration steps.

Detailed persistence behaviour is in `docs/PROJECT_CONTEXT.md` under
"Persistence direction". Default-preset UI/automatic application, culling,
multi-photo actions and other photography workflows are later focused milestones;
they can proceed independently without changing the architecture sequence.

Post-Step-6 code quality, CI, verification and milestone readability follow-ups
are tracked in `docs/PROJECT_CONTEXT.md` under "Quality and verification roadmap".
The focused Step 7 design checkpoint and implementation are complete; outstanding
manual platform checks remain tracked separately and are not passing results.

## Completed editing workflow milestone

RawNode now supports node/fragment and complete-grade copy/paste through the
shared graph-native codec, plus one **Save Preset** action for complete grades in
`.rawnode.json`, including Source-only and branched grades. **Replace** applies
the graph and RAW/Output colours; **Append** inserts an ordinary linear incoming
grade at a validated main-spine point while retaining unrelated destination
branches and colours. Fragment capture and attachment eligibility are separate.
See `SIDECAR.md` and `docs/STEP7_PERSISTENCE_DESIGN.md` for exact semantics.

The former linear `PersistChain` transfers and `.rawnodepreset` files are
historical; live legacy transfer readers are retired without migration or deletion.

A follow-up editing workflow should add filmstrip multi-selection and allow a
copied full grade or preset to be applied to all selected photos in
one action. Each source receives its own validated grade and sidecar update;
do not add a second graph model or persistence path for batch application.

## Default grade preset

**Planned; not implemented.** Add a configurable default preset selected from
the user's preset folder, applied
using **Replace**. Opening precedence is **saved grade → configured default preset
→ global RAW/Output defaults**, following the saved-grade and protected-sidecar rules in
`docs/PROJECT_CONTEXT.md`. Folder scanning and thumbnail generation create no
sidecars; successfully opening a photograph for editing establishes and persists
its grade (D050). Metadata-only updates to unopened photos do not create grades.
Deliberately saved empty grades still win. Changing the default leaves saved
grades untouched, and users can manually apply another preset.

Default selection and automatic application are later workflow features, not
Step 7 prerequisites; future multi-photo grade application supports a selection.

## Application Settings and preset panel

Add a straightforward application **Settings** menu/window for global RAW/Output
defaults, export/UI preferences and configurable/default search locations for
CTL, DCTL, OFX and preset files.
Keep paths and preferred default graph/preset in global application settings,
without per-folder settings persistence. Implement plugin-specific options when
their backends are usable rather than creating unused configuration infrastructure.

Provide **View → Preset Panel** to show/hide a preset browser. Leave space for
later shortcut assignment, scripting and other preferences without designing
an elaborate settings framework in advance.
The panel can expose **Replace** and **Append**. A general-purpose branched-graph
Append workflow may be staged separately when existing graph commands cannot
support it safely; Step 7 provides shared persistence/reconstruction mechanisms.

## DCTL compatibility

Implement incrementally.

Initial target:

- basic RGB transforms;
- common scalar/vector maths;
- `DEFINE_UI_PARAMS` parsing;
- colour-processing DCTLs.

Later possibilities:

- textures / spatial operations;
- includes;
- LUT access;
- broader Resolve compatibility.

Do not require perfect Resolve compatibility before DCTL becomes useful.

## LUT and CLF processors

Add first-class colour-transform processors rather than requiring LUTs to be hosted through OFX.

Initial LUT target:

- `.cube` files;
- trilinear / tetrahedral interpolation as appropriate;
- generic node behaviour and graph-native persistence.

CLF should be treated as a richer transform format rather than assumed to be only a LUT, because it may contain matrices, ranges, LUTs, and other operations.

## Pick / reject workflow

Add a simple photo-culling workflow:

- Pick (Flag) / Reject / Unmarked as minimal optional per-photo sidecar metadata,
  independently of whether the source has a saved processing grade;
- filmstrip filtering and efficient navigation between images;
- later, hide rejects or move rejected photos (and associated sidecars) to the
  operating system Trash through an explicit, confirmed user action.

Keep this folder-based with no catalogue/database or automatic deletion. Step 7
establishes minimal photo-metadata persistence support; the culling UI
and file actions can follow later.

## Batch / workspace export

Allow exporting multiple selected images or all eligible images in the current
workspace/folder, with options to export picked photos and skip rejects.
Reuse the existing single-image export settings and processing path, initially
sequentially, with progress, cancellation and per-photo failure reporting.

Full-resolution export execution is now owned and serialized against live
processor use. Future batch/workspace export should reuse that ownership model
rather than reintroducing detached access, processor cloning or a separate
scheduler without a concrete product need. No project/catalogue system is
required.

## Graph / List interface

**UI delivery milestone in progress (D047):** the dockable Nodes panel switches
between List and Graph over the same authoritative graph. The visual foundation
(PR #79), contextual workspace (PR #80) and connection/deletion UI (PR #82)
are merged. PR 2C adds the approved contextual processor placement described
below, before Step 8 introduces masks. Independent review/native validation remain.
This is a UI/product milestone, not an additional numbered architecture refactor.
The completed correction gate and deferred image-format issues are tracked in
`docs/PROJECT_CONTEXT.md` under "Quality and verification roadmap".

The editor must have an explicit switch between **Graph View and List View**.
Switching views changes presentation only: both show and edit the same
authoritative document graph, preserve NodeId selection, and produce identical
processing results. Neither view owns a separate copy of topology or render
state.

Step 6 makes the graph authoritative. The List View derives its main Image spine
from explicit topology; moving rows rewrites topology rather than rearranging
node storage. Off-spine nodes must remain accessible, even if arbitrary-DAG List
View presentation stays deliberately simple.

**PR 1 — Visual foundation (merged as #79):**

- draggable nodes with pan/zoom and typed Image/Mask ports and connections;
- shared NodeId selection and the existing parameter inspector;
- List controls retain reorder, enable/disable and deletion;
- node layout positions saved with the editable source document, and restored
  when reopened; Step 7 already stores optional positions separately from
  graph evaluation;
- global List/Graph preference, with transient canvas pan/zoom.

**PR 2A — Contextual Inspector and workspace (merged as #80)**

- Preview owns preview-specific controls, including resolution.
- Nodes owns List/Graph switching and the existing searchable Add Processor picker.
- Inspector shows Source input/RAW settings, processor parameters, Output colour
  and export settings, or a neutral state according to one transient selection.
- A compact top toolbar provides Open Image, Open Folder and Export; File menu
  actions and shortcuts remain available.
- Retire the Left Panel. Default docking is Preview above Nodes, Inspector on
  the right and Filmstrip along the bottom. Persist new arrangements normally
  with preferences.json and layout.ini; historical UI-layout migration is not
  required and configuration files must not be automatically deleted.
- Keep append insertion, grade/sidecar contracts, colour semantics and renderer
  ownership unchanged.

PR 2A implementation and validation are recorded in
[WORKSPACE_INSPECTOR_VALIDATION.md](docs/WORKSPACE_INSPECTOR_VALIDATION.md);
independent review and outstanding platform checks remain separate.

**PR 2B — Visual graph editing (merged as #82)**

- Connect, reconnect and disconnect typed ports; edit document Output connections.
- Delete nodes via Delete/Backspace and context menus.
- Keep context menus limited to Delete Node and input Disconnect. Reuse the
  existing graph commands and deletion behaviour without changing their ownership.
- Add Processor retains PR #80's append-to-document-Output behaviour regardless
  of selection. Selection-aware insertion and Before/After were withdrawn from
  PR 2B by the owner; they require a separate product decision, not an automatic
  move into PR 2C. See D051's superseding scope clarification.

Selection-aware Add Processor is now separately approved under D051, superseding
PR 2B's append-only deferral above. PR #85 implements that connection policy;
PR 2C separately implements the approved visual placement below.

**PR 2C — Graph node placement (approved; implemented for independent review)**

- Add Processor uses PR #85's selection-aware insertion workflow in both views.
  After successful insertion, place the new processor immediately right of its
  upstream Image predecessor at the same Y (240-unit width plus an 80-unit gap).
- Preserve all existing generated or manually moved coordinates when adding,
  deleting or reconnecting. Overlap is allowed; no collision avoidance or Auto
  Arrange. List additions produce the same layout before Graph View is opened.
- Source remains draggable. Output remains an automatically positioned virtual,
  non-draggable endpoint; it may move as the graph grows and has no saved position.
- Reuse NodeId-keyed positions and sidecar saves. Layout alone neither renders
  nor writes sidecars. Tests cover placement, both views, preservation, overlap,
  refusal and save/reopen; native macOS/Windows checks remain pending in
  [GRAPH_VIEW_VALIDATION.md](docs/GRAPH_VIEW_VALIDATION.md).
- Paste/duplicate placement, contextual navigation, Insert Before/After, Auto
  Arrange and processor-construction changes remain outside this approval.

**Separate future design — Empty nodes and processor assignment**

Deferred, not a prerequisite for PR 2C or the first Step 8 Mask path. NodeId
and graph connections are already separate from processor instances in the
authoritative graph. If intentionally empty pass-through nodes are approved
later, define their identity, operation, interface, persistence and
assign/replace semantics independently from unavailable processors; preserve
compatible connections without adding another graph or storage system by default.

Step 8 remains the first concrete Mask implementation under the accepted Step 5
contract, followed by compositing and local adjustments. This sequence adds no
architecture migration steps and does not reopen completed graph decisions.

Add number-key node selection as a separate small interaction improvement.

Branching, typed Mask endpoints and future mixers use the same graph. The first
user-facing mask workflows remain Step 8, after the visual editor milestone.
Do not build a second graph model, renderer or persistence representation for
Graph View.

## Assignable input system

Add user-configurable keyboard shortcuts and keyboard/mouse parameter bindings,
with editing in the application Settings window. Store mappings as global
preferences rather than per-photo sidecar data.

Keep the input abstraction generic so future MIDI, OSC / TouchOSC, jog wheels,
or other hardware controllers can use the same path. Scripting remains a later
extension.

## RAW architecture refactor

Separate RAW decoding from RAW development behind interfaces.

Evaluate LibRaw, Rawler, and RawSpeed using real criteria:

- format / camera support;
- metadata access;
- image quality;
- performance;
- cross-platform build complexity;
- compressed DNG / JPEG XL support where relevant.

Do not replace LibRaw without a measured benefit.

## Masks and local adjustments

**Step 8 approved contract:** [docs/STEP8_MASKING_CONTRACT.md](docs/STEP8_MASKING_CONTRACT.md), D054. Mask generators bypass to Inactive (not white/black), Image/Mask transforms bypass to same-type pass-through, and generic Opacity is available on unary transforms with a defined compatible pass-through. Standalone generators and Mask Combine have no generic Opacity initially. A dedicated binary Combine node ignores inactive operands when enabled, while its own bypass passes through A; keep unavailable distinct from intentionally inactive. Retain one typed scalar Mask graph and no separate scheduler/alpha channel. Implement in focused PRs: evaluator-only Inactive bypass, Image-node Mask/Opacity, Radial->Exposure, preview handles/modes, Combine and modifiers. The precise feathered-mask arithmetic is deferred until Combine implementation.

Following the initial Radial/Exposure masking slice, further work includes:

- brush and path masks, additional gradients and qualifiers;
- reusable/grouped masks and local adjustments;
- AI-generated masks, only when an explicit product need exists.

## Completed architecture milestones

### Generic processing core

The original OFX-specific representation was refactored into a generic processor/node model.

Mixed backends now share the same chain, parameter, persistence, UI, and render seams.

### Generic parameters

OFX, native and CTL processors expose controls through the common parameter abstraction.

### Sidecar V2 — historical, superseded by Step 7

V2 established backend-neutral identity and opaque parameter preservation, but
encoded serial order. The live editor now uses graph-native `rawnode` version 1
sidecars with explicit connections and endpoints. V1/V2 files are protected
without migration; compatibility helpers remain only for self-tests.

### Native processors

Native Exposure proved the backend-neutral processor path.

Native CST now provides explicit:

- Input Colour Space;
- Input Gamma;
- Output Colour Space;
- Output Gamma.

### CTL

The CTL backend uses the official CTL reference interpreter and supports standard CTL plus the current ART-compatibility layer.

### Colour architecture

Runtime colour state uses one canonical `ColorEncoding { gamut, gamma }` model.

The shared gamut/transfer registries drive RAW, CST, Output, matrix derivation and RawNode-authored ICC profiles.

Arbitrary third-party RGB ICC profiles are transformed with Little CMS instead of being guessed as the nearest RawNode gamut.

## Deferred edge cases

These should be addressed only when a real use case justifies them:

- TIFF/SubIFD CFA RAW detection for RAW files renamed to `.tif`;
- richer unsupported-ICC user warnings;
- unusual LUT-based ICC profiles with extended-range float TIFF data.

## Working rule

Do not start multiple major architectural changes simultaneously.

Prefer small, focused PRs that prove one behaviour or seam at a time so regressions are attributable and changes remain reversible.
