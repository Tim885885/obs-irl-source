obs = obslua

function script_description()
    return "Updates a text source with IRL Source stats"
end

function script_update(settings)
end

function script_tick(seconds)
    local source = obs.obs_get_source_by_name("IRL Source (irlserver.com)")
    if not source then return end

    local ph = obs.obs_source_get_proc_handler(source)
    local cd = obs.calldata_create()
    obs.proc_handler_call(ph, "get_stats", cd)

    local buf_ms = obs.calldata_int(cd, "buffer_fill_ms")
    local speed = obs.calldata_float(cd, "current_speed")
    local ctrl = obs.calldata_bool(cd, "adaptive_latency_control")
    local reconnecting = obs.calldata_bool(cd, "reconnecting")
    local video = obs.calldata_int(cd, "total_video_frames")
    local audio = obs.calldata_int(cd, "total_audio_frames")
    local repairs = obs.calldata_int(cd, "pts_repairs")
    local silence = obs.calldata_int(cd, "silence_insertions")
    local underruns = obs.calldata_int(cd, "audio_underruns")
    local resync_skips = obs.calldata_int(cd, "audio_resync_skipped_chunks")
    local hidden_trims = obs.calldata_int(cd, "audio_hidden_trimmed_chunks")
    local quality_events = obs.calldata_int(cd, "audio_quality_events")
    local audio_flushes = obs.calldata_int(cd, "audio_decoder_flushes")
    local video_flushes = obs.calldata_int(cd, "video_decoder_flushes")
    local delay = obs.calldata_int(cd, "stream_delay_ms")

    obs.calldata_destroy(cd)
    obs.obs_source_release(source)

    local status = reconnecting and "RECONNECTING" or "LIVE"
    local text = string.format(
        "Status: %s\nDelay: %dms\nBuffer: %dms\nControl: %s\nCorrection: %.3fx\nFrames: %d/%d (v/a)\nPTS Repairs: %d\nAudio Quality: %d events\nSilence/Underruns: %d/%d\nHidden Trims: %d\nResync Skips: %d\nDecoder Flushes: %d/%d (a/v)",
        status, delay, buf_ms, ctrl and "on" or "off", speed, video, audio,
        repairs, quality_events, silence, underruns, hidden_trims,
        resync_skips, audio_flushes, video_flushes
    )

    local text_source = obs.obs_get_source_by_name("IRL Stats")
    if text_source then
        local settings = obs.obs_data_create()
        obs.obs_data_set_string(settings, "text", text)
        obs.obs_source_update(text_source, settings)
        obs.obs_data_release(settings)
        obs.obs_source_release(text_source)
    end
end
