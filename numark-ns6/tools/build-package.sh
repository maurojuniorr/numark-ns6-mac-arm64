#!/bin/sh
set -eu

project_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_dir=${1:-/private/tmp/ns6-modular-build}
version=$(sed -n 's/^project(NumarkNS6 VERSION \([^ ]*\).*/\1/p' "$project_dir/CMakeLists.txt")
package_path=${2:-/private/tmp/NumarkNS6-${version}-experimental.pkg}
stage=$(mktemp -d /private/tmp/ns6-package.XXXXXX)
app="$build_dir/Numark NS6 Status.app"
trap 'rm -rf "$stage"' EXIT

# Optimize the installed driver while keeping assert-based checks active.
cmake -S "$project_dir" -B "$build_dir" -DCMAKE_BUILD_TYPE= -DCMAKE_CXX_FLAGS=-O2
cmake --build "$build_dir" --parallel
ctest --test-dir "$build_dir" --output-on-failure

mkdir -p "$stage/Library/Audio/Plug-Ins/HAL" \
  "$stage/Library/Application Support/Numark NS6" \
  "$stage/Library/LaunchAgents" \
  "$app/Contents/MacOS" "$app/Contents/Resources"
cp -R "$build_dir/NumarkNS6.driver" "$stage/Library/Audio/Plug-Ins/HAL/"
cp "$build_dir/NumarkNS6MIDIBridge" "$stage/Library/Application Support/Numark NS6/"
cp "$project_dir/coreaudio/io.github.maurojuniorr.numark-ns6-midi-bridge.plist" \
  "$stage/Library/LaunchAgents/"
CLANG_MODULE_CACHE_PATH="$build_dir/clang-module-cache" \
SWIFT_MODULE_CACHE_PATH="$build_dir/swift-module-cache" \
/usr/bin/swiftc -O -target arm64-apple-macosx15.0 \
  "$project_dir/status-app/NS6Status.swift" \
  -framework AppKit -framework CoreAudio -framework IOKit \
  -o "$app/Contents/MacOS/NumarkNS6Status"
cp "$project_dir/status-app/Info.plist" "$app/Contents/"
cp "$project_dir/status-app/NS6Status.icns" "$app/Contents/Resources/"
/usr/libexec/PlistBuddy -c "Set :CFBundleShortVersionString $version" \
  "$app/Contents/Info.plist"
/usr/libexec/PlistBuddy -c "Set :CFBundleVersion 2${version##*.}" \
  "$app/Contents/Info.plist"
/usr/bin/codesign --force --sign - "$app"
/usr/bin/tar -czf "$stage/Library/Application Support/Numark NS6/NumarkNS6Status.tar.gz" \
  -C "$build_dir" "Numark NS6 Status.app"
chmod 755 "$project_dir/coreaudio/package-scripts/preinstall" \
  "$project_dir/coreaudio/package-scripts/postinstall"
/usr/bin/pkgbuild --root "$stage" --identifier io.github.maurojuniorr.NumarkNS6 \
  --version "$version" --scripts "$project_dir/coreaudio/package-scripts" \
  --install-location / "$package_path"
printf 'Package: %s\n' "$package_path"
