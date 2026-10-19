# Graph/List editor validation

PR #79 delivered the visual foundation under D047. PR 2A reorganises the
workspace and adds contextual inspection under D051; see
[WORKSPACE_INSPECTOR_VALIDATION.md](WORKSPACE_INSPECTOR_VALIDATION.md) for current
control mapping, selection behaviour, validation results and macOS checklist.
Connection editing remains PR 2B and placement/usability PR 2C. No renderer,
evaluator, graph commands or sidecar schema is replaced by these UI deliveries.

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

Graph provides single selection and header movement; the focused PR 2B adds typed
wiring and deletion. Reorder and enable/disable remain List controls. Add Processor
is available in both modes and retains PR #80's append-to-Output behaviour.
Source is movable and opens input inspection. Output is selectable but derived,
non-draggable and nonpersistent. Pan/zoom reset per photo; zoom spans 0.2–2.5.
Very large layouts may require panning after Frame All reaches minimum zoom.
Saved overlaps are preserved. Frame All scales text with zoom; assess Retina/
non-Retina readability through the manual checklist. Multi-selection, grouping,
empty nodes and production Mask processors remain outside PR 2A.

## Focused PR 2B scope and independent acceptance criteria

The owner withdrew PR #81's insertion changes. The replacement starts at clean
main; #81 stays open and intact for independent comparison. D051 retains its
historical record with a superseding scope clarification. No PR 2C work starts.

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
result. Source Output yields unchanged source pixels; +1 EV processor Output
must yield twice the source values for preview/export.

Use existing real-ImGui frames and graph-command tests. Check exact input/Output
bindings, selection/layout, mutation counts and render demand for accepted,
rejected/no-op/cancelled actions; save/reopen actual sidecars. Retain PR #80
Inspector/workspace/picker regressions. Add Processor must append regardless of
Source/processor/Output selection. Native/CTL/OFX creation, clipboard Paste and
preset Append remain on their existing paths. Do not retain tests of withdrawn
insertion functionality. Audit every production file against the five approved
behaviours before opening the replacement PR; include that table in its description.

## Focused PR 2B macOS checklist (pending)

1. Open Nikon NEF; adjust real processors and check preview updates after wiring.
2. Connect/reconnect in both directions; exercise fan-out, invalid types/cycles,
   empty-drop/Escape cancellation and Disconnect/reconnect of required inputs.
3. Rewire Output to Source and processors; confirm Image-only typing and existing
   Output colour/export settings, preview and export evaluation.
4. Delete middle/final/off-spine processors with Delete/Backspace and the menu;
   protect Source/Output and verify Inspector cleanup, text focus and held keys.
5. Verify trackpad pan/zoom, Retina port targeting, header movement and Frame All.
6. Save/switch/reopen and verify exact topology and remaining node coordinates.
7. Verify export/protected-photo safety and cancellation on photo/view changes.
8. Confirm Add Processor still appends in both views for every selection; check
   Native/CTL/OFX, clipboard/preset and PR #80 Inspector/workspace regressions.

Windows GUI validation remains pending. Linux/headless results cannot establish
macOS/Nikon/Retina behaviour.
