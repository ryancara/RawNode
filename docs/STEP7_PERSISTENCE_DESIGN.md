# Step 7 — Graph-native persistence design checkpoint

**Status:** Proposed schema and implementation boundaries; awaiting independent
Claude review. This checkpoint implements no persistence code.

**Investigated baseline:** `main` at PR #61,
`20e3ba75165e38edf9bd97a6266342dde0a41b1d`. Step 5/6 and the workflow decisions
in PR #61 are accepted inputs, not subjects of another architecture audit.

The existing graph can be persisted without replacing its model or evaluator.
The necessary changes are a graph-shaped codec, explicit grade presence, a safe
file-update path, transactional reconstruction, shared transfers, and global
preferences. The concrete identity gap is durable SlotId allocation state.

Read with [PROJECT_CONTEXT.md](PROJECT_CONTEXT.md#persistence-direction),
[ARCHITECTURE.md](ARCHITECTURE.md#persistence-migration-for-topology),
[DECISIONS.md](DECISIONS.md) (D038, D040–D049), and the
[Step 5 persistence target](STEP5_GRAPH_CONTRACT.md#step-7-graph-persistence-target).
[SIDECAR.md](../SIDECAR.md) and [workspace-format.md](workspace-format.md) describe
existing/historical formats until the corresponding implementation replaces them.

## 1. Current implementation inventory

| Area and existing code | Observed behaviour and Step 7 disposition |
| --- | --- |
| `GraphModel.{h,cpp}`: `Node`, `GraphView`, `InputBindings`, `OutputRef`, `Slot` | Reuse. `App::nodes` includes Source and is the only live graph. `GraphView` borrows storage. Fixed and Slot input maps own connections; output declarations give their types. No global edge list is needed. |
| `nodeInterface(NodeOperation)` | Static interfaces; only `Source` and `ImageProcessor` are production operations today. Mask, multi-output and slotted operations are test-only. Add stable persistence names/versions for production operations; never persist enum ordinals or enable test operations in the production loader. |
| `GraphValidation.cpp`: `graphStructureError`, `requestedGraphError` | Reuse structural validation after reconstruction: identities, Source/output, port storage/types, Slots and cycles, including unreachable/disabled edges. Required disconnections are structurally representable; requested-endpoint readiness is a separate check. Saving an unfinished graph must not require a successful render. |
| `GraphCommands.cpp`: `StagedEdit`, `graph::Document`, splice/Slot commands | Reuse staged validation and no-throw commit discipline. Staging copies topology without cloning processors. Its commit currently accepts one added node, not a transactional multi-node Append. A bounded multi-node splice extension is needed; repeated public insertion calls would permit partial application. |
| `NodeGraph.cpp`: `makeNodeId`, `restoredNodeId` | IDs are strings. A process-local counter generates `node-N` while checking live nodes for collisions. Restore retains a requested ID unless empty/duplicate, then silently generates another. New graph-file loading must reject bad IDs, since references make silent repair ambiguous. Insertion needs one complete remapping table and a reserved-ID set. |
| `Node::makeSlot`, `observeSlotIds`, `StagedEdit::copyTopology` | `nextSlotId_` starts at 1; observation raises it above existing IDs; deletion/reorder retain the high-water mark. Staging observes restored IDs even before deleting the highest one. No public capture/restore access or on-disk high-water field exists. Saving just surviving Slots loses deletion history. Add narrow checked access to this existing state. |
| `PersistNode` / `PersistChain` in `ProjectPersist.h` | Current records contain processor identity, enabled state, opaque parameter JSON, UI groups and selection, but no Source/output, operation/interface, connections, Slots or allocator state. Replace with simple transient graph/grade/file records; these have no evaluator or independent editing API. |
| `captureNode`, `captureChain`, `linearCaptureOrder` | Derive a complete unary Image spine and reject anything outside it. Even single-node copy requires the entire document to fit V2. Retire that guard only as each caller adopts graph-native capture. Capture all stored nodes, including off-spine and disconnected ones. |
| `restoreChain`, `replaceChain`, `applyChain` | Restore invents Source from `app.primarySource`, inserts processors sequentially, infers output and restores selection. Replace swaps nodes/output but not Source identity. Replace with two-pass node/binding restoration and explicit endpoint publication. Reuse processor factories and selection/inspection reset behaviour. |
| `capturePersistedNode`, `restoredNode`, parameter helpers | Preserve backend/identifier/label and raw parameter values for missing processors. Known values overwrite preserved values; unknown stable choice IDs are specially retained. Other malformed known values can be coerced by `strtol`/`strtod`, ignored or overwritten; setter failures are not checked. Retain lossless values, but validate types and restoration results. OFX label fallback must not select a different plugin when an identifier is unavailable. |
| Processor identity | Native IDs are stable (`rawnode.native.exposure`, `rawnode.native.cst`); OFX uses plugin identifier; CTL uses a canonical script path. `Processor` exposes no common schema-version API. Persist host adapter/parameter-contract versions, not invented plugin binary versions. CTL needs an explicit file-reference representation and base-path handling. |
| `ProjectPersistLoad.cpp` | Small cursor/extraction parser captures opaque values, but is not a strict whole-document JSON validator: early extraction can miss malformed tails, nested raw values are mostly bracket-scanned, duplicate keys overwrite, and legacy strings accept raw control characters. Replace these permissive parsing paths for the new format; retain useful escaping/raw-value concepts. The CTL-local metadata parser is not a ready-made persistence validator either. |
| `ProjectPersistSave.cpp`: `writeFile` | Opens/truncates the destination directly. `saveInputSidecar` creates a grade for an empty chain. There is no metadata merge, compare-before-write or atomic replacement. Replace this write path, not merely its JSON body. Keep the source naming rule `imagePath + ".rawnode.json"` and metadata-file filtering. |
| `DocumentActions.cpp`: `applyGrade` | Useful transaction template: resolves colours, enters `DocumentMutation`, creates nodes, decodes changed RAW and builds preview before swapping live state. Reuse it for graph grades and photo opening. No new render-ownership mechanism is needed. |
| `rawWorkingEncodingForOpen`, `loadSidecarForPath`, `openPath` | Read the sidecar twice. `openPath` commits source/path before graph restoration; the nested load applies GUI/chain before checking unknown colours. Exceptions can occur after partial document changes. Replace with one parsed snapshot and one staged reconstruction/publication. |
| `AppState.h` colour fields | `inputEncoding` describes actual decoded pixels; `rawWorkingEncoding` is a session default; `outputEncoding` is current photo state. No distinct default Output field exists. A photo without a sidecar can inherit the previous photo's Output. Separate current grade colours from default colours. |
| Sidecar write protection | `sidecarWriteBlockedPath` blocks the active path after failed/newer reads or unknown colour/choice IDs. Useful policy, but only a UI-session guard: direct file writers bypass it, filesystem errors/nonregular paths can look absent, and the save return value is ignored. Enforce protection inside the shared store as well. |
| Unconditional save callers | `openPath` saves the previous photo; `openWorkspace` saves on folder change; `runApp` in `UI.cpp` saves at shutdown; `reloadCurrentRawEncoding` saves after RAW changes; File → Save Project saves explicitly. No grade-presence/dirty state distinguishes browsing. The menu reports success unconditionally. Audit every caller when replacing it. |
| `UiFrame.cpp` transfers | Clipboard uses `rawnode-transfer` v1; presets use a separate `rawnode-preset` v1 envelope and `.rawnodepreset`. Both serialize `PersistChain`. Node insertion gets a fresh ID; grades use `applyGrade`. Full-grade copy/save is disabled for Source-only documents and may omit colours for protected data. Replace with shared graph/grade codecs, including empty grades; never export an apparently complete grade with omitted colours. |
| Preferences/layout | `PersistGui` mixes Output colour with theme, preview/export and panels. `persistWorkspace` writes `workspace.ofxrawhost.json`; folder open restores it and active image. `ImGuiBackend_SetWorkspaceIni` saves the previous ini then switches to `.ofxrawhost-layout.ini`; initialization otherwise uses working-directory `ofxrawhost.ini`. Retire all three storage paths. |
| Existing tests | `GraphModelTests`, `GraphCommandsTests`, `GraphEvaluatorTests`, `GraphRuntimeTests`, `NodeListTests` and `SelfTest.cpp` already cover identity, Slots, topology, runtime barriers, missing processors, precision/Unicode, transfers, colour protection and D038. Extend this self-test architecture; retain the behavioural guarantees while replacing V1/V2 expectations. |

`DocumentMutation::changed()` currently describes render demand, not an authored
grade change: opening a source and rebuilding preview also call it. Do not use
it, `hasProcessingOutput()`, publication state or renderer busy flags to infer
whether a grade should exist on disk. Parameter widgets and Output controls also
change persistent state without structural graph commands; they need explicit
document dirty notifications.

## 2. Proposed versioned JSON specification

### Common file envelope and grade

Use UTF-8, indented JSON, exact format discriminator `"rawnode"`, integer
`version: 1`. This is a new format family, not Sidecar V1 or V2. The discriminator
prevents confusing it with `rawnode-sidecar`; the new reader rejects old formats.
All graph-native photo sidecars and presets use this same envelope and codec.
There is no preset-specific format, `kind: node`, or separate node-preset schema.

| Field | Contract |
| --- | --- |
| `format`, `version` | Required, exactly `"rawnode"`, `1` for this proposal. Unsupported versions are protected. |
| `grade` | Optional object. Presence means a saved grade, even when it contains Source only. `null` is invalid. Required for a preset. |
| `metadata` | Optional object, independent of `grade`; omitted in presets. Optional `mark` is `"pick"`, `"reject"` or `"unmarked"`; absence means Unmarked. |
| `grade.color` | Required `rawWorking` and `output`, each with required stable `colorSpace` and `gamma` strings. |
| `grade.graph` | Required `primarySource` NodeId, `documentOutput` reference and `nodes` array. |
| `grade.layout` | Optional `positions` object mapping NodeId to `{ "x": number, "y": number }`. No position is required. |

An envelope with neither grade nor metadata is readable as no saved grade; the
writer does not create one for a no-op. Clearing a mark may leave an existing
metadata-only file with `"unmarked"`; no automatic file deletion is necessary.

Both colour pairs are always explicit, including in a one-processor preset and
an empty grade. On a RAW photograph, `rawWorking` is the encoding used for its
decoded Source pixels. On a raster photograph, retain a grade-local RAW pair
(initially the global RAW default) for complete transfer to RAW destinations;
it does not reinterpret raster pixels. A Replace onto a raster retains incoming
RAW settings for later save/copy. Do not use an empty RAW pair as an instruction
to consult the destination default. Raster `inputEncoding`/`inputIsRaw` are
decoder results, not saved instructions. Output remains the existing explicit
encoding/interpretation tag, not a new conversion node.

Representative combined photo sidecar (also a one-processor preset if
`metadata` is omitted):

```json
{
  "format": "rawnode",
  "version": 1,
  "metadata": { "mark": "pick" },
  "grade": {
    "color": {
      "rawWorking": { "colorSpace": "rec2020", "gamma": "linear" },
      "output": { "colorSpace": "rec709", "gamma": "srgb" }
    },
    "graph": {
      "primarySource": "source-a",
      "documentOutput": { "node": "exposure-a", "port": "image" },
      "nodes": [
        {
          "id": "exposure-a",
          "kind": "image-processor",
          "kindVersion": 1,
          "interface": {
            "version": 1,
            "inputs": [{ "id": "image", "type": "image", "required": true }],
            "outputs": [{ "id": "image", "type": "image" }],
            "slotInputs": []
          },
          "processor": {
            "backend": "native",
            "identifier": "rawnode.native.exposure",
            "schemaVersion": 1,
            "label": "Exposure"
          },
          "enabled": true,
          "params": { "exposure": 0.75 },
          "inputs": { "image": { "node": "source-a", "port": "image" } },
          "slots": [],
          "nextSlotId": "1"
        },
        {
          "id": "source-a",
          "kind": "source",
          "kindVersion": 1,
          "interface": {
            "version": 1,
            "inputs": [],
            "outputs": [{ "id": "image", "type": "image" }],
            "slotInputs": []
          },
          "enabled": true,
          "params": {},
          "inputs": {},
          "slots": [],
          "nextSlotId": "1"
        }
      ]
    },
    "layout": {
      "positions": {
        "source-a": { "x": 0, "y": 80 },
        "exposure-a": { "x": 240, "y": 80 }
      }
    }
  }
}
```

Source deliberately follows its consumer in this array. There is no execution
order, selection or second authoritative edge list. The photo path comes from
the opened image, not a filename in the grade; presets bind the same Source node
to the destination photograph. Source is an explicit node; Output is an explicit
reference, not a synthetic Output processor.

### Node, interface, connection and Slot fields

Every node requires `id`, `kind`, `kindVersion`, `interface`, `enabled`, `params`,
`inputs`, `slots` and `nextSlotId`. `processor` is required for
`image-processor`, forbidden for Source. Source must be enabled and have empty
parameters/inputs/Slots. Its output is `image: image`. Non-slotted kinds require
`slots: []` and `nextSlotId: "1"`.

`kind` is the host operation identity (`source`, `image-processor` initially),
distinct from backend processor identity. `kindVersion` versions operation and
generic-control semantics. `interface.version` versions its static port
contract. `processor.schemaVersion` versions the host's parameter encoding for
that backend/identifier (1 for the initial adapters), not OFX/CTL software release
numbers. Installed software may change independently; this format does not bundle
or pin executable dependencies. Labels are informational, never lookup aliases.

The saved interface lists named fixed input/output declarations and repeated
Slot input declarations. Types are exactly `image` or `mask`; input declarations
include requiredness. IDs must be unique within direction/scope. `image` may be
both an input and output name. For known operations the saved declarations must
match the supported interface; a file cannot redefine executable port semantics.
Saving this small declaration snapshot also preserves the interface when the
processor cannot be installed. Declaration order has no meaning.

An input map has exactly one entry per declared input. Its value is either
`null` (disconnected) or `{ "node": NodeId, "port": PortId }`. The producing
output declaration supplies the type, checked against the consuming declaration;
repeating a type inside every reference would add redundant state. References
are structured fields, not delimiter-encoded strings. Slot input addresses are
derived from their owner, SlotId and map key, never stored again as edges.

Slots are ordered objects with required `id`, `inputs` and `settings`.
Currently the only `SlotSettings` field is required boolean `enabled`. The
following is a **record excerpt for the existing test-only slotted interface**,
not a proposed production mixer or complete document. Its declarations include
required Image and optional Mask Slot inputs:

```json
{
  "inputs": { "reference": { "node": "source-a", "port": "image" } },
  "slots": [
    {
      "id": "3",
      "inputs": {
        "image": { "node": "branch-b", "port": "image" },
        "mask": null
      },
      "settings": { "enabled": true }
    },
    {
      "id": "1",
      "inputs": {
        "image": { "node": "branch-a", "port": "image" },
        "mask": { "node": "mask-a", "port": "mask" }
      },
      "settings": { "enabled": false }
    }
  ],
  "nextSlotId": "9"
}
```

This preserves order `[3, 1]` and previously issued/deleted IDs through 8. Fixed
Reference, Image and Mask links all use the same input representation. A
branching production graph today can contain several Image processors referring
to the same Source or upstream processor, including nodes outside documentOutput's
dependencies; save every one. Self-test-only interfaces can exercise joins,
Mask links, multiple outputs and Slots without making those operations loadable
from production files.

The `enabled` field is today's generic node control. Reserve an optional
`controls` object for operation-defined generic controls, separate from backend
`params`; for the two initial production kinds only absent/empty is supported.
Do not implement or silently accept Strength/Blend values before their production
operation semantics exist. Future executable controls and Slot settings require
a supported kind version (and a file-version bump if older readers could
otherwise ignore a meaningful change). They stay on nodes/Slots, not processors.

### Parameters, unavailability and assets

`params` maps stable parameter IDs to any valid JSON value. Keep unknown values
as validated raw JSON slices, including arrays, objects and null. Known values
use the existing bool/integer/double/string/vector mappings; stable choice IDs
are strings where exposed, numeric choices remain only for backends without
stable IDs. Numeric writes use the classic locale and round-trip precision.
Skip Group/Page/PushButton and non-persistent transient controls, as today.

A missing processor is still `kind: "image-processor"` with its saved interface,
backend, identifier, schema version, enabled state and complete raw parameters.
Availability is recomputed on load; there is no persisted `missing: true` flag
that would prevent a newly installed processor from being tried. The current
known unary missing-processor fallback and warning remain unchanged. A future
unknown operation/interface is preserved by protecting its entire document;
do not pretend it is a unary processor or build a new generic executable node.

Optional `assets` is a node-local object keyed by semantic asset role. For the
current CTL backend the role is `script`; future asset roles require their own
operation/adapter support. A file reference is `{ "base": "document" | "absolute",
"path": string }`. `document` means the JSON file's directory, never the process
working directory. Absolute paths are platform-native; unsupported foreign
paths are retained and reported as unavailable. No search, download or relinking
system is introduced.

Example CTL processor/asset excerpt:

```json
{
  "processor": {
    "backend": "ctl",
    "identifier": "looks/warm.ctl",
    "schemaVersion": 1,
    "label": "Warm"
  },
  "assets": {
    "script": { "base": "document", "path": "looks/warm.ctl" }
  },
  "params": {
    "gain": 1.1,
    "futureCurve": { "knots": [0, 0.5, 1], "mode": "future" }
  }
}
```

For CTL, identifier remains path identity: it must equal `assets.script.path`,
whose base defines resolution. The adapter resolves it before calling
`CtlProcessor::create`; it retains the saved reference separately from the live
processor's canonical-path identifier. Native/OFX identities are not paths.
On saving a preset elsewhere or pasting elsewhere, resolve recognized references
against their original base and re-express against the new file directory (or
as absolute paths). Clipboard capture uses absolute recognized asset references
because it has no file directory. Unknown asset roles can survive an unchanged
same-file save; relocation is refused if their relative meaning cannot safely
be preserved. Script includes continue to follow the existing CTL backend rules.

### Independent metadata and intentionally empty grade examples

Metadata-only file:

```json
{
  "format": "rawnode",
  "version": 1,
  "metadata": { "mark": "reject" }
}
```

Intentionally saved empty grade (also a valid empty preset):

```json
{
  "format": "rawnode",
  "version": 1,
  "grade": {
    "color": {
      "rawWorking": { "colorSpace": "aces-ap1", "gamma": "linear" },
      "output": { "colorSpace": "rec709", "gamma": "srgb" }
    },
    "graph": {
      "primarySource": "source-a",
      "documentOutput": { "node": "source-a", "port": "image" },
      "nodes": [{
        "id": "source-a",
        "kind": "source",
        "kindVersion": 1,
        "interface": {
          "version": 1,
          "inputs": [],
          "outputs": [{ "id": "image", "type": "image" }],
          "slotInputs": []
        },
        "enabled": true,
        "params": {},
        "inputs": {},
        "slots": [],
        "nextSlotId": "1"
      }]
    }
  }
}
```

An empty saved grade has Source, colours and Output, not an empty node array.
Deleting the final processor leaves this state; it does not erase grade presence.

## 3. Graph identity, ports and SlotId rules

1. NodeIds are nonempty opaque strings unique within one document. Opening a
   sidecar preserves every valid ID, including Source. Duplicate/empty IDs are
   invalid; reject rather than repair or pick the first occurrence.
2. Whole-grade Replace retains incoming IDs and publishes incoming primarySource
   with the graph, binding its pixels to the destination photo. Photographs have
   separate identity scopes. Clear selection/filter and reset inspection even
   when IDs happen to equal those in the previous grade.
3. Insertion allocates fresh IDs against both destination and incoming reserved
   IDs, builds the complete old→new map, then rewrites every internal fixed/Slot
   reference and layout key. A preset Source is rebound explicitly, never added
   as a second Source. Do not use an allocator pass that can produce an incoming
   old ID and accidentally alias a reference before remapping completes.
4. Keep the current collision-checked `node-N` allocation approach, shared by
   creation and insertion. No persisted NodeId counter or new identity service
   is required: IDs are not numeric order, and there are no retained references
   to deleted nodes. Check exhaustion/wrap rather than silently collide.
5. Port and kind names are durable semantic IDs; display labels and declaration
   order are not identities. A future rename needs an explicit supported alias
   or version conversion, never ordinal matching. Step 7 adds no legacy reader.
6. SlotIds are scoped by owning NodeId. Reorder/reconnect retain ID and settings;
   copying an entire node retains its SlotIds and allocator state because its
   NodeId changes. Duplicating one Slot within an existing node calls `makeSlot`
   for a fresh ID. Slots have no separate `order` or `GroupId`.
7. Encode SlotId and `nextSlotId` as canonical unsigned decimal strings, avoiding
   JSON-number rounding above 2^53. Reject signs, leading zeros, zero, overflow
   and duplicates. Accept values through `UINT64_MAX`; normally nextSlotId must
   exceed all saved SlotIds. `UINT64_MAX` is the existing exhausted sentinel:
   no new allocation is allowed; a restored Slot may itself have that value.
8. Restore the validated high-water value before any edit, including deletion.
   Never recompute it solely as `max(surviving Slots) + 1`. Saving after deleting
   all Slots must still retain it. A small `Node` accessor/checked restore method
   suffices; preserve `StagedEdit` copy/swap/no-op comparisons.

Layout is optional presentation data. Valid finite positions for existing IDs
round-trip; absent positions are normal. Dangling keys or malformed positions
produce a layout warning and are ignored for presentation, while retaining the
raw optional data when safe. They do not invalidate executable topology or
change evaluation. Known node deletion removes its position; remapping rewrites
known keys. If an optional opaque extension cannot safely be remapped, refuse
that transfer rather than attach its data to a different node.

## 4. Sidecar, metadata and grade ownership

| Read result | Editor behaviour | Write/default behaviour |
| --- | --- | --- |
| No sidecar (confirmed absence) | Fresh Source and global colour defaults. | No write from browsing. Eligible for a future default preset. |
| Valid metadata-only sidecar | Restore mark; fresh Source and global colour defaults. | Metadata remains independent; no grade manufactured. Eligible for a future default preset. |
| Valid saved grade | Restore complete graph and grade colours, plus metadata. | Grade takes precedence over all defaults. |
| Valid intentionally empty grade | Restore Source-only graph and its colours. | Still a saved grade; future defaults must not replace it. |
| Invalid/unreadable/newer/unsupported/protected | Report path and reason. Do not publish a guessed grade. | No automatic default, downgrade, metadata update or overwrite. |

For initial Step 7, a protected incoming photo fails to open and leaves the
previous document intact (or the initial no-photo state). A read-only fallback
viewer is not required. This is simpler than presenting fallback colours as the
saved grade and preserves transactional opening. Missing supported unary
processors alone do not make the file protected.

Keep a small control-thread document persistence state in App: grade presence,
grade dirty, metadata dirty, and the loaded file snapshot/protection result.
These are file/edit state, never renderer demand. Presence becomes true on a
successful processing/colour edit, explicit Save Grade (including Source only),
or successful grade/preset application. Successful restore sets presence without
dirtying it. Failed commands and no-ops do not establish a grade. An explicit
clear of an existing grade keeps a Source-only grade. Step 7 has no automatic
"forget grade and use defaults" action.

Graph edits, node enablement, successful parameter/reset/activation changes and
per-photo RAW/Output changes mark the grade dirty. Parameter activation can
modify other persistent parameters (e.g. CTL preset selectors); track the actual
edit, not the transient selector value. UI selection, filters, pan/zoom, preview
resolution, theme, export settings, decoding, render completion and navigation
do not. Optional future node-position edits dirty an existing grade but merely
browsing/layout initialization cannot establish one.

Metadata APIs operate on a file snapshot without opening the photograph or
publishing an editor document. An update has an explicit field intent (e.g. set
mark), not a default-constructed
whole document. Grade updates replace only grade and retain metadata/other safe
file fields; metadata updates retain the entire grade. For an open document,
incorporate the successful store result into its snapshot so later saves do not
revert a mark. On-disk changes to an untouched section can be merged only after
validating the latest file; a changed section being replaced is a conflict and
must be refused. Comparing the original raw section values is enough; no revision
framework or background watcher is needed.

Autosave on navigation/folder change/quit becomes save-if-dirty. Explicit Save
Grade may establish an empty grade, but reports the actual result. No source path
means no photo-sidecar write. Failed outgoing saves retain dirty state and the
open document; abort switching or closing until the user retries or explicitly
discards the unsaved edit. Do not silently destroy the only in-memory copy.
Protected files have no force-overwrite path in Step 7. Removing/moving an old
file externally and reopening can clear protection; the application never
deletes it as migration.

Recognize the legacy `.ofxrawhost.json` filename for protection without parsing
its format. If no valid new sidecar exists and a legacy sidecar is present,
report unsupported state rather than applying defaults or creating a new grade
beside it. An existing valid new sidecar takes precedence; leave the legacy file
untouched. Unsupported old `.rawnode.json` files also block saves. Export remains
outside the sidecar writer under D038.

Future default-preset application will use saved grade → configured default →
global defaults precedence; protected reads are never treated as absence. Step 7
provides these states but adds neither default-preset selection nor automatic
application, culling UI, batch UI or a preset browser.

## 5. Preset Replace/Append and clipboard behaviour

### Shared code and Replace

Keep one node codec, one graph codec, one grade codec and one reconstruction path
under `src/persist`, using existing `Node`, port and binding types where useful.
Transient decoded records hold JSON and processor descriptions until live Nodes
can be built; they do not run, edit or mirror the live graph after commit.
Photo files, presets and transfers compose those same functions. No generic
persistence framework, second graph model or alternate evaluator is warranted.

A `.rawnode.json` preset is the common file envelope with `grade` and without
photo metadata. Its grade object is identical to a sidecar's grade object.
Loading another photograph's sidecar as a preset selects only its grade and
never imports its mark. A metadata-only file is not a preset. Full-grade export
includes all stored nodes; one-node preset export creates a Source-plus-one-node
grade with both colour pairs. Initially that action requires a unary Image
processor and explicitly rebinds its Image input to synthetic Source. Refuse
other connected inputs that a one-node grade would lose; use fragment copy for
those cases. Do not silently trim a selected subgraph into a node preset.

Replace stages and validates the complete incoming graph, keeps its valid IDs,
rebinds its primary Source pixels to the destination photo and uses its colours.
It preserves destination metadata and global defaults. If RAW settings change,
decode/rebuild before commit. Any failure leaves the original graph, processors,
colours, Source pixels, selection, presence and dirty state intact. Supported
branching graphs work without special attachment policy because the whole graph
is replaced. A Source-only preset intentionally replaces the grade with empty
processing. No photo open means application is refused.

### Initial Append policy

For Step 7, both incoming and destination graphs must be **complete ordinary
linear Image-processor chains**, including Source-only chains. Require:

- `graphStructureError` succeeds and `imageSpine` reaches primary Source;
- every non-Source node is on that spine and is `ImageProcessor` with the
  supported unary interface; no Slots, secondary links or off-spine nodes;
- each processor input/output is `image`; no unresolved required input;
- selection is either absent or a non-Source node on the destination spine.
  A stale/off-spine selection is an error, not an instruction to append at end.

Validate both grades, including incoming colour identifiers, before staging.
Incoming colours are deliberately not applied, and no CST is inserted to
compensate for a difference. Retain destination grade-local RAW and Output values.

For `Source → A → B`, with A selected, appending `Source → X → Y` yields
`Source → A → X' → Y' → B`. Without selection it yields
`Source → A → B → X' → Y'`. Generate all fresh IDs first; map incoming Source to
the insertion producer, rewrite internal references, connect the first new
input, and rewire only the successor input (or documentOutput at the end).
An empty incoming chain is a successful no-op, with no dirty/presence change.

Stage all added processors and the final topology before moving any live
processor. Extend the existing staged-edit commit to accept this small batch,
reserve storage before mutation, then commit once under DocumentMutation. Do not
clone destination processors or commit a loop of separately fallible insertions.
Preserve destination metadata/layout; import positions under remapped new IDs,
omit incoming Source's position. Overlap is acceptable until Graph View handles
placement; positions never decide attachment.

Reject incoming **or destination** branching graphs for Append in the initial
release, even when an attachment appears visually obvious. Return a useful error
such as "Append currently requires two complete linear Image chains; use Replace
for this preset." General DAG insertion, Reference/Mask rebinding and multi-input
attachment require explicit later edit commands/UX. This restriction applies to
attachment, not graph serialization, sidecar restoration or full Replace.

### Clipboard fragments

Clipboard transport can keep `format: "rawnode-transfer"` with a new `version: 2`
and `kind: "grade" | "fragment"`. Grade payloads contain the exact `grade` object
above. Fragment payloads contain `fragment: { nodes, layout? }`, using precisely
the same node/Slot/interface/parameter records and layout structure. They need
neither document endpoints nor colours. There is no separate node payload type;
one node is a fragment of size one. Reject old/unsupported transport versions
without changing clipboard or destination state. Transport version 2 uses the
same graph/grade contract as file format `rawnode` version 1; it does not define
an independent graph schema.

Fragment capture retains all links whose producer is in the selected set. Drop
external incoming references to `null`, with diagnostics identifying dropped
links, including Mask/Reference. Do not leave foreign NodeIds that could collide
with destination IDs. Never include the primary Source in an ordinary fragment;
use complete-grade copy for whole-document transfer. Required disconnected inputs
are allowed in a fragment. Outgoing consumers outside the fragment are not
copied, since connections belong to consumers.

Paste always builds the complete fresh NodeId map and preserves node-local
SlotIds/order/settings/high-water values. Two explicit operations share that
reconstruction, not two codecs:

- **Existing Paste command:** a unary node or linear fragment can use the same
  validated Image-splice rule as Append. Identify its one disconnected Image
  entry and one terminal Image output from topology, never array order. Reject
  ambiguous/disconnected collections or unsupported attachments. Optional
  external Mask/Reference links remain disconnected with warnings; no hidden
  rebinding occurs.
- **Detached fragment insertion API:** reconstruct any supported structurally
  valid fragment with internal links remapped, external inputs disconnected and
  documentOutput unchanged. This supports branching fragments without guessing
  attachment; a future Graph View can expose it. Step 7 needs no new UI for it.

Full-grade copy includes Source-only grades and complete colours; full-grade
paste uses Replace and preserves photo metadata. Capture from protected/unresolved
grade data is refused rather than emitting a colour-incomplete "full grade".
Serialize/validate the complete payload before changing the clipboard or opening
a destination preset file.

## 6. Validation, reconstruction and atomic-save contract

### Parse and validate before changing state

1. Classify the path as absent, readable regular file, or protected error. Stat,
   read, permission and nonregular-path failures are not absence. Do not follow
   sidecar symlinks for replacement; report them as protected in the initial
   implementation. Read the whole file once for this operation.
2. Validate one complete JSON document: delimiters, literals/number grammar,
   escaped strings/Unicode, duplicate object keys, types, integer ranges and no
   trailing junk. Use bounded input/depth limits with explicit errors, not
   truncation. A small persistence-local strict reader retaining raw slices can
   replace the existing cursor helpers; no new external framework is needed.
3. Validate the format/version, grade/metadata presence and required fields.
   Unknown versions or meaningful unsupported fields protect the file, even if
   a partial grade looks usable. Reject `grade: null`, incomplete colour pairs,
   duplicate IDs/ports, malformed raw parameters and unknown enum values.
4. Validate colours through `colorEncodingFromIds`, with no numeric menu indices,
   display-name fallback or legacy migration. Validate the graph's saved
   operation/interface versions and declaration shape. Decode all nodes and
   references in two passes so array order never constrains reconstruction.
5. Validate identities/allocator values, references, port types, Source/output
   and cycles across **all** stored nodes. For known kinds, materialize an
   operation-local vector of Nodes with no processors and run the existing
   `graphStructureError`; do not reimplement graph rules in the file layer.
6. Distinguish structural validity from readiness. Explicit `null` required
   inputs may round-trip as unfinished edits; `requestedGraphError`/the evaluator
   will diagnose them if needed. Neither save nor metadata update renders the
   graph. Pixel-dimension compatibility remains an evaluation check.

Unknown members in the same supported format version may only be optional,
non-semantic annotations. Retain their validated raw values at their original
object scope (including metadata and node records). Unknown executable fields in
`controls`, Slot settings, interface declarations or unsupported kind versions
are protected, not ignored. A future writer must bump the relevant version for
new required/executable semantics. This is preservation, not forward execution.

Unknown parameter IDs and missing backend/processor identities are safe to retain
opaquely. For an installed supported processor, known parameters need exact type,
vector arity, numeric range/finite-value and stable choice-ID checks before
calling setters. Check setter success and normalization; do not silently coerce
an object into zero or replace a saved unrecognized choice with a default. A
known-but-uninterpretable value makes grade application fail/protect the sidecar,
matching today's unknown-choice safety rule. Missing processors retain all values
without applying them. On capture, overwrite only values that were successfully
understood/restored or explicitly edited. Do not persist processor defaults over
opaque saved data.

Missing script/plugin or unsupported processor schema uses the known unary
missing-processor representation with a warning and preserved record. Unexpected
factory exceptions/allocation failures abort the transaction; do not disguise
them as successful reconstruction. Unknown graph kinds/interfaces remain
file-level protected data because today's Node/evaluator cannot represent their
execution safely. A metadata update must also refuse a protected document; it
cannot use a partial parse to overwrite the original.

Independent writes use the same protection checks. When a grade is present,
checking known processor parameters may require temporary backend instances;
reuse the restoration preflight, including its processor-construction exclusion
when called from the running editor. It needs no image decode or graph execution.
A metadata-only file needs no processor construction. Do not invent a second
parameter-schema registry merely to avoid this work, or bypass a discovered
parameter error because only the mark is changing.

### Publish a reconstructed photo or replacement grade

The document operation asserts control-thread intent and first resolves a single
validated grade/metadata/defaults snapshot. For photo switching, save the outgoing
dirty document successfully before discarding it. No load publishes a half-valid
incoming document or silently applies a default after a protected read.

Reuse the `applyGrade` staging pattern under one `DocumentMutation` for all live
processor lifetime work. Prepare incoming Nodes, exact IDs, bindings, Slots,
allocator state, optional layout, colours and metadata before commit. Decode
the selected photo using the resolved grade RAW encoding; for Replace on the
same RAW, re-decode only when required. Confirm the decoder result and actual
input encoding; a RAW result that does not match the requested grade encoding
is a load failure, not an occasion to change the saved pair. Then prepare preview
with `makePreviewSource`. Raster opening
continues to use its decoder's ICC/input handling.

All fallible construction, parameter application, decoding and allocation precede
live replacement. Commit nodes, primarySource, documentOutput, full/preview
Source owners, path, grade colours and persistence state as one non-throwing
publication within the mutation. Colour pairs remain coherent under colorMutex;
never call RenderRuntime while holding that leaf lock. Reset selection/filter
and inspection, then mark the mutation changed once. Old processors are destroyed
before the mutation releases execution ownership. The runtime supplies the final
preview decision; persistence adds no scheduler flags, waits or rendering domain.

Failure leaves the old document and its unsaved state intact, and preserves the
operation error through quiet preview restoration. Reuse the existing runtime
barrier tests; no evaluator or render scheduling changes are part of Step 7.

### Save a complete file or update one section

Use one source-sidecar store, callable by UI and future batch code without
opening the image. Return a result distinguishing saved/no-change/protected/
conflict/I/O failure with path and reason; callers must not report success merely
because a function returned. Direct store calls enforce protection too.

1. Capture a coherent document snapshot on the control thread. Backend parameter
   snapshots keep their existing synchronization; if adapter capture needs
   execution exclusion, use DocumentMutation. Do not test transient busy/pending
   flags or require a rendered result to decide whether saving is legal.
2. Read/validate the current destination, including legacy/protected-path checks.
   Apply the explicit grade or metadata update, preserving the other section and
   safe unknown information. Refuse conflicting changes to the edited section.
   Metadata updates need no source decode or live document publication. Compare
   against the loaded original when saving a live grade.
3. Validate the candidate and serialize it fully in memory before touching the
   original. Retain raw untouched grade data during metadata-only updates;
   unknown parameter fragments must remain valid and unmodified. New writes use
   deterministic keys/indentation and may sort nodes by ID for reviewability;
   Slot arrays must retain semantic order.
4. Create a unique temporary file **in the destination directory** with exclusive
   creation. Write all bytes, check short writes, flush, sync and close errors.
   Preserve existing ordinary file permissions where applicable; a failure to
   prepare the replacement does not truncate or unlink the original.
5. Recheck that the destination still matches the read snapshot (including
   expected absence). A detected change aborts with conflict; do not overwrite
   it. This is a conservative single-writer application check, not a distributed
   lock or a promise of race-free multi-process editing.
6. Atomically replace using the platform primitive: same-filesystem rename on
   POSIX; a Windows replacement/move API with atomic replacement semantics,
   including the absent-target case. Never remove the old file first or fall
   back to copy/truncate. Sync the containing directory where supported after
   replacement. Clean up only this operation's temporary file on failure.
7. Only successful replacement updates the document snapshot and clears the
   corresponding dirty flags. Report the actual file operation failure. If the
   rename succeeded but directory durability confirmation fails, report that
   the complete new file is present but durability is uncertain; do not claim
   the old bytes survived or attempt a destructive rollback.

Before atomic replacement, every failure leaves the original byte-for-byte
intact. After replacement, readers see the complete old or new file, never a
partial file. Power-loss durability depends on filesystem/platform guarantees;
no recovery journal, backup rotation, startup repair or migration system is
proposed. Existing protected/legacy files are never overwritten by explicit Save
or a preset save either; a new path may be chosen. Preset file writes reuse the
same atomic writer and validation, without photo metadata merge semantics.

## 7. Global preferences and workspace retirement

Use a small versioned `preferences.json` under the user's application settings
directory and a global `layout.ini` beside it. Keeping ImGui's own layout format
avoids a custom dock serializer; both files belong to the one global scope.

| Platform | Directory |
| --- | --- |
| macOS | User Application Support directory + `RawNode` (normally `~/Library/Application Support/RawNode`). |
| Windows | Roaming AppData known folder + `RawNode` (normally `%APPDATA%/RawNode`). |
| Linux | `$XDG_CONFIG_HOME/RawNode`, or `~/.config/RawNode` when unset. |

Resolve through platform conventions; inject a directory into self-tests so
they never read/write a developer's settings. If resolution or writing fails,
use session defaults with a reported error; never fall back to the current or
photo directory. Unknown/newer preferences are protected from overwrite, while
photos with valid grades can still open independently of preference failure.

The initial global store must retain these current user choices:

| Preference | Current owner/path → new owner |
| --- | --- |
| Theme | `PersistGui::themeIndex` in sidecar/workspace → global theme preference. |
| Preview resolution | `previewRes` in sidecar/workspace → global preference. Rebuilding preview is not a grade edit. |
| Export format and JPEG quality | `exportFormat`, `jpegQuality` → global export defaults. Export itself saves no sidecar. |
| Left/right/filmstrip visibility | `showLeft`, `showRight`, `showFilmstrip` → global UI preferences. |
| Dock/window layout | Per-folder/default ImGui ini → global ini loaded once at startup. Retain default layout construction and window geometry the backend actually supports. |
| Default layout sizes | `leftW`, `rightW`, `filmstripH` currently seed DockBuilder from legacy reads → retain sensible global default sizing; saved ImGui geometry owns actual layout. |
| RAW and Output defaults | Separate default encoding pairs, initialized from current built-in defaults; current per-photo pairs belong only to grade. |

Load global settings before creating/applying the UI and opening the first photo.
Apply/save known preferences through the same bounded validation and atomic-file
helper; layout can use `LoadIniSettingsFromMemory`/`SaveIniSettingsToMemory` and
the helper. Set `ImGuiIO::IniFilename = nullptr` before the first frame so ImGui
never auto-creates `imgui.ini`, `ofxrawhost.ini` or a per-folder layout file. Handle
its save request explicitly and save final layout before destroying the context.

Retain current explicit RAW-control behaviour: an intentional RAW choice updates
the photo and global RAW default after successful decoding. Explicit Output
choice updates the photo and global Output default; restoring/applying a grade
updates neither default. With no photo, those controls change defaults only.
No Settings UI is needed. Loading photo A's grade must never make its colours
the defaults for unedited photo B.

Future plugin/preset folder paths, selected default preset, Graph/List preference,
shortcuts and input mappings belong in this global store when implemented. Step 7
does not add their discovery, selection or application UI. Current OFX environment
and standard-path discovery continues to work; CTL file selection still works.

Remove workspace JSON read/write functions, `persistWorkspace`, workspace write
protection, `ImGuiBackend_SetWorkspaceIni` and working-directory ini defaults.
Keep folder selection/filmstrip scanning as transient session behaviour; rename
user-facing Open Workspace/Save Project to Open Folder/Save Grade as appropriate.
Do not restore an active image from a folder settings file. Last-folder/photo
memory can be a later global preference. Node selection, group expansion,
parameter filters and preview pan/zoom stay transient; Graph View node X/Y belongs
to the grade, not the global dock layout.

No legacy preference import is required: retaining preferences means retaining
their functionality and future restart persistence, not silently reading old
folder files. Leave old files on disk unmodified. No replacement workspace,
catalogue or folder-default file is introduced.

## 8. Focused implementation PR sequence

These five PRs deliberately keep representation, disk safety, live-document
publication, transfers and preferences independently reviewable. PRs 1–2 add
tested seams but do not switch live sidecar writers prematurely. No intermediate
PR may remove D045 guards while a caller still uses the linear codec.

### PR 1 — Graph records, strict codec and durable identities

**Purpose/behaviour:** Define the proposed file/grade/node records and shared
JSON reader/writer. Add stable production kind/interface/adapter schema IDs and
checked Slot high-water capture/restore. Capture complete `GraphView` topology
and reconstruct worker-free Nodes for structural validation. Leave existing UI
file paths active until the integrated transaction is ready.

**Likely files:** `src/persist/ProjectPersist.h`, new focused
`src/persist/GraphPersist.{h,cpp}` (or replace equivalent load/save sections),
`src/GraphModel.{h,cpp}`, parameter capture helpers in `src/NodeGraph.cpp`,
`tests/GraphModelTests.cpp`, `tests/GraphCommandsTests.cpp`, and a focused
`tests/PersistenceTests.{h,cpp}` registered through the existing self-test entry
and CMake source list. These are future implementation changes, not checkpoint
changes.

**Tests/acceptance:** Hand-authored JSON and live graph round-trips retain every
ID, endpoint, typed binding, disabled/off-spine node, opaque parameter and ordered
Slot. Shuffling node storage changes neither connectivity nor requested output.
Saved allocator state survives deleting the highest/all Slots and reopening.
Strict-parser and version/type rejection have unchanged output arguments on
failure. Test-only operation mappings are an explicit internal fixture option,
never accepted by production file loading. No new image operations.

**Dependencies:** Reviewed checkpoint. **Risks:** Mistaking records for a second
graph; enum/array-order identities; losing opaque numeric precision; permissive
parser accepting damaged files; allocator overflow. Bound them with tests before
any new file is written by the application.

### PR 2 — Atomic sidecar store and independent section updates

**Purpose/behaviour:** Implement read classification, optional grade/metadata,
protection, section-preserving updates, conflict checks and atomic replacement.
Reuse existing processor factories with checked parameter preflight where a
grade requires interpretation; unavailable records remain opaque. Provide
headless metadata/grade store calls; no culling UI. Existing sidecar
application remains unchanged until PR 3; do not emit new files through old
document-opening code.

**Likely files:** `src/persist/ProjectPersist{Load,Save,Priv}.cpp/.h` as applicable,
or focused `src/persist/SidecarStore.{h,cpp}` beside the codec;
shared restoration/parameter helpers factored from `src/NodeGraph.cpp`;
`tests/PersistenceTests.cpp`. Keep the atomic writer a small platform utility,
not a generic storage framework.

**Tests/acceptance:** No sidecar vs metadata-only vs saved empty grade; independent
updates preserve untouched sections and extensions. Corrupt/newer/legacy files,
filesystem errors and symlinks remain protected even via direct store calls.
Inject temp-create/write/flush/close/replace failures and changed-target conflicts;
verify original bytes remain intact, no false success and no leaked owned temp
file. Successful replacement reparses as a complete file. Test supported OS
replacement paths; distinguish post-rename durability errors.

**Dependencies:** PR 1. **Risks:** Treating stat errors as absence; truncation
before validation; Windows replace semantics; lost metadata from stale editor
snapshots; false promises about multi-process or crash recovery.

### PR 3 — Transactional graph/colour restoration and authored-grade tracking

**Purpose/behaviour:** Replace `captureChain`/`restoreChain` for photo persistence
with shared graph capture/restoration. Parse once, stage processors/Source/colours,
then commit under DocumentMutation. Introduce presence/dirty state and a separate
grade-local RAW pair/default Output pair. Replace unconditional sidecar saving
with save-if-dirty; explicit Save Grade supports empty processing. Stop navigation
or exit from losing dirty state after save failure. Legacy sidecars become
protected, not migrated. Global defaults can remain in session until PR 5.

**Likely files:** `src/persist/DocumentActions.{h,cpp}`, `src/NodeGraph.{h,cpp}`,
`src/AppState.h`, `src/UI.cpp`, `src/ui/{UiFrame,LeftPanel,ParamWidgets}.cpp`,
small App-owned graph-command dirty notifications in `src/GraphCommands.cpp`,
`tests/{SelfTest,GraphRuntimeTests,NodeListTests,PersistenceTests}.cpp`,
`SIDECAR.md`. Reuse `DocumentMutation`/`makePreviewSource` without changing runtime
or evaluator scheduling. Keep old transfer adapters guarded until PR 4.

**Tests/acceptance:** Photo open/Replace publish correct graph, Source identity,
output and both colour pairs together; decode/processor/preview allocation errors
leave the old document intact. A→B browsing and shutdown create no sidecars;
parameter/Output edits and successful grade application do. Restore is clean;
empty grade is preserved. Missing processors retain topology and parameters.
Existing preview/export mutation-barrier, failure-status and D038 tests pass.

**Dependencies:** PRs 1–2. Before replacing the relevant paths, satisfy the
existing PROJECT_CONTEXT quality gate: record outstanding macOS/native-dialog
and Nikon NEF re-decode checks, or an explicit documented waiver for checks
still unverified. This checkpoint supplies neither evidence nor a waiver.
**Risks:** Missing nonstructural edit notifications;
using render demand as grade presence; inherited Output/default RAW state;
exceptions after Source publication; outgoing save failure losing edits. This
PR needs the most careful runtime-boundary review, not a runtime redesign.

### PR 4 — Unified presets and graph/fragment clipboard transfers

**Purpose/behaviour:** Switch presets to the same `.rawnode.json` grade format;
clipboard to shared grade/fragment codec. Implement Replace and conservative
linear Append, one-node fragments and detached fragment reconstruction. Expose
Replace/Append through the existing preset action/dialog, without a browser.
Remove retired linear records/serializers once no caller remains.

**Likely files:** `src/ui/UiFrame.cpp`, `src/ui/UiContext.h`,
`src/persist/{ProjectPersist,DocumentActions,GraphPersist}*`,
`src/NodeGraph.{h,cpp}`, narrow batch-splice support in
`src/GraphCommands.{h,cpp}`, and existing transfer/command/persistence self-tests;
update current-format portions of `ROADMAP.md` and `SIDECAR.md`.

**Tests/acceptance:** Sidecar grade and preset grade are interchangeable;
metadata never transfers. Replace supports complete branched/empty grades and
colours. Append after selected/end preserves destination colours and produces
the exact expected connections, with fresh IDs and one commit. Reject either
branched input or destination for Append unchanged. Fragment tests prove complete
ID remapping, preserved node-local Slot state and dropped external links. Failed
capture leaves prior clipboard/file bytes untouched. Old `.rawnodepreset` files
are not rewritten/deleted.

**Dependencies:** PRs 1–3. **Risks:** Partial batch insertion; Source duplication;
accidental remapping to existing IDs; external asset base loss; copying metadata;
branch flattening disguised as Append. Keep advanced attachment out of this PR.

### PR 5 — Global preferences and retirement of folder settings

**Purpose/behaviour:** Load/save global preferences/layout, preserve the essential
choices inventoried above, and remove workspace/CWD persistence. Folder browsing
continues as transient filmstrip state. Separate photo colours from global
defaults throughout startup and UI. No Settings UI or legacy import.

**Likely files:** small `src/persist/AppPreferences.{h,cpp}`,
`src/persist/DocumentActions.{h,cpp}`, retired workspace portions of
`ProjectPersist*`, `src/AppState.h`, `src/UI.cpp`,
`src/ui/{ImGuiBackend,DockLayout,UiFrame,LeftPanel}.cpp` and relevant headers;
preference/filesystem self-tests, `docs/workspace-format.md`, `SIDECAR.md` and
current-state summaries. Keep historical format documentation explicitly labelled.

**Tests/acceptance:** Restart round-trips theme, preview, export choices, panel
visibility, global colours and ImGui layout in a temporary global directory.
Opening/switching photos/folders and shutting down creates no per-folder/CWD
settings files and leaves existing legacy files byte-identical. Photo overrides
never change global defaults; explicit default edits survive restart. Preference
failure never falls back to writing beside a photo. Existing OFX/CTL loading
continues to work.

**Dependencies:** PR 2's writer and PR 3's separation of defaults from grade;
land after PR 4 for a simple linear review sequence. **Risks:** ImGui automatic
ini writes before first frame/on shutdown; losing existing controls while
removing PersistGui; startup ordering; accidentally interpreting workspace RAW
values as photo defaults.

## 9. Deterministic test plan

Use the existing `--selftest` runner, bool test cases and temporary-file fixtures.
A focused PersistenceTests translation unit is sufficient, not a new framework.
Use hand-authored fixtures and explicitly enumerated expected identities and
connections; codec→codec equality alone can hide matching save/load bugs.

| Test | Independent assertion and production path |
| --- | --- |
| Linear round-trip | Save/read/restore `S → exposure(+1 EV) → exposure(-2 EV)` in shuffled storage order. Assert exact named links/output and evaluate input RGB × 0.5, preserving alpha, negative and above-1 values. Use binary-exact values where possible. |
| Branching graph | Hand-authored fan-out with off-spine nodes; verify every fixed input and output after disk round-trip. Existing test-only join/Mask fixtures exercise ordered Slot links and secondary outputs through the same codec. Production loader must refuse test kinds. |
| Source and Output | Use Source ID other than `source`; documentOutput points to a non-last node (and a secondary Image output in a fixture). Restore exactly. Source-only grade succeeds; duplicate/missing Source or Mask documentOutput fails unchanged. |
| IDs and Slot order | Store Slot IDs `[3,1,2]` with distinct connections/settings; reorder and round-trip. Compare explicit owner/ID/link tuples, not serializer output. IDs remain valid above 2^53. |
| Allocator restoration | Allocate through 8, delete 8, save/reopen, delete all survivors, allocate 9. Also reopen after all Slots were deleted, delete a restored maximum before first allocation, and round-trip exhausted `UINT64_MAX`. Too-low/zero/overflow high-water values reject. |
| Missing/unknown values | Missing OFX identifier and absent CTL script retain ID/interface/params/links with warning. Unknown nested parameters, Unicode, escaped controls, null and long decimal tokens survive. Unsupported processor schema stays opaque; unknown graph kind protects the file. Installed known parameter type/choice/setter errors must not silently become defaults. |
| Empty vs absent grade | Fresh Source has no grade and does not save automatically. Explicit Save Grade and deletion of last processor leave Source-only grade with colours. Reopen preserves presence, independent of `hasProcessingOutput()`. |
| Metadata-only | Pick/Reject/Unmarked and omitted mark load without grade, decode or processor construction. Updating mark on an absent file creates metadata only. No default preset runs in Step 7. |
| Independent updates | Grade replacement retains mark and unknown metadata; mark update retains grade's raw parameters/topology/layout/colours. Alternate store/UI updates and detect conflicts rather than reverting another edit. |
| RAW and Output | Save valid stable IDs, change global defaults, reopen and assert original pairs. Test raster grades retain RAW transfer pair. Inject RAW decoder success/failure at a narrow source-loading seam, assert requested encoding and coherent published pixels/output; retain a real RAW/macOS manual check. |
| Strict/protected reads | Truncated JSON, bad nested scalar, duplicate keys, trailing junk, null grade, unknown colour, missing port, dangling link, cycle (including disabled/off-spine links), newer version, nonregular/unreadable path and old formats all leave original bytes and live document intact. Do not depend on chmod failing when tests run as root; inject I/O failure. |
| Failed writes | Inject failure at create/write/flush/close/pre-replace and replace; compare original bytes, dirty state and owned-temp cleanup. Modify destination between read and recheck and assert conflict. Successful candidate must parse completely. Test post-rename durability reporting separately. |
| Replace | Reused IDs still reset selection/filter/inspection; destination metadata survives; RAW re-decode and processor construction failure keep old processors/pixels/colour. Use existing deterministic runtime gates, not sleeps. |
| Linear Append | Selected A gives `S→A→X'→Y'→B`; none gives `S→A→B→X'→Y'`. Verify fresh IDs, same internal links, unchanged metadata/destination colours and one commit. Source-only destination works; empty incoming no-op; branching/stale selection and construction failure leave destination untouched. |
| Clipboard fragments | Use existing TestClipboard's in-memory ImGui callbacks. Copy a node from a branching graph; paste twice and prove disjoint IDs. Multi-node fixture remaps fixed/Slot links, keeps Slot order/high-water, drops external links with warnings and leaves documentOutput unchanged for detached insertion. |
| Browsing | Open A then B, change selection/preview/theme, switch folder, invoke normal shutdown save action: no photo sidecar is created. Repeat with a metadata-only file and assert it still has no grade; load a saved grade and assert no rewrite without edits. |
| Global preferences | Inject temp settings root and working/photo directories. Round-trip all inventoried choices and layout. Exercise startup/folder switches/shutdown with no legacy files and with sentinel legacy files; assert no new folder/CWD files and identical sentinel bytes. |
| Source-only export boundary | Retain D038 tests: exporting a derivative, including collision-like source names, never creates or updates any RawNode sidecar. |

Retain the existing `RenderRuntimeTestAccess` barriers, failing OFX fixtures,
`makePreviewSource` failure checks and full self-test suite. Changed lifetime
integration merits ASan/TSan where available; file-only codec tests need no worker.
For key tests, demonstrate that dropping a branch, deriving output from array
order, resetting nextSlotId, unconditional navigation saves, or truncating before
failure makes the test fail. Byte comparisons are appropriate for original-file
protection/opaque slices; structural assertions are appropriate for re-indented
JSON. No new CI system is needed.

Implementation validation includes normal `./build.sh` and the full executable
`--selftest` command documented in [CLAUDE.md](../CLAUDE.md). Record platform-specific
atomic-write and real RAW/native-dialog evidence separately; a fake RAW success
does not establish LibRaw behaviour. This documentation-only checkpoint requires
consistency, scope, JSON-example and `git diff --check` validation, not a rebuild.

## 10. Genuine unresolved decisions and review boundary

No demonstrated blocker requires a new graph model, renderer refactor or another
whole-architecture review. The required implementation seams are explicit:
checked Slot allocator persistence, strict parsing, safe file replacement,
grade/default separation, transactional photo publication and batch insertion.

One existing validation prerequisite remains before PR 3 replaces sidecar paths:
the macOS/native-dialog and successful NEF re-decode evidence (or explicit waiver)
tracked in PROJECT_CONTEXT. It is not a new architecture question or a blocker
to this documentation checkpoint. Platform-specific atomic replacement also
needs implementation-time verification on the supported platforms.

This proposal selects concrete initial policies: protect unknown graph operations,
keep required disconnections representable, fail protected photo opening without
fallback, support only linear attached Append, and preserve branching fragments
through detached insertion. Independent review should challenge the schema and
these boundaries before PR 1 establishes them in code. They are proposals here,
not claims of completed implementation or independent approval.

Remaining advanced behaviour belongs to later features: general branched preset
attachment, asset relinking/bundling, default-preset UI/application, culling UI,
multi-photo actions, Graph View viewport placement, new Mask/mixer controls and
their operation versions. None is required to implement this plan. Exact JSON
reader utility organization and native file APIs are implementation choices
constrained by the acceptance tests, not unresolved product architecture.

Stop after the checkpoint PR for independent Claude review. Do not merge it or
begin Step 7 production implementation as part of this task.
