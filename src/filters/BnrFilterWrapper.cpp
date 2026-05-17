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
    // 1. Convert 24 kHz stereo → 48 kHz mono.
    //    24→48 kHz is an exact 2:1 ratio: duplicate each downmixed sample.
    //    No r8brain resampler — avoids startup delay that would stall the
    //    async NIM pipeline (worker thread would have nothing to send).
    std::vector<float> mono48k;
    mono48k.reserve(static_cast<size_t>(stereoFrames) * 2);
    for (int i = 0; i < stereoFrames; ++i) {
        const float s = (pcm[i * 2] + pcm[i * 2 + 1]) * 0.5f;  // L+R downmix
        mono48k.push_back(s);  // sample at t
        mono48k.push_back(s);  // duplicate for t+0.5 (nearest-neighbour 2:1)
    }

    // 2. Push 48 kHz mono samples into NvidiaBnrFilter (non-blocking).
    //    The filter's worker thread sends 480-sample frames to the NIM server
    //    and accumulates denoised responses in its output buffer.
    std::vector<float> denoised48k = m_filter.process(mono48k.data(),
                                                       static_cast<int>(mono48k.size()));

    // 3. Convert returned 48 kHz mono → 24 kHz stereo.
    //    48→24 kHz is an exact 2:1 ratio: average pairs of samples.
    //    No r8brain resampler — produces output immediately, no startup delay.
    if (!denoised48k.empty()) {
        const int n48 = static_cast<int>(denoised48k.size());
        for (int i = 0; i + 1 < n48; i += 2) {
            const float s = (denoised48k[i] + denoised48k[i + 1]) * 0.5f;
            m_outAccum.push_back(s);  // L
            m_outAccum.push_back(s);  // R
        }
    }

    // Infrequent diagnostic log — shows whether NIM is returning audio
    static int s_callCount = 0;
    if (++s_callCount <= 10 || s_callCount % 200 == 0) {
        fprintf(stderr, "BNR[%d]: in=%d mono48k=%zu nim_out=%zu accum=%zu\n",
                s_callCount, stereoFrames,
                mono48k.size(), denoised48k.size(), m_outAccum.size());
    }

    // 4. Return exactly stereoFrames * 2 samples when we have enough.
    if (static_cast<int>(m_outAccum.size()) >= needed) {
        std::vector<float> result(m_outAccum.begin(), m_outAccum.begin() + needed);
        m_outAccum.erase(m_outAccum.begin(), m_outAccum.begin() + needed);
        return result;
    }

    // Not enough output yet (NIM round-trip latency during startup).
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
