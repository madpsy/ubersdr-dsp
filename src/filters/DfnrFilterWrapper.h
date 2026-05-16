#pragma once

#include "IFilter.h"

#ifdef HAVE_DFNR
#  include "DeepFilterFilter.h"
#  include <memory>
#endif

namespace ubersdr {

// Wraps AetherSDR::DeepFilterFilter (DFNR) for the gRPC server.
// modelHint is passed from SessionConfig params["model"] if present.
class DfnrFilterWrapper final : public IFilter {
public:
    explicit DfnrFilterWrapper(const std::string& modelHint = {});
    ~DfnrFilterWrapper() override = default;

    std::vector<float> process(const float* pcm, int stereoFrames) override;
    std::pair<ParamMap, ParamMap> applyParams(const ParamMap& in) override;
    std::vector<ParamDescriptor> describe() const override;
    std::string name() const override { return "dfnr"; }

#ifdef HAVE_DFNR
    bool isValid() const { return m_filter && m_filter->isValid(); }
private:
    std::unique_ptr<AetherSDR::DeepFilterFilter> m_filter;
#else
    bool isValid() const { return false; }
#endif
};

} // namespace ubersdr
