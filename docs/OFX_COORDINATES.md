# OFX regions and coordinate declarations

RawNode keeps graph `Image`/`Mask` values raster-local (D039/D056). OFX canonical
coordinates belong inside the adapter; they do not become graph canvas offsets
or a global Source coordinate system.

## Regions, storage and rendering

The host currently renders whole frames at time 0, pixel aspect ratio 1 and
render scale `(1,1)`. The current evaluated input is the Source clip's local
`[0,W) x [0,H)` frame, including when it came from another OFX node with an offset
output region. Its pixel buffer starts at `(0,0)`.

`queryOutputRegion` retains the plugin's double-precision canonical output RoD.
A default/unsupported RoD action uses Source. Other action failures are reported;
an invalid declared region is not silently replaced by Source. The exact OFX
`kOfxFlagInfiniteMin` lower edges and `kOfxFlagInfiniteMax` upper edges resolve
individually against the available finite Source frame (`0,0,W,H`) before
validation. Finite edges retain their coordinates; near-sentinel values and IEEE
infinities get no fallback. A resolved empty/reversed region still fails.
This bounds an unbounded still-image effect to available Source, without claiming
support for an infinite canvas. Positive finite
regions, including subpixel regions, are enclosed by integer pixel bounds:
`floor(x1), floor(y1), ceil(x2), ceil(y2)`. Coordinates must fit signed integers;
allocated extents retain the existing 32768-per-axis limit on declared regions.
Empty, reversed, non-finite, unrepresentable or oversized regions fail evaluation.

`OfxOutputRegion` carries those two rectangles together. The actual raster size
is the integer bounds' width/height. The adapter supplies:

- that integer rectangle as the Render window and fetched Output image bounds;
- its integer enclosure as the fetched image's pixel RoD;
- the double rectangle (after resolving infinite edges) for Output `clipGetRegionOfDefinition`;
- a data pointer to the lower-left **allocated** pixel, at `(bounds.x1,bounds.y1)`;
- packed positive row bytes `(bounds.x2-bounds.x1)*4*sizeof(float)`.

Plugins address a CPU pixel relative to the lower bounds, not coordinate zero.
The resulting graph Image receives only the packed samples and dimensions.
Canonical RoD/bounds alone never declare an input-to-output geometric mapping.

## Optional RawNode Render suite

`RawNodeOfxCoordinateMappingSuite` version 1, declared in
`include/rawnode/OfxCoordinateMapping.h`, is a **RawNode-specific extension, not an OpenFX
standard suite**. It has one function:

`setInputToOutput(OfxImageEffectHandle effect, const double *affine)`

A cooperating plugin fetches the optional suite through the existing OFX host
suite mechanism and calls it once from the Render calling thread, using the
same geometry used to sample that render's pixels. Render workers and parameter
notification actions must not call it. This thread rule is a caller requirement:
the host checks active Render lifetime only, not thread affinity. Concurrent or
worker-thread declarations are unsupported and may race; no thread enforcement
or synchronization subsystem is provided by this private extension.
The six doubles are:

`xx, xy, tx, yx, yy, ty`

They mean `x'=xx*x+xy*y+tx`, `y'=yx*x+yy*y+ty`, using normalized lower-left raster
edge coordinates for that render's input and output buffers. Pixel centres are
`((i+.5)/W,(j+.5)/H)`. A declaration must describe actual sampling geometry,
including its relationship to allocated output dimensions, rather than assume
that RoD proves pixel correspondence.

- No call: undeclared. The shared same-dimension Identity / changed-size Unknown
  compatibility default still applies.
- Six finite doubles: explicit affine; exact Identity is canonicalized.
- Null pointer or non-finite coefficients: explicit Unknown, which overrides the
  same-dimension default without failing an otherwise valid photograph render.

The host lends a render-local optional declaration slot for this action only,
copies its value on successful return, and removes access on every action exit,
including failure/exception paths. A call outside active Render returns
`kOfxStatErrBadHandle`. The snapshot is returned through
`ProcessorResult::inputToOutput`. No new action, effect property mutation, later
UI query, parameter query or geometry cache is involved. Live parameter
notifications do not read or mutate this render-local slot.

