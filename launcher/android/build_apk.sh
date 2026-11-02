#!/bin/bash
#
# The Android app, built on a Mac or a Linux box with the Android SDK's own
# tools and no Gradle: SDL2 and the engine (libmain.so) with the NDK, then the
# APK with aapt2, javac, d8, zipalign and apksigner.
#
#   launcher/android/build_apk.sh <android sdk> <SDL2 source> <generated code> <version> [out dir]
#
#   <android sdk>     with ndk/, build-tools/, platforms/ (the newest of each is used)
#   <SDL2 source>     SDL 2's source tree (2.32.10; 2.30.0 does not build with NDK r28 and
#                     later): its CMake build, and its Java side
#                     (android-project/.../org/libsdl/app), which the app is built on
#   <generated code>  port/recomp/gen, the game's code (see the README)
#
# JAVA_HOME is a JDK 17 or later. JSRF_KEYSTORE and JSRF_KEYSTORE_PASS (a file
# holding the password) sign a release; without them a throwaway key is made
# in the work folder, which is fine for testing -- but Android only installs
# an update over an app signed with the same key.
#
# Writes <out dir>/JSRF-Unleashed-android-arm64.apk; the work is kept in
# <out dir>/android-work.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
repo="$(cd "$here/../.." && pwd)"
SDK="${1:?the Android SDK folder}"
SDLSRC="${2:?the SDL2 source folder}"
GEN="${3:?the generated code folder}"
VER="${4:?the version}"
OUT="${5:-$PWD}"
: "${JAVA_HOME:?set JAVA_HOME to a JDK 17 or later}"
export PATH="$JAVA_HOME/bin:$PATH"

API=29          # Android 10: the oldest the app runs on
TARGET=36       # the SDK the app is built against
ABI=arm64-v8a   # 64-bit ARM only
NDK="$(ls -d "$SDK"/ndk/* | sort -V | tail -1)"
BT="$(ls -d "$SDK"/build-tools/* | sort -V | tail -1)"
JAR="$(ls -d "$SDK"/platforms/android-* | sort -V | tail -1)/android.jar"
TC="$NDK/build/cmake/android.toolchain.cmake"
STRIP="$(ls "$NDK"/toolchains/llvm/prebuilt/*/bin/llvm-strip | head -1)"
W="$OUT/android-work"
mkdir -p "$W"
JOBS="$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)"
if command -v ninja >/dev/null 2>&1; then GENERATOR=Ninja; else GENERATOR="Unix Makefiles"; fi
read -r va vb vc _ <<<"$(printf '%s' "$VER" | sed -n 's/^\([0-9]*\)\.\([0-9]*\)\.\([0-9]*\).*/\1 \2 \3/p') 0 0 0"
VCODE=$(( va * 10000 + vb * 100 + vc )); [ "$VCODE" -gt 0 ] || VCODE=1
echo "NDK $(basename "$NDK"), build-tools $(basename "$BT"), $(basename "$(dirname "$JAR")"); version $VER ($VCODE)"

echo "== SDL2"
cmake -S "$SDLSRC" -B "$W/sdl" -G "$GENERATOR" -DCMAKE_TOOLCHAIN_FILE="$TC" \
      -DANDROID_ABI=$ABI -DANDROID_PLATFORM=android-$API -DCMAKE_BUILD_TYPE=Release \
      -DSDL_STATIC=OFF -DSDL_TEST=OFF -DSDL_TESTS=OFF \
      -DCMAKE_INSTALL_PREFIX="$W/sdl-install" > "$W/sdl-configure.log"
cmake --build "$W/sdl" -j"$JOBS" > "$W/sdl-build.log"
cmake --install "$W/sdl" > /dev/null

echo "== the engine (libmain.so)"
cmake -S "$repo/port" -B "$W/engine" -G "$GENERATOR" -DCMAKE_TOOLCHAIN_FILE="$TC" \
      -DANDROID_ABI=$ABI -DANDROID_PLATFORM=android-$API -DCMAKE_BUILD_TYPE=Release \
      -DJSRF_VERSION="$VER" -DJSRF_GEN_DIR="$GEN" \
      -DSDL2_DIR="$W/sdl-install/lib/cmake/SDL2" > "$W/engine-configure.log"
# Past the first failure, so that one build lists every error.
if [ "$GENERATOR" = Ninja ]; then KEEP="-k 0"; else KEEP="-k"; fi
cmake --build "$W/engine" -j"$JOBS" --target jsrf_recomp -- $KEEP > "$W/engine-build.log" 2>&1 \
    || { grep -E 'error:|undefined symbol|undefined reference' "$W/engine-build.log" | sort -u | head -60; exit 1; }

