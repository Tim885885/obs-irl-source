/*
 * obs-irl-source Adaptive RIST experiment
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "live-edge-controller.h"

#include <string.h>

static uint32_t u32max(uint32_t a, uint32_t b)
{
    return a > b ? a : b;
}

static uint32_t u32clamp(uint32_t v, uint32_t lo, uint32_t hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

bool irl_live_edge_decide(const irl_live_edge_input_t *in,
                          irl_live_edge_decision_t *out)
{
    if (!in || !out)
        return false;

    memset(out, 0, sizeof(*out));
    out->action = IRL_LIVE_EDGE_ACTION_NONE;

    if (in->mode == IRL_LIVE_EDGE_OFF)
        return true;

    const bool aggressive = in->mode == IRL_LIVE_EDGE_AGGRESSIVE;
    const uint32_t soft_late_ms = aggressive ? 60u : 120u;
    const uint32_t hard_cooldown_ms = aggressive ? 2500u : 5000u;

    uint32_t hard_threshold = in->hard_live_edge_threshold_ms;
    if (hard_threshold == 0)
        hard_threshold = 1200;
    if (aggressive) {
        /* Aggressive mode intentionally trades continuity for interaction
         * latency, but retains a floor so a single ordinary jitter burst does
         * not become an audible cut. */
        hard_threshold = (hard_threshold * 2u) / 3u;
        hard_threshold = u32max(hard_threshold, 650u);
    }
    out->hard_threshold_ms = hard_threshold;
    out->hard_cooldown_ms = hard_cooldown_ms;
    out->video_late_drop_ms = soft_late_ms;

    const int target_ms = in->audio_target_ms > 0 ? in->audio_target_ms : 120;
    const int fill_excess_ms = in->audio_fill_ms > target_ms
        ? in->audio_fill_ms - target_ms : 0;

    /* Hard reclaim is deliberately based on plugin-side queued audio, not
     * stream_delay_ms. A fixed 1800ms RIST recovery window can make total
     * stream delay large while there is nothing in the playout ring to trim. */
    const bool hard_backlog = in->audio_primed && !in->low_latency_audio &&
        in->allow_hard_live_edge_jump &&
        fill_excess_ms >= (int)hard_threshold;

    if (hard_backlog) {
        out->reason_flags |= IRL_LIVE_EDGE_REASON_AUDIO_BACKLOG;
        if (in->net_state == IRL_NET_CRITICAL)
            out->reason_flags |= IRL_LIVE_EDGE_REASON_CRITICAL_NETWORK;

        const bool cooled_down = in->last_hard_reclaim_ms == 0 ||
            in->timestamp_ms >= in->last_hard_reclaim_ms + hard_cooldown_ms;
        if (cooled_down) {
            int chunk_ms = in->audio_chunk_ms > 0 ? in->audio_chunk_ms : 21;
            /* Keep target plus two decoded chunks. The extra chunk avoids
             * trimming straight into an underrun while the receiver thread is
             * still emerging from the bad interval. */
            uint32_t keep_ms = (uint32_t)(target_ms + chunk_ms * 2);
            keep_ms = u32clamp(keep_ms, (uint32_t)target_ms, 1000u);
            out->audio_keep_ms = keep_ms;
            out->estimated_reclaim_ms = in->audio_fill_ms > (int)keep_ms
                ? (uint32_t)(in->audio_fill_ms - (int)keep_ms) : 0;
            out->action = IRL_LIVE_EDGE_ACTION_HARD_AV_RECLAIM;
            return true;
        }
        out->reason_flags |= IRL_LIVE_EDGE_REASON_COOLDOWN;
    }

    /* Soft video dropping is safe at the decoder level because these frames
     * have already been decoded: throwing away an output frame does not break
     * future reference pictures. It only avoids presenting a burst of stale
     * pictures after the video output thread was delayed. */
    if (in->allow_video_drop && in->video_pacing_count >= 2 &&
        in->video_head_late_ms >= (int)soft_late_ms) {
        out->reason_flags |= IRL_LIVE_EDGE_REASON_VIDEO_LATE;
        out->estimated_reclaim_ms = (uint32_t)in->video_head_late_ms;
        out->action = IRL_LIVE_EDGE_ACTION_SOFT_VIDEO_DROP;
        return true;
    }

    return true;
}

const char *irl_live_edge_action_name(irl_live_edge_action_t action)
{
    switch (action) {
    case IRL_LIVE_EDGE_ACTION_SOFT_VIDEO_DROP:
        return "SOFT_VIDEO_DROP";
    case IRL_LIVE_EDGE_ACTION_HARD_AV_RECLAIM:
        return "HARD_AV_RECLAIM";
    case IRL_LIVE_EDGE_ACTION_NONE:
    default:
        return "NONE";
    }
}

bool irl_live_edge_video_frame_should_drop(bool hard_pending,
                                           int64_t min_pts_ns,
                                           int64_t frame_pts_ns,
                                           bool allow_soft,
                                           uint32_t soft_late_ms,
                                           int pacing_count,
                                           uint64_t frame_due_ns,
                                           uint64_t now_ns)
{
    if (hard_pending && frame_pts_ns < min_pts_ns)
        return true;

    if (!allow_soft || soft_late_ms == 0 || pacing_count < 2 ||
        frame_due_ns == 0 || now_ns <= frame_due_ns)
        return false;

    return now_ns - frame_due_ns >=
        (uint64_t)soft_late_ms * 1000000ULL;
}

bool irl_live_edge_video_reclaim_is_complete(bool hard_pending,
                                             int64_t min_pts_ns,
                                             int pacing_count,
                                             int64_t head_pts_ns)
{
    return hard_pending &&
        (pacing_count == 0 || head_pts_ns >= min_pts_ns);
}
