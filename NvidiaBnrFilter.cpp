#include "NvidiaBnrFilter.h"

#include <cstdio>
#include <cstring>
#include <chrono>

namespace AetherSDR {

NvidiaBnrFilter::NvidiaBnrFilter() = default;

NvidiaBnrFilter::~NvidiaBnrFilter()
{
    disconnect();
}

#ifdef HAVE_BNR

bool NvidiaBnrFilter::connectToServer(const std::string& address)
{
    if (m_connected.load()) disconnect();

    m_stopping.store(false);

    m_channel = grpc::CreateChannel(address, grpc::InsecureChannelCredentials());
    m_stub = nvidia::maxine::bnr::v1::MaxineBNR::NewStub(m_channel);

    m_context = std::make_unique<grpc::ClientContext>();
    m_stream = m_stub->EnhanceAudio(m_context.get());

    if (!m_stream) {
        fprintf(stderr, "NvidiaBnrFilter: failed to open gRPC stream to %s\n", address.c_str());
        if (onError) onError("Failed to open gRPC stream");
        return false;
    }

    // Send initial config
    nvidia::maxine::bnr::v1::EnhanceAudioRequest configReq;
    auto* config = configReq.mutable_config();
    config->set_intensity_ratio(m_intensityRatio);

    if (!m_stream->Write(configReq)) {
        fprintf(stderr, "NvidiaBnrFilter: failed to send config\n");
        m_stream.reset();
        m_context.reset();
        if (onError) onError("Failed to send configuration");
        return false;
    }

    m_connected.store(true);
    {
        std::lock_guard<std::mutex> lock(m_inMutex);
        m_inBuf.clear();
    }
    {
        std::lock_guard<std::mutex> lock(m_outMutex);
        m_outBuf.clear();
    }

    // Start worker thread (handles all gRPC read/write)
    m_workerThread = std::thread(&NvidiaBnrFilter::workerLoop, this);

    fprintf(stderr, "NvidiaBnrFilter: connected to %s, intensity: %.2f\n",
            address.c_str(), m_intensityRatio);
    if (onConnectionChanged) onConnectionChanged(true);
    return true;
}

void NvidiaBnrFilter::disconnect()
{
    if (!m_connected.load() && !m_workerThread.joinable()) return;

    m_stopping.store(true);
    m_connected.store(false);

    // Cancel the gRPC context to unblock any pending Read/Write in the worker
    if (m_context)
        m_context->TryCancel();

    if (m_workerThread.joinable())
        m_workerThread.join();

    if (m_stream)
        m_stream.reset();
    m_context.reset();

    {
        std::lock_guard<std::mutex> lock(m_inMutex);
        m_inBuf.clear();
    }
    {
        std::lock_guard<std::mutex> lock(m_outMutex);
        m_outBuf.clear();
    }

    fprintf(stderr, "NvidiaBnrFilter: disconnected\n");
    if (onConnectionChanged) onConnectionChanged(false);
}

bool NvidiaBnrFilter::isConnected() const
{
    return m_connected.load();
}

void NvidiaBnrFilter::setIntensityRatio(float ratio)
{
    m_intensityRatio = std::clamp(ratio, 0.0f, 1.0f);
    m_configDirty.store(true);
}

std::vector<float> NvidiaBnrFilter::process(const float* samples, int numSamples)
{
    if (!m_connected.load()) return {};

    // Non-blocking: push samples into input buffer for the worker thread
    {
        std::lock_guard<std::mutex> lock(m_inMutex);
        m_inBuf.insert(m_inBuf.end(), samples, samples + numSamples);

        // Cap input buffer at ~200ms to prevent runaway growth
        constexpr int maxInSamples = kFrameSamples * 20;
        if (static_cast<int>(m_inBuf.size()) > maxInSamples)
            m_inBuf.erase(m_inBuf.begin(),
                          m_inBuf.begin() + (m_inBuf.size() - maxInSamples));
    }

    // Non-blocking: return any denoised data from the worker thread
    std::lock_guard<std::mutex> lock(m_outMutex);
    if (m_outBuf.empty()) return {};

    std::vector<float> result;
    result.swap(m_outBuf);
    return result;
}

void NvidiaBnrFilter::workerLoop()
{
    while (!m_stopping.load()) {
        // Send config update if intensity changed
        if (m_configDirty.exchange(false)) {
            nvidia::maxine::bnr::v1::EnhanceAudioRequest configReq;
            auto* config = configReq.mutable_config();
            config->set_intensity_ratio(m_intensityRatio);
            m_stream->Write(configReq);
        }

        // Pull a frame from input buffer
        std::vector<float> frame;
        {
            std::lock_guard<std::mutex> lock(m_inMutex);
            if (static_cast<int>(m_inBuf.size()) >= kFrameSamples) {
                frame = std::vector<float>(m_inBuf.begin(), m_inBuf.begin() + kFrameSamples);
                m_inBuf.erase(m_inBuf.begin(), m_inBuf.begin() + kFrameSamples);
            }
        }

        if (frame.empty()) {
            // No data yet — sleep briefly to avoid busy-wait
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }

        // Write frame to gRPC stream
        nvidia::maxine::bnr::v1::EnhanceAudioRequest req;
        req.set_audio_stream_data(reinterpret_cast<const char*>(frame.data()), kFrameBytes);

        static int s_frameCount = 0;
        ++s_frameCount;
        if (s_frameCount <= 5 || s_frameCount % 500 == 0)
            fprintf(stderr, "NvidiaBnrFilter: writing frame %d (%d bytes)\n",
                    s_frameCount, kFrameBytes);

        if (!m_stream->Write(req)) {
            if (!m_stopping.load()) {
                fprintf(stderr, "NvidiaBnrFilter: gRPC write failed (frame %d)\n", s_frameCount);
                m_connected.store(false);
                if (onConnectionChanged) onConnectionChanged(false);
                if (onError) onError("gRPC write failed");
            }
            return;
        }

        if (s_frameCount <= 5 || s_frameCount % 500 == 0)
            fprintf(stderr, "NvidiaBnrFilter: write OK frame %d, waiting for Read...\n", s_frameCount);

        // Read denoised response (blocking, but on worker thread — not audio)
        nvidia::maxine::bnr::v1::EnhanceAudioResponse response;
        if (!m_stream->Read(&response)) {
            if (!m_stopping.load()) {
                fprintf(stderr, "NvidiaBnrFilter: gRPC read failed (frame %d)\n", s_frameCount);
                m_connected.store(false);
                if (onConnectionChanged) onConnectionChanged(false);
                if (onError) onError("BNR container stream ended");
            }
            return;
        }

        if (s_frameCount <= 5 || s_frameCount % 500 == 0)
            fprintf(stderr, "NvidiaBnrFilter: Read returned frame %d, has_audio=%d size=%zu\n",
                    s_frameCount,
                    response.has_audio_stream_data(),
                    response.has_audio_stream_data() ? response.audio_stream_data().size() : 0);

        if (response.has_audio_stream_data()) {
            const auto& data = response.audio_stream_data();
            const float* fptr = reinterpret_cast<const float*>(data.data());
            const int fcount = static_cast<int>(data.size()) / sizeof(float);

            std::lock_guard<std::mutex> lock(m_outMutex);
            m_outBuf.insert(m_outBuf.end(), fptr, fptr + fcount);

            // Cap output buffer at ~200ms
            constexpr int maxOutSamples = kFrameSamples * 20;
            if (static_cast<int>(m_outBuf.size()) > maxOutSamples)
                m_outBuf.erase(m_outBuf.begin(),
                               m_outBuf.begin() + (m_outBuf.size() - maxOutSamples));
        }
    }
}

#else // !HAVE_BNR — stub implementations

bool NvidiaBnrFilter::connectToServer(const std::string&) { return false; }
void NvidiaBnrFilter::disconnect() {}
bool NvidiaBnrFilter::isConnected() const { return false; }
std::vector<float> NvidiaBnrFilter::process(const float*, int) { return {}; }
void NvidiaBnrFilter::setIntensityRatio(float ratio)
{
    m_intensityRatio = std::clamp(ratio, 0.0f, 1.0f);
}

#endif // HAVE_BNR

} // namespace AetherSDR
