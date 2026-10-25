# Step 8 masking: product and architecture discussion notes (draft)

**Status:** Working design record, not an approved implementation contract.
**Recorded:** 25 October 2026.
**Purpose:** Preserve the RawNode masking discussion before resolving opacity, masking and blend semantics. This document deliberately distinguishes agreed direction from open decisions.

This draft is separate from the ongoing documentation synchronisation PR. It does not replace the existing accepted contracts in docs/STEP5_GRAPH_CONTRACT.md, docs/ARCHITECTURE.md or docs/DECISIONS.md. Where the ideas below extend or differ from Step 5, they require a deliberate decision and documentation update before implementation.

## 1. Agreed product direction

### 1.1 Masks as first-class graph values

- RawNode's node graph should support reusable, connectable **Mask** outputs, not separate per-tool or per-backend masking systems.
- A Mask is scalar coverage: **black (0) = no adjustment**, **white (1) = full adjustment**, and intermediate grey = partial coverage.
- Mask data is independent of image alpha and has no RGB gamut or transfer function.
- The same Mask can feed more than one downstream processor. Masks can be built up from other Mask-producing/processing nodes.
- Node backends (native, CTL, future DCTL and potentially OFX) must not themselves determine whether Mask output is permitted. The declared processor interface and backend adapter determine the types it actually supports; arbitrary CTL/DCTL output must not be presumed to be an already typed Mask.

### 1.2 Universal generic node controls (agreed UI/product direction; maths remains open)

- Every **processing node**, including Mask generators and Mask processors, should expose a **generic optional Effect Mask input**, an **Opacity** control and a **Blend Mode** control.
- **Source** and **document Output** are excluded.
- The controls should be centrally owned by the graph/evaluation layer rather than reimplemented in each native/CTL/DCTL/OFX processor.
- Keep **Blend Mode visible and editable** even on nodes for which it has no processing effect. Do not hide or disable it merely for that reason.
- RGB blend modes such as **Overlay** and **Soft Light** are desired. For Mask-producing nodes without a compatible Image-to-Image blend, the RGB Blend Mode setting may be stored but have no effect.
- A functional processor Mask input (for example, Mask A and Mask B in a Mask Combine operation) is distinct from the node's generic Effect Mask input.

**Important:** Agreement on universal *controls* is not agreement on identical numerical *meaning* for opacity/masking on every possible node. That remains open below.

### 1.3 Mask types and extensibility

Initial candidates: **Radial Gradient** and **Linear Gradient** as native Mask generators. Future candidates:

- Colour/luminance/hue/saturation qualifiers (e.g. blue pixels).
- Brush/paint masks and pen/path masks.
- Mask invert, blur, refinement and combination (Add / Intersect / Subtract).
- Subject/object selection, including AI-assisted Mask generators.
- CTL/DCTL processors that produce or transform coverage, using an explicit typed adapter contract.

These should use the same graph, type system, renderer, persistence and ordinary processor UI conventions. Advanced features need not ship in the first Mask PR.

## 2. Agreed preview and interaction design

### 2.1 Default selection behaviour

- When a **geometric Mask node** is selected, keep displaying the **normal, final processed photograph**.
- Show the **selected Mask node's on-image editing handles** over that photograph (where the node has handles). Do **not** automatically enable a coloured mask overlay.
- Handles are shown **only when the Mask node itself is selected**, not simply because an Image processor consuming it is selected.
- Deselecting the Mask node hides its handles.
- A Colour Qualifier or other Mask node without geometric handles is still selectable and inspectable; it need not invent handles.

### 2.2 Optional Preview modes

- **Normal:** final processed photograph, with the selected node's handles when relevant.
- **Mask Overlay:** an optional coloured, translucent representation of the selected Mask node's **resulting coverage**, over the photograph, while retaining handles.
- **Mask Only:** selected Mask node's coverage in grayscale (0 black, 1 white).
- Locate Preview mode controls in the **existing Preview toolbar**, not in a new masking workspace or embedded separately in every Mask processor.
- Overlay colour may become configurable later. Red can be an initial candidate, not a permanent requirement.
- If a Mask node feeds several Image processors, its overlay/mask-only display represents the **Mask node's own resulting output**, not the adjustment of one arbitrarily selected consumer.

### 2.3 Direct manipulation and history

- Radial editing should support direct on-photo centre/size/shape/feather (and eventually rotation) controls as appropriate; linear gradients should have position/direction/falloff handles.
- The Inspector also presents numerical controls. **Handles and Inspector edit the same processor parameters**, not parallel, UI-owned mask coordinates.
- Use existing mutation, preview scheduling, history and persistence pathways. Dragging one handle should produce **one Undo step per completed gesture**, not one per animation frame.
- Normal Preview and Mask inspection should share the existing requested-endpoint graph evaluation/runtime rather than inventing a separate mask renderer.

## 3. Existing Step 5 architecture to preserve unless deliberately revised

The repository's accepted Step 5 contract already establishes:

