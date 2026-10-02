#!/bin/bash
#
# Package the Linux build: a folder with the engine, the launcher, a portable
# SDL2 and the licences, as JSRF-Unleashed-linux-x86_64.tar.gz.
#
#   launcher/linux/package.sh <jsrf_recomp> <libSDL2-2.0.so.0> [out dir]
#
# How the release build is made (Ubuntu 22.04, so glibc 2.35, and the result
# runs on glibc 2.34 or later), from the repository at the release tag:
#
#   CC=clang cmake -S port -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
#       -DNV2A_GL_CONTEXT=sdl -DOpenGL_GL_PREFERENCE=LEGACY \
#       -DCMAKE_EXE_LINKER_FLAGS=-Wl,--as-needed
#   cmake --build build
#
# --as-needed and the legacy libGL leave the engine needing only libc, libm,
# libGL.so.1 and SDL2. The SDL2 shipped beside it is SDL 2.30 built from
# source with every backend loaded at run time (SDL_X11_SHARED,
# SDL_WAYLAND_SHARED, SDL_PULSEAUDIO_SHARED, SDL_PIPEWIRE_SHARED,
# SDL_ALSA_SHARED, SDL_KMSDRM_SHARED, SDL_LIBSAMPLERATE_SHARED; tests off), so
# it needs nothing but libc and libm either -- a distribution's own SDL2 links
# its backends directly and would not load on a system missing one of them.
#
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
repo="$(cd "$here/../.." && pwd)"
engine="${1:?the jsrf_recomp to package}"
sdl="${2:?a portable libSDL2-2.0.so.0}"
out="${3:-$PWD}"
name=JSRF-Unleashed-linux-x86_64
stage="$(mktemp -d)"
dir="$stage/$name"

mkdir -p "$dir/bin" "$dir/lib" "$dir/share" "$dir/licences"
install -m 755 "$here/jsrf-unleashed" "$dir/jsrf-unleashed"
install -m 644 "$here/README.txt" "$dir/README.txt"
install -m 755 "$engine" "$dir/bin/jsrf_recomp"
# The version is the engine's own (port/CMakeLists.txt builds it in from the
# release tag); the launcher's --version reads it from here.
"$engine" --version > "$dir/VERSION"
echo "version: $(cat "$dir/VERSION")"
install -m 644 "$sdl" "$dir/lib/libSDL2-2.0.so.0"
strip --strip-unneeded "$dir/lib/libSDL2-2.0.so.0" 2>/dev/null || true
install -m 644 "$repo/launcher/icon/AppIcon.iconset/icon_256x256.png" "$dir/share/jsrf-unleashed.png"
install -m 644 "$repo/LICENSE" "$dir/licences/LICENSE.txt"
install -m 644 "$repo/NOTICE" "$dir/licences/NOTICE.txt"
install -m 644 "$repo/LICENSES/LGPL-2.1.txt" "$dir/licences/LGPL-2.1.txt"
[ -n "${SDL_LICENSE:-}" ] && install -m 644 "$SDL_LICENSE" "$dir/licences/SDL2-LICENSE.txt"

# What the engine needs from the system, said once at packaging time.
echo "engine needs:"; objdump -p "$dir/bin/jsrf_recomp" | sed -n 's/^ *NEEDED */  /p'
echo "newest glibc symbol:"; objdump -T "$dir/bin/jsrf_recomp" | grep -o 'GLIBC_[0-9.]*' | sort -t. -k2,2n -k3,3n | uniq | tail -n 1 | sed 's/^/  /'

tar -C "$stage" -czf "$out/$name.tar.gz" "$name"
rm -rf "$stage"
echo "packaged: $out/$name.tar.gz"
