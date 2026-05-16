#include "Rn2FilterWrapper.h"

namespace ubersdr {

Rn2FilterWrapper::Rn2FilterWrapper() = default;

std::vector<float> Rn2FilterWrapper::process(const float* pcm, int stereoFrames)
{
    return m_filter.process(pcm, stereoFrames);
}

std::pair<ParamMap, ParamMap> Rn2FilterWrapper::applyParams(const ParamMap& in)
{
    ParamMap applied, rejected;
    for (const auto& [key, val] : in)
        rejected[key] = "rn2 has no tunable parameters";
    return {applied, rejected};
}

std::vector<ParamDescriptor> Rn2FilterWrapper::describe() const
{
    return {}; // no parameters
}

} // namespace ubersdr
