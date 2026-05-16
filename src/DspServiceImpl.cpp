#include "DspServiceImpl.h"
#include "FilterFactory.h"
#include "Resampler.h"

#include "filters/Nr2FilterWrapper.h"
#include "filters/Rn2FilterWrapper.h"
#include "filters/Nr4FilterWrapper.h"
#include "filters/DfnrFilterWrapper.h"
#include "filters/BnrFilterWrapper.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <sstream>
#include <iomanip>

namespace ubersdr {

// ── UUID helper ───────────────────────────────────────────────────────────────

static std::string generateSessionId()
{
    std::random_device rd;
    std::mt19937_64 gen(rd());
    std::uniform_int_distribution<uint64_t> dist;
    std::ostringstream oss;
    oss << std::hex << std::setw(16) << std::setfill('0') << dist(gen)
        << std::setw(16) << std::setfill('0') << dist(gen);
    return oss.str();
}

// ── Response builders ─────────────────────────────────────────────────────────

dsp::v1::AudioResponse DspServiceImpl::makeError(const std::string& code,
                                                  const std::string& message,
                                                  const std::string& sessionId)
{
    dsp::v1::AudioResponse resp;
    resp.set_session_id(sessionId);
    auto* err = resp.mutable_error();
    err->set_code(code);
    err->set_message(message);
    return resp;
}

dsp::v1::AudioResponse DspServiceImpl::makeParamAck(const ParamMap& applied,
                                                     const ParamMap& rejected,
                                                     const std::string& sessionId)
{
    dsp::v1::AudioResponse resp;
    resp.set_session_id(sessionId);
    auto* ack = resp.mutable_ack();
    for (const auto& [k, v] : applied)
        (*ack->mutable_applied())[k] = v;
    for (const auto& [k, v] : rejected)
        (*ack->mutable_rejected())[k] = v;
    return resp;
}

dsp::v1::AudioResponse DspServiceImpl::makeAudioResponse(const std::vector<float>& pcm,
                                                          uint64_t seqNum,
                                                          const std::string& sessionId)
{
    dsp::v1::AudioResponse resp;
    resp.set_session_id(sessionId);
    auto* chunk = resp.mutable_audio();
    chunk->set_pcm_data(pcm.data(), pcm.size() * sizeof(float));
    chunk->set_sequence_num(seqNum);
    return resp;
}

// ── ProcessAudio ──────────────────────────────────────────────────────────────

grpc::Status DspServiceImpl::ProcessAudio(
    grpc::ServerContext* /*ctx*/,
    grpc::ServerReaderWriter<dsp::v1::AudioResponse,
                             dsp::v1::AudioRequest>* stream)
{
    const std::string sessionId = generateSessionId();
    std::unique_ptr<IFilter> filter;
    bool configured = false;
    int  blockFrames = 960; // default: 40 ms at 24 kHz

    // Resamplers are only created when the client rate differs from 24 kHz.
    std::unique_ptr<AetherSDR::Resampler> upsample;   // client → 24 kHz
    std::unique_ptr<AetherSDR::Resampler> downsample; // 24 kHz → client
    int clientRate     = 24000;
    int clientChannels = 2;    // 1 = mono, 2 = stereo
    dsp::v1::PcmEncoding pcmEncoding = dsp::v1::PCM_FLOAT32_LE;

    fprintf(stderr, "[%s] stream opened\n", sessionId.c_str());

    dsp::v1::AudioRequest req;
    while (stream->Read(&req)) {

        // ── SessionConfig ─────────────────────────────────────────────────────
        if (req.has_config()) {
            if (configured) {
                stream->Write(makeError(
                    "FILTER_CHANGE_NOT_ALLOWED",
                    "Filter cannot be changed mid-stream. "
                    "Close and reopen the stream to select a different filter.",
                    sessionId));
                continue;
            }

            const auto& cfg = req.config();
            blockFrames = (cfg.block() > 0) ? cfg.block() : 960;

            // Validate and store client sample rate
            clientRate = (cfg.sample_rate() > 0) ? cfg.sample_rate() : 24000;
            if (clientRate != 12000 && clientRate != 24000) {
                const std::string errMsg =
                    "unsupported sample_rate " + std::to_string(clientRate) +
                    "; supported: 12000, 24000";
                stream->Write(makeError("INVALID_SAMPLE_RATE", errMsg, sessionId));
                fprintf(stderr, "[%s] %s\n", sessionId.c_str(), errMsg.c_str());
                return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, errMsg);
            }

            // Validate and store client channel count
            clientChannels = (cfg.channels() > 0) ? cfg.channels() : 2;
            if (clientChannels != 1 && clientChannels != 2) {
                const std::string errMsg =
                    "unsupported channels " + std::to_string(clientChannels) +
                    "; supported: 1 (mono), 2 (stereo)";
                stream->Write(makeError("INVALID_CHANNELS", errMsg, sessionId));
                fprintf(stderr, "[%s] %s\n", sessionId.c_str(), errMsg.c_str());
                return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, errMsg);
            }

            std::string errMsg;
            filter = createFilter(cfg, errMsg);
            if (!filter) {
                stream->Write(makeError("FILTER_INIT_FAILED", errMsg, sessionId));
                fprintf(stderr, "[%s] filter init failed: %s\n",
                        sessionId.c_str(), errMsg.c_str());
                return grpc::Status(grpc::StatusCode::INTERNAL, errMsg);
            }

            // Create resamplers only when the client rate differs from 24 kHz
            if (clientRate != 24000) {
                // blockFrames is at the client rate; the 24 kHz block is larger
                const int maxBlock = blockFrames * (24000 / clientRate) + 64;
                upsample   = std::make_unique<AetherSDR::Resampler>(
                                 clientRate, 24000, blockFrames);
                downsample = std::make_unique<AetherSDR::Resampler>(
                                 24000, clientRate, maxBlock);
            }

            pcmEncoding = cfg.pcm_encoding();

            configured = true;
            fprintf(stderr, "[%s] configured filter=%s block=%d sample_rate=%d channels=%d pcm_encoding=%s\n",
                    sessionId.c_str(), cfg.filter().c_str(), blockFrames, clientRate, clientChannels,
                    (pcmEncoding == dsp::v1::PCM_INT16_BE) ? "int16_be" : "float32_le");

            // Send a ParamAck with empty maps to signal successful configuration
            stream->Write(makeParamAck({}, {}, sessionId));
            continue;
        }

