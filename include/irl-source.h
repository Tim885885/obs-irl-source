/*
 * obs-irl-source: IRL streaming source plugin for OBS
 * https://irlserver.com
 *
 * Copyright (C) 2026 Thomas Lekanger
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Codec/protocol/bitrate-agnostic live source with jitter buffering,
 * PTS repair, adaptive latency control, and first-keyframe gating.
 */

#pragma once

#ifndef OBS_IRL_SOURCE_VERSION
#define OBS_IRL_SOURCE_VERSION "0.4.0"
#endif

#include <obs-module.h>
#include <util/platform.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
#include <libavutil/time.h>
#include <libavutil/hwcontext.h>
#include "audio-buffer.h"
#include "pts-repair.h"

/* ── Forward declarations ─────────────────────────────────── */

struct irl_source;

/* ── Configuration defaults ───────────────────────────────── */

#define IRL_DEFAULT_RECONNECT_DELAY 2
#define IRL_DEFAULT_NETWORK_BUFFER_MB 2
#define IRL_DEFAULT_BUFFER_TARGET_MS 120
#define IRL_DEFAULT_ADAPTIVE_SPEED true
#define IRL_DEFAULT_HW_DECODE 0 /* 0 = auto, 1 = off */
#define IRL_DEFAULT_WAIT_KEYFRAME true
#define IRL_DEFAULT_LOW_LATENCY_AUDIO false
#define IRL_DEFAULT_CLOSE_WHEN_INACTIVE false

/* Min/max buffer are derived from the target rather than exposed as
 * settings: min is the speed controller's low watermark, max is where
 * drain speed peaks (and sizes the ring at 4x). Keeps users from
 * creating broken configurations like min > target. */
#define IRL_BUFFER_MIN_DIVISOR 2
#define IRL_BUFFER_MIN_FLOOR_MS 20
#define IRL_BUFFER_MAX_EXTRA_MS 200

/* PTS repair thresholds (formerly settings; nobody could reason
 * about them without reading the source, and the defaults are
 * principled: below small_gap is decoder timestamp wobble, above
 * large_gap the stream fundamentally changed). */
#define IRL_SMALL_GAP_MS 70
#define IRL_LARGE_GAP_MS 2000

/* Audio fade duration on disconnect/reconnect (avoids clicks/pops) */
#define IRL_FADE_DURATION_MS 50

/* Decode/resample/PTS-repair audio in the background on startup,
 * but discard a short window before sending anything to OBS. This
 * avoids AAC/decoder warm-up artifacts without adding steady-state delay. */
#define IRL_STARTUP_AUDIO_WARMUP_MS 150

/* Full-bleed backlog policy: once the local jitter buffer holds this
 * much audio, the receiver stops reading and lets the transport hold
 * the rest (TCP/RTMP backpressure; SRT bounds its own backlog via the
 * latency window). Playback bleeds the excess at up to +5% speed, so
 * nothing audible is ever skipped. Must stay well under the ring
 * buffer capacity (4x buffer_max_ms) or writes would drop old data. */
#define IRL_BLEED_PACE_FILL_MS 1000

/* ── Source configuration ─────────────────────────────────── */

struct irl_config {
	/* General */
	char *url;
	int reconnect_delay;
	int network_buffer_mb;

	/* Audio buffer. Only target_ms is a user setting; min/max are
	 * derived from it in config_load(). */
	int buffer_target_ms;
	int buffer_min_ms;
	int buffer_max_ms;
	bool adaptive_speed;

	/* PTS repair (constants, kept here so pts_repair_init has one
	 * source of truth) */
	int small_gap_ms;
	int large_gap_ms;

	/* Advanced */
	char *ffmpeg_options;
	int hw_decode;
	bool wait_for_keyframe;
	bool low_latency_audio;
	bool close_when_inactive;
};

/* ── Main source context ──────────────────────────────────── */

struct irl_source {
	obs_source_t *source;
	struct irl_config config;

