/*
 * obs-irl-source — IRL streaming source plugin for OBS
 * https://irlserver.com
 *
 * Copyright (C) 2026 Thomas Lekanger
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <limits.h>
#include <math.h>
#include <stdlib.h>

#include "receiver-internal.h"

#define AUDIO_RECOVERY_HOLD_US 1500000ULL
#define AUDIO_TRIM_COOLDOWN_US 750000ULL
#define AUDIO_TRIM_TRIGGER_MS 90

static void maybe_log_audio_timing_diag(struct irl_source *ctx, int fill_ms)
{
	uint64_t now = os_gettime_ns();
	bool severe_lead = ctx->audio_last_obs_lead_ns > 150000000LL;
	bool severe_drift = ctx->audio_last_ts_drift_ns < -40000000LL ||
			    ctx->audio_last_ts_drift_ns > 40000000LL;

	if (!severe_lead && !severe_drift)
		return;
	if (now - ctx->last_audio_diag_time < 5000000000ULL)
		return;

	ctx->last_audio_diag_time = now;
	blog(LOG_WARNING,
	     "[irl-source] Audio timing diag: obs_lead=%lldms ts_drift=%lldms "
	     "fill=%dms speed=%.3f chunk=%u@%u stream_chunk=%llums "
	     "obs_chunk=%llums underruns=%llu resync_skips=%llu "
	     "pll=%llu hard_resets=%llu repairs=%llu silence=%llu",
	     (long long)(ctx->audio_last_obs_lead_ns / 1000000LL),
	     (long long)(ctx->audio_last_ts_drift_ns / 1000000LL), fill_ms,
	     (double)ctx->current_speed, ctx->audio_last_frames_out,
	     ctx->audio_last_samples_per_sec,
	     (unsigned long long)(ctx->audio_last_chunk_stream_duration_ns /
				  1000000ULL),
	     (unsigned long long)(ctx->audio_last_chunk_obs_duration_ns /
				  1000000ULL),
	     (unsigned long long)ctx->audio_underruns,
	     (unsigned long long)ctx->audio_resync_skipped_chunks,
	     (unsigned long long)ctx->audio_pll_corrections,
	     (unsigned long long)ctx->audio_pll_hard_resets,
	     (unsigned long long)ctx->pts_repairs,
	     (unsigned long long)ctx->silence_insertions);
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
	ctx->audio_last_trim_time_us = 0;
	ctx->video_ts_init = false;
	ctx->latest_audio_stream_pts_ns = 0;
	ctx->latest_audio_buffered_end_pts_ns = 0;
	ctx->latest_video_stream_pts_ns = 0;
	ctx->latest_audio_obs_end_ts_ns = 0;
	ctx->decoded_frame_samples = 0;
	ctx->startup_audio_warmup_remaining_ms = 0;
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
	ctx->audio_last_trim_time_us = 0;
	ctx->latest_audio_stream_pts_ns = 0;
	ctx->latest_audio_buffered_end_pts_ns = 0;
	ctx->latest_audio_obs_end_ts_ns = 0;
	ctx->decoded_frame_samples = 0;
	ctx->startup_audio_warmup_remaining_ms = 0;
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

	maybe_log_audio_timing_diag(ctx,
				    audio_buffer_fill_ms_locked(&ctx->audio_buf));
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
	audio_buffer_flush(&ctx->audio_buf);
	irl_reset_audio_timing_state(ctx);
	irl_mark_audio_recovery(ctx, AUDIO_RECOVERY_HOLD_US);
	ctx->current_speed = 1.0f;
	blog(LOG_INFO,
	     "[irl-source] Audio re-sync: flushed %d stale chunks (gap=%lldms)",
	     chunk_count, (long long)(gap_ns / 1000000));
	return true;
}

static bool maybe_trim_audio_buffer(struct irl_source *ctx, int fill_ms,
				    int chunk_count)
{
	if (!ctx->config.adaptive_speed || ctx->config.low_latency_audio)
		return false;
	if (irl_audio_recovery_active(ctx))
		return false;
	if (chunk_count <= 1)
		return false;

	int trim_threshold_ms =
		ctx->config.buffer_target_ms + AUDIO_TRIM_TRIGGER_MS;
	if (fill_ms < trim_threshold_ms)
		return false;

	uint64_t now_us = (uint64_t)av_gettime();
	if (ctx->audio_last_trim_time_us != 0 &&
	    now_us - ctx->audio_last_trim_time_us < AUDIO_TRIM_COOLDOWN_US) {
		return false;
	}

	audio_buffer_skip_chunk(&ctx->audio_buf);
	ctx->audio_last_trim_time_us = now_us;
	ctx->audio_resync_skipped_chunks++;
	ctx->fade_in_pending = true;
	ctx->fade_in_frames_remaining = 0;
	blog(LOG_INFO,
	     "[irl-source] Audio trim: dropped one buffered chunk (fill=%dms target=%dms)",
	     fill_ms, ctx->config.buffer_target_ms);
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
	int required_fill_ms = low_latency ? 0 : ctx->audio_buf.min_ms;
	if (!low_latency && ctx->latest_audio_buffered_end_pts_ns == 0)
		required_fill_ms = ctx->audio_buf.target_ms;

	if (!(low_latency ? has_audio : (fill_ms >= required_fill_ms))) {
		if (!low_latency && !has_audio &&
		    ctx->latest_audio_buffered_end_pts_ns > 0) {
			ctx->audio_underruns++;
			irl_mark_audio_recovery(ctx, AUDIO_RECOVERY_HOLD_US);
			size_t silence_bytes =
				(size_t)base_samples * ctx->audio_buf.frame_size;
			uint8_t *silence_buf = calloc(1, silence_bytes);
			if (!silence_buf)
				return false;

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
			free(silence_buf);
			return true;
		}
		return false;
	}

	if (has_audio &&
	    maybe_resync_audio_buffer(ctx, peek, fill_ms, chunk_count)) {
		return false;
	}

	if (has_audio && maybe_trim_audio_buffer(ctx, fill_ms, chunk_count))
		return false;

	uint8_t *out_buf = NULL;
	int64_t chunk_pts_ns = 0;
	uint64_t stream_duration_ns = 0;
	uint32_t frames_out = 0;

	int chunk_samples = base_samples;
	size_t frame_bytes =
		(size_t)chunk_samples * ctx->audio_buf.frame_size;
	out_buf = malloc(frame_bytes);
	if (!out_buf)
		return false;

	size_t got = audio_buffer_read_pts(&ctx->audio_buf, out_buf,
					   frame_bytes, &chunk_pts_ns);
	if (got == 0) {
		free(out_buf);
		return false;
	}

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
	obs_audio.samples_per_sec = (uint32_t)out_rate;

	obs_source_output_audio(ctx->source, &obs_audio);
	finalize_audio_output(ctx, &obs_audio, chunk_pts_ns, stream_duration_ns);
	free(out_buf);
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
		uint8_t *silence = calloc(1, silence_bytes);
		if (silence) {
			int64_t silence_pts_ns =
				(corrected_pts * 1000000000LL *
				 ctx->pts_state.tb_num /
				 ctx->pts_state.tb_den) -
				(int64_t)silence_ms * 1000000LL;
			if (silence_pts_ns < 0)
				silence_pts_ns = 0;
			audio_buffer_write_pts(&ctx->audio_buf, silence,
					       silence_bytes,
					       silence_pts_ns);
			free(silence);
			ctx->silence_insertions++;
		}
	} else if (action == PTS_ACTION_RESET) {
		pthread_mutex_lock(&ctx->audio_state_lock);
		audio_buffer_flush(&ctx->audio_buf);
		irl_reset_stream_timing_state(ctx);
		pthread_mutex_unlock(&ctx->audio_state_lock);
		irl_mark_audio_recovery(ctx, AUDIO_RECOVERY_HOLD_US);
	}

	if (action != PTS_ACTION_PASS) {
		ctx->pts_repairs++;
		irl_mark_audio_recovery(ctx, AUDIO_RECOVERY_HOLD_US);
	}

	uint8_t *interleaved = NULL;
	int out_samples = frame->nb_samples;

	if (frame->format != AV_SAMPLE_FMT_FLT) {
		if (!ctx->swr_ctx || ctx->swr_in_rate != frame->sample_rate ||
		    ctx->swr_in_channels != frame->ch_layout.nb_channels ||
		    ctx->swr_in_format != frame->format) {
			if (ctx->swr_ctx)
				swr_free(&ctx->swr_ctx);
			ctx->swr_ctx = swr_alloc();
			AVChannelLayout out_layout;
			av_channel_layout_default(&out_layout, out_channels);
			swr_alloc_set_opts2(&ctx->swr_ctx, &out_layout,
					    AV_SAMPLE_FMT_FLT, out_rate,
					    &frame->ch_layout,
					    frame->format, frame->sample_rate,
					    0, NULL);
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
		interleaved =
			malloc((size_t)max_out * out_channels * bytes_per_sample);
		if (!interleaved)
			return;

		out_samples = swr_convert(ctx->swr_ctx, &interleaved, max_out,
					  (const uint8_t **)frame->extended_data,
					  frame->nb_samples);
		if (out_samples <= 0) {
			free(interleaved);
			return;
		}
	} else {
		interleaved = frame->data[0];
	}

	size_t data_bytes =
		(size_t)out_samples * out_channels * bytes_per_sample;
	if (ctx->config.wait_for_keyframe && ctx->video_stream_idx >= 0 &&
	    !ctx->first_keyframe_received) {
		if (interleaved != frame->data[0])
			free(interleaved);
		return;
	}

	int64_t frame_pts_ns = corrected_pts * 1000000000LL *
			       ctx->pts_state.tb_num /
			       ctx->pts_state.tb_den;
	audio_buffer_write_pts(&ctx->audio_buf, interleaved, data_bytes,
			       frame_pts_ns);
	if (interleaved != frame->data[0])
		free(interleaved);

	ctx->latest_audio_stream_pts_ns = frame_pts_ns;
	ctx->decoded_frame_samples = out_samples;
}
