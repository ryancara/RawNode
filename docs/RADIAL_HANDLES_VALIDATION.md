# Radial handles: mapping and interaction validation

Draft PR #105 builds on merged #106/#107. Handles consume the evaluated Image's
Mask placements; no UI reconstruction from processor parameters or Reference
traversal is used. Source, Mask, Crop and Strength pixel processing are unchanged.

## Visible behaviour

- Photo handles follow known, invertible Image mappings through native, CTL and
  OFX processing. All matching consumers must agree exactly; Unknown, singular,
  different or absent placements hide handles with a reason in the Preview.
- Ellipse outlines and radius/feather grips follow full affine geometry, including
  reflection, rotation and shear. Pointer displacement is inverse-mapped; handle
  hit targets and leader separation retain fixed screen-pixel sizes.
- Existing pan/zoom, collapsed-radius recovery, inward feather, Undo/Redo, Escape
  and double-click interaction are retained. A completed/interrupted drag is one
  Undo step, including both centre coordinates.
- Pending or mismatched texture/result generations cannot supply new interactive
  controls. Own live edits may continue against the paired displayed mapping;
  external request changes end the gesture, and changed mappings cannot reuse
  its old baseline.
- Gesture ownership uses the request-only `previewDemand()` counter. The
  execution epoch exposed by `previewGeneration()` also advances on worker
  dequeue; it continues to govern cancellation, publication and texture pairing.
- Mask Only edits its own raster. Identity Overlay handles remain available;
  transformed Overlay alignment is deferred, with a message to use Photo or Mask
  Only. Overlay coverage is not resampled or changed.

## Automated evidence

The focused `--selftest-preview-interaction` includes the original interaction,
collapsed-radius, history, interruption, persistence and image-oracle coverage.
Additional tests exercise independent affine cardinal points and ellipse equations,
inverse centre/radius drags, reflected controls and feather screen projection;
all matching/equal/different/Unknown/singular consumer records; actual runtime
publication/cancellation barriers and texture-generation mismatch; resolution and
pan/zoom; real non-centred and sequential Crop mappings, Crop-before-Mask with an
explicit Reference, same-sized CTL and default OFX Crop at partial Strength.

Real-worker drag regressions upload worker presentations between UI frames in
the fast case and hold publication across six pointer movements in the delayed
case. They check every movement, one Undo step, both-coordinate Undo/Redo, Escape
with pending work, and an independent request that interrupts an unchanged mapping.
Condition-variable barriers control execution without arbitrary sleeps; history
drains have an independent gate-release path so a regression fails instead of hanging.

`--selftest-ofx-coordinates` retains #107's independent pixel/mapping oracles.
Full self-tests and focused graph/runtime tests cover the unchanged evaluator,
Mask application, persistence and publication contract.

## Recommended native macOS GUI checks

1. Select a Radial Mask on an Exposure at +1 EV, then apply Crop 50%, X=50%,
   Y=-100%. In Photo mode, confirm the controls follow the visible adjustment;
   drag centre, each radius and feather. Repeat with two downstream Crops.
   Keep the mouse held through multiple Preview updates: every pointer movement
   must be accepted and the whole continuous movement must remain one Undo step.
2. Put Crop before the masked Exposure and explicitly connect Crop to the Mask's
   Reference. Confirm saved centre/radii do not change when wiring/reordering;
   controls use the consuming Image frame. Check Mask Only's own aspect/raster.
3. Repeat through same-sized CTL and default Crop at Strength 0.5. Confirm
   geometry remains available without backend restrictions.
4. At fit, high zoom, tiny zoom and off-canvas centres, verify fixed handle sizes,
   middle/Space panning, wheel/pinch anchoring and background double-click reset.
   Collapse each/both radii, recover them, and test feather at 0/1 and zero radii.
5. Confirm one Undo step per drag, both centre coordinates restored together,
   no-op/return-to-start behaviour, Escape retaining accepted edits, and Undo/Redo.
6. During rapid edits/resolution changes, confirm controls wait for matching
   pixels and metadata. Change selection, photo (including same-path reopen),
   grade, Preview mode and export ownership during a drag; no stale gesture may
   modify a replacement document.
7. Check the placement explanation for unused/disabled Masks, unknown geometry
   and differing shared placements. In transformed Overlay, verify the deferral
   message; Photo/Mask Only remain available and Overlay pixels stay unchanged.

Affine and shared-placement edge cases also use explicit synthetic publications,
keeping their geometry expectations independent of backend rendering. Crop and
CTL integration cases use actual evaluator results. Native third-party spatial
effects depend on those plugins declaring the private mapping suite; opaque
same-sized effects retain the approved Identity compatibility default.
Outward feathering and the pre-existing Crop black-edge issue remain separate work.
