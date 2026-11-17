# Native Windows persistence

Photograph grades and Pick/Reject metadata, preset-file updates and global
`preferences.json` share `persist_detail::replaceFileAtomically()`. On Windows,
`persist/WindowsFile` implements the native file-I/O portion; serialization,
schema validation, untouched-section preservation and document departure remain
with their existing owners. POSIX publication is unchanged.

The native feasibility evidence is pinned at research commit
`20f820ecafa305c843c0c9ca3dc0846acade1897`, with its report at
`f93327a591eeada9c1ca578e7577619badc88370` and successful Actions run
[43985082057](https://github.com/ryancara/RawNode/actions/runs/43985082057).
ART/vkdt direct-writing implementations were examined and are not adopted.

## Supported boundary

Initial support is for ordinary, singly linked files on conventional local fixed
NTFS volumes, within ordinary Win32 path-length limits. Paths are strict UTF-8,
converted to UTF-16 for both reads and writes. Embedded NUL, invalid UTF-8,
device/UNC/network/alternate-stream paths and ambiguous trailing dots/spaces are
refused. Preference paths read the Unicode Windows environment and return UTF-8.

Existing destinations must have ARCHIVE set. Expanded production tests found
that an ARCHIVE-cleared destination became ARCHIVE-set after NTFS replacement,
including when attributes were applied after closing the completed sibling
([native run 44024441044](https://github.com/ryancara/RawNode/actions/runs/44024441044),
commit `2ea1fe711af05a4be4e446dcfafea47e61b9333b`). The writer refuses such files
before creating a temporary; it does not change a backup marker silently or add
a fallible post-publication attribute fix. This includes an otherwise ordinary
file whose attributes are NORMAL. Supporting cleared ARCHIVE requires further
native investigation and independent approval of a different publication method.

Every existing parent is inspected. Symlinks, junctions/mount points, cloud and
other reparse-backed paths, compressed/encrypted/offline/special file attributes,
and ReFS/FAT32/exFAT/remote volumes are not qualified and refuse persistence.
Restrictions depend on filesystem metadata, never on directory names such as
`OneDrive`. This conservative initial policy can refuse provider-backed folders;
provider-specific support needs native qualification before widening it.

## Publication and permissions

The writer checks destination type, read-only state, explicit write-data access,
DACL/protection and streams before touching its contents. Unsupported object or
callback DACL entries, integrity/resource/central-access/trust/access-filter
protections, non-null/protected SACLs and alternate streams refuse replacement.
Even an empty filtered SACL may contain audit entries invisible to an ordinary
query, so it is refused rather than discarded. Owner and primary group
must match those naturally assigned to the replacement. No privileges are
enabled to force a save. Native fixtures verify auditing and mandatory-label
refusal; privileged audit-fixture setup occurs only in the tests.

A random sibling name is reserved with `CREATE_NEW`. A protected DACL grants
only the current user and SYSTEM access when the temporary is created, before
writing. Existing ordinary DACL ACEs and inheritance protection are copied to
the temporary before publication. Supported file attributes are hidden, archive
and not-content-indexed; read-only destinations are refused, not cleared.

Writes, flush and close are checked. Only then does `MoveFileExW` publish with
flags 0 for creation or `MOVEFILE_REPLACE_EXISTING` for an existing destination.
There is no copy, unlink-first, permission-clearing or alternate-API fallback.
Only the owned temporary is cleaned up on failure. Cleanup failure reports its
exact path and native error without silently retrying. Existing live edits and
Retry / Discard / Cancel behavior remain in force.

Windows can reject replacement while another reader is open, even when it
allows delete sharing. Refusal preserves the original; errors do not imply that
a permission-changing retry or fallback is safe.

## Validation and limits

`tests/windows-persistence` compiles the actual production writer, codecs,
preference functions and shared `SidecarFileTests.cpp` with MSVC C++17. The test
target uses the same pinned GLFW/ImGui headers/sources as the application and
normal function-level linking to omit unused rendering/decoder entry points;
there are no substituted persistence sources or duplicate algorithms.
The image-extension registry is mechanically separated from image decoding so
these production file-dialog dependencies can link without decoder libraries.
Native fixtures cover section/preset-file/preference round trips, Unicode,
permission/attribute preservation and refusal, alternate streams, reparse paths,
reader conflicts, deterministic I/O failures and cleanup diagnostics.
Linux/macOS full self-tests remain the application-level regression coverage.

This does not validate the full Windows application or GUI. It adds no conflict
detection, cross-process transaction, recovery service or power-loss durability
promise. The existing single-session contract assumes stable directories and
does not defend against arbitrary hostile namespace/ACL substitution. Late I/O
failures inside the native rename, other filesystems/providers, long paths and
special security configurations need further qualification.
