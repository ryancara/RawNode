# Graph/List editor validation

PR #79 delivered the visual foundation under D047; PR #80 delivered the
contextual Inspector/workspace under D051; and PR #82 delivered focused typed
wiring, Output rewiring, Disconnect and deletion interactions. All three are
merged. See [WORKSPACE_INSPECTOR_VALIDATION.md](WORKSPACE_INSPECTOR_VALIDATION.md)
for the PR #80 control mapping and historical validation. PR 2C placement/usability
requires a separate approved scope. None of these UI PRs replaces the renderer,
evaluator, graph commands or sidecar schema.

## Graph foundation contracts

- App::nodes owns the graph. Canvas ports come from nodeInterface(); wires read
  fixed/Slot input bindings and documentOutput. No second graph or UI edge list.
- List and Graph share App::selection. Real nodes retain stable NodeIds; virtual
  Output has no stored identity. View changes retain selection and parameter filter.
- Iterative dependency columns with stable NodeId tie-breaking place missing
  nodes. A hand-authored diamond requires consumers right of dependencies,
  nonoverlapping branches and visible disconnected nodes, independent of storage.
- Generated and dragged coordinates use app.nodePositions/grade.positions and
  the same existing save lifecycle. Saved positions restore exactly. Protected
  photos derive drawing positions without changing the grade. No movement-time I/O.
- Navigation/layout gestures preserve executable graph JSON, render epoch, queued demand and
  DocumentMutation observations, including when export owns execution.
- Coordinate transforms invert; zoom retains the cursor anchor at both limits;
  Frame All centers/fits with padding when minimum zoom permits.
- Preferences retain List/Graph with independent optional-field fallbacks. New
  custom docking/floating arrangements save/restore normally. Historical window
  layout compatibility and migration tests were deliberately retired in PR 2A.

## Automated checks

```sh
./build.sh
build/OfxRawHost --selftest-graph-view
build/OfxRawHost --selftest-preferences
build/OfxRawHost --selftest-photo
build/OfxRawHost --selftest
```

Use `build/OfxRawHost.app/Contents/MacOS/OfxRawHost` on macOS. GraphViewTests
runs real Dear ImGui frames/input without a graphics context: selection, body/
header dragging, empty-canvas left/middle panning, zoom, focused F and toolbar
framing, List/Graph switching, protected layouts, current Inspector/picker
behaviour and new workspace restoration. Photo save/open tests cover fractional/
negative and automatically generated positions, two distinct photos, failed
atomic saves and viewport reset. Tests at 0.5x/2x verify inverse-scaled movement;
F is checked during text editing, other-panel focus and view changes.

## Historical PR #79 evidence

Linux Release, focused Graph View/preferences/photo and full self-tests passed.
ASan/UBSan Graph View and preferences passed with leaks enabled. Strict photo
UBSan reported the independently reproduced pre-existing signed-left-shift issue
in third_party/stb/stb_image_write.h's JPEG encoder; a recovery run completed with
that finding and no address/leak report. Real Xvfb interactions exercised panning
and preserved all five saved node positions. PR #79's historical ini-upgrade
regression and fixture are removed under D051, not retained as current requirements.
The owner reports independent review, Linux CI and macOS manual validation before
merge; fresh PR 2A platform checks remain separately pending.

## Current limits

Graph provides single selection and header movement, with typed wiring,
input Disconnect, virtual Output rewiring and focused deletion merged in
PR #82. Reorder and enable/disable remain List controls. Add Processor is
available in both modes and retains PR #80's append-to-Output behaviour.
Source is movable and opens input inspection. Output is selectable but derived,
non-draggable and nonpersistent. Pan/zoom reset per photo; zoom spans 0.2–2.5.
Very large layouts may require panning after Frame All reaches minimum zoom.
Saved overlaps are preserved. Frame All scales text with zoom. Multi-selection,
grouping, intentionally empty nodes and production Mask processors remain
outside the delivered Graph View interactions.

## Historical PR 2B scope and independent acceptance criteria

