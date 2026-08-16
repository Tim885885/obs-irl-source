/*
 * obs-irl-source — IRL streaming source plugin for OBS
 * https://irlserver.com
 *
 * Copyright (C) 2026 Thomas Lekanger
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * audio-buffer.c — PTS-aware jitter buffer
 *
 * Ring buffer for PCM with a parallel PTS chunk queue.  Each
 * write records the stream PTS; each read returns the PTS of
 * the oldest data.  Enables exact A/V sync with stream-PTS-based
 * timestamps (same approach as OBS Media Source).
 */

#include <stdlib.h>
#include <string.h>

#include <util/base.h>
#include <util/bmem.h>

#include "../include/audio-buffer.h"

/* ── Helpers ──────────────────────────────────────────────── */

static size_t ms_to_bytes(const struct audio_buffer *buf, int ms)
{
	if (buf->frame_size == 0 || buf->sample_rate == 0)
		return 0;
	return (size_t)((int64_t)ms * buf->sample_rate / 1000) *
	       buf->frame_size;
}

static int fill_ms_unlocked(const struct audio_buffer *buf)
{
	if (buf->frame_size == 0 || buf->sample_rate == 0)
		return 0;
	size_t samples = buf->fill / buf->frame_size;
	return (int)(samples * 1000 / buf->sample_rate);
}

static size_t capacity_bytes_unlocked(const struct audio_buffer *buf)
{
	return buf->capacity;
}

/* Write raw bytes to the ring buffer (no PTS tracking). */
static size_t ring_write(struct audio_buffer *buf, const uint8_t *samples,
			 size_t bytes)
{
	size_t avail = buf->capacity - buf->fill;
	size_t to_write = bytes < avail ? bytes : avail;

	if (to_write == 0)
		return 0;

	size_t first_chunk = buf->capacity - buf->head;
	if (first_chunk >= to_write) {
		memcpy(buf->data + buf->head, samples, to_write);
	} else {
		memcpy(buf->data + buf->head, samples, first_chunk);
		memcpy(buf->data, samples + first_chunk,
		       to_write - first_chunk);
	}

	buf->head = (buf->head + to_write) % buf->capacity;
	buf->fill += to_write;
	return to_write;
}

/* Read raw bytes from the ring buffer (no PTS tracking). */
static size_t ring_read(struct audio_buffer *buf, uint8_t *out,
			size_t max_bytes)
{
	size_t to_read = max_bytes < buf->fill ? max_bytes : buf->fill;

	if (to_read == 0)
		return 0;

	size_t first_chunk = buf->capacity - buf->tail;
	if (first_chunk >= to_read) {
		memcpy(out, buf->data + buf->tail, to_read);
	} else {
		memcpy(out, buf->data + buf->tail, first_chunk);
		memcpy(out + first_chunk, buf->data,
		       to_read - first_chunk);
	}

	buf->tail = (buf->tail + to_read) % buf->capacity;
	buf->fill -= to_read;
	return to_read;
}

static void skip_oldest_chunk_locked(struct audio_buffer *buf)
{
	if (!buf->data || buf->chunk_count <= 0)
		return;

	struct audio_pts_chunk *c = &buf->chunks[buf->chunk_tail];
	size_t remaining = c->size - c->consumed;

	if (remaining > 0 && remaining <= buf->fill) {
		buf->tail = (buf->tail + remaining) % buf->capacity;
		buf->fill -= remaining;
	}

	buf->chunk_tail = (buf->chunk_tail + 1) % AUDIO_PTS_MAX_CHUNKS;
	buf->chunk_count--;
}

static size_t clamp_incoming_to_capacity_locked(struct audio_buffer *buf,
						const uint8_t **samples,
						size_t bytes,
						int64_t *pts_ns)
{
	size_t capacity = capacity_bytes_unlocked(buf);
	if (capacity == 0)
		return bytes;

	/* If a single decoded chunk is larger than the allocated storage,
	 * keep only the newest tail so buffered mode does not start
	 * several hundred milliseconds behind by construction. */
	if (bytes > capacity) {
		size_t skip_bytes = bytes - capacity;
		if (pts_ns && buf->sample_rate > 0 && buf->frame_size > 0) {
			int64_t skipped_frames =
				(int64_t)(skip_bytes / buf->frame_size);
			*pts_ns += skipped_frames * 1000000000LL /
				   buf->sample_rate;
		}
		*samples += skip_bytes;
		bytes = capacity;
	}

	return bytes;
}

