#!/usr/bin/env bash
# Rebuild the prebuilts in ../prebuilts with the Android NDK.
#
#   WORK=<scratch dir> NDK=<ndk 27.1 dir> ./build.sh
#
# WORK holds the sources and build trees. Missing sources are fetched:
# mesa-$MESA.tar.xz, libdrm-$LIBDRM.tar.xz and a minigbm clone at
# $MINIGBM. Host tools: meson, ninja, pkg-config, python3 with mako,
# bison, flex, git, curl.
#
# Everything is tuned for the Exynos 7420's Cortex-A57 cores (-mcpu also
# keeps the ISA at ARMv8.0, which the A53 cluster runs too), and Mesa is
# built with link-time optimisation.
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
OUTDIR=$(cd "$HERE/../prebuilts" && pwd)
WORK=${WORK:?set WORK to a scratch directory}
NDK=${NDK:?set NDK to the Android NDK 27.1 directory}
API=33
MESA=24.3.4
LIBDRM=2.4.123
MINIGBM=250c655ac972a4057ccacda6653893f22e15c78e

case $(uname -s) in
	Darwin) HOST=darwin-x86_64 ;;
	*) HOST=linux-x86_64 ;;
esac
N=$NDK/toolchains/llvm/prebuilt/$HOST/bin
mkdir -p "$WORK/src" "$WORK/build"
cd "$WORK/src"

[ -d mesa-$MESA ] || {
	[ -f mesa-$MESA.tar.xz ] ||
		curl -fLO https://archive.mesa3d.org/mesa-$MESA.tar.xz
	tar -xf mesa-$MESA.tar.xz
}
[ -d libdrm-$LIBDRM ] || {
	[ -f libdrm-$LIBDRM.tar.xz ] ||
		curl -fLO https://dri.freedesktop.org/libdrm/libdrm-$LIBDRM.tar.xz
	tar -xf libdrm-$LIBDRM.tar.xz
}
[ -d minigbm ] || {
	git clone -q https://chromium.googlesource.com/chromiumos/platform/minigbm
	git -C minigbm checkout -q $MINIGBM
	git -C minigbm apply "$HERE"/patches/minigbm-*.patch
}

build_arch() {
	local arch=$1 triple libdir
	case $arch in
		aarch64) triple=aarch64-linux-android$API; libdir=lib64 ;;
		arm) triple=armv7a-linux-androideabi$API; libdir=lib ;;
	esac
	local B=$WORK/build SYS=$WORK/sysroot/$arch
	local cross=$B/android-$arch.ini cpu_family=$arch cpu=armv8
	[ $arch = arm ] && cpu=armv7
	local tune=-mcpu=cortex-a57
	cat > "$cross" <<EOF
[binaries]
ar = '$N/llvm-ar'
c = ['$N/$triple-clang', '$tune']
cpp = ['$N/$triple-clang++', '$tune', '-fno-exceptions', '-fno-unwind-tables', '-fno-asynchronous-unwind-tables', '-static-libstdc++']
c_ld = 'lld'
cpp_ld = 'lld'
strip = '$N/llvm-strip'
pkg-config = ['env', 'PKG_CONFIG_LIBDIR=$SYS/lib/pkgconfig', 'PKG_CONFIG_SYSROOT_DIR=', 'pkg-config']

