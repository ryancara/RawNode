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
- [x] Independent Codex / Claude port-model review
- [x] Settled fixed-port / repeated-slot representation
- [x] Settled missing Mask / Reference full-coverage fallback + warning
- [x] Settled Mask-generator bypass = full coverage
- [x] Settled that old V2 compatibility is not a product requirement
- [ ] Final Step 5 implementation-readiness review
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

Unavailable processors/effects remain represented in the document.

Their identity, parameters, declared interface and graph connections must be preserved so reinstalling or relinking the dependency can restore the edit.

Missing state is not the same as user bypass state.

RawNode must show a persistent warning when a connected dependency is unavailable.

### Accepted: missing Mask / Reference is fail-open with warning

Ryan's chosen product behaviour is deliberately simple:

```text
Mask input unconnected
    → full coverage

connected Mask unavailable
    → full coverage + persistent warning

Mask generator's connected Reference unavailable
    → generator cannot produce its intended Mask
    → downstream Mask use falls back to full coverage + persistent warning
```

In other words, a missing/unavailable Mask or Reference behaves as though no Mask is restricting the dependent effect.

For an ordinary Image adjustment this means the adjustment becomes global.

For a Layer Mixer slot, an absent or unavailable slot Mask means that layer contributes at full coverage.

The connection/state remains represented. RawNode must not delete the edge or silently convert "connected but unavailable" into "never connected", because restoring the dependency should restore the intended edit.

This is a deliberate fail-open product choice. The persistent warning is therefore important.

### Accepted: unavailable is different from invalid

Availability and numerical graph validity are separate.

If all required values evaluate successfully but cannot legally participate in the requested operation, that is a hard evaluation error.

Example:

```text
Image 6000×4000
Mask  3000×2000

→ dimension incompatibility
→ requested evaluation fails
→ export refuses
```

A dimension mismatch is not treated as a missing Mask and does not fall back to global coverage.

A missing ordinary unary Image effect may continue to use RawNode's existing "preserve + warn + bypass/pass-through where possible" philosophy. More complex unavailable graph operations should preserve their interface/state and use the narrowest operation-specific fallback that is actually defined rather than deleting topology.

## Bypass

### Accepted: ordinary graph operations

Unary Image processor bypass:

```text
pass Image input through unchanged
```

Unary Mask processor bypass:

```text
pass Mask input through unchanged
```

Mixer bypass:

```text
pass its defined base / background Image through
```

### Accepted: Mask-generator bypass = full coverage

A source-like Mask generator such as Radial, Gradient or future Pen/Path has no Mask input to pass through.

Its generic bypass result is therefore:

```text
Mask = 1.0 everywhere in the generator's evaluated frame
```

So:

```text
Radial enabled
    → Radial Mask

Radial bypassed
    → full-coverage Mask
    → dependent adjustment applies globally
```

The Mask connection remains intact, so re-enabling the generator immediately restores its contribution.

This is a defined numerical bypass result, not hidden graph rewiring.

For downstream Mask combiners, the white result participates in their normal maths. "Bypass" therefore does not mean "remove this node from an arbitrary Mask expression"; it means "this generator currently produces full coverage".

## Port model / mixer-ready topology

### Accepted: static named typed port declarations

Each node kind declares a small immutable interface in code.

Ports are:

- named by stable semantic IDs;
- explicitly input or output;
- typed `Image` or `Mask`;
- required or optional.

Examples:

```text
Exposure
    inputs
        image : Image required
        mask  : Mask optional
    outputs
        image : Image

Qualifier
    inputs
        image : Image required
    outputs
        mask : Mask

Radial
    inputs
        reference : Image optional
    outputs
        mask : Mask
```

Port labels, UI position and translated display names are presentation only.

Fixed ports do not need per-instance runtime socket objects. Known node kinds can share immutable declarations.

### Accepted: consumer-owned incoming connections

Each scalar input accepts at most one producer, so RawNode stores the upstream output on the consuming input rather than maintaining a second authoritative global edge list.

Conceptually:

```text
Node.fixedInputs
    portId → optional OutputRef

Slot.inputs
    portId → optional OutputRef
```

Output fan-out is represented by multiple consumers referring to the same `OutputRef`.

Any UI edge list, adjacency index or "find consumers" map is derived data, not a second topology authority.

This makes "two producers connected to one input" impossible by construction and lets deleting a slot delete its incoming connections with that slot.

### Accepted: typed endpoint/address concepts

Use direction-aware C++ concepts rather than requiring input and output port names to be globally unique.

Conceptually:

```text
OutputRef
    nodeId
    outputPortId

InputAddress
    nodeId
    optional slotId
    inputPortId
```

Therefore an ordinary processor may naturally use `image` as both an input and output semantic name if desired; direction disambiguates them.

Examples:

