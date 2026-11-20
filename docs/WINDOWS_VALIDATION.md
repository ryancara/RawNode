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

## PR #122 review follow-up

On 2026-11-20, the owner reported Claude's independent review of
[PR #122](https://github.com/ryancara/RawNode/pull/122) as **SAFE TO MERGE**.
The requested follow-up gives a successfully completed, verified permanent
deletion precedence over aggregate cancellation, while retaining explicit
per-item cancellation. The other non-blocking implementation findings are
accepted without additional defensive state, abstractions or test infrastructure.

PR #122 remains open, draft and unmerged pending the owner's manual validation
of the updated portable Windows build. Updated CI results, build provenance and
checksum are recorded on the PR. Runner results and the earlier PR #120 desktop
result remain separate from validation of this updated build.

## Outstanding Windows work

- Windows JPEG XL support remains outstanding, including native ON
  build/runtime/packaging validation. The validated desktop build has JPEG XL OFF.
- Windows Move to Trash is implemented in draft PR #122 using the revised
  interactive contract. Automated native evidence and manual checks are tracked
  in [Windows Trash research](WINDOWS_TRASH_RESEARCH.md). Independent review is
  reported SAFE TO MERGE; the updated build awaits owner manual validation.
  Physical external/card checks remain separately tracked.
  The owner-reported PR #120 desktop result above did not include this feature.

Feature-specific native checklists retain their own validation records. The
future detachable-panel/multi-monitor milestone is still planned work.
