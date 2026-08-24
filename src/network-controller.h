/*
 * obs-irl-source Adaptive RIST experiment
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef IRL_NETWORK_CONTROLLER_H
#define IRL_NETWORK_CONTROLLER_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum irl_net_state {
    IRL_NET_COLD = 0,
    IRL_NET_GOOD,
    IRL_NET_WARN,
    IRL_NET_BAD,
    IRL_NET_CRITICAL,
} irl_net_state_t;

/*
 * One libRIST receiver-flow stats interval.
 *
 * IMPORTANT: libRIST's receiver_flow.{received,missing,recovered,reordered,lost}
 * are interval counters (stats_instant), not process-lifetime cumulative
 * counters. libRIST clears them after each stats callback. timestamp_ms must be
 * monotonic and is used only for hysteresis timing.
 */
typedef struct irl_rist_sample {
    uint64_t timestamp_ms;
    uint32_t rtt_ms;

    uint64_t received;
    uint32_t missing;
    uint32_t recovered;
    uint32_t reordered;
    uint32_t lost;

    uint64_t max_inter_packet_spacing_us;

    /* Current transport diagnostics from the same stats interval. */
    uint64_t bandwidth_bps;
    uint64_t retry_bandwidth_bps;
    uint64_t avg_buffer_time_ms;
    double quality_pct;
} irl_rist_sample_t;

typedef struct irl_adaptive_output {
    irl_net_state_t state;

    /* Playout cushion used by obs-irl-source's audio/playout controller. */
    uint32_t playout_target_ms;

    /* Recommended RIST recovery range. Upstream libRIST 0.2.20 exposes the
     * min/max at peer creation time; this is therefore a diagnostic / next-
     * reconnect recommendation unless a fork provides a runtime control API. */
    uint32_t recovery_min_ms;
    uint32_t recovery_max_ms;

    /* Suggested catch-up ceiling after a stall. 1.00 means real-time. */
    double catchup_speed;

    /* Policy hints for a later live-edge controller. */
    bool allow_video_drop;
    bool allow_hard_live_edge_jump;
    uint32_t hard_live_edge_threshold_ms;

    /* Smoothed diagnostics. */
    double smoothed_rtt_ms;
    double smoothed_rtt_jitter_ms;
    double interval_loss_pct;
    double interval_missing_pct;
    double interval_recovery_pct;
    double interval_retry_pct;
    double quality_pct;
    uint64_t transport_buffer_ms;
    double risk_score;
} irl_adaptive_output_t;

typedef struct irl_network_controller {
    bool initialized;

    double ewma_rtt_ms;
    double ewma_abs_rtt_delta_ms;
    double ewma_loss_pct;
    double ewma_missing_pct;
    double ewma_retry_pct;

    irl_net_state_t state;
    uint64_t release_candidate_since_ms;
} irl_network_controller_t;

void irl_network_controller_init(irl_network_controller_t *ctl);

/* Returns true when a decision is available. Since libRIST's stats are already
 * interval-based, the first real interval can be classified immediately. */
bool irl_network_controller_update(irl_network_controller_t *ctl,
                                   const irl_rist_sample_t *sample,
                                   irl_adaptive_output_t *out);

const char *irl_net_state_name(irl_net_state_t state);

#ifdef __cplusplus
}
#endif

#endif