```text
Qualifier.output(mask)
Exposure.input(mask)

LayerMixer.slot(7).input(image)
LayerMixer.slot(7).input(mask)
```

Persist these as structured components rather than delimiter-encoded strings.

### Accepted: ordered node-owned repeated slots

Nodes that need a variable number of associated inputs/settings own an ordered vector of Slots.

A Slot has:

```text
stable SlotId
its incoming connections
its slot-local settings
```

The slot's **position in the vector is semantic order**.

The SlotId is identity.

These must never be conflated.

A SlotId:

- is scoped to its owning node;
- is stable across reorder, reconnect and parameter changes;
- is persisted once graph persistence exists;
- is never derived from vector position or producer identity;
- should be monotonic / not reused within that node.

Use a strong SlotId type so vector indices cannot be passed accidentally where identity is required.

There is no current requirement for more than one repeated slot collection on a node, so Step 6 does not need a persisted `GroupId`. If a future node genuinely needs multiple independent repeated collections, that can be added without changing Node/Port/Connection topology.

### Accepted: one authoritative slot order

Store semantic order only as the order of the Slot vector.

Do not also persist an `order` field on each Slot.

For Layer Mixer:

```text
slots = [A, B, C]

evaluation:
    background
      → A
      → B
      → C
```

After the user drags C above A:

```text
slots = [C, A, B]

evaluation:
    background
      → C
      → A
      → B
```

Slot C keeps its:

- Image connection;
- Mask connection;
- opacity;
- blend mode;
- future slot-local settings.

Only its position changes.

Visible labels such as "Layer 1", "Layer 2", "Layer 3" are derived from current position and are not durable identities.

### Accepted: slot-local settings live on the Slot

Layer Mixer concept:

```text
background : Image

slots : ordered
    SlotId
    image : Image
    mask  : Mask?
    opacity
    blendMode
```

The producer does not own these settings because one producer may feed several mixer slots with different settings.

Do not use parallel arrays such as:

```text
images[i]
masks[i]
opacities[i]
blendModes[i]
```

The Slot object keeps the association explicit.

### Accepted: Parallel Mixer uses the same topology primitive

Parallel Mixer concept:

```text
base : Image

slots : ordered
    SlotId
    image : Image
    future branch-local settings if ever needed
```

Its exact arithmetic remains open.

Slot order is stored deterministically even if the eventual equation is mathematically commutative.

Layer Mixer and Parallel Mixer are separate node kinds with separate operation semantics. Reusing the Slot topology primitive does not imply identical slot settings or maths.

### Accepted: Source and document output are explicit endpoints

Source is a normal zero-input graph operation with an Image output.

The document stores an explicit authoritative Image output endpoint rather than deriving output from node-storage order.

Conceptually:

```text
primarySource
    → Source.image

documentOutput
    → some Image OutputRef
```

A source-only document has Source as both.

Requested inspection endpoints do not change `documentOutput`.

### Accepted: graph validation rules

At minimum validate:

- unique Node IDs;
- stable declared port IDs;
- unique Slot IDs within a node;
- endpoint existence;
- output-to-input direction;
- exact Image/Mask type compatibility;
- at most one producer per destination input;
- required/optional input rules;
- required slot inputs;
- an Image-valued document output;
- cycles through all explicit dependencies, including repeated-slot and Reference inputs.

Required inputs may temporarily be unconnected while the user is editing. Such a node is representable but cannot successfully evaluate when the requested result depends on it.

Bypass or unavailability does not remove topology for structural cycle checking.

### Accepted: index safety

Durable graph operations use NodeId / SlotId / semantic port IDs, not list/vector indices.

UI selection IDs and drag/drop payloads for graph objects should also use stable IDs.

A useful regression fixture is deliberately out-of-order Slot IDs such as:

```text
slot vector = [3, 1, 2]
```

and tests that prove:

- reorder changes semantic evaluation order;
- Slot IDs do not change;
- delete/insert does not renumber unrelated slots;
- cycles through repeated inputs are rejected;
- one producer may feed fixed and repeated consumers safely.

### Accepted: current transitional structures are replaced, not extended

The existing transitional `NodeInput` / `NodeInputRole` representation is not rich enough for explicit topology.

Step 6 should replace it cleanly with named typed ports and consumer-owned references.

Retain the useful concepts already present elsewhere:

- stable Node identity;
- Image/Mask distinction;
- processor/backend identity;
- preserved parameters;
- RenderRuntime / DocumentMutation lifetime and mutation boundaries.

Do not keep vector/list order as processing semantics once the graph lands.

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

A connected but unavailable Reference does **not** silently fall back to Source. It makes that Mask unavailable; the dependent Mask use then follows the accepted fail-open rule: full coverage plus a persistent warning.

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

