#include "BnrFilterWrapper.h"

#include <charconv>
#include <cstdio>
#include <string>

namespace ubersdr {

static bool parseFloat(const std::string& s, float& out)
{
    auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
    return ec == std::errc{};
}

// ── BnrFilterWrapper ──────────────────────────────────────────────────────────

BnrFilterWrapper::BnrFilterWrapper(const std::string& bnrAddress)
    : m_up(std::make_unique<AetherSDR::Resampler>(24000, 48000))
    , m_down(std::make_unique<AetherSDR::Resampler>(48000, 24000))
{
    m_filter.onError = [](const std::string& msg) {
        fprintf(stderr, "BNR error: %s\n", msg.c_str());
    };
    m_filter.onConnectionChanged = [](bool connected) {
        fprintf(stderr, "BNR: %s\n", connected ? "connected" : "disconnected");
    };

    // Empty address means "describe-only" construction (used by GetFilters RPC).
    if (bnrAddress.empty())
        return;

#ifdef HAVE_BNR
    if (!m_filter.connectToServer(bnrAddress))
        fprintf(stderr, "BNR: failed to connect to %s\n", bnrAddress.c_str());
#else
    (void)bnrAddress;
    fprintf(stderr, "BNR: not compiled in\n");
#endif
}

BnrFilterWrapper::~BnrFilterWrapper()
{
#ifdef HAVE_BNR
    m_filter.disconnect();
#endif
}

std::vector<float> BnrFilterWrapper::process(const float* pcm, int stereoFrames)
{
    const int needed = stereoFrames * 2;

#ifndef HAVE_BNR
    return std::vector<float>(pcm, pcm + needed);
#else
    // 1. Upsample 24 kHz stereo → 48 kHz mono via r8brain.
    //    processStereoToMono downmixes L+R and resamples in one pass.
    std::vector<float> mono48k = m_up->processStereoToMono(pcm, stereoFrames);

    // 2. Push 48 kHz mono samples into NvidiaBnrFilter (non-blocking).
    //    The filter's worker thread sends them to the NIM server and
    //    accumulates denoised responses in its output buffer.
    std::vector<float> denoised48k = m_filter.process(mono48k.data(),
                                                       static_cast<int>(mono48k.size()));

    // 3. Downsample any returned 48 kHz mono → 24 kHz stereo and accumulate.
    if (!denoised48k.empty()) {
        std::vector<float> stereo24k = m_down->processMonoToStereo(
            denoised48k.data(), static_cast<int>(denoised48k.size()));
        m_outAccum.insert(m_outAccum.end(), stereo24k.begin(), stereo24k.end());
    }

    // 4. Return exactly stereoFrames * 2 samples when we have enough.
    //    This keeps the output frame count consistent with all other filters.
    if (static_cast<int>(m_outAccum.size()) >= needed) {
        std::vector<float> result(m_outAccum.begin(), m_outAccum.begin() + needed);
        m_outAccum.erase(m_outAccum.begin(), m_outAccum.begin() + needed);
        return result;
    }

    // Not enough output yet (NIM round-trip latency during startup).
    // Return silence to maintain timing — this only happens for the first
    // few frames while the pipeline fills.
    return std::vector<float>(needed, 0.0f);
#endif
}

std::pair<ParamMap, ParamMap> BnrFilterWrapper::applyParams(const ParamMap& in)
{
    ParamMap applied, rejected;

    for (const auto& [key, val] : in) {
        if (key == "intensity") {
            float v;
            if (!parseFloat(val, v) || v < 0.0f || v > 1.0f)
                rejected[key] = "expected float in [0.0,1.0]";
            else { m_filter.setIntensityRatio(v); applied[key] = val; }

        } else if (key == "bnr-address") {
            rejected[key] = "bnr-address cannot be changed at runtime; reconnect to change server";

        } else {
            rejected[key] = "unknown parameter";
        }
    }

    return {applied, rejected};
}

std::vector<ParamDescriptor> BnrFilterWrapper::describe() const
{
    return {
        {"bnr-address", "string", "maxine-bnr:8001", "", "",    "NVIDIA Maxine NIM gRPC server address (set at session start only)", false},
        {"intensity",   "float",  "1.0",             "0", "1", "Noise suppression intensity ratio",                                  true},
    };
}

} // namespace ubersdr
