#!/usr/bin/env bash
#
# obs-irl-source — build the bundled media stack
# (mbedTLS, libsrt, librist, nv-codec-headers, FFmpeg).
#
# Copyright (C) 2026 Thomas Lekanger
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Produces static libraries plus a generated irl-deps.cmake describing the
# exact link line. On Windows this runs inside an MSYS2 bash with the MSVC
# environment already active (FFmpeg's configure needs a POSIX shell even when
# it drives cl.exe).
#
# Usage:
#   deps/build-deps.sh [--prefix DIR] [--jobs N] [--clean]

set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_dir="$(cd "${script_dir}/.." && pwd)"

# shellcheck source=versions.env
source "${script_dir}/versions.env"

prefix="${IRL_DEPS_PREFIX:-${repo_dir}/deps/.build/prefix}"
work="${IRL_DEPS_WORK:-${repo_dir}/deps/.build}"
jobs="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"
clean=0

while [[ $# -gt 0 ]]; do
	case "$1" in
	--prefix)
		prefix="$2"
		shift 2
		;;
	--jobs)
		jobs="$2"
		shift 2
		;;
	--clean)
		clean=1
		shift
		;;
	-h | --help)
		sed -n '2,20p' "${BASH_SOURCE[0]}"
		exit 0
		;;
	*)
		echo "unknown argument: $1" >&2
		exit 1
		;;
	esac
done

case "$(uname -s)" in
Linux*) host=linux ;;
Darwin*) host=macos ;;
MINGW* | MSYS* | CYGWIN*) host=windows ;;
*)
	echo "unsupported host: $(uname -s)" >&2
	exit 1
	;;
esac

if [[ ${host} == windows ]]; then
	# MSYS2 ships a coreutils /usr/bin/link.exe that shadows MSVC's linker.
	# CMake is unaffected (it resolves the linker next to the compiler), but
	# meson probes PATH and aborts with "Found GNU link.exe instead of MSVC
	# link.exe". cl.exe and link.exe live in the same directory, so putting
	# that directory first fixes it without modifying the MSYS2 install.
	if ! command -v cl >/dev/null 2>&1; then
		echo "cl.exe is not on PATH. Run this from an MSVC environment" >&2
		echo "(the CI job uses ilammy/msvc-dev-cmd plus msys2 path-type: inherit)." >&2
		exit 1
	fi
	PATH="$(dirname "$(command -v cl)"):${PATH}"
	export PATH
fi

downloads="${work}/downloads"
src="${work}/src"

if [[ ${clean} -eq 1 ]]; then
	rm -rf "${src}" "${prefix}"
fi
mkdir -p "${downloads}" "${src}" "${prefix}"

# Absolute, forward-slash prefix. MSYS2 hands cl.exe/cmake native paths, so
# translate once here rather than at every use site.
prefix="$(cd "${prefix}" && pwd)"
if [[ ${host} == windows ]]; then
	native_prefix="$(cygpath -m "${prefix}")"
else
	native_prefix="${prefix}"
fi

log() { printf '\n\033[1m==> %s\033[0m\n' "$*"; }

# Path in the form the tool expects. FFmpeg's build is MSYS-native and takes
# MSYS paths, but cmake.exe and cl.exe are Windows binaries that cannot read
# them, so anything handed to CMake goes through here first.
npath() {
	if [[ ${host} == windows ]]; then
		cygpath -m "$1"
	else
		printf '%s' "$1"
	fi
}

sha256_of() {
	if command -v sha256sum >/dev/null 2>&1; then
		sha256sum "$1" | cut -d' ' -f1
	else
		shasum -a 256 "$1" | cut -d' ' -f1
	fi
}

# fetch <url> <filename> <sha256>
fetch() {
	local url="$1" file="$2" want="$3" path="${downloads}/$2"

	if [[ -f ${path} ]] && [[ "$(sha256_of "${path}")" == "${want}" ]]; then
		echo "cached: ${file}"
		return
	fi

	echo "download: ${url}"
	curl -fsSL --retry 3 --retry-delay 2 -o "${path}.tmp" "${url}"

	local got
	got="$(sha256_of "${path}.tmp")"
	if [[ ${got} != "${want}" ]]; then
		rm -f "${path}.tmp"
		echo "checksum mismatch for ${file}: got ${got}, expected ${want}" >&2
		exit 1
	fi
	mv "${path}.tmp" "${path}"
}

