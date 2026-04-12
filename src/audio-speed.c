/*
 * obs-irl-source — IRL streaming source plugin for OBS
 * https://irlserver.com
 *
 * Copyright (C) 2026 Thomas Lekanger
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * audio-speed.c — Adaptive playback speed controller
 *
 * Computes the desired playback tempo based on jitter buffer fill.
 * The buffered adaptive-speed path applies this with a pitch-
 * preserving time-stretch backend; low-latency mode stays at 1.0.
 *
 * The controller should spend most of its time at 1.0 and only make
 * gentle corrections when the buffer gets meaningfully too full or
 * too close to underrun.
 */

#include <math.h>

#include "../include/irl-source.h"

/* Speed ramp smoothing time in microseconds (250ms).
 * Buffered mode should react quickly enough to drain burst fill
 * before it turns into a persistent several-hundred-ms backlog. */
#define SPEED_RAMP_US 250000

/* Buffered mode should be much more reluctant to slow down than
 * to speed up.  Staying slightly below target buffer is usually
 * preferable to constant sub-1.0 resampling artifacts. */
#define SPEED_UP_DEAD_ZONE_MS 10
#define SPEED_DOWN_DEAD_ZONE_MS 10
#define SPEED_DOWN_RANGE_SCALE 0.35f
#define SPEED_PANIC_DRAIN_HEADROOM_MS 30
#define SPEED_MILD_DRAIN_RANGE_MS 60
#define SPEED_MILD_DRAIN_MAX 1.020f
#define SPEED_SNAP_TO_ONE_EPSILON 0.012f

float irl_speed_get(struct irl_source *ctx)
{
	if (!ctx->config.adaptive_speed || ctx->config.low_latency_audio)
		return 1.0f;

	int fill_ms = audio_buffer_fill_ms_locked(&ctx->audio_buf);
	int target_ms = ctx->config.buffer_target_ms;
	int min_ms = ctx->audio_buf.min_ms;
	int max_ms = ctx->config.buffer_max_ms;
	float target_speed = 1.0f;
	bool safe_fill =
		fill_ms >= min_ms &&
		fill_ms <= target_ms + SPEED_UP_DEAD_ZONE_MS + 10;

	if (fill_ms >= max_ms - SPEED_PANIC_DRAIN_HEADROOM_MS) {
		/* If we are close to the configured ceiling, stop being
		 * polite and drain at the configured maximum tempo. */
		target_speed = ctx->config.speed_max;
	} else if (fill_ms <=
		   target_ms + SPEED_UP_DEAD_ZONE_MS + SPEED_MILD_DRAIN_RANGE_MS) {
		/* Mildly above target: prefer hovering close to 1.0x rather
		 * than audibly living in stretch mode for long periods. */
		float excess = (float)(fill_ms - target_ms -
				       SPEED_UP_DEAD_ZONE_MS) /
			       (float)SPEED_MILD_DRAIN_RANGE_MS;
		if (excess < 0.0f)
			excess = 0.0f;
		if (excess > 1.0f)
			excess = 1.0f;
		target_speed =
			1.0f + excess * (SPEED_MILD_DRAIN_MAX - 1.0f);
	} else if (fill_ms > target_ms + SPEED_UP_DEAD_ZONE_MS) {
		/* Buffer above dead zone — play faster to drain.
		 * Use a front-loaded curve so buffered mode reacts
		 * decisively once it drifts above target, instead of
		 * spending too long at ~1.01x while the queue grows. */
		int range_ms = max_ms - target_ms - SPEED_UP_DEAD_ZONE_MS -
			       SPEED_MILD_DRAIN_RANGE_MS;
		if (range_ms < 1)
			range_ms = 1;
		float excess = (float)(fill_ms - target_ms -
				       SPEED_UP_DEAD_ZONE_MS -
				       SPEED_MILD_DRAIN_RANGE_MS) /
			       (float)range_ms;
		if (excess > 1.0f)
			excess = 1.0f;
		if (excess < 0.0f)
			excess = 0.0f;
		excess = sqrtf(excess);
		target_speed =
			SPEED_MILD_DRAIN_MAX +
			excess * (ctx->config.speed_max - SPEED_MILD_DRAIN_MAX);
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
