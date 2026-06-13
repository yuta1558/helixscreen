// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#ifdef HELIX_DISPLAY_SDL

#include "sdl_sound_backend.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstring>

SDLSoundBackend::SDLSoundBackend() = default;

SDLSoundBackend::~SDLSoundBackend() {
    shutdown();
}

bool SDLSoundBackend::initialize() {
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0) {
        spdlog::error("[SDLSound] SDL_InitSubSystem(AUDIO) failed: {}", SDL_GetError());
        return false;
    }

    SDL_AudioSpec desired{};
    desired.freq = sample_rate_;
    desired.format = AUDIO_F32SYS;
    desired.channels = 1;
    desired.samples = 64; // Very low latency buffer — keeps callback period ~1.5ms
    desired.callback = audio_callback;
    desired.userdata = this;

    SDL_AudioSpec obtained{};
    device_id_ = SDL_OpenAudioDevice(nullptr, 0, &desired, &obtained, 0);
    if (device_id_ == 0) {
        spdlog::error("[SDLSound] SDL_OpenAudioDevice failed: {}", SDL_GetError());
        return false;
    }

    sample_rate_ = obtained.freq;
    // Resize the mix buffer BEFORE unpausing — SDL's audio thread starts
    // calling audio_callback the instant playback unpauses, and the callback
    // memsets mix_buf_.data(). If the callback fires before the resize,
    // data() is nullptr and the memset segfaults the audio thread.
    mix_buf_.resize(obtained.samples);
    SDL_PauseAudioDevice(device_id_, 0); // Start playback
    initialized_ = true;

    spdlog::info("[SDLSound] Audio initialized: {} Hz, {} samples buffer", sample_rate_,
                 obtained.samples);
    return true;
}

void SDLSoundBackend::shutdown() {
    if (!initialized_)
        return;
    if (device_id_) {
        SDL_CloseAudioDevice(device_id_);
        device_id_ = 0;
    }
    initialized_ = false;
    spdlog::info("[SDLSound] Audio shutdown");
}

// ============================================================================
// SoundBackend interface — write through to voice_slots_[0] or all slots
// ============================================================================

void SDLSoundBackend::set_tone(float freq_hz, float amplitude, float duty_cycle) {
    set_voice(0, freq_hz, amplitude, duty_cycle);
}

void SDLSoundBackend::silence() {
    for (int v = 0; v < MAX_VOICES; ++v) {
        voice_slots_[v].event.velocity = 0;
        voice_slots_[v].generation.fetch_add(1, std::memory_order_release);
    }
}

void SDLSoundBackend::set_waveform(Waveform w) {
    set_voice_waveform(0, w);
}

// Legacy voice interface: writes individual fields and bumps generation so the
// audio callback picks up the change atomically on the next generation check.
void SDLSoundBackend::set_voice(int slot, float freq_hz, float amplitude, float duty_cycle) {
    if (slot < 0 || slot >= MAX_VOICES) return;
    auto& s = voice_slots_[slot];
    s.event.freq_hz = freq_hz;
    s.event.velocity = amplitude;
    s.event.duty_cycle = duty_cycle;
    s.generation.fetch_add(1, std::memory_order_release);
}

void SDLSoundBackend::set_voice_waveform(int slot, Waveform w) {
    if (slot < 0 || slot >= MAX_VOICES) return;
    // No generation bump — waveform alone doesn't restart the note.
    voice_slots_[slot].event.wave = w;
}

void SDLSoundBackend::silence_voice(int slot) {
    if (slot < 0 || slot >= MAX_VOICES) return;
    voice_slots_[slot].event.velocity = 0;
    voice_slots_[slot].generation.fetch_add(1, std::memory_order_release);
}

void SDLSoundBackend::set_filter(const std::string& type, float cutoff) {
    // Legacy path: sets filter on voice 0 for backward compat.
    // NoteEvent callers embed filter params directly in the NoteEvent.
    auto& ev = voice_slots_[0].event;
    if (type.empty()) {
        ev.filter_type = 0;
    } else if (type == "lowpass") {
        ev.filter_type = 1;
        ev.filter_cutoff = cutoff;
    } else if (type == "highpass") {
        ev.filter_type = 2;
        ev.filter_cutoff = cutoff;
    }
    // No generation bump — filter change takes effect on next note start.
}

