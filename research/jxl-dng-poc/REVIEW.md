# Independent review of the JPEG XL DNG experiment

This directory publishes an existing standalone experiment for inspection. It is
not connected to RawNode's build, does not enable JPEG XL support in RawNode, and
does not establish DxO PureRAW compatibility. The original standalone workspace
and its archive were left unchanged.

The branch is based on `main` commit
`f87a6f670ce569e6fc760d60305b8bb60dd0a313`. All publication additions are under
`research/jxl-dng-poc/`. No production files, root build settings or CI workflows
are changed. No PR or merge is part of this publication.

## What to review

- `src/probe.cpp`: direct original-file decode, per-file SDK host lifetime,
  LibRaw processing, float diagnostics, comparison and memory-only tests.
- `src/reference.h`: direct SDK comparison at the corresponding unpacked stage.
- `CMakeLists.txt`: standalone plain and SDK-enabled LibRaw 0.22.3 builds.
- `prepare-libraw.sh`, `prepare-sdk-compat.py`: pinned dependency preparation and
  the XMP-disabled compatibility adjustment.
- `run-*.py`: numerical and ordinary-file regression runners.
- `README.md`: original investigation results, version pins, processing findings,
  limitations and proposed integration boundaries, with publication notes added.
- `logs/`: selected actual build/configuration, numerical, error-control and
  regression evidence. These are historical executed results, not fresh test
  runs performed while publishing this branch.

In particular, challenge whether the stage-reference checks have independent
oracles, required opcode handling is sufficient, float precision is preserved,
white balance and profiles are interpreted correctly, and ordinary decoding is
isolated. Both comparison paths use the same SDK/libjxl implementation. The tests
do not establish complete Adobe rendering equivalence. No PureRAW inputs or
paired compressed/uncompressed PureRAW comparisons have been executed.

## Reproduction without vendoring into RawNode

Copy this directory to a standalone location **outside the RawNode checkout**
before building or preparing any dependencies. The paths in the recorded logs
describe the original Linux executor; they are not prerequisites on a Mac.

Obtain Adobe DNG SDK **1.7.1 build 2768** separately from Adobe or your existing
official archive. Do not substitute an older SDK. No SDK package, source,
headers, libraries, binaries, documentation or sample images are distributed in
this branch. The experiment was built only on Linux x86-64; Apple Silicon and
Windows builds remain unverified.

In the standalone copy, put the extracted package at `sources/dng_sdk_1_7_1` to
match the existing runners, and prepare the following fixtures from your own
package. Both source and destination names are listed; no conversion is involved:

| SDK sample | Standalone fixture |
|---|---|
| `sample_files/01_jxl_linear_raw_integer.dng` | `fixtures/adobe-01-jxl-integer.dng` |
| `sample_files/02_jxl_linear_raw_float.dng` | `fixtures/adobe-02-jxl-float.dng` |

Their SHA256 hashes are in `logs/fixture-sha256.txt`. The ordinary-file runner
also needs the SDK's `sample_files/04_PGTM2_per_profile.dng` at its existing package
path, and the separately obtained `fixtures/iss030e122639.NEF` documented in the
README. No image files are included here.

Provide C++17, CMake, JPEG/zlib development packages and Python. Python 3.12 or
later provides the extraction-filter API used by `prepare-libraw.sh`. On the
original executor, an existing development sysroot supplied JPEG and CMake;
`/workspace/.rawnode-tools/activate.sh` is environment-specific and not included.
An equivalent system toolchain is sufficient; Mac/Windows toolchain details still
require testing.

From that standalone copy, the build commands are:

```bash
bash prepare-libraw.sh
cmake -S . -B build-sdk -DCMAKE_BUILD_TYPE=Release -DWITH_DNGSDK=ON \
  -DLIBRAW_SOURCE_DIR="$PWD/sources/LibRaw-0.22.3" \
  -DDNG_SDK_ROOT="$PWD/sources/dng_sdk_1_7_1"
cmake --build build-sdk --parallel 2
ctest --test-dir build-sdk --output-on-failure
python3 run-sdk-checks.py

cmake -S . -B build-nosdk -DCMAKE_BUILD_TYPE=Release -DWITH_DNGSDK=OFF \
  -DLIBRAW_SOURCE_DIR="$PWD/sources/LibRaw-0.22.3"
cmake --build build-nosdk --parallel 2
ctest --test-dir build-nosdk --output-on-failure
python3 run-checks.py
python3 run-ordinary-checks.py
```

The runners overwrite diagnostic logs in that standalone copy. They decode
original image paths into memory; they do not create intermediate image files.
Keep their new output distinct from this branch's recorded evidence.

## Publication inspection and omissions

The source archive was inspected before publication. Its SHA256 is
`6e8c30dbbd60de47ef5c644bbb922d2161e5cb18399ac45f2a9b47699fc5b6b2`.
Only regular UTF-8 review files are published. No SDK distribution, dependency
source archive, DNG/NEF fixture, generated build, executable or compiled library
is included. Secret-pattern checks found no credentials in the selected files.
Generic executor paths, dependency names/hashes, aggregate sample statistics and
diagnostic process addresses remain as reproducibility evidence.

Two failed compiler logs quoted SDK source and were omitted. The verbose ZIP
file inventory and superseded blocked-build report/logs were omitted as well.
`logs/publication-provenance.json` lists each omitted archive entry and reason.
The ZIP integrity/version/hash findings remain in the report and provenance.

The SDK compatibility patch is published as a **zero-context, additions-only
diff** containing our preprocessor guards, without SDK source code context.
`prepare-sdk-compat.py` remains the original reproduction helper: it identifies
SDK locations using short symbol/text anchors, and generates the adjusted SDK
translation unit locally from the reader's separately obtained package. Neither
that generated translation unit nor its SDK-derived contents are published.

Prototype C++, CMake and all reproduction scripts are byte-identical to the
original archive. Changes made only for publication are the report notices,
this review guide, ignore rules, the context-free patch, omission records and
publication hashes. `logs/original-source-sha256.json` records the archive's
original authored files; `logs/source-sha256.json` records the published authored
files. Neither manifest covers SDK or fixture contents.
