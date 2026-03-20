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

#include <math.h>

#include "../include/irl-source.h"

/* Speed ramp smoothing time in microseconds (100ms) */
#define SPEED_RAMP_US 100000

float irl_speed_calculate(struct irl_source *ctx)
{
	if (!ctx->config.adaptive_speed)
		return 1.0f;

	float ratio = audio_buffer_fill_ratio(&ctx->audio_buf);
	int fill_ms = audio_buffer_fill_ms(&ctx->audio_buf);
	float target_speed = 1.0f;

	if (fill_ms > ctx->config.buffer_max_ms) {
		/* Buffer overfull — play faster to drain.
		 * Higher samples_per_sec → OBS resamples down → faster playback. */
		float excess = (float)(fill_ms - ctx->config.buffer_target_ms) /
			       (float)ctx->config.buffer_target_ms;
		target_speed = 1.0f + excess * (ctx->config.speed_max - 1.0f);
	} else if (fill_ms < ctx->config.buffer_min_ms) {
		/* Buffer too low — play slower to let it fill.
		 * Lower samples_per_sec → OBS resamples up → slower playback. */
		float deficit =
			1.0f - (float)fill_ms / (float)ctx->config.buffer_target_ms;
		target_speed = 1.0f - deficit * (1.0f - ctx->config.speed_min);
	}

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

	return ctx->current_speed;
}

void irl_speed_apply(struct irl_source *ctx, struct obs_source_audio *audio)
{
	float speed = irl_speed_calculate(ctx);

	if (fabsf(speed - 1.0f) > 0.001f) {
		/* Adjust the sample rate to change playback speed.
		 * OBS will resample at the audio subsystem level.
		 * e.g. 48000 * 1.03 = 49440 → OBS plays it faster. */
		audio->samples_per_sec =
			(uint32_t)((float)audio->samples_per_sec * speed);
	}
}
