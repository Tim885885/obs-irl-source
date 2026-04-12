/*
 * obs-irl-source — IRL streaming source plugin for OBS
 * https://irlserver.com
 *
 * Copyright (C) 2026 Thomas Lekanger
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * receiver.c — FFmpeg open/read thread (protocol-agnostic)
 *
 * Opens any FFmpeg-supported URL, decodes audio+video, and feeds
 * decoded frames to the jitter buffer / video handler.
 */

#include "../include/irl-source.h"
#include "receiver-internal.h"

/* ── Main read loop ───────────────────────────────────────── */

void *irl_audio_thread(void *data)
{
	struct irl_source *ctx = data;

	while (ctx->thread_active) {
		if (ctx->reconnecting) {
			os_sleep_ms(1);
			continue;
		}

		bool pumped = false;
		for (int i = 0; i < 16 && ctx->thread_active; i++) {
			pthread_mutex_lock(&ctx->audio_state_lock);
			bool ok = irl_pump_audio_once(ctx);
			pthread_mutex_unlock(&ctx->audio_state_lock);
			if (!ok)
				break;
			pumped = true;
		}

		if (!pumped)
			os_sleep_ms(1);
	}

	return NULL;
}

void *irl_receiver_thread(void *data)
{
	struct irl_source *ctx = data;
	AVPacket *pkt = av_packet_alloc();
	AVFrame *frame = av_frame_alloc();

	blog(LOG_INFO, "[irl-source] Receiver thread started for: %s",
	     ctx->config.url ? ctx->config.url : "(null)");

	while (ctx->thread_active) {
		if (!ctx->fmt_ctx) {
			if (!irl_open_stream(ctx)) {
				if (!irl_wait_for_reconnect(ctx))
					break;
				continue;
			}
			irl_prepare_new_connection(ctx);
		}

		int ret = av_read_frame(ctx->fmt_ctx, pkt);
		if (ret < 0) {
			irl_handle_stream_read_error(ctx, ret);
			continue;
		}

		if (pkt->stream_index == ctx->audio_stream_idx &&
		    ctx->audio_dec_ctx) {
			irl_handle_audio_packet(ctx, pkt, frame);
		} else if (pkt->stream_index == ctx->video_stream_idx &&
			   ctx->video_dec_ctx) {
			irl_handle_video_packet(ctx, pkt, frame);
		}

		av_packet_unref(pkt);
		irl_log_receiver_stats(ctx);
	}

	irl_close_ffmpeg(ctx);
	av_packet_free(&pkt);
	av_frame_free(&frame);
	return NULL;
}

void irl_receiver_stop(struct irl_source *ctx)
{
	if (!ctx->thread_active)
		return;

	ctx->thread_active = false;
	pthread_join(ctx->audio_thread, NULL);
	pthread_join(ctx->receiver_thread, NULL);
}
