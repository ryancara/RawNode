# Graph/List visual foundation — PR 1

This is the first visual editor delivery under D047. Connection editing remains
PR 2. No renderer, evaluator, graph commands or sidecar schema is replaced.

## Design and independently verifiable acceptance criteria

- `App::nodes` owns the graph. Canvas geometry borrows Nodes; ports come from
  `nodeInterface()`. Wires read fixed/Slot input bindings and documentOutput on
  each draw. No editable/cached UI edge list or second graph exists.
- List and Graph share `selectedNodeId` and `selectNode()`. Changing views must
  retain selection, inspector identity and filter contents; choosing a different
  processor retains the existing filter-reset behavior.
- Iterative dependency columns with stable NodeId tie-breaking place missing
  nodes; saved rectangles stay fixed. For an independently authored diamond,
  consumers must appear to the right of their dependencies, branches must not
  overlap, and disconnected nodes must remain visible. Reversing node storage
  must produce identical geometry and port declarations.
- Positions use `app.nodePositions` and `grade.positions` with stable NodeIds.
  The owner deliberately retained automatic position persistence: first layout
  materializes each missing coordinate in an editable photograph. Generated and
  dragged coordinates have identical ownership, with no extra layout state or
  distinction. Reopening restores both exactly. Selection, pan, zoom and framing
  leave these coordinates unchanged. Protected photos derive geometry only.
  No movement performs disk I/O; Save Grade, departure/reopen and shutdown use
  the established save/failure lifecycle.
- Canvas gestures must leave captured executable graph JSON, render epoch,
  preview/display demand and DocumentMutation observations unchanged.
- Coordinate transforms must invert. Zoom must preserve the graph point under
  the cursor, including at the zoom limits. Frame-all must center the graph and
  fit its bounds with padding when the minimum zoom permits it.
- Old settings default to List. Independently authored valid/invalid view values
  must round-trip/fall back without losing unrelated preferences. Old/custom dock
  trees retain existing side windows; saved floating Nodes windows stay floating.
  A real previous-version ini must produce a usable Nodes panel on upgrade,
  including when loaded Preview dock nodes initially have Size=(0,0).

## Automated checks

Linux commands (macOS binary: `build/OfxRawHost.app/Contents/MacOS/OfxRawHost`):

```sh
./build.sh
build/OfxRawHost --selftest-graph-view
build/OfxRawHost --selftest-preferences
build/OfxRawHost --selftest-photo
build/OfxRawHost --selftest
```

`GraphViewTests` checks layout/ports without ImGui, then uses real Dear ImGui
frames and input events without a graphics context for selection, header/body
dragging, Source selection clearing/movement, empty-canvas left and middle-button
pan, wheel zoom, focused F, toolbar framing, List/Graph switching and protected
layouts. Runtime observation checks
that no mutation or new render demand occurs, including preserving queued work
while export owns execution. Actual photo save/open operations
check fractional/negative positions, no movement-time writes, atomic save failure
retention, codec restoration and photo viewport reset. Headless dock tests cover
default placement, old custom splits and saved floating Nodes windows. A real
UI-frame regression checks first-launch framing after docking geometry settles.
[pre-graph-layout.ini](../tests/fixtures/pre-graph-layout.ini) was saved by the unmodified `2491f91` Release
binary under Xvfb, using the normal default 1400x900 window and shutdown path.
The upgrade regression loads it into a fresh ImGui context, establishes the
initial zero Size / nonzero SizeRef condition, then asserts meaningful Nodes and
Preview heights, unchanged side/Filmstrip geometry and stable split IDs over six
real frames. This covers disk-loaded settings, unlike an in-memory DockBuilder
fixture that already has live sizes.

Additional interaction checks use independently calculated screen coordinates at
0.5x and 2x zoom, assert inverse-scaled graph movement, and verify the cursor's
world anchor after zoom. F is tested while the canvas is focused, while text is
actively edited with the pointer over the canvas, while another panel has focus,
and across List/Graph switching. Photo tests retain automatic coordinates through
save/reopen alongside manually moved nodes and distinct layouts for two photos.
`PreferencesTests` covers the optional view field, both valid values and invalid
type/value fallback. The complete suite also retains List actions and transfer
contract coverage.

## Linux correction validation

- Release build, all nine focused Graph View groups, preferences/photo tests and
  the complete `--selftest` suite pass.
- Debug `-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer` runs of
  Graph View and preferences pass with halt-on-error and leak detection enabled.
  The original custom-docking test's same-frame double DockSpace submission was
  corrected to use separate frames, satisfying Debug ImGui assertions.
- The strict photo UBSan run stops at a pre-existing signed left shift in
  `third_party/stb/stb_image_write.h:1263`. This was independently reproduced by
  writing a 3x2 grey JPEG directly with the unchanged `2491f91` header, without
  PR #79 application code. A recovery run completes the photo tests with that
  same UBSan finding and no address/leak reports. The JPEG writer is unchanged.
