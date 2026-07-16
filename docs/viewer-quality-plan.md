# Viewer-quality policy

The plugin optimizes for what viewers hear and see during bad IRL signal.
Latency matters, but it is secondary to avoiding audio artifacts, gray frames,
and video cadence freezes.

## Policy

- Prefer silence over jittery, glitchy, metallic, or artifacty audio.
- Prefer timestamped damaged frames over cadence freezes; avoid gray/blank
  frames and decoder reset storms.
- Prefer bounded latency movement over continuous audio stretching, while
  preserving the plugin's latency advantage over multi-second Media Source
  buffering.
- Keep recovery behavior visible in logs and stats.

## Recovery is observable

PTS repair telemetry is split into separate counters (normalization,
interpolation, silence, reset, last gap, max gap) so each recovery mechanism
can be tuned independently. The aggregate `pts_repairs` counter is kept for
script compatibility. The periodic stats log line reports buffer fill,
underruns, trims, OBS lead, timing state, and the split PTS counters together,
so a single log line describes the health of the whole path.

## Audio behavior

- Small timestamp jitter is treated as timestamp repair (interpolation), not
  inserted silence.
- Real medium gaps get silence insertion, not time compression.
- Buffered audio stays near native rate by default. Steady-state latency
  recovery is done with bounded speed correction (build at -2%, drain at up to
  +5%), which is smoothed and less audible than skips or pops.
- Audible buffered audio is never trimmed just to reduce delay. Hidden-backlog
  trimming runs only before playback primes (nothing was audible yet).
- Underruns emit shaped concealment silence so OBS timestamps remain monotonic.

## Video behavior

- First-keyframe gating is on by default.
- Timestamped damaged frames are passed through during decoder corruption so
  video cadence stays smooth.
- Damaged decoder state is flushed conservatively (only after repeated
  consecutive errors, with a cooldown) so one bad packet does not cause a reset
  storm.
- Smooth frame cadence is preferred over last-good-frame freezes; gray frame
  output is avoided.

## Reading bad-signal logs

When validating against live lossy SRT logs:

- `silence_insertions` should rise only on real medium audio gaps or underruns.
- `pts_normalizations` can be high without audible artifacts (frame-sized
  cadence smoothing).
- `pts_interpolations` should be read together with gap size and audio quality.
- `pts_resets` should stay rare.
- `speed` stays near 1.000 in buffered mode; any deviation should be smooth and
  correlated with high/low fill.
- `resync_skips` happens only during low-latency resync or hidden/recovery
  backlog cleanup.
- `Audio trim` logs are hidden/recovery cleanup only, before old chunks become
  audible.
- Video corruption logs do not imply audio corruption unless audio decoder or
  PTS diagnostics also show damage.