echo "== resources, code, package"
R="$W/res"
rm -rf "$R" "$W/assets" "$W/gen" "$W/classes" "$W/dex" "$W/apk"
cp -R "$here/res" "$R"
ICON="$repo/launcher/icon/AppIcon.iconset/icon_256x256.png"
for d in mdpi:48 hdpi:72 xhdpi:96 xxhdpi:144 xxxhdpi:192; do
    mkdir -p "$R/mipmap-${d%%:*}"
    out="$R/mipmap-${d%%:*}/ic_launcher.png"
    if command -v sips >/dev/null 2>&1; then
        sips -z "${d##*:}" "${d##*:}" "$ICON" --out "$out" > /dev/null
    else
        python3 -c "import sys;from PIL import Image;Image.open(sys.argv[1]).resize((int(sys.argv[2]),)*2,Image.LANCZOS).save(sys.argv[3])" \
            "$ICON" "${d##*:}" "$out" 2>/dev/null || cp "$ICON" "$out"
    fi
done
mkdir -p "$W/assets/licences"
cp "$repo/LICENSE" "$W/assets/licences/LICENSE.txt"
cp "$repo/NOTICE" "$W/assets/licences/NOTICE.txt"
cp "$repo/LICENSES/LGPL-2.1.txt" "$W/assets/licences/LGPL-2.1.txt"
cp "$repo/LICENSES/MIT-JSRF-Recomp.txt" "$W/assets/licences/MIT-JSRF-Recomp.txt"
cp "$SDLSRC/LICENSE.txt" "$W/assets/licences/SDL2-LICENSE.txt"

mkdir -p "$W/gen" "$W/classes" "$W/dex" "$W/apk/lib/$ABI"
"$BT/aapt2" compile --dir "$R" -o "$W/res.zip"
"$BT/aapt2" link -o "$W/base.apk" -I "$JAR" --manifest "$here/AndroidManifest.xml" \
    --min-sdk-version $API --target-sdk-version $TARGET \
    --version-code "$VCODE" --version-name "$VER" \
    -A "$W/assets" --java "$W/gen" "$W/res.zip"
find "$SDLSRC/android-project/app/src/main/java" "$here/java" "$W/gen" -name '*.java' > "$W/sources.txt"
javac -source 11 -target 11 -nowarn -Xlint:-options -encoding UTF-8 \
      -cp "$JAR" -d "$W/classes" @"$W/sources.txt"
find "$W/classes" -name '*.class' > "$W/classes.txt"
"$BT/d8" --release --min-api $API --lib "$JAR" --output "$W/dex" $(cat "$W/classes.txt")

cp "$W/base.apk" "$W/unsigned.apk"
(cd "$W/dex" && zip -q -X "$W/unsigned.apk" classes.dex)
cp "$W/sdl-install/lib/libSDL2.so" "$W/engine/libmain.so" "$W/apk/lib/$ABI/"
"$STRIP" --strip-unneeded "$W/apk/lib/$ABI/libSDL2.so" "$W/apk/lib/$ABI/libmain.so"
(cd "$W/apk" && zip -q -X -r "$W/unsigned.apk" lib)
"$BT/zipalign" -P 16 -f 4 "$W/unsigned.apk" "$W/aligned.apk"

KS="${JSRF_KEYSTORE:-$W/test.keystore}"
PASS="${JSRF_KEYSTORE_PASS:-$W/test.keystore.pass}"
if [ ! -f "$KS" ]; then
    [ -z "${JSRF_KEYSTORE:-}" ] || { echo "no keystore at $KS"; exit 1; }
    head -c 24 /dev/urandom | base64 | tr -d '\n' > "$PASS"
    keytool -genkeypair -keystore "$KS" -alias jsrf -keyalg RSA -keysize 4096 -validity 36500 \
            -dname "CN=JSRF Unleashed (test)" -storepass:file "$PASS" -keypass:file "$PASS" > /dev/null 2>&1
fi
APK="$OUT/JSRF-Unleashed-android-arm64.apk"
# The key's password is the keystore's (a PKCS12 keystore has only the one);
# naming the same file for --key-pass as well would read its second line.
"$BT/apksigner" sign --ks "$KS" --ks-key-alias jsrf --ks-pass "file:$PASS" \
    --out "$APK" "$W/aligned.apk" < /dev/null
"$BT/apksigner" verify --print-certs "$APK" | sed -n '1,2p'
echo "packaged: $APK ($(wc -c < "$APK" | tr -d ' ') bytes)"
