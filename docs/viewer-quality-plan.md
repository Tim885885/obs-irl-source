# Viewer-quality plan

The plugin should optimize for what viewers hear and see during bad IRL signal.
Latency matters, but it is secondary to avoiding audio artifacts and gray/corrupt
video output.

## Policy

- Prefer silence over jittery, glitchy, metallic, or artifacty audio.
- Prefer holding the last good video frame over gray/corrupt frames.
- Prefer bounded latency movement over continuous audio stretching, while
  preserving the plugin's latency advantage over multi-second Media Source
  buffering.
- Keep recovery behavior visible in logs and stats before tuning thresholds.

## Phase 1: Make recovery observable

- Split aggregate PTS repair telemetry into normalization, interpolation,
  silence, reset, last gap, and max gap counters.
- Keep the old `pts_repairs` counter for script compatibility.
- Log buffer fill, underruns, trims, OBS lead, timestamp drift,
  and the split PTS repair counters together.

## Phase 2: Tune audio for viewer quality

- Treat small timestamp jitter as timestamp repair, not inserted silence.
- Treat real medium gaps as silence, not time compression.
- Keep buffered audio at native rate by default.
- If latency needs recovery, prefer hidden-backlog trimming, sustained-high-fill
  single-chunk trims, or silence over continuous rate correction.
- Insert silence on underrun so OBS timestamps remain monotonic.

## Phase 3: Tune video for viewer quality

- Keep first-keyframe gating on by default.
- Hold the last good frame during decoder corruption.
- Flush damaged decoder state conservatively so one bad packet does not cause a
  reset storm.
- Prefer smooth frame cadence and last-good-frame hold over gray frame output.

## Phase 4: Validate with bad-signal logs

Use live lossy SRT logs to check:

- `silence_insertions` rises only when there are real medium audio gaps or
  underruns.
- `pts_normalizations` can be high without audible artifacts.
- `pts_interpolations` should be watched together with gap size and audio
  quality.
- `pts_resets` stays rare.
- `speed` stays at 1.000 in buffered mode unless a future explicitly tested
  correction path proves inaudible.
- `resync_skips` happens only during hidden/recovery backlog cleanup.
- `Audio latency trim` logs appear only after sustained high fill and should be
  spaced by the cooldown.
- Video corruption logs do not imply audio corruption unless audio decoder or PTS
  diagnostics also show damage.
