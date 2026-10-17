# Step 5 Graph / Mask Product Contract

**Status:** Complete; accepted contract for Step 6.  
**Purpose:** Product and architecture contract settled by Step 5 of the RawNode architecture migration.  
**Final independent review head:** `43935dd2bd815e0eecfe43b9a3e418610fe62f70`

This document records the accepted product and architecture contract settled
before Step 6. Steps 6 and 7 have since merged. Historical transition language
below (including temporary V2 guards) describes those completed stages; current
graph-native persistence is specified in `docs/STEP7_PERSISTENCE_DESIGN.md`.
The switchable Graph/List editor is next (D047), followed by Step 8 masking.

Items marked **Accepted** are the Step 5 contract. Items marked **Open** are deliberately deferred feature details and must not be silently decided by unrelated Step 6 implementation. The durable architecture and decision documents summarize the accepted contract; this file remains the detailed design record.

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
- [x] Final independent Step 5 implementation-readiness reviews by Codex and Claude
- [x] Reconcile final availability / bypass / runtime ambiguities
- [x] Review this document against current source
- [x] Close Step 5 in durable architecture / decision docs
- [x] Begin Step 6 implementation (completed and merged in PR #58)

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

- an **available Mask value** is present;
- Strength is less than 1; or
- Blend Mode is non-Normal;

then the processor output width and height must match the node's Image input width and height.

If the dimensions do not match, the requested evaluation fails. Export refuses rather than silently omitting the edit. RawNode does not implicitly resize, resample, align or reproject the result.

A connected Mask whose value is **unavailable** follows the fail-open rule below and counts as absent for this self-mixing trigger. A bypassed Mask generator that successfully produces a real full-coverage Mask is still an available Mask and participates normally in compatibility checks.

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

### Accepted: evaluation distinguishes value, unavailable, hard error and cancellation

Graph evaluation must distinguish at least:

```text
available value
unavailable value + diagnostics
hard evaluation error
cancelled / obsolete evaluation
```

These states are not interchangeable.

A warning does not by itself mean a value is unavailable. For example, an unavailable ordinary unary Image processor with a defined pass-through fallback may produce an **available Image plus a warning**.

A node whose required input value is unavailable produces an unavailable output unless that node explicitly defines a narrower fallback.

Therefore an unavailable Mask propagates through operations that require its Mask data:

```text
Missing External Mask
        ↓ unavailable
      Invert
        ↓ unavailable
     Exposure.mask
        ↓
full coverage + warning
```

RawNode does **not** manufacture an upstream white Mask merely because a later consumer uses fail-open coverage.

Requesting an unavailable Mask directly for inspection returns an unavailable result with diagnostics; it does not invent a consumer-sized white raster.

### Accepted: fail-open occurs at optional coverage use

Ryan's chosen product behaviour is deliberately fail-open at the point where a Mask restricts an effect or layer:

```text
Mask input unconnected
    → full coverage

connected Mask value unavailable
    → treat coverage as absent/full
    → persistent warning

Mask generator's connected Reference unavailable
    → generator Mask unavailable
    → downstream optional coverage use becomes full coverage
    → persistent warning
```

For an ordinary Image adjustment, unavailable optional Mask coverage makes the adjustment global.

For a Layer Mixer slot, an absent or unavailable slot Mask means that layer contributes at full coverage; its own opacity/blend settings still apply.

The connection/state remains represented. RawNode must not delete the edge or silently convert "connected but unavailable" into "never connected", because restoring the dependency should restore the intended edit.

A shared unavailable Mask does not need a fabricated shared raster. Each optional coverage consumer independently applies full coverage and carries the relevant warning.

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

A valid wrong-sized Mask is not treated as unavailable and does not fall back to global coverage.

A dangling endpoint, type mismatch, cycle or missing required connection is graph invalidity rather than missing optional coverage.

Cancellation is also separate and must not be reinterpreted as unavailable data.

A missing ordinary unary Image effect may continue to use RawNode's existing preserve + warn + pass-through philosophy where that fallback is defined. More complex unavailable graph operations should preserve their interface/state and use only explicitly defined fallbacks.

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

A bypassed operation evaluates only the inputs needed to produce its bypass result.

Therefore:

- unary Image bypass needs only its pass-through Image input;
- unary Mask bypass needs only its pass-through Mask input;
- mixer bypass needs only its base/background input;
- source-like Mask-generator bypass still needs its Reference frame.

Unused Mask/branch inputs of a bypassed operation are not evaluated merely because their connections exist. Structural cycle validation still includes all stored dependencies regardless of bypass.

### Accepted: Mask-generator bypass = full coverage when its frame resolves

A source-like Mask generator such as Radial, Gradient or future Pen/Path has no Mask input to pass through.

Its generic bypass result is:

```text
Mask = 1.0 everywhere in the resolved generator frame
```

Frame resolution still occurs.

If Reference is absent, the current-evaluation primary Source supplies the frame.

If a connected Reference is available, that Reference supplies the frame.

If a connected Reference is unavailable, there is no valid frame in which to manufacture a white raster. The generator output therefore remains **unavailable**, and a downstream optional Mask coverage input follows the accepted fail-open rule.

So:

```text
Radial bypassed + valid frame
    → real full-coverage Mask

Radial bypassed + connected unavailable Reference
    → unavailable Mask
    → coverage consumer becomes full + warning
```

The Mask connection remains intact.

A real white bypass Mask is ordinary Mask data. If it feeds Invert, the result is black; if it feeds a future combiner, white participates in that combiner's defined maths.

This is deliberately different from unavailable Mask propagation. Bypass is a defined numerical result when its required frame exists; unavailability means the Mask data does not exist.

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

Each single-producer input accepts at most one producer, so RawNode stores the upstream output on the consuming input rather than maintaining a second authoritative global edge list.

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

`OutputRef` components are persisted as the upstream source stored on each consuming input, and `documentOutput` is persisted explicitly. `InputAddress` is a command/validation address derived from the consumer's storage location; it is **not** separately persisted as a second edge record. Structured fields are preferred over delimiter-encoded strings.

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

A document has exactly one primary Source bound to the current document image. Source cannot be deleted. Copy/paste and presets do not create a second Source; transferred references to Source rebind to the destination document's Source.

A source-only document has Source as both.

`documentOutput` must always name an existing Image output. A unary-spine delete that removes the current output retargets it to that deleted node's Image producer under the List View splice rule below.

Requested inspection endpoints do not change `documentOutput`.

For Step 6, preserve current source-only presentation behaviour: when the requested Image endpoint is `Source.image`, the viewer uses the document input interpretation. Source-only export remains disabled rather than introducing an unrelated product change during the graph migration. Mask inspection uses presentation-only grayscale and never Image colour conversion.

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

The existing transitional `NodeInput` / `NodeInputRole` representation is not rich enough for explicit topology. The reserved `Node::opacity` / `CompositeMode` fields are likewise not the settled generic-control model.

Step 6 should replace these transitional fields cleanly with named typed ports, consumer-owned references, and the accepted node-level Strength/Blend ownership rather than extending the reserved placeholders.

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

### Accepted Step 6 minimum: derived main Image spine

Step 6 does not need to solve presentation of an arbitrary DAG, but the existing List View must stop being an executable-order vector.

For the Step 6 UI subset, derive the main Image spine by walking upstream from `documentOutput` through each ordinary unary node's `image` input toward Source.

List View rows are therefore a presentation of explicit topology.

For unary Image nodes on that spine:

- insert / paste is an explicit edge splice;
- move is an explicit topology rewrite, not a storage-vector permutation;
- delete reconnects consumers of the deleted node's Image output to its Image producer where the unary splice is well-defined;
- if `documentOutput` pointed at the deleted unary node, it retargets to that Image producer;
- consumers of other outputs are disconnected rather than guessed;
- each compound edit is staged/validated and committed through one `DocumentMutation`.

Nodes outside the main spine may be shown separately or minimally in Step 6 and are not reordered by List View presentation order.

### Open

The exact List View projection for an arbitrary future DAG remains deferred. Step 6 must not invent a second presentation order with processing semantics.

## Step 6 mutation, parameter and diagnostic rules

### Accepted: structural edits use the mutation barrier

Structural graph changes are document mutations:

- connect / reconnect / disconnect;
- add / delete node;
- change `documentOutput`;
- add / delete / reorder Slot;
- enable/bypass state when changing it would alter which dependencies the evaluation needs.

Stage and validate compound topology changes before committing them under `DocumentMutation`. `DocumentMutation` provides execution exclusion, not automatic rollback.

### Accepted: node-owned mutable controls must be race-free

Processor backends already synchronize their own parameters. New node-owned controls such as Strength, Blend Mode and future slot settings must also be read race-free by the preview worker.

They are:

- blocked during export under the existing parameter-edit exclusion;
- followed by a normal preview request;
- represented with appropriate atomics/locking or captured in a consistent per-evaluation snapshot.

Do not add a second scheduler or feature-specific lock protocol.

The requested inspection endpoint is preview-demand state and must likewise be captured consistently for an evaluation/publication.

### Accepted: diagnostics travel with evaluation results

Evaluation returns diagnostics for reachable dependencies together with the requested result state.

Warnings such as:

- missing unary processor with a defined pass-through fallback;
- unavailable Mask/Reference resolved fail-open at a coverage input;

do not turn a successful result into a hard error.

Export proceeds when the requested document output is successfully evaluated with such warnings and reports them in its final status.

Hard graph/value errors still refuse export.

Warnings are derived from the dependencies reachable from the requested endpoint, not from an unconditional scan of every stored node.

Publication retains the request identity, result value kind (Image/Mask) and diagnostics together so an obsolete Image/Mask inspection result cannot be recoloured or presented under a newer request's interpretation.

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

Step 6 implements the typed Image/Mask value model, availability/error propagation, requested-endpoint evaluation and enough test-only graph operations to prove those contracts. It does **not** need to expose the production generic adjustment Mask × Strength × Normal wrapper in the UI. The first concrete user-facing Mask application path remains Step 8. This prevents Step 6 topology work from silently absorbing the first masking feature while still making the evaluator ready for it.

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

There are no remaining broad product-model decisions. The final independent implementation-readiness reviews found the architecture sound after the narrow availability/bypass/runtime clarifications now incorporated above.

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

## Historical proposed next steps

Step 5 is closed. The original sequence below is retained as history; Steps 6
and 7 have since merged. D047 adds the switchable Graph/List editor before
Step 8; current milestone status lives in `docs/PROJECT_CONTEXT.md`.

1. Merge the reconciled Step 5 contract and durable-document updates.
2. Begin Step 6 with a focused implementation plan and regression-test matrix.
3. Keep Step 6 scoped to one authoritative explicit graph plus sequential requested-endpoint evaluation.
4. Preserve the OFX adapter findings as focused follow-up work before generic Mask / Strength / Blend is exposed on OFX nodes.
5. Begin the first concrete production Mask path in Step 8 after graph persistence in Step 7.

No additional broad architecture review is required before Step 6 unless implementation uncovers a concrete contradiction against this contract.