/* Retire PTS chunks as data is consumed. */
static void pts_consume(struct audio_buffer *buf, size_t bytes_consumed)
{
	size_t remaining = bytes_consumed;
	while (remaining > 0 && buf->chunk_count > 0) {
		struct audio_pts_chunk *c =
			&buf->chunks[buf->chunk_tail];
		size_t avail = c->size - c->consumed;

		if (remaining >= avail) {
			/* Fully consumed this chunk */
			remaining -= avail;
			buf->chunk_tail =
				(buf->chunk_tail + 1) %
				AUDIO_PTS_MAX_CHUNKS;
			buf->chunk_count--;
		} else {
			/* Partially consumed */
			c->consumed += remaining;
			remaining = 0;
		}
	}
}

/* ── Public API ───────────────────────────────────────────── */

bool audio_buffer_init(struct audio_buffer *buf, int sample_rate, int channels,
		       int bytes_per_sample, int target_ms, int min_ms,
		       int max_ms)
{
	memset(buf, 0, sizeof(*buf));

	/* Initialise the lock before publishing buf->data: stats readers
	 * (e.g. proc_handler) gate locking on buf->data != NULL, so the
	 * mutex must already be live when data becomes visible.
	 *
	 * It also has to happen before sample_rate is set, which is what
	 * audio_buffer_free() reads as its "init ran" marker: leaving the
	 * struct zeroed here keeps free() from destroying a mutex that was
	 * never created. */
	if (irl_mutex_init(&buf->lock) != 0) {
		blog(LOG_ERROR,
		     "[irl-source] Failed to create audio buffer lock");
		return false;
	}

	buf->sample_rate = sample_rate;
	buf->channels = channels;
	buf->bytes_per_sample = bytes_per_sample;
	buf->frame_size = bytes_per_sample * channels;
	buf->target_ms = target_ms;
	buf->min_ms = min_ms;
	buf->max_ms = max_ms;

	/* Allocate enough headroom that Max Buffer is not an audible hard
	 * trim point. Old audio is dropped only by explicit recovery paths. */
	buf->capacity = ms_to_bytes(buf, max_ms * 4);
	if (buf->capacity == 0)
		buf->capacity = 65536; /* fallback */
	buf->data = bzalloc(buf->capacity);
	if (!buf->data)
		buf->capacity = 0;
	return true;
}

bool audio_buffer_reconfigure(struct audio_buffer *buf, int sample_rate,
			      int channels, int bytes_per_sample,
			      int target_ms, int min_ms, int max_ms)
{
	if (!buf)
		return false;

	struct audio_buffer next = {0};
	next.sample_rate = sample_rate;
	next.channels = channels;
	next.bytes_per_sample = bytes_per_sample;
	next.frame_size = bytes_per_sample * channels;
	next.target_ms = target_ms;
	next.min_ms = min_ms;
	next.max_ms = max_ms;
	next.capacity = ms_to_bytes(&next, max_ms * 4);
	if (next.capacity == 0)
		next.capacity = 65536;
	next.data = bzalloc(next.capacity);
	if (!next.data)
		return false;

	irl_mutex_lock(&buf->lock);
	bfree(buf->data);
	buf->data = next.data;
	buf->capacity = next.capacity;
	buf->head = 0;
	buf->tail = 0;
	buf->fill = 0;
	buf->chunk_head = 0;
	buf->chunk_tail = 0;
	buf->chunk_count = 0;
	buf->sample_rate = next.sample_rate;
	buf->channels = next.channels;
	buf->bytes_per_sample = next.bytes_per_sample;
	buf->frame_size = next.frame_size;
	buf->target_ms = next.target_ms;
	buf->min_ms = next.min_ms;
	buf->max_ms = next.max_ms;
	irl_mutex_unlock(&buf->lock);
	return true;
}