        // ── ParamUpdate ───────────────────────────────────────────────────────
        if (req.has_param_update()) {
            if (!configured) {
                stream->Write(makeError(
                    "NOT_CONFIGURED",
                    "Send a SessionConfig message before sending ParamUpdate.",
                    sessionId));
                continue;
            }

            ParamMap params(req.param_update().params().begin(),
                            req.param_update().params().end());
            auto [applied, rejected] = filter->applyParams(params);
            stream->Write(makeParamAck(applied, rejected, sessionId));

            if (!applied.empty()) {
                fprintf(stderr, "[%s] params updated:", sessionId.c_str());
                for (const auto& [k, v] : applied)
                    fprintf(stderr, " %s=%s", k.c_str(), v.c_str());
                fprintf(stderr, "\n");
            }
            if (!rejected.empty()) {
                fprintf(stderr, "[%s] params rejected:", sessionId.c_str());
                for (const auto& [k, v] : rejected)
                    fprintf(stderr, " %s(%s)", k.c_str(), v.c_str());
                fprintf(stderr, "\n");
            }
            continue;
        }

        // ── AudioChunk ────────────────────────────────────────────────────────
        if (req.has_audio()) {
            if (!configured) {
                stream->Write(makeError(
                    "NOT_CONFIGURED",
                    "Send a SessionConfig message before sending audio.",
                    sessionId));
                continue;
            }

            const auto& chunk = req.audio();
            const std::string& raw = chunk.pcm_data();

            // Validate: must be a whole number of frames for the declared channel count
            // and encoding (2 bytes/sample for int16, 4 bytes/sample for float32).
            const size_t bytesPerSample = (pcmEncoding == dsp::v1::PCM_INT16_BE)
                                          ? sizeof(int16_t) : sizeof(float);
            if (raw.size() % (bytesPerSample * static_cast<size_t>(clientChannels)) != 0) {
                stream->Write(makeError(
                    "INVALID_AUDIO",
                    "pcm_data length is not a multiple of " +
                        std::to_string(bytesPerSample * clientChannels) +
                        " bytes (" +
                        ((pcmEncoding == dsp::v1::PCM_INT16_BE) ? "int16_be" : "float32_le") +
                        " frame for " +
                        std::to_string(clientChannels) + " channel(s)).",
                    sessionId));
                continue;
            }

            const int numFrames = static_cast<int>(
                raw.size() / (bytesPerSample * static_cast<size_t>(clientChannels)));

            // ── Decode inbound PCM ────────────────────────────────────────────
            // For PCM_INT16_BE: convert each big-endian int16 sample to float32.
            // For PCM_FLOAT32_LE: use the raw bytes directly (no copy needed).
            std::vector<float> decodedPcm;
            const float* pcm;
            if (pcmEncoding == dsp::v1::PCM_INT16_BE) {
                const int numSamples = numFrames * clientChannels;
                decodedPcm.resize(numSamples);
                const uint8_t* src = reinterpret_cast<const uint8_t*>(raw.data());
                for (int i = 0; i < numSamples; ++i) {
                    // Reconstruct big-endian int16 from two bytes
                    const int16_t s = static_cast<int16_t>(
                        (static_cast<uint16_t>(src[i * 2]) << 8) |
                         static_cast<uint16_t>(src[i * 2 + 1]));
                    decodedPcm[i] = static_cast<float>(s) / 32768.0f;
                }
                pcm = decodedPcm.data();
            } else {
                pcm = reinterpret_cast<const float*>(raw.data());
            }

            // ── Mono → stereo expansion ───────────────────────────────────────
            // All filters and resamplers work on interleaved stereo internally.
            // If the client sent mono, duplicate each sample to L+R.
            std::vector<float> stereoIn;
            const float* stereoPtr = pcm;
            int stereoFrames = numFrames;
            if (clientChannels == 1) {
                stereoIn.resize(numFrames * 2);
                for (int i = 0; i < numFrames; ++i) {
                    stereoIn[i * 2]     = pcm[i];
                    stereoIn[i * 2 + 1] = pcm[i];
                }
                stereoPtr = stereoIn.data();
            }

            // ── Resample + filter ─────────────────────────────────────────────
            std::vector<float> processed;
            if (upsample) {
                // Client rate → 24 kHz → filter → client rate
                auto up = upsample->processStereoToStereo(stereoPtr, stereoFrames);
                const int upFrames = static_cast<int>(up.size() / 2);
                auto filtered = filter->process(up.data(), upFrames);
                processed = downsample->processStereoToStereo(filtered.data(),
                                static_cast<int>(filtered.size() / 2));
            } else {
                processed = filter->process(stereoPtr, stereoFrames);
            }

            // ── Stereo → mono collapse ────────────────────────────────────────
            // If the client expects mono back, average L+R and return mono frames.
            if (clientChannels == 1) {
                const int outFrames = static_cast<int>(processed.size() / 2);
                std::vector<float> mono(outFrames);
                for (int i = 0; i < outFrames; ++i)
                    mono[i] = (processed[i * 2] + processed[i * 2 + 1]) * 0.5f;
                processed = std::move(mono);
            }

            // ── Encode outbound PCM ───────────────────────────────────────────
            // For PCM_INT16_BE: convert each float32 sample back to big-endian int16.
            // For PCM_FLOAT32_LE: makeAudioResponse writes the float bytes directly.
            if (pcmEncoding == dsp::v1::PCM_INT16_BE) {
                const int numSamples = static_cast<int>(processed.size());
                std::string outBytes(static_cast<size_t>(numSamples) * 2, '\0');
                uint8_t* dst = reinterpret_cast<uint8_t*>(outBytes.data());
                for (int i = 0; i < numSamples; ++i) {
                    const float clamped = std::clamp(processed[i], -1.0f, 1.0f);
                    const int16_t s = static_cast<int16_t>(clamped * 32767.0f);
                    dst[i * 2]     = static_cast<uint8_t>((s >> 8) & 0xFF);
                    dst[i * 2 + 1] = static_cast<uint8_t>(s & 0xFF);
                }
                dsp::v1::AudioResponse resp;
                resp.set_session_id(sessionId);
                auto* outChunk = resp.mutable_audio();
                outChunk->set_pcm_data(outBytes);
                outChunk->set_sequence_num(chunk.sequence_num());
                stream->Write(resp);
            } else {
                stream->Write(makeAudioResponse(processed, chunk.sequence_num(), sessionId));
            }
            continue;
        }