# extract <filename> <dest-dir-name>
extract() {
	local file="$1" dest="${src}/$2"
	[[ -d ${dest} ]] && return
	mkdir -p "${dest}"
	tar -xf "${downloads}/${file}" -C "${dest}" --strip-components=1
}

# Marker files keep an interrupted or repeated run from redoing finished work.
# They live inside the prefix so CI can cache the prefix alone: restoring it
# without the (much larger) source and object trees still reads as "built".
built() { [[ -f "${prefix}/.built-$1-$2" ]]; }
mark_built() { touch "${prefix}/.built-$1-$2"; }

export PKG_CONFIG_PATH="${prefix}/lib/pkgconfig:${prefix}/lib64/pkgconfig${PKG_CONFIG_PATH:+:${PKG_CONFIG_PATH}}"

# Both libsrt and librist record their mbedTLS dependency in their .pc file as
# an absolute library path. That breaks a static link twice over:
#
#   1. FFmpeg's configure treats anything that is not -lfoo as a compiler flag
#      and emits it *before* the -lsrt / -lrist it belongs to, which a
#      single-pass linker cannot resolve.
#   2. Whichever mbedTLS the dependency's build system happened to find gets
#      baked in by path. librist's meson picked up the host's shared
#      libmbedcrypto.so, which would both reintroduce a runtime dependency the
#      bundling exists to remove and mismatch the copy libsrt uses.
#
# Rewriting the reference to plain -l flags, appended after the library's own
# entry, fixes the ordering and pins it to the mbedTLS in this prefix.
# FFmpeg's msvc toolchain rewrites the -lfoo it reads from a .pc file into
# foo.lib, but neither CMake nor meson necessarily names its static output that
# way: libsrt builds srt_static.lib, meson builds librist.a. The mismatch shows
# up as "X not found using pkg-config" from a library that just installed
# successfully. Provide the name the linker will ask for.
#
# Patching the .pc to an absolute path would also resolve it, and would also
# reintroduce the link-ordering trap fix_mbedtls_pc exists to undo.
# ensure_msvc_lib_name <-l name> [extra basename ...]
ensure_msvc_lib_name() {
	local want="$1"
	shift
	[[ ${host} == windows ]] || return 0

	local dst="${prefix}/lib/${want}.lib"
	[[ -f ${dst} ]] && return 0

	local cand base
	local candidates=(
		"${want}_static"
		"lib${want}"
		"$@"
	)
	for base in "${candidates[@]}"; do
		for cand in "${prefix}/lib/${base}.lib" "${prefix}/lib/${base}.a"; do
			if [[ -f ${cand} ]]; then
				cp "${cand}" "${dst}"
				echo "provided $(basename "${dst}") from $(basename "${cand}")"
				return 0
			fi
		done
	done

	echo "no static library found for -l${want} in ${prefix}/lib" >&2
	ls -1 "${prefix}/lib" >&2 || true
	exit 1
}

fix_mbedtls_pc() {
	local pc="$1" field="$2"
	[[ -f ${pc} ]] || return 0
	sed -i.bak -E \
		-e 's![^[:space:]]*/lib(mbedtls|mbedx509|mbedcrypto|everest|p256m)\.(a|so[0-9.]*|dylib|lib)!!g' \
		-e 's!-l(mbedtls|mbedx509|mbedcrypto)([[:space:]]|$)!\2!g' \
		-e "s!^(${field}:.*)\$!\\1 -L\\\${libdir} -lmbedtls -lmbedx509 -lmbedcrypto!" \
		"${pc}"
	rm -f "${pc}.bak"
}

cmake_common=(
	-DCMAKE_BUILD_TYPE=Release
	-DCMAKE_INSTALL_PREFIX="${native_prefix}"
	-DCMAKE_PREFIX_PATH="${native_prefix}"
	-DCMAKE_POSITION_INDEPENDENT_CODE=ON
	-DBUILD_SHARED_LIBS=OFF
)
if [[ ${host} == macos ]]; then
	cmake_common+=(-DCMAKE_OSX_DEPLOYMENT_TARGET="${MACOSX_DEPLOYMENT_TARGET:-12.0}")
fi
if [[ ${host} == windows ]]; then
	# Ninja avoids the MSBuild/MSYS path translation mess entirely.
	cmake_common+=(-G Ninja -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl)
	# Pin the dynamic CRT everywhere. See IRL_MSVC_CRT below; CMP0091 has to
	# be forced because a dependency declaring cmake_minimum_required below
	# 3.15 gets the old policy and silently ignores the runtime setting.
	cmake_common+=(
		-DCMAKE_POLICY_DEFAULT_CMP0091=NEW
		-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL
	)
