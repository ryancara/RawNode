#!/usr/bin/env bash
# Only the upstream submodules needed by this standalone build are fetched.
set -euo pipefail
cd "$(dirname "$0")"

checkout_pinned() {
  local destination="$1" upstream="$2" revision="$3"
  if [[ -e "$destination/.git" ]]; then
    local actual
    actual=$(git -C "$destination" rev-parse HEAD)
    if [[ "$actual" != "$revision" ]] || [[ -n "$(git -C "$destination" status --porcelain --untracked-files=no)" ]]; then
      echo "Refusing to replace a different or modified checkout: $destination" >&2
      exit 1
    fi
  else
    if [[ -d "$destination" ]] && [[ -n "$(ls -A "$destination")" ]]; then
      echo "Refusing to overwrite nonempty directory: $destination" >&2
      exit 1
    fi
    git init -q "$destination"
    git -C "$destination" remote add origin "$upstream"
    git -C "$destination" fetch --depth=1 origin "$revision"
    git -C "$destination" checkout -q --detach FETCH_HEAD
  fi
  test "$(git -C "$destination" rev-parse HEAD)" = "$revision"
  echo "$destination $revision"
}

# v0.11.2 and the three gitlinks recorded in that upstream revision.
checkout_pinned sources/libjxl https://github.com/libjxl/libjxl.git \
  332feb17d17311c748445f7ee75c4fb55cc38530
checkout_pinned sources/libjxl/third_party/highway https://github.com/google/highway.git \
  457c891775a7397bdb0376bb1031e6e027af1c48
checkout_pinned sources/libjxl/third_party/brotli https://github.com/google/brotli.git \
  36533a866ed1ca4b75cf049f4521e4ec5fe24727
# Google's GitHub mirror exposes the same skcms commit as the upstream gitlink.
checkout_pinned sources/libjxl/third_party/skcms https://github.com/google/skcms.git \
  b2e692629c1fb19342517d7fb61f1cf83d075492
