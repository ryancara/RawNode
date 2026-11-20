# Windows portable build validation

## Owner-reported native desktop validation

On 2026-11-20, the owner reported successful native Windows desktop validation
using the portable x64 Release build from
[PR #120](https://github.com/ryancara/RawNode/pull/120). This records the owner's
desktop result separately from automated runner checks.

Validated build provenance:

- Source commit: `ffe2ff2e8c6c49a335c8f082d766144e21cd21ce`.
- Application source and CMake settings match main at
  `4d211a68ebfd95e1cd3778aba318d8984cdc64d3`; the build uses MSVC x64 Release
  with `RAWNODE_ENABLE_JXL_DNG=OFF`.
- [Windows CI run 44642660200](https://github.com/ryancara/RawNode/actions/runs/44642660200).
- [Actions artifact 14587593161](https://github.com/ryancara/RawNode/actions/runs/44642660200/artifacts/14587593161),
  `RawNode-Windows-x64-Release`, retained for 14 days.
- Inner application ZIP SHA-256:
  `83287235bd450b25b339ec99b10fca4ef80c12652cfef76f40f19d46f6d8dee1`.

## Automated verification and distribution tooling review

The Windows CI run passed its existing full self-test and a second full self-test
from the actual ZIP extracted into a directory containing spaces. The second run
renamed the entire original checkout out of reach, cleared OFX/CTL search
overrides and limited PATH to Windows system directories. Tests exercised the
bundled Crop OFX plugin and production image-processing paths.

Review before merging PR #120 covered Release DLL/resource assembly, app-local
MSVC/OpenMP runtimes, x64 PE/import checks, ZIP extraction, subprocess exit status,
timeout and checkout/environment restoration, artifact upload and checksum
provenance. Downloaded artifact and inner ZIP checksums matched; the ZIP passed
CRC checks. The review found an omitted TinyEXR notice and added its vendored
header, which contains the notices, alongside the explicit OpenFX Support license
in subsequent packages. The owner-tested build identity above remains exact.

Distribution uses the existing Windows CI build and a portable test ZIP, with
the executable, runtime DLLs, OFX bundle, icon font and dependency notices.
There is no installer or separate packaging framework.

## PR #122 owner-reported Windows Trash validation

On 2026-11-20, the owner reported Claude's independent review of
[PR #122](https://github.com/ryancara/RawNode/pull/122) as **SAFE TO MERGE**.
The requested follow-up gives a successfully completed, verified permanent
deletion precedence over aggregate cancellation, while retaining explicit
per-item cancellation. The other non-blocking implementation findings are
accepted without additional defensive state, abstractions or test infrastructure.

The owner subsequently reported successful manual Windows validation on
2026-11-20, including multi-photo Trash, physical external HDD/SSD support,
native dialog ownership and cancellation, and authorized merging PR #122.

The updated portable build supplied for this validation was:

- PR head: `91f4da2616ba074a98468469348e762f76c2c3ff`.
- CI merge checkout: `92f8bebc860e0f243d9711febc9111363cd2f8fb`.
- [Windows CI run 44663150480](https://github.com/ryancara/RawNode/actions/runs/44663150480).
- [Portable build artifact 14597295972](https://github.com/ryancara/RawNode/actions/runs/44663150480/artifacts/14597295972).
- Inner application ZIP SHA-256:
  `82A57AA0CECCBB86F39EA85F8D7F69D08A2B197F72F747CE1E24F450955F086C`.

This owner-reported desktop result is separate from the eight native runner
fixture cases and full application/extracted-ZIP self-tests recorded on the PR.
The Windows client version and external-drive filesystems were not specified.
SD card testing remains unverified because hardware was unavailable. The owner
explicitly accepts that limitation as non-blocking for merge; no SD card support
claim is inferred from external HDD/SSD or virtual-disk results.

## Outstanding Windows work

- Windows JPEG XL support remains outstanding, including native ON
  build/runtime/packaging validation. The validated desktop build has JPEG XL OFF.
- SD card Trash testing remains unverified due to unavailable hardware. This
  recorded limitation is not a merge blocker for PR #122. Native implementation
  and runner evidence remain in [Windows Trash research](WINDOWS_TRASH_RESEARCH.md).

Feature-specific native checklists retain their own validation records. The
future detachable-panel/multi-monitor milestone is still planned work.
