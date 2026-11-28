# RawNode — Configurable commands and input bindings

PR #135 introduced configurable shortcuts. PR #136 added ordered commands and direct scalar edits. Adjust While Held uses the same binding list, parameter addresses and grade history.

## Keyboard bindings and Settings

A binding pairs one keyboard chord with a nonempty list of typed commands. Settings lists assignments by input, marks held keys, and displays each sequence in execution order. **Add binding** and **Edit** open the same editor. Record a key or chord, choose from the command categories, and use Add command, Up, Down and Remove to shape a discrete sequence. Save applies the entire assignment. Cancel discards the draft. The editor reports reserved keys, conflicting chords and invalid commands. The draft uses a zero key value until a key has been recorded.

**Restore all defaults** reinstates the platform defaults from PR #135. Menu labels list only bindings that invoke that single menu action. Held adjustments are opt-in.

Chord recognition uses ImGui's existing key ownership, exact modifiers and frame-boundary checks around active controls, capture, menus and popups. The primary modifier is Command on macOS and Control elsewhere. The operating system or window manager determines which chords reach RawNode.

## Execution and photograph actions

The application evaluates each step against the current document after the preceding operation completes. A sequence stops on the first unavailable, failed or cancelled step and identifies it in a dismissible toolbar message that survives preview updates until the next shortcut. Original standalone shortcuts retain their existing action feedback. New standalone commands report failures through the same toolbar message as sequences. Completed operations keep their ordinary Undo boundaries. Opening an image, choosing a folder and opening Export must end a sequence. For deferred actions, the terminal step requests the dialog or folder change. There is no pending sequence to resume.

Toggle Node uses the selected node's existing enable/bypass operation. Pick, Reject and Unmarked update the open photograph through the existing mark writer. Next and Previous search the Filmstrip in the chosen direction from the open photograph's position, including when a filter just hid it. Without a current entry they start at the first or last visible photograph. At the boundary they stop. Plain thumbnail activation completes outgoing edits, handles departure-save choices and replaces the selection and anchor only after a successful open. For example, **Mark as Reject → Next Photograph** keeps the reject mark if departure is cancelled. **Copy Full Grade → Next Photograph → Paste** gives the destination photograph its ordinary paste history entry.

## Processor and parameter resolution

Explicit selection commands change the shared Graph/List selection. Processing positions are one-based in Nodes List order, including processing nodes in the Other graph nodes section. Source, Output, section headings and non-processor rows occupy no position. Next and Previous stop at the ends. From any non-processing selection they select the first or last processor respectively.

A scalar command stores the processor's backend and identifier, including the script identity for CTL, and the parameter ID. Set and Adjust also store their numerical operand. The current-grade picker chooses the processor and editable scalar together. **Manual identifiers** exposes the backend, processor ID and parameter ID when a target is absent from the grade.

For processor selection and scalar commands, use the currently selected processor if its identity matches. Otherwise use the first match in Nodes List order. An unavailable processor or parameter fails the step. The resolver checks the parameter only on the resolved instance. Scalar commands leave the shared selection as it was. For example, one binding can adjust Native Exposure while an OFX processor remains selected, then reset a scalar on another processor.

## Scalar edits

Each operation reads fresh parameter metadata. The target must be a visible, enabled, editable Double or Integer. Set accepts an absolute value and Adjust a signed amount in parameter units. Both require finite values. Integer inputs must be whole and the result must be representable. Increase/Decrease use a declared positive step (a whole step for an integer), otherwise one integer unit or 1% of a meaningful finite floating-point display span. An unbounded CTL float without a declared step needs an explicit amount. OFX retains the Inspector’s existing display fallback when the plug-in declares no range.

Hard bounds limit the result. Display bounds supply an increment and never pull a value back into the slider range. Inspector Plus/Minus use the same calculation. For example, native Exposure at +7 EV steps to +7.1 EV on Plus, using its −5 to +5 display span and −10 to +10 hard limits.

A changed value or reset goes through the existing NodeGraph setter or reset, backend notification and preview request, followed by one completed grade edit. An already satisfied adjustment succeeds without another write. Protection and export restrictions apply at use time. Selection, marking, copy and navigation add no artificial grade-history entries.

## Adjust While Held

Choose **Adjust While Held** as the only command in a binding. Record a single key, choose a scalar, and leave **Automatic step** on or enter a positive step in parameter units. An Integer requires a whole step. Up and Down are reserved as adjustment controls for the hold and cannot serve as activators. A held activator conflicts with the same unmodified input. Modified chords such as the primary modifier plus E remain available.

