#!/bin/bash
#
# Package the iPhone / iPad build: JSRF Unleashed.app around the engine, in
# an .ipa for sideloading -- JSRF-Unleashed-iOS.ipa, the release asset.
#
#   launcher/ios/package_ios.sh <jsrf_recomp> <version> [out dir] [--simulator]
#
# The engine is built from the repository for iOS:
#
#   cmake -S port -B build-ios -DCMAKE_BUILD_TYPE=Release -DCMAKE_SYSTEM_NAME=iOS \
#         -DCMAKE_OSX_SYSROOT=iphoneos -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0
#   cmake --build build-ios
#
# (CMAKE_OSX_SYSROOT=iphonesimulator for the Simulator; --simulator then
# leaves the .app in the out dir instead of making an .ipa.)
#
# The signature is ad hoc -- no certificate, so no name and no team in it.
# iOS runs only what a certificate it trusts has signed, so the app is meant
# to be installed with a sideloading tool (AltStore, SideStore, Sideloadly),
# which signs it with the installer's own Apple ID on the way in.
#
# No entitlements by default: a free Apple ID cannot have the restricted ones,
# and an app asking for them is one more thing a tool can refuse. Recent
# iPhones give an app far more than the 4 GB of address space the guest's
# memory map needs; for an older one, JSRF_IOS_ENTITLEMENTS=1 signs with
# entitlements.plist (the extended address space and memory limit), for an
# Apple ID or a TrollStore install that can have them. The Simulator refuses
# restricted entitlements outright, so it never gets them.
#
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
repo="$(cd "$here/../.." && pwd)"
engine="${1:?the jsrf_recomp built for iOS}"
version="${2:?the version, e.g. 0.2.0}"
out="${3:-$PWD}"
sim=0
[ "${4:-}" = "--simulator" ] && sim=1

short="$(printf '%s' "$version" | sed -nE 's/^([0-9]+\.[0-9]+\.[0-9]+).*/\1/p')"
[ -n "$short" ] || short=0.0.0
since="$(printf '%s' "$version" | sed -nE 's/^[0-9]+\.[0-9]+\.[0-9]+-([0-9]+)-g.*/\1/p')"
bundle_version="$short${since:+.$since}"
if [ $sim = 1 ]; then platform=iPhoneSimulator; dt=iphonesimulator; else platform=iPhoneOS; dt=iphoneos; fi
sdk="$(xcrun --sdk $dt --show-sdk-version 2>/dev/null || echo 0)"

stage="$(mktemp -d)"
trap 'rm -rf "$stage"' EXIT
app="$stage/Payload/JSRF Unleashed.app"
mkdir -p "$app"

install -m 755 "$engine" "$app/jsrf_recomp"
printf 'APPL????' > "$app/PkgInfo"

# Icons, from the Mac's iconset: the sizes an iPhone and an iPad home screen
# ask for, named the way CFBundleIconFiles finds them.
src="$repo/launcher/icon/AppIcon.iconset/icon_512x512@2x.png"
for spec in "AppIcon60x60@2x 120" "AppIcon60x60@3x 180" "AppIcon76x76@2x~ipad 152" \
            "AppIcon83.5x83.5@2x~ipad 167" "AppIcon76x76~ipad 76"; do
    set -- $spec
    sips -s format png -z "$2" "$2" "$src" --out "$app/$1.png" >/dev/null
done

for f in LICENSE NOTICE LICENSES/LGPL-2.1.txt LICENSES/MIT-JSRF-Recomp.txt; do
    [ -f "$repo/$f" ] && cp "$repo/$f" "$app/$(basename "${f%.txt}").txt"
done

