#pragma once

#include "IFilter.h"
#include "NvidiaBnrFilter.h"
#include "Resampler.h"
#include <memory>
#include <string>
#include <vector>

namespace ubersdr {

// Wraps AetherSDR::NvidiaBnrFilter (BNR) for the gRPC server.
// bnrAddress is passed from SessionConfig params["bnr-address"].
// The BNR filter itself is a gRPC client to the NVIDIA Maxine NIM server.
//
// NvidiaBnrFilter requires 48 kHz mono float32 input/output.
// This wrapper handles the 24 kHz stereo ↔ 48 kHz mono conversion
// using r8brain resamplers, mirroring the pattern used by DeepFilterFilter.
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
    AetherSDR::NvidiaBnrFilter      m_filter;

    // Resamplers for 24 kHz stereo ↔ 48 kHz mono conversion
    std::unique_ptr<AetherSDR::Resampler> m_up;    // 24 kHz stereo → 48 kHz mono
    std::unique_ptr<AetherSDR::Resampler> m_down;  // 48 kHz mono → 24 kHz stereo

    // Output accumulator: holds denoised 24 kHz stereo samples
    // until we have enough to return exactly stereoFrames * 2 samples.
    std::vector<float> m_outAccum;
};

} // namespace ubersdr
