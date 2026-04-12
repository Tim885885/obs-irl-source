/*
 * obs-irl-source — IRL streaming source plugin for OBS
 * https://irlserver.com
 *
 * Copyright (C) 2026 Thomas Lekanger
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "receiver-internal.h"

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
		blog(LOG_INFO,
		     "[irl-source] First keyframe received (%dx%d fmt=%d)",
		     frame->width, frame->height, frame->format);
	}

	if (irl_video_is_keyframe(frame))
		ctx->video_corrupted = false;

	if (ctx->video_corrupted || frame->decode_error_flags != 0) {
		if (!ctx->video_skip_logged) {
			blog(LOG_WARNING,
			     "[irl-source] Skipping corrupt video frames, holding last good frame");
			ctx->video_skip_logged = true;
		}
		return;
	}
	if (ctx->video_skip_logged) {
		blog(LOG_INFO,
		     "[irl-source] Clean video frame received, resuming output");
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

	if (ctx->fmt_ctx && ctx->video_stream_idx >= 0) {
		AVStream *vs =
			ctx->fmt_ctx->streams[ctx->video_stream_idx];
		ctx->latest_video_stream_pts_ns =
			frame->pts * 1000000000LL * vs->time_base.num /
			vs->time_base.den;
	}

	irl_video_output_frame(ctx, frame);
	ctx->total_video_frames++;
	if (ctx->total_video_frames == 1)
		blog(LOG_INFO, "[irl-source] First video frame output");
}
