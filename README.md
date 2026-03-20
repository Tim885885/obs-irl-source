# obs-irl-source

OBS plugin for receiving live IRL streams over SRT, RTMP, or any FFmpeg-supported protocol. Built for the specific challenges of IRL streaming: unreliable mobile connections, mid-stream joins, codec changes, and audio discontinuities.

## Why not the built-in Media Source?

OBS ships with a Media Source (`ffmpeg_source`) that can play SRT streams. It works, but it was designed for local file playback and general media, not for live IRL ingest. Here's what's different:

| | Media Source | IRL Source |
|---|---|---|
| **Audio jitter buffer** | None. Plays audio as fast as it arrives, leading to stuttering or speedups on unstable connections | Configurable ring buffer (default 80ms) absorbs network jitter and outputs smooth audio |
| **Adaptive playback speed** | Fixed 1x. Buffer grows unbounded on slow connections, causing increasing latency | Dynamically adjusts speed between 0.95x-1.05x to keep buffer at target level, preventing drift |
| **PTS discontinuity repair** | Passes through raw timestamps. Gaps in the stream (cell tower handoff, packet loss) cause audio pops and video freezes | Three-tier repair: small gaps get interpolated, medium gaps get silence insertion, large gaps trigger a clean reset |
| **Audio fade on disconnect** | Abrupt audio cutoff causes a loud click/pop | 50ms linear fade-out on disconnect, fade-in on reconnect |
| **Keyframe gating** | Starts decoding immediately, producing corrupted frames until a keyframe arrives | Waits for the first keyframe before outputting video. Buffers audio received before the keyframe so it's not lost |
| **Reconnection** | Has reconnect support but with general-purpose defaults | Immediate reconnect with configurable delay, designed for the frequent disconnects in IRL streaming |
| **Stats API** | None accessible to scripts | Exposes buffer fill level, playback speed, frame counts, PTS repairs, and silence insertions via `proc_handler` for monitoring overlays |

## Features

- **Protocol agnostic** — SRT, RTMP, RIST, UDP, TCP, HTTP, or anything FFmpeg can open
- **Codec agnostic** — H.264, HEVC (8-bit and 10-bit), AV1, VP9, AAC, Opus, etc.
- **Audio jitter buffer** — Ring buffer sized in milliseconds, adapts to any sample rate/channel count
- **Adaptive playback speed** — Keeps buffer at target level by micro-adjusting playback speed (inaudible 0.95x-1.05x range)
- **PTS discontinuity repair** — Handles the timestamp jumps that happen during cell tower handoffs and packet loss
- **Keyframe gating** — No corrupted frames on stream join or reconnect
- **Audio fade in/out** — Smooth transitions on disconnect/reconnect (no clicks)
- **10-bit video support** — Native passthrough of YUV420P10LE (I010) and P010 formats
- **FFmpeg option passthrough** — Override any demuxer option (latency, probesize, etc.) from the UI
- **Zero-copy video** — Passes decoded frame planes directly to OBS for supported pixel formats

## Installation

### Windows

1. Download `obs-irl-source.dll` from [Releases](../../releases)
2. Copy to `C:\Program Files\obs-studio\obs-plugins\64bit\`
3. Restart OBS

### Linux

1. Download `obs-irl-source.so` from [Releases](../../releases)
2. Copy to `/usr/lib/obs-plugins/` (or `~/.obs-studio/plugins/obs-irl-source/bin/64bit/`)
3. Restart OBS

## Usage

1. Add a new source: **IRL Source (irlserver.com)**
2. Enter your stream URL (e.g. `srt://your-server:4000?streamid=play/stream/key`)
3. Adjust buffer settings if needed (defaults work well for most cases)

### Settings

| Setting | Default | Description |
|---|---|---|
| URL | — | Any FFmpeg-supported URL (SRT, RTMP, etc.) |
| Reconnect Delay | 5s | Seconds between reconnect attempts |
| Target Buffer | 80ms | Audio jitter buffer target fill level |
| Min Buffer | 40ms | Minimum buffer before playback starts |
| Max Buffer | 200ms | Maximum buffer size (excess is trimmed) |
| Adaptive Speed | On | Auto-adjust playback speed to maintain buffer level |
| Speed Min/Max | 0.95/1.05 | Playback speed range for adaptive mode |
| Small Gap | 70ms | PTS gaps below this are interpolated silently |
| Large Gap | 2000ms | PTS gaps above this trigger a full reset |
| FFmpeg Options | — | Extra demuxer options (`key1=val1 key2=val2` format) |
| Wait for Keyframe | On | Don't output video until a keyframe arrives |

## Building from source

### Linux

```bash
sudo apt install build-essential cmake pkg-config libobs-dev \
    libavformat-dev libavcodec-dev libswresample-dev libswscale-dev libavutil-dev
cmake -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build --parallel
```

### Windows (MSVC)

Requires Visual Studio 2022 and OBS source + obs-deps:

```powershell
# Clone OBS and download pre-built dependencies
git clone --depth 1 --branch 32.1.0 https://github.com/obsproject/obs-studio.git obs-src
# Download obs-deps from https://github.com/obsproject/obs-deps/releases

# Build
cmake -B build -G "Visual Studio 17 2022" -A x64 -DOBS_SOURCE_DIR=obs-src -DFFMPEG_DIR=obs-deps
cmake --build build --config RelWithDebInfo
```

## License

AGPL-3.0-or-later. Copyright (C) 2026 Thomas Lekanger.

See [irlserver.com](https://irlserver.com) for more information.
