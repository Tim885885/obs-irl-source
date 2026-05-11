/*
 * obs-irl-source — IRL streaming source plugin for OBS
 * https://irlserver.com
 *
 * Copyright (C) 2026 Thomas Lekanger
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "receiver-internal.h"

#define DECODER_FLUSH_COOLDOWN_US 350000
#define DECODER_WARNING_INTERVAL_US 1000000

static bool should_log_decoder_warning(uint64_t *last_warning_time_us,
				       uint64_t now_us)
{
	if (*last_warning_time_us != 0 &&
	    now_us - *last_warning_time_us < DECODER_WARNING_INTERVAL_US) {
		return false;
	}
	*last_warning_time_us = now_us;
	return true;
}

static bool should_flush_decoder(uint64_t *last_flush_time_us, uint64_t now_us)
{
	if (*last_flush_time_us != 0 &&
	    now_us - *last_flush_time_us < DECODER_FLUSH_COOLDOWN_US) {
		return false;
	}
	*last_flush_time_us = now_us;
	return true;
}

static void reinit_audio_pts_repair(struct irl_source *ctx)
{
	pts_repair_reset(&ctx->pts_state);
	if (ctx->fmt_ctx && ctx->audio_stream_idx >= 0) {
		AVStream *as = ctx->fmt_ctx->streams[ctx->audio_stream_idx];
		pts_repair_init(&ctx->pts_state, ctx->config.small_gap_ms,
				ctx->config.large_gap_ms, as->time_base.num,
				as->time_base.den);
	}
}

void irl_handle_audio_packet(struct irl_source *ctx, AVPacket *pkt,
			     AVFrame *frame)
{
	int ret = avcodec_send_packet(ctx->audio_dec_ctx, pkt);
	if (ret < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) {
		ctx->audio_decode_errors++;
		if (ctx->audio_decode_errors >= 3) {
			uint64_t now_us = (uint64_t)av_gettime();
			bool do_flush = should_flush_decoder(
				&ctx->audio_last_decoder_flush_time_us, now_us);
			if (should_log_decoder_warning(
				    &ctx->audio_last_decoder_warning_time_us,
				    now_us)) {
				blog(LOG_WARNING,
				     "[irl-source] Audio decoder: corruption burst (%d consecutive errors)%s",
				     ctx->audio_decode_errors,
				     do_flush ? ", flushing"
					      : ", suppressing repeated flush");
			}
			if (do_flush) {
				avcodec_flush_buffers(ctx->audio_dec_ctx);
				ctx->audio_decoder_flushes++;
				ctx->audio_quality_events++;
			}
			ctx->audio_decode_errors = 0;
		}
	} else {
		ctx->audio_decode_errors = 0;
	}

	for (;;) {
		ret = avcodec_receive_frame(ctx->audio_dec_ctx, frame);
		if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
			break;
		if (ret < 0) {
			ctx->audio_decode_errors++;
			if (ctx->audio_decode_errors >= 3) {
				uint64_t now_us = (uint64_t)av_gettime();
				bool do_flush = should_flush_decoder(
					&ctx->audio_last_decoder_flush_time_us,
					now_us);
				if (should_log_decoder_warning(
					    &ctx->audio_last_decoder_warning_time_us,
					    now_us)) {
					blog(LOG_WARNING,
					     "[irl-source] Audio decoder receive: corruption burst (%d consecutive errors)%s",
					     ctx->audio_decode_errors,
					     do_flush ? ", resetting audio state"
						      : ", reset cooldown active");
				}
				if (do_flush) {
					avcodec_flush_buffers(ctx->audio_dec_ctx);
					ctx->audio_decoder_flushes++;
					ctx->audio_quality_events++;
					pthread_mutex_lock(&ctx->audio_state_lock);
					audio_buffer_flush(&ctx->audio_buf);
					irl_reset_audio_timing_state(ctx);
					irl_mark_audio_recovery(
						ctx, 2500000ULL);
					pthread_mutex_unlock(&ctx->audio_state_lock);
					reinit_audio_pts_repair(ctx);
				}
				ctx->audio_decode_errors = 0;
			}
			break;
		}

		ctx->audio_decode_errors = 0;
		irl_handle_audio_frame(ctx, frame);
		av_frame_unref(frame);
	}
}

void irl_handle_video_packet(struct irl_source *ctx, AVPacket *pkt,
			     AVFrame *frame)
{
	int ret = avcodec_send_packet(ctx->video_dec_ctx, pkt);
	if (ret < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) {
		ctx->video_decode_errors++;
		ctx->video_corrupted = true;
		if (ctx->video_decode_errors >= 3) {
			uint64_t now_us = (uint64_t)av_gettime();
			bool do_flush = should_flush_decoder(
				&ctx->video_last_decoder_flush_time_us, now_us);
			if (should_log_decoder_warning(
				    &ctx->video_last_decoder_warning_time_us,
				    now_us)) {
				blog(LOG_WARNING,
				     "[irl-source] Video decoder: corruption burst (%d consecutive errors)%s",
				     ctx->video_decode_errors,
				     do_flush ? ", flushing"
					      : ", flush cooldown active");
			}
			if (do_flush) {
				avcodec_flush_buffers(ctx->video_dec_ctx);
				ctx->video_decoder_flushes++;
			}
			ctx->video_decode_errors = 0;
		}
	} else {
		ctx->video_decode_errors = 0;
	}

	for (;;) {
		ret = avcodec_receive_frame(ctx->video_dec_ctx, frame);
		if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
			break;
		if (ret < 0) {
			ctx->video_decode_errors++;
			ctx->video_corrupted = true;
			if (ctx->video_decode_errors >= 3) {
				uint64_t now_us = (uint64_t)av_gettime();
				bool do_flush = should_flush_decoder(
					&ctx->video_last_decoder_flush_time_us,
					now_us);
				if (should_log_decoder_warning(
					    &ctx->video_last_decoder_warning_time_us,
					    now_us)) {
					blog(LOG_WARNING,
					     "[irl-source] Video decoder receive: corruption burst (%d consecutive errors)%s",
					     ctx->video_decode_errors,
					     do_flush ? ", flushing"
						      : ", flush cooldown active");
				}
				if (do_flush) {
					avcodec_flush_buffers(ctx->video_dec_ctx);
					ctx->video_decoder_flushes++;
				}
				ctx->video_decode_errors = 0;
			}
			break;
		}

		ctx->video_decode_errors = 0;
		irl_handle_video_frame(ctx, frame);
		av_frame_unref(frame);
	}
}
