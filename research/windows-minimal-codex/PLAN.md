# Codex independent minimum Windows persistence research

Base: 5e5aa22145b3cfdb23674f00a991f3d748d62feb. Research only.
No changes to production sources or PR #117. No consultation of Claude's new results.

Compare bare sibling replacement, the unchanged reviewed backend and a smaller
experimental backend. Observe bytes, hashes, identities, ordinary attributes,
owner/group/DACL, selected/full SACL and streams independently. Native failures
must leave the original unchanged. Successful edits may create a new owner,
identity, timestamps and ARCHIVE state, as ordinary replacement does on POSIX.

Disputed cases: foreign/stale owner and group; inherited/protected DACLs;
inherited and explicit audit SACLs; integrity enforcement; private versus
inherited creation permissions; Zone.Identifier/provider/arbitrary streams;
hidden/system/not-indexed/archive/compressed/encrypted files and directories;
hard links; junctions to NTFS/FAT/exFAT; final symlinks; reader and write denial.
Use disposable VHDs and ordinary accounts. Record unavailable cases as untested.

Exercise actual folder discovery, image decoding (including profile detection)
and sidecar load/save with accented, CJK and supplementary filenames. Compare
the default process code page, explicit UTF-8 discovery and a UTF-8 manifest.
Compiling the headless image/persistence components is not full GUI validation.

The experimental backend is provisional: retain atomic sibling publication,
write access preflight, DACL inheritance semantics, checked I/O/cleanup and
final-link refusal. Test whether targeted integrity handling earns its cost.
Do not add ownership restoration, stream copying or enterprise policy management.

Acceptance: independently verified old/new bytes; unchanged original on ordinary
failure; no meaningful DACL/integrity weakening in the qualified cases; readable
code with each remaining restriction justified. Final policy recommendations
follow native observations rather than an arbitrary line-count target.