fi

# Everything that ends up in one DLL must agree on the C runtime. Mixing them
# is not merely a link error: /MD and /MT binaries get separate heaps and
# separate stdio state, so a buffer allocated in one and freed in the other
# corrupts the process.
#
# cl.exe defaults to /MT when given no flag, while CMake and meson both emit
# /MD. FFmpeg's configure passes nothing, so it silently built /MT against /MD
# dependencies and failed with unresolved __imp_* CRT imports. The plugin DLL
# itself is /MD (CMake's default, and what libobs uses), so /MD is the target
# and all three build systems are pinned to it explicitly rather than left to
# their defaults.
#
# Seeded with the options every platform needs, so the array is never empty:
# macOS still ships bash 3.2, where expanding an empty array under `set -u` is
# an unbound-variable error.
meson_common=(--buildtype=release --default-library=static)
if [[ ${host} == windows ]]; then
	meson_common+=(-Db_vscrt=md)
fi

# ── zlib ─────────────────────────────────────────────────────────────────────
# Windows only. Linux and macOS supply zlib as a system library, which the
# builds there already link. Building it here rather than dropping
# --enable-zlib on Windows keeps the FFmpeg feature set identical on all three
# platforms; a capability that silently differs per platform is worse than a
# short extra build step.
build_zlib() {
	[[ ${host} == windows ]] || return 0
	built zlib "${ZLIB_VERSION}" && return
	log "zlib ${ZLIB_VERSION}"

	fetch "https://github.com/madler/zlib/releases/download/v${ZLIB_VERSION}/zlib-${ZLIB_VERSION}.tar.gz" \
		"zlib-${ZLIB_VERSION}.tar.gz" "${ZLIB_SHA256}"
	extract "zlib-${ZLIB_VERSION}.tar.gz" "zlib-${ZLIB_VERSION}"

	cmake -S "$(npath "${src}/zlib-${ZLIB_VERSION}")" \
		-B "$(npath "${src}/zlib-${ZLIB_VERSION}/build")" \
		"${cmake_common[@]}" \
		-DZLIB_BUILD_EXAMPLES=OFF
	cmake --build "$(npath "${src}/zlib-${ZLIB_VERSION}/build")" --parallel "${jobs}"
	cmake --install "$(npath "${src}/zlib-${ZLIB_VERSION}/build")"

	# zlib's CMake calls its static output zlibstatic; FFmpeg asks for z.lib.
	ensure_msvc_lib_name z zlibstatic zlib

	# zconf.h.cmakein still carries an autoconf-era block:
	#
	#     #ifdef HAVE_UNISTD_H
	#     #  define Z_HAVE_UNISTD_H
	#     #endif
	#
	# CMake probes for unistd.h, does not find it under MSVC, and correctly
	# leaves Z_HAVE_UNISTD_H undefined earlier in the file. That block then
	# defines it anyway, because FFmpeg's config.h contains
	# "#define HAVE_UNISTD_H 0" and #ifdef is true for a value of zero. The
	# header goes on to include <unistd.h>, which MSVC does not have, and
	# every FFmpeg source that touches zlib fails to compile.
	#
	# zlib's own ./configure rewrites this line for exactly this reason. The
	# CMake path does not, so do it here.
	local zconf="${prefix}/include/zconf.h"
	if [[ -f ${zconf} ]]; then
		sed -i.bak \
			's!^#ifdef HAVE_UNISTD_H.*!#if 0 /* patched: MSVC has no unistd.h, and FFmpeg defines HAVE_UNISTD_H to 0 */!' \
			"${zconf}"
		rm -f "${zconf}.bak"
		if grep -q '^#ifdef HAVE_UNISTD_H' "${zconf}"; then
			echo "failed to patch HAVE_UNISTD_H out of ${zconf}" >&2
			exit 1
		fi
	fi

	mark_built zlib "${ZLIB_VERSION}"
}

