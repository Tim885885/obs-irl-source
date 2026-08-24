/*
 * obs-irl-source Adaptive RIST experiment
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "live-edge-runtime.h"
#include "live-edge-controller.h"
#include "receiver-internal.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#define IRL_LIVE_EDGE_AV_TOLERANCE_NS 100000000LL

static bool ascii_equal_ci(const char *a, const char *b)
{
    if (!a || !b)
        return false;
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
            return false;
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

static irl_live_edge_mode_t mode_from_environment(void)
{
    const char *v = getenv("IRL_ADAPTIVE_LIVE_EDGE");
    if (!v || !*v || ascii_equal_ci(v, "off") || !strcmp(v, "0") ||
        ascii_equal_ci(v, "false"))
        return IRL_LIVE_EDGE_OFF;
    if (ascii_equal_ci(v, "aggressive") || !strcmp(v, "2"))
        return IRL_LIVE_EDGE_AGGRESSIVE;
    if (ascii_equal_ci(v, "balanced") || ascii_equal_ci(v, "on") ||
        ascii_equal_ci(v, "true") || !strcmp(v, "1"))
        return IRL_LIVE_EDGE_BALANCED;
    return IRL_LIVE_EDGE_OFF;
}

void irl_live_edge_runtime_init(struct irl_source *ctx)
{
    if (!ctx)
        return;

    const irl_live_edge_mode_t mode = mode_from_environment();
    ctx->rist_live_edge_mode = (int)mode;
    ctx->rist_live_edge_last_hard_ms = 0;
    ctx->rist_live_edge_hard_reclaims = 0;
    ctx->rist_live_edge_audio_trimmed_chunks = 0;
    ctx->rist_live_edge_video_drops = 0;

    irl_mutex_lock(&ctx->video_queue_lock);
    ctx->rist_live_edge_allow_video_drop = false;
    ctx->rist_live_edge_video_late_drop_ms = 0;
    ctx->rist_live_edge_video_reclaim_pending = false;
    ctx->rist_live_edge_min_video_pts_ns = 0;
    irl_mutex_unlock(&ctx->video_queue_lock);

    blog(LOG_INFO, "[irl-source] Adaptive RIST live-edge mode: %s%s",
         mode == IRL_LIVE_EDGE_AGGRESSIVE ? "aggressive" :
         mode == IRL_LIVE_EDGE_BALANCED ? "balanced" : "off",
         mode == IRL_LIVE_EDGE_OFF ? " (set IRL_ADAPTIVE_LIVE_EDGE=balanced to opt in)" : "");
}

void irl_live_edge_runtime_reset_session(struct irl_source *ctx)
{
    if (!ctx)
        return;

    ctx->rist_live_edge_last_hard_ms = 0;
    irl_mutex_lock(&ctx->video_queue_lock);
    ctx->rist_live_edge_allow_video_drop = false;
    ctx->rist_live_edge_video_late_drop_ms = 0;
    ctx->rist_live_edge_video_reclaim_pending = false;
    ctx->rist_live_edge_min_video_pts_ns = 0;
    irl_mutex_unlock(&ctx->video_queue_lock);
}

static int audio_chunk_ms(const struct irl_source *ctx)
{
    const int rate = ctx->audio_buf.sample_rate;
    if (rate > 0 && ctx->decoded_frame_samples > 0) {
        int ms = (int)((int64_t)ctx->decoded_frame_samples * 1000LL / rate);
        if (ms > 0)
            return ms;
    }
    return 21;
}

static void publish_video_policy(struct irl_source *ctx,
                                 irl_live_edge_mode_t mode,
                                 const irl_adaptive_output_t *policy,
                                 uint32_t late_drop_ms,
                                 bool hard_reclaim,
                                 int64_t new_audio_head_pts_ns)
{
    irl_mutex_lock(&ctx->video_queue_lock);
    ctx->rist_live_edge_allow_video_drop =
        mode != IRL_LIVE_EDGE_OFF && policy->allow_video_drop;
    ctx->rist_live_edge_video_late_drop_ms = late_drop_ms;

    if (hard_reclaim && new_audio_head_pts_ns > 0) {
        int64_t min_video_pts = new_audio_head_pts_ns - IRL_LIVE_EDGE_AV_TOLERANCE_NS;
        if (min_video_pts < 0)
            min_video_pts = 0;
        /* Monotonic within one connection: an older concurrent request must
         * never pull the reclaim boundary backwards. */
        if (!ctx->rist_live_edge_video_reclaim_pending ||
            min_video_pts > ctx->rist_live_edge_min_video_pts_ns)
            ctx->rist_live_edge_min_video_pts_ns = min_video_pts;
        ctx->rist_live_edge_video_reclaim_pending = true;
    }
    irl_mutex_unlock(&ctx->video_queue_lock);
}

