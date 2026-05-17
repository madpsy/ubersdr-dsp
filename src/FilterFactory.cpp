#include "FilterFactory.h"

#include "filters/Nr2FilterWrapper.h"
#include "filters/Rn2FilterWrapper.h"
#include "filters/Nr4FilterWrapper.h"
#include "filters/DfnrFilterWrapper.h"

// Generated protobuf header
#include "ubersdr_dsp.pb.h"

#include <cstdio>

namespace ubersdr {

// Helper: look up a string param from the SessionConfig params map.
static std::string getParam(const dsp::v1::SessionConfig& cfg,
                            const std::string& key,
                            const std::string& def = {})
{
    const auto& m = cfg.params();
    auto it = m.find(key);
    return (it != m.end()) ? it->second : def;
}

std::unique_ptr<IFilter> createFilter(const dsp::v1::SessionConfig& cfg,
                                      std::string& errOut)
{
    const std::string& filterName = cfg.filter();

    // ── NR2 ──────────────────────────────────────────────────────────────────
    if (filterName == "nr2") {
        auto f = std::make_unique<Nr2FilterWrapper>();

        // Apply initial params from SessionConfig (ignore errors — they'll be
        // reported back via the normal ParamAck path on the first update).
        ParamMap initial(cfg.params().begin(), cfg.params().end());
        if (!initial.empty())
            f->applyParams(initial);

        return f;
    }

    // ── RN2 ──────────────────────────────────────────────────────────────────
    if (filterName == "rn2") {
        auto f = std::make_unique<Rn2FilterWrapper>();
        if (!f->isValid()) {
            errOut = "rn2: rnnoise_create() failed";
            return nullptr;
        }
        return f;
    }

    // ── NR4 ──────────────────────────────────────────────────────────────────
    if (filterName == "nr4") {
        auto f = std::make_unique<Nr4FilterWrapper>();
        if (!f->isValid()) {
            errOut = "nr4: specbleach_initialize() failed (not compiled in or init error)";
            return nullptr;
        }
        ParamMap initial(cfg.params().begin(), cfg.params().end());
        if (!initial.empty())
            f->applyParams(initial);
        return f;
    }

    // ── DFNR ─────────────────────────────────────────────────────────────────
    if (filterName == "dfnr") {
        std::string modelHint = getParam(cfg, "model");
        auto f = std::make_unique<DfnrFilterWrapper>(modelHint);
        if (!f->isValid()) {
            errOut = "dfnr: failed to load DeepFilterNet3 model"
                     " (not compiled in, or model path invalid)";
            return nullptr;
        }
        // Apply remaining params (skip "model" — already consumed above)
        ParamMap initial(cfg.params().begin(), cfg.params().end());
        initial.erase("model");
        if (!initial.empty())
            f->applyParams(initial);
        return f;
    }

    errOut = "unknown filter '" + filterName + "'; valid: nr2 rn2 nr4 dfnr";
    return nullptr;
}

} // namespace ubersdr
