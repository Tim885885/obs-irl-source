/*
 * obs-irl-source Adaptive RIST experiment
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef IRL_ADAPTIVE_RIST_H
#define IRL_ADAPTIVE_RIST_H

#include <stdbool.h>

#include "network-controller.h"

struct irl_rist_transport;
struct irl_adaptive_rist;

int irl_adaptive_rist_create(struct irl_adaptive_rist **out,
                             struct irl_rist_transport *transport);
void irl_adaptive_rist_destroy(struct irl_adaptive_rist **adaptive);

/* Feed one exact libRIST stats interval into the policy engine. This is the
 * preferred path when the caller also needs the same sample for telemetry: it
 * prevents a stats callback racing between two independent snapshots. */
bool irl_adaptive_rist_update(struct irl_adaptive_rist *adaptive,
                              const irl_rist_sample_t *sample,
                              irl_adaptive_output_t *out,
                              bool *state_changed);

/* Convenience wrapper: snapshots the transport and updates the controller.
 * Returns true only once per new stats callback. */
bool irl_adaptive_rist_poll(struct irl_adaptive_rist *adaptive,
                            irl_adaptive_output_t *out,
                            bool *state_changed);

#endif
