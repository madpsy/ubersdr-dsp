#include "NvidiaBnrFilter.h"

#include <cstdint>
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

// ── WAV file helpers ──────────────────────────────────────────────────────────

// Write a little-endian 16-bit value into buf at offset.
static void writeLE16(std::vector<uint8_t>& buf, size_t offset, uint16_t v)
{
    buf[offset]     = static_cast<uint8_t>(v & 0xFF);
    buf[offset + 1] = static_cast<uint8_t>((v >> 8) & 0xFF);
}

// Write a little-endian 32-bit value into buf at offset.
static void writeLE32(std::vector<uint8_t>& buf, size_t offset, uint32_t v)
{
    buf[offset]     = static_cast<uint8_t>(v & 0xFF);
    buf[offset + 1] = static_cast<uint8_t>((v >> 8) & 0xFF);
    buf[offset + 2] = static_cast<uint8_t>((v >> 16) & 0xFF);
    buf[offset + 3] = static_cast<uint8_t>((v >> 24) & 0xFF);
}

// Build a complete WAV file (header + float32 PCM data) in memory.
// Format: RIFF/WAVE, PCM IEEE float (format 3), mono, 48000 Hz, 32-bit.
static std::vector<uint8_t> buildWavFile(const float* samples, int numSamples)
{
    // WAV header layout (44 bytes for IEEE float with 16-byte fmt chunk):
    //   RIFF chunk:  "RIFF" (4) + file_size-8 (4) + "WAVE" (4)
    //   fmt  chunk:  "fmt " (4) + chunk_size=16 (4) + audio_format=3 (2) +
    //                num_channels=1 (2) + sample_rate=48000 (4) +
    //                byte_rate=192000 (4) + block_align=4 (2) + bits_per_sample=32 (2)
    //   data chunk:  "data" (4) + data_size (4) + [raw float32 samples]
    static constexpr int kHeaderSize = 44;
    const uint32_t dataSize  = static_cast<uint32_t>(numSamples) * sizeof(float);
    const uint32_t fileSize  = kHeaderSize - 8 + dataSize; // RIFF chunk size = total - 8

    std::vector<uint8_t> wav(kHeaderSize + dataSize);

    // RIFF chunk
    wav[0] = 'R'; wav[1] = 'I'; wav[2] = 'F'; wav[3] = 'F';
    writeLE32(wav, 4, fileSize);
    wav[8] = 'W'; wav[9] = 'A'; wav[10] = 'V'; wav[11] = 'E';

    // fmt chunk
    wav[12] = 'f'; wav[13] = 'm'; wav[14] = 't'; wav[15] = ' ';
    writeLE32(wav, 16, 16);                // chunk size = 16 (no extension)
    writeLE16(wav, 20, 3);                 // audio format = 3 (IEEE float)
    writeLE16(wav, 22, 1);                 // num channels = 1 (mono)
    writeLE32(wav, 24, 48000u);            // sample rate = 48000
    writeLE32(wav, 28, 48000u * 4u);       // byte rate = 48000 * 4
    writeLE16(wav, 32, 4);                 // block align = 4 bytes
    writeLE16(wav, 34, 32);               // bits per sample = 32

    // data chunk
    wav[36] = 'd'; wav[37] = 'a'; wav[38] = 't'; wav[39] = 'a';
    writeLE32(wav, 40, dataSize);

    // Copy float32 samples (already little-endian on x86/ARM)
    std::memcpy(wav.data() + kHeaderSize,
                reinterpret_cast<const uint8_t*>(samples),
                dataSize);

    return wav;
}

