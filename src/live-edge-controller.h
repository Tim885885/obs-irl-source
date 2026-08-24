/*
 * obs-irl-source Adaptive RIST experiment
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef IRL_LIVE_EDGE_CONTROLLER_H
#define IRL_LIVE_EDGE_CONTROLLER_H

#include <stdbool.h>
#include <stdint.h>

#include "network-controller.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum irl_live_edge_mode {
    IRL_LIVE_EDGE_OFF = 0,
    IRL_LIVE_EDGE_BALANCED = 1,
    IRL_LIVE_EDGE_AGGRESSIVE = 2,
} irl_live_edge_mode_t;

typedef enum irl_live_edge_action {
    IRL_LIVE_EDGE_ACTION_NONE = 0,
    /* Drop decoded video frames that are already too late to be useful. */
    IRL_LIVE_EDGE_ACTION_SOFT_VIDEO_DROP,
    /* Trim old buffered audio and drop video before the new audio head PTS. */
    IRL_LIVE_EDGE_ACTION_HARD_AV_RECLAIM,
} irl_live_edge_action_t;

typedef struct irl_live_edge_input {
    uint64_t timestamp_ms;
    irl_live_edge_mode_t mode;

    /* Transport-derived policy from network-controller.c. */
    irl_net_state_t net_state;
    bool allow_video_drop;
    bool allow_hard_live_edge_jump;
    uint32_t hard_live_edge_threshold_ms;

    /* Playout state. audio_fill_ms is plugin-side buffered content, not total
     * network latency. This distinction prevents us from repeatedly trimming
     * content that is merely sitting inside a fixed RIST recovery window. */
    bool audio_primed;
    bool low_latency_audio;
    int audio_fill_ms;
    int audio_target_ms;
    int audio_chunk_ms;

    /* Video-thread observation. Positive means the oldest decoded pacing
     * frame is already late by this much. */
    int video_head_late_ms;
    int video_pacing_count;

    /* Diagnostics only. Never used by itself to trigger a destructive trim. */
    int64_t stream_delay_ms;
    uint64_t transport_buffer_ms;

    /* Rate limiting state supplied by the integration layer. */
    uint64_t last_hard_reclaim_ms;
} irl_live_edge_input_t;

typedef struct irl_live_edge_decision {
    irl_live_edge_action_t action;

    /* For HARD_AV_RECLAIM: keep approximately this much audio. */
    uint32_t audio_keep_ms;

    /* For SOFT_VIDEO_DROP: discard pacing frames older than this lateness,
     * but always keep at least one frame to avoid blanking the source. */
    uint32_t video_late_drop_ms;

    uint32_t estimated_reclaim_ms;
    uint32_t hard_threshold_ms;
    uint32_t hard_cooldown_ms;
    uint32_t reason_flags;
} irl_live_edge_decision_t;

enum {
    IRL_LIVE_EDGE_REASON_VIDEO_LATE = 1u << 0,
    IRL_LIVE_EDGE_REASON_AUDIO_BACKLOG = 1u << 1,
    IRL_LIVE_EDGE_REASON_CRITICAL_NETWORK = 1u << 2,
    IRL_LIVE_EDGE_REASON_COOLDOWN = 1u << 3,
};

bool irl_live_edge_decide(const irl_live_edge_input_t *in,
                          irl_live_edge_decision_t *out);

const char *irl_live_edge_action_name(irl_live_edge_action_t action);

/* Shared by the real OBS video thread and unit tests. Hard boundaries may
 * drop the last stale pacing frame; soft lateness reclaim always keeps at
 * least one queued frame. */
bool irl_live_edge_video_frame_should_drop(bool hard_pending,
                                           int64_t min_pts_ns,
                                           int64_t frame_pts_ns,
                                           bool allow_soft,
                                           uint32_t soft_late_ms,
                                           int pacing_count,
                                           uint64_t frame_due_ns,
                                           uint64_t now_ns);

/* Evaluate a hard-reclaim acknowledgement from the current protected request,
 * not from an earlier snapshot. This prevents the video thread from clearing
 * a newer boundary published while it was dropping frames. */
bool irl_live_edge_video_reclaim_is_complete(bool hard_pending,
                                             int64_t min_pts_ns,
                                             int pacing_count,
                                             int64_t head_pts_ns);

#ifdef __cplusplus
}
#endif

#endif
