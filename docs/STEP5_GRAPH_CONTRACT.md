# Step 5 Graph / Mask Product Contract

**Status:** In progress.  
**Purpose:** Working design record for Step 5 of the RawNode architecture migration.  
**Base reviewed:** `816d44f350a626d27321ad32c9232adc75e32afd`

This document records the current product and architecture contract being settled before Step 6 implements explicit graph topology and sequential DAG evaluation.

It is intentionally a working document. Items marked **Accepted** are the current project direction. Items marked **Strong candidate** still need one final implementation-oriented pass before becoming durable decisions. Items marked **Open** must not be silently decided by Step 6 implementation. Once Step 5 closes, durable decisions should be distilled into `docs/DECISIONS.md` and `docs/ARCHITECTURE.md`.

## Progress

- [x] Ryan + ChatGPT initial graph / mask model
- [x] Independent Claude architecture review
- [x] Independent Codex architecture review
- [x] Claude / Codex reconciliation
- [x] Clarified qualifier as a standalone Mask-producing node
- [x] Clarified Mask as scalar coverage rather than grayscale RGB
- [x] Clarified no automatic intermediate colour-space propagation
- [x] Accepted raster/reference spatial model as current Step 5 direction
- [x] Independent narrow spatial-model challenge by Codex and Claude
- [x] Reconciled spatial review: no per-value SpatialDomain; small evaluator-contract extensions accepted
- [ ] Settle remaining Step 5 implementation contracts
- [ ] Review this document against current source
- [ ] Close Step 5 in durable architecture / decision docs
- [ ] Begin Step 6 implementation

## Product model

### Accepted: one authoritative graph

RawNode has one processing/document graph.

The Node View and List View are views of the same document. They must never become separate processing models.

The Node View owns explicit topology editing.

The List View may show, select and edit nodes, but list position must not become an independent render-order model once explicit topology exists.

### Canonical local-adjustment example

```text
Source ───────────────→ Exposure ─────────────→ Saturation ─→ Output
  │                         ↑                       ↑
  │                         │ Mask                  │ Mask
  ├────→ Qualifier ─────────┘                       │
  │                                                 │
  └────→ Radial Gradient ───────────────────────────┘
```

Meaning:

- Source feeds Exposure's Image input.
- Source also feeds a standalone Qualifier.
- Qualifier analyses RGB and outputs a Mask.
- Qualifier Mask feeds Exposure's Mask input.
- Exposure output feeds Saturation.
- Radial Gradient independently generates a Mask.
- Radial Mask feeds Saturation's Mask input.

The qualifier is not hidden inside Exposure. It is a reusable graph node.

## Graph value types

### Accepted: Image

An Image is floating-point RGBA raster data with width and height.

The first graph does **not** automatically attach source-coordinate provenance, a canvas offset, or a propagated spatial transform to every intermediate Image.

The initial graph does **not** automatically propagate a colour-space / transfer-function tag with every intermediate Image.

RawNode retains the current philosophy:

```text
document input interpretation
        ↓
numeric floating-point RGB graph
        ↓
operations that need colour knowledge declare it explicitly
        ↓
document output interpretation
```

Examples of explicitly colour-aware operations:

- CST declares input/output gamut and transfer function.
- Qualifier declares the gamut / transfer function used for analysis.
- Future colour-sensitive blend modes must explicitly define the colour model/domain their maths uses.

The graph does not automatically infer, validate or convert branch colour encodings.

This decision avoids introducing a large intermediate colour-metadata system merely to support branching. Arbitrary CTL / DCTL / OFX / LUT operations cannot in general have their output colour interpretation inferred automatically.

### Accepted: Mask

A Mask is a single-channel scalar coverage field.

```text
0.0 = no effect
0.5 = 50% coverage
1.0 = full effect
```

Mask values are clamped to the closed range `[0,1]`.

A Mask has:

- no RGB primaries;
- no colour space;
- no transfer function / gamma.

Any gamma used to display a mask in the viewer is presentation-only.

Operations that derive a Mask from an Image may be colour-aware. Their Mask output is still colour-independent scalar coverage.

Mask remains distinct from image alpha.

## Node categories

### Accepted: ordinary Image processor

Conceptually:

