# LibRaw 0.22.3 + Adobe DNG SDK 2768: standalone JPEG XL proof of concept

**Result: the original SDK-enabled experiment built on Linux x86-64 and decoded
both Adobe JPEG XL DNG examples directly into memory.** This branch now also
contains the redistributable SDK source subset and fetches pinned upstream JPEG
XL dependencies, so the build no longer requires the Adobe ZIP. Start with
[REVIEW.md](REVIEW.md) for current reproduction and validation details.

This remains an isolated experiment. RawNode production source, root CMake and
CI/dependency setup are unchanged. No production integration PR, conversion
utility, intermediate DNG/TIFF, image exporter or fixture publication is included.

## Source provenance, licensing and build

The uploaded four-part official SDK archive was concatenated alphabetically and
passed `unzip -t`. Its 80,509,918 bytes have SHA256
`c1d3a545ed45b150e59af2e6aa0695c1551f0e2bed34ad87436898a05194d2b1`.
Version macros identify Adobe DNG SDK 1.7.1 build 2768. Provenance is based on the
user-supplied official archive and file/hash checks, not an independently signed
Adobe checksum. The original 3,769 extracted package files remain untouched.

`../../third_party/dng_sdk/` contains 145 unmodified source/header files selected
for the prototype plus the full Adobe licence and required third-party notices.
Its README explains the actual licence grant and restrictions; `UPSTREAM.json`
records hashes and omissions. Nothing from the XMP toolkit, photographs, SDK
manuals/specifications, upstream project files or prebuilt binaries is vendored.

| Dependency | Reproducible source |
|---|---|
| Adobe DNG SDK | Checked-in 1.7.1 build 2768 subset |
| LibRaw | 0.22.3 archive and SHA256 pinned by `prepare-libraw.sh` |
| libjxl | 0.11.2, Git 332feb17d17311c748445f7ee75c4fb55cc38530 |
| Highway | 1.2.0, Git 457c891775a7397bdb0376bb1031e6e027af1c48 |
| Brotli | 1.0.9, Git 36533a866ed1ca4b75cf049f4521e4ec5fe24727 |
| skcms | Git b2e692629c1fb19342517d7fb61f1cf83d075492 |
| System libraries | JPEG development library and zlib; installed toolchain supplies these |

`prepare-libjxl.sh` fetches only the three submodules needed from libjxl's own
recorded Git revisions. It uses Google's GitHub mirror for the same skcms commit.
External source trees remain ignored, rather than being copied into Git history.
The original executor used GCC 14.2.0, CMake 3.31.6, JPEG ABI 62 and zlib 1.3.1.

From this directory, with those development tools installed:

```bash
bash prepare-libraw.sh
bash prepare-libjxl.sh
cmake -S . -B build-sdk -DCMAKE_BUILD_TYPE=Release -DWITH_DNGSDK=ON \
  -DLIBRAW_SOURCE_DIR="$PWD/sources/LibRaw-0.22.3"
cmake --build build-sdk --parallel 2
ctest --test-dir build-sdk --output-on-failure
```

CMake finds the checked-in SDK subset by default. `DNG_SDK_ROOT` can select another
subset root containing `source/dng_host.h`; `LIBJXL_SOURCE_DIR` selects a separately
prepared JXL source tree. Plain LibRaw remains selectable with `WITH_DNGSDK=OFF`.
Neither the prototype nor this dependency is referenced by RawNode's root build.
No new dependency manager or automatic build-time download is introduced.

The SDK flags remain `qLinux=1` (or the platform branch), `qDNGThreadSafe=1`,
`qDNGUseLibJPEG=1`, `qDNGUseXMP=0`, `qDNGXMPFiles=0`, `qDNGXMPDocOps=0`, and
`qDNGValidateTarget=0`. LibRaw uses `USE_DNGSDK`, `USE_JPEG`, `USE_ZLIB` and
`LIBRAW_NODLL`. OpenMP is omitted. JXL encoder and decoder library code remains
linked because the SDK JXL translation unit references both APIs.

No vendored upstream file is modified. `prepare-sdk-compat.py` generates a local
build-directory copy of `dng_jxl.cpp` with two XMP-disabled guards; the patch is
recorded in `logs/sdk-2768-no-xmp.patch`. It changes XMP access, not pixel decoding.
XMP-disabled profile/metadata handling still needs real PureRAW qualification.

A `dng_host` is attached through `set_dng_host(&host)` only for positively
classified JXL DNGs. The host outlives all borrowed buffers; the probe recycles
LibRaw and detaches the host before destruction. Stage flags are set before
opening the processing instance because identification adjusts WB metadata.

