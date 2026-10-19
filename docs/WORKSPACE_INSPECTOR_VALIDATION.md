# Contextual Inspector and workspace — PR 2A

D051 and ROADMAP.md record the owner-approved product direction. Base is PR #79's
squash merge `b0c48624b9f7ca38e1dcd262008c22329a05ef73`. The first branch commit
contains only roadmap/decision updates, before implementation.

## Design and acceptance

One `App::selection` variant contains no selection, a real NodeId, or virtual
Output. `selectedNodeId()` derives a real identity and `selectedNode()` excludes
Source, retaining processor-only copy/parameter eligibility. Output has no NodeId
and selecting it cannot alter documentOutput. List endpoints are fixed entries,
separate from the reorder projection; Graph uses the same selection. Empty left
canvas clicks clear selection and still support drag panning; middle panning
preserves it. View switching retains selection and processor filter; choosing a
different context clears the filter. Photo/grade replacement resets selection,
and deletion clears an ID only if that real node was removed.

The UI remains a presentation layer over App::nodes, GraphCommands,
DocumentMutation, RenderRuntime and existing parameter/colour/persistence paths.
No evaluator, topology, grade schema, colour maths or decoding contract changes.

| Control | Location and ownership |
| --- | --- |
| Open Image, Open Folder, Export | Compact top toolbar and existing File menu; shared dialog/action functions and shortcuts |
| Status and persistence warning | Toolbar status with full-text tooltip; persistence warning remains visibly wrapped |
| Input file, dimensions, decoded pixel encoding | Source Inspector |
| RAW working gamut/gamma | Source Inspector for RAW input only; current photo, never global defaults |
| Processor parameters/filter/groups | Inspector, retaining stable node widget IDs, export exclusion and protected-photo guards |
| Output gamut/gamma | Output Inspector; photo grade and existing display refresh |
| Export format/JPEG quality | Output Inspector; application preferences, including on protected photos |
| Preview resolution | Preview control area; existing rebuildPreview path |
| Searchable native/CTL/OFX picker | Nodes toolbar in both modes; unchanged append insertion |

Default docking is Preview above Nodes, Inspector right, Filmstrip across the
bottom. Ordinary docking/undocking/resizing and saved floating arrangements use
ImGui layout.ini, with the existing global preference path. No old-window aliases,
upgrade code or historical-layout fixture remain. LeftPanel.cpp and its showLeft/
leftW preference fields are removed; processor widgets move from RightPanel.cpp
to InspectorPanel.cpp, and the single picker lives in ProcessorPicker.cpp.

## Verification

Independent expectations come from the owner-specified control mapping and
unchanged production contracts, not copied implementation output. GraphViewTests
uses real ImGui frames/events without a graphics context to check Source/Output
List selection, RAW/non-RAW/neutral contexts, protected RAW widgets, export
preference edits, processor picker append behaviour in both views, selection-only
graph/render/mutation isolation, photo selection reset and deleted-node cleanup.
It also checks new default geometry and serialization/restoration of a newly
customised layout with floating Nodes. Existing canvas geometry, focus, pan/zoom,
protected positions, photo persistence and List/graph-command tests remain.
Preferences/photo tests retain per-photo/default separation, RAW re-decode,
Output display refresh, protected-sidecar handling and atomic-save boundaries.
The full suite retains native/CTL/OFX parameter and export-lifetime coverage.

**Linux results — 2026-10-19:**

- Release CMake build (`CMAKE_BUILD_TYPE=Release`, GCC 14.2.0, CMake 3.31.6): pass.
- All ten focused `--selftest-graph-view` groups: pass, including the new
  contextual Inspector, picker/filter/narrow toolbar and workspace restart checks.
- `--selftest-preferences`, `--selftest-photo` and complete `--selftest`: pass.
  The complete suite includes native/CTL/OFX parameters and export-lifetime tests.
- Debug `-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer`, with
  halt-on-error and leak detection enabled: Graph View and preferences pass.
  LeakSanitizer required execution outside the process sandbox; rerunning there
  completed without address, undefined-behaviour or leak reports in these suites.
- Strict photo UBSan stops at the already documented JPEG signed-left-shift error
  in `third_party/stb/stb_image_write.h:1263`. The unchanged encoder was independently
  reproduced during PR #79 validation (see GRAPH_VIEW_VALIDATION.md). A UBSan
  recovery run completes every photo group with only that finding and no address
  or leak reports. The encoder is untouched; strict photo sanitizer is not a pass.
