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
// All gRPC I/O runs on a dedicated worker thread to avoid blocking
// the audio callback. The audio thread pushes samples into an input
// buffer via process(), and reads denoised samples back.
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
    void workerLoop();

    std::shared_ptr<grpc::Channel> m_channel;
    std::unique_ptr<nvidia::maxine::bnr::v1::MaxineBNR::Stub> m_stub;
    std::unique_ptr<grpc::ClientContext> m_context;
    std::unique_ptr<grpc::ClientReaderWriter<
        nvidia::maxine::bnr::v1::EnhanceAudioRequest,
        nvidia::maxine::bnr::v1::EnhanceAudioResponse>> m_stream;

    // Worker thread handles all gRPC read/write
    std::thread m_workerThread;
    std::atomic<bool> m_connected{false};
    std::atomic<bool> m_stopping{false};
    std::atomic<bool> m_configDirty{false};

    // Input buffer: audio thread writes, worker thread reads
    std::mutex m_inMutex;
    std::vector<float> m_inBuf;

    // Output buffer: worker thread writes, audio thread reads
    std::mutex m_outMutex;
    std::vector<float> m_outBuf;

    static constexpr int kSampleRate   = 48000;
    static constexpr int kFrameSamples = 480;   // 10ms at 48kHz
    static constexpr int kFrameBytes   = kFrameSamples * sizeof(float);
#endif
};

} // namespace AetherSDR
