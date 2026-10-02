# RawNode Roadmap

This roadmap is intentionally staged so major architectural assumptions are proven before more complex features are added.

The roadmap is a source of truth for future RawNode work. Features may move between phases as implementation experience changes, but colour management, persistence, processor behaviour, and RAW architecture should remain explicit rather than hidden or inferred.

## Current position — October 2026

The following foundational work is complete or substantially complete:

- generic backend-neutral Processor model;
- generic parameter model;
- Sidecar V2 with stable node IDs and missing-processor preservation;
- native Exposure reference processor;
- standard CTL backend using the official CTL interpreter;
- ART CTL entry-point, scalar metadata, presentation metadata, presets, and ART-style numeric shaping;
- explicit LibRaw camera-space -> RawNode colour boundary;
- selectable RAW initial working spaces:
  - Linear Rec.709;
  - Linear Rec.2020;
  - ACES2065-1 / AP0;
- explicit user ownership of processor colour-space settings and final Output Tag.

RawNode does not automatically change processor colour-space parameters or infer colour-space changes from arbitrary processors.

## Immediate next sequence

### PR #17 — Explicit CST processor

Add a first-class colour-space transform processor.

Initial targets should include the spaces most useful to current workflows, such as:

- Linear Rec.709;
- Linear Rec.2020;
- ACES2065-1 / AP0;
- ACEScg / AP1 where useful;
- DaVinci Wide Gamut / Intermediate;
- sRGB / Rec.709 display-referred transforms where appropriate.

Requirements:

- explicit source and destination spaces;
- explicit transfer-function handling;
- no hidden conversions around other processors;
- generic parameter and Sidecar V2 support;
- predictable behaviour with CTL, OFX, future DCTL, LUT, and CLF processors.

The user owns the colour pipeline. CST nodes change colour encoding deliberately; other processors receive the RGB values currently flowing through the graph.

### PR #18 — Display, Output Tag, and export interpretation cleanup

Remove inherited assumptions that depend on whether the processing chain is empty.

Goals:

- make final pixel interpretation explicit at all times;
- keep display conversion separate from creative processing;
- keep Output Tag entirely user-controlled;
- make preview and export interpretation consistent;
- clearly separate image encoding from embedded profile/tagging;
- address awkward cases such as 8-bit export from linear wide-gamut spaces;
- avoid pretending RawNode knows the colour state after arbitrary OFX, CTL, DCTL, or other processors.

### Documentation refresh

After the CST and display/output work settles:

- update ARCHITECTURE.md and DECISIONS.md to reflect the explicit-CST model;
- document the LibRaw camera-space boundary;
- document selectable RAW working spaces;
- update CTL/ART completion status;
- replace old references to automatic per-node colour tracking;
- keep ROADMAP.md aligned with the actual implementation.

## Core editing and workflow usability

These are high-priority usability features and should be addressed before or alongside deeper processor-format work.

### Preview zoom and pan

Add practical image navigation:

- mouse wheel / trackpad zoom;
- pinch-to-zoom where available;
- click-drag pan;
- Fit;
- 100%;
- useful keyboard shortcuts.

RawNode already has preview zoom/pan state, so this should build on the existing preview model rather than introduce a separate viewer architecture.

### Copy and paste nodes

Allow copying one processing node and pasting it elsewhere.

Copy should preserve:

- processor/backend identity;
- processor parameters;
- enabled/bypass state;
- relevant node UI state.

Paste must create a new persistent node instance ID.

### Copy and paste processing graphs between photos

Allow copying the current processing chain and pasting/applying it to another image.

The copied graph should include:

- processors;
- node order;
- CST nodes;
- parameters;
- bypass state;
- processor-local UI state where useful.

Image-specific RAW decode state should not be copied accidentally. The target photo keeps its own RAW working-space/decode state unless the user explicitly chooses otherwise.

This feature should reuse the same backend-neutral serialisation concepts as Sidecar V2 rather than implementing a parallel copy format.

### Presets

Support reusable editing presets.

Initial useful scopes:

- **node preset** — saved settings for one processor;
- **graph/grade preset** — a reusable processing chain that can be applied to another photo.

Graph presets and graph copy/paste should share as much serialisation and validation code as possible.

ART CTL's own internal script presets remain processor-specific behaviour and are separate from RawNode-level node/graph presets.

## DCTL compatibility

Implement DCTL support incrementally.

Initial target:

- load user-selected .dctl files;
- basic RGB colour transforms;
- common scalar/vector maths;
- DEFINE_UI_PARAMS parsing;
- generic parameter integration;
- Sidecar V2 persistence;
- colour-processing DCTLs.

