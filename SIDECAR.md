# RawNode Sidecar Design

The live editor uses the graph-native `rawnode` version 1 format described in
[the Step 7 persistence design](docs/STEP7_PERSISTENCE_DESIGN.md). Each successfully
opened editable photograph has a complete grade, including Source-only; opening
an existing grade does not immediately rewrite it. Metadata-only files gain a
grade while retaining metadata. Scans/thumbnails and exports create no sidecars.

Opening stages graph, source, colours and preview before replacement. Invalid,
unsupported or unreadable sidecars permit protected source viewing, never edits
or overwrite. Failed saves retain the editable document, show a persistent
warning, and offer Retry / Discard / Cancel before departure/reopen/shutdown.

Grade updates preserve metadata and use checked sibling-temp atomic replacement
on POSIX. Existing permissions survive; new files default to owner-only `0600`.
Windows atomic writes remain explicitly unsupported. Backend identities, paths
and parameter strings must be valid UTF-8 to save: invalid bytes cause a useful
error, preserving live state and original files rather than converting values.

## Presets and clipboard

**Save Preset** writes the complete editable grade to `.rawnode.json`, including
Source-only and branched graphs, RAW/Output colour pairs and optional positions.
New presets omit photo metadata. A confirmed overwrite validates the existing
file, retains its metadata and atomically replaces only the grade. Confirmation
never permits overwriting an unsupported/protected file.

**Load Preset → Replace** stages the complete graph, preserves incoming NodeIds,
binds Source to the current photo and applies both colour pairs. Required RAW
re-decoding and preview preparation complete before publication. Incoming positions
replace the old arrangement; absent positions clear it. Destination metadata and
session defaults stay unchanged. Failure keeps the original document available.

**Load Preset → Append** accepts a complete ordinary linear ImageProcessor grade.
It inserts after the selected main-spine node, or at document Output without a
selection, allocating fresh IDs and remapping connections/positions in one staged
splice. Eligible branched destinations retain every unrelated consumer/branch,
colour pair, metadata and existing position. Source-only Append does nothing.
Branched incoming attachment and stale/off-spine selections are refused atomically;
no implicit colour conversions or automatic position offsets are introduced.

Copy Node/Paste and Copy Full Grade/Paste remain separate editing operations.
Clipboard JSON uses `format: "rawnode-transfer"`, `version: 2`: `kind: "grade"`
wraps the shared `grade` object; `kind: "fragment"` carries shared `nodes` and
optional `positions`. Fragments preserve supported internal connections, processor
state and Slot identities/order/settings/allocation, excluding Source and transient
group expansion. External inputs detach; omitted external Mask/Reference inputs
are reported during copy. Copy eligibility is independent of paste eligibility.
Initially only ordinary linear fragments attach through the same batch splice as
Append. Refusal changes neither the document nor clipboard.

Copy Grade, Save Preset and application refuse protected source views. Clipboard
version 1, `rawnode-preset` and `.rawnodepreset` are retired without readers or
migration; existing files are not deleted. Historical V1/V2 sidecar helpers below
remain only for legacy self-test fixtures; folder persistence is retired.


## Global application settings

Application preferences and ImGui layout now live together in the platform user
configuration directory, as `preferences.json` and `layout.ini`. They load before
the UI/first photo and save at normal shutdown. RAW/Output defaults initialise
new grades; changing photo controls, opening saved grades or applying Replace
never changes those defaults. See [application settings](docs/workspace-format.md)
for locations, optional-field defaults and error handling.

Workspace JSON, per-folder layout and CWD ini persistence are retired without
migration or automatic deletion. Folder browsing/filmstrip auto-open remains.
Historical sections below describe older formats only; their readers/writers
are retained for self-test fixtures and do not participate in live persistence.

## Principles

- Sidecars are the source of truth for edits.
- Edits are stored in sidecars; exporting over a raster source remains a known
  overwrite hazard (see `docs/PROJECT_CONTEXT.md`).
