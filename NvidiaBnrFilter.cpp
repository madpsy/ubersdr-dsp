#include "NvidiaBnrFilter.h"

#include <cstdio>
#include <cstdlib>
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

    // Select worker mode from environment variable.
    // Default: batch mode — matches the official NIM Python client protocol.
    const char* modeEnv = std::getenv("BNR_WORKER_MODE");
    if (modeEnv && std::string(modeEnv) == "concurrent") {
        m_workerMode = WorkerMode::Concurrent;
        fprintf(stderr, "NvidiaBnrFilter: using concurrent (send/recv threads) worker mode\n");
    } else if (modeEnv && std::string(modeEnv) == "legacy") {
        m_workerMode = WorkerMode::Legacy;
        fprintf(stderr, "NvidiaBnrFilter: using legacy (alternating Write/Read) worker mode\n");
    } else {
        m_workerMode = WorkerMode::Batch;
        fprintf(stderr, "NvidiaBnrFilter: using batch worker mode (200ms batches + WritesDone)\n");
    }

    m_stopping.store(false);

    m_channel = grpc::CreateChannel(address, grpc::InsecureChannelCredentials());
    m_stub = nvidia::maxine::bnr::v1::MaxineBNR::NewStub(m_channel);

    if (m_workerMode == WorkerMode::Batch) {
        // Batch mode: batchLoop opens/closes its own streams per batch.
        // No persistent stream needed here.
        m_connected.store(true);
        {
            std::lock_guard<std::mutex> lock(m_inMutex);
            m_inBuf.clear();
        }
        {
            std::lock_guard<std::mutex> lock(m_outMutex);
            m_outBuf.clear();
        }
        m_batchThread = std::thread(&NvidiaBnrFilter::batchLoop, this);
    } else {
        // Concurrent / legacy: open a persistent stream now.
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

        if (m_workerMode == WorkerMode::Concurrent) {
            m_sendThread = std::thread(&NvidiaBnrFilter::sendLoop, this);
            m_recvThread = std::thread(&NvidiaBnrFilter::recvLoop, this);
        } else {
            m_workerThread = std::thread(&NvidiaBnrFilter::workerLoop, this);
        }
    }

    fprintf(stderr, "NvidiaBnrFilter: connected to %s, intensity: %.2f\n",
            address.c_str(), m_intensityRatio);
    if (onConnectionChanged) onConnectionChanged(true);
    return true;
}

void NvidiaBnrFilter::disconnect()
{
    if (!m_connected.load() &&
        !m_batchThread.joinable() &&
        !m_workerThread.joinable() &&
        !m_sendThread.joinable() &&
        !m_recvThread.joinable()) return;

    m_stopping.store(true);
    m_connected.store(false);

    // Wake up batchLoop if it's waiting for input
    m_inCv.notify_all();

    // Cancel the persistent gRPC context (concurrent/legacy modes) to unblock Read/Write
    if (m_context)
        m_context->TryCancel();

    if (m_batchThread.joinable())
        m_batchThread.join();
    if (m_workerThread.joinable())
        m_workerThread.join();
    if (m_sendThread.joinable())
        m_sendThread.join();
    if (m_recvThread.joinable())
        m_recvThread.join();

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

        // Cap input buffer at ~1 second to prevent runaway growth
        constexpr int maxInSamples = kFrameSamples * 100;
        if (static_cast<int>(m_inBuf.size()) > maxInSamples)
            m_inBuf.erase(m_inBuf.begin(),
                          m_inBuf.begin() + (m_inBuf.size() - maxInSamples));
    }
    m_inCv.notify_one();

    // Non-blocking: return any denoised data from the worker thread
    std::lock_guard<std::mutex> lock(m_outMutex);
    if (m_outBuf.empty()) return {};

    std::vector<float> result;
    result.swap(m_outBuf);
    return result;
}

// ── Batch mode ────────────────────────────────────────────────────────────────
// Matches the official NIM Python client protocol exactly:
//   open stream → send config → send kBatchFrames frames → WritesDone()
//   → drain all responses → close stream → repeat

