#pragma once

#include "IFilter.h"

#ifdef HAVE_SPECBLEACH
#  include "SpecbleachFilter.h"
#  include <memory>
#endif

namespace ubersdr {

// Wraps AetherSDR::SpecbleachFilter (NR4) for the gRPC server.
// All parameter setters use std::atomic + m_paramsDirty — safe to call
// from the stream reader thread while process() runs.
class Nr4FilterWrapper final : public IFilter {
public:
    Nr4FilterWrapper();
    ~Nr4FilterWrapper() override = default;

    std::vector<float> process(const float* pcm, int stereoFrames) override;
    std::pair<ParamMap, ParamMap> applyParams(const ParamMap& in) override;
    std::vector<ParamDescriptor> describe() const override;
    std::string name() const override { return "nr4"; }

#ifdef HAVE_SPECBLEACH
    bool isValid() const { return m_filter && m_filter->isValid(); }
private:
    std::unique_ptr<AetherSDR::SpecbleachFilter> m_filter;
#else
    bool isValid() const { return false; }
#endif
};

} // namespace ubersdr