- No catalogue or project file is required to reconstruct an edit.
- Sidecars are human-readable and versioned.
- Node instance identity survives reordering and application restarts.
- Unavailable processors and unknown parameter values should survive load-save cycles rather than being silently deleted.
- Both RAW working and Output colour pairs are required per-grade state,
  including for raster photographs.

## Exported derivatives

RawNode sidecars belong to editable source images/documents. Exported JPEG,
PNG, TIFF and other rendered derivatives do not receive RawNode sidecars.
Export consumes the evaluated document and writes the derivative; it does not
save a second editable document or modify the source sidecar.

Older RawNode versions may have created export sidecars. Those legacy files may
remain on disk; current export does not read, update, migrate or delete them.

RawNode ICC profiles include stable colour-encoding IDs so supported wide-gamut exports can be identified on re-import. DaVinci Intermediate is scene-referred and can map encoded values to linear values above 1.0, which a conventional matrix/TRC ICC profile cannot fully represent. RawNode therefore warns on DI export that external ICC-managed applications may clip highlights.

## Versioning

Current files use `format: "rawnode"`, `version: 1`, with independent optional
`grade` and `metadata` objects. Presets use the same file/grade format; clipboard
uses the transfer envelope described above. Schema changes must be explicitly
versioned; unsupported semantics protect the original file from overwrite.
Unavailable processors, opaque parameters and topology must not be silently
discarded merely because they cannot execute. See the accepted Step 7
specification for exact fields and validation rules.

## Current missing-processor evaluation

The graph-native format retains unavailable processor identity, parameters and
connections. A missing unary ImageProcessor uses the evaluator's warned
pass-through; this is not a universal fallback for unknown graph operations.
Unavailable Mask values follow the Step 5 propagation/fail-open contract. Group
expansion is transient and is excluded from current grades and transfers.

## Retired folder-level state

Older optional workspace files held active-image and interface state. These files
are now ignored and left untouched. Only source sidecars and global application
settings remain authoritative; no replacement workspace file is created.

## Historical Sidecar V2 and V1 migration

Everything below describes retired formats and their original reader behaviour.
It is retained to explain old files and self-test fixtures. Live photo opening
does not call these compatibility readers or migrate V1/V2. A V2 file at the
`.rawnode.json` path is protected; if that path is absent, a lone sibling
`.ofxrawhost.json` also protects the photo without creating a new sidecar.
Current schema and unknown-data rules are in `docs/STEP7_PERSISTENCE_DESIGN.md`.

V2 input sidecars were written beside the source image as:

```text
DSC_0001.NEF.rawnode.json
```

The historical V2 shape was:

