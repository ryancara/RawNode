# RawNode — Configurable commands and input bindings

Status: agreed feature direction; implementation details subject to focused PR review.
Baseline: main following PR #134 (`7a5dd82f5fb1b49803a667b28c716aabba21c857`).

## Purpose and scope

Replace the planned assignable keyboard-shortcuts milestone and its directly related keyboard/mouse parameter-binding work with one coherent configurable command system. Users can bind keyboard and mouse inputs to individual application actions or ordered sequences, including navigation, node selection and parameter adjustment. The modular graph must remain the authoritative editing model.

Follow AGENTS.md, CODE_STYLE.md, the existing architecture and D052. Prefer the smallest readable shared implementation that satisfies this contract. Do not create a second editing, rendering, persistence, selection or history system.

MIDI, OSC, hardware controllers, full scripting, variables, expressions, loops, conditions, functions and external scripting runtimes are out of scope. Do not build speculative infrastructure for these features. Custom node naming, arbitrary graph rewiring and specialised parameter types are not required by this milestone.

## 1. Command representation and execution

Agreed: use one ordered list of typed commands per binding. A single action is a one-step list. Do not impose a fixed node–parameter–action shape on every binding.

The reason for ordered sequences is practical: Pick then Next Photo; Copy Full Grade then Next Photo then Paste; selective resets of several processors. These should be expressible through existing actions rather than specialised combined commands.

Use structured configuration controls in Settings. The illustrative `$node(...)` syntax is not a language or storage format. No textual parser is required, and a textual summary is optional rather than a deliverable.

Recommended execution contract: run the list once in order, resolving targets afresh at each step. Stop at the first failed or cancelled step, retain completed work and report a useful diagnostic. Existing action guards, confirmation and Undo boundaries apply. Do not introduce rollback or transactional macros. Dialog-opening and deferred operations should be standalone or terminal, unless an existing synchronous completion contract supports continuation. Repeat only a single adjustment operation, not arbitrary chains.

## 2. Input bindings and preferences

Agreed direction: keyboard and modifier-wheel inputs invoke the same command vocabulary. Separate trigger recognition from action implementation. Use ImGui and existing action paths; do not intercept GLFW events or build a parallel input-routing system.

Use the current global preferences lifecycle and atomic writer. Store stable command and key names and typed operands, not raw ImGui enum integers or photo-local NodeIds. Recommended optional bindings field: missing means existing defaults; explicitly empty means deliberately unbound. Keep the final list-shaped representation from the first PR, even if the first editor exposes only one command per binding. Do not implement unused command kinds or speculative operands in advance.

Derive menu shortcut labels from effective bindings, not separately maintained hardcoded strings. A binding containing multiple commands is not advertised as the shortcut for an individual menu action. Preserve actual existing shortcut defaults, including platform-specific alternatives where applicable. Support clearing/resetting assignments and prevent overlapping bindings that could execute simultaneously. Exact preference schema, conflict policy and capture controls are implementation details to validate in PR 1.

Use platform-aware primary modifiers (Command on macOS, Control on Windows/Linux). Respect native reserved keys, actual pinned ImGui semantics, text and numeric entry, active controls, modal dialogs, binding capture, export restrictions and existing panel focus. Keep destructive actions subject to existing context and confirmation. Migrate only handlers for actions made configurable; do not leave duplicate active handlers.

## 3. Node and parameter targets

Agreed: reuse the existing Graph/List selection and `ui::projectNodeList()` ordering. Support current selection, node position, next/previous node and processor identity. Recommended interpretation: one-based processor rows, excluding Source, virtual Output and section headers; no wrap for ordinary adjacent-node navigation. Resolve positions at invocation, not when storing the binding. No second selection model.

For processor identity, store backend plus the existing stable identifier; for parameters, store the existing parameter ID. CTL identity follows the existing canonical script-path convention; this may require manual adjustment on another installation. Do not introduce a processor catalogue or a persisted name-to-ID mapping.

Agreed: offer both a picker populated from the currently open grade (human-readable processor and parameter labels) and manual entry of the same backend/processor/parameter identity fields. An absent processor or parameter at configuration time is not grounds to reject an otherwise well-formed binding. Display a stored identifier when the live display name is unavailable. Resolve against the current grade when invoked.

Agreed duplicate policy: prefer the currently selected matching processor; otherwise use the first matching processor in Nodes List order. If none matches, stop and report an unavailable target. Do not cycle automatically, use a persistent cycle counter, or guess based on canvas coordinates. Explicit cycling is a possible later feature, not a requirement. If the selected matching instance lacks the requested parameter, report that failure rather than silently choosing a different instance.

A select-node command updates the existing visible selection. A target-parameter command associates one transient parameter ID with that selection. The user needs a visible indication of the active parameter. Do not persist that target or retain processor pointers. Clear or revalidate the target when selection, graph, photograph or grade changes, including Undo/Redo reconstruction. Prefer reuse of existing identity and lifecycle boundaries. The exact cue and whether a direct adjustment needs a non-selection-changing form should be decided with concrete workflows in PR 2; a select–target–adjust chain already provides direct addressing without a second resolver.

## 4. Parameter adjustment

Agreed direction: allow explicit finite increments in parameter units and automatic parameter steps, plus setting and resetting values. Reuse `Processor::parameters()`, the shared parameter metadata, and the existing NodeGraph setters/reset functions for native, CTL and OFX. Resolve fresh metadata before use and respect editability, visibility, valid type, graph protection and export restrictions. Initial numerical scope is scalar Double and Integer; specialised vector, choice, push-button and node Strength operations are not mandatory for this first implementation.

