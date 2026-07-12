# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project overview

IRL Source is a third-party OBS Studio plugin (C11, AGPL-3.0) for receiving live IRL streams over SRT, RTMP, or any FFmpeg-supported protocol. It solves IRL-specific problems: audio jitter buffering, PTS discontinuity repair, adaptive playback speed, keyframe gating, hardware-accelerated decoding, and mid-stream resolution changes.

## Build commands

### Linux

```bash
sudo apt install build-essential cmake pkg-config libobs-dev \
    libavformat-dev libavcodec-dev libswresample-dev libavfilter-dev \
    libswscale-dev libavutil-dev
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
FFmpeg URL, [receiver thread]: demux, decode, PTS repair
  audio: resample, jitter buffer
  video: keyframe gate, format conversion, OBS async video output

[audio thread]: jitter buffer, speed correction, concealment, OBS audio output
```

### Audio output contract (verified against libobs source)

The audio core is built around three facts about libobs:

1. OBS timestamps must be contiguous (`ts[n+1] = ts[n] + frames/rate`). Deviations under 70ms are smoothed, 70ms to 2s gaps are zero filled by OBS (audible), larger jumps flush all queued audio. The plugin therefore derives timestamps from a pure sample counter anchored once at prime time and never jumps the clock outside declared restarts.
2. Changing `samples_per_sec` between submissions makes OBS destroy and recreate its per source resampler with no crossfade (a click per change). Playback speed is instead applied inside the plugin with a persistent swresample compensation, and the rate submitted to OBS never changes.
3. The OBS mixer consumes 21.3ms ticks against wall clock. A source whose queued audio runs dry gets a tick of silence plus a time shifted splice (crackle), and a source that falls behind the mix window causes OBS to permanently add global audio buffering. After priming, the pump always emits (real audio or shaped concealment silence) and keeps a fixed lead ahead of wall clock.

Buffer regulation happens through playback speed only, asymmetric like IRLToolkit's player: builds at an inaudible -2%, drains post-stall backlog at up to +5% (mild chipmunk). Content is never skipped once playback has primed. Backlog beyond a fill ceiling is pushed back into the transport by pausing the read loop (TCP/RTMP backpressure; SRT bounds itself via its latency window), and startup backlog is trimmed only before priming.

### Source files

- **`src/plugin.c`**: OBS module entry point. Registers `irl_source_info` with callbacks.
- **`src/irl-source.c`**: Source lifecycle (create, destroy, update, tick). Loads config, manages threads, registers `proc_handler` for stats.
- **`src/receiver.c`**: thread entry points. The receiver thread runs the `av_read_frame()` loop, the audio thread runs the output pump.
- **`src/receiver-stream.c`**: stream open/close, demuxer options, reconnection, disconnect fade out, periodic stats logging.
- **`src/receiver-decode.c`**: packet to decoder plumbing with corruption burst handling and throttled decoder flushes.
- **`src/receiver-audio.c`**: the audio core. Intake side (receiver thread): PTS repair, resample to interleaved float, write to the PTS aware jitter buffer. Pre-keyframe audio is discarded (not staged) to avoid decoder warm-up artifacts. Output side (audio thread): sample counter output clock, constant rate submission, swr based speed correction, dropout concealment, hidden backlog trims.
- **`src/receiver-video.c`**: decoded video frame handling, keyframe gate, resolution change detection.
- **`src/audio-buffer.c`**: thread safe ring buffer sized in milliseconds with a parallel PTS chunk queue. Mutex protected. Supports fade-out reads.
- **`src/video-handler.c`**: converts AVFrames to OBS video. Maps pixel formats (I420, NV12, I010, P010, etc.), handles HW frame transfer, falls back to swscale for unsupported formats. Maps video PTS through the audio playout offset for lip sync.
- **`src/pts-repair.c`**: three tier PTS discontinuity repair. Small gaps interpolated, medium gaps get silence, large gaps trigger full reset.
- **`src/settings.c`**: OBS properties UI and default values.

### Headers (`include/`)

- **`irl-source.h`** — Central header. Defines `struct irl_source` (main context), `struct irl_config`, all `#define` defaults, and function declarations for every module.
- **`audio-buffer.h`** — `struct audio_buffer` and ring buffer API.
- **`pts-repair.h`** — `struct pts_repair`, `enum pts_action`, and repair API.

### Threading model

- **Main/OBS thread**: calls create, destroy, update, tick, get_properties
- **Receiver thread**: owns demux/decode FFmpeg state. Writes to the audio buffer (mutex protected) and pushes decoded video frames (PTS pre-converted to nanoseconds) onto the video queue. Never blocks on GPU or OBS video delivery.
- **Video thread**: pops the video queue, does the HW frame transfer and format conversion (owns sws_ctx), and calls `obs_source_output_video`. Queue overflow drops the oldest frame (`video_queue_drops`).
- **Audio thread**: drains the jitter buffer and submits audio to OBS via `obs_source_output_audio`, paced against the sample counter output clock. Shared timing state is protected by `audio_state_lock` (lock order: `audio_state_lock` before the buffer mutex).

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

- **`irl-stats.lua`** - Example OBS Lua script that reads plugin stats via proc_handler and updates a text source overlay.
