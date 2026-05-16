#include "Resampler.h"

#include "CDSPResampler.h"
#include <algorithm>

namespace AetherSDR {

Resampler::Resampler(double srcRate, double dstRate, int maxBlockSamples)
    : m_srcRate(srcRate)
    , m_dstRate(dstRate)
    , m_maxBlockSamples(maxBlockSamples)
    , m_resampler(std::make_unique<r8b::CDSPResampler24>(srcRate, dstRate, maxBlockSamples))
{
    m_inBuf.reserve(maxBlockSamples);
}

Resampler::~Resampler() = default;

std::vector<float> Resampler::process(const float* in, int numSamples)
{
    if (numSamples <= 0) return {};

    // r8b does not bounds-check against aMaxInLen; exceeding it silently
    // overflows internal filter buffers. Chunk so each call stays within limit.
    if (numSamples > m_maxBlockSamples) {
        std::vector<float> result;
        for (int offset = 0; offset < numSamples; offset += m_maxBlockSamples) {
            auto chunk = process(in + offset, std::min(numSamples - offset, m_maxBlockSamples));
            result.insert(result.end(), chunk.begin(), chunk.end());
        }
        return result;
    }

    // Convert float32 → double
    m_inBuf.resize(numSamples);
    for (int i = 0; i < numSamples; ++i)
        m_inBuf[i] = static_cast<double>(in[i]);

    // Resample
    double* outPtr = nullptr;
    int outLen = m_resampler->process(m_inBuf.data(), numSamples, outPtr);

    if (outLen <= 0 || !outPtr) return {};

    // Convert double → float32
    std::vector<float> result(outLen);
    for (int i = 0; i < outLen; ++i)
        result[i] = static_cast<float>(outPtr[i]);
    return result;
}

std::vector<float> Resampler::processStereoToMono(const float* stereoIn, int numStereoFrames)
{
    if (numStereoFrames <= 0) return {};

    if (numStereoFrames > m_maxBlockSamples) {
        std::vector<float> result;
        for (int offset = 0; offset < numStereoFrames; offset += m_maxBlockSamples) {
            auto chunk = processStereoToMono(stereoIn + offset * 2, std::min(numStereoFrames - offset, m_maxBlockSamples));
            result.insert(result.end(), chunk.begin(), chunk.end());
        }
        return result;
    }

    // Downmix stereo → mono
    m_inBuf.resize(numStereoFrames);
    for (int i = 0; i < numStereoFrames; ++i)
        m_inBuf[i] = (stereoIn[2 * i] + stereoIn[2 * i + 1]) * 0.5;

    // Resample
    double* outPtr = nullptr;
    int outLen = m_resampler->process(m_inBuf.data(), numStereoFrames, outPtr);

    if (outLen <= 0 || !outPtr) return {};

    // Convert double → float32 mono
    std::vector<float> result(outLen);
    for (int i = 0; i < outLen; ++i)
        result[i] = static_cast<float>(outPtr[i]);
    return result;
}

std::vector<float> Resampler::processMonoToStereo(const float* monoIn, int numSamples)
{
    if (numSamples <= 0) return {};

    if (numSamples > m_maxBlockSamples) {
        std::vector<float> result;
        for (int offset = 0; offset < numSamples; offset += m_maxBlockSamples) {
            auto chunk = processMonoToStereo(monoIn + offset, std::min(numSamples - offset, m_maxBlockSamples));
            result.insert(result.end(), chunk.begin(), chunk.end());
        }
        return result;
    }

    // Convert float32 → double
    m_inBuf.resize(numSamples);
    for (int i = 0; i < numSamples; ++i)
        m_inBuf[i] = static_cast<double>(monoIn[i]);

    // Resample
    double* outPtr = nullptr;
    int outLen = m_resampler->process(m_inBuf.data(), numSamples, outPtr);

    if (outLen <= 0 || !outPtr) return {};

    // Convert double → float32 stereo (duplicate mono to L+R)
    std::vector<float> result(outLen * 2);
    for (int i = 0; i < outLen; ++i) {
        float s = static_cast<float>(outPtr[i]);
        result[2 * i]     = s;
        result[2 * i + 1] = s;
    }
    return result;
}

std::vector<float> Resampler::processStereoToStereo(const float* stereoIn, int numStereoFrames)
{
    if (numStereoFrames <= 0) return {};

    if (numStereoFrames > m_maxBlockSamples) {
        std::vector<float> result;
        for (int offset = 0; offset < numStereoFrames; offset += m_maxBlockSamples) {
            auto chunk = processStereoToStereo(stereoIn + offset * 2, std::min(numStereoFrames - offset, m_maxBlockSamples));
            result.insert(result.end(), chunk.begin(), chunk.end());
        }
        return result;
    }

    // Downmix stereo → mono, resample, duplicate back to stereo
    m_inBuf.resize(numStereoFrames);
    for (int i = 0; i < numStereoFrames; ++i)
        m_inBuf[i] = (stereoIn[2 * i] + stereoIn[2 * i + 1]) * 0.5;

    double* outPtr = nullptr;
    int outLen = m_resampler->process(m_inBuf.data(), numStereoFrames, outPtr);

    if (outLen <= 0 || !outPtr) return {};

    std::vector<float> result(outLen * 2);
    for (int i = 0; i < outLen; ++i) {
        float s = static_cast<float>(outPtr[i]);
        result[2 * i]     = s;
        result[2 * i + 1] = s;
    }
    return result;
}

} // namespace AetherSDR
