/*
 * obs-irl-source — IRL streaming source plugin for OBS
 * https://irlserver.com
 *
 * Copyright (C) 2026 Thomas Lekanger
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * settings.c — OBS properties UI for the IRL source
 */

#include "../include/irl-source.h"

/* ── Defaults ─────────────────────────────────────────────── */

void irl_source_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, "url", "");
	obs_data_set_default_int(settings, "reconnect_delay",
				 IRL_DEFAULT_RECONNECT_DELAY);
	obs_data_set_default_int(settings, "network_buffer_mb",
				 IRL_DEFAULT_NETWORK_BUFFER_MB);

	obs_data_set_default_int(settings, "buffer_target_ms",
				 IRL_DEFAULT_BUFFER_TARGET_MS);
	obs_data_set_default_int(settings, "buffer_min_ms",
				 IRL_DEFAULT_BUFFER_MIN_MS);
	obs_data_set_default_int(settings, "buffer_max_ms",
				 IRL_DEFAULT_BUFFER_MAX_MS);
	obs_data_set_default_bool(settings, "adaptive_speed",
				  IRL_DEFAULT_ADAPTIVE_SPEED);

	obs_data_set_default_int(settings, "small_gap_ms",
				 IRL_DEFAULT_SMALL_GAP_MS);
	obs_data_set_default_int(settings, "large_gap_ms",
				 IRL_DEFAULT_LARGE_GAP_MS);

	obs_data_set_default_string(settings, "ffmpeg_options", "");
	obs_data_set_default_int(settings, "hw_decode", IRL_DEFAULT_HW_DECODE);
	obs_data_set_default_bool(settings, "wait_for_keyframe",
				  IRL_DEFAULT_WAIT_KEYFRAME);
	obs_data_set_default_bool(settings, "low_latency_audio",
				  IRL_DEFAULT_LOW_LATENCY_AUDIO);
	obs_data_set_default_bool(settings, "decoupled_audio",
				  IRL_DEFAULT_DECOUPLED_AUDIO);
	obs_data_set_default_bool(settings, "close_when_inactive",
				  IRL_DEFAULT_CLOSE_WHEN_INACTIVE);
}

/* ── Properties ───────────────────────────────────────────── */

obs_properties_t *irl_source_get_properties(void *data)
{
	UNUSED_PARAMETER(data);

	obs_properties_t *props = obs_properties_create();

	/* ── General ───────────────────────────────────────── */

	obs_properties_add_text(props, "url",
				obs_module_text("URL"),
				OBS_TEXT_DEFAULT);
	obs_properties_add_int(props, "reconnect_delay",
			       obs_module_text("Reconnect Delay (s)"), 1, 60,
			       1);
	obs_properties_add_int(props, "network_buffer_mb",
			       obs_module_text("Network Buffer (MB)"), 0, 16,
			       1);
	obs_properties_add_text(
		props, "general_help",
		obs_module_text(
			"Reconnect Delay controls how quickly the source retries "
			"after a disconnect. Network Buffer is transport-level "
			"buffering before decode; the default 2 MB is usually the "
			"right starting point for SRT/mobile links."),
		OBS_TEXT_INFO);

	/* ── Audio Buffer ──────────────────────────────────── */

	obs_properties_add_int(props, "buffer_target_ms",
			       obs_module_text("Target Buffer (ms)"), 20, 500,
			       10);
	obs_properties_add_int(props, "buffer_min_ms",
			       obs_module_text("Min Buffer (ms)"), 10, 200, 5);
	obs_properties_add_int(props, "buffer_max_ms",
			       obs_module_text("Max Buffer (ms)"), 50, 1000,
			       10);
	obs_properties_add_bool(props, "adaptive_speed",
				obs_module_text("Adaptive Latency Control"));
	obs_properties_add_text(
		props, "audio_buffer_help",
		obs_module_text(
			"Buffered mode is the normal IRL path: Target/Min/Max "
			"Buffer absorb short jitter. Adaptive Latency Control "
			"keeps audio at native rate and trims only hidden/recovery "
			"backlog before it becomes audible. Lower values reduce "
			"delay but make silence or hitches more likely on bad "
			"signal."),
		OBS_TEXT_INFO);

	/* ── PTS Repair ────────────────────────────────────── */

	obs_properties_add_int(props, "small_gap_ms",
			       obs_module_text("Small Gap Threshold (ms)"), 10,
			       500, 5);
	obs_properties_add_int(props, "large_gap_ms",
			       obs_module_text("Large Gap Threshold (ms)"), 500,
			       10000, 100);
	obs_properties_add_text(
		props, "pts_help",
		obs_module_text(
			"Small gaps are interpolated. Medium gaps insert silence. "
			"Large gaps trigger a clean timing reset. The defaults are "
			"meant to avoid glitchy audio; silence is preferred over "
			"audible artifacts when real audio is missing."),
		OBS_TEXT_INFO);

	/* ── Advanced ──────────────────────────────────────── */

	obs_properties_add_text(props, "ffmpeg_options",
				obs_module_text("FFmpeg Options"),
				OBS_TEXT_DEFAULT);

	obs_property_t *hw = obs_properties_add_list(
		props, "hw_decode", obs_module_text("Hardware Decode"),
		OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(hw, obs_module_text("Auto"), 0);
	obs_property_list_add_int(hw, obs_module_text("Off"), 1);

	obs_properties_add_bool(
		props, "wait_for_keyframe",
		obs_module_text("Wait for Keyframe on Start/Reconnect"));
	obs_properties_add_bool(
		props, "low_latency_audio",
		obs_module_text("Use OBS Low-Latency Async Audio Mode"));
	obs_properties_add_bool(
		props, "decoupled_audio",
		obs_module_text("Decoupled Audio (Low Latency Only)"));
	obs_properties_add_bool(props, "close_when_inactive",
				obs_module_text("Close Stream When Inactive"));
	obs_properties_add_text(
		props, "advanced_help",
		obs_module_text(
			"Wait for Keyframe avoids corrupt startup/reconnect frames, "
			"but it can add up to one keyframe interval of delay. The "
			"low-latency option enables OBS async unbuffered audio mode "
			"for this source and makes the plugin drain immediately "
			"instead of waiting for the normal buffer minimum, so it is "
			"faster but less tolerant of jitter. Decoupled Audio only "
			"matters when the low-latency mode is enabled. Close Stream "
			"When Inactive stops receiving when the source is not active "
			"and clears the last frame to black."),
		OBS_TEXT_INFO);

	/* ── About ─────────────────────────────────────────── */

	obs_properties_add_text(
		props, "about_info",
		obs_module_text(
			"IRL Source v" OBS_IRL_SOURCE_VERSION
			" by Thomas Lekanger\n"
			"https://irlserver.com\n\n"
			"Codec/protocol-agnostic live source with audio jitter "
			"buffering, PTS discontinuity repair, adaptive latency "
			"control, and first-keyframe gating.\n\n"
			"Licensed under AGPL-3.0-or-later"),
		OBS_TEXT_INFO);

	return props;
}
