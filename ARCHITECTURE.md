# RawNode Architecture

This document records the intended architecture at a high level. It should describe stable boundaries rather than implementation trivia.

## Core pipeline

```text
Folder / Filmstrip
       |
       v
   Source Image
       |
       v
Minimal RAW Development (for RAW sources)
       |
       v
Scene-linear Working RGB
       |
       v
Processing Graph
       |
       v
Display / Export
```

## Generic processor model

A processing node must not inherently be an OFX plugin.

Conceptually:

```text
Processor
├── OFXProcessor
├── Native processors
├── CTLProcessor
├── DCTLProcessor
└── future processor types
```

Each processor should expose a common interface for:

- stable identity;
- parameter enumeration;
- parameter get/set;
- image processing;
- bypass/enabled state;
- serialization metadata.

The UI, shortcut system, and sidecar layer should interact with this generic interface rather than backend-specific APIs.

## Generic parameter model

All processor types should map their controls to one generic parameter representation.

Typical fields:

```text
Parameter
    id
    label
    type
    value
    default
    min
    max
    step
    choices
```

This shared representation should drive:

- ImGui widgets;
- keyboard/mouse shortcuts;
- future MIDI/OSC/hardware control;
- sidecar persistence.

## Node identity

Every node instance should ultimately have a persistent unique instance ID separate from the processor/plugin identifier.

Sidecar V2 persists this instance ID across sessions. New nodes receive an ID when created; loaded nodes retain the ID stored in their sidecar.

Example:

```text
node-001 -> Exposure.dctl
node-002 -> Contrast.dctl
node-003 -> Exposure.dctl
```

This allows duplicate processors, reordering, selection, and shortcut targeting without ambiguity.

## Graph and list views

Internally, processing should be represented in a graph-capable structure.

Initial releases may enforce a simple serial chain. The UI may present that structure as either:

- a graph view; or
- a list view.

List view must be a representation of the same underlying processing model, not a separate implementation.

Branching/merging can remain disabled until a real use case requires it.

## Masks and local adjustments

Future nodes may support masks and opacity. The architecture should allow a node to gain additional inputs later without changing its basic identity/parameter model.

Potential future structure:

```text
Node
    persistent instance ID
    processor
    parameters
    enabled
    image input(s)
    mask/matte input(s)
    opacity
    composite mode
```

The first generic-node refactor may reserve these neutral fields while leaving them inactive. The serial renderer remains the source of truth until graph routing is deliberately implemented. Detailed qualifier/mask implementations should come later; qualifiers should be able to act as mask generators rather than requiring a special processing architecture.

Mask generation should remain separable from image adjustment. AI features should initially generate masks rather than become a separate image-processing architecture.

## RAW architecture

Keep RAW decoding and RAW development conceptually separate.

```text
IRawDecoder
    -> RawFrame / metadata

IRawDeveloper
    -> scene-linear working RGB
```

The initial decoder can remain LibRaw. Future candidates such as Rawler or RawSpeed should be able to sit behind the same boundary.

The RAW development stage should stay minimal and should primarily handle tasks that belong close to sensor data, such as:

- decode/decompression;
- black/white level handling;
- sensor corrections;
- highlight reconstruction;
- initial white-balance interpretation;
- demosaic;
- camera colour conversion to the working space.

Ordinary photographic adjustments such as exposure, contrast, saturation, curves, creative white balance, split toning, etc. should generally live in the processing graph.

## Rendering

The renderer should not assume every node is OFX.

Target architecture:

```text
RenderPipeline
    |
    +--> Node/Processor interface
             |
             +--> OFX
             +--> Native
             +--> CTL
             +--> DCTL
```

Backend-specific GPU support should remain encapsulated. Do not make the whole application dependent on a single platform API such as Metal.

### CTL backend

CTL is treated as a standalone processor backend, not as an ART-specific format. The core CTL processor executes standard CTL through the reference interpreter and uses the conventional varying float RGB(A) `main` interface.

Host-specific conventions belong in adapters above that backend. ART compatibility may later add support for `ART_main`, ART metadata comments, and ART helper libraries without changing the standard CTL execution model.

The initial Sidecar V2 identifier for a CTL processor is the canonical script path. If the script is unavailable on another system, the normal missing-processor placeholder preserves the node.

## UI

Retain Dear ImGui + GLFW unless a concrete blocker appears.

The UI should consume generic nodes and generic parameters. It should not contain OFX-specific logic except where required for backend-specific diagnostic information.

## Input architecture

Input bindings should target generic parameters on the currently selected node instance by default.

Potential future input sources:

- keyboard;
- mouse wheel/drag;
- MIDI;
- OSC/TouchOSC;
- dedicated hardware controllers.

The input system must not know whether the target parameter belongs to OFX, CTL, or DCTL.

## Persistence

Per-image edit state belongs in sidecars. Folder-level UI state may be stored separately but must not be required to reproduce an edit.

Unknown node types/parameters should be preserved where possible rather than silently discarded.

## Modularity rule

Prefer adapters and interfaces over direct coupling whenever a subsystem is likely to change later.

Particularly important boundaries:

- RAW decoder;
- RAW developer;
- processor backend;
- generic parameters;
- render pipeline;
- persistence;
- input bindings;
- UI presentation.
