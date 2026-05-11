# Audio pipeline

How the plugin keeps audio stable on unreliable mobile connections, and how the buffered and low-latency modes differ.

## Quality policy

The output policy is viewer-first:

- Prefer short silence over jittery, glitchy, metallic, or artifacty audio.
- Prefer holding the last good video frame over outputting gray/corrupt frames.
- Prefer bounded latency movement over aggressive time-stretching.
- Make every recovery mechanism visible in stats so tuning is evidence-driven.

## The problem with big buffers

Media Source handles network jitter the simple way: buffer a lot of data, play it back with a delay. If the network hiccups, the buffer absorbs it. This works, but every millisecond of buffer is a millisecond of extra latency. For IRL streaming — where you're reading chat and reacting live — a 2-3 second buffer means a 2-3 second delay on top of everything else.

The plugin takes the opposite approach: keep the buffer as small as possible and actively compensate for the problems that a small buffer exposes.

## How it works

The normal buffered audio pipeline has four layers that work together:

```
decode -> PTS repair -> jitter buffer -> adaptive latency control -> OBS output
```

Low-latency mode uses a shorter path:

```
decode -> PTS repair -> minimal buffer -> OBS output
```

In that mode the plugin still repairs discontinuities and keeps monotonic OBS-facing timestamps, but it does not wait for the normal `min_ms` fill level and it does not use buffered latency correction.

### 1. Jitter buffer (absorbs short-term network jitter)

A ring buffer sized in milliseconds, not bytes. Default settings:

| Parameter | Default | Purpose |
|---|---|---|
| Target | 120ms | Where the buffer tries to stay |
| Min | 60ms | Playback starts when buffer reaches this level |
| Max | 300ms | Upper bound before the buffer is considered overfull |

The buffer holds decoded audio (interleaved float PCM) regardless of the input codec. AAC, Opus, or anything else goes in; smooth PCM comes out.

Audio is output in chunks matching the decoded frame size (AAC = 1024 samples, Opus = 960). Matching the output chunk size to the codec frame size eliminates drift between OBS's internal smoothed timestamp advance and our push rate. The output loop drains multiple chunks per decoded frame if needed, keeping the buffer near its target.

### 2. Adaptive latency control (prevents latency creep)

Even with the drain loop, the buffer level drifts over time due to network throughput variation, clock mismatch, and decoder recovery events. The plugin avoids continuous time stretching because viewer-visible audio artifacts are worse than a small amount of silence or bounded latency movement.

Buffered mode uses native-rate playback with explicit recovery:

- If hidden/recovery backlog grows too far, the plugin can trim old buffered chunks before they become audible.
- Once audio is audible, the plugin does not trim old chunks just to chase the target buffer. Extra delay is preferable to an audible skip, pop, or cadence discontinuity.
- If latency runs away far beyond the configured buffer, the plugin uses a clean fade/flush/fade-in recovery boundary instead of repeated audible chunk drops.
- If the buffer underruns, the plugin inserts a short silence chunk so OBS timing stays monotonic.
- If timestamps or decoder state go bad, the plugin flushes damaged state and re-enters playback cleanly instead of trying to stretch through corruption.

The result should be transparent on stable links and non-artifacty on unstable links. If the source cannot make damaged audio sound natural, silence is preferred.

### 3. PTS repair (handles timestamp discontinuities)

Mobile connections drop packets. Cell tower handoffs cause gaps. SRT retransmissions arrive late. All of these produce gaps or jumps in the audio PTS (presentation timestamp) that confuse the decoder and cause audible artifacts.

The PTS repair system classifies gaps into three tiers:

| Gap size | Action | What it sounds like without repair |
|---|---|---|
| < 70ms | **Interpolate** — replace the PTS with the expected value (last PTS + last duration). The gap was probably just jitter. | Brief audio stutter or pop |
| 70ms – 2s | **Silence insertion** — keep the PTS but insert the appropriate duration of silence before the frame. Something was actually lost. | Loud click followed by audio jump |
| > 2s | **Full reset** — flush the buffer, re-arm the keyframe gate, restart from scratch. The stream has fundamentally changed. | Extended silence, possibly wrong audio |

Thresholds are configurable (Small Gap and Large Gap settings in the UI).

