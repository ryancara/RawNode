# OFX parameter persistence correctness

## Scope and specification

Parameter visibility and value persistence are independent. Initial restoration
also precedes the plugin's create-instance action. Neither correction changes
the persistence schema or adds undo/redo, migration or recovery of historically
omitted values.

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

Change actions are guarded until creation succeeds. An invalid saved value or
exception during parameter preparation releases host objects without sending
`DestroyInstance`. A failed create action likewise sends no destroy action;
success (including `ReplyDefault`) enables normal destruction. Existing graph
staging publishes only completed processors and retains missing-processor data.
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
strict validation before creation, failed creation, staged graph cleanup and
plugin edits during creation are covered. The last case detects a second restore.
Preset Replace/Append and grade/node clipboard paths use the same cached fixture.

Validation on 2026-10-22 used the full self-test suite:

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

The only user-visible change is that these restoration paths initialise plugin
private state from the saved values. There are no appearance or interaction changes.

## Remaining OFX capture limitation

No production caller dispatches `kOfxActionSyncPrivateData`. A plugin which defers
writing private state to persistent parameters until that action can still save
stale values. `kOfxPropParamSetNeedsSyncing` is not honored either. The
[official header](https://github.com/AcademySoftwareFoundation/openfx/blob/ab779510b2655b4d11a7e01e5c521f9aa8c88976/include/ofxParam.h#L271)
requires synchronization when that optional flag is set or absent; setting it to
zero permits skipping synchronization. This separate capture issue remains
outside the initial-restoration correction. No SyncPrivateData handling or
general existing-instance notification redesign is included here.
