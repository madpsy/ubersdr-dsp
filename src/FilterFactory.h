#pragma once

#include "filters/IFilter.h"
#include <memory>
#include <string>

// Forward-declare the proto type to avoid pulling in generated headers here.
namespace ubersdr::dsp::v1 { class SessionConfig; }

namespace ubersdr {

// Creates the appropriate IFilter subclass from a SessionConfig message.
// Returns nullptr if the filter name is unknown or initialisation fails.
// errOut receives a human-readable error message on failure.
std::unique_ptr<IFilter> createFilter(const dsp::v1::SessionConfig& cfg,
                                      std::string& errOut);

} // namespace ubersdr
