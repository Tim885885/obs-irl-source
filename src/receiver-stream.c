/*
 * obs-irl-source — IRL streaming source plugin for OBS
 * https://irlserver.com
 *
 * Copyright (C) 2026 Thomas Lekanger
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <stdlib.h>
#include <string.h>

#ifdef _MSC_VER
#define strtok_r strtok_s
#endif

#include "receiver-internal.h"

static void apply_demuxer_options(AVDictionary **opts, const char *url,
				  const char *extra, int network_buffer_mb)
{
	av_dict_set(opts, "probesize", "5000000", 0);
	av_dict_set(opts, "analyzeduration", "5000000", 0);
	av_dict_set(opts, "fflags", "+genpts+discardcorrupt", 0);
	av_dict_set(opts, "flush_packets", "1", 0);
	av_dict_set(opts, "thread_queue_size", "1024", 0);
	av_dict_set(opts, "reconnect", "1", 0);
	av_dict_set(opts, "reconnect_streamed", "1", 0);

	if (network_buffer_mb > 0) {
		char buf_size[32];
		snprintf(buf_size, sizeof(buf_size), "%d",
			 network_buffer_mb * 1024 * 1024);
		av_dict_set(opts, "buffer_size", buf_size, 0);
	}

	if (url && strstr(url, "srt://")) {
		av_dict_set(opts, "latency", "200000", 0);
		if (network_buffer_mb > 0) {
			char recv_buf[32];
			snprintf(recv_buf, sizeof(recv_buf), "%d",
				 network_buffer_mb * 1024 * 1024);
			av_dict_set(opts, "recv_buffer_size", recv_buf, 0);
		}
	}

	if (extra && *extra) {
		char *dup = av_strdup(extra);
		char *saveptr = NULL;
		char *token = strtok_r(dup, " ", &saveptr);
		while (token) {
			char *eq = strchr(token, '=');
			if (eq) {
				*eq = '\0';
				av_dict_set(opts, token, eq + 1, 0);
			}
			token = strtok_r(NULL, " ", &saveptr);
		}
		av_free(dup);
	}
}

static const enum AVHWDeviceType hw_device_types[] = {
#ifdef _WIN32
	AV_HWDEVICE_TYPE_D3D11VA,
	AV_HWDEVICE_TYPE_CUDA,
#elif defined(__APPLE__)
	AV_HWDEVICE_TYPE_VIDEOTOOLBOX,
#else
	AV_HWDEVICE_TYPE_VAAPI,
	AV_HWDEVICE_TYPE_CUDA,
#endif
	AV_HWDEVICE_TYPE_NONE,
};

static AVCodecContext *open_decoder(struct irl_source *src, AVStream *stream,
				    bool try_hw)
{
	const AVCodec *codec = avcodec_find_decoder(stream->codecpar->codec_id);
	if (!codec)
		return NULL;

	AVCodecContext *ctx = avcodec_alloc_context3(codec);
	if (!ctx)
		return NULL;

	if (avcodec_parameters_to_context(ctx, stream->codecpar) < 0) {
		avcodec_free_context(&ctx);
		return NULL;
	}

	ctx->pkt_timebase = stream->time_base;
	if (stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
		ctx->thread_count = 1;
		ctx->thread_type = 0;
	} else {
		ctx->thread_count = 0;
	}

	if (try_hw && stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
		for (int i = 0; hw_device_types[i] != AV_HWDEVICE_TYPE_NONE;
		     i++) {
			if (src->hw_device_ctx)
				break;
			int err = av_hwdevice_ctx_create(
				&src->hw_device_ctx, hw_device_types[i], NULL,
				NULL, 0);
			if (err == 0) {
				blog(LOG_INFO,
				     "[irl-source] Using hardware device: %s",
				     av_hwdevice_get_type_name(
					     hw_device_types[i]));
			} else {
				src->hw_device_ctx = NULL;
			}
		}
		if (src->hw_device_ctx)
			ctx->hw_device_ctx = av_buffer_ref(src->hw_device_ctx);
	}

	if (avcodec_open2(ctx, codec, NULL) < 0) {
		if (ctx->hw_device_ctx) {
			avcodec_free_context(&ctx);
			av_buffer_unref(&src->hw_device_ctx);
			blog(LOG_INFO,
			     "[irl-source] Hardware decode failed, falling back to software");
			return open_decoder(src, stream, false);
		}
		avcodec_free_context(&ctx);
		return NULL;
	}

	if (ctx->hw_device_ctx)
		src->using_hw_decode = true;

	return ctx;
}

void irl_close_ffmpeg(struct irl_source *ctx)
{
	if (ctx->swr_ctx) {
		swr_free(&ctx->swr_ctx);
		ctx->swr_ctx = NULL;
	}
	ctx->swr_in_rate = 0;
	ctx->swr_in_channels = 0;
	ctx->swr_in_format = AV_SAMPLE_FMT_NONE;
	if (ctx->sws_ctx) {
		sws_freeContext(ctx->sws_ctx);
		ctx->sws_ctx = NULL;
	}

	if (ctx->audio_dec_ctx) {
		avcodec_free_context(&ctx->audio_dec_ctx);
		ctx->audio_dec_ctx = NULL;
	}
	if (ctx->video_dec_ctx) {
		avcodec_free_context(&ctx->video_dec_ctx);
		ctx->video_dec_ctx = NULL;
	}
	if (ctx->fmt_ctx) {
		avformat_close_input(&ctx->fmt_ctx);
		ctx->fmt_ctx = NULL;
	}
	ctx->audio_stream_idx = -1;
	ctx->video_stream_idx = -1;
	ctx->using_hw_decode = false;
}

static int interrupt_cb(void *opaque)
{
	struct irl_source *ctx = opaque;
	return !ctx->thread_active;
}

bool irl_open_stream(struct irl_source *ctx)
{
	AVDictionary *opts = NULL;
	apply_demuxer_options(&opts, ctx->config.url, ctx->config.ffmpeg_options,
			      ctx->config.network_buffer_mb);

	blog(LOG_INFO, "[irl-source] Connecting to: %s", ctx->config.url);

	ctx->fmt_ctx = avformat_alloc_context();
	if (!ctx->fmt_ctx) {
		blog(LOG_ERROR, "[irl-source] Failed to allocate format context");
		av_dict_free(&opts);
		return false;
	}
	ctx->fmt_ctx->interrupt_callback.callback = interrupt_cb;
	ctx->fmt_ctx->interrupt_callback.opaque = ctx;

	int ret = avformat_open_input(&ctx->fmt_ctx, ctx->config.url, NULL,
				      &opts);
	av_dict_free(&opts);
	if (ret < 0) {
		char errbuf[AV_ERROR_MAX_STRING_SIZE];
		av_strerror(ret, errbuf, sizeof(errbuf));
		blog(LOG_WARNING, "[irl-source] Failed to open input: %s",
		     errbuf);
		return false;
	}

	blog(LOG_INFO, "[irl-source] Input opened, probing streams...");

	if (avformat_find_stream_info(ctx->fmt_ctx, NULL) < 0) {
		blog(LOG_WARNING, "[irl-source] Failed to find stream info");
		avformat_close_input(&ctx->fmt_ctx);
		return false;
	}

	ctx->audio_stream_idx = -1;
	ctx->video_stream_idx = -1;

	for (unsigned i = 0; i < ctx->fmt_ctx->nb_streams; i++) {
		AVStream *s = ctx->fmt_ctx->streams[i];
		if (s->codecpar->codec_type == AVMEDIA_TYPE_VIDEO &&
		    ctx->video_stream_idx < 0) {
			bool try_hw = (ctx->config.hw_decode == 0);
			ctx->video_dec_ctx = open_decoder(ctx, s, try_hw);
			if (ctx->video_dec_ctx) {
				ctx->video_stream_idx = (int)i;
				blog(LOG_INFO,
				     "[irl-source] Video stream %u: %s %dx%d%s",
				     i, avcodec_get_name(s->codecpar->codec_id),
				     s->codecpar->width, s->codecpar->height,
				     ctx->using_hw_decode ? " (NVDEC)" : " (SW)");
			} else {
				blog(LOG_WARNING,
				     "[irl-source] Failed to open video decoder for stream %u (%s)",
				     i, avcodec_get_name(s->codecpar->codec_id));
			}
		} else if (s->codecpar->codec_type == AVMEDIA_TYPE_AUDIO &&
			   ctx->audio_stream_idx < 0) {
			ctx->audio_dec_ctx = open_decoder(ctx, s, false);
			if (ctx->audio_dec_ctx) {
				ctx->audio_stream_idx = (int)i;
				blog(LOG_INFO,
				     "[irl-source] Audio stream %u: %s %dHz %dch",
				     i, avcodec_get_name(s->codecpar->codec_id),
				     s->codecpar->sample_rate,
				     s->codecpar->ch_layout.nb_channels);
			} else {
				blog(LOG_WARNING,
				     "[irl-source] Failed to open audio decoder for stream %u",
				     i);
			}
		}
	}

	if (ctx->video_stream_idx < 0 && ctx->audio_stream_idx < 0) {
		blog(LOG_WARNING,
		     "[irl-source] No usable audio or video streams found");
		irl_close_ffmpeg(ctx);
		return false;
	}

	blog(LOG_INFO, "[irl-source] Stream opened (video=%d, audio=%d)",
	     ctx->video_stream_idx, ctx->audio_stream_idx);

	if (ctx->audio_stream_idx >= 0) {
		AVStream *as = ctx->fmt_ctx->streams[ctx->audio_stream_idx];
		pts_repair_init(&ctx->pts_state, ctx->config.small_gap_ms,
				ctx->config.large_gap_ms, as->time_base.num,
				as->time_base.den);
	}

	return true;
}

void irl_prepare_new_connection(struct irl_source *ctx)
{
	ctx->reconnecting = false;
	ctx->first_keyframe_received = false;
	ctx->video_ts_init = false;
	ctx->fade_in_pending = true;
	ctx->fade_in_frames_remaining = 0;
	ctx->startup_audio_warmup_remaining_ms = IRL_STARTUP_AUDIO_WARMUP_MS;
}

bool irl_wait_for_reconnect(struct irl_source *ctx)
{
	ctx->reconnecting = true;
	ctx->reconnect_count++;
	blog(LOG_INFO, "[irl-source] Reconnecting in %ds...",
	     ctx->config.reconnect_delay);
	for (int i = 0; i < ctx->config.reconnect_delay * 10 &&
			ctx->thread_active;
	     i++) {
		av_usleep(100000);
	}
	ctx->reconnecting = false;
	return ctx->thread_active;
}

static void fade_out_buffered_audio(struct irl_source *ctx)
{
	int buffered_ms = audio_buffer_fill_ms_locked(&ctx->audio_buf);
	if (!ctx->audio_buf.data || buffered_ms <= 0)
		return;

	size_t fade_bytes =
		audio_buffer_ms_to_bytes(&ctx->audio_buf, IRL_FADE_DURATION_MS);
	size_t buffered_bytes =
		audio_buffer_ms_to_bytes(&ctx->audio_buf, buffered_ms);
	if (fade_bytes > buffered_bytes)
		fade_bytes = buffered_bytes;
	if (fade_bytes == 0)
		return;

	uint8_t *fade_buf = malloc(fade_bytes);
	if (!fade_buf)
		return;

	size_t got = audio_buffer_read_with_fade_out(&ctx->audio_buf, fade_buf,
						     fade_bytes);
	if (got > 0) {
		uint32_t fade_frames = (uint32_t)(
			got / (ctx->audio_buf.channels *
			       ctx->audio_buf.bytes_per_sample));
		struct obs_source_audio a = {0};
		a.data[0] = fade_buf;
		a.frames = fade_frames;
		a.format = AUDIO_FORMAT_FLOAT;
		a.speakers = (enum speaker_layout)ctx->audio_buf.channels;
		a.samples_per_sec = (uint32_t)ctx->audio_buf.sample_rate;
		a.timestamp = irl_next_audio_timestamp(
			ctx, (int)fade_frames, ctx->audio_buf.sample_rate);
		obs_source_output_audio(ctx->source, &a);
	}

	free(fade_buf);
}

bool irl_handle_stream_read_error(struct irl_source *ctx, int read_ret)
{
	char errbuf[AV_ERROR_MAX_STRING_SIZE];
	ctx->reconnecting = true;
	av_strerror(read_ret, errbuf, sizeof(errbuf));
	blog(LOG_WARNING,
	     "[irl-source] Stream read error: %s (video_frames=%llu, audio_frames=%llu)",
	     errbuf, (unsigned long long)ctx->total_video_frames,
	     (unsigned long long)ctx->total_audio_frames);

	fade_out_buffered_audio(ctx);
	irl_close_ffmpeg(ctx);
	pts_repair_reset(&ctx->pts_state);
	pthread_mutex_lock(&ctx->audio_state_lock);
	audio_buffer_flush(&ctx->audio_buf);
	irl_stretch_reset(ctx);
	irl_reset_stream_timing_state(ctx);
	pthread_mutex_unlock(&ctx->audio_state_lock);

	ctx->current_speed = 1.0f;
	ctx->last_speed_adjust_time = 0;
	ctx->audio_pll_corrections = 0;
	ctx->audio_pll_hard_resets = 0;
	ctx->audio_underruns = 0;
	ctx->audio_resync_skipped_chunks = 0;
	ctx->pts_repairs = 0;
	ctx->silence_insertions = 0;
	ctx->total_audio_frames = 0;
	ctx->total_video_frames = 0;
	ctx->last_stats_time = 0;
	ctx->fade_in_pending = true;
	return true;
}

void irl_log_receiver_stats(struct irl_source *ctx)
{
	uint64_t now = os_gettime_ns();
	if (now - ctx->last_stats_time <= 30000000000ULL)
		return;

	ctx->last_stats_time = now;
	blog(LOG_INFO,
	     "[irl-source] Stats: video=%llu audio=%llu "
	     "buf=%dms speed=%.3f pts_repairs=%llu "
	     "silence=%llu underruns=%llu resync_skips=%llu "
	     "obs_lead=%lldms ts_drift=%lldms chunk=%u@%u "
	     "stream_chunk=%llums obs_chunk=%llums "
	     "pll=%llu hard_resets=%llu res=%dx%d",
	     (unsigned long long)ctx->total_video_frames,
	     (unsigned long long)ctx->total_audio_frames,
	     audio_buffer_fill_ms_locked(&ctx->audio_buf),
	     (double)ctx->current_speed,
	     (unsigned long long)ctx->pts_repairs,
	     (unsigned long long)ctx->silence_insertions,
	     (unsigned long long)ctx->audio_underruns,
	     (unsigned long long)ctx->audio_resync_skipped_chunks,
	     (long long)(ctx->audio_last_obs_lead_ns / 1000000LL),
	     (long long)(ctx->audio_last_ts_drift_ns / 1000000LL),
	     ctx->audio_last_frames_out, ctx->audio_last_samples_per_sec,
	     (unsigned long long)(ctx->audio_last_chunk_stream_duration_ns /
				  1000000ULL),
	     (unsigned long long)(ctx->audio_last_chunk_obs_duration_ns /
				  1000000ULL),
	     (unsigned long long)ctx->audio_pll_corrections,
	     (unsigned long long)ctx->audio_pll_hard_resets,
	     ctx->last_video_width, ctx->last_video_height);
}
