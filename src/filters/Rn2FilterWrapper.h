#pragma once

#include "IFilter.h"
#include "RNNoiseFilter.h"
#include <memory>

namespace ubersdr {

// Wraps AetherSDR::RNNoiseFilter (RN2) for the gRPC server.
// RNNoise has no tunable parameters; applyParams() rejects everything.
class Rn2FilterWrapper final : public IFilter {
public:
    Rn2FilterWrapper();
    ~Rn2FilterWrapper() override = default;

    std::vector<float> process(const float* pcm, int stereoFrames) override;
    std::pair<ParamMap, ParamMap> applyParams(const ParamMap& in) override;
    std::vector<ParamDescriptor> describe() const override;
    std::string name() const override { return "rn2"; }

    bool isValid() const { return m_filter.isValid(); }

private:
    AetherSDR::RNNoiseFilter m_filter;
};

} // namespace ubersdr
