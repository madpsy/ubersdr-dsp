#ifdef HAVE_DFNR

#include "DeepFilterFilter.h"
#include "Resampler.h"
#include "deep_filter.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <vector>

namespace AetherSDR {

static constexpr const char* kModelFileName = "DeepFilterNet3_onnx.tar.gz";

// modelHint: explicit path supplied by the user (--model flag), or empty.
// Falls back to searching standard locations.
static std::string findModelPath(const std::string& modelHint = {})
{
    namespace fs = std::filesystem;

    auto check = [](const fs::path& p) -> std::string {
        if (fs::exists(p)) return p.string();
        return {};
    };

    if (!modelHint.empty()) {
        auto r = check(modelHint);
        if (!r.empty()) return r;
    }

    // 1. Current working directory
    if (auto r = check(fs::current_path() / kModelFileName); !r.empty()) return r;

    // 2. Executable directory (Linux: /proc/self/exe)
#ifdef __linux__
    {
        char buf[4096] = {};
        if (readlink("/proc/self/exe", buf, sizeof(buf) - 1) > 0) {
            fs::path exeDir = fs::path(buf).parent_path();
            if (auto r = check(exeDir / kModelFileName); !r.empty()) return r;
            if (auto r = check(exeDir / ".." / "third_party" / "deepfilter" / "models" / kModelFileName); !r.empty()) return r;
        }
    }
#endif

    // 3. XDG / system paths (Linux)
    for (const char* prefix : {"/usr/share/ubersdr-dsp", "/usr/local/share/ubersdr-dsp",
                                "/usr/share/AetherSDR",  "/usr/local/share/AetherSDR"}) {
        if (auto r = check(fs::path(prefix) / kModelFileName); !r.empty()) return r;
    }

    fprintf(stderr, "DeepFilterFilter: model '%s' not found. "
            "Pass --model <path> to specify its location.\n", kModelFileName);
    return {};
}

// Drain and print any pending log messages from the DeepFilter Rust runtime.
static void drainDfLogs(DFState* st)
{
    if (!st) return;
    char* msg;
    while ((msg = df_next_log_msg(st)) != nullptr) {
        fprintf(stderr, "DeepFilterFilter [rust]: %s\n", msg);
        df_free_log_msg(msg);
    }
}

DeepFilterFilter::DeepFilterFilter(const std::string& modelHint)
    : m_up(std::make_unique<Resampler>(24000, 48000))
    , m_down(std::make_unique<Resampler>(48000, 24000))
{
    std::string modelPath = findModelPath(modelHint);
    if (modelPath.empty()) {
        return;
    }
    fprintf(stderr, "DeepFilterFilter: loading model from %s\n", modelPath.c_str());
    m_state = df_create(modelPath.c_str(), m_attenLimit.load(), "warn");
    if (m_state) {
        m_frameSize = static_cast<int>(df_get_frame_length(m_state));
        drainDfLogs(m_state);
        fprintf(stderr, "DeepFilterFilter: initialized, frame size = %d\n", m_frameSize);
    } else {
        fprintf(stderr, "DeepFilterFilter: df_create() failed! "
                "(path=%s, check libgcc_s.so.1 is present and model file is readable)\n",
                modelPath.c_str());
    }
}

DeepFilterFilter::~DeepFilterFilter()
{
    if (m_state) {
        df_free(m_state);
    }
}

void DeepFilterFilter::reset()
{
    if (m_state) {
        df_free(m_state);
        m_state = nullptr;
    }
    std::string modelPath = findModelPath();
    if (!modelPath.empty()) {
        m_state = df_create(modelPath.c_str(), m_attenLimit.load(), nullptr);
        if (m_state) {
            m_frameSize = static_cast<int>(df_get_frame_length(m_state));
        }
    }
    m_up = std::make_unique<Resampler>(24000, 48000);
    m_down = std::make_unique<Resampler>(48000, 24000);
    m_inAccum.clear();
    m_outAccum.clear();
    m_paramsDirty.store(true);
}

void DeepFilterFilter::setAttenLimit(float db)
{
    m_attenLimit.store(db);
    m_paramsDirty.store(true);
}

void DeepFilterFilter::setPostFilterBeta(float beta)
{
    m_postFilterBeta.store(beta);
    m_paramsDirty.store(true);
}

std::vector<float> DeepFilterFilter::process(const float* stereoIn, int numStereoFrames)
{
    const int needed = numStereoFrames * 2;

    if (!m_state || m_frameSize <= 0 || numStereoFrames <= 0) {
        return std::vector<float>(stereoIn, stereoIn + needed);
    }

    // Apply any pending parameter changes
    if (m_paramsDirty.exchange(false)) {
        df_set_atten_lim(m_state, m_attenLimit.load());
        df_set_post_filter_beta(m_state, m_postFilterBeta.load());
    }

    // 1. Upsample 24kHz stereo float32 → 48kHz mono float32 via r8brain
    std::vector<float> mono48k = m_up->processStereoToMono(stereoIn, numStereoFrames);
    const int monoSamples48k = static_cast<int>(mono48k.size());

    // 2. Append to input accumulator and process complete frames
    const int prevAccumSamples = static_cast<int>(m_inAccum.size());
    m_inAccum.resize(prevAccumSamples + monoSamples48k);
    for (int i = 0; i < monoSamples48k; ++i)
        m_inAccum[prevAccumSamples + i] = mono48k[i];

    const int totalAccumSamples = prevAccumSamples + monoSamples48k;
    const int completeFrames = totalAccumSamples / m_frameSize;

    if (completeFrames > 0) {
        std::vector<float> processed48k(completeFrames * m_frameSize);

        for (int f = 0; f < completeFrames; ++f) {
            df_process_frame(m_state,
                             &m_inAccum[f * m_frameSize],
                             &processed48k[f * m_frameSize]);
        }

        // Keep leftover input samples
        const int consumedSamples = completeFrames * m_frameSize;
        const int leftoverSamples = totalAccumSamples - consumedSamples;
        if (leftoverSamples > 0)
            m_inAccum = std::vector<float>(m_inAccum.begin() + consumedSamples, m_inAccum.end());
        else
            m_inAccum.clear();

        // 3. Downsample processed 48kHz mono float32 → 24kHz stereo float32
        std::vector<float> downsampled = m_down->processMonoToStereo(
            processed48k.data(), completeFrames * m_frameSize);

        m_outAccum.insert(m_outAccum.end(), downsampled.begin(), downsampled.end());
    }

    // 4. Return exactly the same number of samples as input (stereo)
    if (static_cast<int>(m_outAccum.size()) >= needed) {
        std::vector<float> result(m_outAccum.begin(), m_outAccum.begin() + needed);
        m_outAccum.erase(m_outAccum.begin(), m_outAccum.begin() + needed);
        return result;
    }

    // Not enough output yet — return silence (only happens during startup)
    return std::vector<float>(needed, 0.0f);
}

} // namespace AetherSDR

#endif // HAVE_DFNR
