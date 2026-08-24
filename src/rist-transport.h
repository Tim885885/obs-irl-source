/*
 * obs-irl-source Adaptive RIST experiment
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef IRL_RIST_TRANSPORT_H
#define IRL_RIST_TRANSPORT_H

#include <stddef.h>
#include <stdint.h>

#include "network-controller.h"

#ifdef __cplusplus
extern "C" {
#endif

struct irl_rist_transport;

typedef struct irl_rist_transport_config {
    const char *url;
    int profile;              /* enum rist_profile; Main is 1 */
    int stats_interval_ms;    /* recommended 250-500 */
    uint32_t fifo_packets;    /* 0 = libRIST default; must be power of two */

    /* If enabled and the URL does not explicitly set buffer/buffer-min/max,
     * configure a recovery RANGE. Stock libRIST 0.2.20 has no public hot API
     * for changing the range after peer creation, but a min != max lets its
     * receiver buffering operate dynamically within the configured bounds. */
    int adaptive_recovery;
    uint32_t recovery_min_ms;
    uint32_t recovery_max_ms;
    uint32_t rtt_min_ms;
    uint32_t rtt_max_ms;
    uint32_t reorder_buffer_ms;

    /* -1: leave libRIST default; 0: disabled; 1: enable receiver CBR output. */
    int cbr_output;
} irl_rist_transport_config_t;

/* One normalized libRIST receiver-flow stats INTERVAL. */
typedef struct irl_rist_transport_stats {
    uint64_t timestamp_ms;
    uint32_t rtt_ms;
    uint64_t received;
    uint32_t missing;
    uint32_t recovered;
    uint32_t reordered;
    uint32_t lost;
    uint64_t max_inter_packet_spacing_us;
    uint64_t bandwidth_bps;
    uint64_t retry_bandwidth_bps;
    uint64_t avg_buffer_time_ms;
    double quality_pct;
    uint32_t flow_id;
    int status;
} irl_rist_transport_stats_t;

int irl_rist_transport_open(struct irl_rist_transport **out,
                            const irl_rist_transport_config_t *config);

void irl_rist_transport_close(struct irl_rist_transport **transport);

/* Read bytes from libRIST's data-block FIFO. Returns:
 *   >0 bytes copied
 *    0 timeout/no data
 *   <0 transport error
 * The libRIST read call itself returns queue depth, not payload byte count; this
 * wrapper intentionally returns the actual bytes copied for AVIO. */
int irl_rist_transport_read(struct irl_rist_transport *transport,
                            uint8_t *dst, int dst_size, int timeout_ms);

/* Thread-safe snapshot of the latest stats callback. Returns 1 when valid. */
int irl_rist_transport_get_stats(const struct irl_rist_transport *transport,
                                 irl_rist_transport_stats_t *out);

int irl_rist_transport_get_controller_sample(
    const struct irl_rist_transport *transport, irl_rist_sample_t *out);

/* Optional extension hook. Stock VideoLAN libRIST 0.2.20 returns -2 because
 * the recovery range is not publicly mutable after peer creation. */
int irl_rist_transport_set_rtt_multiplier(struct irl_rist_transport *transport,
                                          int multiplier);

#ifdef __cplusplus
}
#endif

#endif
