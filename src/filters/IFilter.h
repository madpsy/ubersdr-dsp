#pragma once

#include <map>
#include <string>
#include <utility>
#include <vector>

namespace ubersdr {

using ParamMap = std::map<std::string, std::string>;

struct ParamDescriptor {
    std::string name;
    std::string type;        // "float" | "int" | "bool"
    std::string default_val;
    std::string min_val;
    std::string max_val;
    std::string description;
    bool        runtime_safe{true};
};

// Abstract interface for all noise-reduction filter wrappers.
// Each concrete wrapper holds one filter instance and is owned by a single
// gRPC stream — no sharing between sessions.
class IFilter {
public:
    virtual ~IFilter() = default;

    // Process one chunk of 24 kHz stereo float32 PCM.
    // stereoFrames: number of L/R frame pairs (pcm length = stereoFrames * 2 floats).
    // Returns processed audio in the same format.
    virtual std::vector<float> process(const float* pcm, int stereoFrames) = 0;

    // Apply a map of string→string parameter updates.
    // Returns {applied, rejected} maps.
    // applied:  params that were successfully set (key → value actually applied)
    // rejected: params that failed (key → human-readable reason)
    virtual std::pair<ParamMap, ParamMap> applyParams(const ParamMap& in) = 0;

    // Describe all tunable parameters for this filter (used by GetFilters RPC).
    virtual std::vector<ParamDescriptor> describe() const = 0;

    // Human-readable filter name (e.g. "nr2").
    virtual std::string name() const = 0;
};

} // namespace ubersdr
