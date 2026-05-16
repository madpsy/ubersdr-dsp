#include "RNNoiseFilter.h"
#include "Resampler.h"
#include "rnnoise.h"

#include <cstring>
#include <vector>

namespace AetherSDR {

// RNNoise frame size: 480 samples at 48kHz = 10ms
static constexpr int FRAME_SIZE = 480;

RNNoiseFilter::RNNoiseFilter()
    : m_state(rnnoise_create(nullptr))
    , m_up(std::make_unique<Resampler>(24000, 48000))
    , m_down(std::make_unique<Resampler>(48000, 24000))
{}

RNNoiseFilter::~RNNoiseFilter()
{
    if (m_state)
        rnnoise_destroy(m_state);
}

void RNNoiseFilter::reset()
{
    if (m_state)
        rnnoise_destroy(m_state);
    m_state = rnnoise_create(nullptr);
    m_up = std::make_unique<Resampler>(24000, 48000);
    m_down = std::make_unique<Resampler>(48000, 24000);
    m_inAccum.clear();
    m_outAccum.clear();
}

std::vector<float> RNNoiseFilter::process(const float* stereoIn, int numStereoFrames)
{
    const int needed = numStereoFrames * 2; // stereo output samples

    if (!m_state || numStereoFrames <= 0) {
        // passthrough
        return std::vector<float>(stereoIn, stereoIn + needed);
    }

    // 1. Upsample 24kHz stereo float32 → 48kHz mono float32 via r8brain
    std::vector<float> mono48k = m_up->processStereoToMono(stereoIn, numStereoFrames);
    const int monoSamples48k = static_cast<int>(mono48k.size());

    // 2. Append to input accumulator and process complete 480-sample frames
    //    RNNoise expects [-32768, 32768] range, so scale from [-1, 1]
    const int prevAccumSamples = static_cast<int>(m_inAccum.size());
    m_inAccum.resize(prevAccumSamples + monoSamples48k);
    for (int i = 0; i < monoSamples48k; ++i)
        m_inAccum[prevAccumSamples + i] = mono48k[i] * 32768.0f;

    const int totalAccumSamples = prevAccumSamples + monoSamples48k;
    const int completeFrames = totalAccumSamples / FRAME_SIZE;

    if (completeFrames > 0) {
        std::vector<float> processed48k(completeFrames * FRAME_SIZE);

        for (int f = 0; f < completeFrames; ++f) {
            rnnoise_process_frame(m_state,
                                  &processed48k[f * FRAME_SIZE],
                                  &m_inAccum[f * FRAME_SIZE]);
        }

        // Keep leftover input samples
        const int consumedSamples = completeFrames * FRAME_SIZE;
        const int leftoverSamples = totalAccumSamples - consumedSamples;
        if (leftoverSamples > 0)
            m_inAccum = std::vector<float>(m_inAccum.begin() + consumedSamples, m_inAccum.end());
        else
            m_inAccum.clear();

        // 3. Scale processed 48kHz float from RNNoise range [-32768,32768] to [-1,1],
        //    then downsample to 24kHz stereo float32
        const int outputMonoSamples = completeFrames * FRAME_SIZE;
        for (int i = 0; i < outputMonoSamples; ++i)
            processed48k[i] /= 32768.0f;

        // Downsample 48kHz mono → 24kHz stereo via r8brain
        std::vector<float> downsampled = m_down->processMonoToStereo(
            processed48k.data(), outputMonoSamples);

        m_outAccum.insert(m_outAccum.end(), downsampled.begin(), downsampled.end());
    }

    // 4. Return exactly the same number of samples as input (stereo)
    if (static_cast<int>(m_outAccum.size()) >= needed) {
        std::vector<float> result(m_outAccum.begin(), m_outAccum.begin() + needed);
        m_outAccum.erase(m_outAccum.begin(), m_outAccum.begin() + needed);
        return result;
    }

    // Not enough output yet — return silence (only happens during startup)
    return std::vector<float>(needed, 0.0f);
}

} // namespace AetherSDR
