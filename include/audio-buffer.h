/*
 * obs-irl-source — IRL streaming source plugin for OBS
 * https://irlserver.com
 *
 * Copyright (C) 2026 Thomas Lekanger
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * audio-buffer.h — PTS-aware jitter buffer
 *
 * Ring buffer for PCM storage with a parallel queue of PTS-tagged
 * chunk metadata.  Each write records the stream PTS of the data;
 * each read returns the PTS of the oldest data.  This gives exact
 * A/V sync (same approach as OBS Media Source — stream PTS anchored
 * to wall clock).
 *
 * Inspired by Moblin's Deque<CMSampleBuffer>, adapted for C with
 * a fixed-size ring buffer instead of per-chunk allocation.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <util/threading.h> /* OBS cross-platform pthread wrappers */

/* ── PTS chunk metadata ──────────────────────────────────── */

/* Max chunks in the PTS queue.  At 21ms per AAC frame with a
 * 300ms max buffer, that's ~14 chunks.  256 gives ample headroom
 * for burst decode and speed controller variations. */
#define AUDIO_PTS_MAX_CHUNKS 256

struct audio_pts_chunk {
	int64_t pts_ns;  /* stream PTS in nanoseconds */
	size_t size;     /* bytes of PCM data for this chunk */
	size_t consumed; /* bytes already read from this chunk */
};

/* ── Audio buffer ─────────────────────────────────────────── */

struct audio_buffer {
	uint8_t *data;       /* ring-buffer storage */
	size_t capacity;     /* total capacity in bytes */
	size_t head;         /* write position */
	size_t tail;         /* read position */
	size_t fill;         /* current fill in bytes */

	/* PTS chunk queue: tracks what PTS corresponds to each
	 * segment of data in the ring buffer. */
	struct audio_pts_chunk chunks[AUDIO_PTS_MAX_CHUNKS];
	int chunk_head; /* next write slot */
	int chunk_tail; /* next read slot */
	int chunk_count;

	/* Stream format (set once per session) */
	int sample_rate;
	int channels;
	int bytes_per_sample; /* bytes per single sample (e.g. 4 for float32) */
	int frame_size;       /* bytes_per_sample * channels */

	/* Configuration (milliseconds) */
	int target_ms;
	int min_ms;
	int max_ms;

	pthread_mutex_t lock;
};

/* ── API ──────────────────────────────────────────────────── */

/**
 * Initialise the buffer.  Allocates storage with headroom above `max_ms` so
 * the configured max is not an audible old-audio drop point.
 * Call after the first decoded audio frame reveals the stream parameters.
 */
void audio_buffer_init(struct audio_buffer *buf, int sample_rate, int channels,
		       int bytes_per_sample, int target_ms, int min_ms,
		       int max_ms);

/**
 * Reconfigure buffer storage/format without destroying the mutex.
 * Safe to use while other threads may still reference the same
 * audio_buffer object, as long as the caller serialises higher-level
 * access around the transition.
 */
bool audio_buffer_reconfigure(struct audio_buffer *buf, int sample_rate,
			      int channels, int bytes_per_sample,
			      int target_ms, int min_ms, int max_ms);

/**
 * Move the watermarks while the stream is live, keeping every queued
 * sample.  Storage grows if the new max needs more room and is left
 * alone otherwise.  Same serialisation requirement as
 * audio_buffer_reconfigure().  Returns false only on allocation failure,
 * in which case the buffer is untouched.
 */
bool audio_buffer_resize(struct audio_buffer *buf, int target_ms, int min_ms,
			 int max_ms);

/** Release all resources. */
void audio_buffer_free(struct audio_buffer *buf);

/** Reset buffer to empty without freeing (e.g. on reconnect). */
void audio_buffer_flush(struct audio_buffer *buf);

/**
 * Write decoded PCM samples with their stream PTS.
 * Returns the number of bytes actually written (may be less if buffer is full).
 */
size_t audio_buffer_write_pts(struct audio_buffer *buf, const uint8_t *samples,
			      size_t bytes, int64_t pts_ns);

/**
 * Write decoded PCM samples (no PTS tracking).
 * Used for silence insertion and legacy paths.
 */
size_t audio_buffer_write(struct audio_buffer *buf, const uint8_t *samples,
			  size_t bytes);

/**
 * Read up to `max_bytes` of PCM from the buffer, returning the PTS
 * of the oldest data read via `out_pts_ns`.
 * Returns the number of bytes read.
 */
size_t audio_buffer_read_pts(struct audio_buffer *buf, uint8_t *out,
			     size_t max_bytes, int64_t *out_pts_ns);

/**
 * Read up to `max_bytes` of PCM from the buffer (no PTS).
 * Returns the number of bytes read.
 */
size_t audio_buffer_read(struct audio_buffer *buf, uint8_t *out,
			 size_t max_bytes);

/**
 * Peek at the PTS of the oldest chunk without consuming it.
 * Returns 0 if the chunk queue is empty.
 */
int64_t audio_buffer_peek_pts(const struct audio_buffer *buf);

/**
 * Thread-safe variant of audio_buffer_fill_ms().
 */
int audio_buffer_fill_ms_locked(struct audio_buffer *buf);

/**
 * Thread-safe variant of audio_buffer_ready().
 */
bool audio_buffer_ready_locked(struct audio_buffer *buf);

/**
 * Snapshot the oldest chunk PTS/fill/chunk count atomically.
 * Returns true if a PTS-bearing chunk exists.
 */
bool audio_buffer_peek_state(struct audio_buffer *buf, int64_t *out_pts_ns,
			     int *out_fill_ms, int *out_chunk_count);

/**
 * Discard the oldest chunk from the buffer (advance tail past it).
 * Used by re-sync mode to skip stale data after PTS discontinuities.
 */
void audio_buffer_skip_chunk(struct audio_buffer *buf);

/**
 * Drop stale PTS chunks older than min_pts_ns.
 * Returns the number of chunks skipped.
 */
int audio_buffer_skip_until_pts(struct audio_buffer *buf, int64_t min_pts_ns);

/**
 * Drop oldest chunks under a single lock until fill <= keep_ms or
 * the chunk count reaches `min_chunks_to_keep`.  Out-params report
 * the post-trim state.  Used by the hidden-backlog trim path so we
 * don't reacquire the lock per dropped chunk.
 */
int audio_buffer_trim_to_keep_ms(struct audio_buffer *buf, int keep_ms,
				 int min_chunks_to_keep, int *out_fill_ms,
				 int *out_chunk_count);

/** Current fill level in milliseconds. */
int audio_buffer_fill_ms(const struct audio_buffer *buf);

/** True when the buffer has at least `min_ms` worth of data. */
bool audio_buffer_ready(const struct audio_buffer *buf);

/**
 * Read up to `max_bytes` and apply a linear fade-out (1.0 → 0.0).
 * Used on disconnect to avoid audio clicks/pops.
 * Assumes float sample format.
 */
size_t audio_buffer_read_with_fade_out(struct audio_buffer *buf, uint8_t *out,
				       size_t max_bytes);

/**
 * Calculate bytes needed for a given duration at current format.
 */
size_t audio_buffer_ms_to_bytes(const struct audio_buffer *buf, int ms);
