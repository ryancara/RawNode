# RawNode Architectural Decisions

This is a lightweight decision log. Add significant choices here so later contributors and AI assistants understand why the project is structured this way.

## D001 — Folder-based workflow, no catalogue

**Status:** Accepted

RawNode uses folders as workspaces and sidecars as edit state. It will not require a catalogue/database or project file.

**Reason:** Keeps the application portable, transparent, lightweight, and close to ART/Lightroom Desktop-style folder workflows.

## D002 — Sidecars are authoritative edit state

**Status:** Accepted

Per-image settings live beside the image in versioned sidecars. Folder-level state may contain UI preferences only.

## D003 — Generic processors, not OFX-specific nodes

**Status:** Accepted

The inherited OFX-specific node model will be refactored behind a generic processor interface.

Initial processor backends are expected to be OFX, CTL, and DCTL-compatible processing.

## D004 — Graph-capable core with graph/list views

**Status:** Accepted

The internal processing representation should be graph-capable, while early versions may enforce a serial chain.

Graph and list views should display the same underlying processing structure.

**Reason:** Keeps the initial workflow simple while preserving room for future masks, additional inputs, or branching without redesigning the core model.

## D005 — Keep Dear ImGui + GLFW for now

**Status:** Accepted

Do not rewrite the UI in Qt unless a concrete limitation appears.

**Reason:** The existing code already uses ImGui/GLFW, dynamic parameter UIs fit immediate-mode rendering well, and retaining it supports the lightweight goal.

## D006 — Keep C++ as the application language

**Status:** Accepted

Rust may be introduced selectively where it provides a specific benefit, for example a Rawler bridge, but there is no current reason to rewrite the application around Rust.

## D007 — RAW decoder must be replaceable

**Status:** Accepted

Keep LibRaw initially, but define a boundary that can later support Rawler, RawSpeed, or another decoder.

RAW decoding and RAW development should be conceptually separate.

## D008 — Minimal RAW stage

**Status:** Accepted

The RAW stage should primarily perform sensor/RAW-specific work and produce scene-linear working RGB.

Ordinary photographic adjustments such as exposure and contrast should generally be movable processors after that boundary.

## D009 — Masks/local adjustments are future requirements

**Status:** Accepted

Masks, local adjustments, and AI-assisted masking are expected future features. AI should initially generate masks rather than define a separate processing architecture.

## D010 — No cloud/DAM/project system

**Status:** Accepted

Cloud syncing, DAM functionality, mandatory imports/libraries, and project files are explicit non-goals.

## D011 — Assignable controls target selected node instances

**Status:** Accepted

Generic parameter shortcuts should normally operate on the selected node instance. This avoids ambiguity when duplicate instances of the same processor exist.

Future keyboard, mouse, MIDI, OSC/TouchOSC, and hardware input should share the same generic parameter-control layer.

## D012 — Prefer reversible staged refactors

**Status:** Accepted

Do not introduce CTL, DCTL, alternate RAW decoding, masks, and a processing-core rewrite simultaneously. Prove architectural seams one at a time and preserve existing behaviour during foundational refactors.

## D013 — Prefer upstream-friendly infrastructure

**Status:** Accepted

Where practical, foundational refactors should remain useful to OFX Raw Host independently of RawNode-specific features.

**Reason:** This makes it easier to contribute broadly useful work back upstream and easier for RawNode to absorb future upstream changes. RawNode-specific features may still build independently on top of those shared seams.

## D014 — Reserve graph/node extension points before enabling them

**Status:** Accepted

The node data model may reserve neutral extension points for future multiple image inputs, mask inputs, opacity/compositing, and branching while the initial renderer remains serial.

Detailed qualifier, mask, and compositing implementations should not be designed prematurely. A qualifier should ultimately be one possible mask/matte generator rather than a special case embedded in every processor.


## D015 — Sidecar V2 is backend-neutral and preserves missing processors

**Status:** Accepted

Sidecar V2 stores stable node IDs plus string-based backend, identifier, label, enabled state, and raw parameter JSON.

Unavailable processors remain in the runtime node list as bypassed placeholders instead of being deleted. Unknown loaded parameter values are carried forward and overwritten only when a live processor exposes the same parameter ID.

**Reason:** Edit state must survive moving sidecars between systems/builds that do not have the same processors installed, and the persistence layer must not assume OFX.

