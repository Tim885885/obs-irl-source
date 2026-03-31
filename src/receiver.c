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

/* ── Internal helpers ─────────────────────────────────────── */

static void apply_demuxer_options(AVDictionary **opts, const char *url,
				  const char *extra, int network_buffer_mb)
{
	/* Live-stream tuned defaults.
	 * HEVC/H.265 over SRT needs enough probe data to capture a keyframe
	 * with SPS/PPS — 500KB/0.5s is too small for typical 2-4s GOPs. */
	av_dict_set(opts, "probesize", "5000000", 0);   /* 5 MB */
	av_dict_set(opts, "analyzeduration", "5000000", 0); /* 5 s */
	av_dict_set(opts, "fflags", "+genpts", 0);
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
		av_dict_set(opts, "latency", "300000", 0); /* 300ms default */
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

	ctx->thread_count = 0; /* auto */

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

static uint64_t next_audio_timestamp(struct irl_source *ctx, int base_samples,
				     int out_rate)
{
	int64_t frame_ns =
		(int64_t)base_samples * 1000000000LL / out_rate;
	int64_t startup_lead_ns =
		ctx->config.low_latency_audio ? 0 : frame_ns * 2;
	uint64_t now = os_gettime_ns();

	/* Buffered mode is now driven by the dedicated audio pump.
	 * Use the actual queued OBS end timestamp as the playout
	 * timeline, and only re-anchor to a small positive lead when
	 * that queue has collapsed. This avoids the old oscillation
	 * between "late" and "PLL catch-up" states. */
	if (!ctx->config.low_latency_audio) {
		uint64_t target_ts =
			now + (uint64_t)(startup_lead_ns > 0 ? startup_lead_ns : 0);
		uint64_t queued_end_ts = ctx->latest_audio_obs_end_ts_ns;
		uint64_t audio_ts = 0;

		if (!ctx->audio_ts_init || queued_end_ts == 0) {
			audio_ts = target_ts;
			ctx->audio_ts_init = true;
		} else if (queued_end_ts >= target_ts) {
			audio_ts = queued_end_ts;
		} else {
			ctx->audio_pll_corrections++;
			audio_ts = target_ts;
		}

		ctx->audio_sys_base = audio_ts;
		ctx->audio_pll_offset_ns = 0;
		ctx->audio_last_ts_drift_ns =
			(int64_t)audio_ts - (int64_t)now;
		return audio_ts;
	}

	if (!ctx->audio_ts_init) {
		int64_t initial_ts = (int64_t)now + startup_lead_ns;
		ctx->audio_sys_base = (uint64_t)(initial_ts > 0 ? initial_ts : 0);
		ctx->audio_pll_offset_ns = 0;
		ctx->audio_ts_init = true;
	}

	int64_t audio_ts_i64 =
		(int64_t)ctx->audio_sys_base + ctx->audio_pll_offset_ns;
	if (audio_ts_i64 < 0)
		audio_ts_i64 = 0;
	uint64_t audio_ts = (uint64_t)audio_ts_i64;
	int64_t drift = (int64_t)audio_ts - (int64_t)now;

	/* Buffered mode favors continuity with OBS's expected cadence.
	 * Low-latency mode keeps timestamps close to wall clock so
	 * async unbuffered/decoupled mode stays stable. */
	if (ctx->config.low_latency_audio) {
		if (drift < -10000000LL || drift > 10000000LL) {
			ctx->audio_pll_corrections++;
			int64_t base = (int64_t)now - ctx->audio_pll_offset_ns;
			ctx->audio_sys_base = (uint64_t)(base > 0 ? base : 0);
			audio_ts = now;
		}
	} else if (drift > 30000000LL) {
		ctx->audio_pll_corrections++;
		ctx->audio_pll_offset_ns -= frame_ns;
		audio_ts -= frame_ns;
	} else if (drift < -70000000LL) {
		ctx->audio_pll_corrections++;
		int64_t target_ts = (int64_t)now + startup_lead_ns;
		int64_t base = target_ts - ctx->audio_pll_offset_ns;
		ctx->audio_sys_base = (uint64_t)(base > 0 ? base : 0);
		audio_ts = (uint64_t)(target_ts > 0 ? target_ts : 0);
	}

	drift = (int64_t)audio_ts - (int64_t)now;
	if (drift < -500000000LL || drift > 500000000LL) {
		ctx->audio_pll_hard_resets++;
		ctx->audio_sys_base = now;
		ctx->audio_pll_offset_ns = 0;
		audio_ts = now;
	}

	ctx->audio_last_ts_drift_ns = drift;
	ctx->audio_pll_offset_ns += frame_ns;
	return audio_ts;
}

static void maybe_log_audio_timing_diag(struct irl_source *ctx, int fill_ms)
{
	uint64_t now = os_gettime_ns();
	bool severe_lead = ctx->audio_last_obs_lead_ns > 150000000LL;
	bool severe_drift = ctx->audio_last_ts_drift_ns < -40000000LL ||
			    ctx->audio_last_ts_drift_ns > 40000000LL;

	if (!severe_lead && !severe_drift)
		return;
	if (now - ctx->last_audio_diag_time < 5000000000ULL)
		return;

	ctx->last_audio_diag_time = now;
	blog(LOG_WARNING,
	     "[irl-source] Audio timing diag: obs_lead=%lldms ts_drift=%lldms "
	     "fill=%dms speed=%.3f chunk=%u@%u stream_chunk=%llums "
	     "obs_chunk=%llums underruns=%llu resync_skips=%llu "
	     "pll=%llu hard_resets=%llu repairs=%llu silence=%llu",
	     (long long)(ctx->audio_last_obs_lead_ns / 1000000LL),
	     (long long)(ctx->audio_last_ts_drift_ns / 1000000LL), fill_ms,
	     (double)ctx->current_speed, ctx->audio_last_frames_out,
	     ctx->audio_last_samples_per_sec,
	     (unsigned long long)(ctx->audio_last_chunk_stream_duration_ns /
				  1000000ULL),
	     (unsigned long long)(ctx->audio_last_chunk_obs_duration_ns /
				  1000000ULL),
	     (unsigned long long)ctx->audio_underruns,
	     (unsigned long long)ctx->audio_resync_skipped_chunks,
	     (unsigned long long)ctx->audio_pll_corrections,
	     (unsigned long long)ctx->audio_pll_hard_resets,
	     (unsigned long long)ctx->pts_repairs,
	     (unsigned long long)ctx->silence_insertions);
}

static void reset_stream_timing_state(struct irl_source *ctx)
{
	ctx->audio_ts_init = false;
	ctx->audio_pll_offset_ns = 0;
	ctx->audio_last_ts_drift_ns = 0;
	ctx->audio_last_obs_lead_ns = 0;
	ctx->audio_last_chunk_stream_duration_ns = 0;
	ctx->audio_last_chunk_obs_duration_ns = 0;
	ctx->audio_last_frames_out = 0;
	ctx->audio_last_samples_per_sec = 0;
	ctx->last_audio_diag_time = 0;
	ctx->video_ts_init = false;
	ctx->latest_audio_stream_pts_ns = 0;
	ctx->latest_audio_buffered_pts_ns = 0;
	ctx->latest_audio_buffered_end_pts_ns = 0;
	ctx->latest_video_stream_pts_ns = 0;
	ctx->latest_audio_obs_end_ts_ns = 0;
	ctx->decoded_frame_samples = 0;
	ctx->startup_audio_warmup_remaining_ms = 0;
	ctx->audio_decode_errors = 0;
	ctx->video_decode_errors = 0;
	ctx->video_corrupted = false;
	ctx->video_skip_logged = false;
}

static void reset_audio_timing_state(struct irl_source *ctx)
{
	ctx->audio_ts_init = false;
	ctx->audio_pll_offset_ns = 0;
	ctx->audio_last_ts_drift_ns = 0;
	ctx->audio_last_obs_lead_ns = 0;
	ctx->audio_last_chunk_stream_duration_ns = 0;
	ctx->audio_last_chunk_obs_duration_ns = 0;
	ctx->audio_last_frames_out = 0;
	ctx->audio_last_samples_per_sec = 0;
	ctx->last_audio_diag_time = 0;
	ctx->latest_audio_stream_pts_ns = 0;
	ctx->latest_audio_buffered_pts_ns = 0;
	ctx->latest_audio_buffered_end_pts_ns = 0;
	ctx->latest_audio_obs_end_ts_ns = 0;
	ctx->decoded_frame_samples = 0;
	ctx->startup_audio_warmup_remaining_ms = 0;
}

static void reanchor_audio_output_clock(struct irl_source *ctx)
{
	ctx->audio_ts_init = false;
	ctx->audio_pll_offset_ns = 0;
	ctx->audio_last_ts_drift_ns = 0;
	ctx->audio_last_obs_lead_ns = 0;
}

static int64_t audio_frame_pts(const AVFrame *frame)
{
	if (frame->best_effort_timestamp != AV_NOPTS_VALUE)
		return frame->best_effort_timestamp;
	if (frame->pts != AV_NOPTS_VALUE)
		return frame->pts;
	return AV_NOPTS_VALUE;
}

static int64_t video_frame_pts(const AVFrame *frame)
{
	if (frame->best_effort_timestamp != AV_NOPTS_VALUE)
		return frame->best_effort_timestamp;
	if (frame->pts != AV_NOPTS_VALUE)
		return frame->pts;
	return AV_NOPTS_VALUE;
}

static int audio_frame_duration_ms(int samples, int sample_rate)
{
	if (samples <= 0 || sample_rate <= 0)
		return 0;

	int64_t ms = (int64_t)samples * 1000LL / sample_rate;
	if (ms <= 0)
		ms = 1;
	return (int)ms;
}

static void finalize_audio_output(struct irl_source *ctx,
				  const struct obs_source_audio *obs_audio,
				  int64_t chunk_pts_ns,
				  uint64_t stream_duration_ns)
{
	ctx->latest_audio_buffered_pts_ns = chunk_pts_ns;
	ctx->latest_audio_buffered_end_pts_ns =
		chunk_pts_ns + (int64_t)stream_duration_ns;

	if (obs_audio->samples_per_sec > 0) {
		uint64_t audio_duration_ns =
			(uint64_t)obs_audio->frames * 1000000000ULL /
			(uint64_t)obs_audio->samples_per_sec;
		ctx->latest_audio_obs_end_ts_ns =
			obs_audio->timestamp + audio_duration_ns;
		ctx->audio_last_chunk_obs_duration_ns = audio_duration_ns;
	} else {
		ctx->latest_audio_obs_end_ts_ns = obs_audio->timestamp;
		ctx->audio_last_chunk_obs_duration_ns = 0;
	}

	ctx->audio_last_chunk_stream_duration_ns = stream_duration_ns;
	ctx->audio_last_frames_out = obs_audio->frames;
	ctx->audio_last_samples_per_sec = obs_audio->samples_per_sec;

	uint64_t after_output = os_gettime_ns();
	if (ctx->latest_audio_obs_end_ts_ns > after_output) {
		ctx->audio_last_obs_lead_ns =
			(int64_t)(ctx->latest_audio_obs_end_ts_ns - after_output);
	} else {
		ctx->audio_last_obs_lead_ns = 0;
	}

	maybe_log_audio_timing_diag(ctx,
				    audio_buffer_fill_ms_locked(&ctx->audio_buf));
	ctx->total_audio_frames++;
}

static bool maybe_resync_audio_buffer(struct irl_source *ctx, int64_t peek,
				      int fill_ms, int chunk_count,
				      bool reset_stretch)
{
	if (ctx->latest_audio_stream_pts_ns == 0 || chunk_count <= 1)
		return false;

	int64_t expected = ctx->latest_audio_stream_pts_ns -
			   (int64_t)fill_ms * 1000000LL;
	int64_t gap_ns = peek - expected;
	if (gap_ns >= -50000000LL)
		return false;

	int skipped = audio_buffer_skip_until_pts(&ctx->audio_buf, expected);
	if (skipped <= 0)
		return false;

	ctx->audio_resync_skipped_chunks += (uint64_t)skipped;
	if (reset_stretch)
		irl_stretch_reset(ctx);
	reanchor_audio_output_clock(ctx);
	ctx->current_speed = 1.0f;
	ctx->last_speed_adjust_time = 0;
	blog(LOG_INFO,
	     "[irl-source] Audio re-sync: skipped %d stale chunks (gap=%lldms)",
	     skipped, (long long)(gap_ns / 1000000));
	return true;
}

static bool pump_audio_once(struct irl_source *ctx)
{
	bool low_latency = ctx->config.low_latency_audio;
	int out_rate = ctx->audio_buf.sample_rate;
	int out_channels = ctx->audio_buf.channels;
	int bytes_per_sample = ctx->audio_buf.bytes_per_sample;
	int base_samples = ctx->decoded_frame_samples;

	if (!ctx->audio_buf.data || out_rate <= 0 || out_channels <= 0 ||
	    bytes_per_sample <= 0)
		return false;
	if (base_samples <= 0)
		base_samples = 960;

	uint64_t desired_lead_ns =
		low_latency ? 0
			    : ((uint64_t)base_samples * 1000000000ULL /
			       (uint64_t)out_rate) *
				      2ULL;
	uint64_t now = os_gettime_ns();
	if (ctx->latest_audio_obs_end_ts_ns != 0 &&
	    ctx->latest_audio_obs_end_ts_ns > now + desired_lead_ns) {
		return false;
	}

	int64_t peek = 0;
	int fill_ms = 0;
	int chunk_count = 0;
	bool has_audio = audio_buffer_peek_state(&ctx->audio_buf, &peek,
							 &fill_ms,
							 &chunk_count);
	bool use_stretch = ctx->config.adaptive_speed && !low_latency;
	int required_fill_ms = low_latency ? 0 : ctx->audio_buf.min_ms;
	if (!low_latency && ctx->latest_audio_buffered_end_pts_ns == 0)
		required_fill_ms = ctx->audio_buf.target_ms;

	if (!(low_latency ? has_audio : (fill_ms >= required_fill_ms))) {
		if (!low_latency && !has_audio &&
		    ctx->latest_audio_buffered_end_pts_ns > 0) {
			ctx->audio_underruns++;
			size_t silence_bytes =
				(size_t)base_samples * ctx->audio_buf.frame_size;
			uint8_t *silence_buf = calloc(1, silence_bytes);
			if (!silence_buf)
				return false;

			struct obs_source_audio obs_audio = {0};
			obs_audio.data[0] = silence_buf;
			obs_audio.frames = (uint32_t)base_samples;
			obs_audio.format = AUDIO_FORMAT_FLOAT;
			obs_audio.speakers =
				(enum speaker_layout)ctx->audio_buf.channels;
			obs_audio.timestamp = next_audio_timestamp(
				ctx, base_samples, ctx->audio_buf.sample_rate);
			obs_audio.samples_per_sec =
				(uint32_t)ctx->audio_buf.sample_rate;
			obs_source_output_audio(ctx->source, &obs_audio);

			uint64_t stream_duration_ns =
				(uint64_t)obs_audio.frames * 1000000000ULL /
				(uint64_t)ctx->audio_buf.sample_rate;
			int64_t chunk_pts_ns =
				ctx->latest_audio_buffered_end_pts_ns;
			finalize_audio_output(ctx, &obs_audio, chunk_pts_ns,
					      stream_duration_ns);
			free(silence_buf);
			return true;
		}
		return false;
	}

	if (has_audio &&
	    maybe_resync_audio_buffer(ctx, peek, fill_ms, chunk_count,
				      use_stretch)) {
		return false;
	}

	float speed =
		ctx->config.adaptive_speed ? irl_speed_get(ctx) : 1.0f;
	if (speed < 0.9f)
		speed = 0.9f;
	if (speed > 1.1f)
		speed = 1.1f;
	uint8_t *out_buf = NULL;
	int64_t chunk_pts_ns = 0;
	uint64_t stream_duration_ns = 0;
	uint32_t frames_out = 0;

	if (use_stretch) {
		for (int attempt = 0;
		     attempt < 4 &&
		     irl_stretch_available_frames(ctx) < base_samples;
		     attempt++) {
			int64_t stretch_peek = 0;
			int stretch_fill_ms = 0;
			int stretch_chunk_count = 0;
			bool stretch_has_audio =
				audio_buffer_peek_state(&ctx->audio_buf,
							&stretch_peek,
							&stretch_fill_ms,
							&stretch_chunk_count);
			if (!(low_latency ? stretch_has_audio
					  : (stretch_fill_ms >= required_fill_ms))) {
				break;
			}
			if (stretch_has_audio &&
			    maybe_resync_audio_buffer(ctx, stretch_peek,
						      stretch_fill_ms,
						      stretch_chunk_count,
						      true)) {
				return false;
			}

			int in_chunk_samples =
				(int)((float)base_samples * speed + 0.5f);
			size_t in_frame_bytes =
				(size_t)in_chunk_samples * ctx->audio_buf.frame_size;
			float *stretch_in = malloc(in_frame_bytes);
			if (!stretch_in)
				return false;

			int64_t in_chunk_pts_ns = 0;
			size_t got_in = audio_buffer_read_pts(
				&ctx->audio_buf, (uint8_t *)stretch_in,
				in_frame_bytes, &in_chunk_pts_ns);
			if (got_in == 0) {
				free(stretch_in);
				break;
			}

			int in_frames = (int)(got_in /
					      (out_channels * bytes_per_sample));
			uint64_t in_stream_duration_ns = 0;
			if (out_rate > 0) {
				in_stream_duration_ns =
					(uint64_t)in_frames * 1000000000ULL /
					(uint64_t)out_rate;
			}

			if (!irl_stretch_push(ctx, stretch_in, in_frames, speed,
					      in_chunk_pts_ns,
					      in_stream_duration_ns)) {
				free(stretch_in);
				return false;
			}
			free(stretch_in);
		}

		if (irl_stretch_available_frames(ctx) < base_samples)
			return false;

		size_t out_frame_bytes =
			(size_t)base_samples * ctx->audio_buf.frame_size;
		out_buf = malloc(out_frame_bytes);
		if (!out_buf)
			return false;
		if (!irl_stretch_pop(ctx, (float *)out_buf, base_samples,
				     &chunk_pts_ns,
				     &stream_duration_ns)) {
			free(out_buf);
			return false;
		}
		frames_out = (uint32_t)base_samples;
	} else {
		int chunk_samples = (int)((float)base_samples * speed + 0.5f);
		size_t frame_bytes =
			(size_t)chunk_samples * ctx->audio_buf.frame_size;
		out_buf = malloc(frame_bytes);
		if (!out_buf)
			return false;

		size_t got = audio_buffer_read_pts(&ctx->audio_buf, out_buf,
						   frame_bytes, &chunk_pts_ns);
		if (got == 0) {
			free(out_buf);
			return false;
		}

		frames_out = (uint32_t)(got /
					(out_channels * bytes_per_sample));
		if (out_rate > 0) {
			stream_duration_ns =
				(uint64_t)frames_out * 1000000000ULL /
				(uint64_t)out_rate;
		}
	}

	if (ctx->fade_in_pending) {
		ctx->fade_in_frames_remaining =
			out_rate * IRL_FADE_DURATION_MS / 1000;
		ctx->fade_in_pending = false;
	}
	if (ctx->fade_in_frames_remaining > 0) {
		int total_fade = out_rate * IRL_FADE_DURATION_MS / 1000;
		float *s = (float *)out_buf;
		int nf = (int)frames_out;
		for (int f = 0; f < nf && ctx->fade_in_frames_remaining > 0;
		     f++) {
			int into = total_fade - ctx->fade_in_frames_remaining;
			float gain = (float)into / (float)total_fade;
			for (int ch = 0; ch < out_channels; ch++)
				s[f * out_channels + ch] *= gain;
			ctx->fade_in_frames_remaining--;
		}
	}

	int ts_samples = low_latency ? (int)frames_out : base_samples;
	if (ts_samples <= 0)
		ts_samples = base_samples;

	struct obs_source_audio obs_audio = {0};
	obs_audio.data[0] = out_buf;
	obs_audio.frames = frames_out;
	obs_audio.format = AUDIO_FORMAT_FLOAT;
	obs_audio.speakers = (enum speaker_layout)out_channels;
	obs_audio.timestamp = next_audio_timestamp(ctx, ts_samples, out_rate);
	obs_audio.samples_per_sec = (uint32_t)out_rate;
	if (!use_stretch && ctx->config.adaptive_speed &&
	    (speed < 0.999f || speed > 1.001f)) {
		uint32_t scaled_rate =
			(uint32_t)((float)out_rate * speed + 0.5f);
		obs_audio.samples_per_sec =
			scaled_rate > 0 ? scaled_rate : (uint32_t)out_rate;
	}

	obs_source_output_audio(ctx->source, &obs_audio);

	finalize_audio_output(ctx, &obs_audio, chunk_pts_ns, stream_duration_ns);
	free(out_buf);
	return true;
}

/* ── Decoded frame handling ───────────────────────────────── */

static void handle_audio_frame(struct irl_source *ctx, AVFrame *frame)
{
	AVStream *as = NULL;
	if (ctx->fmt_ctx && ctx->audio_stream_idx >= 0)
		as = ctx->fmt_ctx->streams[ctx->audio_stream_idx];

	/* Determine output format: planar float → interleaved float for OBS */
	int out_channels = frame->ch_layout.nb_channels;
	int out_rate = frame->sample_rate;
	int bytes_per_sample = sizeof(float);

	/* Init or reinit audio buffer on format change */
	if (ctx->audio_buf.sample_rate != out_rate ||
	    ctx->audio_buf.channels != out_channels) {
		audio_buffer_free(&ctx->audio_buf);
		irl_stretch_reset(ctx);
		audio_buffer_init(&ctx->audio_buf, out_rate, out_channels,
				  bytes_per_sample, ctx->config.buffer_target_ms,
				  ctx->config.buffer_min_ms,
				  ctx->config.buffer_max_ms);
		ctx->audio_ts_init = false;
		ctx->audio_pll_offset_ns = 0;
		ctx->latest_audio_buffered_pts_ns = 0;
		ctx->latest_audio_buffered_end_pts_ns = 0;
		ctx->latest_audio_stream_pts_ns = 0;
		ctx->latest_audio_obs_end_ts_ns = 0;
		ctx->startup_audio_warmup_remaining_ms =
			IRL_STARTUP_AUDIO_WARMUP_MS;
	}

	/* PTS repair */
	int64_t input_pts = audio_frame_pts(frame);
	if (input_pts == AV_NOPTS_VALUE) {
		if (ctx->pts_state.initialised) {
			input_pts =
				ctx->pts_state.last_pts +
				ctx->pts_state.last_duration;
		} else {
			blog(LOG_WARNING,
			     "[irl-source] Dropping audio frame without valid PTS");
			return;
		}
	}

	int64_t duration = frame->duration;
	if (duration <= 0 && as && out_rate > 0 && frame->nb_samples > 0) {
		duration = av_rescale_q(frame->nb_samples,
					(AVRational){1, out_rate},
					as->time_base);
	}
	if (duration <= 0)
		duration = 1;

	int64_t corrected_pts;
	int silence_ms = 0;
	enum pts_action action = pts_repair_evaluate(
		&ctx->pts_state, input_pts, duration, &corrected_pts,
		&silence_ms);

	int frame_ms = audio_frame_duration_ms(frame->nb_samples, out_rate);

	if (ctx->startup_audio_warmup_remaining_ms > 0) {
		ctx->startup_audio_warmup_remaining_ms -= frame_ms;
		if (ctx->startup_audio_warmup_remaining_ms < 0)
			ctx->startup_audio_warmup_remaining_ms = 0;
		return;
	}

	if (action == PTS_ACTION_SILENCE && silence_ms > 0) {
		/* Insert silence into the buffer */
		size_t silence_bytes =
			audio_buffer_ms_to_bytes(&ctx->audio_buf, silence_ms);
		uint8_t *silence = calloc(1, silence_bytes);
		if (silence) {
			int64_t silence_pts_ns =
				(corrected_pts * 1000000000LL *
				 ctx->pts_state.tb_num /
				 ctx->pts_state.tb_den) -
				(int64_t)silence_ms * 1000000LL;
			if (silence_pts_ns < 0)
				silence_pts_ns = 0;
			audio_buffer_write_pts(&ctx->audio_buf, silence,
					       silence_bytes,
					       silence_pts_ns);
			free(silence);
			ctx->silence_insertions++;
		}
	} else if (action == PTS_ACTION_RESET) {
		audio_buffer_flush(&ctx->audio_buf);
		irl_stretch_reset(ctx);
		reset_stream_timing_state(ctx);
	}

	if (action != PTS_ACTION_PASS)
		ctx->pts_repairs++;

	/* Resample from decoded format to interleaved float if needed */
	uint8_t *interleaved = NULL;
	int out_samples = frame->nb_samples;

	if (frame->format != AV_SAMPLE_FMT_FLT) {
		/* Set up resampler */
		if (!ctx->swr_ctx || ctx->swr_in_rate != frame->sample_rate ||
		    ctx->swr_in_channels != frame->ch_layout.nb_channels ||
		    ctx->swr_in_format != frame->format) {
			if (ctx->swr_ctx)
				swr_free(&ctx->swr_ctx);
			ctx->swr_ctx = swr_alloc();
			AVChannelLayout out_layout;
			av_channel_layout_default(&out_layout, out_channels);
			swr_alloc_set_opts2(&ctx->swr_ctx, &out_layout,
					    AV_SAMPLE_FMT_FLT, out_rate,
					    &frame->ch_layout,
					    frame->format, frame->sample_rate,
					    0, NULL);
			if (swr_init(ctx->swr_ctx) < 0) {
				swr_free(&ctx->swr_ctx);
				av_channel_layout_uninit(&out_layout);
				return;
			}
			av_channel_layout_uninit(&out_layout);
			ctx->swr_in_rate = frame->sample_rate;
			ctx->swr_in_channels = frame->ch_layout.nb_channels;
			ctx->swr_in_format = frame->format;
		}

		int max_out = swr_get_out_samples(ctx->swr_ctx,
						  frame->nb_samples);
		interleaved =
			malloc((size_t)max_out * out_channels * bytes_per_sample);
		if (!interleaved)
			return;

		out_samples = swr_convert(ctx->swr_ctx, &interleaved, max_out,
					  (const uint8_t **)frame->extended_data,
					  frame->nb_samples);
		if (out_samples <= 0) {
			free(interleaved);
			return;
		}
	} else {
		/* Already interleaved float — use directly */
		interleaved = frame->data[0];
	}

	size_t data_bytes =
		(size_t)out_samples * out_channels * bytes_per_sample;

	/* Keyframe gate: discard audio until first video keyframe.
	 * Pre-keyframe audio contains decoder warm-up artifacts
	 * (AAC priming frames) that cause audible stutter if played.
	 * Instead of staging and replaying, drop it entirely.
	 * After the keyframe gate opens, audio_buffer_ready (min_ms)
	 * provides a brief fill delay for clean playback. */
	if (ctx->config.wait_for_keyframe && ctx->video_stream_idx >= 0 &&
	    !ctx->first_keyframe_received) {
		if (interleaved != frame->data[0])
			free(interleaved);
		return;
	}

	/* Write audio with its stream PTS to the PTS-aware buffer.
	 * The PTS flows through the buffer so output timestamps
	 * are exact (same approach as OBS Media Source). */
	int64_t frame_pts_ns = corrected_pts * 1000000000LL *
			       ctx->pts_state.tb_num /
			       ctx->pts_state.tb_den;
	audio_buffer_write_pts(&ctx->audio_buf, interleaved, data_bytes,
			       frame_pts_ns);
	if (interleaved != frame->data[0])
		free(interleaved);

	/* Track stream PTS for A/V sync monitoring */
	ctx->latest_audio_stream_pts_ns = frame_pts_ns;

	/* Track decoded frame size so output chunks match.  When
	 * OBS's smoothed advance (chunk_samples / sample_rate)
	 * equals our push interval (decoded_frame / sample_rate),
	 * there's zero drift → no smoothing threshold breach →
	 * push_back is always used → audio_ts is never reset. */
	ctx->decoded_frame_samples = frame->nb_samples;
}

static void handle_video_frame(struct irl_source *ctx, AVFrame *frame)
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

	/* Keyframe gate */
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

	/* Clear corruption flag on keyframe — the decoder has a
	 * fresh reference point and can produce clean output. */
	if (irl_video_is_keyframe(frame))
		ctx->video_corrupted = false;

	/* Skip frames with decode errors or from a corrupted decoder
	 * state.  OBS_SOURCE_ASYNC_VIDEO holds the last good frame
	 * automatically, so viewers see a freeze instead of
	 * black/corrupted flickering.  Matches Moblin's approach. */
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

	/* Detect mid-stream resolution changes (adaptive bitrate, phone rotation) */
	if (ctx->last_video_width && ctx->last_video_height &&
	    (frame->width != ctx->last_video_width ||
	     frame->height != ctx->last_video_height)) {
		blog(LOG_INFO,
		     "[irl-source] Resolution changed: %dx%d -> %dx%d",
		     ctx->last_video_width, ctx->last_video_height,
		     frame->width, frame->height);
		ctx->video_ts_init = false; /* re-anchor timestamps */
	}
	ctx->last_video_width = frame->width;
	ctx->last_video_height = frame->height;

	/* Track video stream PTS for A/V sync monitoring */
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

/* ── Main read loop ───────────────────────────────────────── */

void *irl_audio_thread(void *data)
{
	struct irl_source *ctx = data;

	while (ctx->thread_active) {
		if (ctx->reconnecting) {
			os_sleep_ms(2);
			continue;
		}

		bool pumped = false;
		for (int i = 0; i < 4 && ctx->thread_active; i++) {
			if (!pump_audio_once(ctx))
				break;
			pumped = true;
		}

		if (!pumped)
			os_sleep_ms(2);
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
							a.timestamp = next_audio_timestamp(
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
			audio_buffer_flush(&ctx->audio_buf);
			irl_stretch_reset(ctx);
			reset_stream_timing_state(ctx);
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
					blog(LOG_WARNING,
					     "[irl-source] Audio decoder: %d consecutive errors, flushing",
					     ctx->audio_decode_errors);
					avcodec_flush_buffers(
						ctx->audio_dec_ctx);
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
						blog(LOG_WARNING,
						     "[irl-source] Audio decoder receive: %d consecutive errors, resetting audio state",
						     ctx->audio_decode_errors);
						avcodec_flush_buffers(
							ctx->audio_dec_ctx);
						audio_buffer_flush(
							&ctx->audio_buf);
						irl_stretch_reset(ctx);
						reset_audio_timing_state(
							ctx);
						pts_repair_reset(
							&ctx->pts_state);
						if (ctx->fmt_ctx &&
						    ctx->audio_stream_idx >=
							    0) {
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
						ctx->audio_decode_errors = 0;
					}
					break;
				}
				ctx->audio_decode_errors = 0;
				handle_audio_frame(ctx, frame);
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
					blog(LOG_WARNING,
					     "[irl-source] Video decoder: %d consecutive errors, flushing",
					     ctx->video_decode_errors);
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
						blog(LOG_WARNING,
						     "[irl-source] Video decoder receive: %d consecutive errors, flushing",
						     ctx->video_decode_errors);
						avcodec_flush_buffers(
							ctx->video_dec_ctx);
						ctx->video_decode_errors = 0;
					}
					break;
				}
				ctx->video_decode_errors = 0;
				handle_video_frame(ctx, frame);
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
