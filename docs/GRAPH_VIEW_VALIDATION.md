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
  Opening Graph materializes missing coordinates for editable photos. Protected
  photos derive geometry only. No movement performs disk I/O; Save Grade,
  departure/reopen and shutdown use the established save/failure lifecycle.
- Canvas gestures must leave captured executable graph JSON, render epoch,
  preview/display demand and DocumentMutation observations unchanged.
- Coordinate transforms must invert. Zoom must preserve the graph point under
  the cursor, including at the zoom limits. Frame-all must center the graph and
  fit its bounds with padding when the minimum zoom permits it.
- Old settings default to List. Independently authored valid/invalid view values
  must round-trip/fall back without losing unrelated preferences. Old/custom dock
  trees retain existing side windows; saved floating Nodes windows stay floating.

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
dragging, Source movement, middle-button pan, wheel zoom, focused F, toolbar
framing, List/Graph switching and protected layouts. Runtime observation checks
that no mutation or new render demand occurs, including preserving queued work
while export owns execution. Actual photo save/open operations
check fractional/negative positions, no movement-time writes, atomic save failure
retention, codec restoration and photo viewport reset. Headless dock tests cover
default placement, old custom splits and saved floating Nodes windows. A real
UI-frame regression checks first-launch framing after docking geometry settles.
`PreferencesTests` covers the optional view field, both valid values and invalid
type/value fallback. The complete suite also retains List actions and transfer
contract coverage.

## macOS manual checklist — not yet performed

Use a disposable photo folder and preserve existing application settings first.
Build the application with `./build.sh`; record the tested head, macOS version
and results. Do not describe these checks as passed until actually performed.

1. Start with no `layout.ini`: controls left, Preview above Nodes, parameters
   right and Filmstrip at the bottom. Resize Nodes, dock it elsewhere, undock it
   and restart; verify restoration. Close Nodes and reopen it from View > Nodes.
2. Restore an old/custom layout with no Nodes entry. Existing side panels and
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
   clipping/hit testing at all canvas edges and while another window overlaps it.
   Preview zoom/pan and the processing result remain unchanged.
7. Middle-drag to pan; wheel zoom around the cursor to both limits. Click canvas
   and press F, then use Frame all. F while editing a filter or using another
   panel must not frame the graph. Check ordinary Copy Node/Copy Grade/Paste
   menu actions and shortcuts in both views.
8. Save Grade, switch photos, reopen and restart: moved stable-ID coordinates
   restore from the photo's `grade.positions`. New nodes without positions receive
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

Connection creation/reconnection/deletion, graph context menus and topology
manipulation remain PR 2 through existing GraphCommands. Multi-selection,
grouping and production Mask processors remain outside this milestone.
