# Step 6 macOS validation

**Status:** Step 6 implementation and independent whole-Step-6 review are complete
and merged in PR #58 (`6294a034`). Reported macOS testing found normal operation,
but complete per-check native-dialog and Nikon NEF validation evidence is not
recorded. Unverified checks below remain unchecked; evidence or an explicit
documented waiver is tracked in `docs/PROJECT_CONTEXT.md` before Step 7 replaces
the relevant capture/sidecar paths.

The implementation environment is Linux. No suitable Nikon NEF/RAW fixture was
available there, so successful camera-RAW grade application (T3) is not claimed
as automated coverage. Synthetic raster tests do not replace this check.

## Step 7 live-persistence prerequisite

PR 3 preflight checked `main` at
`fccd9e53e9a3878cce9678d64c5f362323743646`, including merged PRs #63–#65.
The available manual evidence remains the general report of normal macOS
operation above. No tested commit, OS/hardware details or per-check results are
recorded here for the native-dialog checklist or Nikon NEF T3 re-decode checks,
including differing RAW/Output encodings and downscaled previews.
The repository documentation and discussions on PRs
[#58](https://github.com/ryancara/RawNode/pull/58),
[#62](https://github.com/ryancara/RawNode/pull/62) and
[#65](https://github.com/ryancara/RawNode/pull/65) supplied no additional per-check
manual results or PR 3 waiver during this preflight.

**Owner waiver, 2026-10-16:** Ryan explicitly waived the outstanding historical
Step 6 macOS/native-dialog and Nikon NEF validation prerequisites so Step 7 PR 3
can replace the affected live paths. This satisfies the prerequisite in
[the Step 7 design](STEP7_PERSISTENCE_DESIGN.md#8-focused-implementation-prs);
it is not evidence that any check passed. All unchecked items below remain
unverified. The new opening, saving and RAW re-decode workflow will be validated
separately on macOS, including native dialogs and actual Nikon NEFs.

Use merged Step 6 (`6294a034`) or later `main` and record the tested commit.
Use working copies of two visibly different Nikon NEFs (A and B), and a separate
export folder. Include the Nikon models/compression
modes used in normal editing; record any unsupported decode separately. Run
`build/OfxRawHost.app/Contents/MacOS/OfxRawHost --selftest` as a platform check.

## Editing and native dialogs

- [ ] Open A and B with **Open image** and through a workspace/filmstrip. Confirm
  the correct photo, input encoding, orientation and settled preview dimensions.
- [ ] On A add Exposure and CST, edit their parameters, enable/disable them,
  reorder with arrows and drag/drop, and remove selected/unselected processors.
  Selection and parameter controls must follow the logical node; removing the
  selected node clears its controls. Removing all processors shows Source and
  disables export.
- [ ] Give A and B different grades. Switch repeatedly while previews render;
  revisiting each photo restores its own grade without retaining stale controls.
- [ ] Use **Edit → Copy Node / Paste** and **Copy Full Grade / Paste**. Node paste
  inserts at the selected spine node with fresh identity; full-grade paste
  replaces the grade. Parameters/filter and preview must match the new grade.
- [ ] Through **Edit → Presets**, save/load node and full-grade presets using
  native dialogs. Check cancel, extension handling and loading onto the other
  photo. Node presets insert; grade presets replace.
- [ ] Start a full-resolution export of A, then switch to B and edit. The export
  must contain A with its accepted grade and full dimensions; B's preview must
  settle correctly afterward. Inspect output and source sidecars separately.
- [ ] Check wheel zoom, double-click fit, left/middle-button pan, parameter
  search, preview resolution and panel/filmstrip toggles. Changing output
  interpretation should refresh display without losing edits.
- [ ] Quit during preview/export and reopen. Confirm clean shutdown and each
  photo's saved linear grade. Save results with OS, hardware, commit and filenames.

## T3: successful RAW grade re-decode

1. Open A. Set RAW colour space/gamma to **Rec.2020 / Linear**, add Exposure at
   **+1 EV**, and choose **Display P3 / sRGB** output interpretation. Select the
   Exposure node; use **Copy Full Grade** and save a full-grade preset.
2. Open B. Set RAW to **Rec.709 / Linear**, Exposure to **−1 EV**, and output to
   **Rec.709 / sRGB**. Use **720p** preview (1280-pixel long-edge cap), wait for it to
   settle, and type a parameter filter so stale UI state would be visible.
3. Paste A's full grade onto B. This deliberately requires a RAW re-decode.
   The input label and RAW controls must become **Rec.2020 / Linear**, output
   controls **Display P3 / sRGB**, Exposure **+1 EV**, and the filter must clear.
   The settled preview must still show **B**, with the new grade and expected
   preview dimensions; exporting must produce B at full resolution.
4. Reset B to step 2 and load the saved full-grade preset. Require the same
   settled preview, source/input/output settings and export as the clipboard
   application. Neither operation should change the other photo's source.
5. Save/quit/reopen B and compare with a fresh decode of B manually configured
   with the exact same RAW encoding, grade and output interpretation. Preview
   and full-resolution output must agree. This checks that live full Source and
   preview replacement did not retain the previous encoding's pixels.
6. Repeat with a different RAW gamut and a non-linear RAW gamma (for example
   DaVinci Intermediate) at downscaled preview resolution. Use a CST matching
   that input if the grade is intended to process linear values. This exercises
   decode/resample/re-encode preparation before grade commit.

For a Debug inspection of T3, stop at entry to `applyGrade` and after its live
replacement. Record `app.full.px.data()`, `app.preview.get()`, raster dimensions,
`inputEncoding`, `outputEncoding`, `inputIsRaw` and `rawWorkingEncoding`. Full
Source and preview owners must be replaced, `inputIsRaw` must remain true, input
and output encodings must match the grade, and the session RAW default must stay
unchanged by grade application. The published endpoint must be the new
`documentOutput`, rather than a retained inspection request.

## Step 7 photo workflow follow-up — unverified

The historical prerequisite waiver does not complete these checks. Record the
actual tested PR 3 commit, macOS/hardware and Nikon models/compression modes when
the owner performs this separate validation:

- [ ] Build on macOS and run the complete self-tests plus `--selftest-photo`.
- [ ] Exercise native image/folder dialogs, filmstrip navigation and dropped
      files. Scanning/thumbnails create no sidecars; successful editable opening
      creates a Source-only grade or adds one while preserving existing metadata.
- [ ] Open valid saved grades without an immediate rewrite. Switch two Nikon
      NEFs repeatedly with different RAW/Output pairs, including non-linear RAW
      gamma at downscaled preview, and compare source/preview/export with fresh
      decodes using those exact settings. Photo controls must not change defaults.
- [ ] Open malformed/unsupported/legacy sidecars as protected source views;
      verify persistent warnings, normal viewing/navigation and refusal of edits,
      colours, paste, presets and saving without changing original files.
- [ ] Force an initial save failure and confirm the photo remains editable and
      its warning survives rendering/export status. Retrying a successful save
      clears it; a cancelled departure retains the warning and document.
- [ ] Exercise the native Yes / No / Cancel helper: Yes means Retry, No means
      explicit Discard, and Cancel/dismissal retains the current editor. Test
      navigation, folder changes, same-photo reopening and window/menu shutdown.
- [ ] Verify same-photo reopening saves before reload, and unchanged departure
      saves the complete grade while retaining missing processors, opaque values,
      identities, positions and metadata. Cancelled shutdown must leave preview,
      editing and a later save usable; quit during preview/export must drain safely.

### Linux save-failure dialogs — unverified

- [ ] With real kdialog and Zenity-family desktop helpers, force a save failure
      during navigation, folder change, reopening and shutdown. Yes must Retry,
      No must explicitly Discard, and Cancel/dismissal must retain a usable editor.
      Scripted helper-process tests verify the vendored exit/output mapping;
      they do not establish real desktop dialog behaviour.
- [ ] With no supported desktop helper installed, verify failed departure safely
      keeps the current photo. Retry and Discard are unavailable in this case;
      the save problem must be fixed to leave. Launch with a supported helper
      available for Retry/Discard. The library's echo fallback supplies no usable
      dialog. No automatic discard or alternative recovery UI is provided.

## Step 7 PR 4 manual checks still required

Linux deterministic tests cover graph-native Replace with synthetic RAW decoding,
complete preset capture/overwrites and batch Append/Paste. They do not establish
macOS native-dialog or actual Nikon NEF behaviour. The recorded historical waiver
was for PR 3; these PR 4 checks remain unverified:

- Save Preset from Source-only, one-processor and branched grades; cancel a save
  and an overwrite, including after automatic `.rawnode.json` extension addition.
- Confirm overwrite of a valid photo sidecar preserves its metadata; unsupported
  or protected destinations retain their bytes even after confirmation.
- Load Preset → Replace/Append; verify middle/end insertion, unrelated branches
  and disabled unsupported attachments. Check Copy Node and Copy Full Grade/Paste.
- Repeat T3 above using `.rawnode.json` Save Preset and Load Preset → Replace,
  and full-grade clipboard Paste, on actual Nikon NEFs with differing RAW pairs.
- Repeat while preview/export owns the old graph; verify the accepted old export
  and settled new preview. Record tested commit, macOS/hardware and NEF models.

## Step 7 PR 5 manual checks still required

Linux tests exercise platform path inputs, native ImGui ini save/reload with a
headless context, synthetic RAW opening and cancelled shutdown. They do not
establish macOS filesystem, Retina/native-window docking or actual NEF decoding.
No new macOS validation result is claimed. On the reviewed PR head:

- Launch from different CWDs, including Finder, without preferences/layout. Confirm
  `~/Library/Application Support/RawNode/` receives both files, correct default
  docking appears, and no settings files appear in CWD, installation or photos.
- Change theme, visibility, preview/export choices and dock/window placement;
  close/restart with and without an opened folder. Check restoration immediately
  on the first frame, then switch folders and confirm docking stays unchanged.
- With valid stored non-default colour pairs, open a new Nikon NEF and confirm
  decoding/new grade uses them. Open a saved grade and apply preset Replace;
  confirm photo colours restore while future-photo defaults remain unchanged.
- Exercise absent/unwritable configuration directory and read-only/bad settings
  destinations. Confirm visible warnings, in-memory operation, no alternate writes
  and preservation of an existing valid file on failed preference replacement.
- Keep historical workspace/layout files in multiple folders and CWD; verify
  bytes/mtime remain unchanged through navigation, restart and shutdown.
- Force photo save failure on Quit; Cancel must keep rendering/editing working.
  Retry or explicit Discard may close, saving application preferences. Confirm
  preference-write errors are visible without silently writing elsewhere.

Record macOS/hardware, tested head SHA and Nikon models alongside the results.
