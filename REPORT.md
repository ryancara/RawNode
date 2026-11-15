# PR #116: native Windows Trash findings

**15 November 2026. The unchanged adapter reproduced silent permanent deletion.**
No production correction was implemented. PR #116 remains draft, unmerged, and
at `8eb251a9827133bd22e7e756070114e8e50c5949`; macOS/Linux are unchanged.

## What actually ran

Two fresh GitHub-hosted Windows Server 2022 Datacenter VMs, build 20348, native
MSVC and native Windows Shell COM; 30 disposable BMP fixtures per run. Each
fixture had a byte-verified independent copy on D:, uploaded before any deletion
call. Settings changed only on new, identity-checked 128 MiB virtual disks.
System-volume policies were untouched. No hard links or custom Trash were used.

The executable linked the **exact production FileTrash.cpp** from the pinned
commit. Separate instrumented calls reproduced its flags, callback decisions,
and result classification. Each scenario also tested removal of only
`FOF_NOCONFIRMATION`, an unconditional pre-delete veto, and an ALLOWUNDO policy
control that always vetoed. Controls are diagnostics, not proposed adapters.

| Scenario | Exact production and instrumented equivalent | Without FOF_NOCONFIRMATION |
| --- | --- | --- |
| VM system drive C:, fixed NTFS, existing settings | Recycled; native restore matched SHA-256 | Same |
| New fixed NTFS R:, bin enabled, 64 MiB quota | Recycled; native restore matched SHA-256 | Same |
| New fixed NTFS S:, per-volume NukeOnDelete=1 | Refused; original hash intact; no bin entry | Same |
| New fixed NTFS T:, 1 MiB quota, 12,582,966-byte BMP | **Permanently deleted; original missing; no bin entry** | Permanent-delete prompt; harness clicked No; original intact |
| New fixed FAT32 U:, bin enabled | Recycled; native restore matched SHA-256 | Same |
| New fixed exFAT V:, bin enabled | Recycled; native restore matched SHA-256 | Same |
| Physical external HDD/SSD | **Unverified: no attached hardware** | Unverified |
| Removable FAT32/exFAT SD card | **Unverified: no attached hardware** | Unverified |

Both runs agreed: **24 recycled and restored, 32 originals preserved, four
permanently deleted originals** (two exact-production calls and two instrumented
calls). The 32 preserved originals include 24 unconditional-veto controls, six
disabled-bin refusals and two harness-declined dialogs. Every independent backup
remained verified. No timeout or observation error occurred. Native restoration
used the Shell `undelete` verb, never the independent backup; every restored file
matched its original SHA-256 and disappeared from the Shell bin namespace.

The repeat additionally read `$Recycle.Bin` directly: successful recycling had
matching `$I` original-path metadata and a byte-identical `$R` payload; both
permanently deleted fixtures had neither. They could not be restored from the
Recycle Bin. Their bytes remained recoverable from the harness backups only;
this protection does not exist in the production adapter. No forensic undelete
was attempted.

## Callback evidence

Production flags: `0x00183414`; omitting NOCONFIRMATION: `0x00183404`.
`TSF_DELETE_RECYCLE_IF_POSSIBLE` is bit `0x80`. All instrumented setup calls
(`CoInitializeEx`, creation, flags, source item, queue) returned `S_OK`.

| Instrumented outcome | Pre flags / decision | Post flags / HRESULT / created item | PerformOperations / aborted |
| --- | --- | --- | --- |
| Normal recycle | `0x282` / S_OK | `0x282` / `0x00270008` / non-null bin item | S_OK / false |
| Disabled-bin refusal | `0x202` / E_ABORT | `0x202` / `0x80004004` / null | E_ABORT / false |
| Oversized permanent deletion | `0x282` / S_OK | `0x282` / `0x00270008` / **null** | S_OK / false |
| Oversized without NOCONFIRMATION, No clicked | `0x280` / S_OK | `0x280` / `0x80270000` / null | `0x80270000` / true |
| Forced veto, including oversized | `0x282` / E_ABORT (disabled: `0x202`) | Same flags / E_ABORT / null | E_ABORT / false |

Normal recycling without NOCONFIRMATION used `0x280`; disabled refusal used
`0x200`, with otherwise identical results. GetAnyOperationsAborted itself
returned S_OK throughout. These are per-item sink callbacks; no second
PreDeleteItem removing recycle intent was delivered before oversized deletion.