```text
Image ─────→ [ processor ] ─────→ Image
Mask  ─────→ [ optional generic Mask input ]
```

The underlying native / CTL / DCTL / LUT / OFX processor remains responsible for its image operation.

Mask, node strength/opacity and blend behaviour belong to generic graph/node evaluation rather than being reimplemented independently by every processor backend.

### Accepted product direction: generic controls are not whitelisted by processor type

RawNode should not maintain a hard-coded list saying Exposure may be masked while CST may not.

Image→Image graph nodes may expose generic:

- Mask input;
- Opacity / Strength;
- Blend Mode.

Unusual combinations are user-controlled.

Where the operation cannot be evaluated numerically because the input and processed output do not share a compatible spatial grid, the limitation should come from the generic spatial/value contract rather than a processor-name whitelist.

For example, a Crop that changes dimensions cannot be pixelwise mixed with its uncropped input unless an explicit Resize / Reformat operation produces compatible rasters.

### Accepted: standalone pixel-aware Mask generator

```text
Image → Qualifier → Mask
```

The first Qualifier implementation may be native.

The Qualifier itself owns any colour-aware analysis controls, for example:

```text
Analysis Colour Space
Analysis Gamma / Transfer Function
Hue / Saturation / Luminance ranges
Softness
Blur
Cleanup
Invert
```

Nothing automatic needs to happen elsewhere in the graph to tell the Qualifier what colour space its input "really" is.

### Accepted: geometric Mask generator

Examples:

```text
Radial Gradient → Mask
Linear Gradient → Mask
Rectangle       → Mask
future Pen/Path → Mask
```

Geometric generators do not require RGB samples merely to generate their shape.

They use an optional **Reference Image** input to determine the raster frame in which the Mask is generated. If no Reference is connected, the document Source is the default reference.

Conceptually:

```text
Reference Image → Radial Gradient → Mask
```

The Reference is dimensional/spatial context, not a colour-sampling input.

### Accepted: Mask processor / combiner

Examples:

```text
Mask → Invert → Mask

Mask A ─┐
        ├→ Combine → Mask
Mask B ─┘
```

Add / Intersect / Subtract are desired product concepts.

The exact fractional maths of those operators is still **Open** and must not be implied by their names.

Common refinement that is naturally part of mask creation may remain inside a generator:

- Qualifier softness / blur / cleanup / invert;
- Radial feather / invert;
- brush hardness / opacity;
- path feather.

Explicit Mask→Mask processors remain available for shared or advanced refinement.

### Accepted: mixers are graph operations

Future graph operations include at least:

- Parallel Mixer;
- Layer Mixer.

Exact arithmetic is still open.

## Generic Image-node strength / opacity

### Accepted product behaviour

The UI should provide a convenient node-level strength control without requiring a constant-gray Mask node.

A useful UI may label this **Opacity**.

Internally / persistently, a distinct name such as `strength` or `mix` is preferred so it is not confused with Layer Mixer opacity.

Candidate ordinary-node evaluation:

```text
I = incoming Image
P = Processor(I)

B = Blend(
    base   = I,
    effect = P,
    mode   = node.blendMode
)

coverage = maskOrWhite × node.strength

output = interpolate(I, B, coverage)
```

No connected Mask means full coverage.

Mask and node strength multiply.

At zero coverage the evaluator should return the input exactly. At full coverage with Normal blend it should return the processor output exactly.

Strength is an output mix, not interpolation of processor parameters.

Example:

```text
+1 EV Exposure at 50% strength
```

is a 50% interpolation between input and the +1 EV result. It is not defined as +0.5 EV.

### Accepted: generic self-mixing requires compatible raster dimensions

The generic wrapper is governed by raster compatibility rather than a processor whitelist.

If a node is evaluated as full-strength Normal processing with no Mask, its processor result may change dimensions.

If the node requires pixelwise self-mixing because:

- a Mask is connected;
- Strength is less than 1; or
- Blend Mode is non-Normal;

then the processor output width and height must match the node's Image input width and height.

If the dimensions do not match, the requested evaluation fails. Export refuses rather than silently omitting the edit. RawNode does not implicitly resize, resample, align or reproject the result.

This is intentionally generic. Crop, OFX, native and future processor types are governed by the same rule.

## Blend modes