// Primary note-event path: publish a complete NoteEvent, then bump generation.
// The audio callback snapshots event → active on the next callback, ensuring all
// parameters (freq, envelope, sweep, LFO, filter) are seen as a unit.
void SDLSoundBackend::publish_note(int slot, const NoteEvent& event) {
    if (slot < 0 || slot >= MAX_VOICES) return;
    voice_slots_[slot].event = event;
    voice_slots_[slot].generation.fetch_add(1, std::memory_order_release);
}

void SDLSoundBackend::set_render_source(std::function<void(float*, size_t, int)> fn) {
    std::lock_guard<std::mutex> lock(render_source_mutex_);
    render_source_ = std::move(fn);
}

void SDLSoundBackend::clear_render_source() {
    std::lock_guard<std::mutex> lock(render_source_mutex_);
    render_source_ = nullptr;
}

// ============================================================================
// Audio callback (runs in SDL audio thread)
// ============================================================================

void SDLSoundBackend::audio_callback(void* userdata, uint8_t* stream, int len) {
    auto* self = static_cast<SDLSoundBackend*>(userdata);
    auto* out = reinterpret_cast<float*>(stream);
    int num_samples = len / static_cast<int>(sizeof(float));

    // Guard against SDL delivering a larger buffer than mix_buf_ was sized for.
    // Clamp and zero any trailing bytes so the output is silence for those samples
    // rather than a heap overwrite. Heap allocation in an RT callback is avoided
    // intentionally — mix_buf_ is sized from obtained.samples in initialize().
    if (static_cast<size_t>(num_samples) > self->mix_buf_.size()) {
        std::memset(stream, 0, static_cast<size_t>(len));
        num_samples = static_cast<int>(self->mix_buf_.size());
    }

    auto* mix = self->mix_buf_.data();
    std::memset(mix, 0, num_samples * sizeof(float));
    bool has_audio = false;

    // Render tracker PCM if active
    {
        std::function<void(float*, size_t, int)> source;
        {
            std::lock_guard<std::mutex> lock(self->render_source_mutex_);
            source = self->render_source_;
        }
        if (source) {
            source(mix, static_cast<size_t>(num_samples), self->sample_rate_);
            has_audio = true;
        }
    }

    // Mix synth voices using VoiceSlot per-sample rendering
    float sr = static_cast<float>(self->sample_rate_);
    for (int v = 0; v < MAX_VOICES; ++v) {
        auto& slot = self->voice_slots_[v];

        // Detect new note: snapshot all event params atomically on generation change.
        // This guarantees freq/envelope/sweep/LFO are always read from the same publish.
        uint32_t gen = slot.generation.load(std::memory_order_acquire);
        if (gen != slot.cb_generation) {
            slot.cb_generation = gen;
            slot.reset_for_new_note();
            // reset_for_new_note() calls compute_biquad_coeffs with sample_rate=0;
            // fix up with the real sample rate if a filter is active.
            if (slot.active.filter_type != 0) {
                auto ft = (slot.active.filter_type == 1) ? helix::audio::FilterType::LOWPASS
                                                          : helix::audio::FilterType::HIGHPASS;
                helix::audio::compute_biquad_coeffs(slot.filter, ft,
                                                     slot.active.filter_cutoff, sr);
            }
        }

        // Skip silent voices (both currently playing and envelope tail)
        if (slot.active.velocity <= 0.001f && slot.current_amplitude <= 0.001f) {
            continue;
        }

        // Render per-sample into mix — render_sample() advances phase, elapsed_samples,
        // and current_amplitude internally.
        for (int i = 0; i < num_samples; ++i) {
            mix[i] += slot.render_sample(sr);
        }
        has_audio = true;
    }

    if (!has_audio) {
        std::memset(stream, 0, static_cast<size_t>(len));
        return;
    }

    // Clamp to prevent inter-voice accumulation from overdriving
    for (int i = 0; i < num_samples; ++i)
        mix[i] = std::clamp(mix[i], -1.0f, 1.0f);

    std::memcpy(out, mix, num_samples * sizeof(float));
}

#endif // HELIX_DISPLAY_SDL
