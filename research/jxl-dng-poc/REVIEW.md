# JPEG XL DNG research: build and review guide

This branch remains an isolated prototype. It does not enable JPEG XL support in
RawNode. No root/production build files or CI workflows reference the dependency.
The earlier review snapshot was commit
`81b537dbc62dabd795e6f427123f948bb5c5fa4b`, based on main
`f87a6f670ce569e6fc760d60305b8bb60dd0a313`. This update adds the redistributable SDK
subset and a reproducible upstream libjxl build; it creates no PR or merge.

## Licensing and source layout

`../../third_party/dng_sdk/` contains the minimal linked SDK source subset, original
Adobe licence, required third-party notices and file hashes. Read its README and
`THIRD_PARTY_NOTICES.md` for the actual redistribution assessment. The Adobe grant
permits source redistribution and modification; the RSA MD5, AOSP Apache-2.0,
IJG and Cornell notices also apply. Their notices are preserved. The entire ZIP
is not treated as having one licence. No sample-photo redistribution permission
is assumed. The unmodified IJG README is included because its source licence
explicitly requires it, rather than as general SDK documentation.

No SDK upstream file was changed. The two XMP-disabled guards are generated into
`build-sdk/dng_jxl_no_xmp.cpp` by `prepare-sdk-compat.py`. The corresponding patch
is in `logs/sdk-2768-no-xmp.patch`. No pixel-decoding change is part of that patch.

Not included: Adobe manuals/specifications, SDK project files, XMP toolkit,
validation utility, prebuilt code, unrelated SDK implementations, any DNG/NEF
fixture, or duplicated LibRaw/libjxl source distributions. The exact omitted
SDK source list is in `third_party/dng_sdk/UPSTREAM.json`.

## Dependencies

Provide a C++17 compiler, CMake 3.16 or later (3.31.6 tested), Git, curl, Python
3.12 or later, and JPEG/zlib development libraries. No SDK ZIP is needed. Python's
extraction-filter API is used by the existing LibRaw preparation script.

| Component | Exact source pin | Licence |
|---|---|---|
| Adobe DNG SDK | Vendored 1.7.1 build 2768; per-file SHA256 manifest | Adobe agreement plus embedded third-party notices |
| LibRaw 0.22.3 | `cf02d66eb5c19d4e46df7aee0582e47a4918509a`; archive SHA256 `7edf96c5b70cff88980a02ab0fc8af125cb6a4a377985573c654439d9d02355c` | LGPL/CDDL; see fetched distribution |
| libjxl 0.11.2 | `332feb17d17311c748445f7ee75c4fb55cc38530` | BSD-3-Clause and file-specific notices |
| Highway 1.2.0 | `457c891775a7397bdb0376bb1031e6e027af1c48` | Apache-2.0/BSD-3-Clause |
| Brotli 1.0.9 | `36533a866ed1ca4b75cf049f4521e4ec5fe24727` | MIT |
| skcms | `b2e692629c1fb19342517d7fb61f1cf83d075492` | BSD-3-Clause |

The three codec subdependency revisions are exactly libjxl v0.11.2's gitlinks.
`prepare-libjxl.sh` fetches those alone; skcms comes from Google's GitHub mirror
at the same commit. Tools/examples/tests/JNI/jpegli and extra applications are
disabled in the standalone CMake target. All fetched dependency trees retain
upstream licence files and are ignored by Git. There is no new package manager.

The SDK's bundled codec copy is labelled 0.11.2 but is not byte-identical to
upstream: six common files differ (four build/source-list files, an encoder
include, and a skcms inverse-matrix check), and the bundle has a generated version
header. This build deliberately uses the published upstream commits above without
copying Adobe's codec edits. `logs/vendor/codec-provenance.json` records the file
comparison. The SDK source version itself remains exactly 2768. Actual fixture
tests compare the new output with the earlier experiment rather than assuming
that equal version labels imply equal pixels in every use case.

## Build without an Adobe download

From the repository root, enter the standalone research directory. Do not run
RawNode's root build to build this prototype:

```bash
cd research/jxl-dng-poc
bash prepare-libraw.sh
bash prepare-libjxl.sh
cmake -S . -B build-sdk -DCMAKE_BUILD_TYPE=Release -DWITH_DNGSDK=ON \
  -DLIBRAW_SOURCE_DIR="$PWD/sources/LibRaw-0.22.3"
cmake --build build-sdk --parallel 2
ctest --test-dir build-sdk --output-on-failure

cmake -S . -B build-nosdk -DCMAKE_BUILD_TYPE=Release -DWITH_DNGSDK=OFF \
  -DLIBRAW_SOURCE_DIR="$PWD/sources/LibRaw-0.22.3"
cmake --build build-nosdk --parallel 2
ctest --test-dir build-nosdk --output-on-failure
```

