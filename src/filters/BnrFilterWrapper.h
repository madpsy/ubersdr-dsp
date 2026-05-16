#pragma once

#include "IFilter.h"
#include "NvidiaBnrFilter.h"
#include <string>
#include <vector>

namespace ubersdr {

// Wraps AetherSDR::NvidiaBnrFilter (BNR) for the gRPC server.
// bnrAddress is passed from SessionConfig params["bnr-address"].
// The BNR filter itself is a gRPC client to the NVIDIA Maxine NIM server.
class BnrFilterWrapper final : public IFilter {
public:
    explicit BnrFilterWrapper(const std::string& bnrAddress = "localhost:8001");
    ~BnrFilterWrapper() override;

    std::vector<float> process(const float* pcm, int stereoFrames) override;
    std::pair<ParamMap, ParamMap> applyParams(const ParamMap& in) override;
    std::vector<ParamDescriptor> describe() const override;
    std::string name() const override { return "bnr"; }

    bool isConnected() const { return m_filter.isConnected(); }

private:
    AetherSDR::NvidiaBnrFilter m_filter;
    int                        m_blockFrames{960};
    std::vector<float>         m_accumulated;
};

} // namespace ubersdr
