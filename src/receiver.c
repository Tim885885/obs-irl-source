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

#include <stdlib.h>
#include <string.h>

#ifdef _MSC_VER
#define strtok_r strtok_s
#endif

#include "../include/irl-source.h"
#include "receiver-internal.h"

/* ── Internal helpers ─────────────────────────────────────── */

static void apply_demuxer_options(AVDictionary **opts, const char *url,
				  const char *extra, int network_buffer_mb)
{
	/* Live-stream tuned defaults.
	 * HEVC transport streams can need a fairly generous probe window
	 * before FFmpeg sees enough codec parameter data to identify the
	 * video stream reliably. Bias toward correct video detection. */
	av_dict_set(opts, "probesize", "5000000", 0);       /* 5 MB */
	av_dict_set(opts, "analyzeduration", "5000000", 0); /* 5 s */
	av_dict_set(opts, "fflags", "+genpts+discardcorrupt", 0);
	av_dict_set(opts, "flush_packets", "1", 0);
	av_dict_set(opts, "thread_queue_size", "1024", 0);
	av_dict_set(opts, "reconnect", "1", 0);
	av_dict_set(opts, "reconnect_streamed", "1", 0);

	/* Network buffer: absorbs transport-level jitter before decoding.
	 * Higher values = more resilient to network spikes, but add latency. */
	if (network_buffer_mb > 0) {
		char buf_size[32];
		snprintf(buf_size, sizeof(buf_size), "%d",
			 network_buffer_mb * 1024 * 1024);
		av_dict_set(opts, "buffer_size", buf_size, 0);
	}

	/* SRT-specific: set receive buffer and latency */
	if (url && strstr(url, "srt://")) {
		av_dict_set(opts, "latency", "200000", 0); /* 200ms default */
		if (network_buffer_mb > 0) {
			char recv_buf[32];
			snprintf(recv_buf, sizeof(recv_buf), "%d",
				 network_buffer_mb * 1024 * 1024);
			av_dict_set(opts, "recv_buffer_size", recv_buf, 0);
		}
	}

	/* User-provided overrides */
	if (extra && *extra) {
		/* Parse "key1=val1 key2=val2" format */
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

/* Hardware decode preference order — tries each until one works.
 * Covers NVIDIA (CUDA), Intel (QSV/VAAPI), AMD (D3D11VA/VAAPI). */
static const enum AVHWDeviceType hw_device_types[] = {
#ifdef _WIN32
	AV_HWDEVICE_TYPE_D3D11VA,     /* AMD + Intel + NVIDIA on Windows */
	AV_HWDEVICE_TYPE_CUDA,        /* NVIDIA NVDEC */
#elif defined(__APPLE__)
	AV_HWDEVICE_TYPE_VIDEOTOOLBOX, /* Apple VideoToolbox */
#else
	AV_HWDEVICE_TYPE_VAAPI,       /* Intel + AMD on Linux */
	AV_HWDEVICE_TYPE_CUDA,        /* NVIDIA on Linux */
#endif
	AV_HWDEVICE_TYPE_NONE,        /* sentinel */
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
		/* Audio decoders are cheap; single-threaded decode avoids
		 * reordering/latency overhead from FFmpeg's auto threading. */
		ctx->thread_count = 1;
		ctx->thread_type = 0;
	} else {
		ctx->thread_count = 0; /* auto */
	}

	/* Try hardware decoding for video streams */
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
			ctx->hw_device_ctx =
				av_buffer_ref(src->hw_device_ctx);
	}

	if (avcodec_open2(ctx, codec, NULL) < 0) {
		/* If hw decode failed, retry with software */
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

static void close_ffmpeg(struct irl_source *ctx)
{
	/* Free resampler/scaler so a reconnected stream with different
	 * audio format doesn't get stale contexts producing corrupt output. */
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

/* Interrupt callback: returns 1 to abort blocking I/O when shutting down.
 * Without this, av_read_frame() blocks forever if the remote dies without
 * sending TCP FIN (common IRL scenario — phone loses signal). */
static int interrupt_cb(void *opaque)
{
	struct irl_source *ctx = opaque;
	return !ctx->thread_active;
}

static bool open_stream(struct irl_source *ctx)
{
	AVDictionary *opts = NULL;
	apply_demuxer_options(&opts, ctx->config.url, ctx->config.ffmpeg_options,
			      ctx->config.network_buffer_mb);

	blog(LOG_INFO, "[irl-source] Connecting to: %s", ctx->config.url);

	/* Pre-allocate format context to install the interrupt callback
	 * before avformat_open_input performs blocking I/O. */
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

	/* Note: probing an HEVC/SRT stream mid-GOP produces expected
	 * "PPS id out of range" and "Skipping invalid undecodable NALU"
	 * errors until the first keyframe with parameter sets arrives.
	 * These are harmless and cannot be suppressed reliably —
	 * av_log_set_level is global/not thread-safe, and the errors
	 * come from internal codec contexts we don't control. */
	if (avformat_find_stream_info(ctx->fmt_ctx, NULL) < 0) {
		blog(LOG_WARNING,
		     "[irl-source] Failed to find stream info");
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
			ctx->video_dec_ctx =
				open_decoder(ctx, s, try_hw);
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
			ctx->audio_dec_ctx =
				open_decoder(ctx, s, false);
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
		close_ffmpeg(ctx);
		return false;
	}

	blog(LOG_INFO, "[irl-source] Stream opened (video=%d, audio=%d)",
	     ctx->video_stream_idx, ctx->audio_stream_idx);

	/* Initialise PTS repair for audio stream */
	if (ctx->audio_stream_idx >= 0) {
		AVStream *as =
			ctx->fmt_ctx->streams[ctx->audio_stream_idx];
		pts_repair_init(&ctx->pts_state, ctx->config.small_gap_ms,
				ctx->config.large_gap_ms, as->time_base.num,
				as->time_base.den);
	}

	return true;
}

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
			if (!open_stream(ctx)) {
				/* Reconnect after delay */
				ctx->reconnecting = true;
				ctx->reconnect_count++;
				blog(LOG_INFO,
				     "[irl-source] Reconnecting in %ds...",
				     ctx->config.reconnect_delay);
				for (int i = 0;
				     i < ctx->config.reconnect_delay * 10 &&
				     ctx->thread_active;
				     i++) {
					av_usleep(100000); /* 100ms */
				}
				ctx->reconnecting = false;
				continue;
			}
			ctx->reconnecting = false;
			/* Reset state for new connection */
			ctx->first_keyframe_received = false;
			ctx->video_ts_init = false;
			ctx->fade_in_pending = true;
			ctx->fade_in_frames_remaining = 0;
			ctx->startup_audio_warmup_remaining_ms =
				IRL_STARTUP_AUDIO_WARMUP_MS;
		}

		int ret = av_read_frame(ctx->fmt_ctx, pkt);
		if (ret < 0) {
			ctx->reconnecting = true;
			char errbuf[AV_ERROR_MAX_STRING_SIZE];
			av_strerror(ret, errbuf, sizeof(errbuf));
			blog(LOG_WARNING,
			     "[irl-source] Stream read error: %s (video_frames=%llu, audio_frames=%llu)",
			     errbuf,
			     (unsigned long long)ctx->total_video_frames,
			     (unsigned long long)ctx->total_audio_frames);

			/* Fade out remaining audio to avoid click/pop */
			int buffered_ms = audio_buffer_fill_ms_locked(
				&ctx->audio_buf);
			if (ctx->audio_buf.data && buffered_ms > 0) {
				size_t fade_bytes = audio_buffer_ms_to_bytes(
					&ctx->audio_buf,
					IRL_FADE_DURATION_MS);
				size_t buffered_bytes = audio_buffer_ms_to_bytes(
					&ctx->audio_buf, buffered_ms);
				if (fade_bytes > buffered_bytes)
					fade_bytes = buffered_bytes;
				if (fade_bytes > 0) {
					uint8_t *fade_buf = malloc(fade_bytes);
					if (fade_buf) {
						size_t got =
							audio_buffer_read_with_fade_out(
								&ctx->audio_buf,
								fade_buf,
								fade_bytes);
						if (got > 0) {
							uint32_t fade_frames = (uint32_t)(
								got /
								(ctx->audio_buf.channels *
								 ctx->audio_buf.bytes_per_sample));
							struct obs_source_audio
								a = {0};
							a.data[0] = fade_buf;
							a.frames = fade_frames;
							a.format =
								AUDIO_FORMAT_FLOAT;
							a.speakers =
								(enum speaker_layout)
									ctx->audio_buf
										.channels;
							a.samples_per_sec =
								(uint32_t)
									ctx->audio_buf
										.sample_rate;
							a.timestamp = irl_next_audio_timestamp(
								ctx, (int)fade_frames,
								ctx->audio_buf.sample_rate);
							obs_source_output_audio(
								ctx->source,
								&a);
						}
						free(fade_buf);
					}
				}
			}

			/* OBS_SOURCE_ASYNC_VIDEO holds the last frame on screen
			 * when no new frames arrive.  This is intentional:
			 * viewers see a frozen image instead of black during
			 * disconnection.  Do NOT call
			 * obs_source_output_video(NULL) here. */

			close_ffmpeg(ctx);
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
			continue;
		}

		if (pkt->stream_index == ctx->audio_stream_idx &&
		    ctx->audio_dec_ctx) {
			ret = avcodec_send_packet(ctx->audio_dec_ctx, pkt);
			if (ret < 0 && ret != AVERROR(EAGAIN) &&
			    ret != AVERROR_EOF) {
				/* Only flush after 3 consecutive errors.
				 * A single corrupt packet shouldn't reset
				 * the decoder — the decoder can usually
				 * continue and error-conceal the damage.
				 * Flushing loses decoder state (reference
				 * frames) and requires a new keyframe. */
				ctx->audio_decode_errors++;
				if (ctx->audio_decode_errors >= 3) {
					uint64_t now_us =
						(uint64_t)av_gettime();
					bool do_flush = should_flush_decoder(
						&ctx->audio_last_decoder_flush_time_us,
						now_us);
					if (should_log_decoder_warning(
						    &ctx->audio_last_decoder_warning_time_us,
						    now_us)) {
						blog(LOG_WARNING,
						     "[irl-source] Audio decoder: corruption burst (%d consecutive errors)%s",
						     ctx->audio_decode_errors,
						     do_flush
							     ? ", flushing"
							     : ", suppressing repeated flush");
					}
					if (do_flush) {
						avcodec_flush_buffers(
							ctx->audio_dec_ctx);
					}
					ctx->audio_decode_errors = 0;
				}
			} else {
				ctx->audio_decode_errors = 0;
			}
			/* Always drain queued frames, even if send_packet
			 * failed.  The decoder may have frames buffered
			 * from previous packets that are ready to output.
			 * The old code used `while (ret >= 0)` which
			 * skipped receive_frame entirely when send_packet
			 * returned an error — silently losing frames. */
			for (;;) {
				ret = avcodec_receive_frame(ctx->audio_dec_ctx,
							   frame);
				if (ret == AVERROR(EAGAIN) ||
				    ret == AVERROR_EOF)
					break;
				if (ret < 0) {
					ctx->audio_decode_errors++;
					if (ctx->audio_decode_errors >= 3) {
						uint64_t now_us =
							(uint64_t)av_gettime();
						bool do_flush = should_flush_decoder(
							&ctx->audio_last_decoder_flush_time_us,
							now_us);
						if (should_log_decoder_warning(
							    &ctx->audio_last_decoder_warning_time_us,
							    now_us)) {
							blog(LOG_WARNING,
							     "[irl-source] Audio decoder receive: corruption burst (%d consecutive errors)%s",
							     ctx->audio_decode_errors,
							     do_flush
								     ? ", resetting audio state"
								     : ", reset cooldown active");
						}
						if (do_flush) {
							avcodec_flush_buffers(
								ctx->audio_dec_ctx);
							pthread_mutex_lock(
								&ctx->audio_state_lock);
							audio_buffer_flush(
								&ctx->audio_buf);
							irl_stretch_reset(ctx);
							irl_reset_audio_timing_state(
								ctx);
							pthread_mutex_unlock(
								&ctx->audio_state_lock);
							pts_repair_reset(
								&ctx->pts_state);
							if (ctx->fmt_ctx &&
							    ctx->audio_stream_idx >= 0) {
								AVStream *as =
									ctx->fmt_ctx->streams
										[ctx->audio_stream_idx];
								pts_repair_init(
									&ctx->pts_state,
									ctx->config.small_gap_ms,
									ctx->config.large_gap_ms,
									as->time_base.num,
									as->time_base.den);
							}
						}
						ctx->audio_decode_errors = 0;
					}
					break;
				}
				ctx->audio_decode_errors = 0;
				irl_handle_audio_frame(ctx, frame);
				av_frame_unref(frame);
			}
		} else if (pkt->stream_index == ctx->video_stream_idx &&
			   ctx->video_dec_ctx) {
			ret = avcodec_send_packet(ctx->video_dec_ctx, pkt);
			if (ret < 0 && ret != AVERROR(EAGAIN) &&
			    ret != AVERROR_EOF) {
				ctx->video_decode_errors++;
				ctx->video_corrupted = true;
				if (ctx->video_decode_errors >= 3) {
					uint64_t now_us =
						(uint64_t)av_gettime();
					bool do_flush = should_flush_decoder(
						&ctx->video_last_decoder_flush_time_us,
						now_us);
					if (should_log_decoder_warning(
						    &ctx->video_last_decoder_warning_time_us,
						    now_us)) {
						blog(LOG_WARNING,
						     "[irl-source] Video decoder: corruption burst (%d consecutive errors)%s",
						     ctx->video_decode_errors,
						     do_flush
							     ? ", flushing"
							     : ", flush cooldown active");
					}
					if (do_flush)
						avcodec_flush_buffers(
							ctx->video_dec_ctx);
					ctx->video_decode_errors = 0;
				}
			} else {
				ctx->video_decode_errors = 0;
			}
			for (;;) {
				ret = avcodec_receive_frame(ctx->video_dec_ctx,
							   frame);
				if (ret == AVERROR(EAGAIN) ||
				    ret == AVERROR_EOF)
					break;
				if (ret < 0) {
					ctx->video_decode_errors++;
					ctx->video_corrupted = true;
					if (ctx->video_decode_errors >= 3) {
						uint64_t now_us =
							(uint64_t)av_gettime();
						bool do_flush = should_flush_decoder(
							&ctx->video_last_decoder_flush_time_us,
							now_us);
						if (should_log_decoder_warning(
							    &ctx->video_last_decoder_warning_time_us,
							    now_us)) {
							blog(LOG_WARNING,
							     "[irl-source] Video decoder receive: corruption burst (%d consecutive errors)%s",
							     ctx->video_decode_errors,
							     do_flush
								     ? ", flushing"
								     : ", flush cooldown active");
						}
						if (do_flush)
							avcodec_flush_buffers(
								ctx->video_dec_ctx);
						ctx->video_decode_errors = 0;
					}
					break;
				}
				ctx->video_decode_errors = 0;
				irl_handle_video_frame(ctx, frame);
				av_frame_unref(frame);
			}
		}

		av_packet_unref(pkt);

		/* Periodic stats logging (every 30 seconds) */
		uint64_t now = os_gettime_ns();
		if (now - ctx->last_stats_time > 30000000000ULL) {
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
			     ctx->audio_last_frames_out,
			     ctx->audio_last_samples_per_sec,
			     (unsigned long long)(ctx->audio_last_chunk_stream_duration_ns /
						  1000000ULL),
			     (unsigned long long)(ctx->audio_last_chunk_obs_duration_ns /
						  1000000ULL),
			     (unsigned long long)ctx->audio_pll_corrections,
			     (unsigned long long)ctx->audio_pll_hard_resets,
			     ctx->last_video_width,
			     ctx->last_video_height);
		}
	}

	close_ffmpeg(ctx);
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
