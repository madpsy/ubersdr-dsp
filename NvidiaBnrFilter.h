#pragma once

#include <functional>
#include <mutex>
#include <string>
#include <vector>

#ifdef HAVE_BNR
#include <memory>
#include <thread>
#include <atomic>
#include <condition_variable>
#include <grpcpp/grpcpp.h>
#include "bnr.grpc.pb.h"
#endif

namespace AetherSDR {

// gRPC client for NVIDIA NIM BNR (Background Noise Removal).
// All gRPC I/O runs on a dedicated worker thread to avoid blocking
// the audio callback. The audio thread pushes samples into an input
// buffer via process(), and reads denoised samples back.
//
// Three worker modes (selected at runtime via BNR_WORKER_MODE env var):
//
//   batch (default, BNR_WORKER_MODE unset or "batch"):
//     Accumulates kBatchFrames (200ms) of audio, then opens a new gRPC
//     stream, sends all frames, calls WritesDone(), drains all responses,
//     and closes the stream. Repeats for the next batch.
//     Matches the official NIM Python client's protocol exactly.
//     Introduces ~100ms average latency (half the 200ms batch window).
//
//   concurrent (BNR_WORKER_MODE=concurrent):
//     Two threads — one sends frames, one reads responses concurrently
//     on a persistent stream. Does NOT call WritesDone(). NIM returns
//     empty responses in this mode (kept for debugging).
//
//   legacy (BNR_WORKER_MODE=legacy):
//     Single thread — alternating Write→Read per frame on a persistent
//     stream. Original behaviour (kept for debugging).
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
    enum class WorkerMode { Batch, Concurrent, Legacy };

    void batchLoop();    // default: per-batch stream open/send/WritesDone/recv/close
    void sendLoop();     // concurrent: dedicated send thread (persistent stream)
    void recvLoop();     // concurrent: dedicated recv thread (persistent stream)
    void workerLoop();   // legacy: single thread, alternating Write→Read

    // Helper used by batchLoop to open a fresh stream and send the initial config.
    // Returns nullptr on failure.
    std::unique_ptr<grpc::ClientReaderWriter<
        nvidia::maxine::bnr::v1::EnhanceAudioRequest,
        nvidia::maxine::bnr::v1::EnhanceAudioResponse>>
    openStream(grpc::ClientContext& ctx);

    std::shared_ptr<grpc::Channel> m_channel;
    std::unique_ptr<nvidia::maxine::bnr::v1::MaxineBNR::Stub> m_stub;

    // Used by concurrent and legacy modes (persistent stream)
    std::unique_ptr<grpc::ClientContext> m_context;
    std::unique_ptr<grpc::ClientReaderWriter<
        nvidia::maxine::bnr::v1::EnhanceAudioRequest,
        nvidia::maxine::bnr::v1::EnhanceAudioResponse>> m_stream;

    std::thread m_batchThread;    // batch mode
    std::thread m_workerThread;   // legacy mode
    std::thread m_sendThread;     // concurrent mode
    std::thread m_recvThread;     // concurrent mode

    std::atomic<bool> m_connected{false};
    std::atomic<bool> m_stopping{false};
    std::atomic<bool> m_configDirty{false};
    WorkerMode m_workerMode{WorkerMode::Batch};

    // Input buffer: audio thread writes, worker thread reads
    std::mutex m_inMutex;
    std::condition_variable m_inCv;
    std::vector<float> m_inBuf;

    // Output buffer: worker thread writes, audio thread reads
    std::mutex m_outMutex;
    std::vector<float> m_outBuf;

    static constexpr int kSampleRate   = 48000;
    static constexpr int kFrameSamples = 480;    // 10ms at 48kHz
    static constexpr int kFrameBytes   = kFrameSamples * sizeof(float);
    static constexpr int kBatchFrames  = 20;     // 200ms batch (20 × 10ms frames)
#endif
};

} // namespace AetherSDR