Later possibilities:

- includes;
- LUT access;
- textures/spatial operations;
- broader Resolve compatibility.

Do not require perfect Resolve compatibility before DCTL becomes useful.

DCTL processors do not receive hidden RawNode colour conversions. Users place CST nodes around DCTLs where needed.

## First-class LUT and CLF processors

Add colour-transform processors that do not depend on OFX LUT loaders.

### LUT

Initial target:

- .cube files;
- 1D and 3D support as appropriate;
- trilinear/tetrahedral interpolation where appropriate;
- generic node behaviour;
- Sidecar V2 persistence.

### CLF

Treat CLF as a richer transform format rather than merely another LUT container.

CLF may contain:

- matrices;
- ranges;
- LUTs;
- transfer functions;
- other colour operations.

## Folder workflow features

Expand RawNode's folder-based workflow without introducing a catalogue/database.

Priorities:

- Pick / Neutral / Reject flags;
- move rejected source files deliberately to the operating-system Trash/Recycle Bin;
- export the whole current workspace/folder using each image's own sidecar state;
- batch export controls;
- preserve the folder itself as the workspace;
- keep classification metadata lightweight and transparent.

Review/rating metadata should be designed so it remains portable and does not become a hidden catalogue.

## Graph/List interface

Allow switching between graph and list views of the same underlying processing structure.

Initially both may represent the same serial chain.

Goals:

- number-key node selection;
- clear selected-node behaviour;
- graph/list parity;
- preserve stable node IDs;
- only enable branching/merging when a concrete workflow requires it.

Graph copy/paste and presets should already operate on the same underlying graph representation.

## Assignable input system

Add configurable keyboard/mouse parameter bindings.

Keep the input abstraction generic so future inputs can share the same path:

- keyboard;
- mouse;
- MIDI;
- OSC / TouchOSC;
- dedicated hardware controllers.

Bindings should normally target generic parameters on the currently selected node instance.

## RAW architecture and quality work

The LibRaw camera-space boundary is now established, but deeper RAW work remains.

### Decoder abstraction

Continue toward replaceable decoder/developer interfaces so LibRaw can later coexist with or be replaced by:

- Rawler;
- RawSpeed;
- another decoder where useful.

Evaluate candidates using:

- camera/format support;
- metadata access;
- image quality;
- performance;
- cross-platform build complexity;
- integration complexity;
- compressed DNG / JPEG XL support.

JPEG XL-compressed DNG support is an explicit RawNode requirement.

### RAW quality improvements

Keep these separate from the colour-architecture work:

- investigate highlight / white-balance clipping;
- investigate image-dependent white-level behaviour such as LibRaw adjust_maximum_thr;
- improve DNG colour handling where practical;
- dual-illuminant/profile interpolation where relevant;
- editable RAW-stage white balance if a future decoder/developer architecture makes it worthwhile;
- more genuinely float RAW processing if future quality goals justify it.

## Masks and local adjustments

After the base architecture is mature, investigate:

- node masks;
- node opacity;
- brush masks;
- gradient masks;
- key/qualifier masks;
- shared/group masks;
- local adjustments;
- AI-generated masks.

Mask generation should remain separate from image adjustment. AI should initially generate masks rather than become a separate image-processing architecture.

## Known technical debt

### Full-resolution export thread

Full-resolution export currently runs on a detached background thread while reusing live processor/node objects.

This can race with:

- node deletion;
- node reordering;
- parameter edits;
- opening another image;
- processor render-size changes between preview and full-resolution export.

The preferred long-term model is to snapshot the processing graph and parameters when Export is requested, then render that immutable snapshot in the background.

If independent processor cloning is not yet practical, the safer interim solution is to synchronise export and prevent graph mutation while the export render is using the live graph.

This should be fixed before batch/workspace export becomes a major workflow feature.

## Cross-platform and release work

Continue validating macOS, Windows, and Linux as RawNode grows.

Important future work includes:

- reproducible builds;
- CI for all supported platforms;
- standalone application packaging;
- dependency/version documentation;
- plugin/script path portability where practical.

## Working rules

- Do not start multiple major architectural changes simultaneously.
- Prefer proving one seam at a time so regressions are attributable and reversible.
- Colour-space changes must be explicit and user-controlled.
- Sidecars remain the authoritative per-image edit state.
- Folder workflow remains catalogue-free.
- Reuse generic Processor, Parameter, node, and Sidecar infrastructure rather than building backend-specific parallel systems.