bool irl_live_edge_runtime_update(struct irl_source *ctx,
                                  const irl_adaptive_output_t *policy)
{
    if (!ctx || !policy)
        return false;

    const irl_live_edge_mode_t mode =
        (irl_live_edge_mode_t)ctx->rist_live_edge_mode;
    uint32_t soft_late_ms = mode == IRL_LIVE_EDGE_AGGRESSIVE ? 60u : 120u;

    if (mode == IRL_LIVE_EDGE_OFF) {
        publish_video_policy(ctx, mode, policy, 0, false, 0);
        return false;
    }

    const uint64_t now_ms = os_gettime_ns() / 1000000ULL;
    bool reclaimed = false;
    int64_t new_audio_head_pts_ns = 0;
    int trimmed = 0;
    int post_fill_ms = 0;

    irl_mutex_lock(&ctx->audio_state_lock);
    const int fill_ms = audio_buffer_fill_ms_locked(&ctx->audio_buf);
    const int target_ms = (int)os_atomic_load_long(&ctx->config.buffer_target_ms);
    const int chunk_ms = audio_chunk_ms(ctx);

    /* Avoid designated initializers in code compiled by MSVC as C. */
    irl_live_edge_input_t in;
    memset(&in, 0, sizeof(in));
    in.timestamp_ms = now_ms;
    in.mode = mode;
    in.net_state = policy->state;
    in.allow_video_drop = policy->allow_video_drop;
    in.allow_hard_live_edge_jump = policy->allow_hard_live_edge_jump;
    in.hard_live_edge_threshold_ms = policy->hard_live_edge_threshold_ms;
    in.audio_primed = ctx->audio_out_primed;
    in.low_latency_audio = ctx->config.low_latency_audio;
    in.audio_fill_ms = fill_ms;
    in.audio_target_ms = target_ms;
    in.audio_chunk_ms = chunk_ms;
    in.transport_buffer_ms = policy->transport_buffer_ms;
    in.last_hard_reclaim_ms = ctx->rist_live_edge_last_hard_ms;
    irl_live_edge_decision_t decision;
    (void)irl_live_edge_decide(&in, &decision);
    soft_late_ms = decision.video_late_drop_ms;

    if (decision.action == IRL_LIVE_EDGE_ACTION_HARD_AV_RECLAIM) {
        int post_chunks = 0;
        trimmed = audio_buffer_trim_to_keep_ms(
            &ctx->audio_buf, (int)decision.audio_keep_ms, 1,
            &post_fill_ms, &post_chunks);
        if (trimmed > 0) {
            (void)post_chunks;
            (void)audio_buffer_peek_state(&ctx->audio_buf,
                                          &new_audio_head_pts_ns,
                                          NULL, NULL);
            ctx->audio_resync_skipped_chunks += (uint64_t)trimmed;
            ctx->audio_quality_events++;
            ctx->rist_live_edge_hard_reclaims++;
            ctx->rist_live_edge_audio_trimmed_chunks += (uint64_t)trimmed;
            ctx->rist_live_edge_last_hard_ms = now_ms;
            reclaimed = true;
        }
    }
    irl_mutex_unlock(&ctx->audio_state_lock);

    publish_video_policy(ctx, mode, policy, soft_late_ms, reclaimed,
                         new_audio_head_pts_ns);

    if (reclaimed) {
        blog(LOG_WARNING,
             "[irl-source] Adaptive RIST live-edge reclaim: dropped %d buffered audio chunk%s (fill=%dms -> %dms, keep=%ums); video will skip to audio head",
             trimmed, trimmed == 1 ? "" : "s", fill_ms, post_fill_ms,
             decision.audio_keep_ms);
    }
    return reclaimed;
}