bool audio_buffer_resize(struct audio_buffer *buf, int target_ms, int min_ms,
			 int max_ms)
{
	if (!buf)
		return false;
	/* Not initialised yet: the first decoded frame sizes the ring from
	 * the config values, so there is nothing to move. */
	if (!buf->data)
		return true;

	/* Storage only ever grows.  Shrinking would mean knowing the fill
	 * before allocating, which forces either an allocation with the ring
	 * locked or discarding queued audio that does not fit (forbidden
	 * once playback has primed).  The ring is pure headroom above the
	 * watermarks, so leaving it oversized until the next reconnect costs
	 * nothing but memory.
	 *
	 * The allocation is done before the ring lock so the audio pump is
	 * never blocked on the allocator.  The caller's serialisation is
	 * what keeps this from racing a format-change reconfigure. */
	size_t wanted = ms_to_bytes(buf, max_ms * 4);
	uint8_t *grown = NULL;

	if (wanted > buf->capacity) {
		grown = bzalloc(wanted);
		if (!grown)
			return false;
	}

	irl_mutex_lock(&buf->lock);
	if (grown) {
		/* Linearise into the new allocation.  Chunk metadata is
		 * relative (size/consumed, no ring offsets), so the PTS
		 * queue survives the move untouched. */
		size_t first = buf->capacity - buf->tail;
		if (first >= buf->fill) {
			memcpy(grown, buf->data + buf->tail, buf->fill);
		} else {
			memcpy(grown, buf->data + buf->tail, first);
			memcpy(grown + first, buf->data, buf->fill - first);
		}
		bfree(buf->data);
		buf->data = grown;
		buf->capacity = wanted;
		buf->tail = 0;
		buf->head = buf->fill % buf->capacity;
	}
	buf->target_ms = target_ms;
	buf->min_ms = min_ms;
	buf->max_ms = max_ms;
	irl_mutex_unlock(&buf->lock);
	return true;
}

void audio_buffer_free(struct audio_buffer *buf)
{
	/* sample_rate is the canonical "init was called" marker. capacity
	 * may legitimately be 0 if bzalloc failed, but the mutex is still
	 * live and must be destroyed. */
	if (buf->sample_rate == 0)
		return;

	irl_mutex_destroy(&buf->lock);
	bfree(buf->data);
	memset(buf, 0, sizeof(*buf));
}

void audio_buffer_flush(struct audio_buffer *buf)
{
	if (!buf->data)
		return;

	irl_mutex_lock(&buf->lock);
	buf->head = 0;
	buf->tail = 0;
	buf->fill = 0;
	buf->chunk_head = 0;
	buf->chunk_tail = 0;
	buf->chunk_count = 0;
	irl_mutex_unlock(&buf->lock);
}

size_t audio_buffer_write_pts(struct audio_buffer *buf, const uint8_t *samples,
			      size_t bytes, int64_t pts_ns)
{
	if (!buf->data || bytes == 0)
		return 0;

	irl_mutex_lock(&buf->lock);

	while (buf->chunk_count >= AUDIO_PTS_MAX_CHUNKS)
		skip_oldest_chunk_locked(buf);

	bytes = clamp_incoming_to_capacity_locked(buf, &samples, bytes,
						  &pts_ns);
	if (bytes == 0) {
		irl_mutex_unlock(&buf->lock);
		return 0;
	}

	size_t written = ring_write(buf, samples, bytes);

	/* Record PTS chunk metadata for every successful write. */
	if (written > 0) {
		struct audio_pts_chunk *c =
			&buf->chunks[buf->chunk_head];
		c->pts_ns = pts_ns;
		c->size = written;
		c->consumed = 0;
		buf->chunk_head =
			(buf->chunk_head + 1) % AUDIO_PTS_MAX_CHUNKS;
		buf->chunk_count++;
	}

	irl_mutex_unlock(&buf->lock);
	return written;
}

