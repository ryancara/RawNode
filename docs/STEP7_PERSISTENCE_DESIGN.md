# Step 7 — Graph-native persistence design checkpoint

**Status:** Revised proposal for independent Claude review. Documentation only;
Step 7 implementation has not begun.

**Baseline:** `main` including PR #61,
`20e3ba75165e38edf9bd97a6266342dde0a41b1d`. The accepted Step 5/6 graph and
renderer need no architectural replacement. Extend the existing persistence
records, graph validation and document-mutation path.

Read with [PROJECT_CONTEXT.md](PROJECT_CONTEXT.md#persistence-direction),
[CODE_STYLE.md](CODE_STYLE.md), [ARCHITECTURE.md](ARCHITECTURE.md),
[DECISIONS.md](DECISIONS.md) D038–D049 and the
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
| `ProjectPersistLoad.cpp` / `ProjectPersistSave.cpp` | Current field extraction is not complete JSON validation; it can miss malformed tails/duplicate keys. `captureJsonValue` / `extractValueField` / `extractObject` can retain untouched object text after strict whole-file validation. `writeFile` directly truncates its target. Replace validation and writing, reusing extraction, naming/filtering and parameter encoding where useful. No general JSON dependency is currently declared; use an established small library rather than grow a custom parsing framework. |
| `DocumentActions.cpp`: `applyGrade`, `openPath`, sidecar helpers | `applyGrade` already stages restoration and changed RAW decoding under `DocumentMutation`. Reuse this pattern. Opening currently reads a sidecar twice and publishes the source before completing grade checks. Read once and stage the incoming document before publishing it. |
| `AppState.h`, `LeftPanel.cpp`, `ParamWidgets.cpp` | Add explicit grade presence/dirty state. Current RAW UI edits also update `rawWorkingEncoding`, the session default; stop that coupling. Separate current photo colours from RAW/Output defaults. Successful parameter and Output edits need dirty notifications. |
| `saveCurrentInputSidecar` and callers | Path changes, folder changes, RAW edits and `UI.cpp` shutdown save unconditionally; write failures are ignored. File → Save Project reports success unconditionally. Use checked save-if-dirty for automatic paths and explicit Save Grade for intentional saves, including an empty grade. |
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

`grade` and `metadata` are optional objects, never `null`. `metadata.mark` is
`pick`, `reject` or `unmarked`; an absent mark means Unmarked. Omitting `grade`
means no saved grade. An intentionally empty grade still contains its colours,
one Source node and `documentOutput` referring to that Source's `image` output.
An empty `nodes` array is invalid. A header-only file has no grade or mark, but
RawNode has no reason to create it during browsing.

A grade requires both colour pairs, using `rgbGamutId` / `transferFunctionId`
and the existing registry for validation. Keep a per-document RAW working pair
also for raster photographs: it is unused by raster decoding but makes grade
transfers complete. Initialise it from defaults for an ungraded photograph;
Replace supplies it from the incoming grade. `inputEncoding` continues to mean
the actual decoded pixels. No stored grade depends on later global defaults.

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
  Include every declared input, even if disconnected. Source has none.
- Slotted operations additionally require `slots` and `nextSlotId`, including
  when their Slot array is empty. Ordinary Source/processor nodes omit both.
- Optional `grade.positions` maps NodeId to `{ "x": number, "y": number }`.
  Keep positions separate from topology and retain them without Graph View.
  Missing positions are valid; ignore unusable position entries with a warning.
  Positions never determine graph validity or execution.

The file version defines the supported operation names and their interfaces.
Use `nodeInterface()` for port types, directions and requiredness: repeating
those declarations in every node is unnecessary. The current missing
ImageProcessor has that same known unary interface. Unknown backend/processor
identities can therefore survive as missing processors; an unknown operation
or graph semantics that cannot be interpreted protects the file instead.
No per-node, interface or processor version counters are needed now. A future
incompatible operation/interface change must deliberately revise the file
contract; Step 7 adds no migration machinery.

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

## 4. Sidecar ownership and safe opening

| On disk | Meaning on open |
| --- | --- |
| No sidecar | Ungraded source, global colour defaults; no automatic write. |
| Metadata only | Same ungraded source plus mark; no grade is created. |
| Grade present | Restore graph and both colour pairs, including a Source-only grade. Loading is clean. |
| Invalid, unreadable, unsupported or incompatible sidecar | Protected, read-only source view if decoding succeeds; a persistent warning says the saved grade was not loaded. Never classify it as unedited. |

Use minimal document state: `hasGrade` (loaded or explicitly authored),
`gradeDirty` (needs saving), and the existing protected path plus a warning
reason that survives routine preview status updates. On editable photos,
successful processing/colour edits, preset application, grade paste or explicit
Save Grade establish a grade. Clearing the last processor retains a Source-only
grade. No-op/failed edits, selection, rendering, browsing and
metadata changes do not establish one. Loading sets presence from the file and
leaves dirty false. Dirty clears only after a successful save. Protection takes
precedence over presence; a fallback view must never qualify for future automatic
default-preset application. Step 7 implements no such automatic application.

Protected source viewing is read-only: disable graph/Slot/layout edits, enabled-state
changes, parameter/colour adjustments, node/grade paste, preset Replace/Append
and Save Grade. Reject these operations at document entry points as well as in
the UI, using the existing protection state. Selection, viewing and navigation
remain available. No extra editability state, force-overwrite command, recovery
editor or Save As recovery workflow is needed.

Notify persistence at successful graph, enabled-state, parameter and colour edit
boundaries. Keep this separate from `DocumentMutation::changed()`, whose job is
render demand, and from renderer busy/publication flags. Optional node positions
may dirty an existing editable grade but do not establish one. Parameter-group
expansion never dirties a grade and is not part of copy/paste.
Metadata updates are synchronous file operations and need no document history.

Read a sidecar completely and validate its format/version, field shapes,
identities, colour IDs and topology before accepting it. Reject malformed JSON,
duplicate keys, trailing data and invalid numeric values. Reject unknown
structural fields/semantics rather than silently dropping them. Opaque parameter
values are the intentional exception, not an arbitrary extension map on every
object. Reuse `graphStructureError` on temporary existing `Node` objects with
null processors, solely as topology for validation, never as a live App graph;
static interfaces make this check backend-independent.
`requestedGraphError` may report disconnected required inputs to the UI but is
not a file-validity or save requirement.

For photograph opening:

1. If the current document is dirty, save before abandoning it; a failure offers
   retry/discard/cancel, retaining edits until a save or explicit discard. Then
   read/classify the incoming sidecar once, so reopening the same path cannot
   restore a snapshot taken before that save.
2. Prepare the incoming source, graph and colours without altering live state.
   For a valid grade, use saved RAW settings for decoding, build every node,
   connect references, restore parameters, validate and build preview pixels.
   No-sidecar/metadata-only opens use global defaults, never the previous photo's
   Output setting.
3. Use the existing `DocumentMutation` ownership boundary for processor lifetime
   work and live replacement. Finish all fallible preparation before swapping
   path, source pixels/encoding, preview, graph endpoints and Output settings.
   Reset selection/inspection as today; one completed mutation requests preview.
4. If sidecar parsing or grade reconstruction fails, protect it and stage a
   source-only view instead. Keep the existing safe RAW display fallback of
   Rec.709/Linear when the saved encoding cannot be trusted; raster decoding
   still uses its detected encoding. Source display uses actual `inputEncoding`,
   as `RenderPipeline` does today. Identify this as a read-only protected source
   view with a persistent warning, no restored grade and no automatic default
   preset. If source decoding fails too, keep the previous document. Do not
   expose a half-restored grade.

The fallback is a complete incoming source document, so it does not conflict
with transactional opening. It preserves current source-viewing behaviour.
Applying a preset/pasted grade to an already open photograph instead leaves
that photograph's original grade unchanged on failure; it never falls back to
clearing the destination.

Missing ImageProcessors retain identity, enabled state, parameters and all
bindings, using the evaluator's existing warned unary pass-through. This does
not justify fallback semantics for unknown operations. For installed processors,
validate known parameter values and check restoration results. Preserve unknown
parameter IDs/JSON; never replace an unrecognised stable choice with a default
on save. Keep the current protection for such incompatible installed settings:
report the failure and do not publish a supposedly restored grade. Backend
creation failures can use the existing missing-processor placeholder.

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
  and gains a grade only on an intentional grade save.
- Validate the outgoing document before touching the target. Create a unique
  temporary file in the same directory, check write/flush/close errors, then use
  the platform's atomic replacement operation. Never delete the destination
  first. On failure, remove only our temporary file; keep original bytes and
  in-memory dirty state. No watcher, journal or recovery service is needed.

The simplest section-preservation approach is to validate the whole original
file with the strict parser, then call the existing `extractObject` for the
untouched `grade` or `metadata`. It already walks direct members via
`extractValueField` / `captureJsonValue`, respects strings/escapes and copies the
object text without numeric conversion. Reassemble the small envelope with the
modified section and retained text, then strictly validate the whole result
before writing. These helpers extract from already validated JSON; they do not
replace strict validation. An extraction failure aborts the update.

This reuses less machinery than adding lossless numeric/formatting preservation
to a general DOM serializer. If the selected parser supplies a simpler equivalent
raw-section or lossless serialization facility, use it. Exact whitespace is
preferred, not a reason for a general JSON preservation framework; precision and
opaque data remain mandatory. Keep existing raw parameter capture for unknown
values when a grade itself is reconstructed and saved.

Normal use is one RawNode editing session. Reading the destination protects
unsupported data and preserves the other section; it does not compare against
a loaded snapshot. External modification detection, revision tracking, file
watchers, multi-process locks and conflict UI are outside Step 7. Atomic replacement
is still required. Verify its behaviour on supported platforms without a
copy-over fallback; claim no stronger cross-process or power-loss guarantee.

Automatic save callers skip clean documents and absent grades. Save Grade is an
explicit way to persist an empty grade. Check failures before navigation, folder
switch or shutdown discards edits; on failure keep the document/window available
and report the error. A simple retry/discard/cancel interaction is sufficient;
discard is explicit. Do not report success or finish destructive teardown first.
Protected viewing alone is clean
and can be left normally. Export remains sidecar-free (D038).

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
leaves the previous graph, colours, source pixels and dirty state intact.

**Append:** initially require both graphs to be complete ordinary linear
ImageProcessor chains, with no off-spine nodes, extra connections or Slots.
Use `graph::imageSpine` plus full node coverage and operation checks; that helper
alone does not reject every branch. Insert after the selected main-spine node,
or at document Output if there is no selection. Refuse a stale/off-spine
selection. Allocate all fresh NodeIds first; map the incoming Source reference
to the insertion predecessor, remap internal references, then connect the tail
to the original successor (or update Output at the end). Do not insert a second
Source. Retain destination colours/metadata; do not insert implicit colour
conversions. A Source-only preset Append is a no-op.

Stage and validate the entire splice. `StagedEdit` currently commits at most
one addition, so extend that existing mechanism narrowly for a batch of linear
insertions; repeated committing insert calls could leave half a preset applied.
General branching attachment is deferred to graph-editing needs. File capture,
opening and Replace still preserve supported branching topology fully.

One **Save Preset** operation saves the current complete grade, including colours
and optional positions, without photo metadata. It handles one processor,
multiple processors, branching graphs and Source-only grades identically. Remove
Save Node Preset, the separate Save Full Grade Preset distinction and special
node-preset reconstruction. Applying a preset offers explicit Replace/Append
in the existing menu/dialog flow. Use `.rawnode.json` filters; no preset browser.
Allow Copy Grade / Save Preset for an editable Source-only grade. Disable both
for a protected source view, which has no successfully restored grade to supply.

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
Deliberately omit incoming references whose producers are outside the copied
set, leaving those inputs null and reporting omitted Mask/Reference links.

Attachment is a separate check: initially paste only ordinary linear fragments
at the same validated insertion points as Append, explicitly rebinding the entry
Image input. Refuse branching/incompatible attachments without altering either
the destination or clipboard, rather than restricting what may be copied.
Allocate fresh NodeIds for every pasted node, rewrite all internal references,
preserve node-local Slot identities and use the same staged reconstruction/splice.
A full-grade transfer envelope carries `kind: "grade"` and the shared `grade`
object. This is a transport wrapper, not
another graph model, serializer or general fragment-attachment API.
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
the existing stable colour IDs. Until the future Settings UI is available, use
sensible built-ins: RAW Rec.2020/Linear and Output Rec.709/sRGB, as today. The
future Settings menu exclusively configures these same defaults; no temporary
default controls or manual-file-edit workflow is required during Step 7.

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
| **1. Graph records and codec** | Extend `ProjectPersist.h`, load/save codec code and `GraphModel` Slot high-water access; adapt node capture helpers in `NodeGraph`. Add strict format parsing, complete graph capture/data reconstruction and round-trip tests. Verify reordered storage, branching, Source/Output, opaque values and Slot allocation; exclude group expansion from records. Keep live V2 callers guarded until switched. | Reviewed checkpoint. Risk: accidentally retaining array-order semantics or leaking test operation names into production. A small JSON dependency, if chosen, belongs here with its footprint reviewed. |
| **2. Safe sidecar updates** | New-format file read/update/atomic replacement in `ProjectPersistLoad/Save`; metadata-only update entry point in persistence headers. Add optional grade/metadata records and protection/error results; reuse validated section extraction. Tests prove precision/opaque-data preservation, exact untouched text where practical, zero processor/decode calls and original-file preservation on failure. No live photo-restoration switch yet. | PR 1. Risk: treating a read error as absence or rewriting the untouched grade through backend defaults. No external-change tracking. |
| **3. Photo restoration and edit state** | Stage graph restoration and protection checks in `NodeGraph`/`GraphCommands`/`DocumentActions`; add presence/dirty and separate photo/default colours in `AppState`; wire edit notifications and checked saves in `LeftPanel`, `ParamWidgets`, `UiFrame`, `UI`. Test coherent source/colour publication, read-only protected viewing, no browsing writes and retry/discard/cancel before losing dirty edits. | PRs 1–2 and applicable manual evidence/waiver. Risk: an unguarded editing entry point or pixels published in the wrong RAW encoding. Existing mutation ownership remains unchanged. |
| **4. Presets and clipboard** | Move `UiFrame`, shared persistence/`NodeGraph` helpers and the narrow `GraphCommands` batch splice to the common codec. Provide one whole-grade Save Preset action, Replace/linear Append and common extension; retain node/grade clipboard actions and separate fragment capture from attachment checks. Retire old preset/transfer readers and remaining chain callers. Test all-or-nothing insertion and destination colour/metadata preservation. | PRs 1–3 and applicable native-dialog evidence. Risk: partial Append, wrong ID remapping or unnecessarily rejecting fragment copy. Unsupported attachments stay refused. |
| **5. Global preferences and retirement** | Separate `PersistGui` preferences; update `DocumentActions`, `UI`, `ImGuiBackend` and menus; remove workspace persistence and obsolete fields/helpers. Retain scanning and essential preferences, including default-colour pairs seeded from built-ins for future Settings. Update `SIDECAR.md`/`workspace-format.md`. Test global round-trip and no per-folder/CWD files; add no default-setting UI. | PR 3 colour/state ownership; land after PR 4 for a clean final retirement. Risk: ini writes before the global path is set, or current photo colours being saved as defaults. |

## 9. Deterministic tests

Use the existing `--selftest` runner, tiny image fixtures, temporary directories,
`graph_test` operations and runtime barriers. Adapt existing V2 tests to the new
format as callers transition, preserving behavioural coverage. A focused
persistence test source in the same runner is fine; no new framework is needed.

| Case | Independent expected result |
| --- | --- |
| Linear round-trip | Save Source → gain → offset with shuffled node storage; restore the same IDs/edges/output and known noncommutative pixel result. Changing array order cannot change execution. |
| Branching and endpoints | Fan-out plus test-only reconvergence retains every fixed/Slot edge and off-spine node. Restore a non-default Source ID and an Output that is not the final array entry. Invalid references/types/cycles reject before publication. |
| Optional positions | Present positions survive by NodeId; absent positions remain valid. Altering positions changes no bindings or pixels; unusable entries do not invalidate an otherwise valid graph. |
| Slots | Order `[3,1]`, distinct settings and deleted IDs 2/4 survive; next allocation is 5, including after deleting all live Slots and reopening. Test duplicate/zero IDs, invalid high-water and exhaustion without wrap. |
| Missing/opaque values | Unavailable backend/CTL path retains identity, enabled state, bindings, label and opaque nested parameter JSON. Unknown IDs/Unicode/precise doubles survive. Unknown installed choice values are not overwritten with defaults. |
| Grade presence | Absent file, metadata-only, saved processing grade, saved Source-only grade and protected file remain distinct. Restoring is clean; successful edits establish/dirty a grade; failed/no-op edits do not. Reopening the same dirty path restores the newly saved grade. |
| Independent metadata | Update mark on an unopened photo: zero decoder/factory calls and no live App graph. The untouched grade preserves meaning, precise large/long number tokens, nested opaque values, connections and formatting via extraction; grade-only save preserves the original metadata text. Cover escaped strings/braces and malformed-tail/duplicate-key rejection. Marking an ungraded photo never adds `grade`. |
| Colour ownership | RAW and Output IDs round-trip independently of global defaults. Photo colour edit and preset Replace never alter defaults; no-photo controls cannot edit them. Ungraded B uses built-ins, not A's settings. Stored default pairs round-trip for future Settings without a new format. Failed RAW reload leaves A intact. |
| Protection and viewing | Corrupt/unsupported/incompatible and legacy sidecars retain bytes. A decodable source opens read-only with a warning that persists through preview refresh; decode failure preserves the previous document. Reject graph/Slot/enabled/parameter/colour edits, node/grade paste, preset Replace/Append and Save Grade without changing state. No default preset runs. |
| Atomic failures | Deterministically fail temp creation, write/close and replace using narrow seams; original bytes and dirty state remain. Retry succeeds; discard alone allows losing unsaved changes; cancel prevents navigation/close. Protected clean views can be left normally. Do not rely on chmod failures under privileged CI. |
| Replace and Append | Replace applies incoming graph/colours and retains metadata; failure changes nothing. Linear Append in middle/end remaps colliding IDs, preserves destination colours/metadata and reconnects successor/Output. Branches/stale selection fail without partial insertion. |
| Preset saving | One Save Preset action writes the whole current grade, including when selection names only one of several processors. One-node, branching and Source-only grades share the same path; no Save Node Preset command or special reconstruction remains. |
| Clipboard | Copy from branching source/fragment succeeds independently of paste eligibility, preserving internal edges and Slot state. Report deliberately omitted external Mask/Reference links. Linear paste remaps fresh IDs and rebinds entry; unsupported attachment leaves clipboard/destination intact. Grade paste includes colours. |
| Transient group expansion | Change group expansion, save sidecar/preset and copy node/grade: no expansion fields appear in any payload, no dirty change occurs, and reconstruction uses UI defaults. Optional positions still round-trip. |
| Browsing and preferences | Open/navigate/close unedited tiny photos without any sidecar. Change theme/preview without creating grades. Round-trip essential global preferences/layout across two folders; no workspace JSON, folder ini or CWD ini is created, and pre-existing legacy files retain bytes. |
| Runtime ownership/export | Extend existing barriers to hold preview/export during open/Replace; borrowed old nodes/pixels remain alive until mutation exclusion. No mixed-source/colour publication. Retain D038 checks: export never writes source or derivative sidecars. |

Run the full existing self-test suite for implementation PRs. Keep real Nikon
NEF/native-dialog evidence distinct from synthetic tests. This documentation PR
needs JSON-example/link checks, contract review and `git diff --check`, not a
C++ rebuild.

## 10. Review boundary and remaining questions

No demonstrated graph/evaluator blocker or unresolved product decision requires
another architecture pass. Independent review should confirm this compact
schema and the stated safety/attachment boundaries before implementation.
JSON-library choice and the supported-platform atomic replacement calls are
bounded PR 1/2 implementation choices, not reasons for a persistence framework.
Outstanding manual evidence/waivers apply only to the relevant live paths.

Defer general branching Append, dynamic unknown-operation interfaces, asset
relocation, default-preset/culling UI, Graph View and masks to their actual
features. Keep one authoritative graph and two persistence scopes.

Stop at this checkpoint for independent Claude review. Do not merge PR #62 or
begin Step 7 implementation as part of this task.