Recommended automatic step: declared positive step when available, integer unit step otherwise, or the existing Inspector fallback of 1% of a meaningful finite display span for ranged doubles. An unbounded float without a meaningful step requires an explicit increment. Respect hard limits and representability rather than clamping to the display range. Avoid needless unchanged writes. Backend-specific rounding and notifications remain the responsibility of existing backends. Do not invent a universal physical interpretation of parameter names.

Examples: Exposure E selects the matching processor and targets `exposure`; Plus/Minus adjusts the targeted parameter; an explicit adjustment chain can select Exposure, target its parameter and add 0.1 EV. These are examples, not new default bindings.

The existing Inspector step-button bug (a Plus operation can decrease an exposure value outside its display range) should be corrected when sharing the numerical adjustment operation. Reuse the same meaningful increment/bounds rules where practical; do not change unrelated slider or typed-input behaviour.

## 5. Continuous input and history

Agreed: support held adjustment keys and configurable modified vertical mouse wheel. Initially restrict wheel adjustment to the idle Preview canvas, preserving normal wheel zoom, pinch, Graph zoom and panel scrolling. An eligible configured wheel gesture must not also perform zoom, including when its target is missing. Use existing ImGui ownership and canvas handling. No global raw-wheel polling.

Agreed history grouping: one completed edit per tap, per held adjustment key, and per modifier-held wheel adjustment session. Complete on release and relevant context/target/focus changes or before competing edits, save, departure, export and teardown. No idle timer initially. Reuse existing gesture completion, GradeHistory, DocumentMutation and pending-history behaviour. Do not capture a snapshot for each repeat or wheel tick, or create an independent Undo stack. Discrete commands retain their ordinary history boundaries; selection/targeting is not an editing operation.

## 6. Product choices not yet settled

- Final assignable-action catalog and defaults beyond currently effective bindings. Preserve existing actual defaults; examples are not automatically shipped.
- Whether Quit becomes configurable on platforms where it is not currently handled; native macOS reserved shortcuts must remain native. Do not advertise an ineffective shortcut.
- Exact Settings layout, capture workflow, conflict feedback, target indication and numeric step controls.
- Photo navigation's precise interaction with Filmstrip filters and multi-selection. Recommended: visible Filmstrip order, relative to the open photo, no wrap; reuse successful thumbnail activation semantics. Confirm before implementation.
- Exact handling of fractional trackpad deltas and integer adjustment. Establish through focused native validation rather than speculative machinery.

These decisions should be resolved only when relevant to a PR. They do not justify delaying the basic configurable-action foundation.

## 7. Incremental delivery

### PR 1 — Configurable existing actions

Land the list-shaped typed binding model, preferences integration, a small Settings editor for one-step action bindings, shared action execution and derived menu labels. Preserve actual existing shortcuts and all safety/context requirements. Keep existing local gestures and unmigrated actions unchanged. Correct directly relevant shortcut duplication and text-input safety; resolve the inaccurate Quit label without inventing a native override. Do not implement target/parameter operations, arbitrary chain editing, repeat adjustment or wheel interception yet.

Include this design document and update the roadmap to reflect the combined milestone; record owner-approved choices in the existing decision log with a link to this document. Do not mark subsequent stages implemented.

### PR 2 — Ordered sequences, targeting and scalar editing

Extend the same binding representation/editor for ordered commands. Add node and parameter targets, picker/manual entry, scalar Set/Adjust/Reset, appropriate existing node actions, and photo navigation/marking where approved. Reuse shared operations, correct the Inspector step-button defect and add contextual target feedback. Preserve ordinary per-action history and failure semantics.

### PR 3 — Continuous input

Add held-key adjustment and modifier-wheel adjustment in Preview, using existing gesture and history completion. Verify that normal scrolling/zoom and active text widgets retain ownership, and that a whole hold or modifier session produces one Undo entry. Add only the state actually required for this contract.

These are intended PR boundaries, not mandatory architecture partitions. Adjust if investigation demonstrates a smaller coherent implementation, subject to owner approval for material behaviour changes. Each PR should be usable, reviewed independently and merged only after proportionate validation.

## 8. Validation and review

Use existing preference, UI, graph and history test seams. Test meaningful round trips, unbinding, defaults, label/dispatch agreement, conflicts, routing and active input protection; then targeting, missing/duplicate processors, parameter metadata, bounds, mixed sequences and failure behaviour; then repeated adjustment history and input ownership. Use real native checks for modifier semantics, keyboard layouts and wheel/trackpad behaviour as those features are introduced. Distinguish verified platform behaviour from reported or unverified behaviour. Do not add new test infrastructure solely for theoretical edge cases.

Codex implements focused draft PRs; Claude independently reviews without modifying files; ChatGPT and the owner evaluate findings. Preserve simplicity and human readability as acceptance criteria, not just successful CI.

## Investigation basis

Independent Codex and Claude read-only investigations of the pre-#134 main architecture identified existing application actions, `ui::projectNodeList()`, `ProcessorParameter`, NodeGraph, GradeHistory and ImGui as the shared foundations. Both recommended structured configuration and no scripting engine. Codex proposed ordered typed commands; Claude proposed a narrower fixed-shape binding. The owner selected ordered chaining, current-grade picker plus manual identity entry, selected-match/first-match resolution and release-based history grouping. The detailed implementation must be checked against the current merged baseline rather than assuming old line references remain accurate.
