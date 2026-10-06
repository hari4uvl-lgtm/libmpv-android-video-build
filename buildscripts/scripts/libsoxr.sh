#!/bin/bash -e
. ../../include/depinfo.sh
. ../../include/path.sh
if [ "$1" != build ]; then exit 255; fi
cmake -S . -B "_build$ndk_suffix" \
  -DCMAKE_TOOLCHAIN_FILE="$ANDROID_HOME/ndk/$v_ndk/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-21 \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
  -DCMAKE_INSTALL_PREFIX=/usr/local -DBUILD_SHARED_LIBS=OFF \
  -DBUILD_TESTS=OFF -DBUILD_EXAMPLES=OFF -DWITH_OPENMP=OFF \
  -DWITH_LSR_BINDINGS=OFF -DWITH_DEV_TRACE=OFF
cmake --build "_build$ndk_suffix" -j "$cores"
DESTDIR="$prefix_dir" cmake --install "_build$ndk_suffix"
sed -i 's/^Libs.private:.*/Libs.private: -lm/' "$prefix_dir/lib/pkgconfig/soxr.pc"