### Accepted: blend mode is a generic Image-node property

Node-level blend mode uses:

```text
base   = that node's own Image input
effect = that node's processed output
```

Layer Mixer compositing is a separate use of blend mathematics because its backdrop is the accumulated mixer result.

The two concepts must remain distinct even if they eventually share the same blend-function library.

### Accepted: Normal first

Step 6 does not need to solve non-Normal blend maths.

The first executable mode is:

```text
Normal
```

Non-Normal modes must be specified individually before they ship.

Do not blindly copy bounded Photoshop / PDF / W3C equations into RawNode's floating-point scene-referred workflow.

Desired future workflow examples remain useful acceptance tests:

- Exposure with Color blend should ideally contribute no meaningful colour change.
- RGB contrast curve with Luminosity blend should retain the desired tonal contribution while suppressing the unwanted chroma/saturation contribution.

These are workflow goals, not yet mathematical specifications.

### Open: non-Normal maths

Still to settle later:

- Luminosity;
- Color;
- Hue;
- Saturation;
- arithmetic/photo modes such as Multiply / Screen / Overlay;
- working domain;
- behaviour for negative RGB and RGB > 1;
- singular / zero-luminance cases;
- versioning of blend-mode semantics.

## Missing / unavailable effects

### Accepted product direction

Unavailable processors/effects remain present in the document and sidecar.

Their identity, parameters and graph connections must be preserved so reinstalling the missing dependency can restore the edit.

Missing state is not the same as user bypass state.

RawNode must show a persistent warning.

### Accepted safety behaviour

A missing Mask dependency must never silently become white, because that could widen a local edit into a global edit.

If a connected Mask dependency is unavailable, the dependent masked effect should fail closed / contribute no effect rather than become global.

The graph and connection remain intact in the sidecar.

A missing ordinary image effect may be bypassed for evaluation while remaining present and warned about.

Export may continue with an explicit warning that unavailable effects were omitted. This is intentionally closer to RawNode's current missing-plugin philosophy than treating every unavailable dependency as a hard export failure.

The exact warning / UI presentation can be implemented later.

### Accepted: unavailable dependency is different from an invalid evaluation

RawNode distinguishes recoverable unavailability from a graph/value incompatibility.

If an unavailable node exists anywhere in a Mask's dependency closure, including a connected Reference path:

- preserve the node, parameters and connections;
- mark the Mask unavailable;
- make the dependent masked effect fail closed / contribute no effect;
- show a persistent warning.

An absent Reference is not unavailable. It resolves to the document Source for the current evaluation.

A user bypass remains a normal pass-through according to the bypass rules. It must not be treated as a missing dependency.

By contrast, if all required values evaluate successfully but their raster dimensions are incompatible for a pixelwise operation, that requested evaluation is invalid:

```text
valid Image + valid Mask + mismatched dimensions
    → evaluation error

export evaluation error
    → export refuses
```

Dimension mismatch is therefore not silently converted into fail-closed behaviour.

## Bypass

### Accepted

Unary Image processor bypass:

```text
pass Image input through unchanged
```

Unary Mask processor bypass:

```text
pass Mask input through unchanged
```

Mixer bypass should forward its defined base / background input.

### Open: Mask-generator bypass

There is no universal scalar output that is neutral for every possible consumer.

White is neutral for an ordinary adjustment Mask input but is not neutral for every Add / Intersect / Subtract / Invert graph.

Do not hard-code `generator bypass = white` as a durable graph rule.

Possible product behaviours include:

- no generic bypass for source-like Mask generators;
- disabling/disconnecting a particular Mask use;
- an explicit constant-output override.

Settle this separately from missing/unavailable behaviour.

## Repeated ports / mixer-ready topology

### Strong candidate for Step 6

Step 6 should support a richer port model immediately rather than implementing only fixed one-off named inputs and redesigning it later for mixers.

Required concepts:

- stable node identities;
- typed Image / Mask endpoints;
- named input/output ports;
- zero-input generators;
- one source per scalar input;
- fan-out;
- joins;
- cycle rejection;
- required / optional inputs;
- stable ordered repeated input groups / slots;
- named outputs;
- one authoritative document Image output.

A repeated slot may conceptually contain:

```text
stableSlotId
image : Image
mask  : Mask?
slot parameters
```

