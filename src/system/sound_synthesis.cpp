// SPDX-License-Identifier: GPL-3.0-or-later

#include "sound_synthesis.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace helix::audio {

FilterType filter_type_from_string(const std::string& type) {
    if (type == "lowpass")
        return FilterType::LOWPASS;
    if (type == "highpass")
        return FilterType::HIGHPASS;
    if (type.empty())
        return FilterType::NONE;
    spdlog::warn("[SoundSynth] Unknown filter type '{}', defaulting to lowpass", type);
    return FilterType::LOWPASS;
}

void generate_samples(float* buffer, int num_samples, int sample_rate,
                      Waveform wave, float freq, float amplitude,
                      float duty_cycle, float& phase) {
    if (sample_rate <= 0) {
        std::fill(buffer, buffer + num_samples, 0.0f);
        return;
    }
    const float phase_inc = freq / static_cast<float>(sample_rate);

    for (int i = 0; i < num_samples; ++i) {
        float sample = 0.0f;

        switch (wave) {
        case Waveform::SQUARE:
            sample = (phase < duty_cycle) ? amplitude : -amplitude;
            break;

        case Waveform::SAW:
            sample = amplitude * (2.0f * phase - 1.0f);
            break;

        case Waveform::TRIANGLE:
            sample = amplitude * (4.0f * std::abs(phase - 0.5f) - 1.0f);
            break;

        case Waveform::SINE:
            sample = amplitude * std::sin(2.0f * static_cast<float>(M_PI) * phase);
            break;
        }

        buffer[i] = sample;

        phase += phase_inc;
        phase -= std::floor(phase);
    }
}

void compute_biquad_coeffs(BiquadFilter& f, FilterType type, float cutoff,
                           float sample_rate) {
    if (sample_rate <= 0.0f) {
        // No valid sample rate yet — leave coefficients unchanged to avoid
        // inf/NaN propagating into the filter state.
        return;
    }
    constexpr float Q = 0.707107f; // 1/sqrt(2), Butterworth

    // Clamp cutoff to valid range (above 0, below Nyquist)
    cutoff = std::clamp(cutoff, 20.0f, sample_rate * 0.499f);

    const float omega = 2.0f * static_cast<float>(M_PI) * cutoff / sample_rate;
    const float sin_omega = std::sin(omega);
    const float cos_omega = std::cos(omega);
    const float alpha = sin_omega / (2.0f * Q);

    float a0 = 1.0f + alpha;

    if (type == FilterType::LOWPASS) {
        f.b0 = (1.0f - cos_omega) / 2.0f;
        f.b1 = 1.0f - cos_omega;
        f.b2 = (1.0f - cos_omega) / 2.0f;
    } else if (type == FilterType::HIGHPASS) {
        f.b0 = (1.0f + cos_omega) / 2.0f;
        f.b1 = -(1.0f + cos_omega);
        f.b2 = (1.0f + cos_omega) / 2.0f;
    } else {
        // NONE or unknown — default to lowpass
        f.b0 = (1.0f - cos_omega) / 2.0f;
        f.b1 = 1.0f - cos_omega;
        f.b2 = (1.0f - cos_omega) / 2.0f;
    }

    f.a1 = -2.0f * cos_omega;
    f.a2 = 1.0f - alpha;

    // Normalize by a0
    f.b0 /= a0;
    f.b1 /= a0;
    f.b2 /= a0;
    f.a1 /= a0;
    f.a2 /= a0;

    f.active = true;
    f.current_type = type;
    f.current_cutoff = cutoff;
    f.current_sample_rate = sample_rate;
}

void apply_filter(BiquadFilter& f, float* buffer, int num_samples) {
    if (!f.active)
        return;

    for (int i = 0; i < num_samples; ++i) {
        float x = buffer[i];
        float y = f.b0 * x + f.z1;
        f.z1 = f.b1 * x - f.a1 * y + f.z2;
        f.z2 = f.b2 * x - f.a2 * y;
        buffer[i] = y;
    }
}

void update_filter_if_needed(BiquadFilter& f, FilterType type, float cutoff,
                             float sample_rate) {
    if (f.current_type == type && f.current_cutoff == cutoff &&
        f.current_sample_rate == sample_rate) {
        return;
    }
    compute_biquad_coeffs(f, type, cutoff, sample_rate);
}

} // namespace helix::audio
