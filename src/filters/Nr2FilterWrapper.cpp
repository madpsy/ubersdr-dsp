#include "Nr2FilterWrapper.h"

#include <algorithm>
#include <charconv>
#include <cstring>
#include <stdexcept>
#include <string>

namespace ubersdr {

// ── helpers ───────────────────────────────────────────────────────────────────

static bool parseFloat(const std::string& s, float& out)
{
    auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
    return ec == std::errc{};
}

static bool parseInt(const std::string& s, int& out)
{
    auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
    return ec == std::errc{};
}

static bool parseBool(const std::string& s, bool& out)
{
    if (s == "true"  || s == "1" || s == "on")  { out = true;  return true; }
    if (s == "false" || s == "0" || s == "off") { out = false; return true; }
    return false;
}

// ── Nr2FilterWrapper ──────────────────────────────────────────────────────────

Nr2FilterWrapper::Nr2FilterWrapper()
    : m_filter(256, 24000)
{}

std::vector<float> Nr2FilterWrapper::process(const float* pcm, int stereoFrames)
{
    m_monoIn.resize(stereoFrames);
    m_monoOut.resize(stereoFrames);

    // Stereo → mono (average L+R)
    for (int i = 0; i < stereoFrames; ++i)
        m_monoIn[i] = (pcm[i * 2] + pcm[i * 2 + 1]) * 0.5f;

    m_filter.process(m_monoIn.data(), m_monoOut.data(), stereoFrames);

    // Mono → stereo (duplicate)
    std::vector<float> out(stereoFrames * 2);
    for (int i = 0; i < stereoFrames; ++i) {
        out[i * 2]     = m_monoOut[i];
        out[i * 2 + 1] = m_monoOut[i];
    }
    return out;
}

std::pair<ParamMap, ParamMap> Nr2FilterWrapper::applyParams(const ParamMap& in)
{
    ParamMap applied, rejected;

    for (const auto& [key, val] : in) {
        if (key == "gain-method") {
            int v;
            if (!parseInt(val, v) || v < 0 || v > 3)
                rejected[key] = "expected int in [0,3]";
            else { m_filter.setGainMethod(v); applied[key] = val; }

        } else if (key == "npe-method") {
            int v;
            if (!parseInt(val, v) || v < 0 || v > 2)
                rejected[key] = "expected int in [0,2]";
            else { m_filter.setNpeMethod(v); applied[key] = val; }

        } else if (key == "gain-max") {
            float v;
            if (!parseFloat(val, v) || v < 0.0f || v > 2.0f)
                rejected[key] = "expected float in [0.0,2.0]";
            else { m_filter.setGainMax(v); applied[key] = val; }

        } else if (key == "gain-smooth") {
            float v;
            if (!parseFloat(val, v) || v < 0.0f || v > 1.0f)
                rejected[key] = "expected float in [0.0,1.0]";
            else { m_filter.setGainSmooth(v); applied[key] = val; }

        } else if (key == "qspp") {
            float v;
            if (!parseFloat(val, v) || v < 0.0f || v > 1.0f)
                rejected[key] = "expected float in [0.0,1.0]";
            else { m_filter.setQspp(v); applied[key] = val; }

        } else if (key == "ae") {
            bool v;
            if (!parseBool(val, v))
                rejected[key] = "expected bool (true/false/1/0/on/off)";
            else { m_filter.setAeFilter(v); applied[key] = val; }

        } else {
            rejected[key] = "unknown parameter";
        }
    }

    return {applied, rejected};
}

std::vector<ParamDescriptor> Nr2FilterWrapper::describe() const
{
    return {
        {"gain-method", "int",   "2",    "0", "3",   "Gain method: 0=Linear 1=Log 2=Gamma 3=Trained", true},
        {"npe-method",  "int",   "0",    "0", "2",   "NPE method: 0=OSMS 1=MMSE 2=NSTAT",             true},
        {"gain-max",    "float", "1.0",  "0", "2.0", "Max gain cap",                                   true},
        {"gain-smooth", "float", "0.85", "0", "1.0", "Temporal gain smoothing",                        true},
        {"qspp",        "float", "0.2",  "0", "1.0", "Speech presence probability prior",              true},
        {"ae",          "bool",  "true", "",  "",    "Artifact elimination post-processing",            true},
    };
}

} // namespace ubersdr