With idle Preview focused or the pointer over its canvas, press and hold the assigned key. Up and Down step once per press and use ImGui key repeat. Vertical wheel input over the canvas uses the same step. Each wheel unit adds one step. Doubles apply fractional units directly and Integers accumulate signed wheel fractions until a whole unit is reached. Arrows use whole steps and leave that remainder intact. Release discards the remainder. GLFW already delivers precise macOS scrolling and fractional Windows wheel messages through the shared ImGui backend. There is no extra device or momentum filtering.

The first adjustment resolves the selected matching processor or the first match in Nodes List order. The hold retains that node ID, leaves Graph/List selection intact, and continues to own the controls if resolution fails or the target becomes invalid. A new press is required to resolve again. The first eligible activator stays in control until the hold ends. A second activator needs a fresh press.

Only a changed tick starts the existing parameter gesture. It writes through the scalar calculation and NodeGraph setter and leaves capture to gesture completion. Release, context interruptions and competing edits complete the gesture, as do Save Grade, saving a preset, photograph departure, export and teardown. List and Graph complete the held edit before recording a deletion, so each operation has its own Undo step. History gives mixed arrow and wheel input one Undo entry when the final stored grade differs from the current snapshot. A tap or an exactly unchanged grade adds no entry. Opposing Double steps can leave a floating-point residual that counts as a change. Capture failure retains the live edits and the existing pending-history state.

Preview consumes vertical wheel input for a hold before applying ordinary wheel zoom. Pinch continues through its existing path. Outside the idle context, graph and panel interactions, text and numeric controls, popups, and Space-to-pan continue to own their inputs. An interrupted hold needs a fresh key press.

Preview locks adjustment arrows for the current frame before ordinary shortcut dispatch. ImGui keyboard navigation is disabled, and earlier panels leave these arrows free in the idle context.

## Preferences compatibility

The optional version-1 `keyboardBindings` array continues to store stable key and modifier names. Operand-free actions retain their command strings. A step with operands is an object with an `id`. Select by position adds `position`. Select Processor adds `backend` and `processor`. Scalar steps add `backend`, `processor` and `parameter`, and Set and Adjust also add `value`. For example:

```json
{"id":"adjust-value","backend":"native","processor":"rawnode.native.exposure","parameter":"exposure","value":0.1}
```

An Adjust While Held object uses the ID `adjust-while-held` and the same scalar address. The optional `step` field must be finite and positive. Omitting it selects the automatic step. The binding stores an empty `modifiers` array. Parameter eligibility and Integer step compatibility are checked against the resolved processor at use time.

Missing bindings use the existing platform defaults and an empty array deliberately unbinds all. Invalid entries are skipped as a whole with a warning, the first valid conflicting assignment wins, and an entirely invalid nonempty array falls back to defaults. Saving uses the existing atomic preferences writer.

## Scope

MIDI, OSC, external hardware, scripting, specialised parameter types, custom node naming and multi-photo grade application remain outside these stages.

## Native desktop validation

Repeat these checks on macOS and Windows. On macOS also test native pinch. On Windows use a precision touchpad and a wheel mouse where available.

1. Configure E for Native Exposure with the automatic step. Configure C for a second scalar with an explicit step. Restart and verify both entries, then edit or remove one and verify the change persists.
2. Over Preview, hold E, tap Up and Down, then hold Up long enough for repeat. Add wheel input before releasing E. Confirm the operation leaves Graph/List selection intact and Undo and Redo each traverse the entire hold once. Tap E by itself and verify that Undo availability is unchanged.
3. Use slow vertical touchpad scrolling with a Double and an Integer. Confirm smooth Double adjustment and whole Integer increments. Continue scrolling over Preview after release and confirm that it zooms. Check whether trackpad momentum after release produces unintended zoom and record the device and OS if observed.
4. Set the binding to an unavailable target and assign a discrete action to bare Up. While holding the activator over Preview, neither Up nor the wheel should take its ordinary action. After release, verify the bare Up shortcut, wheel zoom, Graph zoom, panel scrolling, Space-to-pan, and the platform’s primary-modifier shortcut for the same letter.
5. While adjusting, switch apps, activate an Inspector control, open a popup, or start a canvas gesture. Verify one completion for a changed hold and that input requires a fresh activator press. Repeat with Save Grade, photograph navigation, Export and app close.

## Investigation basis

Independent Codex and Claude read-only investigations of the pre-#134 main architecture identified existing application actions, `ui::projectNodeList()`, `ProcessorParameter`, NodeGraph, GradeHistory and ImGui as the shared foundations. Both recommended structured configuration and no scripting engine. Codex proposed ordered typed commands; Claude proposed a narrower fixed-shape binding. The owner selected ordered chaining, current-grade picker plus manual identity entry, selected-match/first-match resolution and release-based history grouping. The detailed implementation must be checked against the current merged baseline rather than assuming old line references remain accurate.
