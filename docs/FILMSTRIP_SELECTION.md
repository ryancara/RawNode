# Filmstrip multi-photo selection

Implemented for review on main including merged PR #114. Selection is transient
and independent of the photograph open in the editor. Move to Trash and batch
export remain separate PRs.

## Interactions and visible changes

- Plain click selects only that photograph after `finishUiEdits` / `openPath`
  succeeds. Retry / Discard / Cancel retains the existing save policy. Cancelled
  departure or failed opening preserves the previous selection, anchor and photo.
- Command-click on macOS, Control-click on Windows/Linux, toggles selection and
  sets the anchor to the clicked path, including when deselecting it.
- Shift-click replaces selection with the inclusive range from the anchor in
  visible filmstrip order. Both mark and RAW/Compressed filters apply. A hidden
  or missing anchor starts at the clicked photograph. Command+Shift/Control+Shift
  adds that range instead; a missing anchor adds the clicked photograph.
- Command+A / Control+A replaces selection with all visible photographs and
  sets the anchor to the first visible path, or clears it when none are visible.
  It runs only with filmstrip window focus and no active widget or text input.
  Graph/List and text shortcuts retain their existing ownership. Delete and
  Backspace behaviour is unchanged; this PR adds no photograph deletion.
- Selected thumbnail frames use the existing theme's HeaderActive colour. An
  independent Text-colour outline identifies the open photograph, which can
  be selected or unselected. The toolbar, thumbnail dimensions, filters and
  P/U/R/? indicators retain their existing layout.

Filters hide selected photographs without removing them from selection. Marking
does not change selection, including when the new mark hides the photograph.
Explicit replacing actions (plain click, ordinary Shift-click and Select All)
replace hidden selections too. Protected/Unknown sidecars do not prevent selection.

Changing to a different folder clears selection after departure succeeds. Folder
auto-opening keeps its existing behaviour and does not establish a batch selection.
Refreshing or reopening the same canonical folder retains paths still present;
removed paths and an absent anchor are pruned. A directly opened photograph has
no additional selection workflow. File-dialog/drop opening retains its existing
document behaviour and does not implicitly replace batch selection.

## Design and regression evidence

`App::filmstripSelection` is a set of existing entry paths, and
`App::filmstripAnchor` is one path. `filmstripIndex` still identifies the open
document. Small selection helpers in Filmstrip use only those identities and
`filmstripEntryVisible`, without filesystem reads, sidecar writes, edit completion,
render requests or thumbnail invalidation. Refresh prunes against its existing
scan; folder opening clears only after successful departure. No selection service,
focus registry, persistent state or export/deletion execution is added.

Acceptance expectations come from the requested Finder-style interactions:
explicit path sets for ordinary, shrinking, additive and filtered ranges;
unchanged document/grade/history/gesture/runtime demand and sidecar bytes for
modifier selection; success-only plain selection; and independent selected/open
appearance. The existing `PhotoDocumentTests` ImGui fixture exercises production
mouse/keyboard paths with both macOS and other-platform modifier configurations.
It also checks protected selection, real failed decode/save with Cancel/Retry/
Discard, folder refresh/removal/switching, marking, Graph/List focus and continued
node deletion, native text Select All, and all eight existing themes. Existing
toolbar geometry tests continue across widths, heights and framebuffer scales.

## Required native macOS checks

Pending; headless tests and macOS CI do not establish native GUI results.

1. In docked, floating, narrow and Retina filmstrips, verify selected frames and
   open-photo outlines independently, including both states on the same image,
   across themes. Confirm the one-row toolbar and visible P/U/R/? footers.
2. Exercise plain, Command, Shift, Command+Shift and Command+A with both filters,
   hidden anchors, reverse/shrinking ranges and selected photographs hidden by
   marking. Modifier selection must leave the editor/preview/active grade alone.
3. Verify Command+A affects only Filmstrip focus; text fields, processor search,
   Graph and List retain their shortcuts. Verify Control-click keeps native
   macOS behaviour. Graph/List Delete/Backspace still deletes only nodes;
   Filmstrip Delete/Backspace has no photo action.
4. With an unwritable outgoing sidecar, exercise the native Retry / Discard /
   Cancel dialog during plain clicks and folder switching. Cancel and failed
   image decode must retain the document and prior selection. Select protected
   photographs without opening or changing their sidecars.
5. Refresh after removing a selected image, switch folders, and restart. Check
   stale selections are pruned, folder switches clear them and nothing persists.
