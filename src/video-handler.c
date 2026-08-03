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

#include "../include/irl-source.h"

/* ── Format mapping ───────────────────────────────────────── */

static enum video_format avpixfmt_to_obs(enum AVPixelFormat fmt)
{
	switch (fmt) {
	case AV_PIX_FMT_YUV420P:
	case AV_PIX_FMT_YUVJ420P:
		return VIDEO_FORMAT_I420;
	case AV_PIX_FMT_YUV420P10LE:
		return VIDEO_FORMAT_I010;
	case AV_PIX_FMT_NV12:
		return VIDEO_FORMAT_NV12;
	case AV_PIX_FMT_P010LE:
		return VIDEO_FORMAT_P010;
	case AV_PIX_FMT_YUV422P:
	case AV_PIX_FMT_YUVJ422P:
		return VIDEO_FORMAT_I422;
	case AV_PIX_FMT_YUV444P:
	case AV_PIX_FMT_YUVJ444P:
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

static void setup_color_params(struct obs_source_frame *obs_frame,
			       const AVFrame *frame,
			       enum video_format out_fmt)
{
	enum video_colorspace cs = convert_color_space(
		frame->colorspace, frame->color_trc, frame->color_primaries);
	enum video_range_type range = convert_color_range(frame->color_range);
	obs_frame->full_range = (range == VIDEO_RANGE_FULL);

	video_format_get_parameters_for_format(cs, range, out_fmt,
					       obs_frame->color_matrix,
					       obs_frame->color_range_min,
					       obs_frame->color_range_max);
}

/* ── Timestamp sync ───────────────────────────────────────── */

/* Drift threshold for video PTS clamping (500ms).
 *
 * Instead of re-anchoring (which causes visible timeline jumps),
 * clamp the computed timestamp to a reasonable range:
 * - Too far behind wall clock: display immediately (use `now`)
 * - Too far ahead: cap at `now + 200ms`
 *
 * The anchor stays unchanged — normal frames self-correct after
 * the burst.  This produces smooth video with no visible skips.
 * Backward drift catches up via brief speedup (frames displayed
 * immediately until PTS catches up).  Forward drift is capped
 * so OBS doesn't hold the previous frame for too long. */
#define VIDEO_TS_CLAMP_NS 500000000LL   /* 500ms */
#define VIDEO_TS_CAP_NS   200000000ULL  /* 200ms forward cap */

/* Convert stream PTS to OBS nanosecond timestamp.
 *
 * When audio is active, treat queued audio as the master playout
 * clock: map video PTS through the same stream-PTS → OBS-clock
 * offset used by the latest audio chunk already handed to OBS.
 * This keeps lip sync stable even when buffered audio or adaptive
 * audio speed changes the effective playout offset.
 *
 * If no audio playout mapping exists yet, fall back to the older
 * video-only wall-clock anchor. */
static uint64_t frame_timestamp(struct irl_source *ctx, const AVFrame *frame)
{
	/* frame->pts is pre-converted to nanoseconds by the receiver
	 * thread (see irl_video_queue_push); fmt_ctx must not be
	 * touched here, it can be freed mid-reconnect. */
	int64_t pts_ns = frame->pts;
	uint64_t now = os_gettime_ns();

	/* Snapshot audio-thread-owned fields under the lock. */
	uint64_t audio_obs_end_ts_ns;
	int64_t audio_buffered_end_pts_ns;
	int startup_warmup_ms;
	irl_mutex_lock(&ctx->audio_state_lock);
	audio_obs_end_ts_ns = ctx->latest_audio_obs_end_ts_ns;
	audio_buffered_end_pts_ns = ctx->latest_audio_buffered_end_pts_ns;
	startup_warmup_ms = ctx->startup_audio_warmup_remaining_ms;
	irl_mutex_unlock(&ctx->audio_state_lock);

	if (ctx->audio_stream_idx >= 0 && audio_obs_end_ts_ns != 0 &&
	    audio_buffered_end_pts_ns > 0) {
		int64_t mapped = (int64_t)pts_ns +
				 ((int64_t)audio_obs_end_ts_ns -
				  audio_buffered_end_pts_ns);
		if (mapped < 0)
			mapped = 0;
		return (uint64_t)mapped;
	}

	if (!ctx->video_ts_init) {
		ctx->video_sys_base = now;
		ctx->video_pts_base = pts_ns;
		ctx->video_ts_init = true;
	}

	uint64_t computed = ctx->video_sys_base +
			    (uint64_t)(pts_ns - ctx->video_pts_base);
	int64_t drift = (int64_t)computed - (int64_t)now;

	/* Clamp without re-anchoring — no visible skip, anchor
	 * stays stable so subsequent frames self-correct. */
	if (drift < -(int64_t)VIDEO_TS_CLAMP_NS) {
		computed = now;
	} else if (drift > (int64_t)VIDEO_TS_CLAMP_NS) {
		computed = now + VIDEO_TS_CAP_NS;
	}

	/* Startup fallback before the audio playout mapping exists. */
	if (ctx->audio_stream_idx >= 0) {
		int64_t audio_lead_ns = 0;
		if (audio_obs_end_ts_ns == 0) {
			audio_lead_ns = (int64_t)startup_warmup_ms * 1000000LL;
			if (!ctx->config.low_latency_audio) {
				audio_lead_ns +=
					os_atomic_load_long(
						&ctx->config.buffer_target_ms) *
					1000000LL;
			}
		}
		if (audio_lead_ns > 0)
			computed += (uint64_t)audio_lead_ns;
	}

	return computed;
}

/* ── Video output ─────────────────────────────────────────── */

void irl_video_output_frame(struct irl_source *ctx, AVFrame *frame)
{
	/* Hardware-decoded frames (NVDEC/D3D11VA/VAAPI/VideoToolbox) come
	 * out on the GPU; we have to expose them to OBS as system memory.
	 *
	 * Try av_hwframe_map(AV_HWFRAME_MAP_READ) first: on backends that
	 * can produce a CPU-readable view without a full download (VAAPI
	 * vaDeriveImage, VideoToolbox IOSurface), this skips the
	 * gpu->cpu copy entirely. On D3D11VA / CUDA the map call falls
	 * back to a copy internally or fails, so we fall back to
	 * av_hwframe_transfer_data which is the historical path.
	 *
	 * hw_map_ok caches the outcome so we don't keep paying for a
	 * doomed map attempt every frame on platforms that can't map. */
	AVFrame *sw_frame = NULL;
	if (frame->hw_frames_ctx) {
		sw_frame = av_frame_alloc();
		if (!sw_frame)
			return;

		bool used_map = false;
		if (ctx->hw_map_ok != 0) {
			int ret = av_hwframe_map(sw_frame, frame,
						 AV_HWFRAME_MAP_READ);
			if (ret == 0) {
				used_map = true;
				if (ctx->hw_map_ok != 1) {
					ctx->hw_map_ok = 1;
					blog(LOG_INFO,
					     "[irl-source] HW frame path: av_hwframe_map (zero-copy)");
				}
			} else {
				av_frame_unref(sw_frame);
				if (ctx->hw_map_ok != 0) {
					ctx->hw_map_ok = 0;
					char errbuf[AV_ERROR_MAX_STRING_SIZE];
					av_strerror(ret, errbuf, sizeof(errbuf));
					blog(LOG_INFO,
					     "[irl-source] HW frame path: av_hwframe_transfer_data (map unsupported: %s)",
					     errbuf);
				}
			}
		}

		if (!used_map) {
			if (av_hwframe_transfer_data(sw_frame, frame, 0) < 0) {
				av_frame_free(&sw_frame);
				return;
			}
		}

		sw_frame->pts = frame->pts;
		sw_frame->colorspace = frame->colorspace;
		sw_frame->color_range = frame->color_range;
		sw_frame->color_trc = frame->color_trc;
		sw_frame->color_primaries = frame->color_primaries;
		sw_frame->flags = frame->flags;
		frame = sw_frame;
	}

	enum video_format obs_fmt = avpixfmt_to_obs(frame->format);

	/* Negative linesize means the frame is laid out bottom-up. OBS's
	 * async path expects positive strides, so taking abs() would
	 * silently flip the image vertically. Route through swscale
	 * instead. Real-world FFmpeg decoders almost never produce this,
	 * but cheap to be defensive. */
	bool negative_stride = false;
	for (int i = 0; i < AV_NUM_DATA_POINTERS; i++) {
		if (frame->data[i] && frame->linesize[i] < 0) {
			negative_stride = true;
			break;
		}
	}
	if (negative_stride)
		obs_fmt = VIDEO_FORMAT_NONE;

	/* If format not directly supported, convert to NV12 via swscale */
	if (obs_fmt == VIDEO_FORMAT_NONE) {
		if (!ctx->sws_ctx || ctx->sws_src_w != frame->width ||
		    ctx->sws_src_h != frame->height ||
		    ctx->sws_src_fmt != frame->format) {
			if (ctx->sws_ctx)
				sws_freeContext(ctx->sws_ctx);

			blog(LOG_INFO,
			     "[irl-source] Converting pixel format %d to NV12 via swscale (%dx%d)",
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

		size_t y_size = (size_t)frame->width * frame->height;
		size_t uv_size = y_size / 2;
		size_t need = y_size + uv_size;
		if (need > ctx->sws_nv12_buf_capacity) {
			uint8_t *next = realloc(ctx->sws_nv12_buf, need);
			if (!next) {
				if (sw_frame)
					av_frame_free(&sw_frame);
				return;
			}
			ctx->sws_nv12_buf = next;
			ctx->sws_nv12_buf_capacity = need;
		}

		uint8_t *dst_planes[2] = {ctx->sws_nv12_buf,
					  ctx->sws_nv12_buf + y_size};
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
		obs_frame.timestamp = frame_timestamp(ctx, frame);
		setup_color_params(&obs_frame, frame, VIDEO_FORMAT_NV12);

		obs_source_output_video(ctx->source, &obs_frame);
		if (sw_frame)
			av_frame_free(&sw_frame);
		return;
	}

	/* Direct output for natively supported formats (zero-copy) */
	struct obs_source_frame obs_frame = {0};
	obs_frame.width = frame->width;
	obs_frame.height = frame->height;
	obs_frame.format = obs_fmt;
	obs_frame.timestamp = frame_timestamp(ctx, frame);
	setup_color_params(&obs_frame, frame, obs_fmt);

	for (int i = 0; i < AV_NUM_DATA_POINTERS; i++) {
		obs_frame.data[i] = frame->data[i];
		/* Linesize is non-negative here: negative_stride above
		 * routes to the swscale path. */
		obs_frame.linesize[i] =
			frame->linesize[i] > 0 ? frame->linesize[i] : 0;
	}

	obs_source_output_video(ctx->source, &obs_frame);
	if (sw_frame)
		av_frame_free(&sw_frame);
}