Missing external assets preserve their graph state and show a persistent warning. Under the accepted fail-open Mask rule, a dependent Mask use falls back to full coverage until the asset is relinked.

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

### Accepted: old V2 compatibility is not a product requirement

RawNode currently has no external user base whose documents must constrain the new graph architecture.

The future graph sidecar format may therefore replace Sidecar V2 cleanly.

Do not add migration machinery, compatibility layers or architectural compromises merely to preserve old development-sidecar semantics.

A cheap one-way migration for developer test files may be added later if convenient, but it is optional.

### Accepted: never silently flatten a graph through V2

Removing backwards-compatibility requirements does **not** make silent corruption acceptable during development.

Once Step 6 can represent topology that V2 cannot express:

```text
graph state not representable by current serializer
    → refuse / disable that save path
    → never flatten it into node-array order
```

This is only a temporary anti-corruption guard until Step 7 lands.

No elaborate V2 gating architecture is required.

### Step 7 graph persistence target

The replacement format should persist at least:

- stable Node IDs;
- node kind/backend identity and schema/interface version where needed;
- parameters and generic node controls;
- fixed input references;
- static/saved interface information sufficient to preserve unavailable or unknown nodes;
- ordered Slot arrays;
- stable Slot IDs and slot-local settings;
- document Source/output endpoints;
- missing/unavailable state and external asset references where relevant.

Port IDs and node-kind IDs become persistence contracts. A rename requires an explicit alias/migration rather than positional guessing.

Node array/storage order is not execution order.

Copy/paste of a graph fragment should:

- allocate fresh Node IDs for pasted nodes;
- build one complete old→new NodeId map before rewriting references;
- preserve semantic port IDs;
- preserve Slot IDs when copying an entire node, because the new owning NodeId changes their scope;
- allocate a fresh SlotId when duplicating a slot inside an existing node;
- preserve slot order/settings;
- copy internal references;
- drop external incoming references unless the command explicitly defines a rebinding rule, with warnings for dropped Mask/Reference links.

Exports remain derivative files and do not receive RawNode sidecars.

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
      static named typed port declarations
      consumer-owned fixed inputs
      ordered node-owned repeated Slots
      stable Slot IDs
      typed OutputRef / InputAddress concepts
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

The Step 5 product/architecture contract is now substantially settled.

There are no remaining broad product-model decisions required before the final implementation-readiness review.

Accepted decisions now include:

- one authoritative graph;
- typed Image / Mask values;
- no automatic intermediate colour metadata;
- minimal raster/reference spatial model;
- per-evaluation context and actual input dimensions;
- requested Image/Mask endpoint evaluation;
- static named typed port declarations;
- consumer-owned scalar input references;
- ordered node-owned repeated Slots with stable Slot IDs;
- semantic slot order separated from slot identity;
- explicit Source/document output endpoints;
- generic Image-node Mask / Strength / Blend ownership;
- missing/unavailable Mask or Reference = full coverage + persistent warning;
- Mask-generator bypass = full coverage;
- valid-but-incompatible raster dimensions = hard evaluation error;
- no backwards-compatibility requirement for Sidecar V2.

The final Step 5 review should now look for:

- contradictions or underspecified implementation boundaries;
- a concrete case that still forces the implementer to invent architecture;
- unsafe interaction with current RenderRuntime / DocumentMutation lifetime rules;
- accidental retention of list/vector processing order;
- persistence or copy/paste ambiguity that would force a topology redesign.

The following still remain intentionally deferred until their corresponding feature:

- exact geometric coordinate convention before the first persisted geometric generator;
- OFX RoD→raster / render-scale / optional-mask-clip policy before generic OFX masking/strength ships;
- full-resolution dimension-only preflight/warning;
- Qualifier algorithm;
- non-Normal blend maths;
- Parallel Mixer arithmetic;
- fractional Mask-combine maths;
- Layer Mixer implementation;
- arbitrary DAG List View presentation;
- pen/path implementation;
- transparent-image alpha compositing;
- ROI/tile optimisation;
- caching;
- parallel graph evaluation.

## Proposed next steps

1. Ask Codex for a final implementation-readiness review against the complete Step 5 contract and current source, without coding.
2. Ask Claude for an independent final adversarial recheck of the same complete contract.
3. Reconcile only concrete blockers or contradictions found by those reviews.
4. Distill the accepted contract into `docs/DECISIONS.md`, `docs/ARCHITECTURE.md`, `docs/PROJECT_CONTEXT.md`, `CLAUDE.md` where appropriate, and remove superseded wording.
5. Close Step 5 and merge the contract/docs PR.
6. Begin Step 6 with a focused implementation plan and regression tests.

The OFX adapter findings from the spatial review remain focused follow-up work and should be resolved before generic Mask / Strength / Blend is exposed on OFX nodes.
