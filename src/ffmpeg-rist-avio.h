/*
 * obs-irl-source Adaptive RIST experiment
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef IRL_FFMPEG_RIST_AVIO_H
#define IRL_FFMPEG_RIST_AVIO_H

#include <stdbool.h>

struct irl_source;
struct irl_direct_rist_io;
struct irl_rist_transport;
struct irl_adaptive_output;
struct irl_rist_sample;
typedef struct AVDictionary AVDictionary;

/* Open a direct libRIST -> custom AVIO -> MPEG-TS path. The caller still runs
 * avformat_find_stream_info() and the existing decoder setup. */
int irl_open_direct_rist_input(struct irl_source *src,
                               struct irl_direct_rist_io **io_out,
                               int stats_interval_ms,
                               AVDictionary **demuxer_options);

void irl_close_direct_rist_io(struct irl_direct_rist_io **io_ptr);

struct irl_rist_transport *
irl_direct_rist_transport(struct irl_direct_rist_io *io);

/* Poll a new libRIST stats interval and update the controller. Returns true only
 * once per stats callback. */
bool irl_direct_rist_poll(struct irl_direct_rist_io *io,
                          struct irl_adaptive_output *policy_out,
                          struct irl_rist_sample *sample_out,
                          bool *state_changed);

#endif
