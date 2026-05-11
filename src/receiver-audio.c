/*
 * obs-irl-source — IRL streaming source plugin for OBS
 * https://irlserver.com
 *
 * Copyright (C) 2026 Thomas Lekanger
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "receiver-internal.h"

#define AUDIO_RECOVERY_HOLD_US 1500000ULL
#define AUDIO_TRIM_TRIGGER_MS 90
#define AUDIO_CONCEAL_FADE_MS 8
#define AUDIO_SUSTAINED_TRIM_EXTRA_MS 100
#define AUDIO_SUSTAINED_TRIM_HOLD_US 4000000ULL
#define AUDIO_SUSTAINED_TRIM_COOLDOWN_US 10000000ULL
#define AUDIO_SUSTAINED_TRIM_MAX_CHUNKS 4

/* Grow a per-thread scratch buffer to at least `need` bytes. Returns
 * the buffer or NULL on OOM. The buffer is owned by the caller's
 * thread; no synchronisation here. */
static uint8_t *ensure_scratch(uint8_t **buf, size_t *cap, size_t need)
{
	if (need == 0)
		return *buf;
	if (need > *cap) {
		size_t new_cap = *cap ? *cap : 4096;
		while (new_cap < need)
			new_cap *= 2;
		uint8_t *next = realloc(*buf, new_cap);
		if (!next)
			return NULL;
		*buf = next;
		*cap = new_cap;
	}
	return *buf;
}

static void maybe_log_audio_timing_diag(struct irl_source *ctx)
{
	uint64_t now = os_gettime_ns();
	bool severe_lead = ctx->audio_last_obs_lead_ns > 150000000LL;
	bool severe_drift = ctx->audio_last_ts_drift_ns < -40000000LL ||
			    ctx->audio_last_ts_drift_ns > 40000000LL;

	if (!severe_lead && !severe_drift)
		return;
	if (now - ctx->last_audio_diag_time < 5000000000ULL)
		return;

	/* Throttle gates passed; only now do we acquire the buffer lock
	 * to read fill_ms. Avoids a per-pump lock acquire that almost
	 * always returns immediately because we don't actually log. */
	int fill_ms = audio_buffer_fill_ms_locked(&ctx->audio_buf);

	ctx->last_audio_diag_time = now;
	blog(LOG_WARNING,
	     "[irl-source] Audio timing diag: obs_lead=%lldms ts_drift=%lldms "
	     "fill=%dms target=%dms speed=%.3f chunk=%u@%u stream_chunk=%llums "
	     "obs_chunk=%llums underruns=%llu resync_skips=%llu "
	     "hidden_trims=%llu latency_trims=%llu quality_events=%llu "
	     "pll=%llu hard_resets=%llu repairs=%llu norm=%llu interp=%llu "
	     "silence=%llu resets=%llu last_gap=%dms max_gap=%dms",
	     (long long)(ctx->audio_last_obs_lead_ns / 1000000LL),
	     (long long)(ctx->audio_last_ts_drift_ns / 1000000LL), fill_ms,
	     ctx->config.buffer_target_ms, (double)ctx->current_speed,
	     ctx->audio_last_frames_out, ctx->audio_last_samples_per_sec,
	     (unsigned long long)(ctx->audio_last_chunk_stream_duration_ns /
				  1000000ULL),
	     (unsigned long long)(ctx->audio_last_chunk_obs_duration_ns /
				  1000000ULL),
	     (unsigned long long)ctx->audio_underruns,
	     (unsigned long long)ctx->audio_resync_skipped_chunks,
	     (unsigned long long)ctx->audio_hidden_trimmed_chunks,
	     (unsigned long long)ctx->audio_latency_trimmed_chunks,
	     (unsigned long long)ctx->audio_quality_events,
	     (unsigned long long)ctx->audio_pll_corrections,
	     (unsigned long long)ctx->audio_pll_hard_resets,
	     (unsigned long long)ctx->pts_repairs,
	     (unsigned long long)ctx->pts_normalizations,
	     (unsigned long long)ctx->pts_interpolations,
	     (unsigned long long)ctx->silence_insertions,
	     (unsigned long long)ctx->pts_resets, ctx->pts_last_gap_ms,
	     ctx->pts_max_gap_ms);
}

void irl_reset_stream_timing_state(struct irl_source *ctx)
{
	ctx->audio_ts_init = false;
	ctx->audio_pll_offset_ns = 0;
	ctx->audio_last_ts_drift_ns = 0;
	ctx->audio_last_obs_lead_ns = 0;
	ctx->audio_last_chunk_stream_duration_ns = 0;
	ctx->audio_last_chunk_obs_duration_ns = 0;
	ctx->audio_last_frames_out = 0;
	ctx->audio_last_samples_per_sec = 0;
	ctx->last_audio_diag_time = 0;
	ctx->audio_recovery_until_us = 0;
	ctx->audio_high_fill_since_us = 0;
	ctx->audio_last_sustained_trim_us = 0;
	ctx->video_ts_init = false;
	ctx->latest_audio_stream_pts_ns = 0;
	ctx->latest_audio_buffered_end_pts_ns = 0;
	ctx->latest_video_stream_pts_ns = 0;
	ctx->latest_audio_obs_end_ts_ns = 0;
	ctx->decoded_frame_samples = 0;
	ctx->startup_audio_warmup_remaining_ms = 0;
	ctx->audio_last_sample_channels = 0;
	ctx->audio_last_sample_valid = false;
	ctx->audio_last_output_sample_channels = 0;
	ctx->audio_last_output_sample_valid = false;
	ctx->audio_trim_crossfade_pending = false;
	ctx->audio_decode_errors = 0;
	ctx->video_decode_errors = 0;
	ctx->audio_last_decoder_flush_time_us = 0;
	ctx->video_last_decoder_flush_time_us = 0;
	ctx->audio_last_decoder_warning_time_us = 0;
	ctx->video_last_decoder_warning_time_us = 0;
	ctx->video_corrupted = false;
	ctx->video_skip_logged = false;
}