std::unique_ptr<grpc::ClientReaderWriter<
    nvidia::maxine::bnr::v1::EnhanceAudioRequest,
    nvidia::maxine::bnr::v1::EnhanceAudioResponse>>
NvidiaBnrFilter::openStream(grpc::ClientContext& ctx)
{
    auto stream = m_stub->EnhanceAudio(&ctx);
    if (!stream) {
        fprintf(stderr, "NvidiaBnrFilter: batchLoop failed to open stream\n");
        return nullptr;
    }

    // Send config (intensity ratio)
    nvidia::maxine::bnr::v1::EnhanceAudioRequest configReq;
    configReq.mutable_config()->set_intensity_ratio(m_intensityRatio);
    if (!stream->Write(configReq)) {
        fprintf(stderr, "NvidiaBnrFilter: batchLoop failed to write config\n");
        return nullptr;
    }

    return stream;
}

void NvidiaBnrFilter::batchLoop()
{
    static constexpr int kBatchSamples = kBatchFrames * kFrameSamples; // 9600 samples = 200ms

    int batchCount = 0;

    while (!m_stopping.load()) {
        // ── 1. Wait until we have a full batch ────────────────────────────────
        std::vector<float> batch;
        {
            std::unique_lock<std::mutex> lock(m_inMutex);
            m_inCv.wait(lock, [this] {
                return m_stopping.load() ||
                       static_cast<int>(m_inBuf.size()) >= kBatchSamples;
            });

            if (m_stopping.load()) break;

            batch = std::vector<float>(m_inBuf.begin(), m_inBuf.begin() + kBatchSamples);
            m_inBuf.erase(m_inBuf.begin(), m_inBuf.begin() + kBatchSamples);
        }

        ++batchCount;

        // ── 2. Open a fresh stream for this batch ─────────────────────────────
        grpc::ClientContext ctx;
        auto stream = openStream(ctx);
        if (!stream) {
            if (!m_stopping.load()) {
                fprintf(stderr, "NvidiaBnrFilter: batchLoop could not open stream (batch %d)\n",
                        batchCount);
                m_connected.store(false);
                if (onConnectionChanged) onConnectionChanged(false);
                if (onError) onError("Failed to open BNR stream");
            }
            break;
        }

        // ── 3. Send all frames in the batch ───────────────────────────────────
        bool writeFailed = false;
        for (int i = 0; i < kBatchFrames && !m_stopping.load(); ++i) {
            nvidia::maxine::bnr::v1::EnhanceAudioRequest req;
            const float* frameStart = batch.data() + i * kFrameSamples;
            req.set_audio_stream_data(
                reinterpret_cast<const char*>(frameStart), kFrameBytes);

            if (!stream->Write(req)) {
                fprintf(stderr, "NvidiaBnrFilter: batchLoop Write failed (batch %d frame %d)\n",
                        batchCount, i);
                writeFailed = true;
                break;
            }
        }

        if (writeFailed || m_stopping.load()) break;

        // ── 4. Signal end-of-batch (equivalent to Python generator exhausting) ─
        stream->WritesDone();

        // ── 5. Drain all responses ────────────────────────────────────────────
        nvidia::maxine::bnr::v1::EnhanceAudioResponse response;
        int responseCount = 0;
        int audioFrames = 0;

        while (stream->Read(&response)) {
            ++responseCount;
            if (response.has_audio_stream_data()) {
                const auto& data = response.audio_stream_data();
                const float* fptr = reinterpret_cast<const float*>(data.data());
                const int fcount = static_cast<int>(data.size()) / sizeof(float);
                audioFrames += fcount;

                std::lock_guard<std::mutex> lock(m_outMutex);
                m_outBuf.insert(m_outBuf.end(), fptr, fptr + fcount);

                // Cap output buffer at ~2 seconds
                constexpr int maxOutSamples = kFrameSamples * 200;
                if (static_cast<int>(m_outBuf.size()) > maxOutSamples)
                    m_outBuf.erase(m_outBuf.begin(),
                                   m_outBuf.begin() + (m_outBuf.size() - maxOutSamples));
            }
        }

        // ── 6. Check final stream status ──────────────────────────────────────
        grpc::Status status = stream->Finish();

        if (batchCount <= 3 || batchCount % 100 == 0) {
            fprintf(stderr,
                    "NvidiaBnrFilter: batch %d — sent %d frames, got %d responses, "
                    "%d audio samples, gRPC status=%d\n",
                    batchCount, kBatchFrames, responseCount, audioFrames,
                    static_cast<int>(status.error_code()));
        }

        if (!status.ok() && !m_stopping.load()) {
            fprintf(stderr,
                    "NvidiaBnrFilter: batchLoop stream finished with error (batch %d): "
                    "code=%d msg=\"%s\"\n",
                    batchCount,
                    static_cast<int>(status.error_code()),
                    status.error_message().c_str());
            // Non-fatal: NIM may close the stream after processing — just open a new one
        }
    }

    fprintf(stderr, "NvidiaBnrFilter: batchLoop exiting after %d batches\n", batchCount);
}

