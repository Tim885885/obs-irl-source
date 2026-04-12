# IRL Source

Third-party plugin for [OBS Studio](https://obsproject.com/) that receives live IRL streams over SRT, RTMP, or any FFmpeg-supported protocol. Built for the actual problems you hit on IRL streams: flaky mobile connections, mid-stream joins, codec changes, audio discontinuities.

> **Note:** This is an independent project by [irlserver.com](https://irlserver.com). It is not developed by, affiliated with, or endorsed by the OBS Project.

## Why open source?

We run a commercial streaming service (relay infrastructure and more), so yes, we have plenty of closed-source code. But OBS itself is GPL-2.0. It is free software built by its community. Paywalling the thing that keeps your stream from dropping frames, just to upsell a subscription, feels like the wrong move.

The IRL streaming scene was mostly built in the open. Projects like [Moblin](https://github.com/eerimoq/moblin), [NOALBS](https://github.com/NOALBS/nginx-obs-automatic-low-bitrate-switching), and [BELABOX](https://github.com/BELABOX) all started open and pushed things forward because anyone could use them, learn from them, and build on top. Some competitors went the other way and shipped closed-source OBS plugins as a product feature, locking basic stream reliability behind a monthly fee. That is a bad trade for streamers and for the ecosystem. The plugin layer should be something everyone can use, inspect, and improve. That is why this is AGPL. If someone builds on it, those improvements come back too.

If you find this useful, great. If you want managed infrastructure on top of it, that's what [irlserver.com](https://irlserver.com) is for.

## Why not the built-in Media Source?

OBS ships with a Media Source (`ffmpeg_source`) that can play SRT streams. It works, but it was built for local file playback and general media, not for live IRL ingest. The differences:

| | Media Source | IRL Source |
|---|---|---|
| **Audio jitter buffer** | None. Plays audio as fast as it arrives, so unstable connections give you stuttering or speedups | Configurable ring buffer (default 120ms) that absorbs network jitter, or a low-latency mode if you prefer |
| **Adaptive playback speed** | Fixed 1x. Buffer grows unbounded on slow connections and latency keeps climbing | Adjusts speed between 0.95x and 1.05x in buffered mode to keep the buffer near target |
| **PTS discontinuity repair** | Passes through raw timestamps. Gaps in the stream (cell tower handoff, packet loss) cause audio pops and video freezes | Three tiers: small gaps get interpolated, medium gaps get silence insertion, large gaps trigger a clean reset |
| **Audio fade on disconnect** | Abrupt cutoff, loud click/pop | 50ms linear fade-out on disconnect, fade-in on reconnect |
| **Keyframe gating** | Starts decoding immediately, so you get corrupted frames until a keyframe arrives | Waits for the first keyframe before outputting video. Drops pre-keyframe audio so the decoder does not warm up with garbage |
| **Decoder recovery** | Decoder gets stuck in a bad state during SRT bitrate starvation. Audio breaks permanently until you restart the source | Flushes the decoder on repeated send/receive errors, resets bad timing state, holds the last good video frame during corruption |
| **Reconnection** | Reconnect exists but uses general-purpose defaults | 2-second default reconnect, tuned for how often IRL streams drop |
| **Hardware decoding** | Supported | Auto-detects D3D11VA, CUDA/NVDEC, VAAPI. Works on NVIDIA, Intel, AMD, with automatic fallback |
| **Resolution changes** | May crash or freeze on adaptive bitrate resolution changes | Handles mid-stream resolution changes gracefully (phone rotation, adaptive bitrate) |
| **Network buffer** | Configurable but not optimized for live | 2MB default transport buffer tuned for SRT live streaming |
| **Stats API** | None accessible to scripts | Exposes buffer fill level, playback speed, frame counts, PTS repairs, silence insertions, stream delay, reconnect count, and low-latency mode flag via `proc_handler` |

For a deeper look at how the jitter buffer, adaptive speed, PTS repair, and timestamp handling work together, see [Audio pipeline](docs/audio-pipeline.md).

## Features

- Protocol agnostic: SRT, RTMP, RIST, UDP, TCP, HTTP, or anything FFmpeg can open.
- Codec agnostic: H.264, HEVC (8-bit and 10-bit), AV1, VP9, AAC, Opus, etc.
- Audio jitter buffer, sized in milliseconds, adapts to any sample rate or channel count.
- Adaptive playback speed that keeps buffered mode near target by micro-adjusting inside an inaudible 0.95x to 1.05x range.
- Low Latency Audio mode that uses OBS async unbuffered semantics and drains immediately instead of building a 60-120ms startup cushion.
- PTS discontinuity repair for the timestamp jumps that happen during cell tower handoffs and packet loss.
- Keyframe gating, so you do not get corrupted frames on stream join or reconnect.
- Audio fade in and out on disconnect/reconnect (no clicks).
- Hardware decoding with auto-detection of NVDEC, D3D11VA, VAAPI, and automatic software fallback.
- Decoder auto-recovery: flushes the decoder on repeated decode errors (including receive-frame failures), audio self-heals.
- Graceful mid-stream resolution changes for adaptive bitrate and phone rotation.
- Configurable network buffer (default 2MB) to absorb network-level jitter.
- Native 10-bit passthrough for YUV420P10LE (I010) and P010.
- FFmpeg option passthrough, so you can override any demuxer option (latency, probesize, etc.) from the UI.
- Zero-copy video for supported pixel formats, planes go straight to OBS.
- Holds the last good video frame while the decoder is damaged, instead of showing gray/corrupt output.
- Periodic stats logging every 30 seconds: frame counts, buffer level, speed, PTS repairs.

## Installation

### Windows

1. Download `obs-irl-source.dll` from [Releases](../../releases)
2. Copy to `C:\Program Files\obs-studio\obs-plugins\64bit\`
3. Restart OBS

### macOS (Apple Silicon)

1. Download `obs-irl-source.so` from [Releases](../../releases) (macOS ARM64 build)
2. Copy to `~/Library/Application Support/obs-studio/plugins/obs-irl-source/bin/`
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
| Reconnect Delay | 2s | Seconds between reconnect attempts |
| Network Buffer | 2 MB | Transport-level buffer size (higher = more resilient, more latency) |
| Target Buffer | 120ms | Audio jitter buffer target fill level |
| Min Buffer | 60ms | Minimum buffer before playback starts |
| Max Buffer | 300ms | Maximum buffer size (excess is trimmed) |
| Adaptive Speed | On | Auto-adjust playback speed to maintain buffer level |
| Speed Min/Max | 0.95/1.05 | Playback speed range for adaptive mode |
| Small Gap | 70ms | PTS gaps below this are interpolated silently |
| Large Gap | 2000ms | PTS gaps above this trigger a full reset |
| FFmpeg Options | — | Extra demuxer options (`key1=val1 key2=val2` format) |
| Hardware Decode | Auto | GPU decoding (Auto tries D3D11VA/CUDA/VAAPI, Off forces software) |
| Wait for Keyframe | On | Don't output video until a keyframe arrives |
| Low Latency Audio | Off | Uses OBS async unbuffered audio mode and drains audio immediately instead of waiting for the normal buffer minimum |
| Decoupled Audio | Off | Enables OBS async decoupled mode when Low Latency Audio is on |

### Buffered vs low-latency audio mode

`Low Latency Audio` changes plugin behavior, not just an OBS flag.

- Buffered mode is the default IRL path. It uses the configured `Target/Min/Max Buffer` values and adaptive speed to stay stable on rough mobile links.
- Low-latency mode drains audio as soon as chunks are available and turns off plugin-side adaptive speed. It matches OBS async unbuffered timing better. Use it when absolute latency matters more than having a jitter cushion.
- `Decoupled Audio` only applies when low-latency mode is enabled.

## Building from source

### Linux

```bash
sudo apt install build-essential cmake pkg-config libobs-dev \
    libavformat-dev libavcodec-dev libswresample-dev libavfilter-dev \
    libswscale-dev libavutil-dev
cmake -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build --parallel
```

### Windows (MSVC)

Requires Visual Studio 2022 and OBS source + obs-deps:

```powershell
# Clone OBS and download pre-built dependencies
git clone --depth 1 --branch 32.1.0 https://github.com/obsproject/obs-studio.git obs-src
# Download obs-deps from https://github.com/obsproject/obs-deps/releases
# Install SIMDe headers or add them to CMAKE_PREFIX_PATH

# Build
cmake -B build -G "Visual Studio 17 2022" -A x64 -DOBS_SOURCE_DIR=obs-src -DFFMPEG_DIR=obs-deps
cmake --build build --config RelWithDebInfo
```

## Stats overlay

The plugin exposes live statistics via OBS's `proc_handler` API. You can query it from a Lua or Python script to build a real-time stats overlay.

### Available stats

| Field | Type | Description |
|---|---|---|
| `buffer_fill_ms` | int | Current audio jitter buffer fill level (ms) |
| `current_speed` | float | Current adaptive playback speed (1.0 = normal) |
| `reconnecting` | bool | Whether the source is currently reconnecting |
| `total_audio_frames` | int | Total audio frames decoded since connection |
| `total_video_frames` | int | Total video frames decoded since connection |
| `pts_repairs` | int | Number of PTS discontinuities repaired |
| `silence_insertions` | int | Number of silence insertions for gap filling |
| `stream_delay_ms` | int | End-to-end stream delay (SRT latency + decode + buffering) |
| `low_latency_audio` | bool | Whether OBS async unbuffered low-latency mode is enabled |
| `decoupled_audio` | bool | Whether OBS async decoupled mode is enabled |
| `reconnect_count` | int | Number of reconnect attempts since the source was created |

### Example Lua script (stats text overlay)

Create a Text (GDI+) source called `IRL Stats`, then add this as a Lua script in OBS:

```lua
obs = obslua

function script_description()
    return "Updates a text source with IRL Source stats"
end

function script_update(settings)
end

function script_tick(seconds)
    local source = obs.obs_get_source_by_name("IRL Source (irlserver.com)")
    if not source then return end

    local ph = obs.obs_source_get_proc_handler(source)
    local cd = obs.calldata_create()
    obs.proc_handler_call(ph, "get_stats", cd)

    local buf_ms = obs.calldata_int(cd, "buffer_fill_ms")
    local speed = obs.calldata_float(cd, "current_speed")
    local reconnecting = obs.calldata_bool(cd, "reconnecting")
    local video = obs.calldata_int(cd, "total_video_frames")
    local audio = obs.calldata_int(cd, "total_audio_frames")
    local repairs = obs.calldata_int(cd, "pts_repairs")
    local delay = obs.calldata_int(cd, "stream_delay_ms")

    obs.calldata_destroy(cd)
    obs.obs_source_release(source)

    local status = reconnecting and "RECONNECTING" or "LIVE"
    local text = string.format(
        "Status: %s\nDelay: %dms\nBuffer: %dms\nSpeed: %.3fx\nFrames: %d/%d (v/a)\nPTS Repairs: %d",
        status, delay, buf_ms, speed, video, audio, repairs
    )

    local text_source = obs.obs_get_source_by_name("IRL Stats")
    if text_source then
        local settings = obs.obs_data_create()
        obs.obs_data_set_string(settings, "text", text)
        obs.obs_source_update(text_source, settings)
        obs.obs_data_release(settings)
        obs.obs_source_release(text_source)
    end
end
```

### OBS log stats

The plugin also logs stats to the OBS log every 30 seconds:

```
[irl-source] Stats: video=1800 audio=2700 buf=82ms speed=1.000 pts_repairs=0 silence=0 res=1920x1080
```

## Hardware decoding

The plugin automatically tries GPU-accelerated decoding in this order:

| Platform | APIs tried |
|---|---|
| Windows | D3D11VA (Intel/AMD/NVIDIA), CUDA (NVIDIA NVDEC) |
| macOS | VideoToolbox (Apple Silicon & Intel) |
| Linux | VAAPI (Intel/AMD), CUDA (NVIDIA) |

Falls back to software decoding if no hardware decoder is available. Turn it off with the **Hardware Decode: Off** setting.

The OBS log shows which decoder is active:

```
[irl-source] Video stream 0: hevc 1920x1080 (NVDEC)
```

## AI usage

I (datagutt) don't really know too much C, and I am a bit unfamiliar with the OBS Studio code base.
What i do have is quite a bit of experience working with video and SRT(LA) protocols from other projects.

The initial version of this plugin was heavily built with LLM assistance. That includes most of this README (except this "AI Usage" section).

Rest assured I will go through both the README and codebase and clean this up, once I have the initial builds working well.

Any tagged release should at least be fully tested, single commits might not (though i will try to use branches).

## Contributing

If you wish to contribute PRs to this project, please understand what you are changing. Also, you should be able to write any replies to reviews/PRs yourself.
Please don't just copy and paste replies directly from the AI.

## License

AGPL-3.0-or-later. Copyright (C) 2026 Thomas Lekanger.

See [irlserver.com](https://irlserver.com) for more information.