`DNG_SDK_ROOT` defaults to the checked-in subset. `LIBJXL_SOURCE_DIR` defaults to
`sources/libjxl`. Build/dependency directories are ignored. The memory-only CTest
needs no photos and exercises numeric level/table/WB/crop/orientation oracles.

Linux needs compiler/JPEG/zlib development packages and pthreads. The original
executor uses its pre-existing `/workspace/.rawnode-tools/activate.sh` for JPEG
and CMake; that activation path is not required on other machines. macOS needs
Xcode command-line tools and suitable CMake/JPEG/zlib/Python/Git installations;
Homebrew prefixes may need `CMAKE_PREFIX_PATH`. Windows needs a C++17 toolchain,
matching JPEG/zlib architecture/runtime, CMake and Python; the preparation shell
scripts can be run in Git Bash. Apple Silicon/MSVC compilation, CRT/path handling
and packaged-app tests have not been executed. Platform branches in CMake alone
are not evidence of successful platform builds.

The standalone Apple branch links CoreFoundation and CoreServices, used by the
SDK's timezone/diagnostic code; the Windows branch adds User32 for SDK diagnostics.
These are source-derived link requirements, not executed platform validation.

## Actual JPEG XL input tests

There are no sample photographs in Git. For the recorded test suite, provide
these original files yourself under ignored `fixtures/`:

| Original sample | Fixture name |
|---|---|
| `01_jxl_linear_raw_integer.dng` | `adobe-01-jxl-integer.dng` |
| `02_jxl_linear_raw_float.dng` | `adobe-02-jxl-float.dng` |
| `04_PGTM2_per_profile.dng` | `adobe-04-ordinary.dng` |
| NASA/rawpy Nikon input described in README | `iss030e122639.NEF` |

The first two hashes are in `logs/fixture-sha256.txt`; the NEF hash/source are in
the README. `run-ordinary-checks.py` now takes its ordinary DNG from `fixtures/`,
so no original SDK package directory is required. Alternatively invoke the probe
with any original input path directly. Real-file tests cannot run without inputs.

```bash
python3 run-sdk-checks.py
./build-sdk/dng_probe --compare-stage fixtures/adobe-01-jxl-integer.dng
python3 run-checks.py
python3 run-ordinary-checks.py
```

Runners overwrite their diagnostic logs; retain new output separately from the
recorded evidence when reviewing. Original paths decode into memory, with no
intermediate image or conversion utility. `--keep-float` stops before integer
LibRaw development; it does not create a colour-managed float renderer.

## Review evidence and limits

`src/probe.cpp` and `src/reference.h` keep the original decoding/numerical code.
`README.md` explains the original stage, precision, ordinary-file and geometry
findings. Earlier logs remain under `logs/`; the new vendored-subset validation is
separate under `logs/vendor/`. These results are actual executed checks, not a
claim that every possible SDK configuration builds.

Both unpacked reference paths use the same SDK/libjxl codec. Zero differences
verify the bridge/stage transfer, not an independent codec or Adobe colour render.
The integer example requires OpcodeList2 processing. Float-to-integer development
loses shadow precision. Full profile/exposure interpretation, OpcodeList1-only,
nonzero-black and geometry-changing JXL cases remain open. No PureRAW fixture or
paired compressed/uncompressed PureRAW colour comparison has been executed.

Original archive provenance/omission records remain in `logs/`. The earlier
publication deliberately omitted all SDK source and source-quoting compiler logs;
this update supersedes that source exclusion only after the actual licence check.
The original external standalone workspace/archive remain unchanged.

## Vendored-subset validation result

On the available Linux x86-64 executor, both Release builds and their CTest memory
checks passed from empty build/source directories. LibRaw and the codec Git pins
were fetched afresh. Compile dependency files contain no references to the old
SDK/LibRaw workspace, and no Adobe ZIP was used by the build. This was a clean
source/dependency/build check using the existing GCC 14.2.0/CMake 3.31.6/JPEG/zlib
toolchain, not a newly provisioned OS or container.

All 11 SDK runner cases returned their expected exit status, including the
host-disabled rejection. All six SDK stage-reference comparisons had zero sample
differences. Nine processed/retained float fingerprint comparisons match the
earlier SDK-bundled experiment. A Nikon NEF and conventional Adobe DNG also match
between the new SDK and no-SDK builds, with no host attached. The plain-build
error-control runner passed as well. Actual fixture files were read from outside
the publication set; none are committed.

See `logs/vendor/validation.json`, numerical/reference logs, build logs and CTest
logs. No Mac, Windows, real PureRAW or trusted external colour-render validation
is claimed. RawNode's production application was not rebuilt for this isolated
dependency change.
