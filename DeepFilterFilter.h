#pragma once

#ifdef HAVE_DFNR

#include <atomic>
#include <memory>
#include <string>
#include <vector>

struct DFState;

namespace AetherSDR {

// Locate the DeepFilterNet3_onnx.tar.gz model file.
// Searches (in order): modelHint (if non-empty), CWD, executable directory,
// standard system paths.  Returns empty string if not found.
std::string findModelPath(const std::string& modelHint = {});

class Resampler;

// Client-side neural noise reduction using DeepFilterNet3.
// Processes 24kHz stereo float32 audio by upsampling to 48kHz mono,
// running the DeepFilter model, and downsampling back to 24kHz stereo.
//
// DeepFilterNet expects 48kHz mono float [-1.0, 1.0] input.
// Frame size determined at runtime via df_get_frame_length().
// Thread-safe parameter setters (main thread writes, audio thread reads).

class DeepFilterFilter {
public:
    // modelHint: explicit path to DeepFilterNet3_onnx.tar.gz, or empty to auto-search.
    explicit DeepFilterFilter(const std::string& modelHint = {});
    ~DeepFilterFilter();

    DeepFilterFilter(const DeepFilterFilter&) = delete;
    DeepFilterFilter& operator=(const DeepFilterFilter&) = delete;

    // Process a block of 24kHz stereo float32 PCM.
    // Returns the processed block (same format, same size).
    std::vector<float> process(const float* stereoIn, int numStereoFrames);

    // Returns true if df_create() succeeded.
    bool isValid() const { return m_state != nullptr; }

    // Reset internal state (e.g., on band change).
    void reset();

    // Attenuation limit in dB (0 = passthrough, 100 = max removal)
    void setAttenLimit(float db);
    float attenLimit() const { return m_attenLimit.load(); }

    // Post-filter beta (0 = disabled, 0.05–0.3 typical)
    void setPostFilterBeta(float beta);
    float postFilterBeta() const { return m_postFilterBeta.load(); }

private:
    DFState* m_state{nullptr};
    bool     m_fromPool{false};             // true → return via DFStatePool on destruction
    int m_frameSize{0};                     // samples per frame (from df_get_frame_length)
    std::unique_ptr<Resampler> m_up;        // 24kHz mono → 48kHz mono
    std::unique_ptr<Resampler> m_down;      // 48kHz mono → 24kHz mono
    std::vector<float> m_inAccum;           // accumulate 48kHz mono float input
    std::vector<float> m_outAccum;          // accumulate 24kHz stereo float32 output
    std::atomic<float> m_attenLimit{20.0f};
    std::atomic<float> m_postFilterBeta{0.0f};
    std::atomic<bool>  m_paramsDirty{false};
};

} // namespace AetherSDR

#endif // HAVE_DFNR
