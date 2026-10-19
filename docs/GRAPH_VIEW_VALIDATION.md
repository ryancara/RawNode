# Graph/List editor validation

PR #79 delivered the visual foundation under D047. PR 2A reorganises the
workspace and adds contextual inspection under D051; see
[WORKSPACE_INSPECTOR_VALIDATION.md](WORKSPACE_INSPECTOR_VALIDATION.md) for current
control mapping, selection behaviour, validation results and macOS checklist.
Interactive editing is PR 2B and placement/usability PR 2C. No renderer,
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

Graph provides single selection and header movement. Reorder, enable/disable and
removal remain List controls; Add Processor is available in Nodes in both modes.
Source is movable and opens input inspection. Output is selectable but derived,
non-draggable and nonpersistent. Pan/zoom reset per photo; zoom spans 0.2–2.5.
Very large layouts may require panning after Frame All reaches minimum zoom.
Saved overlaps are preserved. Frame All scales text with zoom; assess Retina/
non-Retina readability through the manual checklist. Multi-selection, grouping,
empty nodes and production Mask processors remain outside PR 2A.

## PR 2B interaction and acceptance contract

Port interaction wins over node/header movement. Nearest-port hit testing uses
screen pixels with a radius capped below half the visible row spacing. Image and
Mask retain their colours; input/output shape, filled/empty inputs, target rings
and a temporary wire expose direction, connection and drop eligibility.

Dragging either end retains only stable node/Slot/port identities. No topology
changes before release. Output-to-input creates or atomically reconnects;
input-to-output changes that input's producer. Empty drops and Escape cancel.
Input context menus disconnect through GraphCommands, including required inputs.
Virtual Output uses setDocumentOutput, accepts only Image, and cannot disconnect.
Invalid drops report the command diagnostic and preserve the previous graph.

Delete/Backspace require the canvas item to own keyboard focus, no text editing,
no modifiers, and a fresh key press. Source/Output are excluded. Node menus expose
Delete Node and shared-picker Insert Before/After; Source only supports After.
Deletion uses destroyNode's validated unary splice or generic deletion, clears
removed selection/Inspector/layout state, and refuses ambiguous Output replacement.

Add Processor snapshots selection when opening the shared picker. None/Output
appends; Source inserts before the first spine processor; a spine processor
inserts before its next spine consumer, or appends if final. Before targets that
processor's connected Image input. Revalidate identity and complete spine when
choosing. Off-spine, missing and disconnected targets fail before construction.
Native/CTL/OFX retain one construction/identity/parameter path. No popup retains
Node pointers. Preset Append and clipboard Paste keep their existing policy.

Independent expected topology: for Source -> A -> B -> Output plus A -> Branch,
After(A) yields Source -> A -> New -> B -> Output while Branch still consumes A.
Before(A) yields Source -> New -> A -> B; append yields B -> New -> Output.
Replacing B's input changes only B's binding; repeated identical replacement is
a no-op. Disconnecting B keeps A/Branch and a valid but unavailable Output graph.
Deleting A splices applicable Image consumers to Source; deleting B moves Output
to A. Assert exact bindings/Output, not storage order or a UI-derived edge list.

All rejected/no-op edits preserve graph JSON, positions, selection, mutation
counts and render demand. Accepted edits use one existing DocumentMutation and
preview request; persistence stores consumer bindings once in the existing codec.
Export ownership rejects editing without waiting for processor destruction.
Protected photographs allow navigation/selection but no grade/layout edits.
Cancel gestures on photo/graph replacement, deleted endpoints, Graph-to-List,
Escape, lost interaction or invalidated endpoints. Persist none of this state.

Coverage uses existing real ImGui frames and graph-command/evaluator/persistence
suites, including minimum/maximum zoom, port priority, invalid types/directions,
cycles/depth, no-op/cancel, focused deletion, branch preservation, stale picker
identity, export/protection, exact round-trip topology and runtime observations.
PR 2C neighbour placement, making room, paste overlap avoidance, multi-selection
and automatic arrangement are deferred. No new graph/evaluator/scheduler/schema.

## PR 2B macOS manual checklist (pending)

1. Drag Source to a processor input; connect/reconnect processors in both directions.
2. Disconnect/reconnect a required input and confirm preview failure/recovery.
3. Rewire virtual Output to Source and a processor; verify preview and export colours.
4. Delete middle/final processors with Delete, Backspace and the node menu.
5. Add after Source/middle/final selection; use explicit Before/After menus.
6. Exercise a branched graph and verify unrelated input bindings remain intact.
7. Adjust real processors on a Nikon NEF and verify previews after wiring/insertion.
8. Verify trackpad pan/zoom, header movement, Frame All and ports at zoom limits.
9. Save, switch photographs, reopen and verify topology and saved coordinates.
10. Verify Inspector selection, text entry, unrelated-panel focus and held Delete.
11. Verify export ownership and protected photographs prevent all graph mutations.
12. Check Retina/non-Retina port feedback and readable errors at practical panel sizes.

No macOS or Windows GUI result is implied by Linux/headless checks.