// ── Batch mode ────────────────────────────────────────────────────────────────
// Matches the official NIM Python client non-streaming protocol:
//   open stream → send config → send WAV file bytes in 64KB chunks → WritesDone()
//   → drain all responses (WAV file bytes) → close stream → repeat
//
// NIM saves all incoming audio_stream_data bytes to "input.wav" and parses it
// with libsoundfile. We must send a complete WAV file (header + PCM) per batch.

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

    // Send config (intensity ratio) — first message, matches Python client
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
    static constexpr int kBatchSamples  = kBatchFrames * kFrameSamples; // 9600 samples = 200ms
    static constexpr int kChunkBytes    = 64 * 1024;                    // 64KB chunks (Python client default)

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

        // ── 2. Build a complete WAV file in memory ────────────────────────────
        // NIM saves all audio_stream_data bytes to "input.wav" and parses with
        // libsoundfile — we must send a valid WAV file, not raw PCM.
        std::vector<uint8_t> wavFile = buildWavFile(batch.data(),
                                                    static_cast<int>(batch.size()));

        // ── 3. Open a fresh stream for this batch ─────────────────────────────
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

        // ── 4. Send WAV file in 64KB chunks (matches Python non-streaming client) ─
        bool writeFailed = false;
        const uint8_t* ptr = wavFile.data();
        int remaining = static_cast<int>(wavFile.size());

        while (remaining > 0 && !m_stopping.load()) {
            const int chunkSize = std::min(remaining, kChunkBytes);
            nvidia::maxine::bnr::v1::EnhanceAudioRequest req;
            req.set_audio_stream_data(reinterpret_cast<const char*>(ptr), chunkSize);

            if (!stream->Write(req)) {
                fprintf(stderr, "NvidiaBnrFilter: batchLoop Write failed (batch %d)\n",
                        batchCount);
                writeFailed = true;
                break;
            }
            ptr       += chunkSize;
            remaining -= chunkSize;
        }

        if (writeFailed || m_stopping.load()) break;

        // ── 5. Signal end-of-file (Python generator exhausts here) ────────────
        stream->WritesDone();

        // ── 6. Drain all responses ────────────────────────────────────────────
        // NIM responds with the denoised audio as WAV file bytes.
        nvidia::maxine::bnr::v1::EnhanceAudioResponse response;
        int responseCount = 0;
        std::vector<uint8_t> responseBytes;

        while (stream->Read(&response)) {
            ++responseCount;
            if (response.has_audio_stream_data()) {
                const auto& data = response.audio_stream_data();
                responseBytes.insert(responseBytes.end(),
                                     data.begin(), data.end());
            }
        }

        // ── 7. Check final stream status ──────────────────────────────────────
        grpc::Status status = stream->Finish();

        if (batchCount <= 3 || batchCount % 100 == 0) {
            fprintf(stderr,
                    "NvidiaBnrFilter: batch %d — sent %zu WAV bytes, got %d responses, "
                    "%zu response bytes, gRPC status=%d\n",
                    batchCount, wavFile.size(), responseCount, responseBytes.size(),
                    static_cast<int>(status.error_code()));
        }

        if (!status.ok() && !m_stopping.load()) {
            fprintf(stderr,
                    "NvidiaBnrFilter: batchLoop stream error (batch %d): "
                    "code=%d msg=\"%s\"\n",
                    batchCount,
                    static_cast<int>(status.error_code()),
                    status.error_message().c_str());
            continue; // try next batch
        }

        // ── 8. Parse response WAV bytes → float32 samples ─────────────────────
        // NIM returns a WAV file. Skip the 44-byte header and read float32 PCM.
        static constexpr int kWavHeaderSize = 44;
        if (static_cast<int>(responseBytes.size()) > kWavHeaderSize) {
            const uint8_t* audioStart = responseBytes.data() + kWavHeaderSize;
            const int audioBytes = static_cast<int>(responseBytes.size()) - kWavHeaderSize;
            const int fcount = audioBytes / static_cast<int>(sizeof(float));

            if (fcount > 0) {
                const float* fptr = reinterpret_cast<const float*>(audioStart);
                std::lock_guard<std::mutex> lock(m_outMutex);
                m_outBuf.insert(m_outBuf.end(), fptr, fptr + fcount);

                // Cap output buffer at ~2 seconds
                constexpr int maxOutSamples = kFrameSamples * 200;
                if (static_cast<int>(m_outBuf.size()) > maxOutSamples)
                    m_outBuf.erase(m_outBuf.begin(),
                                   m_outBuf.begin() + (m_outBuf.size() - maxOutSamples));
            }
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