The owner withdrew PR #81's insertion changes. PR #82 started from clean
main and was merged after independent review and macOS validation; PR #81
was closed unmerged. D051 retains its historical record with a superseding
scope clarification. PR 2C placement work has not started.

Only five behaviours are approved: interactive typed wire connections, input
Disconnect, virtual Output rewiring, Graph node deletion and the menus needed
for those actions. Existing GraphCommands validate/commit topology; destroyNode
retains splice/generic deletion and Inspector cleanup. The UI handles gestures,
focus, temporary wires, menus and endpoint validity. GraphCommands must not
manage canvas state. No new graph, scheduler, codec, picker/insertion state,
processor-construction path or document-notification framework is needed.

Output-to-input uses connect or reconnect according to the current binding;
input-to-output can reconnect. Invalid drops report errors and preserve topology;
empty drops/Escape cancel. Required inputs may disconnect. Output accepts Image,
remains virtual and cannot disconnect. Port hits take priority over header/body
movement at different zooms. Existing panning, zoom and framing remain intact.
UI editing eligibility is checked at dispatch, including protected photographs
and export ownership; existing document transaction/lifetime APIs are unchanged.
Cancel on photo/view/interaction changes or missing endpoints using UI lifecycle
and stable endpoint identities, with no raw Node/port pointers across frames.

Delete/Backspace require canvas keyboard focus, no active text control/modifiers
and a fresh press. Source/Output cannot delete. Menus contain only Delete Node
or input Disconnect; no insertion commands or placeholders. Hand-authored
expectations for Source -> A -> B -> Output: deleting A makes B consume Source;
deleting B makes Output reference A. An off-spine deletion retains the main
output. Replacing B's input changes only B's binding; fan-out retains other
consumers. Disconnecting B leaves a structurally valid but unavailable requested
result. When Output is connected to Source, preview evaluation displays the
unchanged Source image; export is disabled under existing application behaviour.
When Output is connected to a valid processing node, preview and export use that
node's result; a +1 EV processor must yield twice the source values.

Use existing real-ImGui frames and graph-command tests. Check exact input/Output
bindings, selection/layout, mutation counts and render demand for accepted,
rejected/no-op/cancelled actions; save/reopen actual sidecars. Retain PR #80
Inspector/workspace/picker regressions. Add Processor must append regardless of
Source/processor/Output selection. Native/CTL/OFX creation, clipboard Paste and
preset Append remain on their existing paths. Do not retain tests of withdrawn
insertion functionality. Before PR #82 was opened, its production diff was
audited against those five approved behaviours and the PR description recorded
the resulting per-file scope assessment.

## Focused PR 2B macOS checklist (owner-reported summary below)

1. Open Nikon NEF; adjust real processors and check preview updates after wiring.
2. Connect/reconnect in both directions; exercise fan-out, invalid types/cycles,
   empty-drop/Escape cancellation and Disconnect/reconnect of required inputs.
3. Rewire Output to Source: confirm the Source preview and disabled export. Rewire
   to a valid processing node: confirm preview and export use that node's result.
   Confirm Image-only typing and existing Output colour/export settings.
4. Delete middle/final/off-spine processors with Delete/Backspace and the menu;
   protect Source/Output and verify Inspector cleanup, text focus and held keys.
5. Verify trackpad pan/zoom, Retina port targeting, header movement and Frame All.
6. Save/switch/reopen and verify exact topology and remaining node coordinates.
7. Verify export/protected-photo safety and cancellation on photo/view changes.
8. Confirm Add Processor still appends in both views for every selection; check
   Native/CTL/OFX, clipboard/preset and PR #80 Inspector/workspace regressions.

Windows GUI validation remains pending. Linux/headless results alone do not
establish macOS/Nikon/Retina behaviour; owner-reported macOS results are
recorded separately below.

## Focused replacement validation — 2026-10-19

- Ubuntu 24.04 / GCC 13 CMake Release build passed in the isolated validation
  container with the pinned dependency sources. The initial replacement head
  passed 16 Graph View groups (11 existing and five focused
  wiring/deletion/lifecycle groups). The corrected final head added a seventeenth
  regression group for stale menus across same-photo grade replacement.
  The picker regression now checks append for None/Source/processor/Output
  selection in both views, without changing picker or construction code.
