/*
 * obs-irl-source — IRL streaming source plugin for OBS
 * https://irlserver.com
 *
 * Copyright (C) 2026 Thomas Lekanger
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * video-handler.c — Keyframe gate, frame output, format conversion
 *
 * Converts decoded AVFrames into OBS async video frames.
 */

#include <stdlib.h>
#include <string.h>

#include "../include/irl-source.h"

/* ── Format mapping ───────────────────────────────────────── */

static enum video_format avpixfmt_to_obs(enum AVPixelFormat fmt)
{
	switch (fmt) {
	case AV_PIX_FMT_YUV420P:
		return VIDEO_FORMAT_I420;
	case AV_PIX_FMT_NV12:
		return VIDEO_FORMAT_NV12;
	case AV_PIX_FMT_YUV422P:
		return VIDEO_FORMAT_I422;
	case AV_PIX_FMT_YUV444P:
		return VIDEO_FORMAT_I444;
	case AV_PIX_FMT_UYVY422:
		return VIDEO_FORMAT_UYVY;
	case AV_PIX_FMT_YUYV422:
		return VIDEO_FORMAT_YUY2;
	case AV_PIX_FMT_RGBA:
		return VIDEO_FORMAT_RGBA;
	case AV_PIX_FMT_BGRA:
		return VIDEO_FORMAT_BGRA;
	default:
		return VIDEO_FORMAT_NONE;
	}
}

/* ── Keyframe detection ───────────────────────────────────── */

bool irl_video_is_keyframe(const AVFrame *frame)
{
	return (frame->flags & AV_FRAME_FLAG_KEY) != 0;
}

/* ── Video output ─────────────────────────────────────────── */

void irl_video_output_frame(struct irl_source *ctx, AVFrame *frame)
{
	enum video_format obs_fmt = avpixfmt_to_obs(frame->format);

	/* If format not directly supported, convert to NV12 via swscale */
	if (obs_fmt == VIDEO_FORMAT_NONE) {
		if (!ctx->sws_ctx)
			blog(LOG_INFO,
			     "[irl-source] Converting pixel format %d to NV12 via swscale",
			     frame->format);
		if (!ctx->sws_ctx || ctx->sws_src_w != frame->width ||
		    ctx->sws_src_h != frame->height ||
		    ctx->sws_src_fmt != frame->format) {
			if (ctx->sws_ctx)
				sws_freeContext(ctx->sws_ctx);
			ctx->sws_ctx = sws_getContext(
				frame->width, frame->height, frame->format,
				frame->width, frame->height, AV_PIX_FMT_NV12,
				SWS_FAST_BILINEAR, NULL, NULL, NULL);
			ctx->sws_src_w = frame->width;
			ctx->sws_src_h = frame->height;
			ctx->sws_src_fmt = frame->format;
		}

		if (!ctx->sws_ctx)
			return;

		/* Allocate NV12 output planes */
		int y_size = frame->width * frame->height;
		int uv_size = y_size / 2;
		uint8_t *nv12_data = malloc(y_size + uv_size);
		if (!nv12_data)
			return;

		uint8_t *dst_planes[2] = {nv12_data, nv12_data + y_size};
		int dst_strides[2] = {frame->width, frame->width};

		sws_scale(ctx->sws_ctx, (const uint8_t *const *)frame->data,
			  frame->linesize, 0, frame->height, dst_planes,
			  dst_strides);

		struct obs_source_frame obs_frame = {0};
		obs_frame.width = frame->width;
		obs_frame.height = frame->height;
		obs_frame.format = VIDEO_FORMAT_NV12;
		obs_frame.data[0] = dst_planes[0];
		obs_frame.data[1] = dst_planes[1];
		obs_frame.linesize[0] = dst_strides[0];
		obs_frame.linesize[1] = dst_strides[1];
		obs_frame.timestamp =
			(uint64_t)(frame->pts * 1000000000LL *
				   ctx->fmt_ctx->streams[ctx->video_stream_idx]
					   ->time_base.num /
				   ctx->fmt_ctx->streams[ctx->video_stream_idx]
					   ->time_base.den);

		obs_source_output_video(ctx->source, &obs_frame);
		free(nv12_data);
		return;
	}

	/* Direct output for natively supported formats */
	struct obs_source_frame obs_frame = {0};
	obs_frame.width = frame->width;
	obs_frame.height = frame->height;
	obs_frame.format = obs_fmt;
	obs_frame.timestamp =
		(uint64_t)(frame->pts * 1000000000LL *
			   ctx->fmt_ctx->streams[ctx->video_stream_idx]
				   ->time_base.num /
			   ctx->fmt_ctx->streams[ctx->video_stream_idx]
				   ->time_base.den);

	for (int i = 0; i < AV_NUM_DATA_POINTERS; i++) {
		obs_frame.data[i] = frame->data[i];
		obs_frame.linesize[i] = frame->linesize[i];
	}

	obs_source_output_video(ctx->source, &obs_frame);
}
