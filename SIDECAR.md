# RawNode Sidecar Design

This document defines the direction for non-destructive per-image edit state.

## Principles

- Sidecars are the source of truth for edits.
- Original image files are never modified.
- No catalogue or project file is required to reconstruct an edit.
- Sidecars should be human-readable and versioned.
- Unknown fields and unavailable processors should be preserved where possible.
- Node instance identity must survive reordering.

## Proposed structure

The exact schema is not final, but the target shape is approximately:

```json
{
  "format": "rawnode-sidecar",
  "version": 2,
  "source": "DSC_0001.NEF",
  "workingSpace": "ACEScg",
  "raw": {
    "decoder": "libraw",
    "whiteBalance": "camera",
    "highlightRecovery": "default"
  },
  "graph": {
    "nodes": [
      {
        "id": "node-001",
        "type": "dctl",
        "identifier": "Exposure.dctl",
        "enabled": true,
        "params": {
          "exposure": 0.45
        }
      },
      {
        "id": "node-002",
        "type": "ofx",
        "identifier": "com.example.plugin",
        "enabled": true,
        "params": {}
      }
    ],
    "connections": [
      ["raw", "node-001"],
      ["node-001", "node-002"],
      ["node-002", "output"]
    ]
  }
}
```

For early serial-only versions, connections may be derived from order, but the schema should not prevent explicit connections later.

## Node identity

A node instance ID is distinct from its processor identifier.

This allows multiple instances of the same plugin/script without ambiguity.

## Missing processors

If a processor is unavailable on another system, keep the node and all parameter values in the sidecar. The UI should show it as missing/unavailable rather than deleting or flattening it.

## Versioning

Schema changes must be versioned. Prefer additive changes where possible.

Readers should ignore unknown keys and preserve them when feasible.

## Folder-level state

A separate optional hidden file may store non-essential UI/workspace state, such as:

- active image;
- filmstrip position;
- sort order;
- panel visibility/layout.

That file must never be required to reproduce an image edit.