- Separate `--selftest-graph`, `--selftest-photo`, `--selftest-export-safety` and
  `--selftest-transfers` passed; complete `--selftest` passed. Existing non-UI
  tests and their lifetime/export expectations are unchanged from main.
- Debug ASan/UBSan `--selftest-graph-view`, `--selftest-graph` and
  `--selftest-graph-runtime` passed with leak detection and halt-on-error enabled.
  No finding occurred in these focused runs; no complete sanitizer run is claimed.
  Historical unrelated sanitizer findings remain recorded separately above.
- Real Xvfb/OpenGL smoke on a synthetic PNG rewired Source into an occupied input
  and virtual Output, changed the preview, disconnected/reconnected the required
  input, and deleted an off-spine processor through the one-item node menu.
  Actual saves independently confirmed B.image -> Source, Output -> Source and
  removal of only A/A's position, preserving Source (0,0) and B (640,0).
  A single fresh process restored that grade, then Backspace deleted B; explicit
  Save Grade and File -> Quit saved Source-only topology/layout. Process exit was
  checked before reopening. GUI smoke used menu Quit; the baseline's displayed
  Ctrl+Q label does not implement that shortcut and was not changed in this PR.
- Full-diff scope audit: production changes are confined to GraphCanvas.cpp/.h,
  NodesPanel.cpp and UiFrame.cpp. The PR description maps each to approved UI
  behaviour. No non-UI production supporting change is required. NodeGraph,
  ProcessorPicker, GraphCommands, AppState, renderer/evaluator, persistence and
  processor backends are identical to current main. No withdrawn insertion tests,
  picker state, insertion preflight, graph-to-canvas callbacks or new systems remain.

## PR #82 completion and validation — 2026-10-20

- The final reviewed PR #82 head was
  `551bec78b0288fde4795146a7d8eaaf2b8ca96cf`. The PR was squash-merged
  into `main` as `7d3fdbe9fb2ff93560f19be5ac4a415653eaee1c`.
  PR #81 (`13054eaf05c249d050a6cd48bb175fff2b9ff5a1`) was closed
  without merging; its broader insertion/processor-construction changes were
  explicitly outside PR #82.
- In the review transcript supplied by the owner, Claude reported an
  independent final-delta **SAFE TO MERGE** verdict, local Ubuntu Release build
  and full `--selftest` pass (173/173 groups), including 17 Graph View groups.
  Claude also reported running mutation checks in local scratch builds:
  the original wrong-node Delete/Disconnect regression and removal of either
  new selection guard failed the tests as intended. These local results come
  from the supplied review transcript, not a complete GitHub CI log transcript.
  GitHub Linux CI passed on the final head; the 17 Graph View groups are
  confirmed in source and CI evidence. Codex separately reported focused
  ASan/UBSan and GUI smoke checks; Claude did not independently rerun
  sanitizers on the final correction.
- The owner reported that the macOS Graph View checklist passed overall,
  with no unexpected regression, after running the app locally on a Mac.
  The owner specifically discussed connection endpoints: Source's output
  itself and the virtual document Output input cannot be disconnected;
  this is expected. Individual receiving-node inputs can be disconnected,
  while document Output can be rewired to a valid Image producer. A direct
  Source-to-Output connection previews the unchanged source, with Export
  disabled under the existing policy.
- This was an overall owner report, not item-by-item evidence or an
  independent macOS build/sanitizer certification. Particular subcases were
  **not separately confirmed** in the report: full-resolution Export from
  a processing-node Output; held-key/text-field focus interactions; Retina
  port targeting and trackpad specifics; all Native/CTL/OFX, clipboard/preset
  and Inspector regression combinations. Corrupted-sidecar protection and
  export/edit overlap were discussed but not explicitly verified under the
  respective native failure/race conditions. Automated regression coverage
  remains available; these native scenarios are not claimed as individually
  passed and can receive focused follow-up testing.
- Windows GUI validation remains pending. PR 2C is not yet implemented or
  approved in detail; see D052 and the roadmap for the product-scope boundary.