[host_machine]
system = 'android'
cpu_family = '$cpu_family'
cpu = '$cpu'
endian = 'little'
EOF

	# Static libdrm for the gralloc and HWC modules and for Mesa.
	meson setup --wipe "$B/libdrm-$arch" "$WORK/src/libdrm-$LIBDRM" \
		--cross-file="$cross" -Ddefault_library=static -Dbuildtype=release \
		-Dintel=disabled -Dradeon=disabled -Damdgpu=disabled \
		-Dnouveau=disabled -Dvmwgfx=disabled -Domap=disabled \
		-Dexynos=disabled -Dfreedreno=disabled -Dtegra=disabled \
		-Dvc4=disabled -Detnaviv=disabled -Dcairo-tests=disabled \
		-Dman-pages=disabled -Dvalgrind=disabled -Dtests=false \
		-Dudev=false -Dprefix="$SYS" -Dlibdir=lib
	ninja -C "$B/libdrm-$arch" install

	# Mesa: panfrost only, EGL and GLES, no LLVM, with LTO.
	meson setup --wipe "$B/mesa-$arch" "$WORK/src/mesa-$MESA" \
		--cross-file="$cross" -Dbuildtype=release -Db_ndebug=true \
		-Db_lto=true \
		-Dplatforms=android -Dplatform-sdk-version=$API \
		-Dandroid-stub=true -Dandroid-libbacktrace=disabled \
		-Dgallium-drivers=panfrost -Dvulkan-drivers= -Dllvm=disabled \
		-Dshared-llvm=disabled -Degl=enabled -Dgles1=enabled \
		-Dgles2=enabled -Dopengl=false -Dglx=disabled -Dgbm=disabled \
		-Dxmlconfig=disabled -Dzstd=disabled -Dvalgrind=disabled \
		-Dlibunwind=disabled -Dlmsensors=disabled -Dbuild-tests=false \
		-Degl-lib-suffix=_mesa -Dgles-lib-suffix=_mesa -Dcpp_rtti=false \
		-Dexpat=disabled -Dprefix=/vendor -Dlibdir=$libdir
	ninja -C "$B/mesa-$arch"
	rm -rf "$B/stage-$arch"
	DESTDIR="$B/stage-$arch" meson install -C "$B/mesa-$arch" --no-rebuild

	local stub=$B/mesa-$arch/src/android_stub
	local MESAINC=$WORK/src/mesa-$MESA/include/android_stub
	local inc=(-I"$HERE/compat/include" -I"$MESAINC" -I"$SYS/include" -I"$SYS/include/libdrm")

	# minigbm gralloc0. Mesa's libcutils link stub lacks native_handle_*;
	# a second stub adds them for linking only.
	local M=$B/minigbm-$arch S=$WORK/src/minigbm
	mkdir -p "$M/obj" "$M/stub"
	"$N/$triple-clang++" -shared -fPIC -Wl,-soname,libcutils.so -o "$M/stub/libcutils.so" \
		-I"$MESAINC" "$WORK/src/mesa-$MESA/src/android_stub/cutils_stub.cpp" \
		-x c "$HERE/compat/native_handle_stub.c"
	local def=($tune -D_GNU_SOURCE=1 -DHAS_NO_AIDL_METADATA -DNDEBUG -fPIC -O2 -Wall
		-Wno-unused-function -fvisibility=default)
	local objs=() f o
	for f in "$S"/*.c; do
		case $(basename "$f") in gbm*|minigbm*) continue ;; esac
		o=$M/obj/$(basename "${f%.c}").o
		"$N/$triple-clang" -std=c11 "${def[@]}" -I"$S" "${inc[@]}" -c "$f" -o "$o"
		objs+=("$o")
	done
	for f in "$S"/cros_gralloc/*.cc "$S"/cros_gralloc/gralloc0/gralloc0.cc; do
		o=$M/obj/$(basename "${f%.cc}").o
		"$N/$triple-clang++" -std=c++17 -fno-exceptions -fno-rtti "${def[@]}" \
			-I"$S" "${inc[@]}" -c "$f" -o "$o"
		objs+=("$o")
	done
	"$N/$triple-clang++" -shared -static-libstdc++ -Wl,-soname,gralloc.minigbm.so \
		-Wl,--no-undefined -o "$M/gralloc.minigbm.so" "${objs[@]}" \
		"$SYS/lib/libdrm.a" -L"$M/stub" -L"$stub" -lcutils -lhardware -lsync -llog

	# The HWC 1.4 KMS module.
	local H=$B/hwc-$arch
	mkdir -p "$H"
	"$N/$triple-clang++" $tune -std=c++17 -O2 -Wall -Wextra -Wno-missing-field-initializers \
		-fPIC -shared -fno-exceptions -fno-rtti -static-libstdc++ \
		-Wl,-soname,hwcomposer.s6kms.so -Wl,--no-undefined \
		-I"$S/cros_gralloc" "${inc[@]}" "$HERE/hwc/hwc_kms.cpp" \
		-o "$H/hwcomposer.s6kms.so" "$SYS/lib/libdrm.a" -llog

	# Collect.
	local D=$OUTDIR/$libdir V=$B/stage-$arch/vendor/$libdir
	mkdir -p "$D/egl" "$D/hw"
	for f in libglapi.so libgallium_dri.so; do
		cp "$(find "$V" -name $f -type f | head -1)" "$D/$f"
	done
	for f in libEGL_mesa.so libGLESv1_CM_mesa.so libGLESv2_mesa.so; do
		cp "$(find "$V" -name $f -type f | head -1)" "$D/egl/$f"
	done
	cp "$M/gralloc.minigbm.so" "$H/hwcomposer.s6kms.so" "$D/hw/"
	"$N/llvm-strip" --strip-unneeded "$D"/*.so "$D"/egl/*.so "$D"/hw/*.so
}

build_arch aarch64
build_arch arm
echo "prebuilts updated in $OUTDIR"