void irl_reset_audio_timing_state(struct irl_source *ctx)
{
	ctx->audio_ts_init = false;
	ctx->audio_pll_offset_ns = 0;
	ctx->audio_last_ts_drift_ns = 0;
	ctx->audio_last_obs_lead_ns = 0;
	ctx->audio_last_chunk_stream_duration_ns = 0;
	ctx->audio_last_chunk_obs_duration_ns = 0;
	ctx->audio_last_frames_out = 0;
	ctx->audio_last_samples_per_sec = 0;
	ctx->last_audio_diag_time = 0;
	ctx->audio_recovery_until_us = 0;
	ctx->audio_high_fill_since_us = 0;
	ctx->audio_last_sustained_trim_us = 0;
	ctx->latest_audio_stream_pts_ns = 0;
	ctx->latest_audio_buffered_end_pts_ns = 0;
	ctx->latest_audio_obs_end_ts_ns = 0;
	ctx->decoded_frame_samples = 0;
	ctx->startup_audio_warmup_remaining_ms = 0;
	ctx->audio_last_sample_channels = 0;
	ctx->audio_last_sample_valid = false;
	ctx->audio_last_output_sample_channels = 0;
	ctx->audio_last_output_sample_valid = false;
	ctx->audio_trim_crossfade_pending = false;
	ctx->audio_decode_errors = 0;
	ctx->audio_last_decoder_flush_time_us = 0;
	ctx->audio_last_decoder_warning_time_us = 0;
}

void irl_mark_audio_recovery(struct irl_source *ctx, uint64_t duration_us)
{
	uint64_t now_us = (uint64_t)av_gettime();
	uint64_t until_us = now_us + duration_us;

	if (until_us > ctx->audio_recovery_until_us)
		ctx->audio_recovery_until_us = until_us;
}

bool irl_audio_recovery_active(const struct irl_source *ctx)
{
	uint64_t now_us = (uint64_t)av_gettime();
	return ctx->audio_recovery_until_us != 0 &&
	       now_us < ctx->audio_recovery_until_us;
}

static int64_t audio_frame_pts(const AVFrame *frame)
{
	if (frame->best_effort_timestamp != AV_NOPTS_VALUE)
		return frame->best_effort_timestamp;
	if (frame->pts != AV_NOPTS_VALUE)
		return frame->pts;
	return AV_NOPTS_VALUE;
}

static int audio_frame_duration_ms(int samples, int sample_rate)
{
	if (samples <= 0 || sample_rate <= 0)
		return 0;

	int64_t ms = (int64_t)samples * 1000LL / sample_rate;
	if (ms <= 0)
		ms = 1;
	return (int)ms;
}

static int audio_conceal_fade_frames(int sample_rate, int max_frames)
{
	if (sample_rate <= 0 || max_frames <= 0)
		return 0;

	int frames = sample_rate * AUDIO_CONCEAL_FADE_MS / 1000;
	if (frames <= 0)
		frames = 1;
	return frames < max_frames ? frames : max_frames;
}

static void audio_remember_last_sample(struct irl_source *ctx,
				       const uint8_t *samples, int frames,
				       int channels)
{
	if (!samples || frames <= 0 || channels <= 0 ||
	    channels > (int)(sizeof(ctx->audio_last_sample) /
			     sizeof(ctx->audio_last_sample[0]))) {
		ctx->audio_last_sample_valid = false;
		ctx->audio_last_sample_channels = 0;
		return;
	}

	const float *pcm = (const float *)samples;
	const float *last = pcm + (size_t)(frames - 1) * channels;
	for (int ch = 0; ch < channels; ch++)
		ctx->audio_last_sample[ch] = last[ch];
	ctx->audio_last_sample_channels = channels;
	ctx->audio_last_sample_valid = true;
}

static void audio_apply_fade_in(uint8_t *samples, int frames, int channels,
				int sample_rate)
{
	int fade_frames = audio_conceal_fade_frames(sample_rate, frames);
	if (!samples || fade_frames <= 0 || channels <= 0)
		return;

	float *pcm = (float *)samples;
	for (int f = 0; f < fade_frames; f++) {
		float gain = (float)(f + 1) / (float)fade_frames;
		for (int ch = 0; ch < channels; ch++)
			pcm[(size_t)f * channels + ch] *= gain;
	}
}

