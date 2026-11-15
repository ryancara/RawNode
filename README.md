# Native Windows Trash investigation

Diagnostic-only branch for RawNode PR #116. The production branch remains at
8eb251a9827133bd22e7e756070114e8e50c5949. No production adapter changes and no merge.

The first run checks native Windows availability only. It submits no files to
Trash and changes no Recycle Bin settings. Subsequent disposable-file probes
must retain independent backups, limit settings to test-owned virtual volumes,
and distinguish real hardware from fixed virtual disks.

The harness compares the exact production adapter with an instrumented equivalent,
a variant omitting only FOF_NOCONFIRMATION, and an unconditional PreDeleteItem veto
control. Independent BMP backups are uploaded before any Trash calls. All bin
policy changes are restricted to newly created, identity-checked virtual volumes.
Only Cancel/No may be automated; UI timeouts are inconclusive. Fixed virtual
FAT32/exFAT disks are not evidence for physical removable SD cards or USB devices.

The policy-intent control uses ALLOWUNDO instead of RECYCLEONDELETE and always
vetoes in PreDeleteItem. It tests whether a configured policy changes native
recycle intent; it is not a proposed production strategy. Registry readback alone
is not treated as proof that the shell honored a disabled-bin/size setting.
