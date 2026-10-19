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
- Canvas gestures preserve executable graph JSON, render epoch, queued demand and
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

Graph provides single selection and header movement. Reorder, enable/disable and
removal remain List controls; Add Processor is available in Nodes in both modes.
Source is movable and opens input inspection. Output is selectable but derived,
non-draggable and nonpersistent. Pan/zoom reset per photo; zoom spans 0.2–2.5.
Very large layouts may require panning after Frame All reaches minimum zoom.
Saved overlaps are preserved. Frame All scales text with zoom; assess Retina/
non-Retina readability through the manual checklist. Multi-selection, grouping,
empty nodes and production Mask processors remain outside PR 2A.