cat > "$app/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleDevelopmentRegion</key>   <string>en</string>
  <key>CFBundleName</key>                <string>JSRF Unleashed</string>
  <key>CFBundleDisplayName</key>         <string>JSRF Unleashed</string>
  <key>CFBundleIdentifier</key>          <string>com.jumatt.jsrf-unleashed</string>
  <key>CFBundleExecutable</key>          <string>jsrf_recomp</string>
  <key>CFBundleInfoDictionaryVersion</key><string>6.0</string>
  <key>CFBundlePackageType</key>         <string>APPL</string>
  <key>CFBundleShortVersionString</key>  <string>$short</string>
  <key>CFBundleVersion</key>             <string>$bundle_version</string>
  <key>JSRFVersion</key>                 <string>$version</string>
  <key>CFBundleSupportedPlatforms</key>  <array><string>$platform</string></array>
  <key>DTPlatformName</key>              <string>$dt</string>
  <key>DTSDKName</key>                   <string>$dt$sdk</string>
  <key>MinimumOSVersion</key>            <string>15.0</string>
  <key>LSRequiresIPhoneOS</key>          <true/>
  <key>UIDeviceFamily</key>              <array><integer>1</integer><integer>2</integer></array>
  <key>UIRequiredDeviceCapabilities</key><array><string>arm64</string><string>metal</string></array>
  <key>UISupportedInterfaceOrientations</key>
  <array><string>UIInterfaceOrientationLandscapeRight</string><string>UIInterfaceOrientationLandscapeLeft</string></array>
  <key>UISupportedInterfaceOrientations~ipad</key>
  <array><string>UIInterfaceOrientationLandscapeRight</string><string>UIInterfaceOrientationLandscapeLeft</string></array>
  <key>UIRequiresFullScreen</key>        <true/>
  <key>UIStatusBarHidden</key>           <true/>
  <key>UIViewControllerBasedStatusBarAppearance</key><true/>
  <key>UILaunchScreen</key>              <dict/>
  <key>UIFileSharingEnabled</key>        <true/>
  <key>LSSupportsOpeningDocumentsInPlace</key><true/>
  <key>GCSupportsControllerUserInteraction</key><true/>
  <key>GCSupportedGameControllers</key>
  <array><dict><key>ProfileName</key><string>ExtendedGamepad</string></dict></array>
  <key>ITSAppUsesNonExemptEncryption</key><false/>
  <key>CFBundleIcons</key>
  <dict><key>CFBundlePrimaryIcon</key><dict>
    <key>CFBundleIconFiles</key><array><string>AppIcon60x60</string></array>
  </dict></dict>
  <key>CFBundleIcons~ipad</key>
  <dict><key>CFBundlePrimaryIcon</key><dict>
    <key>CFBundleIconFiles</key><array><string>AppIcon60x60</string><string>AppIcon76x76</string><string>AppIcon83.5x83.5</string></array>
  </dict></dict>
  <key>UIApplicationSceneManifest</key>
  <dict>
    <key>UIApplicationSupportsMultipleScenes</key><false/>
    <key>UISceneConfigurations</key>
    <dict><key>UIWindowSceneSessionRoleApplication</key>
      <array><dict>
        <key>UISceneConfigurationName</key><string>Default</string>
        <key>UISceneDelegateClassName</key><string>NvSceneDelegate</string>
      </dict></array>
    </dict>
  </dict>
</dict>
</plist>
PLIST
plutil -lint "$app/Info.plist" >/dev/null

# Ad hoc: "-" is no identity at all.
ent=()
[ $sim = 0 ] && [ "${JSRF_IOS_ENTITLEMENTS:-0}" = 1 ] && ent=(--entitlements "$here/entitlements.plist")
codesign --force --sign - --timestamp=none ${ent[@]+"${ent[@]}"} "$app"   # (bash 3.2: no empty "${a[@]}" under set -u)
codesign --verify --strict "$app"
echo "version: $version (CFBundleShortVersionString $short, build $bundle_version), $platform $sdk"

mkdir -p "$out"
if [ $sim = 1 ]; then
    rm -rf "$out/JSRF Unleashed.app"
    ditto "$app" "$out/JSRF Unleashed.app"
    echo "built: $out/JSRF Unleashed.app"
else
    name=JSRF-Unleashed-iOS.ipa
    ( cd "$stage" && rm -f "$name" && zip -qry "$name" Payload )
    mv "$stage/$name" "$out/$name"
    echo "packaged: $out/$name"
fi
