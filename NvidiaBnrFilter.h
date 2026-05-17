#pragma once

#include <functional>
#include <mutex>
#include <string>
#include <vector>

#ifdef HAVE_BNR
#include <memory>
#include <thread>
#include <atomic>
#include <grpcpp/grpcpp.h>
#include "bnr.grpc.pb.h"
#endif

namespace AetherSDR {

// gRPC client for NVIDIA NIM BNR (Background Noise Removal).
// All gRPC I/O runs on dedicated worker thread(s) to avoid blocking
// the audio callback. The audio thread pushes samples into an input
// buffer via process(), and reads denoised samples back.
//
// Two worker modes (selected at runtime via BNR_LEGACY_MODE env var):
//
//   Default (BNR_LEGACY_MODE unset or 0):
//     Two threads — one sends frames, one reads responses concurrently.
//     Matches the official NIM Python client's concurrent send/receive pattern.
//
//   Legacy (BNR_LEGACY_MODE=1):
//     Single thread — alternating Write→Read per frame (original behaviour).
//     May cause NIM to return empty responses if it buffers frames internally.
//
// When built without HAVE_BNR, all methods are no-ops.
class NvidiaBnrFilter {
public:
    NvidiaBnrFilter();
    ~NvidiaBnrFilter();

    bool connectToServer(const std::string& address = "localhost:8001");
    void disconnect();
    bool isConnected() const;

    // Non-blocking: pushes samples into input buffer, returns any available
    // denoised samples. Both input and output are 48kHz mono float32.
    std::vector<float> process(const float* samples, int numSamples);

    void setIntensityRatio(float ratio);
    float intensityRatio() const { return m_intensityRatio; }

    // Optional callbacks (set before connectToServer)
    std::function<void(bool)>              onConnectionChanged;
    std::function<void(const std::string&)> onError;

private:
    float m_intensityRatio{1.0f};

#ifdef HAVE_BNR
    // ── shared by both modes ──────────────────────────────────────────────────
    void workerLoop();       // legacy: single thread, alternating Write→Read
    void sendLoop();         // default: dedicated send thread
    void recvLoop();         // default: dedicated receive thread

    std::shared_ptr<grpc::Channel> m_channel;
    std::unique_ptr<nvidia::maxine::bnr::v1::MaxineBNR::Stub> m_stub;
    std::unique_ptr<grpc::ClientContext> m_context;
    std::unique_ptr<grpc::ClientReaderWriter<
        nvidia::maxine::bnr::v1::EnhanceAudioRequest,
        nvidia::maxine::bnr::v1::EnhanceAudioResponse>> m_stream;

    std::thread m_workerThread;   // legacy mode
    std::thread m_sendThread;     // default mode
    std::thread m_recvThread;     // default mode

    std::atomic<bool> m_connected{false};
    std::atomic<bool> m_stopping{false};
    std::atomic<bool> m_configDirty{false};
    bool m_legacyMode{false};     // set from BNR_LEGACY_MODE env var at connect time

    // Input buffer: audio thread writes, send thread reads
    std::mutex m_inMutex;
    std::vector<float> m_inBuf;

    // Output buffer: recv thread writes, audio thread reads
    std::mutex m_outMutex;
    std::vector<float> m_outBuf;

    static constexpr int kSampleRate   = 48000;
    static constexpr int kFrameSamples = 480;   // 10ms at 48kHz
    static constexpr int kFrameBytes   = kFrameSamples * sizeof(float);
#endif
};

} // namespace AetherSDR
