# Application settings and retired workspace files

RawNode has two authoritative persistence scopes: photograph sidecars and global
application settings. Folder browsing and recursive filmstrip scanning remain;
the active folder, photograph, selection and filmstrip position are transient.
Opening a folder auto-opens its first supported photograph without a workspace file.

Application settings live in one directory resolved at startup:

| Platform | Directory |
| --- | --- |
| macOS | `~/Library/Application Support/RawNode/` |
| Linux | `$XDG_CONFIG_HOME/RawNode/`, or `~/.config/RawNode/` |
| Windows, when supported | Roaming application-data `RawNode` directory |

`preferences.json` stores theme, left/right panel and filmstrip visibility,
fallback panel widths/filmstrip height, preview resolution, export format/JPEG
quality, and default RAW working/Output colour pairs. Docking and window geometry
remain in ImGui's native `layout.ini`. Both paths stay fixed for the session;
changing photograph folders changes neither preferences nor docking.

Preferences load before UI initialisation and the first photo opening, and save
on normal shutdown even without an opened folder. A cancelled photo-save departure
keeps the editor and renderer usable and does not finish shutdown. Directory and
file errors produce warnings and in-memory defaults; there is no CWD, photograph
folder or installation-directory fallback. Invalid/unreadable or newer-format
preferences are left untouched for that session. The checked atomic writer protects
previous preference bytes on failure; ImGui's own ini writes are not atomic.
Windows atomic file replacement is not yet supported and reports a warning.

The format is a small JSON object with `format: "rawnode-preferences"`, `version: 1`.
All settings are optional. Missing fields receive built-in defaults independently;
invalid individual fields warn and default without losing other valid settings.
Unknown optional fields are ignored. Future implemented application preferences
can add optional fields with sensible defaults without replacing this persistence
system; no shortcut, MIDI, plugin-path or preset-path schema is defined here.

Both colour pairs use stable IDs, for example:

```json
{
  "format": "rawnode-preferences",
  "version": 1,
  "rawWorking": {"colorSpace": "rec2020", "gamma": "linear"},
  "output": {"colorSpace": "rec709", "gamma": "srgb"}
}
```

Missing pairs use RAW Rec.2020/Linear and Output Rec.709/sRGB. Malformed,
incomplete or unknown pairs warn and use that pair's own built-in fallback.
Valid stored pairs initialise new grades. Saved grades and preset Replace retain
their own photo colours; photo adjustments and folder switching never change
global defaults. Default-colour Settings UI and default presets remain later work.

`workspace.ofxrawhost.json`, per-folder `.ofxrawhost-layout.ini` and CWD
`ofxrawhost.ini` are retired. RawNode does not read, rewrite, migrate, create or
automatically delete them. There is no replacement workspace/catalogue JSON.
See [SIDECAR.md](../SIDECAR.md) for current source-sidecar and preset behaviour.

---

## Historical workspace and V1 sidecar documentation

The following describes older ofxrawhost builds only. None of its workspace or
layout paths is active in current RawNode. Historical V1/V2 sidecar helpers remain
only for self-test fixtures and compatibility assertions, outside the live editor.


OFX Raw Host persists reproducible processing state as JSON beside your files.

## Workspace file

When you **Open Workspace…** on a folder, the host reads and writes:

`{folder}/workspace.ofxrawhost.json`

This file captures folder-level UI state and which image is active. Per-image OFX chains live in sidecars (below), not in the workspace file.

Dock panel sizes and splits are stored separately in ImGui’s layout file:

`{folder}/.ofxrawhost-layout.ini`

(without a workspace, the app uses `ofxrawhost.ini` in the working directory).

```json
{
  "format": "ofxrawhost-workspace",
  "version": 1,
  "activeImage": "subfolder/DSC_0001.cr2",
  "gui": {
    "outputIndex": 0,
    "exportFormat": 1,
    "jpegQuality": 92,
    "previewRes": 1,
    "themeIndex": 2,
    "showLeft": true,
    "showRight": true,
    "showFilmstrip": true
  }
}
```

Paths in `activeImage` are relative to the workspace directory when possible.

### Legacy layout fields

Older files may still contain `leftW`, `rightW`, and `filmstripH` under `gui`. The host still **reads** them to seed a default DockSpace layout when no `.ofxrawhost-layout.ini` exists. They are no longer written on save; ImGui’s `.ini` owns panel geometry.

## Input image sidecar

For each source file `photo.cr2`, settings are stored in:

`photo.cr2.ofxrawhost.json`

Same basename as the image, with `.ofxrawhost.json` appended (works for any extension).

## Exported derivatives

RawNode sidecars belong to editable source documents only. Exported images do
not receive sidecar JSON; export writes the rendered derivative without changing
the source sidecar. Legacy export sidecars may remain on disk but are not used
by export. See [SIDECAR.md](../SIDECAR.md) for the current source-sidecar format.

## Sidecar document shape

The legacy input-sidecar document shape is:

```json
{
  "format": "ofxrawhost-sidecar",
  "version": 1,
  "kind": "input",
  "sourcePath": "DSC_0001.cr2",
  "inputColorSpace": "Linear Rec.2020",
  "gui": { "...": "same keys as workspace gui block" },
  "chain": {
    "selectedNode": 0,
    "nodes": [
      {
        "pluginIdentifier": "com.example.filter",
        "pluginLabel": "Example Filter",
        "enabled": true,
        "groupOpen": { "Grain": true },
        "params": {
          "amount": 0.5,
          "mode": 2,
          "labelParam": "text",
          "toggle": true
        }
      }
    ]
  }
}
```

- `kind`: `"input"` for editable source-image sidecars.
- `params`: keyed by OFX parameter **name** (stable). Values are numbers, booleans, or strings depending on type; multi-dimensional params are JSON arrays.
- `pluginIdentifier`: used to rebind plugins if install order changes; `pluginLabel` is informational.

## Versioning

`version` is incremented when incompatible changes are made. The host ignores unknown keys and fills missing fields with defaults.