# ── mbedTLS ──────────────────────────────────────────────────────────────────
# Supplies TLS for FFmpeg (https, rtmps) and AES for libsrt passphrases.
# Apache-2.0, which is compatible with the LGPLv3 FFmpeg build below.
build_mbedtls() {
	built mbedtls "${MBEDTLS_VERSION}" && return
	log "mbedTLS ${MBEDTLS_VERSION}"

	fetch "https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-${MBEDTLS_VERSION}/mbedtls-${MBEDTLS_VERSION}.tar.bz2" \
		"mbedtls-${MBEDTLS_VERSION}.tar.bz2" "${MBEDTLS_SHA256}"
	extract "mbedtls-${MBEDTLS_VERSION}.tar.bz2" "mbedtls-${MBEDTLS_VERSION}"

	cmake -S "$(npath "${src}/mbedtls-${MBEDTLS_VERSION}")" \
		-B "$(npath "${src}/mbedtls-${MBEDTLS_VERSION}/build")" \
		"${cmake_common[@]}" \
		-DENABLE_TESTING=OFF \
		-DENABLE_PROGRAMS=OFF \
		-DUSE_STATIC_MBEDTLS_LIBRARY=ON \
		-DUSE_SHARED_MBEDTLS_LIBRARY=OFF \
		-DMBEDTLS_AS_SUBPROJECT=OFF
	cmake --build "$(npath "${src}/mbedtls-${MBEDTLS_VERSION}/build")" --parallel "${jobs}"
	cmake --install "$(npath "${src}/mbedtls-${MBEDTLS_VERSION}/build")"

	# mbedTLS draws entropy from BCryptGenRandom on Windows but its generated
	# .pc files do not declare bcrypt, so anything linking it statically via
	# pkg-config fails on that one symbol. FFmpeg reports it as the wholly
	# misleading "ERROR: mbedTLS not found".
	if [[ ${host} == windows ]]; then
		local pc
		for pc in "${prefix}/lib/pkgconfig/mbedcrypto.pc" \
			"${prefix}/lib/pkgconfig/mbedtls.pc"; do
			[[ -f ${pc} ]] || continue
			grep -q -- '-lbcrypt' "${pc}" && continue
			sed -i.bak -E 's!^(Libs:.*)$!\1 -lbcrypt!' "${pc}"
			rm -f "${pc}.bak"
		done
	fi

	mark_built mbedtls "${MBEDTLS_VERSION}"
}

# ── libsrt ───────────────────────────────────────────────────────────────────
build_srt() {
	built srt "${SRT_VERSION}" && return
	log "libsrt ${SRT_VERSION}"

	fetch "https://github.com/Haivision/srt/archive/refs/tags/v${SRT_VERSION}.tar.gz" \
		"srt-${SRT_VERSION}.tar.gz" "${SRT_SHA256}"
	extract "srt-${SRT_VERSION}.tar.gz" "srt-${SRT_VERSION}"

	cmake -S "$(npath "${src}/srt-${SRT_VERSION}")" \
		-B "$(npath "${src}/srt-${SRT_VERSION}/build")" \
		"${cmake_common[@]}" \
		-DENABLE_SHARED=OFF \
		-DENABLE_STATIC=ON \
		-DENABLE_APPS=OFF \
		-DENABLE_EXAMPLES=OFF \
		-DENABLE_UNITTESTS=OFF \
		-DENABLE_ENCRYPTION=ON \
		-DUSE_ENCLIB=mbedtls \
		-DENABLE_CXX11=ON
	cmake --build "$(npath "${src}/srt-${SRT_VERSION}/build")" --parallel "${jobs}"
	cmake --install "$(npath "${src}/srt-${SRT_VERSION}/build")"

	fix_mbedtls_pc "${prefix}/lib/pkgconfig/srt.pc" "Libs.private"
	ensure_msvc_lib_name srt

	mark_built srt "${SRT_VERSION}"
}