	/* Receiver / demux thread */
	pthread_t receiver_thread;
	pthread_t audio_thread;
	pthread_mutex_t audio_state_lock;
	volatile bool thread_active;
	volatile bool reconnecting;

	/* FFmpeg state (owned by receiver thread) */
	AVFormatContext *fmt_ctx;
	AVCodecContext *audio_dec_ctx;
	AVCodecContext *video_dec_ctx;
	AVBufferRef *hw_device_ctx;
	enum AVHWDeviceType hw_device_type;
	int audio_stream_idx;
	int video_stream_idx;
	bool using_hw_decode;
	/* Tri-state: -1 = not yet attempted, 0 = map fails, falling back
	 * to transfer_data, 1 = map succeeded at least once. Used to
	 * skip the doomed map attempt on the second frame onwards once
	 * we've learned the platform can't map. */
	int hw_map_ok;

	/* Resampler (planar → interleaved float) */
	SwrContext *swr_ctx;
	int swr_in_rate;
	int swr_in_channels;
	enum AVSampleFormat swr_in_format;

	/* Video scaler (for format conversion to OBS) */
	struct SwsContext *sws_ctx;
	int sws_src_w;
	int sws_src_h;
	enum AVPixelFormat sws_src_fmt;
	uint8_t *sws_nv12_buf;       /* receiver-thread-owned NV12 scratch */
	size_t sws_nv12_buf_capacity;

	/* Video timestamp sync (anchors stream PTS to system clock) */
	bool video_ts_init;
	uint64_t video_sys_base;  /* os_gettime_ns() at first frame */
	int64_t video_pts_base;   /* stream PTS at first frame (in ns) */

	/* Audio output clock.  OBS timestamps are a pure sample
	 * counter anchored once at prime time:
	 *   ts = anchor + samples/rate
	 * Contiguous by construction, so OBS's timestamp smoothing
	 * always takes the seamless-append path.  The wall clock is
	 * only consulted for pacing and stall detection. */
	bool audio_out_primed;
	uint64_t audio_out_anchor_ns;
	uint64_t audio_out_samples;
	uint64_t audio_output_restarts;

	/* Output-side speed resampler (audio thread).  Playback speed
	 * is applied here via swr compensation because changing the
	 * samples_per_sec submitted to OBS forces libobs to rebuild
	 * its per-source resampler with no crossfade (audible click
	 * per change). */
	SwrContext *speed_swr;
	int speed_swr_rate;
	int speed_swr_channels;
	uint8_t *audio_speed_scratch;      /* audio thread */
	size_t audio_speed_scratch_capacity;

	/* Dropout concealment state (audio thread) */
	float audio_out_last_sample[8];
	int audio_out_last_channels;
	bool audio_out_last_valid;
	bool audio_conceal_fade_pending;

	/* Stream PTS tracking for A/V sync and re-sync mode */
	int64_t latest_audio_stream_pts_ns;
	int64_t latest_video_stream_pts_ns;

	/* Latest audio already queued to OBS, in OBS clock domain.
	 * Used to align video to actual audio playout instead of
	 * approximating from the plugin-side jitter-buffer fill. */
	uint64_t latest_audio_obs_end_ts_ns;
	int64_t latest_audio_buffered_end_pts_ns;

	/* Audio jitter buffer */
	struct audio_buffer audio_buf;

	/* Per-thread scratch buffers (no lock needed; each is owned by
	 * exactly one thread).  Grown on demand to avoid per-frame
	 * malloc, which is a real latency source on lossy IRL streams
	 * where decode/output bursts coincide with allocator pressure. */
	uint8_t *audio_pump_scratch;       /* audio thread */
	size_t audio_pump_scratch_capacity;
	uint8_t *audio_resample_scratch;   /* receiver thread */
	size_t audio_resample_scratch_capacity;

	/* PTS repair state */
	struct pts_repair pts_state;