This does not require implementing Layer Mixer in Step 6.

It ensures the graph can represent it later without redesigning the port model.

### Candidate Layer Mixer shape

```text
background : Image

layers : ordered repeated group
    stableSlotId
    image      : Image
    mask       : Mask?
    opacity
    blendMode
```

Layer blend / opacity belongs to the mixer slot because the same producer Image may feed more than one mixer with different compositing settings.

### Candidate Parallel Mixer shape

```text
base : Image

branches : ordered/repeated Image inputs
```

The explicit Base prevents the mixer from having to infer the common ancestor from topology.

Exact Parallel Mixer arithmetic remains open.

## Spatial model

### Accepted: spatial review outcome

The independent Codex and Claude challenge reviews both support the minimal raster/reference model.

Codex concluded the minimal model is sufficient.

Claude concluded it needs a **small evaluator-contract extension, not richer Image/Mask values**, and explicitly retracted the earlier recommendation for a propagated per-value spatial domain.

RawNode therefore does **not** introduce `SpatialDomain`, source-coordinate provenance, canvas offsets, automatic transform propagation or ROI state on graph values in Step 6.

This is deliberate. Arbitrary OFX / DCTL / CTL / LUT operations cannot in general provide a truthful point mapping back to the Source, and equal-size joins of different geometric histories are intentionally legal.

### Accepted: raster-local graph values

Conceptually:

```text
Image
    RGBA float raster
    width
    height

Mask
    scalar [0,1] raster
    width
    height
```

Pixelwise operations require matching raster dimensions.

Equal dimensions are numerically compatible. RawNode does not attempt to determine whether two equal-size buffers represent:

- the same original Source region;
- the same Crop;
- the same warp;
- the same geometric transform.

If the user connects them, corresponding raster coordinates interact.

Raster-coordinate pairing is the permanent default. Any future alignment or reprojection must be explicit / opt-in so old graphs retain their meaning.

### Accepted: no implicit spatial conversion

The first DAG performs no automatic:

- resize;
- resample;
- reprojection;
- branch alignment;
- canvas-offset reconciliation;
- source-coordinate remapping.

Different-size values require an explicit Resize / Reformat / Transform / Align operation where such behaviour is desired.

No ROI/tile scheduler or backward ROI negotiation is introduced in Step 6.

### Accepted: per-evaluation context, not per-value spatial metadata

Step 6 needs a small evaluation context that describes the current render purpose and Source scale.

Conceptually:

```text
EvaluationContext
    purpose
        preview | export | inspection
    fullSourceWidth
    fullSourceHeight
    evaluatedSourceWidth
    evaluatedSourceHeight
```

The exact C++ representation may differ.

This state is:

- per evaluation;
- not persisted;
- not propagated as metadata on every Image/Mask value.

Each node must receive and use the **actual dimensions of its evaluated inputs**. Step 6 must not continue broadcasting the document Source dimensions to every downstream node after a size-changing operation.

Preview and export are validated independently. A graph that happens to have compatible dimensions at preview resolution is not thereby certified compatible at full-resolution export.

A future cheap full-resolution dimension-only validation may warn earlier, but correctness does not depend on it.

### Accepted: requested-endpoint evaluation

The evaluator should support an explicit requested output endpoint:

```text
evaluate(
    graph,
    requestedEndpoint = { nodeId, outputPort },
    context
)
```

The endpoint may produce either Image or Mask.

The normal document render requests the authoritative document Image output.

Requested endpoint evaluation supports:

- Mask inspection;
- viewing the Image input/output relevant to mask editing;
- future node inspection;
- the same evaluator/runtime rather than a separate mask-rendering path.

### Accepted: geometric Mask generators use a Reference Image

Geometric and file-backed Mask generators may use an optional Image `reference` input.

```text
Reference Image → Radial / Gradient / Pen → Mask
```

The Reference supplies the raster **frame and dimensions**. It does not mean:

- track subjects/content in that Image;
- inherit a geometric transform;
- reproject the Mask through downstream effects.

Two References with equal evaluated dimensions are equivalent for rasterisation.

If Reference is absent:

```text
Reference = the document's primary Source
            as evaluated in this evaluation
```

Therefore:

- preview uses the preview Source raster;
- export uses the full-resolution Source raster.

