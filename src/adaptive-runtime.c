/*
 * obs-irl-source Adaptive RIST experiment
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "adaptive-runtime.h"
#include "receiver-internal.h"

static long target_min(long target)
{
    long min_ms = target / IRL_BUFFER_MIN_DIVISOR;
    if (min_ms < IRL_BUFFER_MIN_FLOOR_MS)
        min_ms = IRL_BUFFER_MIN_FLOOR_MS;
    return min_ms;
}

static long target_max(long target)
{
    return target + IRL_BUFFER_MAX_EXTRA_MS;
}

static bool apply_target_locked(struct irl_source *ctx, long target_ms)
{
    if (target_ms < IRL_BUFFER_MIN_FLOOR_MS)
        target_ms = IRL_BUFFER_MIN_FLOOR_MS;

    const long min_ms = target_min(target_ms);
    const long max_ms = target_max(target_ms);
    const long current = os_atomic_load_long(&ctx->config.buffer_target_ms);
    if (current == target_ms)
        return false;

    if (!audio_buffer_resize(&ctx->audio_buf, (int)target_ms,
                             (int)min_ms, (int)max_ms)) {
        blog(LOG_WARNING,
             "[irl-source] Adaptive RIST could not resize playout buffer to %ldms; keeping %ldms",
             target_ms, current);
        return false;
    }

    os_atomic_set_long(&ctx->config.buffer_target_ms, target_ms);
    os_atomic_set_long(&ctx->config.buffer_min_ms, min_ms);
    os_atomic_set_long(&ctx->config.buffer_max_ms, max_ms);
    return true;
}

void irl_adaptive_runtime_set_user_target(struct irl_source *ctx,
                                          long target_ms)
{
    if (!ctx)
        return;
    os_atomic_set_long(&ctx->rist_user_buffer_target_ms, target_ms);
}

bool irl_adaptive_runtime_apply(struct irl_source *ctx,
                                const irl_adaptive_output_t *policy,
                                const irl_rist_sample_t *sample)
{
    if (!ctx || !policy || !sample)
        return false;

    long user_target = os_atomic_load_long(&ctx->rist_user_buffer_target_ms);
    long effective_target = (long)policy->playout_target_ms;
    if (effective_target < user_target)
        effective_target = user_target;

    irl_mutex_lock(&ctx->audio_state_lock);
    bool changed = apply_target_locked(ctx, effective_target);

    /* Publish a coherent telemetry snapshot for get_stats / websocket. */
    ctx->rist_stats_valid = true;
    ctx->rist_net_state = (int)policy->state;
    ctx->rist_rtt_ms = sample->rtt_ms;
    ctx->rist_missing = sample->missing;
    ctx->rist_recovered = sample->recovered;
    ctx->rist_lost = sample->lost;
    ctx->rist_reordered = sample->reordered;
    ctx->rist_quality_pct = policy->quality_pct;
    ctx->rist_retry_pct = policy->interval_retry_pct;
    ctx->rist_transport_buffer_ms = policy->transport_buffer_ms;
    ctx->rist_effective_playout_ms = effective_target;
    ctx->rist_recovery_recommended_min_ms = policy->recovery_min_ms;
    ctx->rist_recovery_recommended_max_ms = policy->recovery_max_ms;
    ctx->rist_risk_score = policy->risk_score;
    irl_mutex_unlock(&ctx->audio_state_lock);

    if (changed) {
        blog(LOG_INFO,
             "[irl-source] Adaptive RIST: state=%s rtt=%ums target=%ldms transport_buf=%llums loss=%.2f%% missing=%.2f%% retry=%.2f%%",
             irl_net_state_name(policy->state), sample->rtt_ms,
             effective_target,
             (unsigned long long)policy->transport_buffer_ms,
             policy->interval_loss_pct, policy->interval_missing_pct,
             policy->interval_retry_pct);
    }
    return changed;
}

void irl_adaptive_runtime_restore_user_target(struct irl_source *ctx)
{
    if (!ctx)
        return;

    long user_target = os_atomic_load_long(&ctx->rist_user_buffer_target_ms);
    irl_mutex_lock(&ctx->audio_state_lock);
    (void)apply_target_locked(ctx, user_target);
    ctx->rist_stats_valid = false;
    ctx->rist_net_state = IRL_NET_COLD;
    ctx->rist_effective_playout_ms = user_target;
    irl_mutex_unlock(&ctx->audio_state_lock);
}