**The existing pre-delete rule is insufficient.** It safely vetoed disabled-bin
deletion when recycle intent was absent, and an unconditional E_ABORT worked in
every control. But the oversized fallback retained the recycle-intent bit in
both callbacks. It was therefore indistinguishable from normal recycling at
PreDeleteItem using this rule. A null item afterwards detects loss too late.

**Removing NOCONFIRMATION prevented silent approval in the reproduced case, but
did not automatically refuse permanent deletion.** The captured dialog said:
“This file is too big to recycle … Do you want to permanently delete it?” and
offered Yes/No. The harness clicked No. We never clicked Yes; the observed
preservation depends on that response. A Move to Trash operation that still
offers permanent deletion does not meet the strict recycle-or-preserve contract.

**Separate reporting defect:** successful recycling returned the successful
HRESULT `0x00270008`, not S_OK. Production's `result == S_OK` check returned false
even for all eight exact-production operations that actually recycled and
restored. If this adapter is retained later, success classification should accept
`SUCCEEDED(result)` while still requiring a non-null recycled item. That fixes
reporting, not fallback prevention. No such correction was implemented here.

## Recommendation and remaining evidence

No tested small IFileOperation adjustment establishes RawNode's complete safety
requirement. Removing NOCONFIRMATION is supported as a way to stop the observed
silent Yes; it still leaves an impermissible permanent-delete choice. Keeping
the current pre-delete intent check or correcting success classification cannot
prevent the reproduced loss.

**Recommended minimal interim correction, after review: explicitly refuse
Windows Trash before invoking deletion.** Keep macOS/Linux native support.
Do not add a dependency, custom Trash or protective hard links on this evidence.
If a small automatic recycle-or-refuse shell configuration is subsequently
demonstrated, evaluate that before considering a recovery subsystem.

Before enabling Windows: demonstrate automatic fallback refusal, then repeat on
supported Windows 10/11 client builds with physical external HDD/SSD and actual
removable FAT32/exFAT SD cards. Fixed VHD results do not establish removable-media
behavior. Also outstanding: GUI/group-policy variants, occupied/full or damaged
bins, access-denied/locked files, disk full, UNC/mapped/disconnected shares,
reparse/cloud files, and application-level cancellation/partial batches. The
tested per-volume registry settings were read back and corroborated by actual
shell behavior; they are not evidence for every policy mechanism. RAW/sidecar,
Unicode/long-path and metadata/stream restoration cases remain untested here.

## Reproducible evidence

- [First completed native run](https://github.com/ryancara/RawNode/actions/runs/43665533196), diagnostic source `d412c0220113a42d96b6b35aed04c7272342e6f1`; [raw results](https://github.com/ryancara/RawNode/blob/a2c4ba9a9293a88ad0622b4be137c67610a52d98/results/native-observations.json).
- [Repeat with full dialog and direct bin evidence](https://github.com/ryancara/RawNode/actions/runs/43665759467), diagnostic source `c0472eb848711a6972ff0d4d7bdcc8424766626e`; [raw results](https://github.com/ryancara/RawNode/blob/3b6384f4e5a6c2723a7825f51fb69a8191189786/results/native-observations.json), [environment](https://github.com/ryancara/RawNode/blob/3b6384f4e5a6c2723a7825f51fb69a8191189786/results/native-environment.json), [fixture/policy manifest](https://github.com/ryancara/RawNode/blob/3b6384f4e5a6c2723a7825f51fb69a8191189786/results/manifest.json), [callback traces](https://github.com/ryancara/RawNode/tree/3b6384f4e5a6c2723a7825f51fb69a8191189786/results/callbacks).
- [Independent repeat-run backups](https://github.com/ryancara/RawNode/actions/runs/43665759467/artifacts/14141283797) and [observations artifact](https://github.com/ryancara/RawNode/actions/runs/43665759467/artifacts/14141582957), expiring 22 November 2026. JSON/traces remain in diagnostic Git history.
- Production source SHA-256: LF `218ffeac95288cbf30faf73013a29347417cd800cfed727959914602fbe05ae1`; Windows checkout CRLF `e6c5d7919d0a68f96497ecc39074a02464d697f764eff8cfddf5708f3e669915`. The newline-normalized contents match the pinned source.

An initial availability run and one harness link failure preceded these two
completed runs; neither submitted any fixture to the Trash API. A green probe
workflow means evidence collection completed, not that the adapter is safe.