Third-party plugins need not implement this extension. They retain the approved
default, including same-sized opaque effects that move pixels without declaring
it. On another host the bundled Crop simply skips reporting if the optional
suite is absent. The declaration is not part of graph persistence.

## Bundled Crop

Crop computes its continuous sampling window once inside Render and reports the
mapping from those exact values. For input width/height `W,H` and a window with
raster-local lower edge `x0,y0` and extent `w,h`, it declares:

`x'=(W*x-x0)/w`, `y'=(H*y-y0)/h`.

It retains the merged-main continuous-window calculation and distributes output
pixel centres across that window using the existing bilinear/black-outside-Source
sampling. Aspect, percentage, centering and offsets are unchanged; no floating-point
snapping correction is applied.

Only the optional declaration canonicalizes numerical full-frame windows to exact
Identity: output and Source raster dimensions must match, and all four sampling
window edges must match the Source frame within `1e-12` relative per axis. This is
less than `3.3e-8` pixel at the host's maximum axis size. Windows outside those
edge tolerances retain Affine declarations, including changes too small to alter
rounded dimensions. No shared approximate-equality or evaluator mixing rule
changes. Legacy default
Crop black-edge sampling remains a separately deferred pixel-rendering issue.

The output RoD is a separate, pixel-aligned region: its lower edges are the floors
of the Source origin plus the continuous window's lower edges, and its extents
are the legacy rounded window edge differences, including the same floating-point
subtraction at half-integer thresholds. If either edge difference is below one,
both output extents retain the legacy Source-sized fallback. At RawNode's unit
scale/PAR this region's integer enclosure is exactly the intended raster size;
fractional origin/enclosure noise cannot add a row or column. This policy belongs
in Crop, with no Crop-specific handling in the host.

The continuous sampling window, reported RoD, allocated raster dimensions and
normalized mapping therefore have distinct roles. Coordinate correspondence does
not require the output dimensions to equal the continuous window's extents.
Canonical Source origins are included when reporting the Crop RoD and addressing
source pixels. RawNode always supplies a zero-origin Source; nonzero Source
origins cannot be exercised through its graph, so another host's Source-origin
behaviour remains unverified. Partial Source fetches, tiling and non-unit scale/PAR
are not established by these tests.

Independent tests use source R/G channels containing pixel-centre X/Y and a B
channel containing their product. Bilinear interpolation reproduces these fields.
Literal expected sample tables and exact original pixel indices establish which
locations Crop actually samples; forward/inverse mappings are then checked
against those rendered values. Real sequential OFX crops are also evaluated
through the graph, including a visibly non-zero masked +1 EV Exposure before two
Crops. A hard ellipse with explicitly enumerated covered source pixels and a
literal interpolated RGBA table independently checks the rendered effect;
independent sample locations check composed Mask placement.

## Compatibility and boundaries

The host now preserves Output origins and encloses finite declared regions.
Bundled Crop retains legacy dimensions and samples by reporting its own aligned
output region. In particular, 120x80 at Original/95% remains 6x4 with exact-copy
samples; 6000x4000 at 18%/41% remains 4920x3280/3540x2360. Continuous window
`[2.5,2.25,6.5,5.25]` retains 4x3, with X samples `[3,4,5,6]` and Y samples
`[2.75,3.75,4.75]`. The tiny `[3.92,2.94,4.08,3.06]` window retains the old 8x6
raster while sampling only that window. General OFX subpixel RoDs still use
outward enclosure (for example `[.2,-.8,.3,-.7]` allocates 1x1).

Default integer zero-origin rendering stays unchanged. Plugins that relied on
the host's former incorrect zero-origin Output coordinates may behave differently;
plugins following OFX bounds/stride conventions now receive the declared frame.
Invalid finite or over-limit regions now fail instead of using the old generic
Source fallback; preserving that fallback would hide malformed declarations or
violate the allocation contract. Standard infinite declarations use the narrow
edge-resolution rule above. No external third-party plugin compatibility is
claimed solely from synthetic tests.

This is still a whole-frame, fixed-scale/PAR host path, not a general tiled/ROI,
non-unit-PAR or temporal host implementation. CPU behaviour is automatically
covered; native macOS/Metal and real external plugin validation remain platform
checks. No handle drawing/dragging, Overlay alignment, Mask pixels, Strength,
Reference wiring, sidecars or outward feathering changes are included.