```json
{
  "format": "rawnode-sidecar",
  "version": 2,
  "kind": "input",
  "source": "DSC_0001.NEF",
  "raw": {
    "colorSpace": "rec2020",
    "gamma": "linear"
  },
  "gui": {
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

V2 colour-space choices were Rec.709, Rec.2020, Display P3, ACES AP0, ACES AP1, and DaVinci Wide Gamut. V2 gamma choices were Linear, sRGB, Rec.709 (camera), and DaVinci Intermediate.

V2 writes used stable IDs such as `rec2020`, `aces-ap1`, `display-p3`, `linear`, and `davinci-intermediate` rather than UI list positions or display labels. The compatibility reader accepted historical display names and aliases.

`raw.workingSpace` is a read-only migration field for older PR #16-era sidecars. New files write only the independent stable IDs. Older V2 sidecars that contain only `raw.workingSpace` map the three historical choices to Rec.709 + Linear, Rec.2020 + Linear, or ACES AP0 + Linear. V2/V1 RAW sidecars that predate selectable RAW working space still reopen as Rec.709 + Linear, preserving their historical colour-boundary behaviour.

Legacy combined top-level colour strings may still be present in older development sidecars, but current files do not write or depend on them.

The historical reader used a display/decoding fallback for unrecognised RAW colour IDs and protected the sidecar from overwrite. The live editor now protects all V1/V2 formats, without interpreting their grade.

### Output encoding

The Output tag is represented by an independent colour-space + gamma pair:

- `gui.outputColorSpace`
- `gui.outputGamma`

These historical fields used the same stable IDs and shared colour registry as RAW and the native CST. Legacy `gui.outputIndex` belonged to older builds; no live workspace reader remains.

Historically, RAW defaults lived in the optional workspace file as `gui.rawDefaultColorSpace` and `gui.rawDefaultGamma`. Current defaults live only in global preferences; the live editor protects historical V1/V2 sidecars without migration.

### Serial order and graph migration

In Sidecar V2, the order of `graph.nodes` is the authoritative processing order
because V2 described the former serial renderer. Step 6 introduced explicit
in-memory topology with a temporary representability guard. Step 7 replaced V2
with graph-native persistence; node-array order now has no execution meaning.
The guard and V1/V2 migration below are historical, outside live photo opening.

### Node identity

`id` identifies one node instance and is separate from the processor identifier.

For example, two instances of the same plugin may have different IDs:

```text
node-1 -> com.example.exposure
node-2 -> com.example.exposure
```

The historical reader restored the ID from Sidecar V2 and retained it through reordering.

### Processor identity

Each node stores:

- `backend`, such as `ofx`, `ctl`, `dctl`, or `native`;
- `identifier`, the backend-specific processor/plugin/script identity;
- `label`, a human-readable fallback;
- `enabled`;
- parameter values.

The schema uses strings for backend identity so future backend names can still be read and preserved by older builds.

### Missing processors

The historical V2 loader kept an unavailable processor as a missing placeholder
rather than deleting it. Its placeholder preserved:

- node ID;
- backend;
- identifier;
- label;
- enabled state;
- group UI state;
- raw parameter JSON.

In historical V2 serial rendering, missing unary processors were bypassed while
their saved state remained available for restoration. Current graph evaluation
is described separately above; historical group UI state is not current grade data.

### Unknown parameters

Loaded parameter JSON is retained on the node even when the installed processor version does not expose that parameter.

When saving again:

1. preserved unknown parameter values are copied forward;
2. parameters known to the live processor overwrite their corresponding saved values;
3. choice parameters that expose stable choice IDs are saved by ID rather than menu position;
4. if a newer build wrote a choice ID that the current build does not recognise, the opaque saved ID is preserved instead of being replaced by the current fallback/default.

This lets removed, future, or currently unsupported parameter values survive a round trip where possible. Processors that provide stable choice IDs persist those IDs; backends without them retain their existing numeric choice persistence.

RawNode does not yet guarantee preservation of every unknown top-level or unknown node-level metadata field. That can be expanded additively if future schema versions require it.

### RAW state

The V2 `raw` object stores the initial RAW colour encoding through `colorSpace` and `gamma`. Older combined `workingSpace` values are accepted only during migration.

Future RAW decoder/developer settings should be added here without changing the graph node model.

### GUI state

V2 retained the inherited `gui` block for behaviour compatibility.

This GUI block is historical. Current grades contain only photo state; global preferences and ImGui own interface state.

### V1 migration

The historical reader accepted inherited sidecars named:

```text
DSC_0001.NEF.ofxrawhost.json
```

The historical reader normalised V1 nodes as `backend: "ofx"`, assigned stable
node IDs and wrote V2 on the next save. It preferred V2 `.rawnode.json` when
both files existed. This migration is not part of live photo opening.

### Invalid and newer sidecars

When a V2 sidecar could not be parsed safely, the historical reader cleared
that image's inherited on-screen chain and blocked automatic writes, preventing
the previous image's edits from overwriting a damaged sidecar.

It handled newer `rawnode-sidecar` versions the same way, without reinterpreting
or downgrading them. Recognised V2 files with unknown RAW/Output colour IDs or
unknown stable processor choice IDs were also protected from overwrite.

Protection lasted for that session; it cleared after the unsupported data was
fixed or the sidecar removed and the image reopened. This describes the retired
V2 reader, not live photo opening; current graph-native protection is described
at the start of this document.

#### Older-build compatibility

Historical later V2 sidecars contain independent colour-space/gamma IDs that PR #16 and earlier builds do not understand. Those older builds can fall back to their historical Rec.709/Linear interpretation and may overwrite newer colour settings if they save the file. Avoid editing newly written sidecars with an older RawNode build.
