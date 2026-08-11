/*
 * obs-irl-source — IRL streaming source plugin for OBS
 * https://irlserver.com
 *
 * Copyright (C) 2026 Thomas Lekanger
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "receiver-internal.h"

/* ── Video output queue ───────────────────────────────────── */

static void video_queue_drain_locked(struct irl_source *ctx)
{
	while (ctx->video_queue_count > 0) {
		AVFrame *f = ctx->video_queue[ctx->video_queue_head];
		ctx->video_queue[ctx->video_queue_head] = NULL;
		ctx->video_queue_head =
			(ctx->video_queue_head + 1) % IRL_VIDEO_QUEUE_SIZE;
		ctx->video_queue_count--;
		av_frame_free(&f);
	}
}

static void video_pinned_update_locked(struct irl_source *ctx)
{
	int pinned = ctx->video_queue_count + ctx->video_in_flight;
	if (pinned > ctx->video_pinned_peak)
		ctx->video_pinned_peak = pinned;
}

/* Ask the video thread to blank the source. Queued frames are dropped
 * here so nothing decoded before the disconnect can repaint after the
 * clear; a frame already being converted is handled by the ordering in
 * irl_video_thread(), which re-checks the flag after each output. */
void irl_video_request_clear(struct irl_source *ctx)
{
	irl_mutex_lock(&ctx->video_queue_lock);
	video_queue_drain_locked(ctx);
	ctx->video_clear_pending = true;
	irl_cond_signal(&ctx->video_queue_cond);
	irl_mutex_unlock(&ctx->video_queue_lock);
}

void irl_video_queue_push(struct irl_source *ctx, AVFrame *frame,
			  int64_t pts_ns)
{
	AVFrame *clone = av_frame_alloc();
	if (!clone)
		return;
	if (av_frame_ref(clone, frame) < 0) {
		av_frame_free(&clone);
		return;
	}
	clone->pts = pts_ns;

	irl_mutex_lock(&ctx->video_queue_lock);
	if (ctx->video_queue_count >= IRL_VIDEO_QUEUE_SIZE) {
		/* Video thread is stalled; keep the freshest frames and
		 * never make the receiver (and therefore audio) wait. */
		AVFrame *oldest = ctx->video_queue[ctx->video_queue_head];
		ctx->video_queue[ctx->video_queue_head] = NULL;
		ctx->video_queue_head =
			(ctx->video_queue_head + 1) % IRL_VIDEO_QUEUE_SIZE;
		ctx->video_queue_count--;
		ctx->video_queue_drops++;
		av_frame_free(&oldest);
	}
	int tail = (ctx->video_queue_head + ctx->video_queue_count) %
		   IRL_VIDEO_QUEUE_SIZE;
	ctx->video_queue[tail] = clone;
	ctx->video_queue_count++;
	video_pinned_update_locked(ctx);
	irl_cond_signal(&ctx->video_queue_cond);
	irl_mutex_unlock(&ctx->video_queue_lock);
}

void *irl_video_thread(void *data)
{
	struct irl_source *ctx = data;

	irl_mutex_lock(&ctx->video_queue_lock);
	while (os_atomic_load_bool(&ctx->thread_active)) {
		if (ctx->video_clear_pending) {
			ctx->video_clear_pending = false;
			irl_mutex_unlock(&ctx->video_queue_lock);
			obs_source_output_video(ctx->source, NULL);
			irl_mutex_lock(&ctx->video_queue_lock);
			continue;
		}
		if (ctx->video_queue_count == 0) {
			irl_cond_wait(&ctx->video_queue_cond,
				      &ctx->video_queue_lock);
			continue;
		}
		AVFrame *f = ctx->video_queue[ctx->video_queue_head];
		ctx->video_queue[ctx->video_queue_head] = NULL;
		ctx->video_queue_head =
			(ctx->video_queue_head + 1) % IRL_VIDEO_QUEUE_SIZE;
		ctx->video_queue_count--;
		/* Still pinning f's surface until av_frame_free below. */
		ctx->video_in_flight = 1;
		video_pinned_update_locked(ctx);
		irl_mutex_unlock(&ctx->video_queue_lock);

		irl_video_output_frame(ctx, f);
		av_frame_free(&f);

		irl_mutex_lock(&ctx->video_queue_lock);
		ctx->video_in_flight = 0;
	}
	video_queue_drain_locked(ctx);
	irl_mutex_unlock(&ctx->video_queue_lock);
	return NULL;
}

/* ── Decoded frame handling (receiver thread) ─────────────── */

