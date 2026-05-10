/*
 * obs-irl-source — IRL streaming source plugin for OBS
 * https://irlserver.com
 *
 * Copyright (C) 2026 Thomas Lekanger
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * irl-source.c — Source lifecycle: create, destroy, update, tick
 */

#include <stdlib.h>

#include "../include/irl-source.h"
#include "receiver-internal.h"

/* ── Helpers ──────────────────────────────────────────────── */

static void config_free(struct irl_config *cfg)
{
	if (cfg->url) {
		bfree(cfg->url);
		cfg->url = NULL;
	}
	if (cfg->ffmpeg_options) {
		bfree(cfg->ffmpeg_options);
		cfg->ffmpeg_options = NULL;
	}
}

static void config_load(struct irl_config *cfg, obs_data_t *settings)
{
	config_free(cfg);

	const char *url = obs_data_get_string(settings, "url");
	cfg->url = url && *url ? bstrdup(url) : NULL;

	cfg->reconnect_delay =
		(int)obs_data_get_int(settings, "reconnect_delay");
	cfg->network_buffer_mb =
		(int)obs_data_get_int(settings, "network_buffer_mb");

	cfg->buffer_target_ms =
		(int)obs_data_get_int(settings, "buffer_target_ms");
	cfg->buffer_min_ms = (int)obs_data_get_int(settings, "buffer_min_ms");
	cfg->buffer_max_ms = (int)obs_data_get_int(settings, "buffer_max_ms");
	cfg->adaptive_speed = obs_data_get_bool(settings, "adaptive_speed");

	cfg->small_gap_ms = (int)obs_data_get_int(settings, "small_gap_ms");
	cfg->large_gap_ms = (int)obs_data_get_int(settings, "large_gap_ms");

	const char *ff = obs_data_get_string(settings, "ffmpeg_options");
	cfg->ffmpeg_options = ff && *ff ? bstrdup(ff) : NULL;

	cfg->hw_decode = (int)obs_data_get_int(settings, "hw_decode");
	cfg->wait_for_keyframe =
		obs_data_get_bool(settings, "wait_for_keyframe");
	cfg->low_latency_audio =
		obs_data_get_bool(settings, "low_latency_audio");
	cfg->decoupled_audio =
		obs_data_get_bool(settings, "decoupled_audio");
	cfg->close_when_inactive =
		obs_data_get_bool(settings, "close_when_inactive");
	if (!cfg->low_latency_audio)
		cfg->decoupled_audio = false;
}

static void apply_async_audio_mode(struct irl_source *ctx)
{
	obs_source_set_async_unbuffered(ctx->source,
					ctx->config.low_latency_audio);
	obs_source_set_async_decoupled(ctx->source,
				       ctx->config.low_latency_audio &&
					       ctx->config.decoupled_audio);
}

static void reset_runtime_state(struct irl_source *ctx)
{
	ctx->first_keyframe_received = false;
	os_atomic_store_bool(&ctx->reconnecting, false);
	pthread_mutex_lock(&ctx->audio_state_lock);
	audio_buffer_flush(&ctx->audio_buf);
	pts_repair_reset(&ctx->pts_state);
	irl_reset_stream_timing_state(ctx);
	ctx->current_speed = 1.0f;
	ctx->audio_pll_corrections = 0;
	ctx->audio_pll_hard_resets = 0;
	ctx->audio_underruns = 0;
	ctx->audio_resync_skipped_chunks = 0;
	ctx->fade_in_pending = false;
	ctx->fade_in_frames_remaining = 0;
	ctx->startup_audio_warmup_remaining_ms = 0;
	ctx->audio_last_output_sample_channels = 0;
	ctx->audio_last_output_sample_valid = false;
	ctx->audio_trim_crossfade_pending = false;
	pthread_mutex_unlock(&ctx->audio_state_lock);
	ctx->total_audio_frames = 0;
	ctx->total_video_frames = 0;
	ctx->pts_repairs = 0;
	ctx->pts_normalizations = 0;
	ctx->pts_interpolations = 0;
	ctx->pts_resets = 0;
	ctx->pts_last_gap_ms = 0;
	ctx->pts_max_gap_ms = 0;
	ctx->silence_insertions = 0;
	ctx->last_stats_time = 0;
}

static bool should_run_receiver(const struct irl_source *ctx)
{
	return ctx->config.url &&
	       (!ctx->config.close_when_inactive ||
		obs_source_showing(ctx->source));
}

