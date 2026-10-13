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
