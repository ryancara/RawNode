# Step 8 Masking Contract

**Status:** Owner-approved Step 8 product/architecture direction, 2026-10-26. **Not yet implemented.**
**Authority:** D054 in `docs/DECISIONS.md`. This narrows and explicitly supersedes the Step 5 numerical Mask-generator bypass rule in D042; other Step 5 graph and availability guarantees remain.
**Source of discussion:** The historical, non-authoritative `docs/STEP8_MASKING_DISCUSSION_DRAFT.md` on branch `docs/step8-masking-design-notes-20261025`, followed by independent Claude/Codex reviews and owner reconciliation.

## Scope and architectural boundaries

- One existing graph, evaluator, render runtime, document mutation/history, sidecar/grade codec and preview/export ownership. Do not add a separate mask graph/scheduler or per-backend masking infrastructure.
- Image is the existing floating-point RGBA raster. Mask is the existing single-channel, colour-independent scalar coverage raster in [0,1], separate from Image alpha.
- Generic graph-level controls are not implemented independently by native, OFX, CTL or future DCTL backends.
- Node output types and semantic pass-through input ports must be declared by the node interface; do not guess from connection order, rendered RGB values, or a backend whitelist.
- This document approves semantics, not the creation of new production mask algorithms or unrestricted generic controls on every node.

## Bypass and intentional inactivity

Bypass means **do not perform this node's operation**. Bypass does not erase the node or its stored connections, and every stored edge still participates in structural validation.

- Unary Image -> Image: return its declared Image pass-through input unchanged (already implemented).
- Unary Mask -> Mask, e.g. Invert or Blur: return its declared Mask pass-through input unchanged, including its existing Inactive or Unavailable state.
- Mask generator with no same-type pass-through input, e.g. Radial, Linear Gradient or Image -> Mask Qualifier: return **Inactive Mask**, with no raster. Do not evaluate its generation-only Image/Reference dependencies on bypass.
- Mask Combine: while bypassed return its designated A input unchanged, even if A is Inactive; B and the combination operation are not evaluated. This is distinct from enabled Combine with an inactive A.
- For a node with both Image and Mask outputs, determine bypass **per output**: a declared same-type pass-through yields that input; a Mask output with no such input becomes Inactive. Test-only multiple-output nodes must preserve Image pass-through while producing Inactive for a non-pass-through Mask output.
- Source and future output types without a meaningful pass-through remain explicit operation contracts; do not invent universal 'zero output' for Image producers.

**Inactive** means intentional absence of a Mask contribution. It is neither black (0) nor white (1), has no image dimensions, and is not an alpha channel. Extend the existing per-output evaluation outcome with a Mask-only Inactive state, without manufacturing dummy raster data. **Only the evaluator's declared Mask bypass/propagation logic may create Inactive. An Image-typed output must never be Inactive, and an executor/processor returning Inactive is an evaluation Error.** Inactive is derived at evaluation time and is never saved as a separate field; existing enabled/bypass state remains the persisted user edit.

States must remain distinct:
- Available: valid typed Image/Mask raster; a fully black Mask is still Available data.
- Inactive: intentionally bypassed or propagated absence of Mask coverage, no raster, no error/warning solely for inactivity.
- Unavailable: required data or implementation could not be obtained; retain identity, connection and diagnostics.
- Error: invalid topology or execution/numerical failure.
- Cancelled: superseded/cancelled work.

An absent/unconnected optional coverage Mask (including an Image-node Effect Mask or future layer-slot Mask) or an **Inactive** connected coverage Mask gives *full coverage* without an inactivity warning. An **Unavailable** connected optional coverage Mask also fails open to full coverage, **with a persistent warning** (D042 unchanged). A present Mask with incompatible dimensions remains an error; Inactive has no dimensions and does not trigger compatibility checks.

