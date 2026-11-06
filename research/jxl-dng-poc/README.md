# JPEG XL DNG decoding research — corrected after independent review

This is a standalone LibRaw 0.22.3 / Adobe DNG SDK 1.7.1 build 2768 experiment.
It is **not production RawNode support**. No production source, root build or
renderer is changed. Floating-point JPEG XL development is unsupported.

The correction supersedes the stage-selection recommendation at reviewed commit
`58bda0b3ff316f191872d1869fe1b421b3c731a1`. Earlier logs under `logs/` and
`logs/vendor/` are historical evidence; current evidence is in `logs/correction/`.
The original external experiment/archive remains unchanged.

## Supported experimental path

1. Identify the original input with LibRaw and positively select the pinned
   JPEG XL DNG decoder. Do not retry arbitrary unsupported/malformed inputs.
2. Reject identified floating-point JXL DNGs with a clear diagnostic before
   integer development. `--keep-float` remains an unpack-only diagnostic.
3. For JXL with OpcodeList2, configure a **new** processing instance with
   `LIBRAW_RAWOPTIONS_DNG_STAGE2 | LIBRAW_RAWOPTIONS_DNG_ALLOWSIZECHANGE` **before
   its `open_file()` call**. Otherwise perform ordinary SDK decompression.
4. Attach the SDK host only to JXL inputs. Preserve LibRaw's default `use_dngsdk`
   mask. Recycle SDK-owned buffers before detaching/destroying the host.
5. Reconcile a known ActiveArea trim and its crop coordinates before development.
   Do not automatically run Stage 3, including when OpcodeList3 is present.

The prototype still uses RawNode-style 16-bit LibRaw development followed by
float RGBA conversion and the existing fixed camera-to-working matrix example.
This correction does not implement photographic DNG rendering, a new float RAW
pipeline, or general profile/exposure/highlight-policy fixes.

`--stage=default` explicitly diagnoses decompression only. The old `--stage=opcode`
and forced `--stage=stage3` remain visibly labelled diagnostic overrides, outside
the supported auto policy. `--compare-stage` now compares auto against decompression
only. These controls must not become production fallbacks.

## Findings reproduced independently

The small fixtures are authored TIFF/DNG metadata around actual JXL-encoded source
ramps. They reproduced these failures before the correction:

| Reviewed behaviour | Reproduced result |
|---|---|
| Conditional `DNG_STAGE23_IFPRESENT_JPGJXL`, unequal channel white levels and OpcodeList2 | Wrong WB: about 4x red and 2x green; a bright red sample clips. Forced Stage 2 avoids the duplicate white-level correction. |
| OpcodeList3 present | Conditional mode applies Stage 3 to JXL; equivalent ordinary DNG follows its existing path. Auto now does not apply Stage 3. |
| ActiveArea 52x40 inside stored 64x48, OpcodeList2 | Unpack fails without the size-change flag. |
| Explicit DefaultCrop 44x34 inside that ActiveArea | Allowing size change alone resets the requested margins/dimensions and returns 52x40. |
| Floating-point JXL with OpcodeList2 through integer processing | Entire processed RGB buffer becomes black. |

The WB cause is in LibRaw `src/utils/open.cpp`: its identification-time correction
recognises forced Stage 2/3, but not the conditional JPEG/JXL flag. SDK Stage 2
already normalizes channel white levels. Stage configuration must precede open.

### Crop handling and limits

SDK Stage 2 can trim the stored image to ActiveArea. LibRaw's size-change branch
resets dimensions/margins but leaves inset crops in stored-image coordinates.
The correction snapshots the active and requested regions, validates the decoded
shape against that known trim, rebases inset crops, and restores the requested
region in **both** LibRaw size snapshots. `raw2image_start()` restores the second
snapshot, so changing only the public live sizes is insufficient.

DefaultCrop remains an explicit `--crop` choice, matching the experiment's prior
contract. Without it, output is the active image. Tests verify 52x40 active output,
44x34 explicit crop, 34x44 rotated crop, source-pixel positions and a 20x12 crop.
Explicit cropping no longer silently skips crops smaller than LibRaw's default
50% size threshold.