- Real Xvfb/software-GL GUI smoke: clean 1400x900 workspace, non-RAW Source
  inspection without RAW controls, native/CTL/available OFX discovery, native
  Exposure insertion/editing with visible preview refresh, Graph/Output inspection,
  1000x700 window, Inspector undocking/resizing and normal shutdown/restart pass.
  A floating Inspector restores its position/size; the Exposure value and both
  node positions restore from the photo sidecar, while Inspector selection resets.
  Automated ImGui tests cover fresh docking and new custom/floating ini restoration.
- `git diff --check`: pass.

Real RAW GUI/native file-dialog interaction, comprehensive manual redocking,
macOS and Windows builds/GUI checks remain unperformed. The full sanitizer suite
and TSan were not run; this PR changes UI/selection rather than execution ownership.
Use the platform checklist below; these omissions are not passing evidence.

## Targeted post-review correction pass

The endpoint insertion regressions exercise `copySelectedNode`/`pasteFromClipboard`
and the menu's `loadPresetDialog` action. A supplied preset path skips only the
native chooser; the action still reads the file and decides the insertion anchor.
Source and virtual Output each cover copied-processor Paste and two-processor
linear Append. Expectations check extension of the previous document Output,
every incoming Image link and preservation of existing topology/branches.

The Inspector regression writes a tiny uncompressed linear DNG, opens it through
the normal photo action, selects Source through List UI and clicks RAW gamma's
sRGB choice. It uses the actual LibRaw decoder and Inspector handler. Independent
sRGB OETF and +1 EV expectations verify full/preview pixels and rendered output;
the saved photo gamma changes while in-memory and saved global RAW defaults stay
unchanged, including after saving preferences again.

Inspector now supplies a 420x600 `FirstUseEver` size before Begin. Existing fresh
docking and saved custom/floating layout tests pass; there is no migration.
`canExport()` has only an eligibility comment; `showRight`/`rightW` are unchanged.
Release Graph View (11 groups), transfers (10), preferences, photo and complete
`--selftest` pass. The PR description records the corrected SHA and final CI,
mutation-probe and sanitizer results.

## macOS manual checklist — pending

Record the head SHA, macOS version, display and results. Use a disposable photo
folder and preserve configuration before manually testing a fresh layout.
Historical layout migration is not required; the application must not delete it.

1. Start with a fresh layout: Preview above Nodes, Inspector right, Filmstrip
   bottom, compact file toolbar. Resize to a small MacBook window; verify all
   buttons, Nodes toolbar wrapping, Inspector widgets and Preview resolution.
2. Open RAW/NEF and non-RAW photographs. Select Source in List and Graph: inspect
   file/encoding information; RAW controls appear only for RAW input. Edit RAW
   gamut/gamma; confirm preview re-decodes and global defaults remain unchanged.
3. Select native Exposure/CST, CTL and an available OFX processor. Edit/filter/
   group parameters, use parameter-search focus shortcuts, switch List/Graph,
   copy/paste, reorder/toggle/delete in List. Unavailable processors retain their
   saved state and reporting. Delete a selected processor: Inspector becomes neutral.
4. Select Output in both views. Edit colour space/gamma and confirm the established
   output interpretation/display behaviour. Change PNG/JPEG and JPEG quality;
   export with those settings. Selection alone never changes Output wiring.
5. Click empty Graph canvas, pan/zoom/frame and drag Source/processor headers.
   Neutral Inspector must not retain stale widgets; panning still works. Source
   and Output must never be copied/deleted as processors.
6. Test toolbar and File menu Open Image/Open Folder/Export, plus Cmd+O,
   Cmd+Shift+O, Cmd+E and existing copy/paste/Inspector/Filmstrip shortcuts.
   Confirm errors/status and persistent save warnings remain accessible.
7. Resize, dock, undock and redock each panel; restart with custom/floating
   arrangements. New layout and export preferences restore, with no Left Panel.
8. Switch/reopen photographs and restart: photo colours, parameters and node
   positions restore, selection is fresh and application defaults stay separate.
   Protected/invalid sidecars disable grade editing but allow navigation and
   export preferences. During export processor widgets remain disabled; preview
   and export result/warning status restore through the established lifetime path.

Windows builds and GUI validation remain separate where unavailable.

## Deferred scope

PR 2B: typed connection/reconnection/disconnection and document Output editing,
Delete/Backspace/context menus, selected-node insertion and serial-before/after.
PR 2C: predictable serial placement/space, paste/duplicate overlap and contextual
usability; optional Auto Arrange later. Empty pass-through nodes and processor
assignment/replacement remain a separate feature. Step 8's Mask implementation
and subsequent compositing/local adjustments remain unchanged.