# ── librist ──────────────────────────────────────────────────────────────────
# Meson, not CMake, which is why the deps build needs meson/ninja everywhere.
# lz4 and cJSON use librist's vendored copies so this pulls in no further
# system dependencies; mbedTLS is the one we already built, giving RIST its
# encryption support.
build_librist() {
	built librist "${LIBRIST_VERSION}" && return
	log "librist ${LIBRIST_VERSION}"

	fetch "https://code.videolan.org/rist/librist/-/archive/v${LIBRIST_VERSION}/librist-v${LIBRIST_VERSION}.tar.gz" \
		"librist-${LIBRIST_VERSION}.tar.gz" "${LIBRIST_SHA256}"
	extract "librist-${LIBRIST_VERSION}.tar.gz" "librist-${LIBRIST_VERSION}"

	local rist="${src}/librist-${LIBRIST_VERSION}"
	rm -rf "${rist}/build"
	(
		cd "${rist}"
		# librist locates mbedTLS with cc.find_library, which searches
		# the default system paths and would happily bind the host's
		# shared libmbedcrypto. Point the compiler at this prefix first
		# so it finds the static copy libsrt is already using.
		meson setup build \
			"${meson_common[@]}" \
			--prefix="${native_prefix}" \
			-Dc_args="-I${native_prefix}/include" \
			-Dc_link_args="-L${native_prefix}/lib" \
			-Dbuilt_tools=false \
			-Dtest=false \
			-Dbuiltin_lz4=true \
			-Dbuiltin_cjson=true \
			-Dbuiltin_mbedtls=false \
			-Duse_mbedtls=true
		meson compile -C build
		meson install -C build
	)

	fix_mbedtls_pc "${prefix}/lib/pkgconfig/librist.pc" "Libs"

	ensure_msvc_lib_name rist

	mark_built librist "${LIBRIST_VERSION}"
}

# ── nv-codec-headers ─────────────────────────────────────────────────────────
build_nvcodec() {
	[[ ${host} == macos ]] && return 0
	built nvcodec "${NVCODEC_VERSION}" && return
	log "nv-codec-headers ${NVCODEC_VERSION}"

	fetch "https://github.com/FFmpeg/nv-codec-headers/archive/refs/tags/n${NVCODEC_VERSION}.tar.gz" \
		"nv-codec-headers-${NVCODEC_VERSION}.tar.gz" "${NVCODEC_SHA256}"
	extract "nv-codec-headers-${NVCODEC_VERSION}.tar.gz" "nv-codec-headers-${NVCODEC_VERSION}"

	make -C "${src}/nv-codec-headers-${NVCODEC_VERSION}" install PREFIX="${prefix}"

	mark_built nvcodec "${NVCODEC_VERSION}"
}

# ── FFmpeg ───────────────────────────────────────────────────────────────────
#
# Decode-only and deliberately narrow: --disable-everything, then only the
# components an IRL ingest actually touches. Keeping the surface small is what
# makes a static link viable size-wise.
#
# LGPLv3 (--enable-version3, no --enable-gpl). Nothing here needs GPL
# components: the plugin decodes, it never encodes.

FFMPEG_DECODERS="h264,hevc,av1,vp9,aac,aac_latm,aac_fixed,opus,mp3,mp3float,ac3,ac3_fixed,eac3,pcm_s16le,pcm_s16be,pcm_s24le,pcm_s32le,pcm_u8,pcm_f32le,pcm_alaw,pcm_mulaw"
FFMPEG_PARSERS="h264,hevc,av1,vp9,aac,aac_latm,ac3,mpegaudio,opus"
FFMPEG_DEMUXERS="mpegts,mpegtsraw,flv,live_flv,mov,matroska,hls,rtsp,sdp,rtp,h264,hevc,av1,ivf,aac,mp3,ac3,wav,mpjpeg,data"
# SRT and RIST are "libsrt"/"librist" — FFmpeg names each protocol after the
# library. There is no "hls" protocol either; HLS is a demuxer that drives http.
FFMPEG_PROTOCOLS="file,pipe,data,cache,concat,tcp,udp,rtp,http,https,httpproxy,tls,crypto,libsrt,librist,rtmp,rtmpe,rtmps,rtmpt,rtmpte,rtmpts"
FFMPEG_BSFS="h264_mp4toannexb,hevc_mp4toannexb,av1_frame_merge,vp9_superframe,vp9_superframe_split,extract_extradata,aac_adtstoasc,null"

# Components whose absence would silently degrade the plugin rather than fail
# the build. FFmpeg's configure ignores unknown names in an --enable-* list, so
# a typo here disables a feature without any diagnostic: --enable-protocol=srt
# happily produced a build with CONFIG_LIBSRT=yes and no SRT protocol at all.
FFMPEG_REQUIRED_CONFIG="
LIBSRT_PROTOCOL
LIBRIST_PROTOCOL
MBEDTLS
ZLIB
TLS_PROTOCOL
RTMP_PROTOCOL
HTTPS_PROTOCOL
MPEGTS_DEMUXER
FLV_DEMUXER
MATROSKA_DEMUXER
H264_DECODER
HEVC_DECODER
AV1_DECODER
VP9_DECODER
AAC_DECODER
OPUS_DECODER
MP3_DECODER
AC3_DECODER
SWSCALE
SWRESAMPLE
"

