# Codec provenance and decoding relevance

This audit compares the actual libjxl copy in the supplied Adobe SDK 2768 archive
with the pinned upstream libjxl 0.11.2 tree and its pinned submodules. The earlier
file inventory remains in `../logs/vendor/codec-provenance.json`: 771 files were
identical, six common files differed, and the SDK had a generated version header.
No Adobe-specific codec patch is imported by this corrective change.

| Difference in the SDK copy | Relevance to this prototype |
|---|---|
| Root CMake conditionally includes testing/tool directories | Build packaging of the reduced SDK tree; no decoder arithmetic change. |
| `lib/jxl_lists.cmake` adds `extras/gain_map.cc` to encoder sources | Extra encoder/gain-map API packaging. No use of that API was found in the SDK main-image JXL decoder. This does not establish support for all HDR/gain-map cases. |
| `enc_chroma_from_luma.cc` uses `<float.h>` instead of `<cfloat>` | Encoder include portability; no pixel algorithm change in that diff. |
| `third_party/skcms/skcms.cc` changes an inverse-matrix finiteness check | Potentially numerical; inspect separately below. |
| Brotli CMake guards CLI creation | Tool build scope, not Brotli decoding code. |
| Highway CMake guards test/list targets | Tool/test build scope, not SIMD decoding code. |
| SDK supplies `lib/include/jxl/version.h` | Generated upstream by CMake; both versions identify 0.11.2. |

## skcms difference

In `skcms_Matrix3x3_invert`, upstream rejects a double reciprocal determinant
outside `[-FLT_MAX, FLT_MAX]` before checking its float conversion for finiteness.
Adobe's copy converts it to float first and then checks finiteness. Normal finite
matrix values follow the same arithmetic. Do not assume equivalence at overflow,
rounding boundaries or for ill-conditioned/custom profiles; the ordering of a
range check and narrowing conversion can matter.

The examined SDK decoder creates a JXL decoder, preserves bitstream orientation,
requests basic/color/image events and supplies output buffers. It does **not**
call `JxlDecoderSetCms` or select an output profile. In pinned libjxl,
`OutputEncodingInfo::cms_set` starts false and is enabled by `JxlDecoderSetCms`.
The render pipeline's `CmsStage::IsNeeded()` requires that flag. For directly
encoded RGB primaries, `dec_xyb.cc` uses libjxl's own matrix operations for XYB
output; it does not call this skcms inverse routine. Raw ICC bytes can be carried
without parsing through `ColorEncoding::SetICCRaw`.

Therefore the changed skcms check is not on the examined main-image SDK decode
path for the tested linear RGB codestreams, including the synthetic lossy XYB
fixture. skcms is used by the encoder and explicit CMS/ICC paths elsewhere. This
is a source/data-flow conclusion for the current API configuration, not a claim
that skcms never matters to JPEG XL or to the SDK's other functionality.

The synthetic lossy test uses linear sRGB/D65 with XYB enabled and distance 0.01.
Its numerical bound is a compression smoke test, not exact pixel equivalence or
an independent photographic colour reference. The exact independent oracles use
lossless integer codestreams. Earlier Adobe-sample output fingerprints also
matched between the bundled and upstream builds, as recorded in vendor logs.

No representative PureRAW lossy codestream/profile has been supplied. Its
`uses_original_profile`, ICC/direct-primaries encoding, precision/range and any
HDR/gain-map dependencies still require inspection and real-file validation.
There is no demonstrated need to import the bundled codec changes or replace
upstream libjxl; retain the pinned upstream dependency until such a need is shown.

## Local encoder initialization compatibility fix

The full ASan/UBSan run exposed a separate upstream 0.11.2 encoder bug:
`EntropyEncodingData::use_prefix_code` has no default initializer and is read
while a `ModularFrameEncoder` is moved during fixture generation. UBSan reports
an invalid Boolean value from `enc_ans.h:81`. This is an encoder-state issue,
not evidence of a bad DNG decode or a reason to import Adobe's skcms changes.

A single documented local patch initializes that flag to false. The Git version
remains pinned to 0.11.2; `prepare-libjxl-compat.py` accepts only the exact original
or patched header hashes, and `prepare-libjxl.sh` permits only that known tracked
change. CMake rejects a header without the fix. The patch is in
`../patches/libjxl-0.11.2-entropy-initialization.patch`. No upstream copyright or
licence notice is changed. This is justified by the concrete sanitizer trace,
not an automatic import of bundled Adobe modifications. No sanitizer category
is disabled for this fix. Initial failure and final validation logs are retained
under `../logs/correction/`.