static void audio_remember_last_output_sample(struct irl_source *ctx,
					      const uint8_t *samples,
					      int frames, int channels)
{
	if (!samples || frames <= 0 || channels <= 0 ||
	    channels > (int)(sizeof(ctx->audio_last_output_sample) /
			     sizeof(ctx->audio_last_output_sample[0]))) {
		ctx->audio_last_output_sample_valid = false;
		ctx->audio_last_output_sample_channels = 0;
		return;
	}

	const float *pcm = (const float *)samples;
	const float *last = pcm + (size_t)(frames - 1) * channels;
	for (int ch = 0; ch < channels; ch++)
		ctx->audio_last_output_sample[ch] = last[ch];
	ctx->audio_last_output_sample_channels = channels;
	ctx->audio_last_output_sample_valid = true;
}

static void audio_apply_trim_crossfade(struct irl_source *ctx, uint8_t *samples,
				       int frames, int channels,
				       int sample_rate)
{
	int fade_frames = audio_conceal_fade_frames(sample_rate, frames);
	if (!samples || fade_frames <= 0 || channels <= 0)
		return;

	float *pcm = (float *)samples;
	bool have_anchor = ctx->audio_last_output_sample_valid &&
			   ctx->audio_last_output_sample_channels == channels;

	for (int f = 0; f < fade_frames; f++) {
		float wet = (float)(f + 1) / (float)fade_frames;
		float dry = 1.0f - wet;
		for (int ch = 0; ch < channels; ch++) {
			float anchor = have_anchor ?
					       ctx->audio_last_output_sample[ch] :
					       0.0f;
			size_t idx = (size_t)f * channels + ch;
			pcm[idx] = anchor * dry + pcm[idx] * wet;
		}
	}
}

static void audio_shape_inserted_silence(struct irl_source *ctx,
					 uint8_t *silence, size_t silence_bytes)
{
	int channels = ctx->audio_buf.channels;
	int frame_size = ctx->audio_buf.frame_size;
	int sample_rate = ctx->audio_buf.sample_rate;
	if (!silence || silence_bytes == 0 || channels <= 0 ||
	    frame_size <= 0 || sample_rate <= 0)
		return;

	int frames = (int)(silence_bytes / (size_t)frame_size);
	int fade_frames = audio_conceal_fade_frames(sample_rate, frames);
	if (!ctx->audio_last_sample_valid ||
	    ctx->audio_last_sample_channels != channels || fade_frames <= 0)
		return;

	float *pcm = (float *)silence;
	for (int f = 0; f < fade_frames; f++) {
		float gain = 1.0f - (float)(f + 1) / (float)fade_frames;
		for (int ch = 0; ch < channels; ch++) {
			pcm[(size_t)f * channels + ch] =
				ctx->audio_last_sample[ch] * gain;
		}
	}
}

static int audio_expected_samples(const struct irl_source *ctx,
				  int64_t duration, int out_rate,
				  int fallback_samples)
{
	if (duration <= 0 || out_rate <= 0 || ctx->pts_state.tb_den <= 0)
		return fallback_samples;

	int64_t expected = av_rescale_q(duration,
					(AVRational){ctx->pts_state.tb_num,
						     ctx->pts_state.tb_den},
					(AVRational){1, out_rate});
	if (expected <= 0 || expected > INT_MAX)
		return fallback_samples;
	return (int)expected;
}

static int audio_soft_compensation_samples(const struct irl_source *ctx,
					   int64_t duration, int out_rate,
					   int actual_samples)
{
	int expected = audio_expected_samples(ctx, duration, out_rate,
					     actual_samples);
	int delta = expected - actual_samples;

	/* Let PTS repair handle real discontinuities. This is only for
	 * tiny per-frame drift, similar in spirit to a bounded aresample
	 * async correction. */
	if (delta < -8 || delta > 8)
		return 0;
	return delta;
}

static float compute_buffered_output_speed(struct irl_source *ctx, int fill_ms)
{
	UNUSED_PARAMETER(fill_ms);

	/* Viewer-quality rule: do not continuously retune OBS's audio
	 * sample rate. Even very small correction can sound phasey or
	 * unstable on voice. Latency recovery is handled by buffering,
	 * silence, and rare hidden-backlog trims instead. */
	ctx->current_speed = 1.0f;
	return ctx->current_speed;
}

