#pragma once

#include "IFilter.h"
#include "SpectralNR.h"
#include <memory>
#include <vector>

namespace ubersdr {

// Wraps AetherSDR::SpectralNR (NR2) for the gRPC server.
// All parameter setters are std::atomic — safe to call from the stream
// reader thread while process() runs.
class Nr2FilterWrapper final : public IFilter {
public:
    Nr2FilterWrapper();
    ~Nr2FilterWrapper() override = default;

    std::vector<float> process(const float* pcm, int stereoFrames) override;
    std::pair<ParamMap, ParamMap> applyParams(const ParamMap& in) override;
    std::vector<ParamDescriptor> describe() const override;
    std::string name() const override { return "nr2"; }

private:
    AetherSDR::SpectralNR m_filter;
    std::vector<float>    m_monoIn;
    std::vector<float>    m_monoOut;
    // Output FIFO: absorbs the OLA write/read imbalance that occurs when the
    // packet size is not a multiple of hopSize (128).  Without this, SpectralNR
    // reads unwritten (zero) positions from its ring buffer on every packet,
    // producing periodic crackling at a constant rate.  Same pattern as
    // RNNoiseFilter::m_outAccum.
    std::vector<float>    m_outAccum;
};

} // namespace ubersdr
