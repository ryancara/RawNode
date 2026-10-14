# Step 7 — Graph-native persistence design checkpoint

**Review provenance:** Claude's independent review of
`a38e05adecf6713c9456efc52232f28c3d58abd4` concluded **SAFE AFTER SMALL FIXES**
(result supplied by the user). This revision incorporates those fixes and the
subsequently accepted ART/vkdt-inspired save lifecycle. Targeted independent
Claude verification is pending. Documentation only; implementation has not begun.

**Baseline:** `main` including PR #61,
`20e3ba75165e38edf9bd97a6266342dde0a41b1d`. The accepted Step 5/6 graph and
renderer need no architectural replacement. Extend the existing persistence
records, graph validation and document-mutation path.

Read with [PROJECT_CONTEXT.md](PROJECT_CONTEXT.md#persistence-direction),
[CODE_STYLE.md](CODE_STYLE.md), [ARCHITECTURE.md](ARCHITECTURE.md),
[DECISIONS.md](DECISIONS.md) D038–D050 and the
[Step 5 persistence target](STEP5_GRAPH_CONTRACT.md#step-7-graph-persistence-target).
[SIDECAR.md](../SIDECAR.md) and [workspace-format.md](workspace-format.md)
describe the existing formats until their implementation is retired.

## 1. What changes in the existing code

| Existing code | Finding and minimum change |
| --- | --- |
| `GraphModel.{h,cpp}`: `Node`, `GraphView`, `InputBindings`, `OutputRef`, `Slot` | Reuse directly. Consumers already own connections; `App::nodes` includes Source. `nodeInterface()` supplies typed ports even for an unavailable unary processor. Only Source and ImageProcessor are production operations today. |
| `GraphValidation.cpp` | Reuse `graphStructureError` for IDs, endpoints, declared ports, connection types, Slots and cycles, including disabled/off-spine nodes. `requestedGraphError` checks rendering readiness separately; a disconnected graph may still be saved. |
| `Node::makeSlot`, `observeSlotIds`; `GraphCommands.cpp` staging | The private `nextSlotId_` survives deletion/reordering in memory, but cannot be captured/restored. Add narrow checked access to that state. Saving only surviving SlotIds loses allocation history. |
| `NodeGraph.cpp`: `makeNodeId`, `restoredNodeId` | Collision-checked `node-N` allocation is sufficient. Loading referenced IDs must reject duplicates/empty IDs instead of silently repairing them. Append needs a complete old-to-new map before reconnecting nodes. |
| `ProjectPersist.h`: `PersistNode`, `PersistChain` | Extend `PersistNode` with operation and topology; replace the chain with a plain graph record containing nodes and endpoints. Reuse `InputBindings`, `Slot` and `PersistGradeColor`. These are temporary serialization data, with no editing/evaluation API or persistent shadow of `App::nodes`. |
| `captureChain`, `captureNode`, `restoreChain`, `replaceChain` | Current capture refuses graphs outside a complete linear spine; restore recreates Source and connections from array order. Replace with full graph capture and two-pass restoration. Publish saved `primarySource` as well as nodes/output. Remove each V2 guard only when its caller has transitioned. |
| `capturePersistedNode`, `restoredNode`, parameter helpers | Reuse factories, stored backend/identifier/label, `preservedParamsJson`, stable choice IDs and precise numeric output. Check known parameter types and setter results; current permissive conversions can lose invalid values. Bind OFX by identifier, never substitute a different plugin by matching its label. |
| `ProjectPersistLoad.cpp` / `ProjectPersistSave.cpp` | Current field extraction is not complete JSON validation; it can miss malformed tails/duplicate keys. Replace it with one strict reader, retaining raw values/sections through that reader where practical. Existing extraction helpers are available if a small validated-slice helper is still needed. `writeFile` directly truncates its target; replace it with checked atomic writes. |
| `DocumentActions.cpp`: `applyGrade`, `openPath`, sidecar helpers | `applyGrade` already stages restoration and changed RAW decoding under `DocumentMutation`. Reuse this pattern. Opening currently reads a sidecar twice and publishes the source before completing grade checks. Read once and stage the incoming document before publishing it. |
| `AppState.h`, photo colour controls | The open source path and existing protection state identify an editable document; no live grade-presence or dirty flag is needed. Stop photo RAW edits changing `rawWorkingEncoding`, the session default; separate photo colours from defaults. No per-edit persistence notifications. |
| `saveCurrentInputSidecar` and callers | Existing navigation saves can be retained with checked results; include same-photo reopening. Add the initial save after a successful open. Move shutdown saving before `UI.cpp` tears down workers/editor state. Remove the RAW-control-triggered save; use open/leave/close and explicit Save Grade. File → Save Project must report the real result. |
| `UiFrame.cpp` | Presets and clipboard both use `PersistChain`, but separate envelopes and `.rawnodepreset`. Use shared codecs, one Save Preset action for the complete grade, and retain Copy Node / Copy Grade. Remove Save Node Preset and its special reconstruction path. Full-grade actions currently use `hasProcessingOutput()` and can omit colours for protected files; neither is a valid completeness test. |
| `Node::groupOpen`, node capture/restoration | Existing persistence transfers parameter-group expansion. Keep it only in memory with ordinary UI defaults on reconstruction; exclude it from sidecars, presets and every clipboard payload. |
| `PersistGui`, `persistWorkspace`, `ImGuiBackend.cpp` | GUI state mixes photo Output with preferences. Folder opens load/save workspace JSON and swap ImGui ini paths; startup otherwise uses CWD `ofxrawhost.ini`. Separate preferences, use one global ini path and keep folder scanning. |
| Existing tests | `SelfTest.cpp` covers parameters, Unicode/precision, missing processors, colour protection and transfers. Graph model/command/evaluator/runtime suites cover topology, Slot identity and mutation barriers. Extend these suites; no new framework. |

## 2. File shape

Use UTF-8, readable JSON with `"format": "rawnode"` and integer `"version": 1`.
This discriminator distinguishes the new format from historical Sidecar V1/V2.
A source sidecar remains `imagePath + ".rawnode.json"`. A preset uses the same
format and `grade` object, without `metadata`; any sidecar containing a grade
can also be used as a preset. The photograph is supplied by the open/apply
operation, so the graph's Source needs no filename or copied pixels.

One representative sidecar:

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
      "primarySource": "source",
      "documentOutput": { "node": "exposure-1", "port": "image" },
      "nodes": [
        { "id": "source", "kind": "source" },
        {
          "id": "exposure-1",
          "kind": "image-processor",
          "backend": "native",
          "identifier": "rawnode.native.exposure",
          "enabled": true,
          "params": { "exposure": 0.5 },
          "inputs": { "image": { "node": "source", "port": "image" } }
        }
      ]
    }
  }
}
```

`grade` and `metadata` are optional objects, never `null`. New metadata writes use
`pick` or `reject`; absent `metadata.mark` is canonical Unmarked. The reader
also accepts explicit `unmarked`; clearing a mark removes the key, without
deleting the sidecar. An empty metadata object may remain. Omitting `grade`
means no grade saved in this file, valid for unopened metadata-only photos.
An intentionally empty grade contains its colours, one Source node and
`documentOutput` referring to that Source's `image` output.
An empty `nodes` array is invalid. A header-only file has no grade or mark, but
RawNode has no reason to create it during scanning or thumbnail generation.

A grade requires both colour pairs, using `rgbGamutId` / `transferFunctionId`
and the existing registry for validation. Keep a per-document RAW working pair
also for raster photographs: it is unused by raster decoding but makes grade
transfers complete. Initialise it from defaults for an ungraded photograph;
Replace supplies it from the incoming grade. `inputEncoding` continues to mean
the actual decoded pixels. No stored grade depends on later global defaults.
This correction retains the required RAW pair for raster grades; omitting it
would require a separate product decision.

Node records follow the existing model:

- `id` is a nonempty NodeId; `kind` is a stable operation name, never an enum
  ordinal. Source has only these fields and is always enabled.
- `image-processor` requires nonempty `backend`/`identifier` strings, boolean
  `enabled`, `params` and `inputs` objects. Backend/identifier identify native,
  OFX or CTL processors; an optional `label` is informational. Parameter-group
  expansion and selection are transient UI state and are never serialized,
  including in clipboard transfers. Group expansion gets no global preference.
- `params` maps parameter IDs to JSON values. Keep opaque unknown values in
  the existing `preservedParamsJson`; do not constrain them to today's scalar
  types. Known stable choices use IDs; backends without choice IDs keep their
  current numeric representation. Transient/non-persistent parameters stay out.
- `inputs` maps declared fixed input names to `{ "node", "port" }` or `null`.
  Writers include every declared input, even if disconnected. Source has none.
- Slotted operations additionally require `slots` and `nextSlotId`, including
  when their Slot array is empty. Ordinary Source/processor nodes omit both.
- Optional `grade.positions` maps NodeId to `{ "x": number, "y": number }`.
  Keep positions separate from topology and retain them without Graph View.
  Missing positions are valid; ignore unusable entries or keys for absent nodes
  with a warning. Delete a node's position with the node. Positions never affect
  graph validity or execution. Transfer rules are in section 6.

The file version defines the supported operation names and their interfaces.
Use `nodeInterface()` for port types, directions and requiredness: repeating
those declarations in every node is unnecessary. The current missing
ImageProcessor has that same known unary interface. Unknown backend/processor
identities can therefore survive as missing processors; an unknown operation
or graph semantics that cannot be interpreted protects the file instead.
When reading, start from the operation's declared defaults and overlay saved
values. A newly introduced optional fixed/Slot input may be absent: default it
to unconnected. Optional controls use documented defaults that preserve prior
meaning (currently `SlotSettings.enabled` defaults to true). Missing required
fields/inputs or a present value of the wrong type still reject; an explicitly
null required input remains representable but not render-ready. Do not invent
controls to exercise this policy. Incompatible changes revise the file contract;
no per-node version counters or migration machinery are needed.

CTL's identifier remains its existing canonical script path. If moved to a
machine without that path, retain a missing processor and its parameters. This
is a known portability limit, not a reason to introduce asset relocation,
bundling or a parallel asset list. LUT/DCTL requirements wait for those backends.

## 3. Graph, port and Slot identity rules

Capture every node and binding, including disabled, disconnected and off-spine
nodes. Node-array order is storage order only. Restore node identities first,
then bindings and the explicit Source/Output references. There is exactly one
Source and an existing Image output endpoint, enforced by `graphStructureError`.
Connections live only on consumer inputs; there is no second edge list. Several
consumers may name the same producer, and fixed/Slot ports use the same reference
shape. Display labels never resolve identities or ports.

Opening and Replace preserve valid saved NodeIds, including Source identity.
Append/paste allocate fresh IDs against both the destination and the pending
insertions, then remap every internal reference. Reuse the existing counter and
collision checks; NodeId has no node-local non-reuse/high-water contract needing
another saved counter. Never repair duplicate IDs in a loaded graph by guessing
which node a reference meant.

This partial node record shows the Slot fields using the existing test slotted
interface (`reference` is Image; each Slot has Image and optional Mask inputs):

```json
{
  "inputs": { "reference": { "node": "source", "port": "image" } },
  "slots": [
    {
      "id": "3",
      "settings": { "enabled": true },
      "inputs": { "image": { "node": "a", "port": "image" }, "mask": null }
    },
    {
      "id": "1",
      "settings": { "enabled": false },
      "inputs": { "image": { "node": "b", "port": "image" }, "mask": null }
    }
  ],
  "nextSlotId": "5"
}
```

Slot-array order is semantic; moving a Slot carries its settings and inputs.
Here IDs 2 and 4 were deleted; restoring from `max(live IDs) + 1` would wrongly
reuse 4. Save the existing allocator high-water value instead. SlotIds and
`nextSlotId` are decimal strings representing nonzero `uint64_t`, avoiding JSON
number precision loss. Require unique SlotIds within a node and a next value
above all surviving IDs. Preserve `UINT64_MAX` as the existing exhausted
sentinel (no further allocation, and no wrap), including after Slots are deleted.
It may equal a restored maximum ID. Checked capture/restoration must also
observe live IDs as `StagedEdit` does today. Copying a node preserves its local
SlotIds/high-water value; duplicating a Slot within that node allocates a fresh ID.

No production slotted or Mask operation exists yet. Exercise these fields with
the existing test interfaces through test-only access to the shared codec;
do not expose `Test*` operation names in the production loader. The common Slot
representation is needed by the established graph contract, not a Step 8
implementation or a registry for hypothetical operations.

## 4. Opening establishes the grade

This lifecycle supersedes the earlier rule that viewing a photo in the editor
must leave it without a saved grade. Folder scanning/discovery and thumbnail
work never create grades or sidecars; successfully opening an editable photo does.

| On disk | Meaning on editor open |
| --- | --- |
| No sidecar | Initialise from applicable defaults; save the established grade after successful opening. |
| Metadata only | Initialise and save a grade, preserving metadata. |
| Grade present | Restore graph and colours, including Source-only grades; no immediate rewrite is needed. |
| Invalid, unreadable, unsupported or incompatible sidecar | Protected, read-only source view if decoding succeeds; a persistent warning says the saved grade was not loaded. Never classify it as unedited or apply a default preset. |

Every successfully opened editable photo has a complete live grade. Reuse the
open source path and protection state; add neither `hasGrade` nor `gradeDirty`.
Optional grade presence belongs to the file record for unopened metadata-only
photos, not another live state machine. Capture/save the current complete grade
before navigation, folder-driven document replacement, same-photo reopening
and shutdown, whether or not any adjustment changed. Explicit Save Grade remains.
No per-edit notifications, comparison snapshots, revision counters, history or
mutation event bus are needed. Saving is independent of rendering demand and
`DocumentMutation::changed()`; no autosave timer is introduced.

For photograph opening:

1. Save the current editable photo before replacing/reloading it, including the
   same path. On failure offer Retry / Discard / Cancel; retain the live document
   until a save succeeds or discard is explicit. Read the incoming sidecar after
   this save so same-photo reopening cannot reload stale settings.
2. Stage incoming source, graph and colours. Restore an existing grade; otherwise
   initialise from global defaults (or the future selected default grade preset
   via Replace). Never inherit the previous photo's Output setting. Saved RAW
   settings determine decoding; restore nodes/parameters/bindings, validate and
   prepare preview pixels before publishing anything.
3. Use the existing `DocumentMutation` boundary for processor lifetime and live
   replacement. Complete fallible preparation before swapping path, source
   pixels/encoding, preview, graph endpoints and Output settings. Reset selection/
   inspection as today. Failed source opening leaves the old document intact
   and creates no initial sidecar.
4. Only after a successful editable open, if the file had no grade, save the
   established complete grade through the atomic update path. Preserve any mark.
   A Source-only grade is legitimate. If this initial save fails, keep the opened
   photo editable in memory and report the failure clearly; retry saving at
   navigation/shutdown without a special pending/dirty flag. Existing grades
   need no immediate rewrite. Folder auto-open affects only the photo opened.
5. If sidecar parsing or reconstruction fails, protect it and stage a source-only
   view instead. Use the established Rec.709/Linear RAW fallback when the saved
   encoding cannot be trusted; raster decoding uses its detected encoding.
   Source display uses actual `inputEncoding`. Show a persistent warning and
   never save this fallback. If source decoding also fails, keep the old document.

Protected viewing disables graph/Slot/layout edits, enabled-state changes,
parameters/colours, node/grade paste, preset Replace/Append and Save Grade.
Reject these at document entry points as well as in the UI using existing
protection state. Viewing, selection and navigation remain available. No recovery
editor, force-overwrite command or Save As recovery workflow is needed.

Missing ImageProcessors retain identity, enabled state, parameters and bindings,
using the evaluator's existing warned unary pass-through. Unknown operations
have no guessed fallback. Validate known parameter types and restoration results;
retain unknown parameter IDs/JSON and never overwrite an unknown stable choice
with a default. Incompatible installed settings protect the photo instead of
publishing a supposedly restored grade. Backend creation failures may use the
existing missing-processor placeholder. Preset/pasted grade application failure
leaves an open destination's original graph/colours/pixels intact.

Read a complete sidecar before accepting it. Use one strict JSON reader that
checks UTF-8/escapes, JSON number grammar, duplicate decoded keys at every level,
trailing data and schema types/ranges; do not round opaque parameter numbers.
Reject unsupported structural fields/semantics rather than discarding them.
Apply documented optional defaults, then reuse `graphStructureError` on temporary
existing Nodes with null processors, solely for topology validation, never a live
App graph. `requestedGraphError` is render readiness, not file/save validity:
an unfinished grade with explicit disconnected required inputs remains saveable.

## 5. Independent updates and atomic saves

Use one small file update path: **read → validate → change one section → write
a sibling temporary file → atomically replace**. It returns success or a useful
error with the path; every caller must handle the result.

- A grade save captures the current graph/colours and preserves existing
  metadata. A mark update preserves the existing grade, or its absence. Prefer
  keeping the untouched section's JSON exactly, including numerical tokens and
  internal formatting. No meaningful state, precision or opaque parameter data
  may be lost, whichever representation is used.
- Metadata updates parse the stored format, colours and topology as above but
  never instantiate OFX, CTL, native or other processors, decode an image, build
  a live graph or open it in the editor. Parameters remain opaque. This same
  function can serve future batch flag changes without a second validation system.
- Unsupported structure/version/colour IDs, read errors and an active protected
  path refuse writes. Metadata validation need not prove processor availability
  or execute known parameters to preserve their JSON safely. Missing processors
  alone are not a protected-file condition. A protected file is never rewritten
  through a metadata shortcut.
- Read the destination at save time, preserving the other section and
  checking protection even for direct writers. An inaccessible/nonregular target
  is not an absent sidecar. An absent file becomes metadata-only on a mark update
  and gains a grade when editor opening establishes it, or on another grade save.
- Validate the outgoing document before touching the target. Create a unique
  temporary file in the same directory, check write/flush/close errors, then use
  the platform's atomic replacement operation. Never delete the destination
  first. On failure, remove only our temporary file; keep original bytes and
  the complete live document. No watcher, journal or recovery service is needed.

Prefer the strict reader's source spans/raw-value or lossless serialization
support for unchanged sections and opaque parameters. Existing `extractObject`
and raw-value helpers can extract exact text without numeric conversion after
validation if that is simpler, but are not a required second parsing layer.
Do not maintain overlapping JSON validators or a preservation framework.
Validate the outgoing whole document with the same reader; capture/parse failure
aborts the update. Exact formatting is preferred where practical; preserving
meaning, precision and opaque data is mandatory. Newly edited known numbers
keep the existing locale-independent, round-trip precision output.

Normal use is one RawNode editing session. Reading the destination protects
unsupported data and preserves the other section; it does not compare against
a loaded snapshot. External modification detection, revision tracking, file
watchers, multi-process locks and conflict UI are outside Step 7. Atomic replacement
is still required. Verify its behaviour on supported platforms without a
copy-over fallback; claim no stronger cross-process or power-loss guarantee.

Report failed saves through the existing meaningful-operation status policy.
Save every open editable grade on departure/reopen/close, including Source-only
and unchanged grades. Retry / Discard / Cancel handles a failed save; only an
explicit discard permits losing unsaved changes. On close, decide before stopping
workers, setting shutdown intent or destroying editor state. Cancel clears the
close request and returns to the usable running editor; Retry can save again.
Initial-save failure follows this same path on departure, with no extra flag.
Protected source views are never saved and can be left normally. Export remains
sidecar-free (D038).

No readers/migrations for Sidecar V1/V2 or `.rawnodepreset` are required. Existing
files are never silently overwritten/deleted. An old `.rawnode.json` is protected;
a lone legacy `.ofxrawhost.json` is detected as protected legacy state without
parsing/migrating it. Do not silently create a replacement grade alongside it.
When a valid new sidecar also exists, use it and leave the legacy file untouched.

## 6. Presets and clipboard

**Replace:** require a grade, ignore incoming photo metadata, stage the whole
graph and both colour pairs, and bind Source pixels to the destination photo.
Retain valid incoming NodeIds, including its Source ID. Decode RAW again when
needed and prepare the preview before publishing, using the `applyGrade`
transaction pattern. Preserve destination metadata and global defaults. Failure
leaves the previous graph, colours and source pixels intact. Replace also
replaces positions with the incoming grade's valid positions; absent positions
clear the old arrangement, even where NodeIds happen to match.

**Append:** require a complete ordinary linear incoming ImageProcessor chain
with no off-spine nodes, auxiliary connections or Slots. Use `graph::imageSpine`
plus full node coverage and operation checks for the incoming preset. A branched
destination is allowed if it is structurally valid and has a main-spine insertion
point resolvable by `imageSpine`; do not require destination node coverage.
Insert after the selected main-spine node, or at document Output if unselected.
Refuse stale/off-spine selections and unsupported spine operations.

Allocate fresh NodeIds first; map incoming Source.image to the insertion
predecessor, remap internal connections, then connect the tail only to the chosen
main-spine successor's Image input (or update Output at the end). Leave every
other destination branch/consumer connected as before. Do not add a second
Source or implicit colour conversion. Preserve destination colours, metadata
and positions; copy incoming non-Source positions under remapped IDs only.
Missing positions stay absent; automatic placement/offsets wait for Graph View.
A Source-only incoming Append is a no-op.

Stage and validate the entire splice. `StagedEdit` currently commits at most
one addition, so extend that existing mechanism narrowly for a batch of linear
insertions; repeated committing insert calls could leave half a preset applied.
Attaching branched incoming presets is deferred to graph-editing needs. File capture,
opening and Replace still preserve supported branching topology fully.

One **Save Preset** operation saves the current complete grade, including colours
and optional positions, without photo metadata. It handles one processor,
multiple processors, branching graphs and Source-only grades identically. Remove
Save Node Preset, the separate Save Full Grade Preset distinction and special
node-preset reconstruction. Applying a preset offers explicit Replace/Append
in the existing menu/dialog flow. Use `.rawnode.json` filters; no preset browser.
Allow Copy Grade / Save Preset for an editable Source-only grade. Disable both
for a protected source view, which has no successfully restored grade to supply.

Use ordinary confirmed overwrites at any chosen preset path, including a photo's
sidecar path; no photograph-sidecar filename restriction. Read and strictly
validate the destination, preserve its metadata, replace only its grade and
write atomically. New presets omit source-photo metadata; overwriting an existing
sidecar preserves that destination's metadata rather than copying the source's.
Confirmation does not permit replacing an unsupported/protected file.

Keep Copy Node / Paste Node and Copy Grade / Paste Grade using the existing
clipboard interaction. Preset saving always concerns the whole current grade;
node copy remains a distinct editing action. Full-grade clipboard data uses the
same grade encoder/decoder and Replace path.
For a node/fragment, use a thin `rawnode-transfer` version 2 envelope containing
`kind: "fragment"` and `nodes` encoded by the same node functions; it needs no
Source, document Output, metadata or colours. Capture any valid supported node
or fragment without requiring the source graph or fragment to fit a future paste
destination. Preserve enabled state, parameters, internal NodeIds/connections,
Slot identities/order/settings and allocation state, excluding group expansion.
Optional fragment `positions` use the same NodeId-to-X/Y map. Source is not
copied into a node fragment: its connections, like other external incoming
connections, are deliberately omitted and become null. Report omitted external
Mask/Reference connections even if their producer was Source. Retain all
internal Image/Mask/Reference connections. Never resolve omitted references by
matching destination NodeIds or silently restore an external Source connection.

Attachment is a separate check: initially paste only ordinary linear fragments
at the same validated insertion points as Append, explicitly rebinding the entry
Image input. Refuse branching/incompatible attachments without altering either
the destination or clipboard, rather than restricting what may be copied.
An ordinary entry Image input formerly fed by Source or another external node
is explicitly rebound to the destination insertion predecessor. External
Mask/Reference inputs remain unconnected; any declared default for an absent
Reference is operation semantics, not an implicit reconnection. Internal
Mask/Reference dependencies cannot be dropped to make an attachment acceptable.
Allocate fresh NodeIds for every pasted node, rewrite all internal references,
preserve node-local Slot identities and remap any copied positions. Preserve
existing destination positions; node deletion removes its position. Use the same
staged reconstruction/splice as Append, including branched destination handling.
A full-grade transfer envelope carries `kind: "grade"` and the shared `grade`
object. This is a transport wrapper sharing graph serialization/reconstruction;
it needs no general fragment-attachment API.
Version 2 replaces the existing chain-based clipboard payload; no legacy reader
is needed.

## 7. Global preferences replace workspace persistence

Use a small global `preferences.json` and ImGui's existing `layout.ini`, under:

- macOS: `~/Library/Application Support/RawNode/`
- Linux: `$XDG_CONFIG_HOME/RawNode/`, or `~/.config/RawNode/`
- Windows, when supported: the user's roaming application-data `RawNode` folder.

Resolve this directory once; never fall back to a photograph folder or CWD on
failure. A few load/save/path functions are sufficient. Preferences JSON can
reuse the checked atomic writer. Configure ImGui's one global ini path before
initialisation/first frame and remove folder-triggered path swapping. No custom
layout format or settings manager is needed; do not promise atomicity for
ImGui's own ini writes.

Retain theme, panel/filmstrip visibility and geometry, preview resolution,
export format/JPEG quality, dock/window layout and RAW/Output defaults. Keep
geometry owned by ImGui where it already is; preserve `filmstripH`/fallback
sizes where still used. Keep RAW/Output default pairs in global preferences using
the existing stable colour IDs. Read and validate stored pairs at startup;
valid stored values initialise new grades. For an absent pair use the built-in
RAW Rec.2020/Linear or Output Rec.709/sRGB; malformed/unknown pairs warn and use
that pair's built-in fallback. Do not merely write defaults that are never read.
The future Settings menu configures these same fields; no temporary default
controls or manual-file-edit workflow is required during Step 7.

Photo controls require an editable photograph (RAW controls a RAW) and change
only that photograph. Opening/applying a grade never writes preferences. Remove
`updateSessionDefault` from the photo RAW reload path. With no photograph open,
photo controls cannot become default-setting controls. Default colour pairs are
separate from the future default grade preset: precedence remains saved grade
(including empty) → future default grade preset via Replace → global defaults.
Step 7 implements neither Settings UI nor default-preset selection/application.
Future plugin/preset paths, default-preset choice, shortcuts and Graph/List
preference belong here when implemented, without
placeholder machinery in Step 7.

Retire workspace JSON read/write calls, per-folder `.ofxrawhost-layout.ini`
switching and CWD `ofxrawhost.ini`. Do not migrate or delete these old files.
Folder browsing/filmstrip scanning remains; active folder/photo can stay
transient. Rename the existing workspace/project menu actions to folder/open
and Save Grade actions as their persistence responsibilities change. There is
no replacement workspace/catalogue file. Update historical format docs with
the implementation that retires their behaviour.

## 8. Focused implementation PRs

Each PR extends the current functions/types and includes its tests. Do not add
unused service interfaces to prepare later PRs. Before replacing affected live
capture/sidecar paths, record outstanding macOS/native-dialog and successful
Nikon NEF colour re-decode evidence from
[STEP6_MANUAL_VALIDATION.md](STEP6_MANUAL_VALIDATION.md), or an explicit documented
waiver for unverified checks. This does not block format-only PR 1.

| PR | Purpose, likely files and acceptance | Dependencies and specific risk |
| --- | --- | --- |
| **1. Graph records and codec** | Extend `ProjectPersist.h`, load/save codec code and `GraphModel` Slot high-water access; adapt node capture helpers in `NodeGraph`. Use one strict reader and documented optional defaults. Add hand-authored accepted/must-reject fixtures plus round-trips covering shuffled storage, branching, endpoints, opaque values and Slots; exclude group expansion. Keep live V2 callers guarded until switched. | Targeted checkpoint verification. Risk: array-order assumptions, overlapping parsers or test operation names leaking into production. Review any small JSON dependency here. |
| **2. Safe sidecar updates** | New-format file read/update/atomic replacement in `ProjectPersistLoad/Save`; processor-independent metadata updates, optional file-grade records and protection/error results. Verify numerical/opaque-data preservation, unchanged sections, canonical Unmarked, zero processor/decode calls and original-file preservation on failure. No live opening switch yet. | PR 1. Risk: treating read errors as absence or rewriting grades through backend defaults. No external-change tracking. |
| **3. Photo opening and saving** | Stage restoration/protection in `NodeGraph`/`GraphCommands`/`DocumentActions`; separate photo/default colours in `AppState`. Update `UI`/`UiFrame` open/close and `LeftPanel`/`ParamWidgets` protection: create the initial grade only after successful opening; save complete grades on leave/reopen/close before replacement/teardown. No presence/dirty flags or edit notifications. Test initial-save failure, read-only protection and Retry / Discard / Cancel with a usable editor after cancellation. | PRs 1–2 and applicable manual evidence/waiver. Risk: initial writes before successful opening, wrong RAW encoding, or teardown before a failed-save decision. Mutation ownership stays unchanged. |
| **4. Presets and clipboard** | Move `UiFrame`, shared persistence/`NodeGraph` helpers and the narrow `GraphCommands` batch splice to the common codec. One Save Preset action with normal validated overwrites; Replace and linear incoming Append into eligible branched destinations. Retain node/grade copy/paste; implement explicit external-link and position rules. Retire old transfer/chain paths. | PRs 1–3 and applicable native-dialog evidence. Risk: changing unrelated branches, partial splice, metadata loss on overwrite or wrong ID/position remapping. |
| **5. Global preferences and retirement** | Separate `PersistGui` preferences; update `DocumentActions`, `UI`, `ImGuiBackend` and menus. Remove workspace persistence, retaining scanning/essential preferences. Read/validate stored default-colour pairs with built-in fallback; test global round-trip and no per-folder/CWD files. Update `SIDECAR.md`/`workspace-format.md`; no Settings UI. | PR 3 colour ownership; land after PR 4 for final retirement. Risk: ini writes before configuring its global path, defaults ignored on read, or photo colours saved as defaults. |

## 9. Deterministic tests

Use the existing `--selftest` runner, tiny image fixtures, temporary directories,
`graph_test` operations and runtime barriers. Adapt existing V2 coverage as paths
transition; no new framework. Hand-author accepted JSON fixtures independently
of the writer, with explicit expected IDs/bindings/colours and numerical results.
Include shuffled node arrays, missing optional inputs/controls, branching/Slots,
metadata-only and Source-only grades; codec-to-codec equality alone is insufficient.

| Case | Independent expected result |
| --- | --- |
| Strict JSON must-reject | Truncation/trailing junk, missing commas, duplicate decoded keys (including escaped aliases), invalid UTF-8/escapes/control characters and invalid JSON numbers reject. No partial grade is published or original file overwritten. |
| Schema/topology must-reject | Wrong format/version/types, null grade, missing required fields/input storage, empty/duplicate NodeIds, missing/duplicate Source, bad Output/ports/types, dangling references and cycles (including disabled/off-spine edges) reject. Include zero/duplicate/overflow SlotIds, absent/too-low allocator state and unknown structural fields/colour IDs. Production refuses test-only kinds. |
| Optional defaults | Independently authored fixtures omitting declared optional fixed/Slot inputs restore null; omitted optional settings use documented defaults, including Slot enabled=true. Missing required input storage or malformed present values reject. Explicit required null remains saveable, with render-readiness failure when requested. |
| Linear graph | Hand-authored Source → gain → offset with shuffled node storage restores exact IDs/edges/output and the known noncommutative pixel result. Array order cannot change execution. |
| Branches/endpoints/Slots | Fan-out and test-only reconvergence preserve every fixed/Slot edge and off-spine node, a non-default Source ID and non-final-array Output. Slot order `[3,1]`, settings and deleted IDs 2/4 survive; next allocation is 5, including after deleting all live Slots and reopening. Exhaustion never wraps. |
| Missing/opaque values | Unavailable processors/CTL paths retain identity, enabled state, bindings and raw parameters. Unknown nested values, Unicode, `9007199254740993` and long decimal tokens survive. Unknown installed choice values never become saved defaults. |
| Scan versus editor open | Scan a folder and generate thumbnails for A/B: no sidecars. Open A successfully: only A gets a complete Source-only grade; opening a folder that auto-opens A has the same result. Failed B decoding creates no B sidecar. |
| Initial and existing grades | Open metadata-only A: retain its mark and add a grade. Open a valid hand-authored grade: restore it without an immediate rewrite; preserve its Source-only grade even if defaults differ. Initial write failure leaves a usable in-memory grade and warning; departure retries even without edits. |
| Leave, close and same-photo reopen | Directly adjust parameters/colours without any persistence notification, then leave/reopen: the complete saved grade contains those values. Same-path reopen saves before reading and retains edits. Departure/close saves even with no adjustment changes; Source-only grades persist normally. |
| Independent metadata/Unmarked | Update an unopened photo's mark with zero processor/decoder calls and no live graph; preserve any grade and its precision/raw data. Grade-only updates retain metadata. Clearing Pick removes mark; absent mark reads Unmarked; explicit unmarked is accepted on read. Metadata-only changes never add a grade. |
| Protection | Corrupt/newer/incompatible and legacy files retain bytes and open a decodable source read-only with a persistent warning. Every editing/paste/preset/save entry refuses; no default preset runs. Failed decoding leaves the old document intact. Leaving a protected source view does not save it. |
| Atomic and lifecycle failures | Inject temp-create/write/flush/close/replace failures: original bytes and live edits remain. Retry succeeds; only explicit Discard permits losing changes; Cancel prevents navigation/close. After cancelled shutdown, preview/editing and a later save still work because no infrastructure was torn down. No chmod-dependent CI assumptions or exceptional dirty flags. |
| Replace/Append | Replace applies graph/colours, retains metadata and replaces positions (absent incoming positions clear old ones). Linear incoming Append at middle/end remaps IDs into a branched destination; only the selected successor/Output changes, all other branches/colours/metadata remain. Branched incoming, stale selection or unsupported insertion fails atomically. |
| Positions | Remove positions on node deletion; ignore stale/invalid entries without changing graph meaning. Append/Paste preserve destination positions and remap copied non-Source positions; missing entries stay absent. No implicit placement or topology change. |
| Save Preset | One action saves the whole grade regardless of selection, including Source-only and branching grades. Normal confirmed overwrite of a valid photo-sidecar path works, preserving destination metadata; unsupported destinations refuse, cancellation/failure preserves original bytes. |
| Clipboard | Copy a supported branch/fragment independently of paste eligibility. Internal Image/Mask/Reference links survive; external Source/Image and Mask/Reference links detach, with relevant warnings. Only ordinary entry Image is rebound on paste; no accidental matching of foreign IDs. Fresh IDs/Slot state/positions remap; unsupported attachment preserves clipboard/destination. |
| Preferences/group expansion | A hand-authored global file with valid non-built-in RAW/Output pairs actually initialises a new grade; missing/invalid pairs fall back as specified. Photo changes never modify defaults. Global/layout round-trip creates no folder/CWD settings files; old files retain bytes. Group expansion appears in no grade/transfer and uses UI defaults. |
| Runtime/export | Existing barriers prove open/Replace waits for old borrowed nodes/pixels, with coherent source/colour publication. Export produces no derivative sidecar and does not update the source sidecar (D038). |

Run the full existing self-test suite for implementation PRs. Keep real Nikon
NEF/native-dialog evidence distinct from synthetic tests. This documentation PR
needs JSON-example/link checks, contract review and `git diff --check`, not a
C++ rebuild.

## 10. Review boundary and remaining questions

Claude found the architecture sound at the reviewed head. Targeted verification
should check the agreed corrections and subsequent lifecycle change before
implementation; this is not another architecture pass. The required RAW working
pair for raster grades is unchanged by this revision.
JSON-library choice and the supported-platform atomic replacement calls are
bounded PR 1/2 implementation choices, not reasons for a persistence framework.
Outstanding manual evidence/waivers apply only to the relevant live paths.

Defer branched incoming attachment, dynamic unknown-operation interfaces, asset
relocation, default-preset/culling UI, Graph View and masks to their actual
features. Keep one authoritative graph and two persistence scopes.

Stop at this checkpoint for targeted independent Claude review. Do not merge
PR #62 or begin Step 7 implementation as part of this task.
