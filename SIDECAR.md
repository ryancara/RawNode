# RawNode Sidecar Design

This document defines the non-destructive per-image edit state used by RawNode.

## Principles

- Sidecars are the source of truth for edits.
- Original image files are never modified.
- No catalogue or project file is required to reconstruct an edit.
- Sidecars are human-readable and versioned.
- Node instance identity survives reordering and application restarts.
- Unavailable processors and unknown parameter values should survive load-save cycles rather than being silently deleted.
- RAW initial working space is edit state and is stored per image when explicitly available.

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
  "raw": {
    "workingSpace": "Linear Rec.2020",
    "colorSpace": "rec2020",
    "gamma": "linear"
  },
  "gui": {
    "outputIndex": 0,
    "outputColorSpace": "rec709",
    "outputGamma": "srgb",
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

### RAW working encoding

RAW colour space (primaries/gamut) and gamma/transfer function are persisted independently:

- `raw.colorSpace`
- `raw.gamma`

Current colour-space choices are Rec.709, Rec.2020, Display P3, ACES AP0, ACES AP1, and DaVinci Wide Gamut. Current gamma choices are Linear, sRGB, Rec.709 (camera), and DaVinci Intermediate.

New writes use stable IDs such as `rec2020`, `aces-ap1`, `display-p3`, `linear`, and `davinci-intermediate` rather than UI list positions or display labels. Historical display names and aliases remain accepted when reading older sidecars.

`raw.workingSpace` is retained as a descriptive/legacy combined field so PR #16 sidecars remain readable. When a sidecar has the newer `raw.colorSpace` and `raw.gamma` fields, those are authoritative.

Older V2 sidecars that contain only `raw.workingSpace` map the three historical choices to Rec.709 + Linear, Rec.2020 + Linear, or ACES AP0 + Linear. V2/V1 RAW sidecars that predate selectable RAW working space still reopen as Rec.709 + Linear, preserving their historical colour-boundary behaviour.

The top-level `inputColorSpace` / `workingSpace` fields remain descriptive.

If an explicit RAW colour-space or gamma ID is present but is not recognised by the current build, RawNode may use a safe fallback for display/decoding but write-protects the sidecar so the unknown value is not destroyed.

### Output encoding

The Output tag is represented by an independent colour-space + gamma pair:

- `gui.outputColorSpace`
- `gui.outputGamma`

These fields use the same stable IDs and shared colour registry as RAW and the native CST. The legacy numeric `gui.outputIndex` is still written/read as a migration fallback for older Sidecar V2 files, but it is no longer the active colour model.

RAW session defaults are **not** per-image edit state. They are stored only in the optional workspace file as `gui.rawDefaultColorSpace` and `gui.rawDefaultGamma`. Per-image sidecars neither write nor apply those fields. Development sidecars that already contain recognised values are ignored; unknown future values are protected from destructive rewrite.

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
2. parameters known to the live processor overwrite their corresponding saved values;
3. choice parameters that expose stable choice IDs are saved by ID rather than menu position;
4. if a newer build wrote a choice ID that the current build does not recognise, the opaque saved ID is preserved instead of being replaced by the current fallback/default.

This lets removed, future, or currently unsupported parameter values survive a round trip where possible. Early PR #17 CST sidecars that stored numeric choice positions are still accepted for compatibility.

RawNode does not yet guarantee preservation of every unknown top-level or unknown node-level metadata field. That can be expanded additively if future schema versions require it.

## RAW state

The V2 `raw` object currently stores the initial RAW colour encoding through `colorSpace` and `gamma`, plus the legacy/descriptive `workingSpace` field.

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

RawNode ICC profiles include stable colour-encoding IDs so supported wide-gamut exports can be identified on re-import. DaVinci Intermediate is scene-referred and can map encoded values to linear values above 1.0, which a conventional matrix/TRC ICC profile cannot fully represent. RawNode therefore warns on DI export that external ICC-managed applications may clip highlights.

## Invalid and newer sidecars

If a V2 sidecar exists but cannot be parsed safely, RawNode clears the inherited on-screen chain for that image and blocks automatic sidecar writes to the file. This prevents edits from the previously viewed image from overwriting a damaged sidecar.

A `rawnode-sidecar` with a version newer than this build understands is handled the same way. RawNode does not reinterpret or downgrade future schema versions.

Recognised V2 sidecars containing an unknown future RAW or Output colour-space/gamma ID are also protected from overwrite. Unknown stable processor choice IDs (for example a future CST gamma) also block sidecar writes until the user deliberately replaces the unsupported choice.

The write protection clears when the sidecar is fixed/removed and the image is reopened.

### Older-build compatibility

Sidecars written by this version may contain colour-space/gamma IDs and combined working-space descriptions that PR #16 and earlier builds do not understand. Those older builds can fall back to their historical Rec.709/Linear interpretation and may overwrite newer colour settings if they save the file. Avoid editing newly written sidecars with an older RawNode build.

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
