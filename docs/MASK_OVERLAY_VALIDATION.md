# Coordinate-aware Mask Overlay validation

The accepted scope is presentation through the existing published
`CoordinateMapping`/`MaskPlacement` architecture. No processor-specific Preview
logic, Reference traversal, persistent transforms, scheduler or GPU pass is added.

## Acceptance and independent expectations

- One UI-independent resolver beside `EvaluationResult` supplies handles and
  Overlay. It distinguishes Identity, invertible Affine, Unknown/singular,
  Ambiguous and Absent. All matching uses participate; exact duplicates agree;
  an invalid use prevents known placement regardless of order.
- Identity/equal dimensions retain exact legacy RGBA8 bytes. Known unequal
  dimensions work. Absent/unsupported placement preserves the valid photo,
  hides tint and explains why; Mask Only still inspects the Mask's own raster.
- Inverse pixel-centre bilinear sampling uses zero outside `[0,1]` and zero
  missing taps, bounded finite coverage and unchanged red tint/alpha/orientation.
- Published Image/Mask/mapping identity, display refresh, request demand,
  execution epochs and gesture-owned edits retain their existing owners.

`--selftest-mask-overlay` contains literal coverage tables for translation,
non-uniform scale, rotation, shear, reflection and unequal raster sizes. For a
black photo the independently expected output is red `round(102*coverage)`,
zero green/blue and alpha 96. A 4x3 soft plane translated half a Mask pixel has
bottom-row coverage `[0,.125,.375,.625]`; this catches half-pixel errors and
direct indexing. A 2x2 Mask sampled into 4x4 has top-right coverage `.5625`,
not 1: missing taps are empty, not clamped. Additional cases exercise malformed
dimensions/storage, non-finite coordinates/samples, singular/inverse overflow,
multiple equal/conflicting uses and unrelated Mask output ports.

Real bundled Crop cases use a test Mask plane `(x+3*y)/32` in an 8x6 Reference.
The single Crop window `[3,0,7,3]` gives first-row coverage `[3,4,5,6]/32` in
4x3. Two Crops select original centres X `[3.5,4.5]`, Y `[1.125,1.875]`, giving
`[4.875,5.875;7.125,8.125]/32` in 2x2. The unequal axis weights also distinguish
reversed composition (an earlier `x+2*y` plane accidentally cancelled that error).
A full-size Crop panned 100% in X remains
8x6 but selects original columns 4–7 then empty coverage; equal dimensions
cannot pass as Identity. These expectations come from literal sample locations,
not calls to the mapping or sampler under test.

Preview tests compare a real cropped Radial's tint against the documented radial
equation at original pixel centres while its handles forward-map that same
publication. Real-worker Overlay drags run six movements with publication both
between frames and held pending, including actual Crop/differing Mask and Image
dimensions, release/Escape, one Undo step, both-coordinate Undo/Redo and external
demand interruption. Runtime tests cover transformed colour-only refresh without
processor execution, unknown/singular explanations and supersession of both
coverage and geometry before publication. Existing suites retain Photo, Mask Only,
error/inactive clearing, texture/result pairing and export/persistence coverage.

## User-visible changes

- Mask Overlay follows known downstream transforms, including OFX Crop, even
  when the Mask and photograph dimensions differ (requested alignment feature).
- Transformed Overlay now permits Radial handles under the existing freshness
  and interaction guards (requested handle integration).
- Unknown/singular, conflicting and unapplied Masks show the photograph without
  tint and an existing-status-line explanation (required safe alignment policy).
  Matching Reference dimensions no longer produce an invented placement.
- Tint strength, colour management, alpha, Photo and Mask Only remain as before.

## Native macOS checklist (pending manual validation)

1. Apply Radial Mask to Exposure +1 EV, followed by real OFX Crop 50%, X=50%,
   Y=-100%. Compare the photo adjustment, Overlay and centre/radius/feather grips.
   Repeat with a second downstream Crop 50%, X=-100%, Y=0%.
2. Use full-size Crop with X=100%: confirm same-size non-Identity alignment and
   empty coverage outside the Mask. Try fractional pan and soft feathering.
3. Keep a centre/radius/feather drag held through several Preview updates in
   Overlay. Try slow rendering. Each movement stays responsive; tint may lag
   until publication. Release and Escape each retain one Undo step; Undo/Redo
   restore both centre coordinates and the complete drag.
4. Switch Photo → Overlay → Mask Only and back, including during pending renders.
   Change selected Masks and Preview resolution; no new metadata may drive an
   old texture. Pan/zoom/pinch and fixed-size grips retain their normal behaviour.
5. Change display/monitor interpretation: tint stays aligned without processor
   rerendering. Disable/bypass/delete a Mask or its consumer and test unavailable
   coverage: no stale tint; valid photo and appropriate diagnostics remain.
6. Undo/Redo Crop transforms and topology edits; reconnect Reference to a prior
   Crop so Mask generation differs from Source. Placement still begins at the
   coverage consumer, and saved Radial geometry does not change automatically.
7. Inspect an unused Mask, unknown geometry and conflicting consumer placements:
   tint/handles are unavailable with an explanation; Mask Only remains usable.
8. Check photo/grade replacement, export and return to Preview. No stale gesture,
   tint or publication; exports contain the ordinary processed Image only.

Opaque equal-size plugins retain D056's compatibility Identity assumption;
accurate spatial correspondence depends on explicit declarations. Outward
feathering, Crop's pre-existing pixel/black-edge behaviour and native GUI checks
are outside this implementation's automated guarantees.
