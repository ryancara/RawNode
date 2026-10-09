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

An Image is floating-point image data plus the spatial information required to know where its pixels belong.

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

For example, a Crop that changes dimensions cannot be pixelwise mixed with its uncropped input unless a valid spatial relationship / reformat path exists.

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

Geometric generators do not require RGB pixels merely to generate their shape, but they do require a defined document/spatial reference.

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

### Open: spatially incompatible processor output

The generic wrapper needs a precise rule when a processor changes raster dimensions or spatial mapping.

This is a generic value-compatibility question, not a processor whitelist.

Likely initial behaviour:

- full-strength Normal processing may forward the processor result;
- any operation that requires pixelwise mixing / Mask application requires compatible spatial domains;
- otherwise evaluation reports a clear incompatibility until an explicit alignment/reformat operation exists.

This must be settled before Step 6 evaluator implementation.

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

### Accepted: dimensions alone are insufficient

Two images can have equal width/height while referring to different regions of the document.

Example:

```text
1000×1000 crop from top-left
1000×1000 crop from bottom-right
```

Those buffers are not spatially interchangeable.

A Mask also needs to know the spatial domain to which its samples apply.

### Strong candidate: minimal document-space descriptor

Step 6 should carry enough spatial information to determine whether graph values are aligned.

The intent is deliberately smaller than a full ROI/tile scheduler.

Candidate conceptual data:

```text
SpatialDomain
    reference/document identity
    raster width
    raster height
    mapping between raster and document/reference coordinates
```

The exact representation is **Open**.

The design should borrow established concepts rather than inventing a large RawNode-specific geometry system.

Useful references include:

- OpenFX canonical coordinates vs pixel coordinates / Region of Definition;
- GIMP layer extents and offsets relative to the canvas;
- Natron image bounds / merge extents;
- vkdt ROI propagation and its documented drawn-mask/transform limitations;
- Resolve's distinction between input, node and output sizing.

### Accepted constraints for the first DAG

- no automatic resize/reformat;
- no ROI/tile scheduler;
- no branch-specific spatial optimisation;
- incompatible spatial domains should not be silently treated as aligned;
- geometric Mask parameters must not be stored in preview-pixel coordinates;
- preview and full-resolution export must describe the same document-space shape.

### Open spatial decisions

Before Step 6 implementation, settle at minimum:

- exact `SpatialDomain` representation;
- reference frame / origin convention;
- pixel-centre convention if needed;
- preview-scale representation;
- how Crop/rotation/geometry-changing operations describe their output mapping;
- what happens when the mapping is unknown;
- exact compatibility test for pixelwise Mask/mix/mixer operations.

## External masks

### Accepted conceptual direction

Future workflow:

```text
External Mask File → Mask
```

This supports masks generated by Photoshop, external AI tools, or other applications.

A coverage/data mask should interpret stored values as coverage rather than automatically applying an RGB display transfer function.

Example:

```text
8-bit value 128 → approximately 128/255 coverage
```

A separate explicit operation can convert a colour Image to Mask using luminance / R / G / B / alpha semantics.

Exact file decoding, asset references, relinking, orientation and persistence details can wait until the feature is implemented / Step 7 persistence work.

Missing external assets follow the unavailable-Mask safety rule and must never silently become white.

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

### Accepted product need

RawNode needs a practical way to inspect Mask output.

Candidate behaviour:

- selecting a Mask-producing node can show its black/gray/white Mask;
- selecting an Image node can provide "Show Node Mask";
- future viewer modes may include Normal / Mask only / Mask overlay.

This should use the same evaluator/runtime rather than create a separate mask-processing path.

The minimum evaluator API required for this is still to be settled with Step 6.

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
      Image
      Mask
      spatial descriptor

2. Port model
      fixed named ports
      repeated / variadic groups
      stable slot IDs
      typed endpoint references

3. Validation
      types
      cardinality
      cycles
      required/optional inputs
      unavailable state

4. Sequential evaluator
      dependency traversal
      fan-out
      shared upstream evaluation once
      intermediate buffer lifetime
      cancellation

5. Migrate equivalent linear App::nodes behaviour
      onto the real graph/evaluator
```

These may be implemented and reviewed incrementally, but there must not be two authoritative processing models.

Preserve the existing RenderRuntime / DocumentMutation ownership and lifetime guarantees.

Do not add:

- persistent node caches;
- branch parallelism;
- ROI/tile scheduling;
- general job system;
- automatic processor cloning.

## Decisions still required before Step 6 coding

The remaining implementation-level decisions are intentionally small:

1. **SpatialDomain representation and compatibility rules.**
2. **Exact graph/port data shape**, including repeated slot identity and ordering.
3. **Generic wrapper behaviour when processor output changes dimensions/spatial mapping.**
4. **Exact unavailable-state propagation**, especially how a missing Mask disables its dependent effect while preserving the graph.
5. **Mask-generator bypass behaviour.**
6. **Minimum requested-endpoint mechanism for Mask inspection**, or whether that waits until the first mask implementation.
7. **How Step 6 gates V2 save/copy/autosave paths** until Step 7 can persist topology safely.

The following do **not** need to block Step 6:

- Qualifier algorithm;
- non-Normal blend maths;
- Parallel Mixer equation;
- fractional Mask-combine maths;
- Layer Mixer implementation;
- arbitrary DAG List View presentation;
- external-mask decoding details;
- pen/path implementation;
- transparent-image alpha compositing;
- ROI/tile optimisation;
- caching;
- parallel graph evaluation.

## Proposed next steps

1. Resolve the seven implementation-level questions above, beginning with the spatial model and graph/port shape.
2. Update this working document with the accepted answers.
3. Ask Codex for an implementation-readiness review against current source without coding.
4. Ask Claude for an independent adversarial recheck of the final Step 5 contract.
5. Distill accepted Step 5 decisions into `docs/DECISIONS.md`, `docs/ARCHITECTURE.md`, and `docs/PROJECT_CONTEXT.md`.
6. Close Step 5.
7. Begin Step 6 with a focused implementation plan and regression tests.