Unary Mask processors propagate Inactive from required Mask data without executing pixels (Invert(Inactive) = Inactive, Blur(Inactive) = Inactive). An Unavailable required Mask remains Unavailable. An unconnected required input remains incomplete/error when required for execution and is not silently treated as Inactive. An enabled missing third-party Mask modifier/producer is Unavailable unless a future, separately reviewed operation-specific fallback is explicitly defined; backend failure is never intentional Inactive. Bypass is evaluated **before** backend availability for any declared Mask bypass result: a bypassed missing Mask generator is Inactive with no evaluation warning, even if its Reference cannot resolve; a bypassed missing Mask modifier returns its same-type Mask pass-through. The graph UI may still indicate the missing processor itself.

All planning and execution must agree about bypass so only bypass dependencies are evaluated, while all stored connections/cycles are still structurally validated. Both Image and Mask endpoints use the existing evaluator and runtime; inspection of an Inactive Mask has no raster, shows a non-error 'Mask inactive (bypassed)' state when user-facing status is appropriate, and clears stale Mask Only/Overlay imagery. **A quiet preview restoration must not publish an Inactive status over a meaningful export/result status** (D025); ordinary inactive inspection must not be labelled 'Render failed'. Full-resolution export of an Image with an inactive optional Mask follows ordinary unmasked Image processing.

## Opacity / Strength

When a node has a **defined same-type pass-through raster** and an applicable unary operation, generic Opacity interpolates between that original pass-through value and its processed output. 0% produces the bypass raster exactly; 100% with Normal blend produces the processed raster exactly. The evaluator owns this operation, not the processor backend. Zero should be handled as an exact endpoint, without evaluating unnecessary processor work; partial raster mixing still requires compatible dimensions. **This is an explicit D054 amendment to D043 and the historical Step 5 self-mix dimension rule**: at exactly 0% Strength, bypass without processor execution or an Image/Mask pixelwise compatibility check; a positive partial mix still requires compatible dimensions. Dependency planning and execution must read the same Strength value per evaluation if this skips dependencies.

- Image -> Image processors: generic Opacity.
- Unary Mask -> Mask modifiers (Invert, Blur): generic Opacity when real pass-through Mask data exists; an Inactive input stays Inactive.
- Input-free or Image -> Mask generators: **no generic Opacity**. Their bypass value is nonnumeric Inactive, so blending towards it is undefined.
- Binary Mask Combine: **no generic Opacity initially**, despite having a designated bypass port. A may be Inactive while B is an active raster, making partial interpolation undefined.
- Do not use physical cable presence to determine whether the UI displays Opacity. Applicability is declared by the node operation contract, not by current connections or a hardcoded backend list.

No Mask Gain/density feature in the initial implementation. Blend Mode on Image processors is separate from Mask Combine's operation; Normal Image blend first. Do not silently expose no-op RGB blend controls on Mask generators, or invent extra generic Effect Mask connections on them. Future expanded generic Mask-node operation masking must be designed separately from functional Mask data inputs.

## Mask generation, functional inputs and combination

- Geometric generators output Mask coverage in a defined Reference raster; Qualifier and future adapted third-party producers may analyse Image data but output declared typed Mask coverage.
- The first design uses a **dedicated binary Mask Combine** node; there is no automatic generator-to-generator intersection or automatic growth of generic Mask input sockets. Mask modifiers have explicit functional Mask inputs. Generic optional Effect Mask on an Image adjustment has distinct coverage semantics.
- Enabled Combine has semantic A/B Mask inputs and an operation (first target: Add/Union, Intersect, Subtract B from A). All active input rasters must match dimensions. Exact fractional equations remain a separate product decision before that implementation.
- **Enabled Combine evaluation precedence:** Error or Cancelled in a required evaluated operand propagates first; next, any required **Unavailable** operand **must** propagate Unavailable with its diagnostic. Only when both operands are Available or Inactive does the following participation rule apply.
- **One operand participation rule for Available/Inactive inputs:** both Available => calculate the chosen operation; only A Available => return A; only B Available => return B; both Inactive => Inactive. This intentionally applies even to Subtract when A is Inactive: returning B is the accepted user-controlled trade-off, **not** an accidental algebraic identity. Inactive is deliberate; missing data is not. Do not conflate a disconnected required port with a connected source evaluating Inactive. Required connections can be staged/disconnected during editing but may not successfully evaluate when required.
- A bypassed Combine returns designated A even if B is active, because bypass is not the same operation as enabled Combine with A inactive.