| Probe option | LibRaw option/effect |
|---|---|
| `--stage=default` | Read compressed samples; control that skips the integer example's required opcodes |
| `--stage=opcode` | `LIBRAW_RAWOPTIONS_DNG_STAGE23_IFPRESENT_JPGJXL` |
| `--stage=stage2` / `stage3` | Force SDK Stage 2 / Stage 2 and Stage 3 for diagnosis |
| `--keep-float` | Clear `CONVERTFLOAT_TO_INT`, retain unpacked float samples, stop before integer development |
| `--allow-size-change` | Explicit `DNG_ALLOWSIZECHANGE`, not automatic recovery |

The following original numerical findings are retained as historical evidence;
current vendored-source validation is recorded separately under `logs/vendor/`.

## Executed JPEG XL decoding

The previously downloaded examples match byte-for-byte the copies inside the
uploaded official package. The executable reads original files directly; it does
not use embedded previews as the image. SDK-disabled controls fail at `unpack()`
with error -2, while the SDK-enabled path records `SDK_PROCESSED`.

| Example | Main image | Tested unpacked representation | Current RawNode-style processed output |
|---|---|---|---|
| 01 integer | 9504 x 6336, 3 channels | uint16 RGB | 16-bit bitmap -> float RGBA |
| 02 floating point | 4667 x 7000, 3 channels | float32 RGB when retained | Default path converts to integer, then 16-bit bitmap -> float RGBA |

Both main IFDs use Compression=52546 and PhotometricInterpretation=34892
(LinearRaw). Example 01 declares BitsPerSample=16, SampleFormat=1 (unsigned
integer); example 02 declares BitsPerSample=16, SampleFormat=3 (floating point).
The latter is expanded to float32 in memory by the SDK.

All reported processed/retained sample statistics are finite and nonconstant.
This verifies decoded image data, not a visually judged match to Camera Raw or
PureRAW. Reference logs also record the DNG IFD compression/sample-format fields.

Commands:

```bash
./build-sdk/dng_probe fixtures/adobe-01-jxl-integer.dng
./build-sdk/dng_probe --stage=opcode fixtures/adobe-01-jxl-integer.dng
./build-sdk/dng_probe --reference-unpack fixtures/adobe-01-jxl-integer.dng
./build-sdk/dng_probe --stage=opcode --reference-unpack fixtures/adobe-01-jxl-integer.dng
./build-sdk/dng_probe --keep-float --reference-unpack fixtures/adobe-02-jxl-float.dng
./build-sdk/dng_probe --keep-float --stage=stage2 --reference-unpack fixtures/adobe-02-jxl-float.dng
./build-sdk/dng_probe --compare-stage fixtures/adobe-01-jxl-integer.dng
```

The program returns bottom-up float RGBA using RawNode's LibRaw settings and a
fixed standard Rec.709-to-Rec.2020 matrix corresponding to its default linear
working gamut. It does not depend on or modify RawNode's colour registry. It is
not a bit-exact test of every RawNode working encoding.

## Numerical results and processing requirements

### Bridge comparisons

A separate direct SDK read of the SAME original file is compared against the
LibRaw unpacked buffer before white balance and gamut conversion. The reference
uses the corresponding SDK stage, not a second RAW framework. All these comparisons
have **zero differing samples, maximum absolute error 0, and RMSE 0**:

| Comparison | Samples |
|---|---:|
| Integer default, Stage 1 | 180,652,032 |
| Integer conditional opcode processing, Stage 2 | 180,652,032 |
| Integer forced Stage 2 | 180,652,032 |
| Integer forced Stage 3 | 180,652,032 |
| Retained float, Stage 1 | 98,007,000 |
| Retained float, Stage 2 | 98,007,000 |

This checks tile/sample transfer and stage interpretation in the integration.
It is NOT an independent validation of libjxl itself: both paths use the same SDK
and JPEG XL implementation. It also does not validate the later matrix-only
colour rendering against Adobe's complete photographic renderer.

### Successful decompression alone gives incorrect development

The integer example has three required `MapPolynomial` operations (ID 8, flags 0)
in OpcodeList2. Default direct decompression skips them. With RawNode-style white
balance, 54,763,421 of 60,217,344 red samples then hit 1.0. Enabling
`LIBRAW_RAWOPTIONS_DNG_STAGE23_IFPRESENT_JPGJXL` applies Stage 2 and changes red
mean from 0.982863117 to 0.098041634; no red samples hit 1.0 in that processed run.

The same-file float-RGBA comparison, default versus conditional-stage processing,
produced RGB MAE **0.634878823**, RMSE **0.669640715**, maximum absolute difference
**0.910855580**. The compressed bytes are identical: these differences arise from
processing, not lossy compression error. See `logs/integer-stage-comparison.log`.

