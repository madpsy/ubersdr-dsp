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
#ifndef HAVE_BNR
    return std::vector<float>(pcm, pcm + stereoFrames * 2);
#else
    // Downmix stereo → mono for BNR
    std::vector<float> mono(stereoFrames);
    for (int i = 0; i < stereoFrames; ++i)
        mono[i] = (pcm[i * 2] + pcm[i * 2 + 1]) * 0.5f;

    auto chunk = m_filter.process(mono.data(), stereoFrames);
    m_accumulated.insert(m_accumulated.end(), chunk.begin(), chunk.end());

    // Emit output when we have enough samples
    std::vector<float> out;
    while (static_cast<int>(m_accumulated.size()) >= stereoFrames) {
        for (int i = 0; i < stereoFrames; ++i) {
            out.push_back(m_accumulated[i]);
            out.push_back(m_accumulated[i]);
        }
        m_accumulated.erase(m_accumulated.begin(),
                            m_accumulated.begin() + stereoFrames);
    }

    // If we don't have enough output yet, return silence to maintain timing
    if (out.empty())
        out.assign(stereoFrames * 2, 0.0f);

    return out;
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
        {"bnr-address", "string", "localhost:8001", "", "",    "NVIDIA Maxine NIM gRPC server address (set at session start only)", false},
        {"intensity",   "float",  "1.0",            "0", "1", "Noise suppression intensity ratio",                                  true},
    };
}

} // namespace ubersdr
