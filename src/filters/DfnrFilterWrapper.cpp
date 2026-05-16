#include "DfnrFilterWrapper.h"

#include <charconv>
#include <string>

namespace ubersdr {

static bool parseFloat(const std::string& s, float& out)
{
    auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
    return ec == std::errc{};
}

// ── DfnrFilterWrapper ─────────────────────────────────────────────────────────

DfnrFilterWrapper::DfnrFilterWrapper(const std::string& modelHint)
{
#ifdef HAVE_DFNR
    m_filter = std::make_unique<AetherSDR::DeepFilterFilter>(modelHint);
#else
    (void)modelHint;
#endif
}

std::vector<float> DfnrFilterWrapper::process(const float* pcm, int stereoFrames)
{
#ifdef HAVE_DFNR
    if (m_filter && m_filter->isValid())
        return m_filter->process(pcm, stereoFrames);
#endif
    return std::vector<float>(pcm, pcm + stereoFrames * 2);
}

std::pair<ParamMap, ParamMap> DfnrFilterWrapper::applyParams(const ParamMap& in)
{
    ParamMap applied, rejected;

#ifndef HAVE_DFNR
    for (const auto& [key, val] : in)
        rejected[key] = "dfnr not compiled in";
    return {applied, rejected};
#else
    if (!m_filter || !m_filter->isValid()) {
        for (const auto& [key, val] : in)
            rejected[key] = "filter not initialised";
        return {applied, rejected};
    }

    for (const auto& [key, val] : in) {
        if (key == "atten-limit") {
            float v;
            if (!parseFloat(val, v) || v < 0.0f || v > 100.0f)
                rejected[key] = "expected float in [0,100]";
            else { m_filter->setAttenLimit(v); applied[key] = val; }

        } else if (key == "pf-beta") {
            float v;
            if (!parseFloat(val, v) || v < 0.0f || v > 0.3f)
                rejected[key] = "expected float in [0,0.3]";
            else { m_filter->setPostFilterBeta(v); applied[key] = val; }

        } else if (key == "model") {
            // model path cannot be changed at runtime (requires re-init)
            rejected[key] = "model path cannot be changed at runtime; reconnect to change model";

        } else {
            rejected[key] = "unknown parameter";
        }
    }

    return {applied, rejected};
#endif
}

std::vector<ParamDescriptor> DfnrFilterWrapper::describe() const
{
    return {
        {"model",       "string", "",    "",  "",    "Path to DeepFilterNet3_onnx.tar.gz (set at session start only)", false},
        {"atten-limit", "float",  "100", "0", "100", "Attenuation limit in dB",                                        true},
        {"pf-beta",     "float",  "0",   "0", "0.3", "Post-filter beta",                                               true},
    };
}

} // namespace ubersdr
