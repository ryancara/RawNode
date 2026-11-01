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
an invalid declared region is not silently replaced by Source. Positive finite
regions, including subpixel regions, are enclosed by integer pixel bounds:
`floor(x1), floor(y1), ceil(x2), ceil(y2)`. Coordinates must fit signed integers;
allocated extents retain the existing 32768-per-axis limit on declared regions.
Empty, reversed, non-finite, unrepresentable or oversized regions fail evaluation.

`OfxOutputRegion` carries those two rectangles together. The actual raster size
is the integer bounds' width/height. The adapter supplies:

- that integer rectangle as the Render window and fetched Output image bounds;
- its integer enclosure as the fetched image's pixel RoD;
- the original double rectangle for Output `clipGetRegionOfDefinition`;
- a data pointer to the lower-left **allocated** pixel, at `(bounds.x1,bounds.y1)`;
- packed positive row bytes `(bounds.x2-bounds.x1)*4*sizeof(float)`.

Plugins address a CPU pixel relative to the lower bounds, not coordinate zero.
The resulting graph Image receives only the packed samples and dimensions.
Canonical RoD/bounds alone never declare an input-to-output geometric mapping.

## Optional RawNode Render suite

`RawNodeOfxCoordinateMappingSuite` version 1, declared in
`src/ofx/OfxCoordinateMapping.h`, is a **RawNode-specific extension, not an OpenFX
standard suite**. It has one function:

`setInputToOutput(OfxImageEffectHandle effect, const double *affine)`

A cooperating plugin fetches the optional suite through the existing OFX host
suite mechanism and calls it once from the Render calling thread, using the
same geometry used to sample that render's pixels. Render workers and parameter
notification actions must not call it. The six doubles are:

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

It continues to distribute output pixel centres across the full continuous
window, including when integer enclosure changes the allocated size. Therefore
window extents, not rounded output dimensions or a host copy of Crop's parameter
maths, determine its normalized mapping. Its existing aspect, percentage,
centering, offset, bilinear interpolation and out-of-source black rules remain.
The fitted source edge stays exact to avoid a floating-point ratio round-trip
adding a spurious pixel to an identity crop. Canonical Source origins are added
when reporting the Crop RoD and addressing source pixels.

Independent tests use source R/G channels containing pixel-centre X/Y and a B
channel containing their product. Bilinear interpolation reproduces these fields.
Literal expected sample tables and exact original pixel indices establish which
locations Crop actually samples; forward/inverse mappings are then checked
against those rendered values. Real sequential OFX crops are also evaluated
through the graph, including consumer-local Mask placement composition.

## Compatibility and boundaries

The old host discarded RoD origins and rounded only its width/height. Correct
outward enclosure can add edge pixels and change fractional Crop dimensions and
sample positions. For example, `[2.5,2.25,6.5,5.25]` now allocates 5x4 instead of
4x3, while still sampling the same continuous window. Valid subpixel windows no
longer fall back to full-size Source; `[3.92,2.94,4.08,3.06]` allocates 2x2.
Default integer zero-origin rendering stays unchanged. Plugins that relied on
the host's former incorrect zero-origin Output coordinates may behave differently;
plugins following OFX bounds/stride conventions now receive the declared frame.

This is still a whole-frame, fixed-scale/PAR host path, not a general tiled/ROI,
non-unit-PAR or temporal host implementation. CPU behaviour is automatically
covered; native macOS/Metal and real external plugin validation remain platform
checks. No handle drawing/dragging, Overlay alignment, Mask pixels, Strength,
Reference wiring, sidecars or outward feathering changes are included.
