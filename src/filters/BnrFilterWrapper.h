#pragma once

#include "IFilter.h"
#include "NvidiaBnrFilter.h"
#include <string>
#include <vector>

namespace ubersdr {

// Wraps AetherSDR::NvidiaBnrFilter (BNR) for the gRPC server.
// bnrAddress is passed from SessionConfig params["bnr-address"].
// The BNR filter itself is a gRPC client to the NVIDIA Maxine NIM server.
//
// NvidiaBnrFilter requires 48 kHz mono float32 input/output.
// Both 24→48 kHz and 48→24 kHz are exact 2:1 integer ratios, so this
// wrapper uses simple integer-ratio conversion with no r8brain resampler
// (avoiding any startup delay that would stall the async NIM pipeline):
//   Input:  24 kHz stereo → 48 kHz mono via sample duplication + L+R downmix
//   Output: 48 kHz mono → 24 kHz stereo via pair-averaging + L=R expand
class BnrFilterWrapper final : public IFilter {
public:
    explicit BnrFilterWrapper(const std::string& bnrAddress = "maxine-bnr:8001");
    ~BnrFilterWrapper() override;

    std::vector<float> process(const float* pcm, int stereoFrames) override;
    std::pair<ParamMap, ParamMap> applyParams(const ParamMap& in) override;
    std::vector<ParamDescriptor> describe() const override;
    std::string name() const override { return "bnr"; }

    bool isConnected() const { return m_filter.isConnected(); }

private:
    AetherSDR::NvidiaBnrFilter m_filter;

    // Output accumulator: holds denoised 24 kHz stereo samples
    std::vector<float> m_outAccum;
};

} // namespace ubersdr
