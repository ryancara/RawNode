# RawNode Roadmap

This roadmap is intentionally staged so major architectural assumptions are proven before more complex features are added.

## Phase 0 — Fork validation

Verify the inherited OFX Raw Host behaviour on macOS, Windows, and Linux.

Confirm:

- RAW and standard image opening;
- OFX discovery/rendering;
- reorder/bypass;
- filmstrip/folder workflow;
- sidecar save/restore;
- export;
- build reproducibility.

Add CI builds for all three platforms.

## Phase 1 — Generic processing core

Refactor the current OFX-specific node representation into a generic processor/node model.

Initially, only `OFXProcessor` needs to exist.

Success criterion: the app behaves the same as before the refactor.

## Phase 2 — Generic parameters

Move parameter UI/state away from direct OFX assumptions.

Existing OFX parameters should be surfaced through the common parameter abstraction.

## Phase 3 — Sidecar V2

Generalise persistence to store:

- RAW settings;
- working colour space;
- node instance IDs;
- processor/backend type;
- plugin/script identifier;
- order/connections;
- bypass state;
- parameters;
- space for future mask metadata.

Unknown processors/parameters should be preserved where possible.

## Phase 4 — Prove mixed processors

Add a minimal native scene-linear Exposure processor as the first non-OFX reference implementation.

This proves the backend-neutral processor, parameter, persistence, UI, and render seams. It does not imply that every photographic adjustment should be reimplemented as a native processor.

Test mixed processing:

```text
OFX -> Native Exposure -> OFX
```

Verify preview, reorder, persistence, and export.

## Phase 5 — CTL

Add CTL as the first external non-OFX backend using the official CTL reference interpreter.

Initial standard-CTL target:

- load a user-selected `.ctl` file;
- execute a conventional `void main(...)`;
- support varying float `rIn/gIn/bIn` and `rOut/gOut/bOut`;
- support optional `aIn/aOut`;
- expose defaulted uniform scalar `float`, `int`, and `bool` inputs through the generic parameter API;
- use direct numeric fields when plain CTL provides no min/max metadata rather than inventing slider ranges;
- leave arrays/vectors, richer metadata, and host-specific parameter conventions for later;
- resolve sibling/imported CTL modules using the script directory plus the normal CTL module path;
- persist the script path through Sidecar V2 and preserve a missing script as a placeholder.

Validate mixed stacks such as:

```text
OFX -> CTL -> OFX
```

ART compatibility is a layer on top of standard CTL, not the definition of the CTL backend.

### ART CTL compatibility

Build an adapter above the standard CTL backend that can:

- recognise and call `ART_main`;
- parse `@ART-param` metadata into RawNode's generic parameter model;
- honour ART labels and colour-space tags where practical;
- resolve helper libraries such as `_artlib.ctl`;
- preserve standard CTL behaviour for non-ART scripts;
- add richer ART features such as choices, groups, arrays/curves, presets, and `@ART-lut` incrementally rather than all at once.

## Near-term workflow features

Add folder-workflow features without introducing a catalogue/database:

- export the entire current workspace/folder, using each image's own sidecar state;
- Pick / Neutral / Reject flags;
- a deliberate command to move rejected source files to the operating system Trash/Recycle Bin;
- preserve the folder itself as the workspace and keep classification state lightweight and transparent.

## Colour-management refactor

Make colour-space state explicit rather than inferred from whether the processing chain is empty.

Initial goals:

- audit and correct the inherited RAW colour-space tagging;
- support Linear Rec.2020 and ACES2065-1 / AP0 as working-space options;
- add a first-class CST/colour-space transform processor so colour space can change deliberately between nodes;
- track the colour space flowing through the chain;
- keep display conversion and export tagging/transforms separate from creative processors.

The current inherited RAW path sets LibRaw `output_color = 1` (sRGB primaries) with a linear transfer curve, while RawNode currently tags the result as Linear Rec.2020. This mismatch must be resolved as part of this refactor.

## Phase 6 — DCTL compatibility

Implement incrementally.

Initial target:

- basic RGB transforms;
- common scalar/vector maths;
- `DEFINE_UI_PARAMS` parsing;
- colour-processing DCTLs.

Later possibilities:

- textures/spatial operations;
- includes;
- LUT access;
- broader Resolve compatibility.

Do not require perfect Resolve compatibility before DCTL becomes useful.

## Future processor formats — LUT and CLF

Add first-class colour-transform processors rather than requiring LUTs to be hosted through OFX.

Initial LUT target:

- `.cube` files;
- trilinear/tetrahedral interpolation as appropriate;
- generic node behaviour and sidecar persistence.

CLF should be treated as a richer transform format rather than assumed to be only a LUT, because it may contain matrices, ranges, LUTs, and other operations.

These can be introduced after the generic persistence and mixed-processor seams are proven.

## Phase 7 — Graph/List interface

Allow switching between graph and list views of the same processing structure.

Initially both can represent a serial chain.

Add number-key node selection.

Enable graph branching only when a concrete use case justifies it.

## Phase 8 — Assignable input system

Add configurable keyboard/mouse parameter bindings.

Keep the input abstraction generic so future MIDI, OSC/TouchOSC, or hardware controllers can use the same path.

## Phase 9 — RAW architecture refactor

Separate RAW decoding from RAW development behind interfaces.

Evaluate LibRaw, Rawler, and RawSpeed using real criteria:

- format/camera support;
- metadata access;
- image quality;
- performance;
- cross-platform build complexity;
- compressed DNG/JPEG XL support.

JPEG XL-compressed DNG support is an explicit RawNode requirement, not merely an optional evaluation criterion. The decoder boundary should allow a fallback or alternate decoder when LibRaw cannot open a supported DNG.

## Phase 10 — Masks and local adjustments

After the base architecture is mature, investigate:

- node masks;
- opacity;
- brush/gradient/key masks;
- shared/group masks;
- local adjustments;
- AI-generated masks.

## Known technical debt

- Full-resolution export currently runs on a detached thread that can race with node mutation and processor resize/lifetime changes. This predates the generic Processor refactor and should be fixed separately by snapshotting or synchronising the render graph.

## Working rule

Do not start multiple major architectural changes simultaneously. Prefer proving one seam at a time so regressions are attributable and changes remain reversible.
