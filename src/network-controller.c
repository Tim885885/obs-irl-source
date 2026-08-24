/*
 * obs-irl-source Adaptive RIST experiment
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "network-controller.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

#define EWMA_ALPHA_FAST 0.30
#define EWMA_ALPHA_SLOW 0.18
#define RELEASE_DWELL_MS 5000ULL

static uint32_t clampu32(uint32_t v, uint32_t lo, uint32_t hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static double clampd(double v, double lo, double hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static double ewma(double old_value, double new_value, double alpha)
{
    if (old_value <= 0.0)
        return new_value;
    return alpha * new_value + (1.0 - alpha) * old_value;
}

static irl_net_state_t classify(double score)
{
    if (score >= 7.0)
        return IRL_NET_CRITICAL;
    if (score >= 4.5)
        return IRL_NET_BAD;
    if (score >= 2.0)
        return IRL_NET_WARN;
    return IRL_NET_GOOD;
}

static double rtt_risk(double rtt_ms)
{
    if (rtt_ms < 100.0) return 0.0;
    if (rtt_ms < 160.0) return 0.8;
    if (rtt_ms < 250.0) return 1.7;
    if (rtt_ms < 400.0) return 2.7;
    if (rtt_ms < 650.0) return 3.6;
    return 4.4;
}

static double jitter_risk(double jitter_ms)
{
    if (jitter_ms < 15.0) return 0.0;
    if (jitter_ms < 40.0) return 0.5;
    if (jitter_ms < 90.0) return 1.2;
    if (jitter_ms < 180.0) return 2.0;
    return 2.8;
}

static double loss_risk(double loss_pct)
{
    if (loss_pct < 0.10) return 0.0;
    if (loss_pct < 0.50) return 0.6;
    if (loss_pct < 1.50) return 1.3;
    if (loss_pct < 4.00) return 2.2;
    return 3.2;
}

static double missing_risk(double missing_pct)
{
    if (missing_pct < 0.20) return 0.0;
    if (missing_pct < 0.75) return 0.4;
    if (missing_pct < 2.00) return 0.9;
    if (missing_pct < 5.00) return 1.6;
    return 2.3;
}

static double retry_risk(double retry_pct)
{
    if (retry_pct < 1.0) return 0.0;
    if (retry_pct < 4.0) return 0.3;
    if (retry_pct < 10.0) return 0.7;
    if (retry_pct < 20.0) return 1.2;
    return 1.8;
}

static double spacing_risk(uint64_t spacing_us)
{
    const double ms = (double)spacing_us / 1000.0;
    if (ms < 80.0) return 0.0;
    if (ms < 160.0) return 0.4;
    if (ms < 300.0) return 0.9;
    if (ms < 600.0) return 1.6;
    return 2.2;
}

static void fill_policy(irl_net_state_t state,
                        double rtt_ms,
                        double jitter_ms,
                        irl_adaptive_output_t *out)
{
    /* Recommendation only on stock libRIST: recovery_length_{min,max} are peer
     * configuration, not a public hot-control API. A wide min/max still lets
     * libRIST use its dynamic receiver buffer internally. */
    uint32_t rec_min = (uint32_t)lround(2.5 * rtt_ms + 1.0 * jitter_ms + 40.0);
    uint32_t rec_max = (uint32_t)lround(5.5 * rtt_ms + 2.0 * jitter_ms + 80.0);
    rec_min = clampu32(rec_min, 200, 900);
    rec_max = clampu32(rec_max, 450, 1800);
    if (rec_max < rec_min + 200)
        rec_max = clampu32(rec_min + 200, 450, 1800);

    out->recovery_min_ms = rec_min;
    out->recovery_max_ms = rec_max;
    out->allow_video_drop = false;
    out->allow_hard_live_edge_jump = false;
    out->hard_live_edge_threshold_ms = 1500;

    switch (state) {
    case IRL_NET_GOOD:
        out->playout_target_ms = 120;
        out->catchup_speed = 1.02;
        out->hard_live_edge_threshold_ms = 1000;
        break;
    case IRL_NET_WARN:
        out->playout_target_ms = 200;
        out->catchup_speed = 1.035;
        out->allow_video_drop = true;
        out->hard_live_edge_threshold_ms = 1200;
        break;
    case IRL_NET_BAD:
        out->playout_target_ms = 350;
        out->catchup_speed = 1.05;
        out->allow_video_drop = true;
        out->hard_live_edge_threshold_ms = 1500;
        break;
    case IRL_NET_CRITICAL:
        out->playout_target_ms = 600;
        out->catchup_speed = 1.05;
        out->allow_video_drop = true;
        out->allow_hard_live_edge_jump = true;
        out->hard_live_edge_threshold_ms = 1700;
        break;
    case IRL_NET_COLD:
    default:
        out->playout_target_ms = 120;
        out->catchup_speed = 1.00;
        break;
    }
}

void irl_network_controller_init(irl_network_controller_t *ctl)
{
    if (!ctl)
        return;
    memset(ctl, 0, sizeof(*ctl));
    ctl->state = IRL_NET_COLD;
}

