# OFX parameter persistence correctness

## Scope and specification

This correction separates parameter visibility from value persistence. It changes
no persistence schema, instance lifecycle or undo/redo behaviour, and adds no
migration or recovery of historically omitted values.

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
- The evaluator independently expects `(1.5, -3, 12, 0.375)` from input
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

## Remaining OFX state-restoration limitations

These are separate pre-existing correctness issues affecting ordinary sidecars,
presets and clipboard transfers, rather than concerns specific to future undo.
They prevent a universal same-output guarantee for arbitrary OFX plugins even
after their persistent parameter values are saved correctly. The verified
same-output guarantee above covers plugins reading their current parameter set
during processing, including the test fixture; it does not cover arbitrary
private-state reconstruction.

1. **Private data is not synchronized before capture.** No production caller
   dispatches `kOfxActionSyncPrivateData`. A plugin which defers writing private
   state to persistent parameters until that action can save stale values.
   `kOfxPropParamSetNeedsSyncing` is not honored either. The
   [official header](https://github.com/AcademySoftwareFoundation/openfx/blob/ab779510b2655b4d11a7e01e5c521f9aa8c88976/include/ofxParam.h#L271)
   requires synchronization when that optional flag is set or absent; setting it
   to zero permits skipping synchronization. Removing the visibility filter
   alone cannot flush deferred private state.
2. **Restoration happens after `CreateInstance`.** `OfxPlugins::createInstance`
   clones descriptors, applies defaults, then calls the action.
   `NodeGraph::restoredNode` only then applies saved values. Plugins building
   private state or adjusting parameter metadata from initial saved values see
   defaults instead. This violates the specified setup ordering even when the
   sidecar contains every persistent value.
3. **Post-creation restoration suppresses change notifications.**
   `restoredNode` calls `setParameterValue(..., false)`. No final notification
   reconciles plugin caches with restored values. The existing UI edit path does
   send Begin/Instance/EndChanged with `kOfxChangeUserEdited`; loading values into
   an already-created instance currently does not. Blindly enabling callbacks
   here could cause parameter-order-dependent overwrites and is not equivalent
   to restoring a complete initial set before creation.

Temporary diagnostic probes used the same in-process descriptor and real PNG
save/reopen paths. They were removed after collecting evidence, rather than
adding regression assertions that would require retaining these bugs:

- A callback initialized a private gain from parameters at `CreateInstance`,
  refreshed it on `InstanceChanged`, and rendered using that cache. Ordinary
  edits set visible/hidden gains to 2 and 3. Before saving, cached gain was 6
  after two notifications. On fresh reopen the parameter values were correctly
  2 and 3, but creation had initialized gain to 1 and restoration sent zero
  notifications. A 0.25 red input rendered as 1.5 before save and 0.25 after
  reopen. This demonstrates the combined creation-order/notification failure.
- The probe's private gain was then changed to 10 without updating the parameter
  set. Its sync handler would write hidden gain 5 (visible gain remained 2).
  Ordinary save wrote hidden gain 3, with zero sync calls. Explicitly invoking
  the handler diagnostically before saving wrote 5, with one sync call. The
  optional needs-syncing property was absent, so synchronization was required.

Correcting those lifecycle issues requires a separately scoped OFX change;
this PR does not silently add synchronization callbacks, change instance
creation ordering or redesign processor restoration. They directly limit the
broad interpretation of “restored output is always identical,” and should be
resolved before relying on that claim for arbitrary OFX plugins. No migrations,
compatibility mechanisms or undo/redo work are proposed here.