static int64_t video_frame_pts(const AVFrame *frame)
{
	if (frame->best_effort_timestamp != AV_NOPTS_VALUE)
		return frame->best_effort_timestamp;
	if (frame->pts != AV_NOPTS_VALUE)
		return frame->pts;
	return AV_NOPTS_VALUE;
}

void irl_handle_video_frame(struct irl_source *ctx, AVFrame *frame)
{
	int64_t pts = video_frame_pts(frame);
	if (pts == AV_NOPTS_VALUE) {
		if (!ctx->video_skip_logged) {
			blog(LOG_WARNING,
			     "[irl-source] Dropping video frame without valid PTS");
			ctx->video_skip_logged = true;
		}
		return;
	}
	frame->pts = pts;

	if (!ctx->first_keyframe_received) {
		if (!irl_video_is_keyframe(frame)) {
			if (ctx->total_video_frames == 0)
				blog(LOG_DEBUG,
				     "[irl-source] Waiting for keyframe (dropped non-keyframe)");
			return;
		}

		ctx->first_keyframe_received = true;
		ctx->video_corrupted = false;
		/* hw_frames_ctx on the decoded frame is the ground truth
		 * for whether hardware decode is actually in use; the
		 * stream-open log only reports what was requested. */
		blog(LOG_INFO,
		     "[irl-source] First keyframe received (%dx%d fmt=%d %s decode)",
		     frame->width, frame->height, frame->format,
		     frame->hw_frames_ctx ? "hardware" : "software");
	}

	if (irl_video_is_keyframe(frame))
		ctx->video_corrupted = false;

	if (ctx->video_corrupted || frame->decode_error_flags != 0) {
		if (!ctx->video_skip_logged) {
			blog(LOG_WARNING,
			     "[irl-source] Passing through corrupt video frames to preserve cadence");
			ctx->video_skip_logged = true;
		}
	} else if (ctx->video_skip_logged) {
		blog(LOG_INFO,
		     "[irl-source] Clean video frame received, normal video cadence restored");
		ctx->video_skip_logged = false;
	}

	if (ctx->last_video_width && ctx->last_video_height &&
	    (frame->width != ctx->last_video_width ||
	     frame->height != ctx->last_video_height)) {
		blog(LOG_INFO,
		     "[irl-source] Resolution changed: %dx%d -> %dx%d",
		     ctx->last_video_width, ctx->last_video_height,
		     frame->width, frame->height);
		ctx->video_ts_init = false;
	}
	ctx->last_video_width = frame->width;
	ctx->last_video_height = frame->height;

	/* Convert PTS to nanoseconds here: the video thread must not
	 * touch fmt_ctx, which this thread frees on reconnect while
	 * queued frames may still be in flight. */
	int64_t pts_ns = 0;
	if (ctx->fmt_ctx && ctx->video_stream_idx >= 0) {
		AVStream *vs =
			ctx->fmt_ctx->streams[ctx->video_stream_idx];
		pts_ns = av_rescale_q(frame->pts, vs->time_base,
				      (AVRational){1, 1000000000});

		/* Frame interval EMA, for the video thread's estimate of how
		 * many frames a given output lead parks in the libobs async
		 * queue. Measured rather than taken from avg_frame_rate,
		 * which live SRT/RTMP demuxers routinely leave unset or
		 * wrong. Out-of-range deltas (PTS repair, discontinuities,
		 * reordering) are skipped rather than smoothed in. */
		int64_t delta = pts_ns - ctx->video_prev_pts_ns;
		bool usable_delta = ctx->video_prev_pts_ns != 0 &&
				    delta >= IRL_VIDEO_INTERVAL_MIN_NS &&
				    delta <= IRL_VIDEO_INTERVAL_MAX_NS;
		ctx->video_prev_pts_ns = pts_ns;

		irl_mutex_lock(&ctx->audio_state_lock);
		ctx->latest_video_stream_pts_ns = pts_ns;
		if (usable_delta) {
			if (ctx->video_frame_interval_ns == 0)
				ctx->video_frame_interval_ns = delta;
			else
				ctx->video_frame_interval_ns +=
					(delta - ctx->video_frame_interval_ns) /
					8;
		}
		irl_mutex_unlock(&ctx->audio_state_lock);
	}

	irl_video_queue_push(ctx, frame, pts_ns);
	ctx->total_video_frames++;
	if (ctx->total_video_frames == 1)
		blog(LOG_INFO, "[irl-source] First video frame queued");
}