Conditional opcode, forced Stage 2 and forced Stage 3 produce matching processed
pixel fingerprints for this example. It has no OpcodeList3 and no CFA pattern,
so that does NOT justify forcing Stage 3 for arbitrary inputs.

### Float precision and exposure

The float example's retained Stage 1 red samples range from 0.570950627 to
2352.62549 in its stored sample domain. Its white level is 32768. Stage 2 red
values range from 0.0000174240304 to 0.0717964321 after normalization, with an exact
match to direct SDK Stage 2 output.

The default `CONVERTFLOAT_TO_INT` route loses precision: after the current
processing path, 2,929,354 red samples are zero in this example. `--keep-float`
therefore stops after unpack/diagnostic comparison rather than feeding a float
buffer into the integer `dcraw_process()` pipeline. A float diagnostic buffer is
not automatically a white-balanced working-colour image.

BaselineExposure metadata (0.35 for integer; 3.32 for float) is logged. Its intended
exposure treatment and complete DNG profile interpretation remain unresolved for
RawNode. The bridge does not invoke the SDK's colour renderer. The production
`rgb_cam` approach must not be mistaken for complete DCP/ForwardMatrix/look-table
or illuminant-dependent rendering.

### Existing synthetic checks retained

Both builds pass the in-memory ordinary linear-DNG suite, with no SDK host:

- LinearizationTable applied once: raw sample 2080 becomes 4160.
- Black/white normalization agrees with independently calculated values within 1e-4.
- Default WB clips blue to 1.0; highlight mode 1 retains information equivalent to
  approximately 2.031708 after restoring the known normalization factor.
- Matrix-created negatives and highlights (-0.633717 and 1.631312) survive float conversion.
- Orientation 6 changes 64x48 to 48x64; explicit default inset crop yields 60x44.
- Default processing records that crop but leaves output at 64x48.
- Malformed input is rejected without a generic SDK retry.

The table/black-level checks above are ordinary uncompressed memory fixtures,
not proof of every PureRAW JPEG XL table/black-level combination. Real PureRAW
fixtures are still needed. No general RAW-development fix was implemented.

### Responsibility boundaries

Default SDK hand-off reads compressed samples; the bridge applies LibRaw's parsed
curve for relevant short-integer data. SDK Stage 2 handles linearization,
normalization and its opcode processing; the bridge then resets LibRaw black/max
metadata to avoid repeating those stages. Stage 3 handles remaining corrections
and CFA interpolation when applicable. Existing LibRaw processing still owns WB,
its integer scaling, the current camera matrix path and processed orientation.

Stage options affect identification-time WB metadata adjustment too. The probe
classifies first, reopens the original path with the requested stage options set,
then attaches a host only for JXL. Unexpected classification changes are rejected.
`DNG_ALLOWSIZECHANGE` and inset crop are explicit opt-ins, not automatic recovery.
OpcodeList1-only, nonzero-black and dimension-changing JXL cases remain untested.

## Existing RAW compatibility

Tested plain versus SDK-enabled **LibRaw 0.22.3**, with host attachment disabled by
classification for ordinary files:

| Input | Dimensions | Observed result |
|---|---|---|
| Nikon NEF `iss030e122639.NEF` | 4284x2844 | Successful in both; matching processed-camera and float-RGBA FNV64 fingerprints |
| Adobe conventional DNG `04_PGTM2_per_profile.dng` | 1000x1000 | Same result; no SDK host attached |
| Synthetic ordinary DNG variants | 64x48, rotated/cropped variants | Numeric oracles pass in both builds |

See `logs/ordinary-compatibility.json`. Fingerprints are useful regression evidence,
not a mathematical proof for all files. These tests do not establish equivalence
with the pre-existing system LibRaw 0.21.4, nor cover all NEFs/DNGs or TIFF raster
loading. RawNode's ordinary TIFF loader was not changed or replaced.

NEF source:
https://raw.githubusercontent.com/letmaik/rawpy/2705f5b223c162c3cbc48474fe1a01c3cf09fa4f/test/iss030e122639.NEF

NEF SHA256:
`5922721d13f11795557d97fdeb0a60b900086c402bc82a848ff280d15b99ffd4`.
Fixtures are not included in the source/report bundle; retain their attribution
and redistribution terms separately if adding any to a public repository.

## Timing, memory and binary size

Single diagnostic runs on this Linux executor; not controlled benchmarks. Default
runs include WB, conversion, statistics and pixel fingerprints. Reference runs
also decode through the SDK separately. Peak RSS is per-process lifetime and
includes extra diagnostic copies, not a prediction of RawNode's final memory use.

