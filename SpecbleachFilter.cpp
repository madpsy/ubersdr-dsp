#ifdef HAVE_SPECBLEACH

#include "SpecbleachFilter.h"
#include <specbleach_denoiser.h>
#include <cstdio>
#include <cstring>
#include <algorithm>

namespace AetherSDR {

static constexpr int kSampleRate = 24000;
static constexpr float kFrameSizeMs = 40.0f;  // 40ms frames (~960 samples)

SpecbleachFilter::SpecbleachFilter()
{
    m_handle = specbleach_initialize(kSampleRate, kFrameSizeMs);
    if (!m_handle) {
        fprintf(stderr, "SpecbleachFilter: failed to initialize\n");
        return;
    }
    applyParams();
    fprintf(stderr, "SpecbleachFilter: initialized, latency = %u samples\n",
            specbleach_get_latency(m_handle));
}

SpecbleachFilter::~SpecbleachFilter()
{
    if (m_handle)
        specbleach_free(m_handle);
}

void SpecbleachFilter::reset()
{
    m_frameCount = 0;
    m_inAccum.clear();
    m_outAccum.clear();
    if (m_handle)
        specbleach_reset_noise_profile(m_handle);
}

void SpecbleachFilter::applyParams()
{
    if (!m_handle) return;

    SpectralBleachDenoiserParameters params{};
    params.learn_noise = 0;
    params.residual_listen = false;
    params.reduction_amount = m_reduction.load();
    params.smoothing_factor = m_smoothing.load();
    params.whitening_factor = m_whitening.load();
    params.adaptive_noise = m_adaptive.load() ? 1 : 0;
    params.noise_estimation_method = m_noiseMethod.load();
    params.masking_depth = m_maskingDepth.load();
    params.suppression_strength = m_suppression.load();
    params.aggressiveness = 0.0f;
    params.tonal_reduction = 0.0f;

    specbleach_load_parameters(m_handle, params);
    m_paramsDirty = false;
}

std::vector<float> SpecbleachFilter::process(const float* stereoIn, int numStereoFrames)
{
    const int needed = numStereoFrames * 2; // stereo output samples to return

    if (!m_handle)
        return std::vector<float>(stereoIn, stereoIn + needed);

    // Apply parameter changes if dirty
    if (m_paramsDirty.load())
        applyParams();

    if (numStereoFrames <= 0)
        return {};

    // ── Stereo → mono, append to input accumulator ────────────────────────────
    // libspecbleach was initialised with a fixed 40 ms frame size (kFrameSamples).
    // We accumulate incoming mono samples and feed complete frames to the library,
    // keeping any leftover samples for the next call.
    const int prevAccum = static_cast<int>(m_inAccum.size());
    m_inAccum.resize(prevAccum + numStereoFrames);
    for (int i = 0; i < numStereoFrames; ++i)
        m_inAccum[prevAccum + i] = (stereoIn[i * 2] + stereoIn[i * 2 + 1]) * 0.5f;

    // ── Process complete kFrameSamples-sized frames ───────────────────────────
    const int totalAccum   = static_cast<int>(m_inAccum.size());
    const int completeFrames = totalAccum / kFrameSamples;

    if (completeFrames > 0) {
        const int consumedSamples = completeFrames * kFrameSamples;

        // Resize scratch buffers once
        if (static_cast<int>(m_monoIn.size()) < consumedSamples) {
            m_monoIn.resize(consumedSamples);
            m_monoOut.resize(consumedSamples);
        }

        // Copy the consumed portion into a contiguous scratch buffer
        std::copy(m_inAccum.begin(), m_inAccum.begin() + consumedSamples, m_monoIn.begin());

        // Feed audio to build noise profile even during learning period
        specbleach_process(m_handle, consumedSamples, m_monoIn.data(), m_monoOut.data());

        // Keep leftover input samples
        const int leftover = totalAccum - consumedSamples;
        if (leftover > 0)
            m_inAccum = std::vector<float>(m_inAccum.begin() + consumedSamples, m_inAccum.end());
        else
            m_inAccum.clear();

        // During the learning period, pass original audio through so the user
        // hears unprocessed audio instead of silence while the noise profile
        // builds. The library still receives the audio above for profiling.
        // m_frameCount counts complete kFrameSamples-sized frames processed.
        const int prevFrameCount = m_frameCount;
        m_frameCount += completeFrames;

        // Expand processed mono → stereo and append to output accumulator.
        // For frames still in the learning period, use the original stereo input
        // instead of the processed output.
        const int outBase = static_cast<int>(m_outAccum.size());
        m_outAccum.resize(outBase + consumedSamples * 2);
        for (int f = 0; f < completeFrames; ++f) {
            const bool learning = (prevFrameCount + f) < kLearningFrames;
            for (int s = 0; s < kFrameSamples; ++s) {
                const int monoIdx   = f * kFrameSamples + s;
                const int stereoIdx = outBase + monoIdx * 2;
                if (learning) {
                    // Passthrough: reconstruct stereo from original input.
                    // The original stereo input for this frame starts at
                    // frame f * kFrameSamples in the stereoIn array — but
                    // stereoIn may be shorter than consumedSamples if the
                    // client sent a smaller chunk.  Guard with a bounds check.
                    const int srcIdx = monoIdx; // mono index == stereo frame index
                    if (srcIdx < numStereoFrames) {
                        m_outAccum[stereoIdx]     = stereoIn[srcIdx * 2];
                        m_outAccum[stereoIdx + 1] = stereoIn[srcIdx * 2 + 1];
                    } else {
                        // Accumulated samples from a previous call — use mono value
                        m_outAccum[stereoIdx]     = m_monoIn[monoIdx];
                        m_outAccum[stereoIdx + 1] = m_monoIn[monoIdx];
                    }
                } else {
                    m_outAccum[stereoIdx]     = m_monoOut[monoIdx];
                    m_outAccum[stereoIdx + 1] = m_monoOut[monoIdx];
                }
            }
        }
    }

    // ── Return exactly numStereoFrames worth of stereo output ─────────────────
    if (static_cast<int>(m_outAccum.size()) >= needed) {
        std::vector<float> result(m_outAccum.begin(), m_outAccum.begin() + needed);
        m_outAccum.erase(m_outAccum.begin(), m_outAccum.begin() + needed);
        return result;
    }

    // Not enough output yet — return silence (only during startup / first chunk)
    return std::vector<float>(needed, 0.0f);
}

// Parameter setters — mark dirty so next process() applies them
void SpecbleachFilter::setReductionAmount(float dB)   { m_reduction = std::clamp(dB, 0.0f, 40.0f); m_paramsDirty = true; }
void SpecbleachFilter::setSmoothingFactor(float pct)   { m_smoothing = std::clamp(pct, 0.0f, 100.0f); m_paramsDirty = true; }
void SpecbleachFilter::setWhiteningFactor(float pct)   { m_whitening = std::clamp(pct, 0.0f, 100.0f); m_paramsDirty = true; }
void SpecbleachFilter::setAdaptiveNoise(bool on)       { m_adaptive = on; m_paramsDirty = true; }
void SpecbleachFilter::setNoiseEstimationMethod(int m) { m_noiseMethod = std::clamp(m, 0, 2); m_paramsDirty = true; }
void SpecbleachFilter::setMaskingDepth(float v)        { m_maskingDepth = std::clamp(v, 0.0f, 1.0f); m_paramsDirty = true; }
void SpecbleachFilter::setSuppressionStrength(float v) { m_suppression = std::clamp(v, 0.0f, 1.0f); m_paramsDirty = true; }

} // namespace AetherSDR

#endif // HAVE_SPECBLEACH