// ── Concurrent mode: separate send and receive threads ───────────────────────

void NvidiaBnrFilter::sendLoop()
{
    while (!m_stopping.load()) {
        if (m_configDirty.exchange(false)) {
            nvidia::maxine::bnr::v1::EnhanceAudioRequest configReq;
            auto* config = configReq.mutable_config();
            config->set_intensity_ratio(m_intensityRatio);
            if (!m_stream->Write(configReq)) {
                if (!m_stopping.load()) {
                    fprintf(stderr, "NvidiaBnrFilter: sendLoop config write failed\n");
                    m_connected.store(false);
                    if (onConnectionChanged) onConnectionChanged(false);
                    if (onError) onError("gRPC config write failed");
                }
                return;
            }
        }

        std::vector<float> frame;
        {
            std::lock_guard<std::mutex> lock(m_inMutex);
            if (static_cast<int>(m_inBuf.size()) >= kFrameSamples) {
                frame = std::vector<float>(m_inBuf.begin(), m_inBuf.begin() + kFrameSamples);
                m_inBuf.erase(m_inBuf.begin(), m_inBuf.begin() + kFrameSamples);
            }
        }

        if (frame.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }

        nvidia::maxine::bnr::v1::EnhanceAudioRequest req;
        req.set_audio_stream_data(reinterpret_cast<const char*>(frame.data()), kFrameBytes);

        if (!m_stream->Write(req)) {
            if (!m_stopping.load()) {
                fprintf(stderr, "NvidiaBnrFilter: sendLoop write failed\n");
                m_connected.store(false);
                if (onConnectionChanged) onConnectionChanged(false);
                if (onError) onError("gRPC write failed");
            }
            return;
        }
    }
}

void NvidiaBnrFilter::recvLoop()
{
    fprintf(stderr, "NvidiaBnrFilter: recvLoop started, about to call Read() #1\n");

    nvidia::maxine::bnr::v1::EnhanceAudioResponse response;
    int recvCount = 0;

    while (!m_stopping.load()) {
        if (recvCount == 0) {
            fprintf(stderr, "NvidiaBnrFilter: recvLoop blocking on Read() #1...\n");
        }

        if (!m_stream->Read(&response)) {
            if (!m_stopping.load()) {
                grpc::Status status = m_stream->Finish();
                fprintf(stderr,
                        "NvidiaBnrFilter: recvLoop Read() returned false after %d responses "
                        "(gRPC status code=%d msg=\"%s\")\n",
                        recvCount,
                        static_cast<int>(status.error_code()),
                        status.error_message().c_str());
                m_connected.store(false);
                if (onConnectionChanged) onConnectionChanged(false);
                if (onError) onError("BNR container stream ended");
            }
            return;
        }

        ++recvCount;
        const bool hasAudio = response.has_audio_stream_data();
        const bool hasConfig = response.has_config();
        const size_t audioSize = hasAudio ? response.audio_stream_data().size() : 0;

        if (recvCount <= 5 || recvCount % 500 == 0) {
            fprintf(stderr,
                    "NvidiaBnrFilter: recvLoop response #%d — has_audio=%d size=%zu has_config=%d\n",
                    recvCount, hasAudio ? 1 : 0, audioSize, hasConfig ? 1 : 0);
        }

        if (hasAudio) {
            const float* fptr = reinterpret_cast<const float*>(response.audio_stream_data().data());
            const int fcount = static_cast<int>(audioSize) / sizeof(float);

            std::lock_guard<std::mutex> lock(m_outMutex);
            m_outBuf.insert(m_outBuf.end(), fptr, fptr + fcount);

            constexpr int maxOutSamples = kFrameSamples * 20;
            if (static_cast<int>(m_outBuf.size()) > maxOutSamples)
                m_outBuf.erase(m_outBuf.begin(),
                               m_outBuf.begin() + (m_outBuf.size() - maxOutSamples));
        }
    }
    fprintf(stderr, "NvidiaBnrFilter: recvLoop exiting (stopping flag set), %d responses received\n",
            recvCount);
}

