#!/bin/bash -e

. ../../include/depinfo.sh
. ../../include/path.sh

if [ "$1" == "build" ]; then
	true
elif [ "$1" == "clean" ]; then
	rm -rf _build$ndk_suffix
	exit 0
else
	exit 255
fi

# ---------------------------------------------------------------------------
# Lumen DSP filter
#
# WHY THIS IS DONE HERE AND NOT AS A PATCH FILE
#
# FFmpeg is downloaded fresh, so anything edited inside it by hand is wiped on
# the next build. The registration has to be performed BY the build, every time.
#
# patch.sh exists for this, but a .patch applies against exact line numbers and
# breaks the moment FFmpeg's own files shift. These edits are idempotent
# appends guarded by a grep, so they apply cleanly to any FFmpeg 6.x and can be
# run twice with no effect the second time.
#
# The whole thing is skipped if the source file is absent, so a checkout
# without it still builds exactly as before.
# ---------------------------------------------------------------------------
lumen_src="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/patches/af_lumendsp.c"

if [ -f "$lumen_src" ]; then
	echo "Lumen: installing af_lumendsp.c"
	cp "$lumen_src" libavfilter/af_lumendsp.c

	# 1. declare the filter
	if ! grep -q "ff_af_lumendsp" libavfilter/allfilters.c; then
		echo "extern const AVFilter ff_af_lumendsp;" >> libavfilter/allfilters.c
		echo "Lumen: registered in allfilters.c"
	fi

	# 2. add it to the build
	if ! grep -q "af_lumendsp.o" libavfilter/Makefile; then
		echo 'OBJS-$(CONFIG_LUMENDSP_FILTER) += af_lumendsp.o' >> libavfilter/Makefile
		echo "Lumen: added to libavfilter/Makefile"
	fi

	# 3. declare it to configure, so CONFIG_LUMENDSP_FILTER is generated
	if ! grep -q "lumendsp_filter" configure; then
		sed -i 's/^    loudnorm_filter/    lumendsp_filter\n    loudnorm_filter/' configure
		echo "Lumen: declared in configure"
	fi

	grep -q "lumendsp_filter" configure || { echo "Lumen: FAILED to declare filter"; exit 1; }
else
	echo "Lumen: patches/af_lumendsp.c not found, building without it"
fi

mkdir -p _build$ndk_suffix
cd _build$ndk_suffix

cpu=armv7-a
[[ "$ndk_triple" == "aarch64"* ]] && cpu=armv8-a
[[ "$ndk_triple" == "x86_64"* ]] && cpu=generic
[[ "$ndk_triple" == "i686"* ]] && cpu="i686 --disable-asm"

cpuflags=
[[ "$ndk_triple" == "arm"* ]] && cpuflags="$cpuflags -mfpu=neon -mcpu=cortex-a8"