uint64_t irl_next_audio_timestamp(struct irl_source *ctx, int base_samples,
				  int out_rate)
{
	int64_t frame_ns =
		(int64_t)base_samples * 1000000000LL / out_rate;
	int64_t startup_lead_ns =
		ctx->config.low_latency_audio ? 0 : frame_ns * 2;
	uint64_t now = os_gettime_ns();

	if (!ctx->config.low_latency_audio) {
		uint64_t target_ts =
			now + (uint64_t)(startup_lead_ns > 0 ? startup_lead_ns : 0);
		uint64_t queued_end_ts = ctx->latest_audio_obs_end_ts_ns;
		uint64_t audio_ts = 0;
		uint64_t resync_slack_ns =
			(uint64_t)(frame_ns > 0 ? frame_ns / 2 : 0);

		if (!ctx->audio_ts_init || queued_end_ts == 0) {
			audio_ts = target_ts;
			ctx->audio_ts_init = true;
		} else if (queued_end_ts + resync_slack_ns >= target_ts) {
			audio_ts = queued_end_ts;
		} else {
			ctx->audio_pll_corrections++;
			audio_ts = target_ts;
		}

		ctx->audio_sys_base = audio_ts;
		ctx->audio_pll_offset_ns = 0;
		ctx->audio_last_ts_drift_ns =
			(int64_t)audio_ts - (int64_t)now;
		return audio_ts;
	}

	if (!ctx->audio_ts_init) {
		int64_t initial_ts = (int64_t)now + startup_lead_ns;
		ctx->audio_sys_base = (uint64_t)(initial_ts > 0 ? initial_ts : 0);
		ctx->audio_pll_offset_ns = 0;
		ctx->audio_ts_init = true;
	}

	int64_t audio_ts_i64 =
		(int64_t)ctx->audio_sys_base + ctx->audio_pll_offset_ns;
	if (audio_ts_i64 < 0)
		audio_ts_i64 = 0;
	uint64_t audio_ts = (uint64_t)audio_ts_i64;
	int64_t drift = (int64_t)audio_ts - (int64_t)now;

	if (ctx->config.low_latency_audio) {
		if (drift < -10000000LL || drift > 10000000LL) {
			ctx->audio_pll_corrections++;
			int64_t base = (int64_t)now - ctx->audio_pll_offset_ns;
			ctx->audio_sys_base = (uint64_t)(base > 0 ? base : 0);
			audio_ts = now;
		}
	} else if (drift > 30000000LL) {
		ctx->audio_pll_corrections++;
		ctx->audio_pll_offset_ns -= frame_ns;
		audio_ts -= frame_ns;
	} else if (drift < -70000000LL) {
		ctx->audio_pll_corrections++;
		int64_t target_ts = (int64_t)now + startup_lead_ns;
		int64_t base = target_ts - ctx->audio_pll_offset_ns;
		ctx->audio_sys_base = (uint64_t)(base > 0 ? base : 0);
		audio_ts = (uint64_t)(target_ts > 0 ? target_ts : 0);
	}

	drift = (int64_t)audio_ts - (int64_t)now;
	if (drift < -500000000LL || drift > 500000000LL) {
		ctx->audio_pll_hard_resets++;
		ctx->audio_sys_base = now;
		ctx->audio_pll_offset_ns = 0;
		audio_ts = now;
	}

	ctx->audio_last_ts_drift_ns = drift;
	ctx->audio_pll_offset_ns += frame_ns;
	return audio_ts;
}

static void finalize_audio_output(struct irl_source *ctx,
				  const struct obs_source_audio *obs_audio,
				  int64_t chunk_pts_ns,
				  uint64_t stream_duration_ns)
{
	ctx->latest_audio_buffered_end_pts_ns =
		chunk_pts_ns + (int64_t)stream_duration_ns;

	if (obs_audio->samples_per_sec > 0) {
		uint64_t audio_duration_ns =
			(uint64_t)obs_audio->frames * 1000000000ULL /
			(uint64_t)obs_audio->samples_per_sec;
		ctx->latest_audio_obs_end_ts_ns =
			obs_audio->timestamp + audio_duration_ns;
		ctx->audio_last_chunk_obs_duration_ns = audio_duration_ns;
	} else {
		ctx->latest_audio_obs_end_ts_ns = obs_audio->timestamp;
		ctx->audio_last_chunk_obs_duration_ns = 0;
	}

	ctx->audio_last_chunk_stream_duration_ns = stream_duration_ns;
	ctx->audio_last_frames_out = obs_audio->frames;
	ctx->audio_last_samples_per_sec = obs_audio->samples_per_sec;

	uint64_t after_output = os_gettime_ns();
	if (ctx->latest_audio_obs_end_ts_ns > after_output) {
		ctx->audio_last_obs_lead_ns =
			(int64_t)(ctx->latest_audio_obs_end_ts_ns - after_output);
	} else {
		ctx->audio_last_obs_lead_ns = 0;
	}

	maybe_log_audio_timing_diag(ctx);
	ctx->total_audio_frames++;
}

static bool maybe_resync_audio_buffer(struct irl_source *ctx, int64_t peek,
				      int fill_ms, int chunk_count)
{
	UNUSED_PARAMETER(peek);
	UNUSED_PARAMETER(fill_ms);
	UNUSED_PARAMETER(chunk_count);

	if (!ctx->config.low_latency_audio)
		return false;
	if (ctx->latest_audio_stream_pts_ns == 0 || chunk_count <= 1)
		return false;

	int64_t expected = ctx->latest_audio_stream_pts_ns -
			   (int64_t)fill_ms * 1000000LL;
	int64_t gap_ns = peek - expected;
	if (gap_ns >= -50000000LL)
		return false;

	ctx->audio_resync_skipped_chunks += (uint64_t)chunk_count;
	ctx->audio_quality_events++;
	audio_buffer_flush(&ctx->audio_buf);
	irl_reset_audio_timing_state(ctx);
	irl_mark_audio_recovery(ctx, AUDIO_RECOVERY_HOLD_US);
	ctx->current_speed = 1.0f;
	blog(LOG_INFO,
	     "[irl-source] Audio re-sync: flushed %d stale chunks (gap=%lldms)",
	     chunk_count, (long long)(gap_ns / 1000000));
	return true;
}

