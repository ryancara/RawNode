# RawNode Sidecar Design

This document defines the non-destructive per-image edit state used by RawNode.

## Principles

- Sidecars are the source of truth for edits.
- Original image files are never modified.
- No catalogue or project file is required to reconstruct an edit.
- Sidecars are human-readable and versioned.
- Node instance identity survives reordering and application restarts.
- Unavailable processors and unknown parameter values should survive load-save cycles rather than being silently deleted.

## Sidecar V2

Input sidecars are written beside the source image as:

```text
DSC_0001.NEF.rawnode.json
```

The current V2 shape is:

```json
{
  "format": "rawnode-sidecar",
  "version": 2,
  "kind": "input",
  "source": "DSC_0001.NEF",
  "inputColorSpace": "Linear Rec.2020",
  "workingSpace": "Linear Rec.2020",
  "raw": {},
  "gui": {
    "outputIndex": 0,
    "exportFormat": 1,
    "jpegQuality": 92,
    "previewRes": 1,
    "themeIndex": 2,
    "showLeft": true,
    "showRight": true,
    "showFilmstrip": true
  },
  "graph": {
    "selectedNodeId": "node-1",
    "nodes": [
      {
        "id": "node-1",
        "backend": "ofx",
        "identifier": "com.example.plugin",
        "label": "Example Plugin",
        "enabled": true,
        "ui": {
          "groupOpen": {}
        },
        "params": {
          "amount": 0.45
        }
      }
    ]
  }
}
```

### Serial order and future graph connections

In Sidecar V2, the order of `graph.nodes` is the authoritative processing order because the current renderer is serial.

Explicit graph connections are deliberately not written yet. They should be added as an additive schema field when RawNode can actually execute graph routing. Stable node IDs provide the identity needed for that later step.

## Node identity

`id` identifies one node instance and is separate from the processor identifier.

For example, two instances of the same plugin may have different IDs:

```text
node-1 -> com.example.exposure
node-2 -> com.example.exposure
```

The ID is restored from Sidecar V2 and remains attached to the node through reordering.

## Processor identity

Each node stores:

- `backend`, such as `ofx`, `ctl`, `dctl`, or `native`;
- `identifier`, the backend-specific processor/plugin/script identity;
- `label`, a human-readable fallback;
- `enabled`;
- parameter values.

The schema uses strings for backend identity so future backend names can still be read and preserved by older builds.

## Missing processors

If a processor is unavailable, RawNode keeps the node as a missing placeholder rather than deleting it.

The placeholder preserves:

- node ID;
- backend;
- identifier;
- label;
- enabled state;
- group UI state;
- raw parameter JSON.

Missing processors are bypassed during rendering. If the processor becomes available again in a future session/build, the saved state remains available for restoration.

## Unknown parameters

Loaded parameter JSON is retained on the node even when the installed processor version does not expose that parameter.

When saving again:

1. preserved unknown parameter values are copied forward;
2. parameters known to the live processor overwrite their corresponding saved values.

This lets removed, future, or currently unsupported parameter values survive a round trip where possible.

RawNode does not yet guarantee preservation of every unknown top-level or unknown node-level metadata field. That can be expanded additively if future schema versions require it.

## RAW state

The V2 schema includes a `raw` object. It is currently empty because RawNode does not yet expose adjustable RAW-development settings separately from the inherited LibRaw path.

Future RAW decoder/developer settings should be added here without changing the graph node model.

## GUI state

V2 currently retains the inherited `gui` block for behaviour compatibility.

Essential edit reconstruction should not depend on layout/theme state. Folder-level UI state may move fully into the optional workspace state as RawNode evolves.

## V1 migration

RawNode still reads inherited sidecars named:

```text
DSC_0001.NEF.ofxrawhost.json
```

V1 nodes are normalised as `backend: "ofx"`. They receive stable node IDs when loaded and are written in Sidecar V2 format the next time the image is saved.

When both files exist, RawNode prefers the V2 `.rawnode.json` sidecar.

## Export sidecars

Export metadata uses the same V2 graph representation and is written as:

```text
export-name.rawnode.json
```

The export sidecar also records the source path and export timestamp.

## Versioning

Schema changes must increment or deliberately extend the versioned format.

Prefer additive fields where possible. Older versions should never silently discard unavailable processors or parameter state merely because they cannot execute it.

## Folder-level state

A separate optional workspace file may store non-essential UI/session state such as:

- active image;
- filmstrip position;
- sort order;
- panel visibility/layout.

That file must never be required to reproduce an image edit.
