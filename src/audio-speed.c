/*
 * obs-irl-source — IRL streaming source plugin for OBS
 * https://irlserver.com
 *
 * Copyright (C) 2026 Thomas Lekanger
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * audio-speed.c — Adaptive playback speed controller
 *
 * Adjusts the effective sample rate reported to OBS to speed up
 * or slow down audio playback based on jitter buffer fill level.
 *
 * For changes of <5%, simple resampling is inaudible.  No pitch-
 * shifting library is needed.
 */

#include "../include/irl-source.h"

/* Speed ramp smoothing time in microseconds (800ms).
 * Longer ramp = smoother speed changes = fewer resampling artifacts. */
#define SPEED_RAMP_US 800000

/* Buffered mode should be much more reluctant to slow down than
 * to speed up.  Staying slightly below target buffer is usually
 * preferable to constant sub-1.0 resampling artifacts. */
#define SPEED_UP_DEAD_ZONE_MS 15
#define SPEED_DOWN_DEAD_ZONE_MS 10
#define SPEED_DOWN_RANGE_SCALE 0.35f
#define SPEED_SNAP_TO_ONE_EPSILON 0.01f

float irl_speed_get(struct irl_source *ctx)
{
	if (!ctx->config.adaptive_speed || ctx->config.low_latency_audio)
		return 1.0f;

	int fill_ms = audio_buffer_fill_ms_locked(&ctx->audio_buf);
	int target_ms = ctx->config.buffer_target_ms;
	int min_ms = ctx->audio_buf.min_ms;
	float target_speed = 1.0f;
	bool safe_fill =
		fill_ms >= min_ms &&
		fill_ms <= target_ms + SPEED_UP_DEAD_ZONE_MS;

	if (fill_ms > target_ms + SPEED_UP_DEAD_ZONE_MS) {
		/* Buffer above dead zone — play faster to drain.
		 * Scale proportionally: at max_ms, use full speed_max. */
		float excess = (float)(fill_ms - target_ms -
				       SPEED_UP_DEAD_ZONE_MS) /
			       (float)(ctx->config.buffer_max_ms - target_ms -
				       SPEED_UP_DEAD_ZONE_MS);
		if (excess > 1.0f)
			excess = 1.0f;
		target_speed =
			1.0f + excess * (ctx->config.speed_max - 1.0f);
	} else if (fill_ms < min_ms - SPEED_DOWN_DEAD_ZONE_MS) {
		/* Only slow down when the buffer falls near underrun
		 * territory.  Use only a fraction of the configured
		 * slowdown range to keep buffered mode sounding clean. */
		float deficit = (float)(min_ms - SPEED_DOWN_DEAD_ZONE_MS -
					fill_ms) /
				(float)(min_ms - SPEED_DOWN_DEAD_ZONE_MS);
		if (deficit > 1.0f)
			deficit = 1.0f;
		target_speed =
			1.0f - deficit * (1.0f - ctx->config.speed_min) *
					SPEED_DOWN_RANGE_SCALE;
	}

	/* Buffered-mode safe zone: once we're out of underrun risk and
	 * not above target, lock to real-time. This avoids spending
	 * long stretches at ~0.99x just because the buffer sits below
	 * target but still comfortably above minimum. */
	if (safe_fill)
		target_speed = 1.0f;

	/* Clamp to configured range */
	if (target_speed < ctx->config.speed_min)
		target_speed = ctx->config.speed_min;
	if (target_speed > ctx->config.speed_max)
		target_speed = ctx->config.speed_max;

	/* Smooth the change (ramp, not instant jump) */
	uint64_t now = av_gettime();
	float alpha = 1.0f;
	if (ctx->last_speed_adjust_time > 0) {
		uint64_t elapsed = now - ctx->last_speed_adjust_time;
		alpha = (float)elapsed / (float)SPEED_RAMP_US;
		if (alpha > 1.0f)
			alpha = 1.0f;
	}
	ctx->last_speed_adjust_time = now;

	ctx->current_speed =
		ctx->current_speed + alpha * (target_speed - ctx->current_speed);
	if (target_speed == 1.0f &&
	    ctx->current_speed > 1.0f - SPEED_SNAP_TO_ONE_EPSILON &&
	    ctx->current_speed < 1.0f + SPEED_SNAP_TO_ONE_EPSILON) {
		ctx->current_speed = 1.0f;
	}

	return ctx->current_speed;
}
