#!/bin/bash
#
# Package the Windows build: "JSRF Unleashed.exe" (the launcher, built here
# with the same MinGW toolchain) beside engine\jsrf_recomp.exe and the DLLs
# it needs, zipped.
#
#   launcher/windows/package.sh <jsrf_recomp.exe> <llvm-mingw dir> <dll dir> <version> [out dir]
#
# <dll dir> holds SDL2.dll and libepoxy-0.dll; libwinpthread-1.dll comes from
# the toolchain. Licence texts are taken from LICENCES_DIR when it is set
# (SDL2-COPYING.txt, libepoxy-COPYING.txt, winpthreads-COPYING.txt).
#
# How the release is built (from a Linux box; nothing here needs Windows):
#   llvm-mingw 20241217 (LLVM 19.1.6, UCRT), SDL2 2.30.0 (the MinGW
#   development package), libepoxy 1.5.10 (meson, cross-built with the same
#   toolchain), both installed into one prefix, then
#     cmake -S port -B build-win -G Ninja \
#           -DCMAKE_TOOLCHAIN_FILE="$PWD/launcher/windows/llvm-mingw.cmake" \
#           -DLLVM_MINGW=<llvm-mingw dir> -DWIN_DEPS=<that prefix> \
#           -DCMAKE_BUILD_TYPE=Release -DJSRF_VERSION=<version>
#     ninja -C build-win jsrf_recomp
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
repo="$(cd "$here/../.." && pwd)"
engine="${1:?the jsrf_recomp.exe to package}"
tc="${2:?the llvm-mingw toolchain directory}"
dlls="${3:?the folder with SDL2.dll and libepoxy-0.dll}"
version="${4:?the version}"
out="${5:-$PWD}"
name=JSRF-Unleashed-windows-x86_64
stage="$(mktemp -d)"
dir="$stage/$name"
trap 'rm -rf "$stage"' EXIT

mkdir -p "$dir/engine" "$dir/licences"

# The launcher, with the same icon, manifest and version information.
num="$(printf '%s' "$version" | sed -n 's/^\([0-9]*\)\.\([0-9]*\)\.\([0-9]*\).*/\1,\2,\3,0/p')"
[ -n "$num" ] || num=0,0,0,0
printf '#define JSRF_VER_NUM %s\n#define JSRF_VER_STR "%s"\n' "$num" "$version" > "$stage/jsrf_version.h"
"$tc/bin/x86_64-w64-mingw32-windres" -I "$stage" -I "$here" "$here/resources.rc" -O coff -o "$stage/launcher.res"
"$tc/bin/x86_64-w64-mingw32-clang" -O2 -s -Wall -Wextra -municode -mwindows \
    -I "$repo/port/ios" -I "$repo/port" -o "$dir/JSRF Unleashed.exe" \
    "$here/jsrf_launcher.c" "$repo/port/ios/xiso.c" "$repo/port/xbe_check.c" "$stage/launcher.res" \
    -lshell32 -lole32 -luuid -lcomctl32

# The engine and what it loads.
install -m 755 "$engine" "$dir/engine/jsrf_recomp.exe"
"$tc/bin/llvm-strip" --strip-debug "$dir/engine/jsrf_recomp.exe"
install -m 644 "$dlls/SDL2.dll" "$dlls/libepoxy-0.dll" "$dir/engine/"
install -m 644 "$tc/x86_64-w64-mingw32/bin/libwinpthread-1.dll" "$dir/engine/"
"$tc/bin/llvm-strip" --strip-debug "$dir/engine/libepoxy-0.dll" "$dir/engine/libwinpthread-1.dll"

printf '%s\n' "$version" > "$dir/VERSION"
sed 's/$/\r/' "$here/README.txt" > "$dir/README.txt"
for f in LICENSE NOTICE; do sed 's/$/\r/' "$repo/$f" > "$dir/licences/$f.txt"; done
sed 's/$/\r/' "$repo/LICENSES/LGPL-2.1.txt" > "$dir/licences/LGPL-2.1.txt"
sed 's/$/\r/' "$repo/LICENSES/MIT-JSRF-Recomp.txt" > "$dir/licences/MIT-JSRF-Recomp.txt"
if [ -n "${LICENCES_DIR:-}" ]; then
    for f in SDL2-COPYING.txt libepoxy-COPYING.txt winpthreads-COPYING.txt; do
        [ -f "$LICENCES_DIR/$f" ] && sed 's/$/\r/' "$LICENCES_DIR/$f" > "$dir/licences/$f"
    done
fi

echo "launcher needs:"; "$tc/bin/llvm-objdump" -p "$dir/JSRF Unleashed.exe" | sed -n 's/^ *DLL Name: */  /p' | grep -v '^  api-ms-win-crt' | tr '\n' ' '; echo
echo "engine needs:";   "$tc/bin/llvm-objdump" -p "$dir/engine/jsrf_recomp.exe" | sed -n 's/^ *DLL Name: */  /p' | grep -v '^  api-ms-win-crt' | tr '\n' ' '; echo

python3 - "$stage" "$name" "$out/$name.zip" <<'PY'
import os, sys, zipfile
stage, name, dest = sys.argv[1:4]
with zipfile.ZipFile(dest, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
    for root, dirs, files in os.walk(os.path.join(stage, name)):
        dirs.sort()
        for f in sorted(files):
            p = os.path.join(root, f)
            z.write(p, os.path.relpath(p, stage))
print("packaged:", dest, os.path.getsize(dest), "bytes")
PY