- Typed Image / Mask values, fan-out, explicit connections and requested-endpoint evaluation.
- Mask as scalar coverage independent of alpha.
- A geometric generator can take an **optional Reference Image** for raster dimensions. Without one, it uses document Source as the reference; reference does not imply content tracking or automatic reprojection.
- Missing/unavailable optional Mask input has fail-open **full coverage** semantics, and a bypassed Mask generator has full coverage under the currently accepted Step 5 contract.
- Actual raster dimensions must be respected. A mask or opacity blend is not automatically valid for image processors that resize/crop to incompatible dimensions.
- The evaluator, not every backend, owns generic Image-node Mask/Strength/Blend behaviour. Do not add another render scheduler, sidecar model, cache or graph representation.
- Colour semantics for HDR/scene-linear processing, OFX frame/bounds handling and multi-input compositing need deliberate decisions before those features ship.

**Potential expansion:** Step 5 primarily specified generic Mask/Strength/Blend for Image-to-Image nodes. The proposed universal controls and effect-mask handling on Mask-producing nodes extend that design. Do not silently treat the extension as already formally accepted.

## 4. Open design questions — do not implement by assumption

### 4.1 What does Opacity mean for Mask-producing nodes?

The user wants Opacity to be intuitively **general across nodes**, ideally fading each node's effect away at 0%. Options discussed, with **no final selection**:

**A. Fade Mask coverage towards black:** Radial Mask at 0% outputs zero coverage. This removes the downstream adjustment where the Mask is used, rather than removing the Mask's restriction.

**B. Fade standalone Mask outputs towards white:** Radial Mask at 0% outputs full coverage (white), so a downstream Exposure adjustment applies everywhere. At 50%, black regions become 50% grey, white stays white. This aligns with interpreting 0% as bypass/no restriction and with Step 5's bypassed-generator full-coverage rule. One candidate expression is: output = (1-opacity) * 1 + opacity * generatedMask.

**C. Fade between each node's bypass value and processed result:** Image Exposure → input Image; Mask Blur/Invert → incoming Mask; standalone Mask generator → white. This yields consistent *bypass semantics*, but requires an explicit baseline contract for multi-input nodes and some generators.

**D. Keep generic Opacity mixing for Image nodes initially and defer or separately name Mask strength/density for Mask nodes.** Simpler first slice, but less uniform UI behaviour.

These approaches are **not equivalent**. For example, a white Mask is neutral when multiplied with an adjustment-coverage mask, but white is not a neutral contribution for a future Add/Union Mask Combine node. Do not use “always fade to white if no input” as a blanket rule without testing all graph operations.

### 4.2 What exactly does the generic Effect Mask input mean on a Mask node?

A Mask generator may accept a generic Effect Mask while also emitting a Mask. Possible semantics include:

- Restrict/scale the emitted coverage by multiplying with the incoming Effect Mask.
- Mix between the node's bypass result and processed Mask, with coverage controlling how much of the node's operation applies.

These yield different results, particularly for generators, Invert/Blur and multi-input Mask Combine. The choice is still open. Keep the generic Effect Mask distinct from functional Mask A/B inputs.

### 4.3 Blend Mode semantics

- **Normal** Image-to-Image mixing is the first likely implementation.
- **Overlay, Soft Light, Multiply, Screen** and other familiar modes are desired, but their numerical definitions for negative and >1 scene-linear RGB and alpha policy are unresolved.
- Blend Mode remains visible/editable even when a typed node's output cannot use RGB blending. Persist that setting; do not change rendered output in the no-op case.
- Future Layer/Parallel Mixer branch-compositing semantics are different from ordinary single-node blending and are not yet specified.

### 4.4 Other open implementation details

- Whether 0% Opacity on a Mask Combine preserves one designated functional input, outputs white, or uses an operation-specific bypass.
- Whether all node categories should use exactly the same generic Effect Mask application formula or merely share one central implementation with typed rules.
- What to do with incompatible image/mask dimensions on size-changing OFX or Crop processors (never silently resample without an explicit contract).
- Detailed geometry coordinate/persistence rules, qualifier algorithm, fractional Mask Combine maths and CTL/DCTL typed-Mask adapters.

## 5. Tentative Step 8 implementation order (not yet approved)

1. **Design checkpoint:** settle opacity / generic Effect Mask / bypass semantics without reopening the whole architecture.
2. **First vertical slice:** one native Radial Mask → native Exposure through an optional typed Mask input, Normal mode and a clear processing contract. Test preview, full-resolution export, sidecar save/reopen, fan-out, error handling and Undo/Redo.
3. **Preview UI:** selected Mask handles over the final photo by default; optional Overlay and Mask Only. Inspector and handles share parameter storage.
4. **Follow-up processors:** Linear Gradient, Mask processing and combination, qualifier and backends as defined by their separate contracts.
5. **Follow-up blend modes:** explicitly specified scene-linear/HDR mathematical operators and tests. Do not claim Photoshop matching until defined.

Prefer small, focused PRs and independent review of the mask-specific design before implementing contested semantics.

## 6. Deliberately out of scope for this draft

Do not infer approval for new persistence formats, a second masking graph, Mask-as-alpha, layer-style canvas composites, automatic spatial mask reprojection, a separate mask render pipeline, an extensive masking panel, AI implementation, precise qualifier/brush algorithms, or silently treating existing Step 5 open questions as settled.

---

**Next conversation question:** Choose the semantics of generic Opacity and the generic Effect Mask **on Mask outputs** using concrete cases: standalone Radial → Exposure, Radial → Qualifier → Exposure, Mask Invert/Blur, and Mask Combine Add/Intersect. Distinguish reducing a generator's *coverage* from bypassing its *restriction* before asking Codex to code it.