static bool should_hide_audio_backlog(const struct irl_source *ctx)
{
	return !ctx->config.low_latency_audio &&
	       (!ctx->audio_ts_init || ctx->fade_in_pending ||
		irl_audio_recovery_active(ctx));
}

static bool maybe_trim_hidden_audio_backlog(struct irl_source *ctx, int fill_ms,
					    int chunk_count)
{
	if (!ctx->config.adaptive_speed)
		return false;
	if (!should_hide_audio_backlog(ctx))
		return false;
	if (chunk_count <= 1)
		return false;

	int chunk_ms = 0;
	if (ctx->audio_buf.sample_rate > 0 && ctx->decoded_frame_samples > 0) {
		chunk_ms = (int)((int64_t)ctx->decoded_frame_samples * 1000LL /
				 ctx->audio_buf.sample_rate);
	}
	if (chunk_ms <= 0)
		chunk_ms = 21;

	int keep_ms = ctx->config.buffer_target_ms + chunk_ms;
	if (fill_ms <= keep_ms + AUDIO_TRIM_TRIGGER_MS)
		return false;

	int post_fill_ms = fill_ms;
	int post_chunks = chunk_count;
	int trimmed = audio_buffer_trim_to_keep_ms(&ctx->audio_buf, keep_ms,
						   1, &post_fill_ms,
						   &post_chunks);
	if (trimmed <= 0)
		return false;

	ctx->audio_resync_skipped_chunks += (uint64_t)trimmed;
	ctx->audio_hidden_trimmed_chunks += (uint64_t)trimmed;
	blog(LOG_INFO,
	     "[irl-source] Audio trim: dropped %d hidden buffered chunk%s before playback (fill=%dms target=%dms)",
	     trimmed, trimmed == 1 ? "" : "s", post_fill_ms,
	     ctx->config.buffer_target_ms);
	return true;
}

static bool maybe_trim_sustained_audio_latency(struct irl_source *ctx,
					       int fill_ms, int chunk_count)
{
	if (!ctx->config.adaptive_speed || ctx->config.low_latency_audio)
		return false;
	if (should_hide_audio_backlog(ctx))
		return false;
	if (chunk_count <= 3)
		return false;

	int trim_threshold_ms =
		ctx->config.buffer_target_ms + AUDIO_SUSTAINED_TRIM_EXTRA_MS;
	int chunk_ms = 0;
	if (ctx->audio_buf.sample_rate > 0 && ctx->decoded_frame_samples > 0) {
		chunk_ms = (int)((int64_t)ctx->decoded_frame_samples * 1000LL /
				 ctx->audio_buf.sample_rate);
	}
	if (chunk_ms <= 0)
		chunk_ms = 21;
	int clear_threshold_ms = ctx->config.buffer_target_ms + chunk_ms;

	uint64_t now_us = (uint64_t)av_gettime();
	if (fill_ms <= clear_threshold_ms) {
		ctx->audio_high_fill_since_us = 0;
		return false;
	}
	if (fill_ms <= trim_threshold_ms)
		return false;

	if (ctx->audio_high_fill_since_us == 0) {
		ctx->audio_high_fill_since_us = now_us;
		return false;
	}
	if (now_us - ctx->audio_high_fill_since_us <
	    AUDIO_SUSTAINED_TRIM_HOLD_US) {
		return false;
	}
	if (ctx->audio_last_sustained_trim_us != 0 &&
	    now_us - ctx->audio_last_sustained_trim_us <
		    AUDIO_SUSTAINED_TRIM_COOLDOWN_US) {
		return false;
	}

	int desired_fill_ms = trim_threshold_ms - chunk_ms;
	if (desired_fill_ms < clear_threshold_ms)
		desired_fill_ms = clear_threshold_ms;
	int trim_chunks = (fill_ms - desired_fill_ms + chunk_ms - 1) / chunk_ms;
	if (trim_chunks < 1)
		trim_chunks = 1;
	if (trim_chunks > AUDIO_SUSTAINED_TRIM_MAX_CHUNKS)
		trim_chunks = AUDIO_SUSTAINED_TRIM_MAX_CHUNKS;
	if (trim_chunks > chunk_count - 3)
		trim_chunks = chunk_count - 3;
	if (trim_chunks <= 0)
		return false;

	for (int i = 0; i < trim_chunks; i++)
		audio_buffer_skip_chunk(&ctx->audio_buf);

	int post_fill_ms = fill_ms;
	int post_chunk_count = chunk_count;
	int64_t post_peek = 0;
	audio_buffer_peek_state(&ctx->audio_buf, &post_peek, &post_fill_ms,
				&post_chunk_count);

	ctx->audio_trim_crossfade_pending = true;
	ctx->audio_resync_skipped_chunks += (uint64_t)trim_chunks;
	ctx->audio_latency_trimmed_chunks += (uint64_t)trim_chunks;
	ctx->audio_quality_events++;
	ctx->audio_last_sustained_trim_us = now_us;
	ctx->audio_high_fill_since_us = 0;
	blog(LOG_INFO,
	     "[irl-source] Audio latency trim: dropped %d old buffered chunk%s (fill=%dms->%dms target=%dms)",
	     trim_chunks, trim_chunks == 1 ? "" : "s", fill_ms,
	     post_fill_ms, ctx->config.buffer_target_ms);
	return true;
}

