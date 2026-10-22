#!/bin/sh
set -eu

project_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_dir=${1:-/private/tmp/ns6-modular-build}
version=$(sed -n 's/^project(NumarkNS6 VERSION \([^ ]*\).*/\1/p' "$project_dir/CMakeLists.txt")
package_path=${2:-/private/tmp/NumarkNS6-${version}-experimental.pkg}
stage=$(mktemp -d /private/tmp/ns6-package.XXXXXX)
trap 'rm -rf "$stage"' EXIT

cmake -S "$project_dir" -B "$build_dir"
cmake --build "$build_dir" --parallel
ctest --test-dir "$build_dir" --output-on-failure

mkdir -p "$stage/Library/Audio/Plug-Ins/HAL"
cp -R "$build_dir/NumarkNS6.driver" "$stage/Library/Audio/Plug-Ins/HAL/"
/usr/bin/pkgbuild --root "$stage" --identifier io.github.maurojuniorr.NumarkNS6 \
  --version "$version" --scripts "$project_dir/coreaudio/package-scripts" \
  --install-location / "$package_path"
printf 'Package: %s\n' "$package_path"
