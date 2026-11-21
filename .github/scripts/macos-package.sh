#!/bin/bash
# Build inputs and package names are set by macos-ci.yml.
set -euo pipefail

app="$BUILD_DIR/RawNode.app"
binary="$app/Contents/MacOS/RawNode"
mkdir -p "$app/Contents/Frameworks"
# Bundle both direct and transitive Homebrew libraries instead of
# relying on packages installed on the build/test machine.
crop="$app/Contents/PlugIns/OfxRawHost.ofx.bundle/Contents/MacOS/OfxRawHost.ofx"
bundle_path='@executable_path/../Frameworks/'
printf 'quit\n' | dylibbundler -ns -b -x "$binary" -x "$crop" \
  -d "$app/Contents/Frameworks" \
  -p "$bundle_path"
# Distinct Homebrew rpaths can collapse to one bundled path.
for file in "$binary" "$crop" "$app"/Contents/Frameworks/*.dylib; do
  while [ "$(otool -l "$file" | grep -cF "path $bundle_path ")" -gt 1 ]; do
    install_name_tool -delete_rpath "$bundle_path" "$file"
  done
done

# Retain binary redistribution notices alongside the packaged app.
notices="$app/Contents/Resources/RawDependencyNotices"
printf 'RawNode %s source and build instructions:\n%s/%s/tree/%s\n' \
  "$VERSION" "$GITHUB_SERVER_URL" "$GITHUB_REPOSITORY" "$(git rev-parse HEAD)" > "$notices/Source.txt"
cp LICENSE "$notices/RawNode-LICENSE.txt"
if [ "$JXL" = ON ]; then
  # Preserve all original per-file notices with the Adobe agreement.
  cp -R third_party/dng_sdk/source third_party/dng_sdk/UPSTREAM.json "$notices/Adobe/"
fi
for dependency in ctl glfw imgui; do
  mkdir -p "$notices/$dependency"
  cp "$BUILD_DIR/_deps/$dependency-src"/LICENSE* "$notices/$dependency/"
done
cp third_party/fontawesome/README.md "$notices/FontAwesome.txt"
cp third_party/fontawesome/LICENSE.txt "$notices/FontAwesome-LICENSE.txt"
cp third_party/openfx/LICENSE.md "$notices/OpenFX.md"
cp third_party/openfx/Support/LICENSE "$notices/OpenFX-Support.txt"
cp third_party/tinyexr/tinyexr.h "$notices/TinyEXR.h"
for formula in jpeg-turbo libomp libtiff little-cms2 openexr imath libdeflate openjph webp xz zstd; do
  root="$(realpath "$(brew --prefix "$formula")")"
  mkdir -p "$notices/$formula"
  find "$root" -maxdepth 2 -type f \( -iname 'LICENSE*' -o -iname 'LICENCE*' -o -iname 'COPYING*' -o -iname 'COPYRIGHT*' -o -iname 'README.ijg' \) \
    -exec cp {} "$notices/$formula/" \;
  if [ -z "$(ls -A "$notices/$formula")" ]; then
    echo "Missing licence notice for $formula" >&2
    exit 1
  fi
done
for lib in "$app"/Contents/Frameworks/*.dylib; do
  codesign --force --sign - "$lib"
done
codesign --force --sign - "$app/Contents/PlugIns/OfxRawHost.ofx.bundle"
codesign --force --sign - "$app"
codesign --verify --deep --strict --verbose=2 "$app"

archive="$BUILD_DIR/$PACKAGE_NAME.zip"
ditto -c -k --sequesterRsrc --keepParent "$app" "$archive"

# Inspect and run the final ZIP from outside the checkout with a system-only PATH.
scratch="$(mktemp -d "${RUNNER_TEMP}/rawnode-package.XXXXXX")"
trap 'rm -rf "$scratch"' EXIT
ditto -x -k "$archive" "$scratch"
app="$scratch/RawNode.app"
binary="$app/Contents/MacOS/RawNode"
crop="$app/Contents/PlugIns/OfxRawHost.ofx.bundle/Contents/MacOS/OfxRawHost.ofx"
notices="$app/Contents/Resources/RawDependencyNotices"
plist="$app/Contents/Info.plist"
test -x "$binary"
test "$(/usr/libexec/PlistBuddy -c 'Print :CFBundleExecutable' "$plist")" = RawNode
test "$(/usr/libexec/PlistBuddy -c 'Print :CFBundleIdentifier' "$plist")" = com.ryancara.rawnode
test "$(/usr/libexec/PlistBuddy -c 'Print :CFBundleName' "$plist")" = RawNode
test "$(/usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' "$plist")" = "$VERSION"
test "$(/usr/libexec/PlistBuddy -c 'Print :LSMinimumSystemVersion' "$plist")" = "$MACOSX_DEPLOYMENT_TARGET"
test -s "$app/Contents/Resources/AppIcon.icns"
test -s "$app/Contents/Resources/fa-solid-900.ttf"
test -s "$notices/LibRaw/LICENSE.CDDL"
test -s "$notices/Source.txt"
test -s "$notices/DISTRIBUTION_NOTICES.md"
if [ "$JXL" = ON ]; then
  test -s "$notices/Adobe/LICENSE.txt"
  test -s "$notices/Adobe/THIRD_PARTY_NOTICES.md"
  test -s "$notices/Adobe/source/dng_fingerprint.cpp"
  test -s "$notices/Adobe/source/dng_lossless_jpeg_shared.cpp"
  test -s "$notices/libjxl/LICENSE"
else
  test ! -e "$notices/Adobe"
fi
codesign --verify --deep --strict --verbose=2 "$app"
test -f "$app/Contents/Frameworks/libomp.dylib"

for file in "$binary" "$crop" "$app"/Contents/Frameworks/*.dylib; do
  test "$(lipo -archs "$file")" = "$ARCHS"
  # A prebuilt dependency must not raise the minimum OS advertised by the app.
  otool -l "$file" | awk -v target="$MACOSX_DEPLOYMENT_TARGET" '
    function version(s, a) { split(s, a, "."); return 1000000*a[1] + 1000*a[2] + a[3] }
    $1 == "cmd" { command = $2 }
    (command == "LC_BUILD_VERSION" && $1 == "minos") ||
    (command == "LC_VERSION_MIN_MACOSX" && $1 == "version") {
      print "Minimum macOS version:", $2
      found = 1
      if (version($2) > version(target)) incompatible = 1
    }
    END { exit (!found || incompatible) }
  ' || { echo "Incompatible deployment target in $file" >&2; exit 1; }
  # All load commands must resolve to macOS system libraries or this bundle.
  dependencies="$(otool -L "$file")"
  printf '%s\n' "$dependencies"
  while IFS= read -r dependency; do
    case "$dependency" in
      /usr/lib/*|/System/Library/*) ;;
      "$bundle_path"*) test -f "$app/Contents/Frameworks/${dependency#"$bundle_path"}" ;;
      *) echo "Unbundled library dependency in $file: $dependency" >&2; exit 1 ;;
    esac
  done < <(printf '%s\n' "$dependencies" | awk 'NR > 1 {print $1}')
done

cd "$scratch"
env -i PATH=/usr/bin:/bin:/usr/sbin:/sbin HOME="$scratch" TMPDIR="$scratch" "$binary" --selftest