An explicit Reference is a normal graph dependency. It participates in:

- reachability;
- dependency ordering;
- cycle rejection;
- shared-upstream evaluation;
- intermediate lifetime.

A connected but unavailable Reference does **not** silently fall back to Source. It makes that Mask unavailable, and the dependent masked effect follows the fail-closed unavailable rule.

A geometric generator has one Reference and one output raster per evaluation. Its output never changes frame according to whichever consumer asks for it.

### Accepted: Reference is frame-anchored, not content-anchored

Reference-normalised geometry is **frame-anchored**.

For example, changing a Crop's offset while preserving its output dimensions does not move an existing normalized Radial relative to that cropped output frame. The underlying photographic subject may move beneath the Mask.

Automatic content attachment through geometric transforms is a different future capability and is not implied by Reference.

### Accepted: resolution-independent geometry

Geometric parameters must not be stored in preview-pixel coordinates.

The high-level convention remains:

- positions are resolution-independent relative to the Reference frame;
- distances such as radius / feather use an isotropic, resolution-independent unit based on the Reference frame.

Before the first geometric generator is persisted, pin the exact convention for:

- origin and Y direction;
- edge- vs centre-based normalized coordinates;
- pixel-centre mapping;
- angles;
- Pen/path coordinate frame.

A strong candidate from the spatial review is:

```text
normalized frame edges: [0,1] × [0,1]
pixel centre:            ((x + 0.5) / width,
                          (y + 0.5) / height)
position:                normalized per axis
length/radius/feather:   fraction of shorter edge
angle/distance maths:    isotropic raster space
```

This exact convention may be finalized with the first generator implementation, but it must be fixed before such parameters are persisted.

Pixel-aware generators such as Qualifier inherit the dimensions of their Image input.

Mask→Mask processors preserve dimensions unless an explicit operation declares otherwise.

Mask combiners require equal dimensions.

### Accepted: dimension compatibility is evaluation-specific

Size-changing operations must use deterministic, resolution-independent parameters and a documented rounding rule.

A preview can otherwise accidentally produce equal branch dimensions while export produces different dimensions, or vice versa.

For native size-changing operations, use one shared size-only rounding convention rather than independently snapping unrelated edges where possible.

The durable correctness rule is still simple:

```text
evaluate actual inputs at this resolution
    ↓
pixelwise operation requires equal dimensions
    ↓
mismatch = requested evaluation error
```

### Accepted: Crop placement is user-directed

RawNode does not impose a global "Crop must be first" or "Crop must be last" rule.

The existing bundled OFX Crop is currently documented as being useful at the beginning of a chain so downstream plugins process fewer pixels.

A future native Crop may be placed wherever the user's graph requires.

Crop and other geometry-changing nodes remain governed by the same generic self-mixing compatibility rule as every other Image processor.

### Other-software lessons, corrected

RawNode borrows concepts selectively rather than copying another application's spatial architecture.

- **OpenFX:** canonical/pixel coordinates and render scale matter at the OFX adapter boundary. RoD / RoI / arbitrary bounds do not therefore become per-value RawNode graph state.
- **GIMP:** layer masks matching layer dimensions is useful precedent; canvas-relative offsets are not required for RawNode's first DAG.
- **Natron:** format/RoD policies are relevant to OFX adaptation and compositing, but general bounding-box composition is deferred.
- **darktable:** masks can remain attached to image content because darktable derives transformations through its ordered pixelpipe and per-module transform callbacks. It does not require transform metadata on every image buffer. Its Crop is not simply a mandatory late-pipeline operation.
- **vkdt:** its ROI data primarily carries logical/current dimensions for its rendering model; it does not provide automatic mask reprojection. RawNode does not need to copy its ROI negotiation.

### Accepted: richer spatial features remain additive

RawNode is intentionally deferring:

- automatic mask reprojection through transforms;
- automatic branch alignment;
- source-anchored transform-aware mask editing;
- general canvas/bounding-box compositing;
- ROI propagation / tiling.

If a future feature genuinely needs logical size, scale, origin or source mapping, that information may be added to Image/Mask values or exposed through explicit graph operations without changing:

- node identity;
- port identity;
- endpoint references;
- connection representation;
- evaluator topology.

