#!/bin/bash
#
# Assemble "JSRF Unleashed.app" around the recompiled binary.
#
# The app is the launcher; the game engine rides inside it. Run this after any
# rebuild of jsrf_recomp -- the engine is copied in, not linked, so the app
# keeps running the version it was built with until this is run again.
#
# Usage:  launcher/build_app.sh [path/to/jsrf_recomp] [output dir]
#
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
engine="${1:-$here/../../build-arm64/jsrf_recomp}"
outdir="${2:-$here/..}"
app="$outdir/JSRF Unleashed.app"

[ -x "$engine" ] || { echo "no engine at $engine" >&2; exit 1; }

rm -rf "$app"
mkdir -p "$app/Contents/MacOS" "$app/Contents/Resources"

cat > "$app/Contents/Info.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleName</key>              <string>JSRF Unleashed</string>
  <key>CFBundleDisplayName</key>       <string>JSRF Unleashed</string>
  <key>CFBundleIdentifier</key>        <string>com.jumatt.jsrf-unleashed</string>
  <key>CFBundleVersion</key>           <string>1</string>
  <key>CFBundleShortVersionString</key><string>0.1</string>
  <key>CFBundlePackageType</key>       <string>APPL</string>
  <key>CFBundleExecutable</key>        <string>JSRF Unleashed</string>
  <key>CFBundleIconFile</key>          <string>AppIcon</string>
  <key>LSMinimumSystemVersion</key>    <string>12.0</string>
  <key>NSHighResolutionCapable</key>   <true/>
  <key>NSSupportsAutomaticGraphicsSwitching</key><true/>
</dict>
</plist>
PLIST

echo "compiling launcher…"
swiftc -O -target arm64-apple-macos12 \
       -o "$app/Contents/MacOS/JSRF Unleashed" \
       "$here/JSRFLauncher.swift"

cp "$engine" "$app/Contents/MacOS/jsrf_recomp"
chmod +x "$app/Contents/MacOS/jsrf_recomp"

# The engine loads shaders and the like from beside itself in some builds; copy
# anything that sits next to it and is not another executable.
for extra in "$(dirname "$engine")"/*.metallib "$(dirname "$engine")"/*.glsl; do
  [ -e "$extra" ] && cp "$extra" "$app/Contents/MacOS/" || true
done

# The icon is kept as an .iconset of PNGs rather than a built .icns, so it can
# be read and changed in the repository. Build it here if it is not already.
if [ ! -f "$here/AppIcon.icns" ] && [ -d "$here/icon/AppIcon.iconset" ]; then
  iconutil -c icns "$here/icon/AppIcon.iconset" -o "$here/AppIcon.icns" 2>/dev/null || true
fi
if [ -f "$here/AppIcon.icns" ]; then
  cp "$here/AppIcon.icns" "$app/Contents/Resources/AppIcon.icns"
else
  echo "note: no icon built (needs iconutil); the app gets the generic one"
fi

# Ad-hoc signature. Without one, Gatekeeper kills the app on first launch with
# no message anyone can act on; with one it is a local app the user allows in
# System Settings the usual way.
codesign --force --deep --sign - "$app" >/dev/null 2>&1 || \
  echo "note: codesign failed; the app still runs after Right-click > Open"

echo "built: $app"