build_ffmpeg() {
	built ffmpeg "${FFMPEG_VERSION}" && return
	log "FFmpeg ${FFMPEG_VERSION}"

	fetch "https://ffmpeg.org/releases/ffmpeg-${FFMPEG_VERSION}.tar.xz" \
		"ffmpeg-${FFMPEG_VERSION}.tar.xz" "${FFMPEG_SHA256}"
	extract "ffmpeg-${FFMPEG_VERSION}.tar.xz" "ffmpeg-${FFMPEG_VERSION}"

	local ff="${src}/ffmpeg-${FFMPEG_VERSION}"

	# Upstream bug. libavformat/tls_mbedtls.c calls gmtime_r without
	# including libavutil/time_internal.h, which is where FFmpeg keeps the
	# ff_gmtime_r fallback for platforms that lack the POSIX function.
	# Everywhere except MSVC the real gmtime_r resolves and the missing
	# include is invisible, so it surfaces only as a lone unresolved
	# external when the plugin links.
	#
	# Applied on every platform: the include is a no-op where HAVE_GMTIME_R
	# is set, and keeping one source state across platforms beats carrying a
	# Windows-only divergence.
	local tls="${ff}/libavformat/tls_mbedtls.c"
	if [[ -f ${tls} ]] && ! grep -q 'libavutil/time_internal.h' "${tls}"; then
		sed -i.bak \
			's!^#include "libavutil/random_seed.h"!&\n#include "libavutil/time_internal.h"!' \
			"${tls}"
		rm -f "${tls}.bak"
		if ! grep -q 'libavutil/time_internal.h' "${tls}"; then
			echo "failed to patch gmtime_r include into ${tls}" >&2
			echo "check whether the anchor include still exists upstream." >&2
			exit 1
		fi
		echo "patched gmtime_r include into libavformat/tls_mbedtls.c"
	fi

	local args=(
		--prefix="${prefix}"
		--disable-shared
		--enable-static
		--enable-pic
		--enable-version3
		--disable-programs
		--disable-doc
		--disable-avdevice
		--disable-avfilter
		--disable-everything
		--disable-autodetect
		--enable-swscale
		--enable-swresample
		--enable-network
		--enable-zlib
		--enable-mbedtls
		--enable-libsrt
		--enable-librist
		--enable-decoder="${FFMPEG_DECODERS}"
		--enable-parser="${FFMPEG_PARSERS}"
		--enable-demuxer="${FFMPEG_DEMUXERS}"
		--enable-protocol="${FFMPEG_PROTOCOLS}"
		--enable-bsf="${FFMPEG_BSFS}"
		--pkg-config-flags=--static
	)

	case "${host}" in
	linux)
		args+=(
			--enable-ffnvcodec
			--enable-cuda
			--enable-nvdec
		)
		# VAAPI links libva at build time, so a plugin built with it
		# carries a hard DT_NEEDED on libva.so.2. That is the accepted
		# trade for Intel/AMD hardware decode, but it must never be
		# dropped silently: a release build without VAAPI would ship
		# software-only decode with no visible failure.
		if [[ ${IRL_DEPS_DISABLE_VAAPI:-0} == 1 ]]; then
			echo "warning: VAAPI disabled by IRL_DEPS_DISABLE_VAAPI — do not ship this build" >&2
			args+=(--enable-hwaccel=h264_nvdec,hevc_nvdec,av1_nvdec,vp9_nvdec)
		elif pkg-config --exists libva libva-drm; then
			args+=(
				--enable-vaapi
				--enable-hwaccel=h264_vaapi,hevc_vaapi,av1_vaapi,vp9_vaapi,h264_nvdec,hevc_nvdec,av1_nvdec,vp9_nvdec
			)
		else
			echo "libva development files not found (install libva-dev)." >&2
			echo "Set IRL_DEPS_DISABLE_VAAPI=1 to build without hardware decode on Intel/AMD." >&2
			exit 1
		fi
		;;
	macos)
		args+=(
			--enable-videotoolbox
			--enable-hwaccel=h264_videotoolbox,hevc_videotoolbox,av1_videotoolbox,vp9_videotoolbox
		)
		;;
	windows)
		args+=(
			--toolchain=msvc
			--target-os=win64
			--arch=x86_64
			# cl.exe defaults to the static CRT; everything else here
			# is /MD. See the meson_common comment above.
			--extra-cflags=-MD
			# Not every configure probe goes through pkg-config; the
			# fallbacks link a bare -lmbedtls with no search path and
			# fail with "cannot open input file mbedtls.lib".
			--extra-cflags=-I"${native_prefix}/include"
			--extra-ldflags=-libpath:"${native_prefix}/lib"
			--enable-d3d11va
			--enable-dxva2
			--enable-ffnvcodec
			--enable-cuda
			--enable-nvdec
			--enable-hwaccel=h264_d3d11va,h264_d3d11va2,hevc_d3d11va,hevc_d3d11va2,av1_d3d11va,av1_d3d11va2,vp9_d3d11va,vp9_d3d11va2,h264_dxva2,hevc_dxva2,av1_dxva2,vp9_dxva2,h264_nvdec,hevc_nvdec,av1_nvdec,vp9_nvdec
		)
		;;
	esac

	# configure prints only "X not found using pkg-config" on failure; the
	# actual compiler and linker invocation lives in config.log, which is
	# the only thing that distinguishes a missing library from one whose
	# name or link order the toolchain got wrong.
	if ! (cd "${ff}" && ./configure "${args[@]}"); then
		echo
		echo "---- tail of ffbuild/config.log ----" >&2
		tail -60 "${ff}/ffbuild/config.log" >&2 || true
		exit 1
	fi

	# config.mak marks a disabled component by prefixing it with '!'.
	local missing=()
	local component
	for component in ${FFMPEG_REQUIRED_CONFIG}; do
		if ! grep -qx "CONFIG_${component}=yes" "${ff}/ffbuild/config.mak"; then
			missing+=("${component}")
		fi
	done
	if [[ ${#missing[@]} -gt 0 ]]; then
		echo "FFmpeg configure did not enable: ${missing[*]}" >&2
		echo "Check the --enable-* component names in ${BASH_SOURCE[0]}." >&2
		exit 1
	fi

	local hwaccels
	hwaccels="$(grep -c '^CONFIG_[A-Z0-9_]*_HWACCEL=yes' "${ff}/ffbuild/config.mak" || true)"
	echo "FFmpeg configured with ${hwaccels} hwaccel(s)"
	if [[ ${hwaccels} -eq 0 ]]; then
		echo "no hardware accelerators enabled — hardware decode would be dead" >&2
		exit 1
	fi

	make -C "${ff}" -j"${jobs}"
	make -C "${ff}" install

	mark_built ffmpeg "${FFMPEG_VERSION}"
}

# ── Generated CMake description ──────────────────────────────────────────────
#
# The plugin's CMakeLists includes this file rather than rediscovering the
# stack. Our own libraries go in by absolute path (no -l name resolution to get
# wrong); everything else comes from pkg-config --static, which is the only
# thing that knows the full transitive system-library set for a static FFmpeg.
emit_cmake() {
	log "generating irl-deps.cmake"

	local libdir="${prefix}/lib"
	local out="${prefix}/irl-deps.cmake"

	# Link order matters for a single-pass static link.
	local ordered=(avformat avcodec swscale swresample avutil srt rist mbedtls mbedx509 mbedcrypto)

	# zlib is ours only on Windows; Linux and macOS pull the system one in
	# through pkg-config as -lz. Listing it explicitly guarantees it reaches
	# the link line: the Windows .pc chain does not surface it as a -l flag,
	# so it would otherwise be dropped and show up as unresolved inflate and
	# deflate at plugin link time. It goes last, after its consumers.
	if [[ ${host} == windows ]]; then
		ordered+=(z)
	fi

	local own=()
	local name path
	for name in "${ordered[@]}"; do
		path=""
		# .lib first so a Windows build picks the name the MSVC linker
		# expects: meson emits librist.a even under MSVC, and both that
		# and the rist.lib beside it would link, but mixing conventions
		# in one link line is needless room for surprise. On Linux only
		# the lib*.a form exists, so the order costs nothing there.
		for candidate in \
			"${libdir}/${name}.lib" \
			"${libdir}/lib${name}.lib" \
			"${libdir}/${name}_static.lib" \
			"${libdir}/lib${name}.a" \
			"${libdir}/lib${name}_static.a"; do
			[[ -f ${candidate} ]] && path="${candidate}" && break
		done
		if [[ -z ${path} ]]; then
			echo "missing static library: ${name} (looked in ${libdir})" >&2
			exit 1
		fi
		if [[ ${host} == windows ]]; then
			path="$(cygpath -m "${path}")"
		fi
		own+=("${path}")
	done

	# Everything pkg-config reports that is not one of ours is a system
	# dependency (-lm, -lva, -framework VideoToolbox, ws2_32.lib, ...).
	#
	# srt is queried explicitly: FFmpeg's .pc files do not propagate it, and
	# libsrt is C++, so its .pc is the only place the C++ runtime
	# (-lstdc++ / -lc++) appears.
	local raw
	raw="$(pkg-config --static --libs libavformat libavcodec libswscale libswresample libavutil srt librist 2>/dev/null || true)"

	local system=()
	local pending_framework=0
	local tok
	for tok in ${raw}; do
		if [[ ${pending_framework} -eq 1 ]]; then
			system+=("-Wl,-framework,${tok}")
			pending_framework=0
			continue
		fi
		case "${tok}" in
		-framework)
			pending_framework=1
			;;
		-L* | -Wl,-rpath* | -libpath:* | -LIBPATH:* | /libpath:* | /LIBPATH:*)
			# Our libraries are absolute paths; search paths that
			# point back into the prefix would only add ambiguity.
			#
			# The MSVC spellings must be matched here rather than
			# left to the -l* arm below, which is greedy enough to
			# read "-libpath:C:/x" as a library named "ibpath:C:/x"
			# and emit "ibpath:C:/x.lib".
			;;
		-l*)
			name="${tok#-l}"
			# Already listed by absolute path above.
			if [[ " ${ordered[*]} " == *" ${name} "* ]]; then
				continue
			fi
			# The compiler adds these itself, and naming them
			# explicitly only creates link-order hazards.
			case "${name}" in
			c | gcc | gcc_s) continue ;;
			esac
			# A prefix-local library we did not anticipate: keep it,
			# by absolute path, rather than silently dropping it.
			# .lib first, matching the candidate order above.
			if [[ -f "${libdir}/${name}.lib" ]]; then
				own+=("$(npath "${libdir}/${name}.lib")")
				continue
			elif [[ -f "${libdir}/lib${name}.a" ]]; then
				own+=("${libdir}/lib${name}.a")
				continue
			fi
			if [[ ${host} == windows ]]; then
				system+=("${name}.lib")
			else
				system+=("${tok}")
			fi
			;;
		*.lib)
			name="$(basename "${tok}")"
			[[ -f "${libdir}/${name}" ]] && continue
			system+=("${name}")
			;;
		-*)
			system+=("${tok}")
			;;
		esac
	done

	# Order within the system list does not matter (these resolve against
	# shared libraries), so collapse the duplicates pkg-config emits.
	local deduped=()
	for tok in "${system[@]:-}"; do
		[[ -z ${tok} ]] && continue
		[[ " ${deduped[*]:-} " == *" ${tok} "* ]] && continue
		deduped+=("${tok}")
	done
	# Not `system=("${deduped[@]:-}")`: on an empty array that idiom yields a
	# single empty element, and an empty entry in the CMake link list is an
	# error rather than a no-op.
	system=()
	if [[ ${#deduped[@]} -gt 0 ]]; then
		system=("${deduped[@]}")
	fi

	{
		echo "# Generated by deps/build-deps.sh — do not edit."
		echo "set(IRL_DEPS_FOUND TRUE)"
		echo "set(IRL_DEPS_HOST \"${host}\")"
		echo "set(IRL_DEPS_FFMPEG_VERSION \"${FFMPEG_VERSION}\")"
		echo "set(IRL_DEPS_SRT_VERSION \"${SRT_VERSION}\")"
		echo "set(IRL_DEPS_LIBRIST_VERSION \"${LIBRIST_VERSION}\")"
		echo "set(IRL_DEPS_MBEDTLS_VERSION \"${MBEDTLS_VERSION}\")"
		echo "set(IRL_DEPS_INCLUDE_DIRS \"${native_prefix}/include\")"
		printf 'set(IRL_DEPS_STATIC_LIBRARIES\n'
		printf '    "%s"\n' "${own[@]}"
		printf ')\n'
		printf 'set(IRL_DEPS_SYSTEM_LIBRARIES\n'
		if [[ ${#system[@]} -gt 0 ]]; then
			printf '    "%s"\n' "${system[@]}"
		fi
		printf ')\n'
	} >"${out}"

	echo "wrote ${out}"
	cat "${out}"
}

log "building bundled deps for ${host} into ${prefix}"
build_zlib
build_mbedtls
build_srt
build_librist
build_nvcodec
build_ffmpeg
emit_cmake
log "done"