bool irl_pump_audio_once(struct irl_source *ctx)
{
	bool low_latency = ctx->config.low_latency_audio;
	int out_rate = ctx->audio_buf.sample_rate;
	int out_channels = ctx->audio_buf.channels;
	int bytes_per_sample = ctx->audio_buf.bytes_per_sample;
	int base_samples = ctx->decoded_frame_samples;

	if (!ctx->audio_buf.data || out_rate <= 0 || out_channels <= 0 ||
	    bytes_per_sample <= 0)
		return false;
	if (base_samples <= 0)
		base_samples = 960;

	uint64_t desired_lead_ns =
		low_latency ? 0
			    : ((uint64_t)base_samples * 1000000000ULL /
			       (uint64_t)out_rate) *
				      2ULL;
	uint64_t now = os_gettime_ns();
	if (ctx->latest_audio_obs_end_ts_ns != 0 &&
	    ctx->latest_audio_obs_end_ts_ns > now + desired_lead_ns) {
		return false;
	}

	int64_t peek = 0;
	int fill_ms = 0;
	int chunk_count = 0;
	bool has_audio = audio_buffer_peek_state(&ctx->audio_buf, &peek,
							 &fill_ms,
							 &chunk_count);
	if (has_audio &&
	    maybe_trim_hidden_audio_backlog(ctx, fill_ms, chunk_count)) {
		return true;
	}
	if (has_audio &&
	    maybe_trim_sustained_audio_latency(ctx, fill_ms, chunk_count)) {
		return true;
	}

	int required_fill_ms = low_latency ? 0 : ctx->audio_buf.min_ms;
	if (!low_latency && ctx->latest_audio_buffered_end_pts_ns == 0)
		required_fill_ms = ctx->audio_buf.target_ms;

	if (!(low_latency ? has_audio : (fill_ms >= required_fill_ms))) {
		if (!low_latency && !has_audio &&
		    ctx->latest_audio_buffered_end_pts_ns > 0) {
			ctx->audio_underruns++;
			ctx->audio_quality_events++;
			irl_mark_audio_recovery(ctx, AUDIO_RECOVERY_HOLD_US);
			size_t silence_bytes =
				(size_t)base_samples * ctx->audio_buf.frame_size;
			uint8_t *silence_buf = ensure_scratch(
				&ctx->audio_pump_scratch,
				&ctx->audio_pump_scratch_capacity,
				silence_bytes);
			if (!silence_buf)
				return false;
			memset(silence_buf, 0, silence_bytes);

			struct obs_source_audio obs_audio = {0};
			obs_audio.data[0] = silence_buf;
			obs_audio.frames = (uint32_t)base_samples;
			obs_audio.format = AUDIO_FORMAT_FLOAT;
			obs_audio.speakers =
				(enum speaker_layout)ctx->audio_buf.channels;
			obs_audio.timestamp = irl_next_audio_timestamp(
				ctx, base_samples, ctx->audio_buf.sample_rate);
			obs_audio.samples_per_sec =
				(uint32_t)ctx->audio_buf.sample_rate;
			obs_source_output_audio(ctx->source, &obs_audio);

			uint64_t stream_duration_ns =
				(uint64_t)obs_audio.frames * 1000000000ULL /
				(uint64_t)ctx->audio_buf.sample_rate;
			int64_t chunk_pts_ns =
				ctx->latest_audio_buffered_end_pts_ns;
			finalize_audio_output(ctx, &obs_audio, chunk_pts_ns,
					      stream_duration_ns);
			return true;
		}
		return false;
	}

	if (has_audio &&
	    maybe_resync_audio_buffer(ctx, peek, fill_ms, chunk_count)) {
		return false;
	}

	uint8_t *out_buf = NULL;
	int64_t chunk_pts_ns = 0;
	uint64_t stream_duration_ns = 0;
	uint32_t frames_out = 0;
	compute_buffered_output_speed(ctx, fill_ms);
	uint32_t obs_rate = (uint32_t)out_rate;

	int chunk_samples = base_samples;
	size_t frame_bytes =
		(size_t)chunk_samples * ctx->audio_buf.frame_size;
	out_buf = ensure_scratch(&ctx->audio_pump_scratch,
				 &ctx->audio_pump_scratch_capacity, frame_bytes);
	if (!out_buf)
		return false;

	size_t got = audio_buffer_read_pts(&ctx->audio_buf, out_buf,
					   frame_bytes, &chunk_pts_ns);
	if (got == 0)
		return false;

	frames_out = (uint32_t)(got /
				(out_channels * bytes_per_sample));
	if (out_rate > 0) {
		stream_duration_ns =
			(uint64_t)frames_out * 1000000000ULL /
			(uint64_t)out_rate;
	}

	if (ctx->fade_in_pending) {
		ctx->fade_in_frames_remaining =
			out_rate * IRL_FADE_DURATION_MS / 1000;
		ctx->fade_in_pending = false;
	}
	if (ctx->fade_in_frames_remaining > 0) {
		int total_fade = out_rate * IRL_FADE_DURATION_MS / 1000;
		float *s = (float *)out_buf;
		int nf = (int)frames_out;
		for (int f = 0; f < nf && ctx->fade_in_frames_remaining > 0;
		     f++) {
			int into = total_fade - ctx->fade_in_frames_remaining;
			float gain = (float)into / (float)total_fade;
			for (int ch = 0; ch < out_channels; ch++)
				s[f * out_channels + ch] *= gain;
			ctx->fade_in_frames_remaining--;
		}
	}
	if (ctx->audio_trim_crossfade_pending) {
		audio_apply_trim_crossfade(ctx, out_buf, (int)frames_out,
					   out_channels, out_rate);
		ctx->audio_trim_crossfade_pending = false;
	}

	int ts_samples = low_latency ? (int)frames_out : base_samples;
	if (ts_samples <= 0)
		ts_samples = base_samples;

	struct obs_source_audio obs_audio = {0};
	obs_audio.data[0] = out_buf;
	obs_audio.frames = frames_out;
	obs_audio.format = AUDIO_FORMAT_FLOAT;
	obs_audio.speakers = (enum speaker_layout)out_channels;
	obs_audio.timestamp = irl_next_audio_timestamp(ctx, ts_samples,
							 out_rate);
	obs_audio.samples_per_sec = obs_rate;

	obs_source_output_audio(ctx->source, &obs_audio);
	audio_remember_last_output_sample(ctx, out_buf, (int)frames_out,
					  out_channels);
	finalize_audio_output(ctx, &obs_audio, chunk_pts_ns, stream_duration_ns);
	return true;
}