Any later automatic alignment/reprojection must be opt-in so existing raster-coordinate graphs retain their meaning.

## External masks

### Accepted conceptual direction

Future workflow:

```text
Reference Image ─────────┐
                         ↓
External Mask File → explicit resample → Mask
```

External Mask File is a Mask-producing node with the same optional Reference concept as geometric generators.

If Reference is absent, it uses the document Source for the current evaluation.

The file-backed node itself is responsible for explicitly sampling/resizing the stored mask into the Reference raster. This is not implicit evaluator resampling; it is part of that node's declared operation.

This avoids the otherwise unavoidable problem where a full-resolution external mask would match export but fail every downscaled preview.

A coverage/data mask should interpret stored values as coverage rather than automatically applying an RGB display transfer function.

Example:

```text
8-bit value 128 → approximately 128/255 coverage
```

A separate explicit operation can convert a colour Image to Mask using luminance / R / G / B / alpha semantics.

Exact file decoding, filtering, asset references, relinking, orientation and persistence details can wait until the feature is implemented / Step 7 persistence work.

Missing external assets make the Mask unavailable and follow the fail-closed unavailable-Mask rule. They must never silently become white.

## Pen / path masks

### Accepted conceptual direction

Future vector/path masks fit the same graph model:

```text
Pen / Bezier Path
        ↓
      Mask
```

Path/control-point state should be serializable and resolution-independent.

Viewer drawing/editing is a UI tool. It does not require a separate mask renderer or scheduler.

Rasterisation occurs for the current preview/export evaluation domain.

Detailed path maths and UI interaction are deferred.

## Mask inspection

### Accepted product need and Step 6 evaluator support

RawNode needs a practical way to inspect both Mask and intermediate Image outputs.

The Step 6 evaluator should therefore support requesting a named output endpoint, not only the authoritative document output.

Candidate behaviour:

- selecting a Mask-producing node can show its black/gray/white Mask;
- selecting an Image node can show that node's Image output;
- while editing a Mask attached to an adjustment, the viewer can request the masked node's Image input/output so the user draws against the content the Mask is actually affecting;
- future viewer modes may include Normal / Mask only / Mask overlay.

The mask generator's Reference is **not necessarily the correct editing image**. Reference supplies frame dimensions only. Across a same-size warp, showing the Reference could display different content from the masked node's Image input.

This uses the same evaluator/runtime and does not create a separate processing path.

General editing of an upstream Mask while viewing through arbitrary downstream geometry remains a later transform-aware viewer feature.

## List View / Node View

### Accepted

Both views represent the same graph.

List order does not determine render order.

Topology editing belongs primarily to Node View.

### Open

Exact List View projection for arbitrary DAGs is not yet settled.

Possible approaches include:

- derived main-spine / nested mask presentation;
- stable topological order plus non-semantic presentation rank.

Do not let Step 6 accidentally make vector/list order authoritative again.

## Persistence boundary

### Accepted

Sidecar V2 remains the current linear document format.

Step 6 must not silently flatten nonlinear topology into V2.

Until Step 7 provides versioned graph persistence, nonlinear state must either remain unavailable to normal user editing or unsupported writes must be clearly gated/refused.

Future persistence must preserve at least:

- stable node IDs;
- node/backend identity;
- parameters;
- bypass/enabled state;
- strength/mix;
- blend mode identity;
- typed connections;
- named output ports;
- repeated mixer-slot IDs and order;
- document output;
- missing/unavailable nodes;
- external asset references where relevant.

Exports do not receive RawNode sidecars.

## Step 6 implementation target

Step 6 remains one architectural transition: explicit topology and executable sequential DAG evaluation land together.

A sensible internal implementation sequence is:

```text
1. Graph value model
      Image raster + dimensions
      Mask raster + dimensions

2. Evaluation contract
      EvaluationContext
      actual per-input dimensions
      requested Image/Mask output endpoint
      authoritative document output by default

3. Port model
      fixed named ports
      repeated / variadic groups
      stable slot IDs
      typed endpoint references
      optional Reference Image ports

4. Validation
      types
      cardinality
      cycles, including Reference dependencies
      required/optional inputs
      unavailable state
      raster-dimension compatibility

5. Sequential evaluator
      dependency traversal
      fan-out
      shared upstream evaluation once
      intermediate buffer lifetime
      cancellation
      per-evaluation compatibility checks

6. Migrate equivalent linear App::nodes behaviour
      onto the real graph/evaluator
```

