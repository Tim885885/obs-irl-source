# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project overview

obs-irl-source is an OBS Studio plugin (C11, AGPL-3.0) for receiving live IRL streams over SRT, RTMP, or any FFmpeg-supported protocol. It solves IRL-specific problems: audio jitter buffering, PTS discontinuity repair, adaptive playback speed, keyframe gating, hardware-accelerated decoding, and mid-stream resolution changes.

## Build commands

### Linux
```bash
sudo apt install build-essential cmake pkg-config libobs-dev \
    libavformat-dev libavcodec-dev libswresample-dev libswscale-dev libavutil-dev
cmake -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build --parallel
```

### Windows (MSVC)
```powershell
cmake -B build -G "Visual Studio 17 2022" -A x64 -DOBS_SOURCE_DIR=obs-src -DFFMPEG_DIR=obs-deps
cmake --build build --config RelWithDebInfo
```

### macOS (Apple Silicon)
```bash
brew install cmake pkg-config ffmpeg simde uthash jansson
cmake -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo -DOBS_SOURCE_DIR=$PWD/obs-src
cmake --build build --parallel
```

Output: `build/obs-irl-source.so` (Linux/macOS) or `build/RelWithDebInfo/obs-irl-source.dll` (Windows).

There are no tests.

## Architecture

Single OBS MODULE shared library. All source is C11.

### Data flow

```
FFmpeg URL → [receiver thread] → demux → decode → PTS repair
  → audio: jitter buffer → adaptive speed → fade → OBS audio output
  → video: keyframe gate → format conversion → OBS async video output
```

### Source files

- **`src/plugin.c`** — OBS module entry point. Registers `irl_source_info` with callbacks.
- **`src/irl-source.c`** — Source lifecycle: create/destroy/update/tick. Loads config, manages receiver thread, registers `proc_handler` for stats.
- **`src/receiver.c`** — FFmpeg demux/decode thread (~700 lines, the core). Opens URLs, sets up HW decode, runs `av_read_frame()` loop, handles reconnection, audio processing (PTS repair → resample → buffer → speed adjust → OBS output), video processing (keyframe gate → video handler).
- **`src/audio-buffer.c`** — Thread-safe ring buffer sized in milliseconds. Mutex-protected. Supports fade-out reads.
- **`src/audio-speed.c`** — Adaptive playback speed controller (0.95x–1.05x). Adjusts `samples_per_sec` on OBS audio output to leverage OBS's built-in resampler.
- **`src/video-handler.c`** — Converts AVFrames to OBS video. Maps pixel formats (I420, NV12, I010, P010, etc.), handles HW frame transfer, falls back to swscale for unsupported formats.
- **`src/pts-repair.c`** — Three-tier PTS discontinuity repair: small gaps interpolated, medium gaps get silence, large gaps trigger full reset.
- **`src/settings.c`** — OBS properties UI and default values.

### Headers (`include/`)

- **`irl-source.h`** — Central header. Defines `struct irl_source` (main context), `struct irl_config`, all `#define` defaults, and function declarations for every module.
- **`audio-buffer.h`** — `struct audio_buffer` and ring buffer API.
- **`pts-repair.h`** — `struct pts_repair`, `enum pts_action`, and repair API.

### Threading model

- **Main/OBS thread**: calls create, destroy, update, tick, get_properties
- **Receiver thread** (`receiver.c`): owns all FFmpeg state. Writes to audio buffer (mutex-protected). Outputs video frames directly to OBS via `obs_source_output_video`.

### OBS API conventions

- Memory: use `bfree()`/`bstrdup()`/`bzalloc()` (OBS allocators), not stdlib malloc/free
- Logging: `blog(LOG_INFO, "[irl-source] ...")` — always use the `[irl-source]` prefix
- Stats are exposed via `proc_handler` ("get_stats" call) for Lua/Python script consumption
- Source flags: `OBS_SOURCE_AUDIO | OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_DO_NOT_DUPLICATE`

## CI

GitHub Actions (`.github/workflows/build.yml`) builds on three platforms: Linux x64 (Ubuntu 24.04), Windows x64 (VS 2022), macOS ARM64 (macos-15). The Windows and macOS jobs clone OBS source, patch it to build only libobs, and link against that. No release automation — artifacts are uploaded only.

## Contributing

If you wish to contribute PRs to this project, please understand what you are changing. You should be able to write any replies to reviews/PRs yourself — don't copy and paste replies directly from AI.

The initial version of this plugin was heavily built with LLM assistance. The author (datagutt) has experience with video and SRT(LA) protocols but is less familiar with C and the OBS Studio codebase. Tagged releases are fully tested; individual commits may not be.

## Other files

- **`irl-stats.lua`** — Example OBS Lua script that reads plugin stats via proc_handler and updates a text source overlay.