static void clear_async_video(struct irl_source *ctx)
{
	obs_source_output_video(ctx->source, NULL);
}

static void start_receiver(struct irl_source *ctx)
{
	if (os_atomic_load_bool(&ctx->thread_active) ||
	    !should_run_receiver(ctx))
		return;

	reset_runtime_state(ctx);
	os_atomic_store_bool(&ctx->thread_active, true);
	if (pthread_create(&ctx->audio_thread, NULL, irl_audio_thread, ctx) !=
	    0) {
		blog(LOG_ERROR,
		     "[irl-source] Failed to create audio thread");
		os_atomic_store_bool(&ctx->thread_active, false);
		return;
	}
	if (pthread_create(&ctx->receiver_thread, NULL, irl_receiver_thread,
			   ctx) != 0) {
		blog(LOG_ERROR,
		     "[irl-source] Failed to create receiver thread");
		os_atomic_store_bool(&ctx->thread_active, false);
		pthread_join(ctx->audio_thread, NULL);
	}
}

static void stop_receiver(struct irl_source *ctx, bool clear_video)
{
	irl_receiver_stop(ctx);
	reset_runtime_state(ctx);
	if (clear_video)
		clear_async_video(ctx);
}

/* ── Stats proc_handler callback ──────────────────────────── */

static void irl_source_get_stats(void *data, calldata_t *cd)
{
	struct irl_source *ctx = data;

	/* Snapshot all shared mutable state under the lock so the stats
	 * blob is internally consistent and we don't race the receiver
	 * thread reconfiguring the audio buffer. */
	pthread_mutex_lock(&ctx->audio_state_lock);
	int buffer_fill_ms = audio_buffer_fill_ms_locked(&ctx->audio_buf);
	float current_speed = ctx->current_speed;
	uint64_t total_audio_frames = ctx->total_audio_frames;
	uint64_t total_video_frames = ctx->total_video_frames;
	uint64_t pts_repairs = ctx->pts_repairs;
	uint64_t pts_normalizations = ctx->pts_normalizations;
	uint64_t pts_interpolations = ctx->pts_interpolations;
	uint64_t pts_resets = ctx->pts_resets;
	int pts_last_gap_ms = ctx->pts_last_gap_ms;
	int pts_max_gap_ms = ctx->pts_max_gap_ms;
	uint64_t silence_insertions = ctx->silence_insertions;
	uint64_t reconnect_count = ctx->reconnect_count;
	bool video_ts_init = ctx->video_ts_init;
	uint64_t video_sys_base = ctx->video_sys_base;
	int64_t video_pts_base = ctx->video_pts_base;
	int64_t latest_video_stream_pts_ns = ctx->latest_video_stream_pts_ns;
	pthread_mutex_unlock(&ctx->audio_state_lock);

	calldata_set_int(cd, "buffer_fill_ms", buffer_fill_ms);
	calldata_set_float(cd, "current_speed", (double)current_speed);
	calldata_set_bool(cd, "adaptive_latency_control",
			  ctx->config.adaptive_speed);
	calldata_set_bool(cd, "reconnecting",
			  os_atomic_load_bool(&ctx->reconnecting));
	calldata_set_int(cd, "total_audio_frames",
			 (long long)total_audio_frames);
	calldata_set_int(cd, "total_video_frames",
			 (long long)total_video_frames);
	calldata_set_int(cd, "pts_repairs", (long long)pts_repairs);
	calldata_set_int(cd, "pts_normalizations",
			 (long long)pts_normalizations);
	calldata_set_int(cd, "pts_interpolations",
			 (long long)pts_interpolations);
	calldata_set_int(cd, "pts_resets", (long long)pts_resets);
	calldata_set_int(cd, "pts_last_gap_ms", pts_last_gap_ms);
	calldata_set_int(cd, "pts_max_gap_ms", pts_max_gap_ms);
	calldata_set_int(cd, "silence_insertions",
			 (long long)silence_insertions);

	/* Stream delay: how far behind real-time the video output is.
	 * Computed as wall_clock - anchored_video_PTS.  Includes SRT
	 * latency, decode time, and any buffering.  Useful for
	 * monitoring end-to-end latency in stats overlays. */
	int64_t stream_delay_ms = 0;
	if (video_ts_init && latest_video_stream_pts_ns != 0) {
		int64_t video_wall_ns = (int64_t)video_sys_base +
					(latest_video_stream_pts_ns -
					 video_pts_base);
		stream_delay_ms =
			((int64_t)os_gettime_ns() - video_wall_ns) / 1000000;
		if (stream_delay_ms < 0)
			stream_delay_ms = 0;
	}
	calldata_set_int(cd, "stream_delay_ms",
			 (long long)stream_delay_ms);
	calldata_set_bool(cd, "low_latency_audio",
			  ctx->config.low_latency_audio);
	calldata_set_bool(cd, "decoupled_audio",
			  ctx->config.decoupled_audio);
	calldata_set_int(cd, "reconnect_count", (long long)reconnect_count);
}