// ── Legacy mode: single thread, alternating Write→Read ───────────────────────

void NvidiaBnrFilter::workerLoop()
{
    while (!m_stopping.load()) {
        if (m_configDirty.exchange(false)) {
            nvidia::maxine::bnr::v1::EnhanceAudioRequest configReq;
            auto* config = configReq.mutable_config();
            config->set_intensity_ratio(m_intensityRatio);
            m_stream->Write(configReq);
        }

        std::vector<float> frame;
        {
            std::lock_guard<std::mutex> lock(m_inMutex);
            if (static_cast<int>(m_inBuf.size()) >= kFrameSamples) {
                frame = std::vector<float>(m_inBuf.begin(), m_inBuf.begin() + kFrameSamples);
                m_inBuf.erase(m_inBuf.begin(), m_inBuf.begin() + kFrameSamples);
            }
        }

        if (frame.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }

        nvidia::maxine::bnr::v1::EnhanceAudioRequest req;
        req.set_audio_stream_data(reinterpret_cast<const char*>(frame.data()), kFrameBytes);

        static int s_frameCount = 0;
        ++s_frameCount;
        if (s_frameCount <= 5 || s_frameCount % 500 == 0)
            fprintf(stderr, "NvidiaBnrFilter[legacy]: writing frame %d (%d bytes)\n",
                    s_frameCount, kFrameBytes);

        if (!m_stream->Write(req)) {
            if (!m_stopping.load()) {
                fprintf(stderr, "NvidiaBnrFilter[legacy]: gRPC write failed (frame %d)\n", s_frameCount);
                m_connected.store(false);
                if (onConnectionChanged) onConnectionChanged(false);
                if (onError) onError("gRPC write failed");
            }
            return;
        }

        if (s_frameCount <= 5 || s_frameCount % 500 == 0)
            fprintf(stderr, "NvidiaBnrFilter[legacy]: write OK frame %d, waiting for Read...\n", s_frameCount);

        nvidia::maxine::bnr::v1::EnhanceAudioResponse response;
        if (!m_stream->Read(&response)) {
            if (!m_stopping.load()) {
                fprintf(stderr, "NvidiaBnrFilter[legacy]: gRPC read failed (frame %d)\n", s_frameCount);
                m_connected.store(false);
                if (onConnectionChanged) onConnectionChanged(false);
                if (onError) onError("BNR container stream ended");
            }
            return;
        }

        if (s_frameCount <= 5 || s_frameCount % 500 == 0)
            fprintf(stderr, "NvidiaBnrFilter[legacy]: Read returned frame %d, has_audio=%d size=%zu\n",
                    s_frameCount,
                    response.has_audio_stream_data(),
                    response.has_audio_stream_data() ? response.audio_stream_data().size() : 0);

        if (response.has_audio_stream_data()) {
            const auto& data = response.audio_stream_data();
            const float* fptr = reinterpret_cast<const float*>(data.data());
            const int fcount = static_cast<int>(data.size()) / sizeof(float);

            std::lock_guard<std::mutex> lock(m_outMutex);
            m_outBuf.insert(m_outBuf.end(), fptr, fptr + fcount);

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