size_t audio_buffer_write(struct audio_buffer *buf, const uint8_t *samples,
			  size_t bytes)
{
	if (!buf->data || bytes == 0)
		return 0;

	irl_mutex_lock(&buf->lock);

	/* Continuation marker: derive the new chunk's PTS from the prior
	 * chunk's end so reads stay PTS-consistent. Without this, downstream
	 * read_pts would return 0 for these bytes while pts_consume drained
	 * unrelated chunk metadata, slowly poisoning the PTS queue. */
	int64_t pts_ns = 0;
	if (buf->chunk_count > 0 && buf->sample_rate > 0 &&
	    buf->frame_size > 0) {
		int last_idx = (buf->chunk_head - 1 + AUDIO_PTS_MAX_CHUNKS) %
			       AUDIO_PTS_MAX_CHUNKS;
		const struct audio_pts_chunk *last = &buf->chunks[last_idx];
		int64_t samples_in_chunk =
			(int64_t)last->size / buf->frame_size;
		pts_ns = last->pts_ns + samples_in_chunk * 1000000000LL /
						 buf->sample_rate;
	}

	while (buf->chunk_count >= AUDIO_PTS_MAX_CHUNKS)
		skip_oldest_chunk_locked(buf);

	bytes = clamp_incoming_to_capacity_locked(buf, &samples, bytes,
						  &pts_ns);
	if (bytes == 0) {
		irl_mutex_unlock(&buf->lock);
		return 0;
	}

	size_t written = ring_write(buf, samples, bytes);
	if (written > 0) {
		struct audio_pts_chunk *c = &buf->chunks[buf->chunk_head];
		c->pts_ns = pts_ns;
		c->size = written;
		c->consumed = 0;
		buf->chunk_head =
			(buf->chunk_head + 1) % AUDIO_PTS_MAX_CHUNKS;
		buf->chunk_count++;
	}
	irl_mutex_unlock(&buf->lock);
	return written;
}

size_t audio_buffer_read_pts(struct audio_buffer *buf, uint8_t *out,
			     size_t max_bytes, int64_t *out_pts_ns)
{
	if (!buf->data || max_bytes == 0)
		return 0;

	irl_mutex_lock(&buf->lock);

	/* Get PTS of the oldest data before reading */
	if (out_pts_ns) {
		if (buf->chunk_count > 0) {
			struct audio_pts_chunk *c =
				&buf->chunks[buf->chunk_tail];
			/* Interpolate PTS based on how much of this
			 * chunk has already been consumed. */
			if (buf->sample_rate > 0 && buf->frame_size > 0) {
				int64_t consumed_samples =
					(int64_t)c->consumed /
					buf->frame_size;
				*out_pts_ns =
					c->pts_ns +
					consumed_samples * 1000000000LL /
						buf->sample_rate;
			} else {
				*out_pts_ns = c->pts_ns;
			}
		} else {
			*out_pts_ns = 0;
		}
	}

	size_t got = ring_read(buf, out, max_bytes);
	if (got > 0)
		pts_consume(buf, got);

	irl_mutex_unlock(&buf->lock);
	return got;
}

size_t audio_buffer_read(struct audio_buffer *buf, uint8_t *out,
			 size_t max_bytes)
{
	if (!buf->data || max_bytes == 0)
		return 0;

	irl_mutex_lock(&buf->lock);
	size_t got = ring_read(buf, out, max_bytes);
	if (got > 0)
		pts_consume(buf, got);
	irl_mutex_unlock(&buf->lock);
	return got;
}

int64_t audio_buffer_peek_pts(const struct audio_buffer *buf)
{
	if (buf->chunk_count <= 0)
		return 0;

	const struct audio_pts_chunk *c =
		&buf->chunks[buf->chunk_tail];
	if (buf->sample_rate > 0 && buf->frame_size > 0) {
		int64_t consumed_samples =
			(int64_t)c->consumed / buf->frame_size;
		return c->pts_ns +
		       consumed_samples * 1000000000LL / buf->sample_rate;
	}
	return c->pts_ns;
}

int audio_buffer_fill_ms_locked(struct audio_buffer *buf)
{
	if (!buf->data)
		return 0;

	irl_mutex_lock(&buf->lock);
	int fill_ms = fill_ms_unlocked(buf);
	irl_mutex_unlock(&buf->lock);
	return fill_ms;
}

bool audio_buffer_ready_locked(struct audio_buffer *buf)
{
	if (!buf->data)
		return false;

	irl_mutex_lock(&buf->lock);
	bool ready = fill_ms_unlocked(buf) >= buf->min_ms;
	irl_mutex_unlock(&buf->lock);
	return ready;
}

