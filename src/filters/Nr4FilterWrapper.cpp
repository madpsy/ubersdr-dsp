#include "Nr4FilterWrapper.h"

#include <charconv>
#include <string>

namespace ubersdr {

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

// ── Nr4FilterWrapper ──────────────────────────────────────────────────────────

Nr4FilterWrapper::Nr4FilterWrapper()
{
#ifdef HAVE_SPECBLEACH
    m_filter = std::make_unique<AetherSDR::SpecbleachFilter>();
#endif
}

std::vector<float> Nr4FilterWrapper::process(const float* pcm, int stereoFrames)
{
#ifdef HAVE_SPECBLEACH
    if (m_filter && m_filter->isValid())
        return m_filter->process(pcm, stereoFrames);
#endif
    // Passthrough if not compiled in
    return std::vector<float>(pcm, pcm + stereoFrames * 2);
}

std::pair<ParamMap, ParamMap> Nr4FilterWrapper::applyParams(const ParamMap& in)
{
    ParamMap applied, rejected;

#ifndef HAVE_SPECBLEACH
    for (const auto& [key, val] : in)
        rejected[key] = "nr4 not compiled in";
    return {applied, rejected};
#else
    if (!m_filter || !m_filter->isValid()) {
        for (const auto& [key, val] : in)
            rejected[key] = "filter not initialised";
        return {applied, rejected};
    }

    for (const auto& [key, val] : in) {
        if (key == "reduction") {
            float v;
            if (!parseFloat(val, v) || v < 0.0f || v > 40.0f)
                rejected[key] = "expected float in [0,40]";
            else { m_filter->setReductionAmount(v); applied[key] = val; }

        } else if (key == "smoothing") {
            float v;
            if (!parseFloat(val, v) || v < 0.0f || v > 100.0f)
                rejected[key] = "expected float in [0,100]";
            else { m_filter->setSmoothingFactor(v); applied[key] = val; }

        } else if (key == "whitening") {
            float v;
            if (!parseFloat(val, v) || v < 0.0f || v > 100.0f)
                rejected[key] = "expected float in [0,100]";
            else { m_filter->setWhiteningFactor(v); applied[key] = val; }

        } else if (key == "adaptive") {
            bool v;
            if (!parseBool(val, v))
                rejected[key] = "expected bool (true/false/1/0/on/off)";
            else { m_filter->setAdaptiveNoise(v); applied[key] = val; }

        } else if (key == "noise-method") {
            int v;
            if (!parseInt(val, v) || v < 0 || v > 2)
                rejected[key] = "expected int in [0,2]";
            else { m_filter->setNoiseEstimationMethod(v); applied[key] = val; }

        } else {
            rejected[key] = "unknown parameter";
        }
    }

    return {applied, rejected};
#endif
}

std::vector<ParamDescriptor> Nr4FilterWrapper::describe() const
{
    return {
        {"reduction",    "float", "10",   "0",   "40",  "Noise reduction amount in dB",                    true},
        {"smoothing",    "float", "0",    "0",   "100", "Smoothing factor in %",                           true},
        {"whitening",    "float", "0",    "0",   "100", "Whitening factor in %",                           true},
        {"adaptive",     "bool",  "true", "",    "",    "Enable adaptive noise tracking",                  true},
        {"noise-method", "int",   "0",    "0",   "2",   "Noise estimator: 0=SPP-MMSE 1=Brandt 2=Martin",  true},
    };
}

} // namespace ubersdr