        // Unknown payload type — ignore silently
    }

    fprintf(stderr, "[%s] stream closed\n", sessionId.c_str());
    return grpc::Status::OK;
}

// ── GetFilters ────────────────────────────────────────────────────────────────

grpc::Status DspServiceImpl::GetFilters(
    grpc::ServerContext* /*ctx*/,
    const dsp::v1::GetFiltersRequest* /*req*/,
    dsp::v1::GetFiltersResponse* resp)
{
    populateFilterInfo(resp);
    return grpc::Status::OK;
}

// ── Static filter metadata ────────────────────────────────────────────────────

static void addFilter(dsp::v1::GetFiltersResponse* resp,
                      const std::string& filterName,
                      const std::string& description,
                      const std::vector<ParamDescriptor>& params)
{
    auto* fi = resp->add_filters();
    fi->set_name(filterName);
    fi->set_description(description);
    for (const auto& pd : params) {
        auto* pi = fi->add_params();
        pi->set_name(pd.name);
        pi->set_type(pd.type);
        pi->set_default_val(pd.default_val);
        pi->set_min_val(pd.min_val);
        pi->set_max_val(pd.max_val);
        pi->set_description(pd.description);
        pi->set_runtime_safe(pd.runtime_safe);
    }
}

void DspServiceImpl::populateFilterInfo(dsp::v1::GetFiltersResponse* resp)
{
    // NR2
    {
        Nr2FilterWrapper tmp;
        addFilter(resp, "nr2",
                  "SpectralNR — MMSE-LSA + OSMS spectral subtraction (always available)",
                  tmp.describe());
    }
    // RN2
    {
        Rn2FilterWrapper tmp;
        addFilter(resp, "rn2",
                  "RNNoise — Mozilla/Xiph RNN-based suppressor (always available, no params)",
                  tmp.describe());
    }
    // NR4
    {
        Nr4FilterWrapper tmp;
        addFilter(resp, "nr4",
                  "libspecbleach — SPP-MMSE adaptive denoiser"
#ifndef HAVE_SPECBLEACH
                  " [NOT COMPILED IN]"
#endif
                  ,
                  tmp.describe());
    }
    // DFNR
    {
        DfnrFilterWrapper tmp;
        addFilter(resp, "dfnr",
                  "DeepFilterNet3 — neural network denoiser"
#ifndef HAVE_DFNR
                  " [NOT COMPILED IN]"
#endif
                  ,
                  tmp.describe());
    }
    // BNR
    {
        BnrFilterWrapper tmp(""); // don't actually connect
        addFilter(resp, "bnr",
                  "NVIDIA Maxine BNR — cloud/NIM gRPC denoiser"
#ifndef HAVE_BNR
                  " [NOT COMPILED IN]"
#endif
                  ,
                  tmp.describe());
    }
}

} // namespace ubersdr