bool audio_buffer_peek_state(struct audio_buffer *buf, int64_t *out_pts_ns,
			     int *out_fill_ms, int *out_chunk_count)
{
	if (!buf->data)
		return false;

	irl_mutex_lock(&buf->lock);

	if (out_fill_ms)
		*out_fill_ms = fill_ms_unlocked(buf);
	if (out_chunk_count)
		*out_chunk_count = buf->chunk_count;

	bool has_pts = buf->chunk_count > 0;
	if (out_pts_ns) {
		if (has_pts) {
			const struct audio_pts_chunk *c =
				&buf->chunks[buf->chunk_tail];
			if (buf->sample_rate > 0 && buf->frame_size > 0) {
				int64_t consumed_samples =
					(int64_t)c->consumed / buf->frame_size;
				*out_pts_ns = c->pts_ns +
					      consumed_samples * 1000000000LL /
						      buf->sample_rate;
			} else {
				*out_pts_ns = c->pts_ns;
			}
		} else {
			*out_pts_ns = 0;
		}
	}

	irl_mutex_unlock(&buf->lock);
	return has_pts;
}

void audio_buffer_skip_chunk(struct audio_buffer *buf)
{
	if (!buf->data || buf->chunk_count <= 0)
		return;

	irl_mutex_lock(&buf->lock);
	skip_oldest_chunk_locked(buf);
	irl_mutex_unlock(&buf->lock);
}

int audio_buffer_trim_to_keep_ms(struct audio_buffer *buf, int keep_ms,
				 int min_chunks_to_keep, int *out_fill_ms,
				 int *out_chunk_count)
{
	if (!buf->data) {
		if (out_fill_ms)
			*out_fill_ms = 0;
		if (out_chunk_count)
			*out_chunk_count = 0;
		return 0;
	}

	irl_mutex_lock(&buf->lock);

	int trimmed = 0;
	while (buf->chunk_count > min_chunks_to_keep &&
	       fill_ms_unlocked(buf) > keep_ms) {
		skip_oldest_chunk_locked(buf);
		trimmed++;
	}

	if (out_fill_ms)
		*out_fill_ms = fill_ms_unlocked(buf);
	if (out_chunk_count)
		*out_chunk_count = buf->chunk_count;

	irl_mutex_unlock(&buf->lock);
	return trimmed;
}

int audio_buffer_skip_until_pts(struct audio_buffer *buf, int64_t min_pts_ns)
{
	if (!buf->data)
		return 0;

	irl_mutex_lock(&buf->lock);

	int skipped = 0;
	while (buf->chunk_count > 0) {
		const struct audio_pts_chunk *c =
			&buf->chunks[buf->chunk_tail];
		int64_t pts_ns = c->pts_ns;
		if (buf->sample_rate > 0 && buf->frame_size > 0) {
			int64_t consumed_samples =
				(int64_t)c->consumed / buf->frame_size;
			pts_ns += consumed_samples * 1000000000LL /
				  buf->sample_rate;
		}

		if (pts_ns >= min_pts_ns)
			break;

		skip_oldest_chunk_locked(buf);
		skipped++;
	}

	irl_mutex_unlock(&buf->lock);
	return skipped;
}

int audio_buffer_fill_ms(const struct audio_buffer *buf)
{
	return fill_ms_unlocked(buf);
}

bool audio_buffer_ready(const struct audio_buffer *buf)
{
	return audio_buffer_fill_ms(buf) >= buf->min_ms;
}

size_t audio_buffer_read_with_fade_out(struct audio_buffer *buf, uint8_t *out,
				       size_t max_bytes)
{
	size_t got = audio_buffer_read(buf, out, max_bytes);
	if (got == 0 || buf->frame_size == 0)
		return got;

	/* Apply linear gain ramp 1.0 → 0.0 over the entire read */
	int total_frames = (int)(got / buf->frame_size);
	if (total_frames <= 0)
		return got;

	float *samples = (float *)out;
	for (int f = 0; f < total_frames; f++) {
		float gain = 1.0f - (float)f / (float)total_frames;
		for (int ch = 0; ch < buf->channels; ch++)
			samples[f * buf->channels + ch] *= gain;
	}

	return got;
}

size_t audio_buffer_ms_to_bytes(const struct audio_buffer *buf, int ms)
{
	return ms_to_bytes(buf, ms);
}