/* ── Lifecycle ────────────────────────────────────────────── */

const char *irl_source_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("IRL Source (irlserver.com)");
}

void *irl_source_create(obs_data_t *settings, obs_source_t *source)
{
	struct irl_source *ctx = bzalloc(sizeof(*ctx));
	ctx->source = source;
	ctx->current_speed = 1.0f;
	pthread_mutex_init(&ctx->audio_state_lock, NULL);

	config_load(&ctx->config, settings);
	apply_async_audio_mode(ctx);

	/* Register stats proc_handler so scripts/overlays can query state */
	proc_handler_t *ph = obs_source_get_proc_handler(source);
	proc_handler_add(
		ph,
		"void get_stats(out int buffer_fill_ms, "
		"out float current_speed, out bool adaptive_latency_control, "
		"out bool reconnecting, "
		"out int total_audio_frames, out int total_video_frames, "
		"out int pts_repairs, out int pts_normalizations, "
		"out int pts_interpolations, out int pts_resets, "
		"out int pts_last_gap_ms, out int pts_max_gap_ms, "
		"out int silence_insertions, "
		"out int stream_delay_ms, out bool low_latency_audio, "
		"out bool decoupled_audio, out int reconnect_count)",
		irl_source_get_stats, ctx);

	/* Start the receiver thread if we have a URL */
	if (ctx->config.url) {
		blog(LOG_INFO, "[irl-source] Created with URL: %s",
		     ctx->config.url);
		start_receiver(ctx);
	} else {
		blog(LOG_INFO, "[irl-source] Created with no URL configured");
	}

	return ctx;
}

void irl_source_destroy(void *data)
{
	struct irl_source *ctx = data;
	if (!ctx)
		return;

	stop_receiver(ctx, false);
	audio_buffer_free(&ctx->audio_buf);
	pthread_mutex_destroy(&ctx->audio_state_lock);

	free(ctx->audio_pump_scratch);
	free(ctx->audio_resample_scratch);
	free(ctx->sws_nv12_buf);

	if (ctx->swr_ctx)
		swr_free(&ctx->swr_ctx);
	if (ctx->sws_ctx)
		sws_freeContext(ctx->sws_ctx);
	if (ctx->hw_device_ctx)
		av_buffer_unref(&ctx->hw_device_ctx);

	config_free(&ctx->config);
	bfree(ctx);
}

void irl_source_update(void *data, obs_data_t *settings)
{
	struct irl_source *ctx = data;

	/* Stop existing receiver */
	stop_receiver(ctx, false);

	/* Reload config */
	config_load(&ctx->config, settings);
	apply_async_audio_mode(ctx);

	start_receiver(ctx);
	if (!should_run_receiver(ctx))
		clear_async_video(ctx);
}

void irl_source_activate(void *data)
{
	struct irl_source *ctx = data;

	if (!ctx || !ctx->config.close_when_inactive)
		return;

	start_receiver(ctx);
}

void irl_source_deactivate(void *data)
{
	struct irl_source *ctx = data;

	if (!ctx || !ctx->config.close_when_inactive)
		return;

	if (!obs_source_showing(ctx->source))
		stop_receiver(ctx, true);
}

void irl_source_show(void *data)
{
	struct irl_source *ctx = data;

	if (!ctx || !ctx->config.close_when_inactive)
		return;

	start_receiver(ctx);
}

void irl_source_hide(void *data)
{
	struct irl_source *ctx = data;

	if (!ctx || !ctx->config.close_when_inactive)
		return;

	stop_receiver(ctx, true);
}

void irl_source_tick(void *data, float seconds)
{
	UNUSED_PARAMETER(seconds);
	struct irl_source *ctx = data;

	/* Reconnection is handled inside the receiver thread via
	 * sleep + retry, so there's nothing to poll here. */
	UNUSED_PARAMETER(ctx);
}
