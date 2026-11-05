#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p downloads sources
archive=downloads/LibRaw-0.22.3.tar.gz
expected=7edf96c5b70cff88980a02ab0fc8af125cb6a4a377985573c654439d9d02355c
if [[ ! -f "$archive" ]]; then
  curl -fL https://codeload.github.com/LibRaw/LibRaw/tar.gz/refs/tags/0.22.3 -o "$archive"
fi
python3 - "$archive" "$expected" <<'PY'
import hashlib, pathlib, sys, tarfile
archive=pathlib.Path(sys.argv[1])
actual=hashlib.sha256(archive.read_bytes()).hexdigest()
if actual != sys.argv[2]:
    raise SystemExit('LibRaw archive checksum mismatch: '+actual)
destination=pathlib.Path('sources/LibRaw-0.22.3')
if not destination.exists():
    destination.mkdir()
    with tarfile.open(archive,'r:gz') as tf:
        for member in tf.getmembers():
            parts=pathlib.PurePosixPath(member.name).parts
            if len(parts)<2 or '..' in parts or member.issym() or member.islnk(): continue
            member.name=str(pathlib.PurePosixPath(*parts[1:]))
            tf.extract(member,destination,filter='data')
print('LibRaw 0.22.3 archive SHA256:',actual)
PY
