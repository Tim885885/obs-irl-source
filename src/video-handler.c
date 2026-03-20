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

/* ── Keyframe detection ───────────────────────────────────── */

bool irl_video_is_keyframe(const AVFrame *frame)
{
	return (frame->flags & AV_FRAME_FLAG_KEY) != 0;
}

/* ── Color space helpers ──────────────────────────────────── */

static enum video_colorspace
convert_color_space(enum AVColorSpace cs, enum AVColorTransferCharacteristic trc,
		    enum AVColorPrimaries prm)
{
	switch (cs) {
	case AVCOL_SPC_BT709:
		return VIDEO_CS_709;
	case AVCOL_SPC_SMPTE170M:
	case AVCOL_SPC_BT470BG:
		return VIDEO_CS_601;
	case AVCOL_SPC_BT2020_NCL:
	case AVCOL_SPC_BT2020_CL:
		if (trc == AVCOL_TRC_ARIB_STD_B67)
			return VIDEO_CS_2100_HLG;
		return VIDEO_CS_2100_PQ;
	default:
		break;
	}
	(void)prm;
	return VIDEO_CS_709;
}

static enum video_range_type convert_color_range(enum AVColorRange range)
{
	return range == AVCOL_RANGE_JPEG ? VIDEO_RANGE_FULL
					 : VIDEO_RANGE_PARTIAL;
}

/* ── Video output ─────────────────────────────────────────── */

void irl_video_output_frame(struct irl_source *ctx, AVFrame *frame)
{
	/* Always convert to NV12 via swscale.  OBS's render pipeline uses
	 * NV12 textures natively, and this avoids issues with direct plane
	 * passthrough for other planar formats. */
	if (!ctx->sws_ctx || ctx->sws_src_w != frame->width ||
	    ctx->sws_src_h != frame->height ||
	    ctx->sws_src_fmt != frame->format) {
		if (ctx->sws_ctx)
			sws_freeContext(ctx->sws_ctx);

		blog(LOG_INFO,
		     "[irl-source] Setting up NV12 conversion for pixel format %d (%dx%d)",
		     frame->format, frame->width, frame->height);

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

	/* Build OBS frame with proper color space parameters.
	 * Without color_matrix/color_range_min/color_range_max, OBS
	 * cannot convert YUV→RGB and the frame will not display. */
	struct obs_source_frame obs_frame = {0};
	obs_frame.width = frame->width;
	obs_frame.height = frame->height;
	obs_frame.format = VIDEO_FORMAT_NV12;
	obs_frame.data[0] = dst_planes[0];
	obs_frame.data[1] = dst_planes[1];
	obs_frame.linesize[0] = dst_strides[0];
	obs_frame.linesize[1] = dst_strides[1];
	obs_frame.timestamp = os_gettime_ns();

	enum video_colorspace cs = convert_color_space(
		frame->colorspace, frame->color_trc, frame->color_primaries);
	enum video_range_type range = convert_color_range(frame->color_range);
	obs_frame.full_range = (range == VIDEO_RANGE_FULL);

	video_format_get_parameters_for_format(cs, range,
					       VIDEO_FORMAT_NV12,
					       obs_frame.color_matrix,
					       obs_frame.color_range_min,
					       obs_frame.color_range_max);

	obs_source_output_video(ctx->source, &obs_frame);
	free(nv12_data);
}
