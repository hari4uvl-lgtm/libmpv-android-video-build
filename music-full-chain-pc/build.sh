#!/usr/bin/env bash
# PC diagnostic tools only. Never calls the Android build or changes its sources.
set -euo pipefail
export LC_ALL=C

repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo"
target="${TARGET:?Set TARGET=linux or TARGET=windows}"
case "$target" in linux|windows) ;; *) echo 'Unsupported PC target' >&2; exit 2;; esac
build="$repo/full-chain-pc-build-$target"
artifact="$repo/full-chain-pc-artifact"
test ! -e "$build" || { echo 'Use a fresh checkout/build directory' >&2; exit 2; }
test ! -e "$artifact" || { echo 'Artifact destination already exists' >&2; exit 2; }
mkdir -p "$build" "$artifact/provenance" "$artifact/sources"
exec > >(tee "$artifact/build.log") 2>&1

# The manifest is committed with this tool. Assert exact shipping C, not an
# earlier PC adapter or one of the deliberately rejected limiter experiments.
sha256sum -c music-full-chain-pc/SHIPPING_SOURCE_SHA256SUMS
cp music-full-chain-pc/SHIPPING_SOURCE_SHA256SUMS "$artifact/provenance/"
cp buildscripts/patches/af_lumendsp.c buildscripts/patches/af_lumenout.c \
   buildscripts/patches/lumenout_*.c buildscripts/patches/lumenout_*.h "$artifact/sources/"
git rev-parse HEAD > "$artifact/provenance/tool-revision.txt"
git status --porcelain > "$artifact/provenance/tool-working-tree.txt"
cp music-full-chain-pc/build.sh music-full-chain-pc/smoke.py "$artifact/provenance/"

soxr_sha=b111c15fdc8c029989330ff559184198c161100a59312f5dc19ddeb9b5a15889
ffmpeg_revision=ea3d24bbe3c58b171e55fe2151fc7ffaca3ab3d2
curl --fail --location --retry 3 \
  https://deb.debian.org/debian/pool/main/libs/libsoxr/libsoxr_0.1.3.orig.tar.xz \
  -o "$build/soxr-source.tar.xz"
printf '%s  %s\n' "$soxr_sha" "$build/soxr-source.tar.xz" | sha256sum -c -
tar -xJf "$build/soxr-source.tar.xz" -C "$build"
git clone --depth 1 --branch n6.0 https://github.com/FFmpeg/FFmpeg.git "$build/ffmpeg"
test "$(git -C "$build/ffmpeg" rev-parse HEAD)" = "$ffmpeg_revision"
printf 'FFmpeg %s\nSoX archive SHA256 %s\n' "$ffmpeg_revision" "$soxr_sha" \
  > "$artifact/provenance/dependencies.txt"

prefix="$build/prefix"
cross=()
cmake_cross=()
compiler=gcc
suffix=
if [ "$target" = windows ]; then
  compiler=x86_64-w64-mingw32-gcc
  suffix=.exe
  cross=(--enable-cross-compile --target-os=mingw32 --arch=x86_64 \
         --cross-prefix=x86_64-w64-mingw32-)
  # CMAKE_AR/RANLIB are FILEPATH cache entries: a bare name is made relative
  # to the checkout by CMake, unlike the C compiler's PATH lookup.
  cmake_cross=(-DCMAKE_SYSTEM_NAME=Windows -DCMAKE_SYSTEM_PROCESSOR=x86_64 \
    "-DCMAKE_C_COMPILER=$(command -v x86_64-w64-mingw32-gcc)" \
    "-DCMAKE_AR=$(command -v x86_64-w64-mingw32-ar)" \
    "-DCMAKE_RANLIB=$(command -v x86_64-w64-mingw32-ranlib)")
fi
"$compiler" --version > "$artifact/provenance/compiler.txt"
cmake --version > "$artifact/provenance/cmake.txt"
dpkg-query -W gcc gcc-mingw-w64-x86-64 make pkg-config cmake \
  > "$artifact/provenance/host-packages.txt"

# Same source/options as Android's SoX dependency, with host/cross compiler only.
cmake -S "$build/soxr-0.1.3-Source" -B "$build/soxr-build" \
  "${cmake_cross[@]}" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$prefix" \
  -DCMAKE_C_FLAGS=-ffp-contract=off -DBUILD_SHARED_LIBS=OFF \
  -DBUILD_TESTS=OFF -DBUILD_EXAMPLES=OFF -DWITH_OPENMP=OFF \
  -DWITH_LSR_BINDINGS=OFF -DWITH_DEV_TRACE=OFF
cmake --build "$build/soxr-build" --parallel 2
cmake --install "$build/soxr-build"
# Upstream SoX installs pkg-config metadata only when NOT WIN32. Its static
# Windows library/header are already built; describe those actual outputs for
# FFmpeg's pkg-config probe without patching the dependency's source/build.
if [ "$target" = windows ]; then
  python3 - "$prefix" <<'PY'