	/* Buffered audio correction state */
	float current_speed;
	uint64_t audio_underruns;
	uint64_t audio_resync_skipped_chunks;
	uint64_t audio_hidden_trimmed_chunks;
	uint64_t audio_quality_events;
	int64_t audio_last_obs_lead_ns;
	uint64_t audio_last_chunk_stream_duration_ns;
	uint64_t audio_last_chunk_obs_duration_ns;
	uint32_t audio_last_frames_out;
	uint32_t audio_last_samples_per_sec;
	uint64_t audio_recovery_until_us;

	/* Decoded frame size (samples per frame).  Used as the output
	 * chunk size so OBS's smoothing advance matches our push rate.
	 * AAC = 1024, Opus = 960.  If mismatched, smoothing drifts
	 * and periodically resets audio_ts → "audio is lagging". */
	int decoded_frame_samples;

	/* Consecutive decode error counters.  Only flush the decoder
	 * after 3+ consecutive errors — a single corrupt packet should
	 * not reset the decoder state (losing reference frames). */
	int audio_decode_errors;
	int video_decode_errors;
	uint64_t audio_decoder_flushes;
	uint64_t video_decoder_flushes;
	uint64_t audio_last_decoder_flush_time_us;
	uint64_t video_last_decoder_flush_time_us;
	uint64_t audio_last_decoder_warning_time_us;
	uint64_t video_last_decoder_warning_time_us;

	/* Video corruption tracking.  Set when send_packet fails
	 * (HW decoders may not set decode_error_flags reliably).
	 * Cleared on next keyframe. */
	bool video_corrupted;
	bool video_skip_logged;

	/* Keyframe gate.  Packet-level: don't feed the decoder at all
	 * until a key packet arrives (avoids reference-miss error spam
	 * and decoder churn on join).  Frame-level backstop:
	 * first_keyframe_received gates decoded output. */
	bool first_keyframe_received;
	bool video_pkt_gate_open;
	uint64_t video_pkt_gate_start_us;

	/* Audio fade state */
	bool fade_in_pending;
	int fade_in_frames_remaining;
	int startup_audio_warmup_remaining_ms;
	float audio_last_sample[8];
	int audio_last_sample_channels;
	bool audio_last_sample_valid;

	/* Resolution tracking (for mid-stream changes) */
	int last_video_width;
	int last_video_height;

	/* Statistics */
	uint64_t total_audio_frames;
	uint64_t total_video_frames;
	uint64_t pts_repairs;
	uint64_t pts_normalizations;
	uint64_t pts_interpolations;
	uint64_t pts_resets;
	int pts_last_gap_ms;
	int pts_max_gap_ms;
	uint64_t silence_insertions;
	uint64_t reconnect_count;
	uint64_t last_stats_time;
};

/* ── Lifecycle (irl-source.c) ─────────────────────────────── */

void *irl_source_create(obs_data_t *settings, obs_source_t *source);
void irl_source_destroy(void *data);
void irl_source_update(void *data, obs_data_t *settings);
void irl_source_activate(void *data);
void irl_source_deactivate(void *data);
void irl_source_show(void *data);
void irl_source_hide(void *data);
void irl_source_tick(void *data, float seconds);
const char *irl_source_get_name(void *unused);

/* ── Settings (settings.c) ────────────────────────────────── */

obs_properties_t *irl_source_get_properties(void *data);
void irl_source_get_defaults(obs_data_t *settings);

/* ── Receiver thread (receiver.c) ─────────────────────────── */

void *irl_receiver_thread(void *data);
void *irl_audio_thread(void *data);
void irl_receiver_stop(struct irl_source *ctx);

/* ── Audio buffer (audio-buffer.c) ────────────────────────── */
/* See audio-buffer.h */

/* ── Video handler (video-handler.c) ──────────────────────── */

void irl_video_output_frame(struct irl_source *ctx, AVFrame *frame);
bool irl_video_is_keyframe(const AVFrame *frame);

/* ── PTS repair (pts-repair.c) ────────────────────────────── */
/* See pts-repair.h */