These may be implemented and reviewed incrementally, but there must not be two authoritative processing models.

Preserve the existing RenderRuntime / DocumentMutation ownership and lifetime guarantees.

Do not add:

- per-value SpatialDomain / transform propagation;
- persistent node caches;
- branch parallelism;
- ROI/tile scheduling;
- general job system;
- automatic processor cloning.

### OFX adapter follow-ups exposed by the spatial review

The spatial challenge found several existing OFX host issues. They are **adapter/backend correctness issues**, not reasons to enlarge the graph value model.

Before generic RawNode Mask / Strength / Blend is shipped on OFX nodes, explicitly settle and test:

- how OFX Region of Definition origin/bounds collapse into RawNode's raster-local frame;
- per-evaluation render scale for preview versus full-resolution export;
- project size / extent properties expected by plugins;
- optional OFX Mask clips: do not report/feed an internal plugin Mask clip as connected Source RGB unless RawNode intentionally supports that clip;
- normalized OFX parameter-default handling.

A strong candidate policy for third-party OFX filters is a frame-preserving RawNode adapter: render/clip the effect into the input frame by default so ordinary effects remain maskable and strength-mixable. The existing bundled OFX Crop is a special size-changing operation and is a candidate for eventual native implementation rather than defining third-party OFX frame semantics.

This OFX policy should be finalized before generic masking/strength on OFX ships. It does not need to block the generic Step 6 DAG if that feature is not yet exposed.

## Decisions still required before Step 6 coding

The spatial architecture itself is now settled. The remaining implementation-level decisions are smaller:

1. **Exact graph/port data shape**, including repeated slot identity and ordering.
2. **Unavailable-state representation / propagation mechanics**, implementing the accepted distinction between recoverable unavailability and hard graph/value incompatibility.
3. **Mask-generator bypass behaviour.**
4. **How Step 6 gates V2 save/copy/autosave paths** until Step 7 can persist topology safely.

The following are accepted Step 6 requirements rather than open questions:

- no per-value SpatialDomain;
- raster dimensions live on Image/Mask values;
- per-evaluation `EvaluationContext`;
- each node uses actual evaluated input dimensions;
- Reference is a real graph dependency;
- requested Image/Mask endpoint evaluation;
- raster mismatch is a hard error for that requested evaluation;
- export refuses on a hard evaluation incompatibility.

The following should be resolved before their corresponding feature ships, but do **not** need to block the core DAG:

- OFX RoD→raster / render-scale / optional-mask-clip policy;
- exact geometric coordinate convention before the first geometric generator is persisted;
- full-resolution dimension-only preflight/warning;
- External Mask decoding/filtering details;
- native Crop / Resize rounding details beyond the generic deterministic-sizing rule.

The following do **not** need to block Step 6:

- Qualifier algorithm;
- non-Normal blend maths;
- Parallel Mixer equation;
- fractional Mask-combine maths;
- Layer Mixer implementation;
- arbitrary DAG List View presentation;
- pen/path implementation;
- transparent-image alpha compositing;
- ROI/tile optimisation;
- caching;
- parallel graph evaluation.

## Proposed next steps

1. Settle the exact graph/port/repeated-slot data shape.
2. Settle unavailable-state representation/propagation mechanics using the accepted fail-closed-vs-hard-error distinction.
3. Settle Mask-generator bypass behaviour.
4. Settle the temporary V2 persistence gate for Step 6.
5. Ask Codex for a final implementation-readiness review against current source without coding.
6. Ask Claude for an independent final adversarial recheck of the complete Step 5 contract.
7. Distill accepted Step 5 decisions into `docs/DECISIONS.md`, `docs/ARCHITECTURE.md`, and `docs/PROJECT_CONTEXT.md`, including removal of the older branch-compatibility/spatial-metadata wording.
8. Close Step 5.
9. Begin Step 6 with a focused implementation plan and regression tests.

The OFX adapter findings from the spatial review should be tracked as focused follow-up work and resolved before generic Mask / Strength / Blend is exposed on OFX nodes.
