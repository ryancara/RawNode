# Corrected JPEG XL prototype review entry

Start with [README.md](README.md) for the current stage policy, independently
reproduced failures, crop/float limits, Release/sanitizer commands and remaining
work. This correction follows review of commit
`58bda0b3ff316f191872d1869fe1b421b3c731a1` on the same research branch.

Review these focused changes:

- `src/probe.cpp`: before-open Stage 2 selection from OpcodeList2; float rejection;
  ActiveArea/crop reconciliation; explicit unsupported TrimBounds handling.
- `tests/jxl_checks.h`: temporary real JXL DNG generation and all-pixel analytical
  oracles, SDK dispatch, geometry, float diagnostics and error controls.
- `tests/check_stage_mutation.py`: restore the reviewed flag in a temporary build
  and require numerical failure.
- Standalone CMake/preparation: OpenMP, sanitizer builds and a hash-guarded one-line
  libjxl encoder initialization fix justified by the actual UBSan trace.
- `tests/PROVENANCE.md`: bundled/upstream codec differences and decoding relevance.
- `logs/correction/`: initial reproductions/findings and final executed evidence.

The original photographic fixture runners remain optional and need externally
supplied images under ignored `fixtures/`; no images are committed. Their
ordinary DNG filename is `adobe-04-ordinary.dng`. CTest needs no photographs or SDK
ZIP. Historical SDK/vendor logs remain clearly separated from corrected results.

The SDK subset and licensing assessment remain in `third_party/dng_sdk/`, including
its original Adobe agreement, required RSA/Apache/IJG/Cornell notices and upstream
hashes. No SDK source changes are introduced by this correction. LibRaw and codec
source checkouts/builds are ignored; preparation scripts retain upstream notices.

RawNode production files, root CMake, main and renderer are outside this change.
No production integration PR or merge is part of this task. Review independent
expectations and failure boundaries, not merely same-SDK comparisons or successful
unpack. General DNG photographic rendering and PureRAW validation remain deferred.