../configure \
	--target-os=android --enable-cross-compile --cross-prefix=$ndk_triple- --ar=$AR --cc=$CC --nm=llvm-nm --ranlib=$RANLIB \
	--arch=${ndk_triple%%-*} --cpu=$cpu --pkg-config=pkg-config \
	--extra-cflags="-I$prefix_dir/include $cpuflags" --extra-ldflags="-L$prefix_dir/lib" \
	\
	--disable-gpl \
	--disable-nonfree \
	--enable-version3 \
	--enable-static \
	--disable-shared \
	--disable-vulkan \
	--disable-iconv \
	--disable-stripping \
	--pkg-config-flags=--static \
	\
	--disable-muxers \
	--disable-decoders \
	--disable-encoders \
	--disable-demuxers \
	--disable-parsers \
	--disable-protocols \
	--disable-devices \
	--disable-filters \
	--disable-doc \
	--disable-avdevice \
	--disable-postproc \
	--disable-programs \
	--disable-gray \
	--disable-swscale-alpha \
	\
	--enable-jni \
	--enable-bsfs \
	--enable-mediacodec \
	\
	--disable-dxva2 \
	--disable-vaapi \
	--disable-vdpau \
	--disable-bzlib \
	--disable-linux-perf \
	--disable-videotoolbox \
	--disable-audiotoolbox \
	\
	--enable-small \
	--enable-hwaccels \
	--enable-optimizations \
	--enable-runtime-cpudetect \
	\
	--enable-mbedtls \
	\
	--enable-libdav1d \
	\
	--enable-libxml2 \
	\
	--enable-avutil \
	--enable-avcodec \
	--enable-avfilter \
	--enable-avformat \
	--enable-swscale \
	--enable-swresample \
	\
	--enable-decoder=flv \
	--enable-decoder=h263 \
	--enable-decoder=h263i \
	--enable-decoder=h263p \
	--enable-decoder=h264* \
	--enable-decoder=mpeg1video \
	--enable-decoder=mpeg2* \
	--enable-decoder=mpeg4* \
	--enable-decoder=vp6 \
	--enable-decoder=vp6a \
	--enable-decoder=vp6f \
	--enable-decoder=vp8* \
	--enable-decoder=vp9* \
	--enable-decoder=hevc* \
	--enable-decoder=av1* \
	--enable-decoder=libdav1d \
	--enable-decoder=theora \
	--enable-decoder=msmpeg* \
	--enable-decoder=mjpeg* \
	--enable-decoder=wmv* \
	\
	--enable-decoder=aac* \
	--enable-decoder=ac3 \
	--enable-decoder=alac \
	--enable-decoder=als \
	--enable-decoder=ape \
	--enable-decoder=atrac* \
	--enable-decoder=eac3 \
	--enable-decoder=flac \
	--enable-decoder=gsm* \
	--enable-decoder=mp1* \
	--enable-decoder=mp2* \
	--enable-decoder=mp3* \
	--enable-decoder=mpc* \
	--enable-decoder=opus \
	--enable-decoder=ra* \
	--enable-decoder=ralf \
	--enable-decoder=shorten \
	--enable-decoder=tak \
	--enable-decoder=tta \
	--enable-decoder=vorbis \
	--enable-decoder=wavpack \
	--enable-decoder=wma* \
	--enable-decoder=pcm* \
	--enable-decoder=dsd* \
	--enable-decoder=dca \
	\
	--enable-decoder=ssa \
	--enable-decoder=ass \
	--enable-decoder=dvbsub \
	--enable-decoder=dvdsub \
	--enable-decoder=srt \
	--enable-decoder=stl \
	--enable-decoder=subrip \
	--enable-decoder=subviewer \
	--enable-decoder=subviewer1 \
	--enable-decoder=text \
	--enable-decoder=vplayer \
	--enable-decoder=webvtt \
	--enable-decoder=movtext \
	\
	--enable-demuxer=concat \
	--enable-demuxer=data \
	--enable-demuxer=flv \
	--enable-demuxer=hls \
	--enable-demuxer=latm \
	--enable-demuxer=live_flv \
	--enable-demuxer=loas \
	--enable-demuxer=m4v \
	--enable-demuxer=mov \
	--enable-demuxer=mpegps \
	--enable-demuxer=mpegts \
	--enable-demuxer=mpegvideo \
	--enable-demuxer=hevc \
	--enable-demuxer=rtsp \
	--enable-demuxer=mpeg4 \
	--enable-demuxer=mjpeg* \
	--enable-demuxer=avi \
	--enable-demuxer=av1 \
	--enable-demuxer=matroska \
	--enable-demuxer=dash \
	--enable-demuxer=webm_dash_manifest \
	\
	--enable-demuxer=aac \
	--enable-demuxer=ac3 \
	--enable-demuxer=aiff \
	--enable-demuxer=ape \
	--enable-demuxer=asf \
	--enable-demuxer=au \
	--enable-demuxer=avi \
	--enable-demuxer=flac \
	--enable-demuxer=flv \
	--enable-demuxer=matroska \
	--enable-demuxer=mov \
	--enable-demuxer=m4v \
	--enable-demuxer=mp3 \
	--enable-demuxer=mpc* \
	--enable-demuxer=ogg \
	--enable-demuxer=pcm* \
	--enable-demuxer=rm \
	--enable-demuxer=shorten \
	--enable-demuxer=tak \
	--enable-demuxer=tta \
	--enable-demuxer=wav \
	--enable-demuxer=wv \
	--enable-demuxer=xwma \
	--enable-demuxer=dsf \
	--enable-demuxer=truehd \
        --enable-demuxer=dts \
        --enable-demuxer=dtshd \
	\
	--enable-demuxer=ass \
	--enable-demuxer=srt \
	--enable-demuxer=stl \
	--enable-demuxer=webvtt \
	--enable-demuxer=subviewer \
	--enable-demuxer=subviewer1 \
	--enable-demuxer=vplayer \
	\
	--enable-parser=h263 \
	--enable-parser=h264 \
	--enable-parser=hevc \
	--enable-parser=mpeg4 \
	--enable-parser=mpeg4video \
	--enable-parser=mpegvideo \
	\
	--enable-parser=aac* \
	--enable-parser=ac3 \
	--enable-parser=cook \
	--enable-parser=dca \
	--enable-parser=flac \
	--enable-parser=gsm \
	--enable-parser=mpegaudio \
	--enable-parser=tak \
	--enable-parser=vorbis \
 	--enable-parser=dca \
	\
	--enable-filter=overlay \
	--enable-filter=equalizer \
	--enable-filter=aresample \
	--enable-filter=astats \
	--enable-filter=lumendsp \
	\
	--enable-protocol=async \
	--enable-protocol=cache \
	--enable-protocol=crypto \
	--enable-protocol=data \
	--enable-protocol=ffrtmphttp \
	--enable-protocol=file \
	--enable-protocol=ftp \
	--enable-protocol=hls \
	--enable-protocol=http \
	--enable-protocol=httpproxy \
	--enable-protocol=https \
	--enable-protocol=pipe \
	--enable-protocol=rtmp \
	--enable-protocol=rtmps \
	--enable-protocol=rtmpt \
	--enable-protocol=rtmpts \
	--enable-protocol=rtp \
	--enable-protocol=subfile \
	--enable-protocol=tcp \
	--enable-protocol=tls \
	--enable-protocol=srt \
	\
	--enable-encoder=mjpeg \
	--enable-encoder=ljpeg \
	--enable-encoder=jpegls \
	--enable-encoder=jpeg2000 \
	--enable-encoder=png \
	--enable-encoder=jpegls \
	\
	--enable-network \

make -j$cores
make DESTDIR="$prefix_dir" install

ln -sf "$prefix_dir"/lib/libswresample.so "$native_dir"
ln -sf "$prefix_dir"/lib/libpostproc.so "$native_dir"
ln -sf "$prefix_dir"/lib/libavutil.so "$native_dir"
ln -sf "$prefix_dir"/lib/libavcodec.so "$native_dir"
ln -sf "$prefix_dir"/lib/libavformat.so "$native_dir"
ln -sf "$prefix_dir"/lib/libswscale.so "$native_dir"
ln -sf "$prefix_dir"/lib/libavfilter.so "$native_dir"
ln -sf "$prefix_dir"/lib/libavdevice.so "$native_dir"
