# RawNode Windows atomic persistence: native feasibility evidence

Date: 17 November 2026. Research only; no production persistence changes, implementation PR or merge.

**Decision: READY FOR IMPLEMENTATION of a bounded local-NTFS implementation, subject to owner approval of the strategy below.** This is not qualification of every Windows filesystem, cloud provider, security configuration or I/O-failure class. Unsupported configurations must refuse safely.

## Provenance and native environment

- Branch: [`research/windows-atomic-persistence-2026-11-17`](https://github.com/ryancara/RawNode/tree/research/windows-atomic-persistence-2026-11-17).
- Base main: `22cd635669237324ece419a671385b023f153dc9`, including merged PR #116.
- Exact tested source/workflow commit: [`20f820ecafa305c843c0c9ca3dc0846acade1897`](https://github.com/ryancara/RawNode/commit/20f820ecafa305c843c0c9ca3dc0846acade1897).
- [Final native Actions run 43985082057: SUCCESS](https://github.com/ryancara/RawNode/actions/runs/43985082057). MSVC compilation, runner-account matrix, ordinary-user matrix and genuine disk-full test all passed.
- [Complete raw evidence artifact](https://github.com/ryancara/RawNode/actions/runs/43985082057/artifacts/14285585676), SHA-256 `a2e8828dc6dc27d8e2952dc54c9aa953940093b27e11386b4ee1df80f5f3949f`; artifact expiry 1 December 2026.
- GitHub-hosted `windows-2022`, x64 Windows Server 2022 Datacenter **10.0.20348.5870**; Visual Studio 2022 17.14.45; MSVC **19.44.35230.0**, C++17.
- Program verified `D:\` as NTFS and `DRIVE_FIXED`, volume serial `3901700721`. Both accounts used this volume. A separate disposable 64-MB NTFS VHD on `Z:\`, serial `3738307975`, supplied the disk-full condition.
- Runner account SID ended in `-500`; the additional disposable account ended in `-1003` and was added only to the ordinary Users group. The account was removed after testing. All fixtures and the VHD were disposable, with independent backups outside the parent directory being manipulated.

The standalone [probe](https://github.com/ryancara/RawNode/blob/20f820ecafa305c843c0c9ca3dc0846acade1897/research/windows-atomic-persistence/probe.cpp) and [workflow](https://github.com/ryancara/RawNode/blob/20f820ecafa305c843c0c9ca3dc0846acade1897/.github/workflows/windows-atomic-research.yml) are isolated from production code. The probe does not call or implement RawNode's Windows persistence entry point.

Two earlier runs remain useful evidence, not passing qualification runs:

- [43984002679](https://github.com/ryancara/RawNode/actions/runs/43984002679), commit `ed994bd38c66c75cc938a30921d25d9c53081c13`: two incorrect probe assertions. Changing a parent DACL made the temporary file unobservable before rename; `-1` also names the current-process pseudo-handle, so it was an unsuitable invalid-close fixture. Neither was observed destruction of a destination.
- [43984628111](https://github.com/ryancara/RawNode/actions/runs/43984628111), commit `90e725938f9c5ed9e6cdc446e6b8cb5c362911dd`: stopped at ACL preparation, where a temporary handle requested only `WRITE_DAC` and `SetSecurityInfo` returned error 5 under both accounts. The final run tested a sufficient access mask and both handle/named API variants.

## Measurement and independent verification

`native.jsonl`, `native-standard-user.jsonl` and `disk-full.jsonl` contain operation returns/errors and independent before/after snapshots: exact Unicode paths, existence/open errors, bytes in hex, SHA-256, sizes, attributes, reparse tags/data, volume/file IDs, creation/last-write times, and owner/group/DACL SDDL. Access times are deliberately excluded from unchanged-state comparisons because observation can affect them.

The ordinary fixtures are distinct binary payloads, including an embedded NUL and CRLF:

| Payload | Size | SHA-256 |
|---|---:|---|
| OLD | 38 bytes | `7b2fb52ace4e9a7891ec8a730a5587af71c80331417b532df81ada49c63205f5` |
| NEW | 46 bytes | `26fb676aa0dddbf1ce32a371aff3f4286bcfe1cba47723e2ddbbdeabc55c77cb` |

Independent local Python SHA-256 recomputation matched all **1,319 complete byte/hash snapshots** in the final artifact. The large disk-full temporary is described by its length, streamed SHA-256 and known repeated `F` payload; its complete hex is intentionally omitted. Independent backup bytes/hashes and identities were checked, not inferred from API returns.

Final results: **399 runner-account checks + 382 ordinary-user checks + 3 disk-full checks; zero failed checks and no skipped cases.** Check totals differ because the concurrent observer produces different numbers of failed-rename preservation checks.

## Native outcomes

Except for the VHD row, outcomes below were observed under both accounts. Error 0 on success is a normalized log value; `GetLastError` is captured immediately only on failure.

| Experiment | Actual API outcome | Independently observed filesystem state |
|---|---|---|
| Completed sibling to absent destination | `MoveFileExW(...,0)` TRUE | Destination has all NEW bytes; temporary name absent; published ID equals temporary ID. |
| Existing regular destination | `MOVEFILE_REPLACE_EXISTING` TRUE | Same complete publication and identity change; independent backup unchanged. Twelve isolated replacement fixtures per account also passed. |
| Creation when destination exists | FALSE, **183**, `ERROR_ALREADY_EXISTS` | Original bytes, pathname, ID, times, attributes and security unchanged; temporary unchanged. |
| Held destination reader, with/without delete sharing | FALSE, **5**, `ERROR_ACCESS_DENIED` | Original and temporary unchanged. `FILE_SHARE_DELETE` did not make replacement succeed in these tests. |
| Reader held by another process, both sharing modes | FALSE, **5** | Same preservation. Child subsequently read complete OLD bytes through its held handle. |
| Read-only destination | FALSE, **5** | Original bytes/security/ID unchanged; read-only attribute remains set. No clearing fallback. |
| File denies write-data/append, raw rename | Write preflight FALSE, **5**; rename **TRUE** | NEW replaces OLD despite denied writing; original DACL is replaced by the temporary DACL. This contradicts relying on rename alone for RawNode's writable-file policy. |
| Same write-denied file with explicit preflight refusal | Preflight FALSE, **5**; no rename | Original and temporary unchanged. |
| Destination denies DELETE and parent denies delete-child | Rename FALSE, **5** | Observable original and temporary both unchanged. |
| Destination is directory | FALSE, **5** | Directory remains unchanged; temporary unchanged. |
| Missing destination parent | FALSE, **3**, `ERROR_PATH_NOT_FOUND` | No destination/parent created; temporary remains intact. |
| Existing temporary-name collision | `CREATE_NEW` fails, **80**, `ERROR_FILE_EXISTS` | Collision file and destination unchanged. |
| Write and flush through a read-only temporary handle | Both FALSE, **5** | Native access failures; destination and temporary unchanged. This is not a simulated storage failure. |
| Real closed event handle closed again | FALSE, **6**, `ERROR_INVALID_HANDLE` | Valid fixture handles were explicitly closed and checked successfully. This does not simulate a valid-file close failing after buffered I/O. |
| Abort after writing half a temporary payload | Explicit pre-publication abort | Partial temporary observed, then deleted; original unchanged. Cleanup evidence only, not an internal rename-failure test. |
| Source held without delete sharing | Rename FALSE, **32**, `ERROR_SHARING_VIOLATION` | Destination and source unchanged. Attempted cleanup also returned 32 and left the exact temporary path intact. |
| Ordinary cleanup after destination-reader rename failure | Rename FALSE, **5**; deleting own temporary TRUE | Only own temporary removed; original and backup unchanged. |
| Real NTFS disk-full temporary writing | `WriteFile` FALSE, **112**, `ERROR_DISK_FULL`, after **53,608,448 bytes** | Original/backup unchanged. Subsequent flush and close succeeded; partial temporary was independently hashed, then cleanup succeeded. No rename was attempted. |

**No tested failed publication removed, renamed, truncated, changed the bytes of, or changed the compared metadata of the original destination.** This statement describes these observations only.

### Concurrent visibility

Each account attempted 200 replacements of the **same pathname** while a thread repeatedly opened/read it with read/write/delete sharing. Payloads were 4,094-byte A/B records with identifying headers/trailers.

| Account | Successful replacements | Failed replacements | OLD reads | NEW reads | Partial/unexpected payloads | Missing destination |
|---|---:|---:|---:|---:|---:|---:|
| Runner | 50 | 150, all error 5 | 3,373 | 5,772 | 0 | 0 |
| Ordinary user | 67 | 133, all error 5 | 6,853 | 6,855 | 0 | 0 |

All 283 failures had independent destination/temporary comparisons; all passed. Total complete observer reads: **22,853**. This supports atomic visibility on this configuration while revealing frequent safe refusal with open readers. It is not proof for every interleaving, filesystem driver or late I/O error.

## Security, attributes, paths and alternate rename

**ACLs:** Raw rename did not retain an explicitly protected destination DACL. Copying it to the temporary before publication preserved it exactly using either `GetSecurityInfo`/`SetSecurityInfo` or the separately tested named setter. Explicit ACEs combined with inherited, unprotected DACLs also survived exactly. The tested destination handle requested `READ_CONTROL`; the temporary handle requested `READ_CONTROL | WRITE_DAC | FILE_READ_ATTRIBUTES`. A private protected DACL supplied at `CREATE_NEW` also survived publication. These are small native operations, not a need for a security subsystem. Ordinary callers can refuse when necessary inspection/copying fails. No privilege escalation is proposed. These tests do not qualify SACLs, mandatory labels, foreign ownership or other specialized security metadata. [Native security API](https://learn.microsoft.com/en-us/windows/win32/api/aclapi/nf-aclapi-setsecurityinfo)

**Attributes:** Replacing a hidden/archive destination (34) with an ordinary temporary (32) dropped the hidden bit. Preparing the temporary with the intended hidden/archive attributes preserved them. Read-only refusal remained intact. Advanced attributes, EFS/compression and alternate streams were not qualified.

**Unicode:** Accented, CJK and supplementary-character destinations were created and replaced successfully. Strict `MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS)` paths and C++17 `u8path` agreed; invalid UTF-8 was refused before I/O. Reading and writing must share this conversion. Narrow `std::filesystem::path(std::string)` conversions in RawNode's current read/configuration paths still need scoped correction; a Unicode writer alone is insufficient. [Conversion contract](https://learn.microsoft.com/en-us/windows/win32/api/stringapiset/nf-stringapiset-multibytetowidechar)

**Reparse points:** Symbolic links and parent junctions were constructed successfully under both accounts. Preflight refused them, with link data, target contents and compared metadata unchanged. A final-component-only check missed the parent junction; ancestor inspection detected it. These are refusal experiments, not attempts to qualify raw rename through links or adversarial namespace races. A blanket all-reparse refusal would also reject some cloud placeholders. Microsoft's tag/name-surrogate distinction permits more precise classification, but **OneDrive was not present/tested**, so no claim of safe cloud compatibility follows. Do not reject a path merely because its directory is named OneDrive; provider-backed reparse configurations still need qualification. [Reparse tags](https://learn.microsoft.com/en-us/windows/win32/fileio/reparse-point-tags)

**Handle rename:** `SetFileInformationByHandle(FileRenameInfo)` successfully published a source while the owned source handle prevented another DELETE-access open (error 32). This demonstrates a concrete source-identity benefit. It was one comparison per account, not a full alternative matrix; it is not needed as an automatic fallback for the observed reader refusals.

## Recommended minimal production strategy

The earlier ART/vkdt comparison remains applicable: ART `40ec1cd38a11e13883d8f6159ce7017aa97e988a` uses [`ProcParams::write()`](https://github.com/artraweditor/ART/blob/40ec1cd38a11e13883d8f6159ce7017aa97e988a/src/engine/procparams.cc#L6361), and vkdt `aa9329fcc04b666f0f4b1cf5961835b6e56ac636` uses [`dt_graph_write_config_ascii()`](https://github.com/hanatos/vkdt/blob/aa9329fcc04b666f0f4b1cf5961835b6e56ac636/src/pipe/graph-io.c#L476). Both directly open/truncate the destination and leave write/close errors unchecked. Their Unicode mechanisms are useful references; their publication behavior does not satisfy RawNode's contract and is not adopted.

Retain the existing persistence interface, serialization, destination validation and document-save lifecycle. Add a small Windows-specific writer with ordinary RAII:

1. Strictly convert the path; qualify the resolved local NTFS location and reject unsupported destinations/reparse configurations. Preserve the existing single-session/stable-directory scope; do not claim protection against arbitrary concurrent namespace substitution.
2. Inspect an existing destination without following its final reparse point. Refuse non-regular/read-only/write-denied files and security/metadata cases that cannot be preserved by the supported policy. Read destination DACL/protection before preparing replacement data.
3. Reserve a unique sibling with `CREATE_NEW` and a suitable private creation DACL. Write all bytes, prepare required DACL/ordinary attributes, check `FlushFileBuffers` and close before publication.
4. Use `MoveFileExW` flags 0 for an absent destination and only `MOVEFILE_REPLACE_EXISTING` for an existing validated file. Never enable `MOVEFILE_COPY_ALLOWED`, delete the destination first, clear its permissions, or switch to in-place writing. Microsoft documents the copy behavior controlled by that flag; it is outside this strategy. [MoveFileExW](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-movefileexw)
5. On ordinary failure, delete only the owned temporary and report operation/error plus the exact remaining path if cleanup fails. Preserve live edits through existing Retry / Discard / Cancel. Do not infer that access denied means a transient reader conflict or automatically overwrite on retry.

ACL transfer and attribute preparation are essential: **bare MoveFileExW is insufficient** for RawNode's complete contract. No persistence manager, background worker, recovery journal or third-party dependency is justified by these results.

## Practical limits and decision

Actual evidence supports implementing this bounded local-NTFS strategy. The decision is an engineering inference from the tests plus documented API behavior, not an unconditional Windows guarantee.

Not qualified: ReFS, FAT32/exFAT, SMB/NAS/mapped network drives, OneDrive/other cloud providers, desktop Windows variants, long-path boundaries, special security/stream metadata, malicious or concurrent directory/ACL replacement, media errors inside rename, interruption/power loss, and a valid-file close or flush failing because of storage hardware. The access-denied rename tests are genuine native failures; none deliberately forces the filesystem's rename operation to fail after an internal mutation. The disk-full experiment exercises preparation failure, not a disk-full rename.

Unverified filesystem/provider configurations should remain unsupported initially. New regression coverage must later exercise the actual RawNode implementation and its callers, not just this standalone probe. No crash-durability or filesystem-wide transaction promise is added.

**READY FOR IMPLEMENTATION within the stated local-NTFS boundary, pending approval.** Broader Windows/filesystem/cloud support still requires focused qualification. Stop here; no production implementation has been written.
