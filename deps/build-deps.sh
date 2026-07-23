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
fi

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
			--prefix="${native_prefix}" \
			--buildtype=release \
			--default-library=static \
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

	# meson names its static library librist.a even under MSVC, but FFmpeg's
	# msvc toolchain rewrites the -lrist from librist.pc into rist.lib.
	# Providing that name is less invasive than patching the .pc back to an
	# absolute path, which is exactly the link-ordering trap fix_mbedtls_pc
	# exists to undo.
	if [[ ${host} == windows && -f "${prefix}/lib/librist.a" ]]; then
		cp "${prefix}/lib/librist.a" "${prefix}/lib/rist.lib"
	fi

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

	local own=()
	local name path
	for name in "${ordered[@]}"; do
		path=""
		# FFmpeg's msvc toolchain keeps the .a suffix; CMake-built deps
		# use .lib, and libsrt may keep its _static target name.
		for candidate in \
			"${libdir}/lib${name}.a" \
			"${libdir}/${name}.lib" \
			"${libdir}/lib${name}.lib" \
			"${libdir}/${name}_static.lib" \
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
		-L* | -Wl,-rpath*)
			# Our libraries are absolute paths; search paths that
			# point back into the prefix would only add ambiguity.
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
			if [[ -f "${libdir}/lib${name}.a" ]]; then
				own+=("${libdir}/lib${name}.a")
				continue
			elif [[ -f "${libdir}/${name}.lib" ]]; then
				own+=("$(cygpath -m "${libdir}/${name}.lib")")
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
	system=("${deduped[@]:-}")

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
build_mbedtls
build_srt
build_librist
build_nvcodec
build_ffmpeg
emit_cmake
log "done"
