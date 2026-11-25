# RawNode — Configurable commands and input bindings

PR #135 introduced configurable shortcuts. PR 2 extends the same bindings with ordered commands. Both stages use the existing application actions, Graph/List selection, processor parameters and grade history.

## Keyboard bindings and Settings

A binding pairs one keyboard chord with a nonempty ordered list of typed commands. Settings groups assignments by their first action. **Add shortcut** assigns a single action. **New sequence** and **Edit commands** open an ordered editor with command choices, operand fields and Up, Down and Remove controls. A picker lists processors and editable scalar parameters from the open grade. The same backend, processor ID and parameter ID can be entered manually, including for a grade that is currently unavailable. The parameter picker supplies only a parameter ID. It applies to whichever node is selected when that step runs.

Change and Clear operate on the whole assignment. Restoring an action's defaults replaces only its one-action shortcuts. If a default chord belongs to another binding, restoration reports the conflict and keeps all assignments. Restore all defaults returns to the platform defaults from PR #135. Menu labels list only bindings that invoke that single menu action. PR 2 adds no default shortcuts.

Chord recognition uses ImGui's existing key ownership, exact modifiers and frame-boundary checks around active controls, capture, menus and popups. The primary modifier is Command on macOS and Control elsewhere. The operating system or window manager determines which chords reach RawNode.

## Execution and photograph actions

The application evaluates each step against the current document after the preceding operation completes. It stops on the first unavailable, failed or cancelled operation and identifies the step in a dismissible toolbar message that survives preview updates until the next shortcut. Completed operations keep their ordinary Undo boundaries. Opening an image, choosing a folder and opening Export must end a sequence. For deferred actions, the terminal step requests the dialog or folder change. There is no pending sequence to resume.

Toggle Node uses the selected node's existing enable/bypass operation. Pick, Reject and Unmarked update the open photograph through the existing mark writer. Next and Previous search the Filmstrip in the chosen direction from the open photograph's position, including when a filter just hid it. Without a current entry they start at the first or last visible photograph. At the boundary they stop. Plain thumbnail activation completes outgoing edits, handles departure-save choices and replaces the selection and anchor only after a successful open. For example, **Mark as Reject → Next Photograph** keeps the reject mark if departure is cancelled. **Copy Full Grade → Next Photograph → Paste** gives the destination photograph its ordinary paste history entry.

## Node and parameter selection

Processing positions are one-based in Nodes List order, including processing nodes in the Other graph nodes section. Source, Output, section headings and non-processor rows occupy no position. Next and Previous stop at the ends. From any non-processing selection they select the first or last processor respectively. A processor identity is its saved backend and identifier, including CTL's script path. The selected matching instance takes precedence, followed by the first match in List order. A missing parameter on that instance fails the step.

Select changes the shared Graph/List selection. Target Parameter associates one transient ID with that selection and the Inspector displays its label. Selection changes, deletion of the selected node, successful photograph changes, grade replacement and Undo/Redo reconstruction clear the target. Selecting the same node keeps it. Neither selection nor targeting adds grade history. To address a parameter directly, chain Select Processor, Target Parameter and an adjustment.

## Scalar edits

Each operation reads fresh parameter metadata. The target must be a visible, enabled, editable Double or Integer. Set accepts an absolute value and Adjust a signed amount in parameter units. Both require finite values. Integer inputs must be whole and the result must be representable. Increase/Decrease use a declared positive step (a whole step for an integer), otherwise one integer unit or 1% of a meaningful finite floating-point display span. An unbounded float without a declared step needs an explicit amount.

Hard bounds limit the result. Display bounds supply an increment and never pull a value back into the slider range. Inspector Plus/Minus use the same calculation. For example, native Exposure at +7 EV steps to +7.1 EV on Plus, using its −5 to +5 display span and −10 to +10 hard limits.

A changed value or reset goes through the existing NodeGraph setter or reset, backend notification and preview request, followed by one completed grade edit. An already satisfied adjustment succeeds without another write. Protection and export restrictions apply at use time. Selection, targeting, marking, copy and navigation add no artificial grade-history entries.

## Preferences compatibility

The optional version-1 `keyboardBindings` array continues to store stable key and modifier names. Operand-free actions retain their command strings. A step with operands is an object with an `id` and either `position`, `backend` and `processor`, `parameter`, or `value`. Processor and parameter references store only their identifiers. Missing bindings use the existing platform defaults and an empty array deliberately unbinds all. Invalid entries are skipped as a whole with a warning, the first valid conflicting assignment wins, and an entirely invalid nonempty array falls back to defaults. Saving uses the existing atomic preferences writer.

## PR 3 and future scope

PR 3 is planned to add held adjustment keys and modified vertical wheel input on the idle Preview canvas. It will retain normal zoom, scrolling and input ownership and group each hold or modifier-wheel session into one completed edit. An eligible wheel gesture consumes the wheel event even when its target is missing. Only a single adjustment can repeat. Complete the edit on release, a context, target or focus change, or before a competing edit, save, departure, export or teardown. Use existing ImGui ownership, gesture completion and pending grade history. The design uses neither global raw-wheel polling nor an idle timer. Native wheel/trackpad behaviour and fractional deltas still require validation. Keyboard sequences in PR 2 run once per press.

MIDI, OSC, external hardware, scripting, specialised parameter types, custom node naming and multi-photo grade application remain outside these stages.

## Investigation basis

Independent Codex and Claude read-only investigations of the pre-#134 main architecture identified existing application actions, `ui::projectNodeList()`, `ProcessorParameter`, NodeGraph, GradeHistory and ImGui as the shared foundations. Both recommended structured configuration and no scripting engine. Codex proposed ordered typed commands; Claude proposed a narrower fixed-shape binding. The owner selected ordered chaining, current-grade picker plus manual identity entry, selected-match/first-match resolution and release-based history grouping. The detailed implementation must be checked against the current merged baseline rather than assuming old line references remain accurate.