| Mode | SDK/LibRaw unpack | Process peak RSS |
|---|---:|---:|
| Integer default | 1.507 s | 2.713 GiB |
| Float default, integer conversion | 1.055 s | 1.483 GiB |
| Integer conditional opcode + reference | 2.802 s | 2.708 GiB |
| Retained float + reference | 1.021 s | 0.755 GiB |
| Float Stage 2 + reference | 1.324 s | 1.115 GiB |

The same-file two-mode float comparison peaks around 3.61 GiB because both output
buffers are retained. Do not interpret differences between diagnostic modes as
production performance improvements without controlled repeated measurements.

Release executables on this build: plain **1,373,264 bytes**; SDK-enabled
**8,826,072 bytes**, approximately 7.1 MiB additional. SDK/libjxl dependencies are
static; `logs/sdk-linkage.txt` lists the remaining zlib, JPEG and platform runtimes.

## Minimal RawNode integration and recommendation

The backend is technically viable. Keep the existing float RGBA `Image`,
working-encoding/lifecycle contracts, graph, renderer, preview and export paths.
A future change belongs at `src/imgio/ImageLoad.cpp`: positive JXL main-image
classification, adapter/host lifetime, deliberately selected processing flags,
error reporting and conversion into the existing buffer. Do not route embedded
JXL previews as the main image or retry arbitrary malformed files.

The PoC uses the pinned 0.22.3 decoder diagnostic name. That should not become an
unversioned permanent format API. Prefer a public metadata check for the selected
main DNG IFD's Compression=52546, using the existing TIFF infrastructure or SDK
metadata parser. Keep detection separate from pixel processing.

The smallest single-library configuration is SDK-enabled LibRaw with per-file
host attachment and an ordinary system-LibRaw build option. If strict runtime
retention of a separate system LibRaw is required, isolate the SDK-enabled copy
behind a narrow module/C interface and private symbols; do not link two globally
visible LibRaw C++ implementations together. This is a proposed boundary, not a
new subsystem implemented in RawNode.

**Recommendation: continue with LibRaw + SDK; no evidence justifies changing RAW
frameworks. The direct-decoding feasibility gate is met. Hold a production
PureRAW-support PR until actual PureRAW validation resolves the remaining colour,
level/opcode, precision and geometry contracts.** No PR has been created.

Possible later review stages, after obtaining those samples:

1. Reproducible optional SDK-enabled backend build and licence packaging, with
   ordinary system LibRaw still selectable.
2. Narrow main-image JXL dispatch and host lifetime adapter, with explicit errors
   and regression fixtures. Keep it gated until the pixel contract is validated.
3. Separately reviewed opcode/level/WB/profile, precision and geometry decisions,
   supported by paired PureRAW and trusted-render comparisons.
4. macOS Apple Silicon and Windows CI/package tests, followed by enabling support.

## Licensing, security and platform limitations

The actual 2768 `LICENSE.txt` was inspected. It grants royalty-free software use,
modification and distribution subject to notice/other terms, includes a commercial
indemnity clause, and limits modification of documentation. The DNG patent licence
is separate. Preserve SDK notices and the applicable LibRaw LGPL/CDDL, libjxl,
Highway, Brotli, skcms, JPEG and zlib terms when packaging. An XMP-disabled source
patch does not remove licensing obligations. Record package hashes and track
security updates; bundled dependency versions are reproducible, not a claim that
all are the newest security releases.

Only Linux x86-64 was built and executed. macOS Apple Silicon and Windows require
real toolchain, architecture, CRT/path, dependency and packaged-app tests. The
CMake platform branches are not evidence of successful builds there. XMP metadata
behaviour and camera-profile requirements must be checked before selecting this
minimal build for production.

## Remaining tests requiring real DxO PureRAW files

No PureRAW samples or matching ordinary/high-fidelity pairs have been supplied.
Required next: actual DRC LinearizationTable handling with appropriate black/white
levels; warm/daylight colour and profile behaviour; highlight/shadow retention;
required/optional opcodes and size changes; as-shot WB/exposure conventions;
orientation/default/user crop; comparison with a trusted neutral render and
matching ordinary outputs. Separate expected lossy compression differences from
interpretation differences. Test native RAW/DNG regressions on the supported
platforms before enabling production compatibility.

Useful evidence: `logs/sdk-checks.json`, `logs/integer-stage-comparison.log`,
`logs/ordinary-compatibility.json`, the individual reference logs, final reference
logs, both CTest logs, successful build logs, SDK patch and provenance manifest.
Superseded reports and source-quoting compiler logs are omitted; see `REVIEW.md`.