from pathlib import Path
import re
import sys
prefix = Path(sys.argv[1])
header = (prefix / 'include/soxr.h').read_text()
version = re.search(r'^#define SOXR_THIS_VERSION_STR\s+"([^"]+)"', header, re.M)[1]
assert version == '0.1.3'
directory = prefix / 'lib/pkgconfig'
directory.mkdir(exist_ok=True)
path = directory / 'soxr.pc'
assert not path.exists()
path.write_text('Name: soxr\nDescription: SoX resampler static PC build\n'
                f'Version: {version}\nLibs: -L{prefix}/lib -lsoxr\n'
                f'Cflags: -I{prefix}/include\n')
PY
fi
printf 'Libs.private: -lm\n' >> "$prefix/lib/pkgconfig/soxr.pc"
cp "$build/soxr-build/CMakeCache.txt" "$artifact/provenance/soxr-CMakeCache.txt"
cp "$prefix/lib/pkgconfig/soxr.pc" "$artifact/provenance/"
sha256sum "$prefix/lib/libsoxr.a" > "$artifact/provenance/soxr-library-sha256.txt"

ff="$build/ffmpeg"
cp "$artifact/sources/"* "$ff/libavfilter/"
python3 - "$ff" <<'PY'
from pathlib import Path
import sys
root = Path(sys.argv[1]) / 'libavfilter'
path = root / 'allfilters.c'
text = path.read_text()
anchor = 'extern const AVFilter ff_af_ladspa;'
assert text.count(anchor) == 1
text = text.replace(anchor, anchor + '\nextern const AVFilter ff_af_lumendsp;'
                    '\nextern const AVFilter ff_af_lumenout;')
assert text.index('ff_af_lumenout;') < text.index('#include "libavfilter/filter_list.c"')
path.write_text(text)
with (root / 'Makefile').open('a') as stream:
    stream.write('\nOBJS-$(CONFIG_LUMENDSP_FILTER) += af_lumendsp.o\n')
    stream.write('OBJS-$(CONFIG_LUMENOUT_FILTER) += af_lumenout.o '
                 'lumenout_core.o lumenout_bands.o lumenout_clip.o\n')
PY
cd "$ff"
export PKG_CONFIG_LIBDIR="$prefix/lib/pkgconfig"
unset PKG_CONFIG_PATH
./configure "${cross[@]}" \
  --extra-cflags="-I$prefix/include -ffp-contract=off" \
  --extra-ldflags="-L$prefix/lib -static" --pkg-config=pkg-config \
  --pkg-config-flags=--static --extra-libs=-lm \
  --disable-everything --disable-autodetect --disable-network --disable-doc \
  --disable-asm --disable-debug --disable-ffplay --disable-gpl --disable-nonfree \
  --enable-version3 --enable-libsoxr --enable-static --disable-shared \
  --enable-ffmpeg --enable-ffprobe --enable-protocol=file,pipe \
  --enable-demuxer=wav,flac,mp3,mov,pcm_f64le,pcm_f32le \
  --enable-decoder=pcm_s16le,pcm_s24le,pcm_s32le,pcm_f32le,pcm_f64le,mp3,mp3float,aac,flac \
  --enable-parser=mpegaudio,aac,ac3,flac \
  --enable-encoder=pcm_f64le,pcm_f32le,pcm_s16le \
  --enable-muxer=pcm_f64le,pcm_f32le,wav \
  --enable-filter=lumendsp,lumenout,alimiter,aresample,volume,anull,aformat,asetnsamples,ametadata,asendcmd,ashowinfo,equalizer,bass,treble
make -j2
cp "ffmpeg$suffix" "ffprobe$suffix" "$artifact/"
cp config.h config_components.h ffbuild/config.log "$artifact/provenance/"
git rev-parse HEAD > "$artifact/provenance/ffmpeg-revision.txt"
git diff -- libavfilter/Makefile libavfilter/allfilters.c \
  > "$artifact/provenance/ffmpeg-registration.patch"
grep -q '^#define CONFIG_LIBSOXR 1$' config.h
for filter in LUMENDSP LUMENOUT ARESAMPLE VOLUME EQUALIZER BASS TREBLE; do
  grep -q "^#define CONFIG_${filter}_FILTER 1$" config_components.h
done
if [ "$target" = linux ]; then
  ./ffmpeg -hide_banner -filters > "$artifact/provenance/filters.txt" 2>&1
  for filter in aresample lumendsp lumenout equalizer bass treble; do
    ./ffmpeg -hide_banner -h "filter=$filter" \
      > "$artifact/provenance/$filter-options.txt" 2>&1
  done
fi
cd "$artifact"
sha256sum "ffmpeg$suffix" "ffprobe$suffix" sources/* > SHA256SUMS
printf 'Built %s complete-graph PC tools only; no Android build or install.\n' "$target"
