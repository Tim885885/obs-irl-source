# Third-party notices

`obs-irl-source` is licensed under AGPL-3.0-or-later. The released binaries
statically link the libraries below, so this notice travels with them.

Versions are pinned in [`deps/versions.env`](deps/versions.env) and the complete
build recipe is [`deps/build-deps.sh`](deps/build-deps.sh); together they
reproduce the exact binaries that ship. Upstream source for every component is
available from the project URLs listed here.

## Statically linked

### FFmpeg — LGPL-3.0-or-later

<https://ffmpeg.org/> · <https://git.ffmpeg.org/ffmpeg.git>

Configured with `--enable-version3` and **without** `--enable-gpl` or
`--enable-nonfree`, so the build is LGPLv3 rather than GPL. The plugin only
decodes and never encodes, so no GPL-only component is needed. The build is also
`--disable-everything` plus an explicit decoder/demuxer/protocol allowlist; see
`deps/build-deps.sh` for the exact set.

LGPLv3 requires that recipients be able to relink the work against a modified
FFmpeg. `deps/build-deps.sh` builds FFmpeg from unmodified upstream release
tarballs at the pinned version, and the plugin's own source is available under
AGPL-3.0-or-later, which together satisfy that requirement.

License text: <https://www.gnu.org/licenses/lgpl-3.0.html>

### libsrt — MPL-2.0

<https://github.com/Haivision/srt>

Used unmodified. MPL-2.0 requires that modifications to covered files be made
available under the same license; no modifications are made.

License text: <https://www.mozilla.org/en-US/MPL/2.0/>

### librist — BSD-2-Clause

<https://code.videolan.org/rist/librist>

```
Copyright © librist authors

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
   list of conditions and the following disclaimer.
2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

### Mbed TLS — Apache-2.0

<https://github.com/Mbed-TLS/mbedtls>

Mbed TLS 3.x is dual licensed Apache-2.0 or GPL-2.0-or-later; it is used here
under Apache-2.0. Provides TLS and the crypto backing SRT's encryption.

License text: <https://www.apache.org/licenses/LICENSE-2.0>

### zlib — zlib license (Windows builds only)

<https://zlib.net/>

Linux and macOS use the system zlib; Windows has none to link, so it is built
into the Windows binary.

```
This software is provided 'as-is', without any express or implied warranty. In
no event will the authors be held liable for any damages arising from the use
of this software.

Permission is granted to anyone to use this software for any purpose, including
commercial applications, and to alter it and redistribute it freely, subject to
the following restrictions:

1. The origin of this software must not be misrepresented; you must not claim
   that you wrote the original software. If you use this software in a product,
   an acknowledgment in the product documentation would be appreciated but is
   not required.
2. Altered source versions must be plainly marked as such, and must not be
   misrepresented as being the original software.
3. This notice may not be removed or altered from any source distribution.
```

## Build-time only

### nv-codec-headers — MIT

<https://github.com/FFmpeg/nv-codec-headers>

Headers only. FFmpeg loads `nvcuda`/`nvcuvid` at runtime, so these add no
build-time or load-time dependency on a CUDA installation and no code from them
is linked into the plugin.

### w32-pthreads — Apache-2.0 (Windows builds only)

Ships with OBS Studio (<https://github.com/obsproject/obs-studio>). libobs
headers declare against it, so the Windows build carries the import. The
plugin's own threading uses the Win32 primitives in `include/irl-threading.h`.

## Vendored source

### obs-websocket-api.h — GPL-2.0-or-later

<https://github.com/obsproject/obs-websocket>

A verbatim copy lives in `third_party/`; see `third_party/README.md` for
provenance. It is a header-only client API. Nothing links against obs-websocket,
and the vendor extension degrades to a log line when it is absent.

## Interfaces

### libobs — GPL-2.0-or-later

<https://github.com/obsproject/obs-studio>

Dynamically linked against the host OBS Studio installation, which supplies it.
It is not redistributed here.