- Removing only the SizeRef repair makes the real-ini regression fail. Real
  Xvfb binary upgrades produce a 22px-high Nodes panel without the repair and
  256px with it in List and Graph. Left/Right/Filmstrip settings remain identical.
- A real XTest empty-canvas left drag visibly pans the graph; normal shutdown
  preserves the entire grade and all five saved node positions.

macOS and Windows builds/GUI checks remain unperformed. Native trackpad/Magic
Mouse interaction, upgraded docking and Retina text require the checklist below.

## macOS manual checklist — not yet performed

Use a disposable photo folder and preserve existing application settings first.
Build the application with `./build.sh`; record the tested head, macOS version
and results. Do not describe these checks as passed until actually performed.

1. Start with no `layout.ini`: controls left, Preview above Nodes, parameters
   right and Filmstrip at the bottom. Resize Nodes, dock it elsewhere, undock it
   and restart; verify restoration. Close Nodes and reopen it from View > Nodes.
2. Restore an actual previous-version `layout.ini` with no Nodes entry, including
   the regression fixture above. Nodes must have a usable height, not a collapsed
   title bar, without deleting/resetting settings. Existing side panels and
   Filmstrip retain their layout; Nodes appears below a docked Preview. If Preview
   is floating, Nodes starts as a regular dockable window. Restart with Nodes
   floating and confirm it remains floating.
3. Open a RAW/NEF and add native, CTL and available OFX processors. In List,
   reorder by arrows and drag/drop, toggle enable and delete a disposable node.
   Confirm preview behavior matches the previous List controls.
4. Select a processor, enter a parameter filter and switch List/Graph repeatedly.
   Selection, filter and the right inspector remain on the same NodeId. Choose a
   different processor in Graph and edit its parameters through the inspector.
5. In Graph verify Source, all processors, typed ports, connections and the visual
   Output. Use a saved branching grade with a shared producer, off-spine node,
   disconnected required input and unavailable/disabled processor. Confirm every
   node appears, wires follow bindings, and indicators remain legible across themes.
6. Drag Source and processor headers. Body clicks select without moving. Check
   that clicking Source clears the processor inspector and remains cleared when
   switching views. At 0.5x and 2x zoom, dragging tracks graph-space coordinates.
   Check clipping/hit testing at all canvas edges and while another window overlaps it.
   Preview zoom/pan and the processing result remain unchanged.
7. Left-drag empty canvas with a trackpad/Magic Mouse, and middle-drag to pan.
   Body-origin drags must neither move nodes nor pan, even when crossing into
   empty space. Pan must not alter saved coordinates. Wheel zoom around the
   cursor to both limits. Click canvas and press F, then use Frame all.
   F while editing a filter or using another
   panel must not frame the graph. Check ordinary Copy Node/Copy Grade/Paste
   menu actions and shortcuts in both views.
8. First open Graph without dragging, then Save Grade/switch/reopen/restart:
   automatically generated positions must restore exactly. Repeat after dragging;
   moved stable-ID coordinates restore from the photo's `grade.positions`.
   New nodes without positions receive
   clear placement. The second photo uses its own coordinates; view preference
   remains global and viewport pan/zoom are not written to the sidecar.
9. Open a photo with a protected/invalid sidecar: canvas navigation remains usable
   and dragging cannot change layout or overwrite the sidecar. Exercise an
   unwritable destination: failed grade departure retains photo and positions;
   Retry/Cancel/explicit Discard retain their established behavior.

## Limits and remaining PR 2 scope

Graph View provides single selection and header movement. Reordering,
enable/disable and deletion remain available in List; Add Processor remains in
the left controls. Source is movable but has no processor inspector. Output is a
derived, non-draggable endpoint and has no persisted NodeId. Minimum/maximum zoom
are 0.2/2.5; very large or widely separated layouts may require panning after
Frame all reaches the minimum. Auto placement is deliberately simple; existing
saved overlaps are preserved. Pan/zoom reset on photo change and stay transient.

Frame All necessarily reduces the existing text scale with zoom (roughly 7px
for some default-panel layouts). Linux smoke checks can establish bounds and
clipping, but cannot establish Retina/macOS legibility. Keep the current zoom/text
design for this correction; manually assess default panel height, resizing and
text at Frame All on Retina and non-Retina macOS displays before choosing a
cosmetic adjustment. No new text rendering system is introduced.

Connection creation/reconnection/deletion, graph context menus and topology
manipulation remain PR 2 through existing GraphCommands. Multi-selection,
grouping and production Mask processors remain outside this milestone.

PR 2 should place a newly inserted serial node between its upstream/downstream
neighbours, make room predictably within manually arranged graphs where practical,
and offset pasted/duplicated nodes to avoid overlap. Preserve existing saved
positions on reopen. Optional user-requested Auto Arrange remains later work;
none of these placement behaviours is implemented in PR 1.
