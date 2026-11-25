# Application settings and retired workspace files

RawNode has two authoritative persistence scopes: photograph sidecars and global
application settings. Folder browsing and non-recursive filmstrip scanning remain;
the active folder, photograph, selection and filmstrip position are transient.
Opening a folder auto-opens its first supported photograph without a workspace file.

Application settings live in one directory resolved at startup:

| Platform | Directory |
| --- | --- |
| macOS | `~/Library/Application Support/RawNode/` |
| Linux | `$XDG_CONFIG_HOME/RawNode/`, or `~/.config/RawNode/` |
| Windows | `%APPDATA%\RawNode\` |

`preferences.json` stores theme, Inspector/Nodes/Filmstrip/Preset Panel
visibility, fallback Inspector width/filmstrip height, preview resolution,
export format/JPEG quality, and default RAW working/Output colour pairs.
Docking and window geometry remain in ImGui's native `layout.ini`. Both paths
stay fixed for the session; changing photograph folders changes neither
preferences nor docking.

The optional `nodesView` string accepts `"list"` or `"graph"`. Missing or invalid
values use List; invalid values warn without discarding other valid preferences.
The optional `showNodes` and `showPresetPanel` booleans default to true and false,
respectively. The Preset Panel's selection and canvas pan/zoom are transient.
Photo layout coordinates remain in `grade.positions`. Default docking places
Preview above Nodes, Inspector right and Filmstrip across the bottom.
New custom/floating arrangements persist normally; historical
window-layout migration is deliberately unsupported (D051). No configuration is
automatically deleted. Retired Left Panel fields `showLeft` and `leftW` are no
longer read or written; unrelated preferences, including `showRight` (Inspector)
and `rightW`, retain their existing scope.

Open **File → Settings…** to choose or clear the root preset directory under
**Presets** and the CTL scripts directory under **Processors**. The optional
`presetDirectory` and `ctlScriptsDirectory` fields contain absolute UTF-8
directory paths. Clearing a directory omits its field on save. Missing fields
and empty strings both mean no directory is configured. Settings reports
unavailable saved paths when opened and keeps the paths available to change or
clear. Choosing a folder requires an existing directory. Cancelling the picker
keeps the prior selection.

The **Default preset** selector under **Presets** lists **None** and presets
discovered in the configured root and its subfolders. The list refreshes each
time the selector opens. The optional `defaultPreset` string stores the chosen
UTF-8 path relative to `presetDirectory`, for example
`Portraits/Warm.rawnode.json`. A missing or empty value means **None**. Changing
or clearing the configured root clears this selection. If RawNode cannot read
the selected file as a preset, Settings shows the saved selection and the read
error so the user can change or clear it.

Preferences load before UI initialisation and the first photo opening, and save
on normal shutdown even without an opened folder. A cancelled photo-save departure
keeps the editor and renderer usable and does not finish shutdown. Directory and
file errors produce warnings and in-memory defaults; there is no CWD, photograph
folder or installation-directory fallback. Invalid/unreadable or newer-format
preferences are left untouched for that session. Preferences use the shared
[atomic writer](DECISIONS.md#d058--shared-atomic-persistence) and layout uses
ImGui's native ini writes.

The format is a small JSON object with `format: "rawnode-preferences"`, `version: 1`.
All settings are optional. Missing fields receive built-in defaults independently;
invalid individual fields warn and default without losing other valid settings.
Unknown optional fields do not invalidate a supported file, but are not currently
preserved when that file is saved again. Unsupported future file versions are
protected from overwriting. Future implemented application preferences
can add optional fields with sensible defaults without replacing this persistence
system.

The optional `keyboardBindings` array records the effective assignments. Each
entry contains a named `key`, a list of named `modifiers` and an ordered list
of `commands` that currently contains one command ID. For example,
`{"key":"Z","modifiers":["primary"],"commands":["undo"]}` uses Command on macOS
and Control on Windows and Linux. Omit the field for platform defaults. An empty
array clears all keyboard bindings. The first valid assignment for a chord wins
when a file contains duplicates. Invalid entries produce a preferences warning
and valid entries still load.

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
Valid stored pairs initialise new grades when there is no selected default
preset. Saved grades and presets applied with Replace supply their own colours.
Photo adjustments and folder switching preserve the global defaults.
Default-colour Settings UI remains later work.

`workspace.ofxrawhost.json`, per-folder `.ofxrawhost-layout.ini` and CWD
`ofxrawhost.ini` are retired. RawNode does not read, rewrite, migrate, create or
automatically delete them. There is no replacement workspace/catalogue JSON.
See [SIDECAR.md](../SIDECAR.md) for current source-sidecar and preset behaviour.

---

## Historical workspace and V1 sidecar documentation

The following describes older ofxrawhost builds only. None of its workspace or
layout paths is active in current RawNode. Historical V1/V2 sidecar helpers remain
only for self-test fixtures and compatibility assertions, outside the live editor.
All reader/writer descriptions below refer to those historical builds, not to
current photo opening or global preferences. The current graph-native schema
is specified in [STEP7_PERSISTENCE_DESIGN.md](STEP7_PERSISTENCE_DESIGN.md).


OFX Raw Host persists reproducible processing state as JSON beside your files.

## Historical workspace file

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

## Historical V1 input image sidecar

For each source file `photo.cr2`, settings are stored in:

`photo.cr2.ofxrawhost.json`

Same basename as the image, with `.ofxrawhost.json` appended (works for any extension).

## Exported derivatives

RawNode sidecars belong to editable source documents only. Exported images do
not receive sidecar JSON; export writes the rendered derivative without changing
the source sidecar. Legacy export sidecars may remain on disk but are not used
by export. See [SIDECAR.md](../SIDECAR.md) for the current source-sidecar format.

## Historical V1 sidecar document shape

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

## Historical V1 versioning

Historical V1 builds incremented `version` for incompatible changes, ignored
unknown keys and filled missing fields with defaults. This was the retired V1
reader's policy. The current graph-native reader strictly rejects unsupported
structure; see [the current schema](STEP7_PERSISTENCE_DESIGN.md#2-file-shape).
