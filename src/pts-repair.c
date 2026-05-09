/*
 * obs-irl-source — IRL streaming source plugin for OBS
 * https://irlserver.com
 *
 * Copyright (C) 2026 Thomas Lekanger
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * pts-repair.c — PTS discontinuity detection and repair
 *
 * Three gap ranges:
 *   small  (<  small_gap_ms):  interpolate from previous PTS
 *   medium (>= small_gap_ms, < large_gap_ms): insert silence
 *   large  (>= large_gap_ms): full timestamp reset
 */

#include <limits.h>

#include <libavutil/mathematics.h>
#include <libavutil/rational.h>

#include "../include/pts-repair.h"

#define PTS_SMALL_GAP_RELOCK_COUNT 8
#define PTS_SMALL_GAP_TOLERANCE_MS 2
#define PTS_RELOCK_STEP_MS 2

/* ── Helpers ──────────────────────────────────────────────── */

static int ts_to_ms(const struct pts_repair *r, int64_t ts)
{
	if (r->tb_den <= 0 || r->tb_num <= 0)
		return 0;
	int64_t ms = av_rescale_q(ts, (AVRational){r->tb_num, r->tb_den},
				  (AVRational){1, 1000});
	if (ms > INT_MAX)
		return INT_MAX;
	if (ms < INT_MIN)
		return INT_MIN;
	return (int)ms;
}

static int64_t ms_to_ts_ceil(const struct pts_repair *r, int ms)
{
	if (r->tb_num <= 0 || r->tb_den <= 0 || ms <= 0)
		return 1;

	int64_t ts = av_rescale_q_rnd(ms, (AVRational){1, 1000},
				      (AVRational){r->tb_num, r->tb_den},
				      AV_ROUND_UP);
	return ts > 0 ? ts : 1;
}

/* ── Public API ───────────────────────────────────────────── */

void pts_repair_init(struct pts_repair *r, int small_gap_ms, int large_gap_ms,
		     int tb_num, int tb_den)
{
	r->last_pts = 0;
	r->last_duration = 0;
	r->tb_num = tb_num;
	r->tb_den = tb_den;
	r->small_gap_ms = small_gap_ms;
	r->large_gap_ms = large_gap_ms;
	r->last_gap_ms = 0;
	r->consecutive_small_repairs = 0;
	r->relocking = false;
	r->initialised = false;
}

void pts_repair_reset(struct pts_repair *r)
{
	r->last_pts = 0;
	r->last_duration = 0;
	r->last_gap_ms = 0;
	r->consecutive_small_repairs = 0;
	r->relocking = false;
	r->initialised = false;
}

enum pts_action pts_repair_evaluate(struct pts_repair *r, int64_t pts,
				    int64_t duration,
				    int64_t *corrected_pts,
				    int *silence_ms)
{
	*silence_ms = 0;

	/* First frame — just record and pass through */
	if (!r->initialised) {
		r->last_pts = pts;
		r->last_duration = duration > 0 ? duration : 1;
		r->last_gap_ms = 0;
		r->consecutive_small_repairs = 0;
		r->relocking = false;
		r->initialised = true;
		*corrected_pts = pts;
		return PTS_ACTION_PASS;
	}

	/* Expected PTS = last_pts + last_duration */
	int64_t expected = r->last_pts + r->last_duration;
	int64_t gap = pts - expected;

	/* Convert gap to milliseconds for threshold comparison */
	int gap_ms = ts_to_ms(r, gap >= 0 ? gap : -gap);
	bool is_backward = gap < 0;

	/* Backward jump or tiny gap — likely reorder, pass through */
	if (is_backward || gap_ms < 1) {
		r->last_pts = pts;
		r->last_duration = duration > 0 ? duration : r->last_duration;
		r->last_gap_ms = 0;
		r->consecutive_small_repairs = 0;
		r->relocking = false;
		*corrected_pts = pts;
		return PTS_ACTION_PASS;
	}

	enum pts_action action;

	if (gap_ms < r->small_gap_ms) {
		int64_t relock_step_ts =
			ms_to_ts_ceil(r, PTS_RELOCK_STEP_MS);
		if (!r->relocking) {
			bool same_small_gap =
				r->consecutive_small_repairs > 0 &&
				gap_ms >=
					r->last_gap_ms -
						PTS_SMALL_GAP_TOLERANCE_MS &&
				gap_ms <=
					r->last_gap_ms +
						PTS_SMALL_GAP_TOLERANCE_MS;
			if (same_small_gap)
				r->consecutive_small_repairs++;
			else
				r->consecutive_small_repairs = 1;
			r->last_gap_ms = gap_ms;

			/* If the same small positive gap repeats for long enough,
			 * corruption likely shifted the sender timeline and the
			 * old baseline is now wrong. Enter a short relock phase
			 * and slew toward the new baseline instead of snapping. */
			if (r->consecutive_small_repairs >=
			    PTS_SMALL_GAP_RELOCK_COUNT) {
				r->relocking = true;
				r->last_gap_ms = 0;
				r->consecutive_small_repairs = 0;
			}
		}

		if (r->relocking) {
			if (gap <= relock_step_ts) {
				*corrected_pts = pts;
				r->last_pts = pts;
				r->last_duration =
					duration > 0 ? duration : r->last_duration;
				r->last_gap_ms = 0;
				r->consecutive_small_repairs = 0;
				r->relocking = false;
				return PTS_ACTION_PASS;
			}

			*corrected_pts = expected + relock_step_ts;
			action = PTS_ACTION_INTERPOLATE;
		} else {
			/* Small gap — interpolate: use expected PTS */
			*corrected_pts = expected;
			action = PTS_ACTION_INTERPOLATE;
		}
	} else if (gap_ms < r->large_gap_ms) {
		r->last_gap_ms = 0;
		r->consecutive_small_repairs = 0;
		r->relocking = false;
		/* Medium gap — insert silence, then use original PTS */
		*corrected_pts = pts;
		*silence_ms = gap_ms;
		action = PTS_ACTION_SILENCE;
	} else {
		r->last_gap_ms = 0;
		r->consecutive_small_repairs = 0;
		r->relocking = false;
		/* Large gap — full reset */
		*corrected_pts = pts;
		action = PTS_ACTION_RESET;
	}

	r->last_pts = *corrected_pts;
	r->last_duration = duration > 0 ? duration : r->last_duration;

	return action;
}
