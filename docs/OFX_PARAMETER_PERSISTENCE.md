# OFX parameter persistence correctness

## Scope and specification

Parameter visibility and value persistence are independent. Initial restoration
precedes the plugin's create-instance action, and private state is synchronised
before parameter capture. These complementary corrections change no persistence
schema and add no undo/redo, migration or recovery of historically omitted values.

Independently checked against the upstream OpenFX documentation and the official
header at RawNode's pinned OpenFX revision
`ab779510b2655b4d11a7e01e5c521f9aa8c88976`:

- [Parameter properties](https://github.com/AcademySoftwareFoundation/openfx/blob/ab779510b2655b4d11a7e01e5c521f9aa8c88976/include/ofxParam.h#L323):
  `kOfxParamPropPersistant` controls saving values and defaults to 1.
  `kOfxParamPropSecret` controls exposure in the UI and defaults to 0; a hidden
  parameter otherwise behaves normally. The spelling `Persistant` is the OFX API.
- [Parameter persistence](https://openfx.readthedocs.io/en/latest/Reference/ofxParameter.html#parameter-persistence):
  persistent values are saved; missing saved parameters use plugin defaults and
  unknown parameters are allowed. Restoration precedes the create-instance action.
- [Image effect actions](https://openfx.readthedocs.io/en/main/Reference/ofxImageEffectActions.html):
  `SyncPrivateData` flushes private state to parameters before capture;
  `CreateInstance` expects restored initial values; host changes to an existing
  instance use bracketed `InstanceChanged` actions.

The original implementation filtered `secret` values from capture, rejected
them on graph-native restore, and never populated `persistValue` from OFX
metadata. Its existing Crop regression incorrectly expected hidden-value loss.

## Implementation and acceptance criteria

`OfxProcessor::parameters()` now maps `kOfxParamPropPersistant` to `persistValue`,
using the specified default when absent. `NodeGraph` capture and restoration
decide value eligibility using persistence and supported type, independently of
`secret`. The Inspector's existing visibility filters remain in place.

| Secret | Persistent | Saved | Restored in a fresh instance | Inspector |
| --- | --- | --- | --- | --- |
| false | true | current value | saved value | visible |
| true | true | current value | saved value | hidden |
| false | false | omitted | plugin default | visible |
| true | false | omitted | plugin default | hidden |

The graph-native contract still rejects explicitly supplied values for known
installed non-persistent parameters. It does not silently ignore or migrate
incompatible saved values. Such sidecars open as protected source views and
cannot be overwritten. Capture erases now-known transient values from retained
JSON, while retaining unknown parameter data and its original precision.
Native and CTL metadata/defaults are unchanged; in particular ART CTL preset
selectors remain transient and their resulting adjustments remain persistent.

User-visible changes, all required by this correction:

- Sidecars, presets and clipboard transfers retain hidden persistent OFX values.
- Hidden persistent values are now subject to RawNode's existing strict
  representability checks; malformed UTF-8, non-finite numbers or invalid choices
  may cause saving or copying to fail explicitly instead of being silently omitted.
- They omit OFX values declared non-persistent. Fresh instances keep their
  defaults for these omitted controls.
- A saved known non-persistent value triggers the existing incompatible-grade
  protection/refusal, now also for OFX. There is no new warning or recovery flow.

There are no appearance, control, selection or interaction changes.

## Regression coverage and evidence

`tests/OfxPersistenceTests.cpp` registers an in-process OFX descriptor and uses
the production adapter, factory, graph codec, atomic file writer, document
actions, clipboard callbacks, Inspector and evaluator. No test-only processor
backend or persistence format is enabled.

- All four flag combinations have non-default live values. The persistent
  numeric values are 2 and 3; transient values 17 and 19 must reopen as 1.
- A hidden custom string includes quotes, a newline and Unicode; it omits the
  persistence property to verify the OFX default. Unknown structured JSON
  includes an integer above double's exact range and a long decimal token.
- Real PNG creation/opening, `saveCurrentInputSidecar`, a fresh App reopening
  the file, and same-photo save-before-reopen exercise ordinary sidecars.
- `savePreset`, `loadPresetFile`, and preset UI actions with supplied paths
  exercise Replace and Append onto separate, opened PNG photographs. Supplied
  paths bypass only native file choosers.
- `copyGrade`, `copySelectedNode` and `pasteFromClipboard` exercise full grades
  and fragments onto opened photographs through ImGui clipboard callbacks.
  Inserted nodes get fresh IDs.
- The diagnostic plugin reads initial gain through the real OFX parameter suite
  during `CreateInstance` and builds a private cache. Render uses that cache;
  ordinary UI changes update it, while restoration sends no change actions.
  The evaluator independently expects `(1.5, -3, 12, 0.375)` from input
  `(0.25, -0.5, 2, 0.375)` at gain `2 * 3` after every reconstruction. This tests
  actual OFX processing, negative/HDR samples and unchanged alpha, beyond merely
  checking matching serialized values. Every reconstruction also processes its
  actual decoded photograph, checking the independently expected RGB gain of 6
  and unchanged alpha.
- Real Inspector frames/logging verify hidden persistent and transient values
  stay hidden both normally and with parameter search.
- Injected known transient values refuse preset replacement without changing
  destination processors; ordinary photo opening protects the incompatible
  sidecar and save refusal leaves its bytes intact.
- The existing bundled Crop test now checks hidden persistent capture/restore.
  Existing full-suite native, standard CTL and ART CTL tests cover their unchanged
  persistence and rendering behaviour.

Linux Release validation commands:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j4
build/OfxRawHost --selftest
git diff --check
```

The baseline and corrected full self-test suites pass. Temporary mutations were
rebuilt and run against the full suite, then removed:

| Mutation | Result |
| --- | --- |
| Restore all three original behaviours | Exit 1: saved parameter assertions fail |
| Reintroduce only capture's secret filter | Exit 1: saved parameter assertions fail |
| Reintroduce only restore's secret rejection | Exit 1: fresh photo reopen becomes protected |
| Remove only the OFX persistence mapping | Exit 1: saved parameter assertions fail |

Native desktop dialogs and a third-party plugin collection are outside this
automated evidence; the Inspector is rendered headlessly with actual ImGui.

## Initial values before CreateInstance

The host prepares clips, parameter instances and defaults without invoking the
plugin. `OfxProcessor::create` then runs a parameter-initialisation callback and
calls `CreateInstance` exactly once. The graph layer supplies that callback,
reusing its existing parsing, persistence eligibility and exact-value checks.
No sidecar schema enters the OFX adapter. Native and CTL restoration is unchanged.

Numeric OFX choices have one deliberate validation boundary: JSON must contain
an integer index (not a string, boolean or fraction), but menu membership is
checked after creation. A plugin may start with an empty or partial descriptor
menu and append options to its instance during `CreateInstance`. The saved
selection is already accessible through the OFX parameter suite at that time.
Before publishing the node, the graph layer validates both the saved selection
and the plugin's resulting selection against the completed instance menu.
Valid plugin edits are retained; values are never restored a second time.
An invalid saved selection is still rejected even if the plugin replaces it
with a valid default. An out-of-range numeric index can therefore reach
`CreateInstance` before final menu membership validation; malformed JSON and
non-integer types still fail before creation. An empty final menu causes
restoration refusal when a saved choice is being validated, as does an invalid
resulting selection. An unsaved choice does not enter this saved-value check.

`persist_detail::parameterValue` remains strict by default. Only pre-creation
OFX numeric membership is explicitly deferred; its JSON/type checks, supported
parameter and persistence rules are unchanged. Stable choice IDs still require
strings and resolve through their existing ID/value mapping; deferral does not
coerce numeric values to IDs or accept unknown IDs. Native/CTL, capture and
other parsing callers retain complete validation.

Change actions are guarded until creation succeeds. A parameter preparation
error releases host objects without sending
`DestroyInstance`. A failed create action likewise sends no destroy action;
success (including `ReplyDefault`) enables normal destruction. Existing graph
staging publishes only completed processors and retains missing-processor data.
Failure in post-creation choice validation destroys the successful instance
normally and leaves the existing document and sidecar protection intact.
Saved values are not applied again after the plugin's create action.

This follows the precondition in the pinned
[OpenFX header](https://github.com/AcademySoftwareFoundation/openfx/blob/ab779510b2655b4d11a7e01e5c521f9aa8c88976/include/ofxCore.h#L332).
The independent references were examined directly:

- [Natron RB-2.6, `createOfxImageEffectInstance`](https://github.com/NatronGitHub/Natron/blob/3763d805d7d277d10af10025ae41af677682b3e6/Engine/OfxEffectInstance.cpp#L344)
  calls `populate`, loads serialized knob values, and only then calls
  `createInstanceAction`. Its [`knobChanged` guard](https://github.com/NatronGitHub/Natron/blob/3763d805d7d277d10af10025ae41af677682b3e6/Engine/OfxEffectInstance.cpp#L2548)
  suppresses normal notifications until initialisation is complete.
- [TuttleOFX's copy constructor](https://github.com/tuttleofx/TuttleOFX/blob/13936341f897037b7a6be0a72aefe9d32c2bb849/libraries/tuttle/src/tuttle/host/ImageEffectNode.cpp#L52)
  populates, copies attribute values, then calls `createInstanceAction`.
  Its [OFX node implementation](https://github.com/tuttleofx/TuttleOFX/blob/13936341f897037b7a6be0a72aefe9d32c2bb849/libraries/tuttle/src/tuttle/host/ofx/OfxhImageEffectNode.cpp#L347)
  sends `DestroyInstance` only after successful creation.

Only the callback order and lifecycle guard inform this correction; no reference
project's threading, GUI or knob infrastructure is introduced.

Additional regression checks save gain 5, reopen the real photo, and assert the
saved value, value observed during creation, private cache and exact pixels
`(1.25, -2.5, 10, 0.375)` for input `(0.25, -0.5, 2, 0.375)`. All numbers are
exact binary fractions. Missing defaults, hidden custom data, typed values,
strict validation at the appropriate creation boundary, failed creation, staged graph cleanup and
plugin edits during creation are covered. The last case detects a second restore.
Preset Replace/Append and grade/node clipboard paths use the same cached fixture.

The review correction adds a choice whose descriptor has no options. It saves
index 2 to a real photo sidecar; creation reads 2 through the real suite, then
adds three instance options. Reopening succeeds and preserves unknown JSON.
Tests also cover a partial descriptor menu, valid plugin selection changes,
invalid saved/resulting choices, a still-empty final menu, protected sidecar
bytes, unchanged preset destinations and balanced post-creation destruction.
Static choices and stable-ID parsing retain their existing strict tests.
The fixture independently counts every Begin/Instance/EndChanged action received
before successful creation, including calls made with `notify=true` during
parameter preparation; the expected count is zero.

Initial implementation validation on 2026-10-22 used the full self-test suite:

| Check | Result |
| --- | --- |
| Release, GCC 14 | Pass, exit 0 |
| Debug ASan/UBSan, GCC 14, leak detection enabled | Pass, exit 0 |
| Debug TSan, GCC 14, halt on error | Pass, exit 0 |
| Gain-5 test on unmodified `main` production sources (`1857a0b`) | Expected exit 1: creation/cache observed 1 |
| Mutation moving the initialisation callback after `CreateInstance` | Expected exit 1: same gain-5 creation/cache assertion |

The old-main build retained the same gain-5 fixture and real photo save/reopen
test; only tests depending on the new factory callback API were omitted.
Both temporary changes were removed, then all three final suites were rebuilt
and passed. The only UBSan exception is the repository's existing narrow JPEG
`shift-base` suppression. LeakSanitizer's sandboxed thread attachment was denied;
the final leak-enabled run passed outside that sandbox. No extra suppression or
production workaround was added. `git diff --check` also passed.

Review-correction mutation checks independently exercise the new assertions:

| Mutation | Result |
| --- | --- |
| Validate numeric menu membership before `CreateInstance` | Expected exit 1: saved index 2 reopens as protected |
| Remove post-creation choice validation | Expected exit 1: invalid resulting selection is accepted |
| Remove the `initialized_` notification guard | Expected exit 1: pre-creation notification counter is nonzero |

After removing all three mutations, the corrected source passed complete Release,
leak-enabled ASan/UBSan and TSan self-tests (GCC 14). These runs include the original
gain-5 creation/cache/pixel test, successful/failed plugin destruction checks,
static choices, stable-ID parsing, dynamic menus and notification suppression.
The existing narrow JPEG UBSan exception is unchanged; no new suppression is used.

User-visible changes are limited to these restoration paths initialising plugin
private state from saved values and preserving reopening of valid instance-defined
OFX choices. Invalid choices still protect/refuse the saved grade. There are no
appearance or interaction changes.

## Private-state synchronisation before capture

### Verified requirements and implementation

Checked independently against the official
[`ofxParam.h`](https://github.com/AcademySoftwareFoundation/openfx/blob/ab779510b2655b4d11a7e01e5c521f9aa8c88976/include/ofxParam.h#L271),
[`ofxCore.h`](https://github.com/AcademySoftwareFoundation/openfx/blob/ab779510b2655b4d11a7e01e5c521f9aa8c88976/include/ofxCore.h#L297)
and upstream generated [action specification](https://github.com/AcademySoftwareFoundation/openfx/blob/main/Documentation/sources/Reference/ofxImageEffectActions.rst).
The upstream current headers agree with RawNode's pinned headers on this contract.
The action takes the effect instance, null input/output arguments, and follows
`CreateInstance`. Its postcondition is reconstructible private state in the
parameter set. `OK` and `ReplyDefault` are non-failures; other returned statuses
abort capture and are reported with the plugin identifier and OFX status.
The [parameter-setting rules](https://github.com/AcademySoftwareFoundation/openfx/blob/ab779510b2655b4d11a7e01e5c521f9aa8c88976/Documentation/sources/Reference/ofxParameter.rst#setting-parameters)
explicitly permit parameter writes in this callback. The
[thread/recursion rules](https://github.com/AcademySoftwareFoundation/openfx/blob/ab779510b2655b4d11a7e01e5c521f9aa8c88976/Documentation/sources/Reference/ofxThreadSafety.rst)
put it on the instance's main thread and permit it to trigger recursive actions.

`kOfxPropParamSetNeedsSyncing` belongs to the **parameter-set instance**:

| Property | Before parameter capture | After successful action |
| --- | --- | --- |
| absent | call `SyncPrivateData` every time | leave absent; never invent a default |
| 1 | call `SyncPrivateData` | set to 0 |
| 0 | skip action | leave 0 |

There is an upstream specification/library discrepancy. The pinned header says
"Property Set - param set instance (read/write)", and the official
[properties-by-object reference](https://github.com/AcademySoftwareFoundation/openfx/blob/ab779510b2655b4d11a7e01e5c521f9aa8c88976/Documentation/sources/Reference/ofxPropertiesByObject.rst#properties-on-parameter-set-instances)
also places it on the parameter-set instance. However, the official plugin
[Support helper](https://github.com/AcademySoftwareFoundation/openfx/blob/ab779510b2655b4d11a7e01e5c521f9aa8c88976/Support/Library/ofxsImageEffect.cpp#L1355)
sets `_effectProps`, and
[HostSupport's `syncPrivateDataAction`](https://github.com/AcademySoftwareFoundation/openfx/blob/ab779510b2655b4d11a7e01e5c521f9aa8c88976/HostSupport/src/ofxhImageEffect.cpp#L801)
reads the effect instance's `_properties`. Those libraries use the
**effect-instance property set**, not the specified parameter-set instance.

RawNode follows the specification conservatively: it reads only the parameter-set
flag and creates no default. If that flag is absent, every capture synchronises,
even if a plugin using the Support helper has an effect-instance flag of 0.
RawNode neither reads nor clears that effect-instance flag. It does not treat
the SDK helper's different property location as permission to skip the action.
The existing effect/individual-parameter decoys independently cover this choice.

Natron's [`NodeSerialization.cpp`](https://github.com/NatronGitHub/Natron/blob/RB-2.6/Engine/NodeSerialization.cpp)
calls `syncPrivateData_other_thread()` before obtaining serialised knobs.
[`OfxEffectInstance.cpp`](https://github.com/NatronGitHub/Natron/blob/RB-2.6/Engine/OfxEffectInstance.cpp)
dispatches through its main-thread `onSyncPrivateDataRequested()` slot, enables
parameter writes, and avoids a preferences lock that could deadlock recursive
actions. RawNode uses its own existing control-thread/execution contract; no
Natron scheduling or lifecycle machinery is imported.

`Processor::syncPrivateData()` is an explicit capture-only hook, a no-op for
native/CTL processors. `OfxProcessor` implements the property/action protocol.
The common node capture helper calls it once immediately before reading that
processor's parameters. `parameters()` remains an ordinary snapshot for the
Inspector, parameter edits and restoration. No change notifications are added.

### Capture and ownership audit

| Capture | Existing execution boundary and common path |
| --- | --- |
| Explicit photograph Save Grade | `savePhoto` -> `captureGraph` |
| Automatic same-photo reopen, navigation, folder switch, close/shutdown | departure -> `savePhoto` -> `captureGraph` |
| Initial editable photograph save | `openPreparedPhoto` -> `savePhoto` -> `captureGraph` |
| Full grade / preset save / grade clipboard | `captureGrade` -> `captureGraph` |
| Individual node clipboard / multi-node fragments | `captureNodeFragment` -> `captureFragment` |
| Low-level graph, fragment and historical V2 chain helpers | caller-owned exclusion -> common node capture |

All live document paths above use `DocumentMutation` on the control thread.
`RenderRuntime::beginMutation` closes dispatch, cancels/drains preview and waits
for export before the plugin callback. The scope excludes subsequent execution
and control-thread ownership prevents concurrent graph edits/destruction.
`completeMutation` retains quiet recovery of interrupted work. Capture does not
mark the document changed or request a new render merely for flushing private
state, which should represent the same processing state in parameter form.

The plugin action runs without `gValueMutex`; its parameter setters can acquire
that mutex normally. Parameter snapshots take it only after the action returns.
Low-level helpers retain their explicit caller-owned lifetime/exclusion contract
for detached graphs and tests; they must not be used unguarded on an executing
live graph. Document/UI single-node and multi-node capture both use
`captureNodeFragment`; graph capture helpers do not call document actions. No new
scheduler, mutation system or processor lifecycle framework is introduced.

Synchronisation failure aborts the complete logical capture before its result
is published or any file/clipboard replacement starts. The existing atomic file
writer and staged capture outputs remain authoritative. A callback can partially
change **live** parameters before failing; RawNode does not roll back plugin
side effects. It leaves the needs-syncing flag uncleared and a later capture can
retry. This is publication safety, not a transaction over arbitrary plugin code.

### Regression evidence and precise guarantee

`tests/OfxPersistenceTests.cpp` extends the existing in-process descriptor with
a separate private-state diagnostic plugin. Its live hidden gain 5 initially
differs from saved hidden gain 3, with visible gain 2. Before capture, ordinary
parameter reads still report 3, while independently expected processing gain is
10. The sync callback writes through the real OFX `paramSetValue` suite.
This deferred-state fixture renders from current parameters in fresh instances
to isolate capture behaviour. A separate cache-based integration regression below
checks the combined initial-restoration and synchronisation contract.

Coverage includes explicit/automatic sidecar save and fresh-App reopen, preset
save/load, full-grade and node clipboard paste, multi-node fragments, disabled
and unreachable processors, single-node and low-level graph/V2 captures, exact
per-capture action counts, absent/0/1 flags (including decoy effect/individual
parameter properties), successful no-op `ReplyDefault`, callback parameter writes,
and `Failed`/`ErrFatal` publication refusal after a partial live write. Existing
valid sidecars/presets/clipboard and caller outputs survive failure; new failed
presets are absent, photo replacement/departure refuses, and retry succeeds.
Every reconstruction checks saved/restored values and independently specified
RGB gain with negative/HDR samples and unchanged alpha, plus decoded photo pixels.
`tests/GraphRuntimeTests.cpp` also verifies the hook runs only under mutation
exclusion after blocked preview/export work drains, including capture failure
and quiet recovery.

**Guarantee:** before any supported processor-state capture reads OFX parameters,
RawNode obeys the parameter-set flag and completes the required private-data
synchronisation. Failed actions do not publish a saved grade, preset or clipboard
payload. For plugins that reconstruct processing from their current persistent
parameters, the tested private state survives save/reopen and transfers with the
expected output. Initial restoration now supplies saved values before
`CreateInstance`, so plugins can rebuild their caches from those values before
the first render or subsequent capture. The combined regression below checks
that synchronisation cannot overwrite saved gain 5 with a cache built from defaults.

User-visible changes required by this correction: deferred OFX state is included
in saves/copies, and sync failures use the existing save/copy error paths while
preserving prior valid output and refusing automatic departure. No appearance,
controls or interaction workflows change. No schema, migration or undo/redo
changes are included. Initial restoration is supplied by the separately merged
PR #91; synchronisation adds no further restoration changes.

Original #90 validation, before integration with #91:

- Linux GCC 14 Release: full self-tests pass, including existing native, standard
  CTL, ART CTL and OFX persistence/rendering tests.
- GCC Debug with `-fsanitize=address,undefined -fno-omit-frame-pointer
  -fno-optimize-sibling-calls`: full self-tests pass with
  `ASAN_OPTIONS=detect_leaks=1:halt_on_error=1:symbolize=1` and
  `UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1:suppressions=$PWD/.github/sanitizers/ubsan.supp`.
  This uses only the existing narrow JPEG UBSan suppression. The final leak scan
  required running outside the execution sandbox, whose process restrictions
  otherwise prevent LeakSanitizer inspection.
- GCC Debug with `-fsanitize=thread -fno-omit-frame-pointer -O1`: full self-tests
  pass with `TSAN_OPTIONS=halt_on_error=1:second_deadlock_stack=1` and no suppressions.
- Temporary mutation returning success immediately from
  `OfxProcessor::syncPrivateData()`: rebuilt Release suite exits 1 at the saved
  hidden-value assertion (stale 3 instead of independently expected 5). The
  mutation is removed; the corrected suite passes again.
- `git diff --check` passes. Repository macOS arm64 Release, Linux Release and
  Linux Clang ASan/UBSan workflows run on the PR; their results are recorded there.

### Combined restoration and synchronisation regression

PR #91 supplies saved initial values before `CreateInstance`; PR #90 flushes
private state before capture. The combined test `cachedSyncRoundTrips` exercises
both through real photograph, document and OFX parameter-suite paths. It extends
the existing diagnostic fixture locally; no lifecycle or test framework is added.

The fixture reads visible gain 5 and hidden gain 1 during `CreateInstance`, builds
private render gain 5, renders exclusively from that cache, and writes the cache
back through `paramSetValue` during `SyncPrivateData`. The parameter-set flag is
absent, so synchronisation is required at each capture. After initially saving a
photograph, a fresh App opens it and checks the parameter, creation-time value,
cache and independently calculated pixels. Navigation without editing triggers
automatic saving, after which the sidecar must still contain 5.

Full-grade copy/paste retains the grade ID; node copy/paste remaps it. Both
destination photographs check their restored values/caches, explicit sidecar
save, automatic navigation save and fresh-App disk reopening. The source also
navigates away after copying without an edit and must retain 5. Action counters
verify the real sync callbacks ran. Expected RGBA is exactly
`(1.25, -2.5, 10, 0.375)` for input `(0.25, -0.5, 2, 0.375)`; decoded photograph
pixels independently use RGB gain 5 with unchanged alpha.

The first cache/pixel checks are recorded before departure but asserted after
the sidecar check. This deliberately lets the incorrect-creation-order mutation
reach automatic saving: creation caches default 1, restoration then exposes
parameter 5, and synchronisation writes stale cache 1 back into the sidecar.
The regression must fail on that saved-value loss, rather than stopping at the
earlier cache mismatch. Existing #90 absent/0/1, failure-publication and blocked
preview/export ownership regressions and #91 choice/lifecycle tests remain active.

Integration validation on 2026-10-22 rebases #90 onto main
`3f1f53ec194f4c4e35d482bed01f39cdc16060e0` (merged #91):

| Check | Result |
| --- | --- |
| Complete Release, GCC 14 | Pass, exit 0 |
| Complete Debug ASan/UBSan, GCC 14, leak detection enabled | Pass, exit 0 |
| Complete Debug TSan, GCC 14, halt on error | Pass, exit 0 |
| Mutation returning success without private synchronisation | Expected exit 1: combined fixture's first save receives zero sync callbacks |
| Mutation restoring values after `CreateInstance` | Expected exit 1: unedited navigation saves stale gain 1 over gain 5; diagnostic prints `expected 5, got 1` |

Both mutations were rebuilt as Release and run against the complete self-test
entry point, then removed. The corrected Release suite was rebuilt and passed
again; final ASan/UBSan and TSan builds/suites also passed. Sanitizer flags and
runtime options are the same as the capture validation above: leak detection is
enabled, only the existing narrow JPEG UBSan exception is used, and TSan has no
suppressions. Process inspection required the sanitizer runs outside the sandbox.
The reviewed #90 `SyncPrivateData` implementation is unchanged. `git diff --check`
passes. The PR records the exact new head and Linux/macOS CI results for it.

## Remaining limits

Successful initial restoration and capture are complementary guarantees. They do
not roll back arbitrary live side effects from a failed plugin action or add a
general redesign of existing-instance parameter-change notifications. A plugin
is responsible for encoding private state into its persistent parameters and
reconstructing it during creation. Invalid final saved-choice membership still
refuses restoration after creation; that validation boundary is described above.
Desktop dialogs and a representative third-party plugin collection remain
outside the automated evidence. Saved values are restored before `CreateInstance`.
Historical migrations, undo/redo and new scheduling remain outside these corrections.
