# RawNode Roadmap

This roadmap is intentionally staged so major architectural assumptions are proven before more complex features are added.

## Current foundation

The original backend-neutral architecture work is now in place:

- generic processor/node model;
- generic parameters;
- Sidecar V2 persistence;
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
- portable node and full-grade presets.

The current codebase is a clean base for feature development. New work should return to small, focused PRs.

## Current architecture/stabilisation sequence

The approved eight-step architecture migration is in progress. Steps 1–6 are
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
- Step 7 (next): one graph-native JSON format for sidecars, presets and clipboard,
  with independent grade/photo metadata, optional node layout, safe atomic writes
  and photo/global settings ownership. Transition existing preset/copy/paste
  functionality and retire workspace settings persistence in focused changes.
- **Graph View UI milestone (between Steps 7 and 8; not a new architecture
  step; D047):** implement a switchable Graph/List editor on the same
  authoritative document graph with shared NodeId selection.
- Step 8: first concrete mask path and remaining implementation details under
  the Step 5 product/architectural contract.

The completed stabilisation tasks are not extra migration steps.

Detailed persistence behaviour is in `docs/PROJECT_CONTEXT.md` under
"Persistence direction". Default-preset UI/automatic application, culling,
multi-photo actions and other photography workflows are later focused milestones;
they can proceed independently without changing the architecture sequence.

Post-Step-6 code quality, CI, verification and milestone readability follow-ups
are tracked in `docs/PROJECT_CONTEXT.md` under "Quality and verification roadmap".
The focused pre-persistence checkpoint is due before Step 7 finalises the format;
the whole-Step-6 implementation/architecture review is already complete.

## Completed editing workflow milestone

RawNode now supports:

- copying and pasting a single processor node through a versioned system-clipboard payload;
- copying and pasting the complete serial processing chain between images, including RAW working and Output colour space/gamma;
- portable `.rawnodepreset` files for both single nodes and full grades.

These currently use the linear `PersistNode` / `PersistChain` representation and
`.rawnodepreset` files. Step 7 transitions existing node/full-grade presets and
copy/paste to shared graph-native `.rawnode.json` serialization and reconstruction;
the old format can be retired. Full-grade transfers retain RAW and Output colour
settings. This transition is separate from future preset-browser/default UI.

A follow-up editing workflow should add filmstrip multi-selection and allow a
copied full grade or full-grade preset to be applied to all selected photos in
one action. Each source receives its own validated grade and sidecar update;
do not add a second graph model or persistence path for batch application.

## Default grade preset

Add a configurable default full-grade preset selected from the user's preset
folder. Opening precedence is **saved grade → configured default preset → global
RAW/Output defaults**, following the saved-grade and protected-sidecar rules in
`docs/PROJECT_CONTEXT.md`. Browsing or metadata-only changes cannot manufacture a
grade; deliberately saved empty grades still win. Changing the default leaves
saved grades untouched, and users can manually apply another preset.

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

Keep this folder-based with no catalogue/database or automatic deletion. The
Step 7 establishes minimal photo-metadata persistence support; the culling UI
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

**Delivery milestone:** After Step 7 graph-native persistence and before Step 8
introduces the first user-facing masks, build the graphical Node/Graph View.
This is a UI/product milestone, not an additional numbered architecture refactor.

The editor must have an explicit switch between **Graph View and List View**.
Switching views changes presentation only: both show and edit the same
authoritative document graph, preserve NodeId selection, and produce identical
processing results. Neither view owns a separate copy of topology or render
state.

Step 6 makes the graph authoritative. The List View derives its main Image spine
from explicit topology; moving rows rewrites topology rather than rearranging
node storage. Off-spine nodes must remain accessible, even if arbitrary-DAG List
View presentation stays deliberately simple.

Initial Graph View scope:

- draggable nodes with pan/zoom and typed Image/Mask ports and connections;
- connection editing through the existing validated graph commands;
- shared selection/inspection and editing actions with List View;
- node layout positions saved with the editable source document, and restored
  when reopened; Step 7 should make room for this without coupling layout to
  graph evaluation.

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

After the base architecture and editing workflow are mature, investigate:

- node masks;
- opacity;
- brush / gradient / key masks;
- shared / group masks;
- local adjustments;
- AI-generated masks.

## Completed architecture milestones

### Generic processing core

The original OFX-specific representation was refactored into a generic processor/node model.

Mixed backends now share the same chain, parameter, persistence, UI, and render seams.

### Generic parameters

OFX, native and CTL processors expose controls through the common parameter abstraction.

### Sidecar V2

Persistence stores processor/backend identity, order, bypass state, parameter state, RAW/output colour state, and stable choice IDs.

Unknown future data is protected from destructive overwrite where possible.

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