`pts_repairs` tracks non-normal PTS discontinuities. For tuning, use the split diagnostics: `pts_normalizations`, `pts_interpolations`, `silence_insertions`, `pts_resets`, `pts_last_gap_ms`, and `pts_max_gap_ms`. A high normalization count with low silence usually means frame-sized timestamp cadence smoothing, not packet-loss concealment.

### 4. Fade in/out (eliminates clicks on disconnect)

When the stream drops, the last audio chunk in the buffer gets a 50ms linear fade-out (gain ramp from 1.0 to 0.0). When the stream reconnects and audio resumes, the first chunk gets a matching fade-in. This prevents the sharp transient that sounds like a loud click/pop.

## Timestamp handling

OBS expects audio timestamps in its system clock domain (`os_gettime_ns()`). Live streams use PTS values in a stream-local epoch, and decoded frames may occasionally arrive with missing or damaged timestamps. Passing these raw causes OBS to report "audio is lagging" and restart the source repeatedly.

### Audio timestamps

The plugin uses a running timestamp counter anchored to `os_gettime_ns()` on first output. In buffered mode it advances by the decoded frame duration; in low-latency mode it advances by the actual emitted frame count so OBS async unbuffered mode stays close to real time.

Three properties keep this stable:

1. **Chunk size matches codec frame size** — output chunks are exactly one codec frame (AAC = 1024, Opus = 960 samples). OBS's internal smoothing advances `next_audio_ts_min` by `chunk_samples / mixer_rate` per push. When our PTS advance matches this exactly, there's zero drift and OBS always uses the fast `push_back` path (which doesn't reset `audio_ts`).

2. **Buffered mode still tracks real playout** — audio timestamps advance from the actual chunk cadence handed to OBS, while the plugin separately tracks the source-side end PTS for the same audio. Video sync uses that mapping instead of assuming a fixed buffer delay.

3. **Small wall-clock guardrails** — the running counter is periodically pulled back toward wall clock if it drifts too far ahead or too far away from real time. This is intentionally limited: buffered mode favors continuity, while low-latency mode is stricter about staying near wall clock.

The initial audio timestamp is set to `os_gettime_ns()` with no large offset. When decoded audio frames arrive without a usable PTS, the plugin falls back to `best_effort_timestamp` and only synthesizes continuity from the previous repaired PTS when necessary. Frames with no safe starting point are dropped instead of pushing broken timing into OBS.

### Video timestamps

Video uses a rebasing approach: the first frame's stream PTS is anchored to `os_gettime_ns()` via `video_sys_base` / `video_pts_base`. Subsequent frames compute their timestamp as `video_sys_base + (frame_pts_ns - video_pts_base)`, preserving the inter-frame timing from the stream.

Instead of a fixed audio offset, video is delayed by the current buffered-audio age when audio exists. That tracks the real state of the audio path better than always adding the configured target buffer.

If the computed timestamp drifts too far from wall clock, it is clamped rather than fully re-anchored. That avoids visible jumps while still preventing long freezes if the stream sends a bad future timestamp.

## What this means in practice

| Scenario | Media Source | IRL Source |
|---|---|---|
| Stable connection | Works fine, but adds seconds of latency | Buffered mode adds ~120ms, low-latency mode keeps the source much closer to real time |
| Brief packet loss (< 70ms) | Audio pop, possible stutter | Interpolated silently, inaudible |
| Cell tower handoff (100-500ms gap) | Loud click, audio jumps ahead | Silence inserted, smooth transition |
| Sender clock drift / slow latency creep | Buffer grows forever, latency increases | Buffered mode keeps native audio rate; audible-path latency can grow, and extreme runaway delay is recovered with a fade/flush/fade-in boundary |
| Connection drops and reconnects | Loud click on disconnect, possibly corrupted frames on reconnect | Fade out, clean reconnect, keyframe gate, fade in |
| Decoder corruption | Gray/corrupt flicker until manual restart | Last good frame is held, bad frames are skipped, decoder state is flushed on repeated errors |
| Long stream (hours) | Timestamp epoch causes OBS sync issues | Timestamps are repaired and anchored to system clock |

The tradeoff: buffered mode is more resilient to short stalls, but adds intentional latency. Low-latency mode reacts faster and works better with OBS async unbuffered audio, but it gives up most of that jitter cushion. For rough SRTLA field conditions, buffered mode should still be the default. Low-latency mode is there when absolute latency matters more than smoothing over short network wobble.
