# Integer JPEG XL DNG integration (draft)

Every configuration builds one pinned LibRaw **0.22.3** static library, `RawNodeRaw`.
No system LibRaw is linked or supported. Upgrading from an older LibRaw can change
ordinary RAW pixels; reproducing historical output is not a requirement.

`RAWNODE_ENABLE_JXL_DNG` defaults to **OFF**. It adds Adobe DNG SDK/JPEG XL support
to that same LibRaw target when ON; it does not select a LibRaw version. OFF needs
JPEG/zlib development libraries but neither the SDK, libjxl nor Python. OpenMP is
enabled when CMake finds it; otherwise LibRaw runs serially. Never link a second
LibRaw into the host to implement dispatch.

## Build

Use the normal dependencies listed in the root README, a C++17 compiler, Git and
CMake 3.16+. ON additionally requires Python 3 to prepare the two reviewed source
adjustments. Configure an out-of-tree build:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DRAWNODE_ENABLE_JXL_DNG=ON
cmake --build build --parallel 2
./build/OfxRawHost --selftest
```

On macOS the executable is also copied into `build/OfxRawHost.app/Contents/MacOS/`.
A fresh OFF build uses the same commands with `-DRAWNODE_ENABLE_JXL_DNG=OFF`.
Use separate build directories when comparing configurations.

The reviewed reference is research commit
`588b95a2093431007a0771883d367cdb3253cc53`; its branch is unchanged. Only the SDK
subset and necessary build/decoding rules are carried over, without research
executables, logs, float diagnostics or unused DefaultCrop rebasing.

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
assignment; UBSan found an invalid bool read in the prototype's fixture encoder.
This is a concrete initialization fix, not a sanitizer suppression or decoder
change. Patch files document both adjustments; unknown source hashes fail configuration.

## Loader policy and colour boundary

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
opcode lists. Its accepted geometry is the reviewed ActiveArea trim: final stored
and active dimensions must equal the original active dimensions, with zero
margins. Other changes fail explicitly. Production continues to ignore DefaultCrop;
it neither applies nor rebases those unused inset-crop fields.

Floating-point JPEG XL development is unsupported and rejected before unpacking.
The integer development path must not silently quantize float input or produce
black pixels. The research branch retains its separate float diagnostic experiment.

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
loader's existing failure result. This uses the existing UI failure convention;
there are no new controls or appearance changes beyond the requested decoding.

## Tests

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

Private optional fixture paths can be supplied as `RAWNODE_TEST_NEF`,
`RAWNODE_TEST_DNG` and `RAWNODE_TEST_JXL`. These run the same public loader and
thumbnail path, checking dimensions, finite pixels and numerical sanity. Do not
check photographs into the repository. Full self-tests also exercise the existing
raster loading, colour transforms, preview/export, graph and persistence paths.

The available Linux x86_64 runs use GCC 14.2 and Clang 19.1.7. The production
loader's 18 synthetic cases comprise three ordinary DNG checks, eleven successful
JPEG XL checks and four expected failures (missing host, two float inputs and
TrimBounds). Measured errors against the independent all-pixel oracle:

| Fixture | Maximum absolute channel error | Assertion bound |
| --- | ---: | ---: |
| Ordinary/lossless, across both working gamuts | 0.000055669 | 0.0001 |
| Unequal whites plus OpcodeList2 | 0.000026922 | 0.0001 |
| Saturated lossy linear Rec.2100, output Rec.2020 | 0.000345440 | 0.0005 |
| Same lossy fixture, output Rec.709 | 0.000572794 | 0.0008 |

Upstream libjxl remains in use; no Adobe-specific skcms changes are imported.
This linear Rec.2100/XYB test does not establish behaviour for every PureRAW ICC
profile or JPEG XL colour encoding.

These bounds concern this authored distance-.01 fixture, not arbitrary lossy
photographs. The modest allowance covers quantization, published matrix rounding
and compiler/SIMD variation. White reference pixels stay inside ActiveArea to
avoid triggering LibRaw's existing automatic maximum adjustment in the oracle.

A temporary copy of the production loader with the original
`DNG_STAGE23_IFPRESENT_JPGJXL` policy restored fails the unequal-white OpcodeList2
oracle with maximum error **0.499998**, versus the 0.0001 bound. The working source
is not mutated. The corrected source passes. This reproduces the genuine WB
error rather than merely asserting two paths through the same SDK agree.

The available NEF decodes to 4284x2844 with RGB range approximately
[-0.390947, 1.21029]; the ordinary Adobe DNG to 1000x1000. Loading and thumbnails
succeed. Serial and OpenMP LibRaw 0.22.3 builds, verified against identical source
bytes, give identical entire-float-RGBA FNV64 fingerprints: NEF
`4b8ccd2b9513ddae`, ordinary DNG `cb1b7a418cf015a5`. The comparison reuses the same
production-loader objects; it creates no alternate production build path or
historical pixel baseline. Private-fixture timing also includes pixel validation,
fingerprinting and thumbnails; concurrent-build timings are not benchmark results.

GCC Release OFF and ON full builds and `--selftest` pass with the final bounds.
ON additionally decodes Adobe's real integer JPEG XL DNG to **9504x6336**, with
finite RGB values (approximately [0.003127, 0.485421]), working RGBA fingerprint
`21e9744e679140ab` and a successful thumbnail. Adobe's real float JPEG XL DNG is
explicitly rejected, as is the integer sample in an OFF build. These sample
files remain outside Git and are not PureRAW fixtures.

For these GCC Release builds, executables including the repository's self-tests
are 9,666,144 bytes OFF and 17,104,048 bytes ON. The complete ON suite including
the 60-megapixel Adobe sample peaks at 2,171,328 KiB RSS. This measures the whole
process/suite, not decoder-only allocation or a controlled performance benchmark.
Link-command and dynamic-library audits find exactly one static `RawNodeRaw`
implementation and no dynamic/system LibRaw in either executable.

Clang 19.1.7 **ASan/UBSan + leak detection** (Debug, JPEG XL ON, OpenMP ON)
passes the complete self-test suite, the available NEF/ordinary DNG and all new
JPEG XL fixtures. The existing narrowly scoped `stbiw__jpg_writeBits` shift-base
suppression is unchanged; no suppression is added for LibRaw, SDK or libjxl.
Instrumentation of both the host and OFX module was checked, and all four
independent deliberate-failure probes (use-after-free, signed overflow, negative
shift and leak) produced the expected symbolized diagnostics.

Clang 19.1.7 **TSan** (RelWithDebInfo, `-O1 -gline-tables-only`, JPEG XL ON,
`-DCMAKE_DISABLE_FIND_PACKAGE_OpenMP=TRUE`) also passes the complete suite,
NEF/ordinary DNG and JPEG XL cases, with no suppressions. This checks the serial
LibRaw configuration and host concurrency; it does not claim TSan validation of
OpenMP. No new sanitizer findings were observed. Sanitizer executables were run
with process-inspection access required by their runtimes, without disabling
leak detection.

Reproduce ASan/UBSan by configuring a fresh Clang build with
`-DCMAKE_BUILD_TYPE=Debug -DRAWNODE_ENABLE_JXL_DNG=ON` and setting both
`CMAKE_C_FLAGS` and `CMAKE_CXX_FLAGS` to
`-fsanitize=address,undefined -fno-omit-frame-pointer -fno-optimize-sibling-calls`.
Run `--selftest` with `ASAN_OPTIONS=detect_leaks=1:halt_on_error=1` and
`UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1:suppressions=$PWD/.github/sanitizers/ubsan.supp`.
Use the corresponding `-fsanitize=thread` flags in a separate build for TSan;
never combine the ASan and TSan runtimes. The normal Linux CI records the complete
ASan command, instrumentation checks and independent failure probes.

## Platform and release gates

Linux is the available development platform. macOS Apple Silicon and Windows
must be built and executed independently; source configuration is not proof of
platform support. Linux CI covers OFF/ON Release and Clang ASan/UBSan with JPEG XL
ON. The existing narrow stb JPEG UBSan suppression is unchanged; none is added
for RAW, SDK or JPEG XL.

For macOS Apple Silicon:

1. Install Xcode command-line tools and `brew install cmake pkg-config jpeg-turbo libtiff little-cms2 openexr python libomp`.
2. Configure fresh OFF and ON Release directories with `-DCMAKE_OSX_ARCHITECTURES=arm64`.
   If necessary pass Homebrew's `jpeg-turbo` prefix in `CMAKE_PREFIX_PATH`.
   For Apple Clang OpenMP, use `-DOpenMP_CXX_FLAGS="-Xpreprocessor -fopenmp"`,
   `-DOpenMP_CXX_LIB_NAMES=omp` and `-DOpenMP_omp_LIBRARY="$(brew --prefix libomp)/lib/libomp.dylib"`;
   add the libomp include directory to the compiler's include search if required.
   Confirm the resulting compile/link flags actually enable OpenMP; otherwise
   report the serial build, rather than claiming OpenMP is enabled.
3. Build and run full self-tests, then actual NEF/ordinary DNG/JPEG XL samples.
   Check rotated dimensions, previews, thumbnails, selected working space/transfer
   and export against trusted references. Run Clang ASan/UBSan with leak detection.
4. Check `file`, `lipo -archs` and `otool -L`; confirm arm64, exactly one static
   LibRaw implementation and all external runtime libraries packaged/resolvable.
   Repeat serial/OpenMP numerical and timing checks on the same 0.22.3 sources.

Windows requires a C++17 toolchain, CMake/Git/Python and the normal JPEG, zlib,
TIFF, lcms2, OpenEXR/Imath/OpenGL dependencies. SDK `qWinOS=1`, `LIBRAW_NODLL` and
User32 linkage are configured, but MSVC/Clang-cl, OpenMP and packaging are untested.
No new dependency manager is imposed. Check generated patches with LF checkout
and verify Release, float/geometry failure diagnostics and sanitizers where supported.

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

**Before merging or enabling JPEG XL by default:** validate real DxO PureRAW
High-Fidelity JPEG XL DNGs against ordinary PureRAW DNGs from the same RAW plus a
trusted external render. Check WB/as-shot metadata, camera matrices/profiles,
black/white/exposure, highlight/shadow retention, lossy error versus decoder error,
float rejection, dimensions/orientation and any OpcodeList1/2/3. Neither Adobe's
sample DNGs nor these synthetic colour tests establish PureRAW photographic
colour correctness. PureRAW samples are currently unavailable.
