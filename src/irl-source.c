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
	cfg->speed_min =
		(float)obs_data_get_double(settings, "speed_min");
	cfg->speed_max =
		(float)obs_data_get_double(settings, "speed_max");

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
	ctx->reconnecting = false;
	ctx->video_corrupted = false;
	ctx->video_skip_logged = false;
	audio_buffer_flush(&ctx->audio_buf);
	pts_repair_reset(&ctx->pts_state);
	ctx->current_speed = 1.0f;
	ctx->latest_audio_stream_pts_ns = 0;
	ctx->latest_audio_buffered_pts_ns = 0;
	ctx->latest_video_stream_pts_ns = 0;
	ctx->audio_ts_init = false;
	ctx->audio_pll_offset_ns = 0;
	ctx->video_ts_init = false;
	ctx->decoded_frame_samples = 0;
	ctx->audio_decode_errors = 0;
	ctx->video_decode_errors = 0;
}

static bool should_run_receiver(const struct irl_source *ctx)
{
	return ctx->config.url &&
	       (!ctx->config.close_when_inactive ||
		obs_source_active(ctx->source));
}

static void clear_async_video(struct irl_source *ctx)
{
	obs_source_output_video(ctx->source, NULL);
}

static void start_receiver(struct irl_source *ctx)
{
	if (ctx->thread_active || !should_run_receiver(ctx))
		return;

	reset_runtime_state(ctx);
	ctx->thread_active = true;
	pthread_create(&ctx->receiver_thread, NULL, irl_receiver_thread, ctx);
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
	calldata_set_int(cd, "buffer_fill_ms",
			 audio_buffer_fill_ms_locked(&ctx->audio_buf));
	calldata_set_float(cd, "current_speed",
			   (double)ctx->current_speed);
	calldata_set_bool(cd, "reconnecting", ctx->reconnecting);
	calldata_set_int(cd, "total_audio_frames",
			 (long long)ctx->total_audio_frames);
	calldata_set_int(cd, "total_video_frames",
			 (long long)ctx->total_video_frames);
	calldata_set_int(cd, "pts_repairs",
			 (long long)ctx->pts_repairs);
	calldata_set_int(cd, "silence_insertions",
			 (long long)ctx->silence_insertions);

	/* Stream delay: how far behind real-time the video output is.
	 * Computed as wall_clock - anchored_video_PTS.  Includes SRT
	 * latency, decode time, and any buffering.  Useful for
	 * monitoring end-to-end latency in stats overlays. */
	int64_t stream_delay_ms = 0;
	if (ctx->video_ts_init && ctx->latest_video_stream_pts_ns != 0) {
		int64_t video_wall_ns =
			(int64_t)ctx->video_sys_base +
			(ctx->latest_video_stream_pts_ns -
			 ctx->video_pts_base);
		stream_delay_ms =
			((int64_t)os_gettime_ns() - video_wall_ns) /
			1000000;
		if (stream_delay_ms < 0)
			stream_delay_ms = 0;
	}
	calldata_set_int(cd, "stream_delay_ms",
			 (long long)stream_delay_ms);
	calldata_set_bool(cd, "low_latency_audio",
			  ctx->config.low_latency_audio);
	calldata_set_bool(cd, "decoupled_audio",
			  ctx->config.decoupled_audio);
	calldata_set_int(cd, "reconnect_count",
			 (long long)ctx->reconnect_count);
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

	config_load(&ctx->config, settings);
	apply_async_audio_mode(ctx);

	/* Register stats proc_handler so scripts/overlays can query state */
	proc_handler_t *ph = obs_source_get_proc_handler(source);
	proc_handler_add(
		ph,
		"void get_stats(out int buffer_fill_ms, "
		"out float current_speed, out bool reconnecting, "
		"out int total_audio_frames, out int total_video_frames, "
		"out int pts_repairs, out int silence_insertions, "
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