An Image-producing third-party DCTL can produce grayscale RGB without automatically becoming a Mask: conversion from Image to typed Mask must have an explicit, persisted channel/interpretation policy. Exact UX (visible converter node versus connection convenience) is deferred; do not infer Mask type merely from equal RGB samples.

## Preview and user interaction (Step 8 follow-ups)

- Selecting a Mask node leaves the normal fully processed photograph visible by default, with appropriate on-image editing handles for the selected Mask only.
- Preview toolbar offers optional coloured **Mask Overlay** and grayscale **Mask Only**, inspecting the selected Mask endpoint's actual final output; inactive shows no stale mask raster.
- Inspector and handles edit the same parameters; a completed drag is one Undo step.
- Reuse the existing requested-endpoint evaluator/runtime. An overlay requiring both final Image and Mask may require a bounded multi-endpoint publication extension, **not** another rendering system.
- No silent automatic remapping through Crop, warp, or other geometry. Equal-sized raster coordinates remain the default; hide/qualify handles when they cannot accurately correspond to the displayed photograph.

## First implementation and verification

1. **PR 1: evaluator-only Inactive Mask bypass contract.** Extend existing evaluation outcome; update dependency planning and generator bypass test path; optional coverage, unary propagation and inspection/status handling. Reuse test-only Mask operations before producing UI-facing generators. No new persistence format, new renderer or production mask processor in this PR. Confirm existing Image-node bypass unchanged.
2. **PR 2: ordinary Image-node optional Effect Mask + Opacity, Normal mixing.** Central evaluator wrapper; exact 0%/100% endpoints; deterministic Image dimensions/alpha/HDR behaviour; save, clipboard, history and preview/export equivalence. Settle OFX frame/render-scale/internal mask policy **before exposing these controls on OFX**. A temporary documented OFX UI rollout gate is acceptable if required for safety, but does not change the generic evaluator semantics or create per-backend masking/mixing implementations.
3. **PR 3: first native Radial Mask -> Exposure vertical slice.** Pin resolution-independent coordinates before persisting. Test fan-out, preview/export, source/Reference, disabled generator, save/reopen and Undo/Redo.
4. **Follow-up: Mask Only, Overlay, on-image handles** under existing runtime; inspect inactive outputs.
5. **Follow-up: binary Combine, then Invert/Blur**, after fractional maths and unavailable-input details are reviewed. Later Linear, Qualifier, Image-to-Mask, DCTL and advanced selection tools.

PR 1 acceptance should cover: disabled generator -> optional Image Effect Mask (global, no inactivity warning); disabled generator -> test-only unary Mask modifier -> Effect Mask (still Inactive); Inactive versus Available black/white and versus Unavailable (warning); bypassed generator ignores missing/unavailable Reference/analysis data and bypassed missing backend gives no evaluation warning, while invalid stored cycles still fail structural validation; shared fan-out; **test-only multiple-output nodes where Image outputs pass through and Mask outputs without pass-through become Inactive**; execution rejects producer/executor-supplied Inactive; Image results can never be Inactive; inactive inspection clears stale content and does not report failure; **quiet inactive preview restoration preserves Export complete / other meaningful status**; full self-tests and appropriate sanitizers. Tests must catch accidental restoration of the former white-bypass behaviour.

The enabled Combine A/B participation matrix, Unavailable precedence and **bypassed Combine -> A** belong to the **later Combine implementation PR**, not PR 1. PR 1 may use existing test-only Mask operations but must not add a production Combine fixture solely to expand its scope.

## Deferred, not implicit approvals

Exact fractional coverage combination equations (particularly soft masks), geometrical conventions, advanced masks/AI, mask Gain, generic Mask-output Effect Mask, auto Image-to-Mask connection conversion, HDR non-Normal Image blend maths, general alpha association and compositing, OFX RoD/render-scale/internal-mask policy, transformed handle alignment and multiple automatic Combine slots. Introduce any future capability via a focused, reviewed contract rather than assumptions in processor code.