bool irl_network_controller_update(irl_network_controller_t *ctl,
                                   const irl_rist_sample_t *sample,
                                   irl_adaptive_output_t *out)
{
    if (!ctl || !sample || !out)
        return false;

    memset(out, 0, sizeof(*out));

    const double packet_opportunities =
        (double)sample->received + (double)sample->missing;
    const double loss_pct = packet_opportunities > 0.0
        ? 100.0 * (double)sample->lost / packet_opportunities : 0.0;
    const double missing_pct = packet_opportunities > 0.0
        ? 100.0 * (double)sample->missing / packet_opportunities : 0.0;
    const double recovery_pct = sample->missing > 0
        ? clampd(100.0 * (double)sample->recovered / (double)sample->missing,
                 0.0, 100.0)
        : 100.0;
    const double total_bw =
        (double)sample->bandwidth_bps + (double)sample->retry_bandwidth_bps;
    const double retry_pct = total_bw > 0.0
        ? 100.0 * (double)sample->retry_bandwidth_bps / total_bw : 0.0;

    if (!ctl->initialized) {
        ctl->initialized = true;
        ctl->ewma_rtt_ms = sample->rtt_ms;
        ctl->ewma_abs_rtt_delta_ms = 0.0;
        ctl->ewma_loss_pct = loss_pct;
        ctl->ewma_missing_pct = missing_pct;
        ctl->ewma_retry_pct = retry_pct;
    } else {
        const double previous_rtt = ctl->ewma_rtt_ms > 0.0
            ? ctl->ewma_rtt_ms : sample->rtt_ms;
        const double abs_rtt_delta =
            fabs((double)sample->rtt_ms - previous_rtt);
        ctl->ewma_abs_rtt_delta_ms = ewma(
            ctl->ewma_abs_rtt_delta_ms, abs_rtt_delta, EWMA_ALPHA_FAST);
        ctl->ewma_rtt_ms = ewma(
            ctl->ewma_rtt_ms, sample->rtt_ms, EWMA_ALPHA_FAST);
        ctl->ewma_loss_pct = ewma(
            ctl->ewma_loss_pct, loss_pct, EWMA_ALPHA_SLOW);
        ctl->ewma_missing_pct = ewma(
            ctl->ewma_missing_pct, missing_pct, EWMA_ALPHA_SLOW);
        ctl->ewma_retry_pct = ewma(
            ctl->ewma_retry_pct, retry_pct, EWMA_ALPHA_SLOW);
    }

    double score = 0.0;
    score += rtt_risk(ctl->ewma_rtt_ms);
    score += jitter_risk(ctl->ewma_abs_rtt_delta_ms);
    score += loss_risk(ctl->ewma_loss_pct);
    score += missing_risk(ctl->ewma_missing_pct);
    score += retry_risk(ctl->ewma_retry_pct);
    score += spacing_risk(sample->max_inter_packet_spacing_us);

    /* Missing packets that do not recover inside the transport window are
     * disproportionately dangerous because the decoder may lose references. */
    if (sample->missing >= 4 && recovery_pct < 95.0)
        score += 0.8;
    if (sample->missing >= 4 && recovery_pct < 80.0)
        score += 1.0;

    /* Emergency paths should not wait for EWMA to catch up. */
    if (sample->rtt_ms >= 650 || loss_pct >= 7.5 ||
        sample->max_inter_packet_spacing_us >= 1000000ULL)
        score = score < 7.0 ? 7.0 : score;

    irl_net_state_t candidate = classify(score);

    /* Fast attack, slow release: increase protection immediately, but require
     * sustained improvement before lowering latency. */
    if (ctl->state == IRL_NET_COLD || candidate > ctl->state) {
        ctl->state = candidate;
        ctl->release_candidate_since_ms = 0;
    } else if (candidate < ctl->state) {
        if (ctl->release_candidate_since_ms == 0) {
            ctl->release_candidate_since_ms = sample->timestamp_ms;
        } else if (sample->timestamp_ms - ctl->release_candidate_since_ms >=
                   RELEASE_DWELL_MS) {
            ctl->state = (irl_net_state_t)(ctl->state - 1);
            ctl->release_candidate_since_ms = sample->timestamp_ms;
        }
    } else {
        ctl->release_candidate_since_ms = 0;
    }

    out->state = ctl->state;
    out->smoothed_rtt_ms = ctl->ewma_rtt_ms;
    out->smoothed_rtt_jitter_ms = ctl->ewma_abs_rtt_delta_ms;
    out->interval_loss_pct = loss_pct;
    out->interval_missing_pct = missing_pct;
    out->interval_recovery_pct = recovery_pct;
    out->interval_retry_pct = retry_pct;
    out->quality_pct = sample->quality_pct;
    out->transport_buffer_ms = sample->avg_buffer_time_ms;
    out->risk_score = score;
    fill_policy(ctl->state, ctl->ewma_rtt_ms,
                ctl->ewma_abs_rtt_delta_ms, out);
    return true;
}

const char *irl_net_state_name(irl_net_state_t state)
{
    switch (state) {
    case IRL_NET_GOOD: return "GOOD";
    case IRL_NET_WARN: return "WARN";
    case IRL_NET_BAD: return "BAD";
    case IRL_NET_CRITICAL: return "CRITICAL";
    case IRL_NET_COLD:
    default: return "COLD";
    }
}
