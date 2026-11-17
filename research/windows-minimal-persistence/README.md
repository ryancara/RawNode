# Disposable research: minimal Windows atomic persistence (Claude)

Research only. Based on PR #117 head `5e5aa22145b3cfdb23674f00a991f3d748d62feb`.
Nothing here is linked into RawNode. Other CI workflows are removed on this
disposable branch only, so a push runs just this native Windows experiment.

- `MinimalWindowsFile.cpp`: experimental minimal writer.
- `probe.cpp`: compares bare `MoveFileExW`, the PR #117 production writer and the
  minimal writer on disposable fixtures, as an ordinary user and as the runner
  administrator (privileged fixture setup only).
- `unicode_integration.cpp`: production photo discovery -> sidecar read/save,
  built with and without a UTF-8 `activeCodePage` manifest.
