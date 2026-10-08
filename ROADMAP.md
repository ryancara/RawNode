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

The approved eight-step architecture migration is in progress. Steps 1-4 are
complete. Step 4 (renderer ownership encapsulation) landed in PR #45 after
independent review, sanitizer/stress validation and routine macOS validation.

The full post-Step-4 stabilisation gate is complete:

1. OpenFX host multithread lifetime correction (#48);
2. behaviour-preserving readability/source organisation (#50), independently
   reviewed and validated on macOS;
3. the whole-architecture checkpoint against RawNode's product goals and the
   pinned vkdt reference.

Codex and Claude independently found the direction sound for the next stage;
no further broad refactor is justified before Step 5. Immediate follow-ups #53
(source-only sidecars) and #54 (mutation-completion ordering regression) are
complete. Remaining focused findings are recorded in
`docs/ARCHITECTURE_AUDIT.md` and do not block Step 5.

- **Step 5 — current:** settle the first mask/graph product contract through
  decision work before implementation; see `docs/PROJECT_CONTEXT.md`.
- Step 6: transition from today's serial chain to explicit topology and a
  sequential DAG evaluator together.
- Step 7: versioned graph persistence.
- Step 8: first concrete mask path and remaining implementation details under
  the Step 5 product/architectural contract.

The completed stabilisation tasks are not extra migration steps.

## Completed editing workflow milestone

RawNode now supports:

- copying and pasting a single processor node through a versioned system-clipboard payload;
- copying and pasting the complete serial processing chain between images, including RAW working and Output colour space/gamma;
- portable `.rawnodepreset` files for both single nodes and full grades.

All three features reuse the same `PersistNode` / `PersistChain` representation as Sidecar V2. Full-grade transfers and presets additionally carry the colour-pipeline settings required to reproduce the grade.

A follow-up editing workflow should add filmstrip multi-selection and allow a copied full grade or full-grade preset to be applied to all selected photos in one action.

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
- generic node behaviour and Sidecar V2 persistence.

CLF should be treated as a richer transform format rather than assumed to be only a LUT, because it may contain matrices, ranges, LUTs, and other operations.

## Pick / reject workflow

Add a simple photo-culling workflow:

- Pick / Reject states;
- filmstrip filtering;
- efficient navigation between images;
- delete or move rejected photos through an explicit user action.

## Batch / workspace export

Allow exporting multiple selected/workspace images using the same processing and export infrastructure.

Full-resolution export execution is now owned and serialized against live
processor use. Future batch/workspace export should reuse that ownership model
rather than reintroducing detached access, processor cloning or a separate
scheduler without a concrete product need.

## Graph / List interface

Allow switching between graph and list views of the same processing structure.

Initially both should represent the existing serial chain.

Add number-key node selection.

Enable branching only when a concrete use case justifies it.

## Assignable input system

Add configurable keyboard/mouse parameter bindings.

Keep the input abstraction generic so future MIDI, OSC / TouchOSC, jog wheels, or other hardware controllers can use the same path.

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
