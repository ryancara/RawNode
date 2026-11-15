# Native Windows Trash investigation

Diagnostic-only branch for RawNode PR #116. The production branch remains at
8eb251a9827133bd22e7e756070114e8e50c5949. No production adapter changes and no merge.

The first run checks native Windows availability only. It submits no files to
Trash and changes no Recycle Bin settings. Subsequent disposable-file probes
must retain independent backups, limit settings to test-owned virtual volumes,
and distinguish real hardware from fixed virtual disks.
