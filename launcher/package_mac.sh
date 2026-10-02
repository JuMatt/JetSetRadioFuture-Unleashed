#!/bin/bash
#
# Package the Mac build: JSRF Unleashed.app, zipped, as
# JSRF-Unleashed-macOS-arm64.zip -- the release asset.
#
#   launcher/package_mac.sh <jsrf_recomp> [out dir]
#
# Build the engine from the repository at the release tag (port/CMakeLists.txt)
# so it carries that version; build_app.sh gives the app the same one, puts the
# licences in its Resources and signs it ad hoc.
#
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
engine="${1:?the jsrf_recomp to package}"
out="${2:-$PWD}"
name=JSRF-Unleashed-macOS-arm64.zip
stage="$(mktemp -d)"
trap 'rm -rf "$stage"' EXIT

bash "$here/build_app.sh" "$engine" "$stage"
codesign --verify --deep --strict "$stage/JSRF Unleashed.app"
( cd "$stage" && ditto -c -k --sequesterRsrc --keepParent "JSRF Unleashed.app" "$name" )
mv "$stage/$name" "$out/$name"
echo "packaged: $out/$name"
