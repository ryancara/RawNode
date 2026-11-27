# RawNode — Configurable commands and input bindings

PR #135 introduced configurable shortcuts. PR 2 extends the same bindings with ordered commands. Both stages use the existing application actions, Graph/List selection, processor parameters and grade history.

## Keyboard bindings and Settings

A binding pairs one keyboard chord with a nonempty ordered list of typed commands. Settings lists all configured inputs with their actions in execution order and offers Edit and Remove on each row. **Add binding** and **Edit** open the same editor. Record a key or chord, choose a command, and use Add command, Up, Down and Remove to shape a sequence. Save applies the entire assignment. Cancel discards the draft. The editor reports reserved keys, conflicting chords and invalid commands.

**Restore all defaults** reinstates the platform defaults from PR #135. Restoring individual actions, including actions with no assigned key, would require an additional action chooser. Menu labels list only bindings that invoke that single menu action. PR 2 adds no default shortcuts.

Chord recognition uses ImGui's existing key ownership, exact modifiers and frame-boundary checks around active controls, capture, menus and popups. The primary modifier is Command on macOS and Control elsewhere. The operating system or window manager determines which chords reach RawNode.

## Execution and photograph actions

The application evaluates each step against the current document after the preceding operation completes. A sequence stops on the first unavailable, failed or cancelled step and identifies it in a dismissible toolbar message that survives preview updates until the next shortcut. Original standalone shortcuts retain their existing action feedback. New standalone commands report failures through the same toolbar message as sequences. Completed operations keep their ordinary Undo boundaries. Opening an image, choosing a folder and opening Export must end a sequence. For deferred actions, the terminal step requests the dialog or folder change. There is no pending sequence to resume.

Toggle Node uses the selected node's existing enable/bypass operation. Pick, Reject and Unmarked update the open photograph through the existing mark writer. Next and Previous search the Filmstrip in the chosen direction from the open photograph's position, including when a filter just hid it. Without a current entry they start at the first or last visible photograph. At the boundary they stop. Plain thumbnail activation completes outgoing edits, handles departure-save choices and replaces the selection and anchor only after a successful open. For example, **Mark as Reject → Next Photograph** keeps the reject mark if departure is cancelled. **Copy Full Grade → Next Photograph → Paste** gives the destination photograph its ordinary paste history entry.

## Processor and parameter resolution

Explicit selection commands change the shared Graph/List selection. Processing positions are one-based in Nodes List order, including processing nodes in the Other graph nodes section. Source, Output, section headings and non-processor rows occupy no position. Next and Previous stop at the ends. From any non-processing selection they select the first or last processor respectively.

A scalar command stores the processor's backend and identifier, including the script identity for CTL, and the parameter ID. A Set or Adjust command also stores its numerical operand. The current-grade picker chooses the processor and editable scalar together and saves their identifiers. The backend, processor ID and parameter ID are available for manual entry when a target is absent from the grade.

For processor selection and scalar commands, use the currently selected processor if its identity matches. Otherwise use the first match in Nodes List order. An unavailable processor or parameter fails the step. The resolver checks the parameter only on the resolved instance. Scalar commands leave the shared selection as it was. For example, one binding can adjust Native Exposure while an OFX processor remains selected, then reset a scalar on another processor.

## Scalar edits

Each operation reads fresh parameter metadata. The target must be a visible, enabled, editable Double or Integer. Set accepts an absolute value and Adjust a signed amount in parameter units. Both require finite values. Integer inputs must be whole and the result must be representable. Increase/Decrease use a declared positive step (a whole step for an integer), otherwise one integer unit or 1% of a meaningful finite floating-point display span. An unbounded CTL float without a declared step needs an explicit amount. OFX retains the Inspector’s existing display fallback when the plug-in declares no range.

Hard bounds limit the result. Display bounds supply an increment and never pull a value back into the slider range. Inspector Plus/Minus use the same calculation. For example, native Exposure at +7 EV steps to +7.1 EV on Plus, using its −5 to +5 display span and −10 to +10 hard limits.

A changed value or reset goes through the existing NodeGraph setter or reset, backend notification and preview request, followed by one completed grade edit. An already satisfied adjustment succeeds without another write. Protection and export restrictions apply at use time. Selection, marking, copy and navigation add no artificial grade-history entries.

## Preferences compatibility

The optional version-1 `keyboardBindings` array continues to store stable key and modifier names. Operand-free actions retain their command strings. A step with operands is an object with an `id`. Select by position adds `position`. Select Processor adds `backend` and `processor`. Scalar steps add `backend`, `processor` and `parameter`, and Set and Adjust also add `value`. For example:

```json
{"id":"adjust-value","backend":"native","processor":"rawnode.native.exposure","parameter":"exposure","value":0.1}
```

Missing bindings use the existing platform defaults and an empty array deliberately unbinds all. Invalid entries are skipped as a whole with a warning, the first valid conflicting assignment wins, and an entirely invalid nonempty array falls back to defaults. The temporary contextual assignments from draft PR #136 are covered by these validation rules. Saving uses the existing atomic preferences writer.

## PR 3 and future scope

PR 3 will add a standalone **Adjust While Held** assignment for one directly addressed scalar parameter. Hold the configured key, for example E for Exposure or C for Contrast, and use Up or Down or scroll vertically over the idle Preview to adjust it. The wheel is implied by the assignment. Steps are automatic by default and may be specified explicitly. Releasing the key ends the session. Pressing and releasing without adjusting has no effect on selection, the grade or Undo. The first adjustment resolves the processor and parameter and retains the resolved node identity for that gesture. Graph/List selection stays as it was.

Reuse ImGui input ownership, parameter setters, gesture completion and grade history. One completed hold with a grade change produces one Undo entry, including a hold that combines arrows and wheel. Complete the gesture on release, a relevant context or focus change, or before a competing edit, save, departure, export or teardown. Ordinary wheel input continues to zoom when no held setting owns it. An eligible held assignment owns vertical wheel input even if its processor or parameter is unavailable. Preserve pinch, Graph interaction, panel scrolling, text entry and existing canvas gestures.

The assignment needs no separate wheel binding, persistent parameter target, general-purpose ordinary-key modifiers, tap-versus-hold timer or global raw-input polling. PR 3 will determine and validate the input routing, repeat and conflict rules, fractional wheel input and native trackpad behaviour. Native desktop validation should exercise wheel ownership with and without an available addressed processor, mixtures of arrows and wheel, and the completion boundaries above. Discrete keyboard sequences in PR 2 run once per press.

MIDI, OSC, external hardware, scripting, specialised parameter types, custom node naming and multi-photo grade application remain outside these stages.

## Investigation basis

Independent Codex and Claude read-only investigations of the pre-#134 main architecture identified existing application actions, `ui::projectNodeList()`, `ProcessorParameter`, NodeGraph, GradeHistory and ImGui as the shared foundations. Both recommended structured configuration and no scripting engine. Codex proposed ordered typed commands; Claude proposed a narrower fixed-shape binding. The owner selected ordered chaining, current-grade picker plus manual identity entry, selected-match/first-match resolution and release-based history grouping. The detailed implementation must be checked against the current merged baseline rather than assuming old line references remain accurate.