A Stage 1 `TrimBounds` plus Stage 2 test also exposed that output dimensions alone
can be insufficient to infer coordinates. This combination is rejected explicitly
before SDK processing. Other unexpected Stage 2 size changes are rejected, rather
than guessed. Complex geometry/opcode combinations and fractional/default/user
crop conventions still need real-file qualification. This is not universal DNG
geometry support.

## Deterministic real-decoder regressions

`tests/jxl_checks.h` generates tiny synthetic JXL DNGs in a unique temporary
folder, opens their original paths and removes them on exit. No photograph is
converted and no generated image is committed. The pinned libjxl encoder produces
both lossless integer and floating-point codestreams; one lossy XYB case is also
included. Tests verify `SDK_PROCESSED`, not just identification.

CTest runs 18 checks covering integer decode, OpcodeList2, unequal whites with
and without it, OpcodeList3 alone and combined, ordinary DNG comparison,
ActiveArea, explicit/small/rotated crop, unsupported geometry, unchanged SDK
eligibility, rejected float inputs, retained float diagnostics and missing host.

The main pixel oracle independently computes the authored source ramp divided by
its per-channel white level, then the declared polynomial `y = 0.5*x` when needed.
Every processed camera sample is checked, including analytically mapped crop and
orientation coordinates. The tolerance is four 16-bit output steps to allow the
SDK/LibRaw integer quantizations. Observed maximum lossless error is about
`1.53e-5` (approximately one step); the restored bug gives `0.633699634`.
Retained float diagnostics are checked against their authored samples and
analytical normalization within `1e-9`. The lossy XYB case has a separate `0.01`
compression smoke-test bound; it is not an exact colour/rendering reference.

`tests/check_stage_mutation.py` restores the old conditional selection in a
**temporary** translation unit, links against the current libraries and requires
the numerical oracle to fail. It leaves checkout/build objects unchanged. This
proves that the regressions detect the reviewed defect; same-SDK reference
comparisons remain supplementary, not the independent oracle.

## Reproduce the standalone build

Requirements: C++17, CMake (3.31.6 tested), Git, curl, Python 3.12+, JPEG/zlib
development packages and OpenMP when enabled. The checked-in SDK subset is under
`../../third_party/dng_sdk/`; no Adobe ZIP or sample photographs are needed for
CTest. Its original licence, third-party notices and per-file hashes remain there.

```bash
cd research/jxl-dng-poc
bash prepare-libraw.sh
bash prepare-libjxl.sh
cmake -S . -B build-sdk -DCMAKE_BUILD_TYPE=Release -DWITH_DNGSDK=ON \
  -DWITH_OPENMP=ON -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DLIBRAW_SOURCE_DIR="$PWD/sources/LibRaw-0.22.3"
cmake --build build-sdk --parallel 2
ctest --test-dir build-sdk --output-on-failure
python3 tests/check_stage_mutation.py build-sdk

cmake -S . -B build-nosdk -DCMAKE_BUILD_TYPE=Release -DWITH_DNGSDK=OFF \
  -DWITH_OPENMP=ON -DLIBRAW_SOURCE_DIR="$PWD/sources/LibRaw-0.22.3"
cmake --build build-nosdk --parallel 2
ctest --test-dir build-nosdk --output-on-failure
```

The mutation helper currently supports GCC/Clang with CMake Makefiles and requires
`compile_commands.json`. It is an explicit review audit, separate from normal CTest.

LibRaw is pinned to 0.22.3 (archive SHA256
`7edf96c5b70cff88980a02ab0fc8af125cb6a4a377985573c654439d9d02355c`). libjxl is
pinned to 0.11.2 commit `332feb17d17311c748445f7ee75c4fb55cc38530`, with its recorded
Highway, Brotli and skcms gitlinks. No external source distribution is vendored.
`prepare-libjxl.sh` also applies the documented, exact-hash encoder initialization
fix below; CMake verifies that header. Adobe's skcms/codec modifications are not
imported. All original dependency notices remain intact.

OpenMP is enabled by default in this standalone build. On platforms without an
OpenMP toolchain, install the corresponding runtime/development package or use
`-DWITH_OPENMP=OFF`. Apple Silicon and Windows are untested. macOS additionally
needs the SDK's CoreFoundation/CoreServices links and suitable library prefixes;
Windows needs matching JPEG/zlib/OpenMP CRT/architecture and User32. Preparation
shell scripts can run in Git Bash. Production dependency/CI configuration is deferred.