void irl_handle_audio_frame(struct irl_source *ctx, AVFrame *frame)
{
	AVStream *as = NULL;
	if (ctx->fmt_ctx && ctx->audio_stream_idx >= 0)
		as = ctx->fmt_ctx->streams[ctx->audio_stream_idx];

	int out_channels = frame->ch_layout.nb_channels;
	int out_rate = frame->sample_rate;
	int bytes_per_sample = sizeof(float);

	if (ctx->audio_buf.sample_rate != out_rate ||
	    ctx->audio_buf.channels != out_channels) {
		pthread_mutex_lock(&ctx->audio_state_lock);
		bool reconfigured = true;
		if (ctx->audio_buf.data) {
			reconfigured = audio_buffer_reconfigure(
				&ctx->audio_buf, out_rate, out_channels,
				bytes_per_sample,
				ctx->config.buffer_target_ms,
				ctx->config.buffer_min_ms,
				ctx->config.buffer_max_ms);
		} else {
			audio_buffer_init(&ctx->audio_buf, out_rate,
					  out_channels, bytes_per_sample,
					  ctx->config.buffer_target_ms,
					  ctx->config.buffer_min_ms,
					  ctx->config.buffer_max_ms);
		}
		ctx->audio_ts_init = false;
		ctx->audio_pll_offset_ns = 0;
		ctx->latest_audio_buffered_end_pts_ns = 0;
		ctx->latest_audio_stream_pts_ns = 0;
		ctx->latest_audio_obs_end_ts_ns = 0;
		ctx->startup_audio_warmup_remaining_ms =
			IRL_STARTUP_AUDIO_WARMUP_MS;
		pthread_mutex_unlock(&ctx->audio_state_lock);
		if (!reconfigured)
			return;
	}

	int64_t input_pts = audio_frame_pts(frame);
	if (input_pts == AV_NOPTS_VALUE) {
		if (ctx->pts_state.initialised) {
			input_pts = ctx->pts_state.last_pts +
				    ctx->pts_state.last_duration;
		} else {
			blog(LOG_WARNING,
			     "[irl-source] Dropping audio frame without valid PTS");
			return;
		}
	}

	int64_t duration = frame->duration;
	if (duration <= 0 && as && out_rate > 0 && frame->nb_samples > 0) {
		duration = av_rescale_q(frame->nb_samples,
					(AVRational){1, out_rate},
					as->time_base);
	}
	if (duration <= 0)
		duration = 1;

	int64_t corrected_pts;
	int silence_ms = 0;
	enum pts_action action = pts_repair_evaluate(
		&ctx->pts_state, input_pts, duration, &corrected_pts,
		&silence_ms);
	bool inserted_silence = false;

	int frame_ms = audio_frame_duration_ms(frame->nb_samples, out_rate);
	if (ctx->startup_audio_warmup_remaining_ms > 0) {
		ctx->startup_audio_warmup_remaining_ms -= frame_ms;
		if (ctx->startup_audio_warmup_remaining_ms < 0)
			ctx->startup_audio_warmup_remaining_ms = 0;
		return;
	}

	if (action == PTS_ACTION_SILENCE && silence_ms > 0) {
		size_t silence_bytes =
			audio_buffer_ms_to_bytes(&ctx->audio_buf, silence_ms);
		uint8_t *silence = ensure_scratch(
			&ctx->audio_resample_scratch,
			&ctx->audio_resample_scratch_capacity, silence_bytes);
		if (silence) {
			memset(silence, 0, silence_bytes);
			audio_shape_inserted_silence(ctx, silence,
						     silence_bytes);
			int64_t silence_pts_ns =
				av_rescale_q(corrected_pts,
					     (AVRational){ctx->pts_state.tb_num,
							  ctx->pts_state.tb_den},
					     (AVRational){1, 1000000000}) -
				(int64_t)silence_ms * 1000000LL;
			if (silence_pts_ns < 0)
				silence_pts_ns = 0;
			audio_buffer_write_pts(&ctx->audio_buf, silence,
					       silence_bytes, silence_pts_ns);
			ctx->silence_insertions++;
			ctx->audio_quality_events++;
			inserted_silence = true;
		}
	} else if (action == PTS_ACTION_RESET) {
		pthread_mutex_lock(&ctx->audio_state_lock);
		audio_buffer_flush(&ctx->audio_buf);
		irl_reset_stream_timing_state(ctx);
		irl_mark_audio_recovery(ctx, AUDIO_RECOVERY_HOLD_US);
		ctx->audio_quality_events++;
		pthread_mutex_unlock(&ctx->audio_state_lock);
	}

	if (action != PTS_ACTION_PASS) {
		ctx->pts_last_gap_ms = ctx->pts_state.last_action_gap_ms;
		if (ctx->pts_last_gap_ms > ctx->pts_max_gap_ms)
			ctx->pts_max_gap_ms = ctx->pts_last_gap_ms;

		bool frame_sized_normalization =
			action == PTS_ACTION_INTERPOLATE && frame_ms > 0 &&
			ctx->pts_last_gap_ms <= frame_ms + 2;
		if (frame_sized_normalization) {
			ctx->pts_normalizations++;
		} else {
			ctx->pts_repairs++;
			if (action == PTS_ACTION_INTERPOLATE)
				ctx->pts_interpolations++;
		}
		if (action == PTS_ACTION_RESET)
			ctx->pts_resets++;
	}

	uint8_t *interleaved = NULL;
	int out_samples = frame->nb_samples;

	if (frame->format != AV_SAMPLE_FMT_FLT) {
		if (!ctx->swr_ctx || ctx->swr_in_rate != frame->sample_rate ||
		    ctx->swr_in_channels != frame->ch_layout.nb_channels ||
		    ctx->swr_in_format != frame->format) {
			if (ctx->swr_ctx)
				swr_free(&ctx->swr_ctx);
			AVChannelLayout out_layout;
			av_channel_layout_default(&out_layout, out_channels);
			if (swr_alloc_set_opts2(&ctx->swr_ctx, &out_layout,
						AV_SAMPLE_FMT_FLT, out_rate,
						&frame->ch_layout,
						frame->format,
						frame->sample_rate, 0,
						NULL) < 0 ||
			    !ctx->swr_ctx) {
				av_channel_layout_uninit(&out_layout);
				return;
			}
			if (swr_init(ctx->swr_ctx) < 0) {
				swr_free(&ctx->swr_ctx);
				av_channel_layout_uninit(&out_layout);
				return;
			}
			av_channel_layout_uninit(&out_layout);
			ctx->swr_in_rate = frame->sample_rate;
			ctx->swr_in_channels = frame->ch_layout.nb_channels;
			ctx->swr_in_format = frame->format;
		}

		int soft_comp_samples = audio_soft_compensation_samples(
			ctx, duration, out_rate, frame->nb_samples);
		if (soft_comp_samples != 0) {
			swr_set_compensation(ctx->swr_ctx, soft_comp_samples,
					     frame->nb_samples);
		}

		int max_out = swr_get_out_samples(ctx->swr_ctx,
						  frame->nb_samples);
		if (soft_comp_samples < 0)
			soft_comp_samples = -soft_comp_samples;
		max_out += soft_comp_samples + 32;
		size_t need = (size_t)max_out * out_channels * bytes_per_sample;
		interleaved = ensure_scratch(&ctx->audio_resample_scratch,
					     &ctx->audio_resample_scratch_capacity,
					     need);
		if (!interleaved)
			return;

		out_samples = swr_convert(ctx->swr_ctx, &interleaved, max_out,
					  (const uint8_t **)frame->extended_data,
					  frame->nb_samples);
		if (out_samples <= 0)
			return;
	} else {
		interleaved = frame->data[0];
	}

	size_t data_bytes =
		(size_t)out_samples * out_channels * bytes_per_sample;
	if (inserted_silence)
		audio_apply_fade_in(interleaved, out_samples, out_channels,
				    out_rate);
	if (ctx->config.wait_for_keyframe && ctx->video_stream_idx >= 0 &&
	    !ctx->first_keyframe_received) {
		return;
	}

	int64_t frame_pts_ns = av_rescale_q(
		corrected_pts,
		(AVRational){ctx->pts_state.tb_num, ctx->pts_state.tb_den},
		(AVRational){1, 1000000000});
	audio_buffer_write_pts(&ctx->audio_buf, interleaved, data_bytes,
			       frame_pts_ns);
	audio_remember_last_sample(ctx, interleaved, out_samples,
				   out_channels);

	pthread_mutex_lock(&ctx->audio_state_lock);
	ctx->latest_audio_stream_pts_ns = frame_pts_ns;
	ctx->decoded_frame_samples = out_samples;
	pthread_mutex_unlock(&ctx->audio_state_lock);
}
