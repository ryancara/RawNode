# Adobe DNG SDK 1.7.1 build 2768 — research subset

These are unmodified source files from the user-supplied official Adobe archive
`dng_sdk_1_7_1_2768_20261013.zip`, SHA256
`c1d3a545ed45b150e59af2e6aa0695c1551f0e2bed34ad87436898a05194d2b1`.
The version/build macros remain in `source/dng_flags.h`. `UPSTREAM.json` records
each included upstream file's SHA256 and every omitted source filename.

This dependency is used only by `research/jxl-dng-poc/CMakeLists.txt`. RawNode's
production build does not reference it.

## Included and omitted

`source/` contains 145 upstream files: 63 C++ source files and 82 headers. The
selection starts with the 62 SDK translation units linked into the working Linux
prototype, then follows their local includes and the probe/LibRaw bridge's header
includes. The extra C++ file, `dng_lossless_jpeg_shared.cpp`, is included directly
by `dng_lossless_jpeg.cpp` and is not separately compiled. Some writer/preview and
profile support remains because the SDK's linked core references it internally;
this is not a new image-export path in the prototype. Conditional XMP declaration
headers are retained where included, but XMP implementation/toolkit code is absent.

Excluded: the validation application, XMP implementations and toolkit, file
metadata updater, standalone renderer implementation and other unreferenced
translation units/headers listed in the manifest. The package's projects,
documentation/specifications, photographs, JPEG/JXL dependency trees, executables
and compiled libraries are not vendored.

`LICENSE.txt` is the original, unmodified Adobe agreement. The additional licence
files under `licenses/` are necessary third-party notices, not optional SDK
documentation. See `THIRD_PARTY_NOTICES.md` for their scope.

## Compatibility adjustment

No checked-in upstream file has been edited. The existing research helper
`prepare-sdk-compat.py` generates a build-directory copy of `source/dng_jxl.cpp`
with two `qDNGUseXMP` guards: one around encoder XMP access, and one around the
`ProcessXMPBox` body. The original 2768 code otherwise fails to compile with XMP
disabled. `research/jxl-dng-poc/logs/sdk-2768-no-xmp.patch` records the added guards.
The generated copy and patch retain upstream notices; no pixel codec changes are
made. Never apply this adjustment silently to a different SDK release.

## Redistribution assessment

Section 1 of the actual Adobe agreement explicitly grants worldwide, royalty-free
rights to use, reproduce, prepare derivatives of, publicly display/perform,
distribute and sublicense the **software**, including source code, for any purpose.
There is no source-private or noncommercial-only restriction in that grant.
Accordingly this source subset may be published with the agreement's conditions
and the separately applicable third-party conditions preserved.

Section 2 requires preserving copyright and other notices in human-readable
copies. All upstream file contents/notices and the full licence are retained.
The separately defined **documentation** has a different grant and may not be
modified; no Adobe manuals/specifications/readmes are included here. Do not infer
that sample photographs or every other ZIP member have the source files' terms.

Other relevant terms: software is supplied as-is; section 5 requires Adobe
indemnification when distributing the software in a commercial product; section 6
does not permit Adobe trademark endorsement without separate permission; material
noncompliance terminates rights. California law/jurisdiction is specified. The
DNG file-format/patent licence is separate from this software agreement. This
publication is source-only research, not a commercial binary packaging decision.

The included source's non-Adobe notices are accounted for below. No necessary
source component with unclear or prohibited redistribution was identified in
this subset. Future binary distribution, project-wide licence compatibility and
commercial-product obligations still need their own packaging review.
