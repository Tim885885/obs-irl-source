/*
 * obs-irl-source: IRL streaming source plugin for OBS
 * https://irlserver.com
 *
 * Copyright (C) 2026 Thomas Lekanger
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Codec/protocol/bitrate-agnostic live source with jitter buffering,
 * PTS repair, adaptive playback speed, and first-keyframe gating.
 */

#pragma once

#ifndef OBS_IRL_SOURCE_VERSION
#define OBS_IRL_SOURCE_VERSION "0.3.0"
#endif

#include <obs-module.h>
#include <util/platform.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
#include <libavutil/time.h>
#include <libavutil/hwcontext.h>
#include <libavutil/audio_fifo.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>

#include "audio-buffer.h"
#include "pts-repair.h"

#define IRL_STRETCH_META_MAX 256

struct irl_stretch_meta_entry {
	int64_t pts_ns;
	uint64_t duration_ns;
	int out_frames;
	int consumed_frames;
};

/* ── Forward declarations ─────────────────────────────────── */

struct irl_source;

/* ── Configuration defaults ───────────────────────────────── */

#define IRL_DEFAULT_RECONNECT_DELAY 2
#define IRL_DEFAULT_NETWORK_BUFFER_MB 2
#define IRL_DEFAULT_BUFFER_TARGET_MS 120
#define IRL_DEFAULT_BUFFER_MIN_MS 60
#define IRL_DEFAULT_BUFFER_MAX_MS 300
#define IRL_DEFAULT_ADAPTIVE_SPEED true
#define IRL_DEFAULT_SPEED_MIN 0.95f
#define IRL_DEFAULT_SPEED_MAX 1.05f
#define IRL_DEFAULT_SMALL_GAP_MS 70
#define IRL_DEFAULT_LARGE_GAP_MS 2000
#define IRL_DEFAULT_HW_DECODE 0 /* 0 = auto, 1 = off */
#define IRL_DEFAULT_WAIT_KEYFRAME true
#define IRL_DEFAULT_LOW_LATENCY_AUDIO false
#define IRL_DEFAULT_DECOUPLED_AUDIO false
#define IRL_DEFAULT_CLOSE_WHEN_INACTIVE false

/* Audio fade duration on disconnect/reconnect (avoids clicks/pops) */
#define IRL_FADE_DURATION_MS 50

/* Decode/resample/PTS-repair audio in the background on startup,
 * but discard a short window before sending anything to OBS. This
 * avoids AAC/decoder warm-up artifacts without adding steady-state delay. */
#define IRL_STARTUP_AUDIO_WARMUP_MS 150

/* ── Source configuration ─────────────────────────────────── */

struct irl_config {
	/* General */
	char *url;
	int reconnect_delay;
	int network_buffer_mb;

	/* Audio buffer */
	int buffer_target_ms;
	int buffer_min_ms;
	int buffer_max_ms;
	bool adaptive_speed;
	float speed_min;
	float speed_max;

	/* PTS repair */
	int small_gap_ms;
	int large_gap_ms;

	/* Advanced */
	char *ffmpeg_options;
	int hw_decode;
	bool wait_for_keyframe;
	bool low_latency_audio;
	bool decoupled_audio;
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
	int audio_stream_idx;
	int video_stream_idx;
	bool using_hw_decode;

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

	/* Video timestamp sync (anchors stream PTS to system clock) */
	bool video_ts_init;
	uint64_t video_sys_base;  /* os_gettime_ns() at first frame */
	int64_t video_pts_base;   /* stream PTS at first frame (in ns) */

	/* Audio timestamp sync (same approach as video — stream PTS
	 * anchored to system clock, giving proper A/V sync).
	 * PTS comes from the PTS-aware jitter buffer, not estimated. */
	bool audio_ts_init;
	uint64_t audio_sys_base;  /* os_gettime_ns() at first audio output */
	int64_t audio_pts_base;   /* stream PTS at first audio output (ns) */

	/* Gentle PLL: Moblin-style ±1 frame correction when the
	 * computed audio PTS drifts >30ms from wall clock.  Each
	 * correction is ~21ms (one frame), well within OBS's 70ms
	 * smoothing window — absorbed safely, no cascade. */
	int64_t audio_pll_offset_ns;

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

	/* PTS repair state */
	struct pts_repair pts_state;

	/* Adaptive speed controller */
	float current_speed;
	uint64_t last_speed_adjust_time;
	uint64_t audio_pll_corrections;
	uint64_t audio_pll_hard_resets;
	uint64_t audio_underruns;
	uint64_t audio_resync_skipped_chunks;
	int64_t audio_last_ts_drift_ns;
	int64_t audio_last_obs_lead_ns;
	uint64_t audio_last_chunk_stream_duration_ns;
	uint64_t audio_last_chunk_obs_duration_ns;
	uint32_t audio_last_frames_out;
	uint32_t audio_last_samples_per_sec;
	uint64_t last_audio_diag_time;

	/* Pitch-preserving time stretch (buffered adaptive-speed mode) */
	AVFilterGraph *stretch_graph;
	AVFilterContext *stretch_src_ctx;
	AVFilterContext *stretch_tempo_ctx;
	AVFilterContext *stretch_sink_ctx;
	AVAudioFifo *stretch_fifo;
	AVChannelLayout stretch_layout;
	int stretch_sample_rate;
	int stretch_channels;
	float stretch_speed;
	struct irl_stretch_meta_entry
		stretch_meta[IRL_STRETCH_META_MAX];
	int stretch_meta_head;
	int stretch_meta_tail;
	int stretch_meta_count;
	int64_t stretch_next_pts_ns;
	bool stretch_next_pts_valid;
	uint64_t stretch_last_active_time_us;
	uint64_t stretch_last_retune_time_us;

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

	/* Video corruption tracking.  Set when send_packet fails
	 * (HW decoders may not set decode_error_flags reliably).
	 * Cleared on next keyframe. */
	bool video_corrupted;
	bool video_skip_logged;

	/* Keyframe gate */
	bool first_keyframe_received;

	/* Audio fade state */
	bool fade_in_pending;
	int fade_in_frames_remaining;
	int startup_audio_warmup_remaining_ms;

	/* Resolution tracking (for mid-stream changes) */
	int last_video_width;
	int last_video_height;

	/* Statistics */
	uint64_t total_audio_frames;
	uint64_t total_video_frames;
	uint64_t pts_repairs;
	uint64_t silence_insertions;
	uint64_t reconnect_count;
	int64_t latest_audio_buffered_pts_ns;
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

/* ── Adaptive speed (audio-speed.c) ───────────────────────── */

float irl_speed_get(struct irl_source *ctx);

/* ── Pitch-Preserving Time Stretch (audio-stretch.c) ─────── */

void irl_stretch_reset(struct irl_source *ctx);
bool irl_stretch_configure(struct irl_source *ctx, int sample_rate,
			   int channels);
bool irl_stretch_push(struct irl_source *ctx, const float *samples, int frames,
		      float speed, int64_t pts_ns, uint64_t duration_ns);
bool irl_stretch_pop(struct irl_source *ctx, float *out, int out_frames,
		     int64_t *pts_ns, uint64_t *duration_ns);
int irl_stretch_available_frames(struct irl_source *ctx);

/* ── Video handler (video-handler.c) ──────────────────────── */

void irl_video_output_frame(struct irl_source *ctx, AVFrame *frame);
bool irl_video_is_keyframe(const AVFrame *frame);

/* ── PTS repair (pts-repair.c) ────────────────────────────── */
/* See pts-repair.h */