## Sanitizers and findings

GCC ASan/UBSan and TSan builds instrument the prototype, SDK and codec dependencies.
Use `-DPOC_SANITIZERS=address,undefined` or `thread` in a separate build directory.
Sanitizer Release runs use `-O1 -g -DNDEBUG` for both C and C++; normal Release
uses its ordinary optimization settings. TSan was tested with OpenMP off;
ASan/UBSan was tested with OpenMP on. This is not TSan validation of libgomp.

The initial full run found an upstream **encoder** uninitialized Boolean copied
while moving `EntropyEncodingData` in libjxl 0.11.2. The local, one-line patch
initializes `use_prefix_code` to false. `prepare-libjxl-compat.py` accepts only the
exact pristine/patched hashes; unrelated source edits are rejected. The patch and
investigation are in `patches/` and `tests/PROVENANCE.md`. No sanitizer category is
disabled, and no Adobe-specific codec modification is substituted.

LeakSanitizer could not inspect worker threads in the sandbox (fatal ptrace/thread
inspection error). The full leak-enabled suite was rerun outside that restriction;
leak detection was not disabled. Both ASan/UBSan tests then passed, as did both
TSan tests. Initial failures and final logs are retained in `logs/correction/`.

Example sanitizer configuration after the same dependency preparation:

```bash
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Release -DWITH_DNGSDK=ON \
  -DWITH_OPENMP=ON -DPOC_SANITIZERS=address,undefined \
  -DCMAKE_C_FLAGS_RELEASE="-O1 -g -DNDEBUG" \
  -DCMAKE_CXX_FLAGS_RELEASE="-O1 -g -DNDEBUG" \
  -DLIBRAW_SOURCE_DIR="$PWD/sources/LibRaw-0.22.3"
cmake --build build-asan --parallel 2
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
  ctest --test-dir build-asan --output-on-failure
```

For TSan select `thread`, `WITH_OPENMP=OFF` and a different build directory. Tests
must run where the sanitizer runtime has the OS permissions it needs; investigate
runtime failures rather than reporting a skipped or suppressed check as passing.

## Ordinary RAW compatibility and performance

Plain and SDK-enabled LibRaw 0.22.3 match for the tested Nikon NEF. Repeated serial
and OpenMP 0.22.3 probes also have matching processed-camera and working-RGBA
fingerprints. `tests/compare_openmp.py SERIAL PARALLEL NEF` records single-host
measurements; these are not general performance guarantees. In three interleaved
runs on the available three-thread executor, median diagnostic total was 1744 ms
serial and 1505 ms with OpenMP. Statistics and hashing are included, so this is
not a pure decoder/development benchmark or a universal speedup estimate.

Claude's separately reported 0.21.2-versus-0.22.3 pixel differences remain an
integration consideration, not a version comparison rerun by this correction.
Per-file SDK dispatch cannot guarantee identical pixels after a LibRaw upgrade.
Do not link two globally visible LibRaw implementations into RawNode. This
experiment uses separate executables for comparisons; production dependency
selection remains a separate design/review task.

## Codec provenance and remaining work

See `tests/PROVENANCE.md` for all six actual SDK-bundled/upstream codec differences
and the examined decoder data flow. The changed skcms inverse check is outside
the standard RGB SDK decode path examined here (`cms_set` remains false). The
synthetic lossy linear-RGB case passes. Arbitrary ICC/PureRAW codestreams still
need actual fixtures; equal version numbers do not prove all-path equivalence.

No representative PureRAW file or paired compressed/uncompressed PureRAW colour
comparison has been executed. DRC LinearizationTable/black levels, as-shot WB and
exposure, full camera profiles, highlight policy, complex opcodes/geometry and
platform packaging remain open. OpcodeList3 is intentionally not automatically
executed, so this is not a complete DNG rendering implementation. Floating-point
JPEG XL **development is unsupported**; `--keep-float` only exposes raw diagnostics.

The corrected foundation is ready for another independent Claude review. It is
not ready for production integration or a PureRAW compatibility claim.
