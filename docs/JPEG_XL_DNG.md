# Integer JPEG XL DNG decoding

Every configuration builds one pinned LibRaw **0.22.3** static library, `RawNodeRaw`.
No system LibRaw is linked or supported. Upgrading from an older LibRaw can change
ordinary RAW pixels; reproducing historical output is not a requirement.
See [D057](DECISIONS.md#d057--one-pinned-libraw-with-optional-jpeg-xl-dng-support).

`RAWNODE_ENABLE_JXL_DNG` defaults to **OFF**, including normal release builds.
It adds Adobe DNG SDK/JPEG XL support to that same LibRaw target when ON; it does
not select a LibRaw version. OFF needs JPEG/zlib development libraries but neither
the SDK, libjxl nor Python. OpenMP is enabled when CMake finds it; otherwise LibRaw
runs serially. Never link a second
LibRaw into the host to implement dispatch.

## Build

Use the normal dependencies listed in the root README, a C++17 compiler, Git and
CMake 3.16+. ON additionally requires Python 3 for the source compatibility
adjustments; no original Adobe ZIP is required. Use a separate build directory:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DRAWNODE_ENABLE_JXL_DNG=ON
cmake --build build --parallel 2
./build/OfxRawHost --selftest
```

On macOS the executable is also copied into `build/OfxRawHost.app/Contents/MacOS/`.
A fresh OFF build uses the same commands with `-DRAWNODE_ENABLE_JXL_DNG=OFF`.
Use separate build directories when comparing configurations.

### macOS Apple Silicon

Install Xcode command-line tools and the Homebrew dependencies, then pass the
keg-only JPEG/libomp locations explicitly:

```sh
brew install cmake pkg-config jpeg-turbo libtiff little-cms2 openexr python libomp
cmake -S . -B build-jxl-on -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_PREFIX_PATH="$(brew --prefix jpeg-turbo)" \
  -DRAWNODE_ENABLE_JXL_DNG=ON \
  -DOpenMP_CXX_FLAGS="-Xpreprocessor -fopenmp" \
  -DOpenMP_CXX_INCLUDE_DIR="$(brew --prefix libomp)/include" \
  -DOpenMP_CXX_LIB_NAMES=omp \
  -DOpenMP_omp_LIBRARY="$(brew --prefix libomp)/lib/libomp.dylib"
cmake --build build-jxl-on --parallel 2
build-jxl-on/OfxRawHost.app/Contents/MacOS/OfxRawHost --selftest
```

When OpenMP is found on Apple platforms, `LIBRAW_FORCE_OPENMP` prevents LibRaw's
`_REENTRANT` guard from silently disabling it. The definition propagates to users
of LibRaw's public inline allocator. The macOS CI checks actual compile flags,
OpenMP references in the compiled archive, executable linkage and architecture;
installing libomp alone does not establish that OpenMP is active.

### Dependency provenance

`cmake/RawDependencies.cmake` pins:

| Dependency | Source / exact revision |
| --- | --- |
| LibRaw 0.22.3 | `LibRaw/LibRaw` tag archive; SHA256 `7edf96c5b70cff88980a02ab0fc8af125cb6a4a377985573c654439d9d02355c` |
| Adobe DNG SDK 1.7.1 build 2768 | vendored `third_party/dng_sdk`; original official archive SHA256 `c1d3a545ed45b150e59af2e6aa0695c1551f0e2bed34ad87436898a05194d2b1`; per-file manifest `UPSTREAM.json` |
| libjxl 0.11.2 | `libjxl/libjxl` commit `332feb17d17311c748445f7ee75c4fb55cc38530` |
| Highway | `google/highway` commit `457c891775a7397bdb0376bb1031e6e027af1c48` |
| Brotli 1.0.9 | `google/brotli` commit `36533a866ed1ca4b75cf049f4521e4ec5fe24727` |
| skcms | Google GitHub mirror `google/skcms` commit `b2e692629c1fb19342517d7fb61f1cf83d075492` |

The latter three are libjxl's exact gitlinks. No downloaded dependency tree or
photograph is checked in. Standard FetchContent source-directory overrides can
reuse verified local dependencies for offline builds.

`cmake/patches/prepare_dng_sdk.py` verifies the original `dng_jxl.cpp` checksum and
writes a build-directory copy with two `qDNGUseXMP` guards. SDK originals remain
unchanged. XMP implementation/toolkit is not compiled. The SDK platform/thread/
JPEG/XMP definitions propagate consistently into LibRaw and the host.

`prepare_libjxl.py` verifies and applies the reviewed one-line initialization of
`EntropyEncodingData::use_prefix_code`. libjxl's encoder moves this member before
assignment; UBSan found an invalid bool read while encoding synthetic fixtures.
This is a concrete initialization fix, not a sanitizer suppression or decoder
change. Patch files document both adjustments; unknown source hashes fail configuration.

## Decoding policy and limitations

`src/imgio/ImageLoad.cpp` positively identifies the selected main image using
LibRaw 0.22.3's `jxl_dng_load_raw_placeholder()` decoder name plus DNG identity.
Filename extensions, preview compression and generic decode failures do not
trigger Adobe dispatch.

Ordinary RAW inputs are opened once and get no SDK host. Integer JPEG XL inputs
without OpcodeList2 use SDK decompression without forcing extra stages. With
OpcodeList2, recycle/reopen with `DNG_STAGE2 | DNG_ALLOWSIZECHANGE` set **before**
identification: LibRaw's identification-time white-balance adjustment must see
forced Stage 2 when channel white levels differ. Default `use_dngsdk` eligibility
is retained. Stage 3 is never requested; OpcodeList3 is not automatically applied.
Require SDK_PROCESSED and the expected Stage 2/3 warning state after unpacking.

The host is constructed only for positively classified JPEG XL input and outlives
LibRaw's borrowed buffers. Forced Stage 2 rejects Stage 1 TrimBounds and malformed
opcode lists. Supported geometry preserves the identified ActiveArea dimensions,
with zero margins after the SDK's ActiveArea trim. Other changes, including
unsupported Stage 2 TrimBounds, fail explicitly: unmapped bounds changes can
produce incorrectly positioned pixels. DefaultCrop remains unapplied, and its
unused inset-crop fields are not rebased.

Floating-point JPEG XL development is unsupported and rejected before unpacking.
The integer development path must not silently quantize float input or produce
black pixels.

### Colour and precision boundary

SDK Stage 2 handles linearization, black/white normalization and pre-demosaic
opcodes for those inputs. It does not replace LibRaw's subsequent development.
The existing `dcraw_process` settings, camera WB, 16-bit camera-channel buffer,
camera-to-working matrix, selected transfer function, bottom-up float RGBA,
thumbnails, preview/export paths and renderer remain in place. No new renderer,
profile engine, RAW controls, crop mode or intermediate image files are added.
Matrix-generated negative/above-one values remain float as before; pre-matrix
integer development still has its existing quantization/clipping limitations.
General photographic DNG profile rendering, look tables and highlight recovery
are not solved by this integration.

Unsupported cases print an identifiable `JPEG XL DNG:` diagnostic and return the
loader's existing failure result. OFF builds reject identified JPEG XL input.

## Validation

Completed executable validation includes the local fixture/sanitizer checks
below and [Linux CI](https://github.com/ryancara/RawNode/actions/runs/42535180397),
[macOS CI](https://github.com/ryancara/RawNode/actions/runs/42535180372) and the
[manual Release run](https://github.com/ryancara/RawNode/actions/runs/42721710437):

| Configuration | Completed checks |
| --- | --- |
| Linux x86_64 Release OFF/ON | Full builds/self-tests and one-LibRaw linkage audit; local NEF/Adobe DNG checks |
| Linux Clang ASan/UBSan, JPEG XL ON, OpenMP ON | Local/CI full suites with leak detection; host/OFX instrumentation and four deliberate-failure probes |
| macOS arm64 Release OFF/ON | Full builds/self-tests, JPEG XL numerical regressions, architecture and actual LibRaw OpenMP/linkage checks |
| macOS arm64 and Intel release builds, JPEG XL OFF | DMG creation, bundled libomp/licence, app-relative load path, architecture, nested/app signatures and mounted-DMG self-tests |

### Numerical regressions

`tests/RawImageTests.cpp` is part of the existing `--selftest` executable. It
writes temporary, authored DNG wrappers and uses the pinned libjxl encoder to
create actual JPEG XL payloads when enabled. Files are removed on exit. JPEG XL
fixtures declare DNGBackwardVersion 1.7.0.0. These are synthetic tests, not
conversions of photographs.

Distinct channel/spatial ramps, known per-channel white levels, an independently
specified XYZ-to-Rec.2020 DNG matrix and polynomial `y = .5*x` supply analytical
expectations for every output pixel. Tests exercise ordinary DNG, forced Stage 2,
unequal whites, absent OpcodeList2, ignored OpcodeList3, ActiveArea, orientation,
working-space conversion, SDK/no-host dispatch, float rejection and TrimBounds
rejection through the public production loader. Saturated linear Rec.2100 XYB
fixtures exercise lossy colour conversion. DefaultCrop is deliberately different
from ActiveArea to detect accidental cropping. No historical LibRaw pixel baseline
is part of acceptance.

The 19 cases comprise three ordinary DNG checks, eleven successful JPEG XL checks
and five expected failures: missing host, two float inputs, Stage 1 TrimBounds and
Stage 2 TrimBounds.

Observed maximum absolute channel errors against the independent oracle:

| Fixture | Linux x86_64 | macOS arm64 | Assertion bound |
| --- | ---: | ---: | ---: |
| Ordinary/lossless, across both working gamuts | 0.000055669 | 0.000055669 | 0.0001 |
| Unequal whites plus OpcodeList2 | 0.000026922 | 0.000026922 | 0.0001 |
| Lossy linear Rec.2100, output Rec.2020 | 0.000345440 | 0.000401966 | 0.0005 |
| Same lossy fixture, output Rec.709 | 0.000572794 | 0.000662977 | 0.0008 |

The Linux measurements use GCC 14.2/Clang 19.1.7; the arm64 measurements use
AppleClang. Bounds allow 16-bit quantization, published matrix rounding and
compiler/SIMD variation for this distance-.01 fixture. They are not a general
error guarantee for lossy photographs. White reference pixels stay inside
ActiveArea to avoid LibRaw's automatic maximum adjustment in the oracle.
Upstream libjxl remains in use without Adobe-specific skcms changes; this test
does not establish behaviour for every PureRAW ICC profile or JPEG XL encoding.

Independent mutation checks detect the incorrect conditional Stage 2/3 policy
(approximately 0.5 error in the unequal-white oracle) and removal of the Stage 2
geometry rejection (the unsupported image loads, failing its rejection assertion).
These checks supplement the all-pixel expectations rather than comparing only
two paths through the same SDK.

### Real-file checks

The available local NEF decodes to 4284x2844 with RGB range approximately
[-0.390947, 1.21029]; the ordinary Adobe DNG to 1000x1000. Loading and thumbnails
succeed. Serial and OpenMP LibRaw 0.22.3 builds from identical sources give matching
full-buffer fingerprints for these NEF/DNG samples; no historical LibRaw pixel
baseline is required.

An ON build also decodes Adobe's real integer JPEG XL DNG to **9504x6336**, with
finite RGB values and a successful thumbnail. Adobe's real float JPEG XL DNG is
explicitly rejected, as is the integer sample in an OFF build. These sample
files remain outside Git and are not PureRAW fixtures.

Private optional fixture paths can be supplied as `RAWNODE_TEST_NEF`,
`RAWNODE_TEST_DNG` and `RAWNODE_TEST_JXL`. These run the same public loader and
thumbnail path, checking dimensions, finite pixels and numerical sanity. Do not
check photographs into the repository. Full self-tests also exercise the existing
raster loading, colour transforms, preview/export, graph and persistence paths.

### Sanitizer checks

Clang 19.1.7 **ASan/UBSan + leak detection** (Debug, JPEG XL ON, OpenMP ON)
passes the complete self-test suite, the available NEF/ordinary DNG and the synthetic
JPEG XL fixtures. The existing narrowly scoped `stbiw__jpg_writeBits` shift-base
suppression is unchanged; no suppression is added for LibRaw, SDK or libjxl.
Instrumentation of both the host and OFX module was checked, and all four
independent deliberate-failure probes (use-after-free, signed overflow, negative
shift and leak) produced the expected symbolized diagnostics.

Clang 19.1.7 TSan checked serial LibRaw and host concurrency without suppressions.
This is not OpenMP TSan coverage. Sanitizer executables had the process-inspection
access required by their runtimes; leak detection was not disabled.

Reproduce ASan/UBSan by configuring a fresh Clang build with
`-DCMAKE_BUILD_TYPE=Debug -DRAWNODE_ENABLE_JXL_DNG=ON` and setting both
`CMAKE_C_FLAGS` and `CMAKE_CXX_FLAGS` to
`-fsanitize=address,undefined -fno-omit-frame-pointer -fno-optimize-sibling-calls`.
Run `--selftest` with `ASAN_OPTIONS=detect_leaks=1:halt_on_error=1` and
`UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1:suppressions=$PWD/.github/sanitizers/ubsan.supp`.
Use `-fsanitize=thread` in a separate build with
`-DCMAKE_DISABLE_FIND_PACKAGE_OpenMP=TRUE` for serial TSan; never combine ASan and
TSan runtimes. Linux CI records the complete ASan command, instrumentation checks
and independent failure probes.

## Release configuration and remaining validation

Normal release builds explicitly keep JPEG XL **OFF**. The workflow tests arm64
on `macos-15` and Intel on `macos-15-intel`; branch dispatches use app version
0.0.0 and tag builds retain their tagged version. It bundles libomp and its
licence, rewrites the executable load command to
`@executable_path/../Frameworks/libomp.dylib`, signs nested code before the parent
and verifies signatures before packaging and inside each mounted DMG. Both
architectures have completed these checks and packaged self-tests successfully;
no public release is needed for this branch validation.

This validates the packages on runners with Homebrew dependencies available.
Other existing Homebrew runtime libraries are not newly bundled. Dependency-free
DMG distribution, relocation to a Mac without Homebrew, Developer ID/notarization
and minimum-OS compatibility (including Homebrew libomp's deployment target)
remain unverified. Current signatures are ad-hoc.

Before default enablement or distributing JPEG XL-enabled binaries, retain these
separate validation requirements:

- Compare paired PureRAW High-Fidelity JPEG XL and ordinary DNGs from the same RAW
  with a trusted external render. Verify WB/as-shot metadata, matrices/profiles,
  black/white/exposure, highlight/shadow retention, dimensions/orientation and
  OpcodeList1/2/3 behaviour. Separate compression loss from decoding error.
  Synthetic/Adobe tests do not establish this photographic reference comparison.
- Execute Windows builds/runtime/packaging and macOS sanitizer checks. Windows
  currently has only configuration support: C++17, CMake/Git/Python, normal
  JPEG/zlib/TIFF/lcms2/OpenEXR/Imath/OpenGL dependencies, SDK `qWinOS=1`,
  `LIBRAW_NODLL` and User32 linkage. Intel JPEG XL ON is also not covered by the
  release checks, which use OFF.
- Complete portable packaging and the binary licensing/redistribution review
  below. Passing hosted-runner DMG tests does not complete these requirements.

## Licensing and security

Adobe software source redistribution is permitted by the actual SDK agreement,
with notices preserved; separately licensed Apache/RSA/IJG-derived files are
listed in `third_party/dng_sdk/THIRD_PARTY_NOTICES.md`. No Adobe documentation,
photographs, tools or binaries are vendored. Section 5 commercial-product
indemnification, trademarks, per-file notices and binary redistribution terms
still require release review. Fetched libraries retain their own licences.
CMake collects principal notices in `RawDependencyNotices` and copies them into
macOS app Resources. This does not replace a complete binary notice/source-offer
review, including LibRaw's selected LGPL/CDDL obligations, per-file notices and
reproducible source availability. Never ship an optional SDK binary without that
review and the required acknowledgements. No externally fetched parser is a
security guarantee; retain malformed-input/memory-safety validation and monitor
upstream security fixes when updating these exact pins.
