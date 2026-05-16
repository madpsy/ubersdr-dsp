#pragma once

#include "filters/IFilter.h"
#include "Resampler.h"

// Generated gRPC/protobuf headers (produced at build time by protoc)
#include "ubersdr_dsp.grpc.pb.h"
#include "ubersdr_dsp.pb.h"

#include <grpcpp/grpcpp.h>
#include <memory>
#include <string>

namespace ubersdr {

// gRPC service implementation.
// Each ProcessAudio call gets its own filter instance — no shared state.
class DspServiceImpl final : public dsp::v1::DspService::Service {
public:
    DspServiceImpl() = default;
    ~DspServiceImpl() override = default;

    // Bidirectional streaming RPC: processes audio frames through the
    // selected noise-reduction filter with real-time parameter updates.
    grpc::Status ProcessAudio(
        grpc::ServerContext* ctx,
        grpc::ServerReaderWriter<dsp::v1::AudioResponse,
                                 dsp::v1::AudioRequest>* stream) override;

    // Unary RPC: returns metadata about all available filters and their params.
    grpc::Status GetFilters(
        grpc::ServerContext* ctx,
        const dsp::v1::GetFiltersRequest* req,
        dsp::v1::GetFiltersResponse* resp) override;

private:
    // Build an AudioResponse wrapping an error payload.
    static dsp::v1::AudioResponse makeError(const std::string& code,
                                            const std::string& message,
                                            const std::string& sessionId = {});

    // Build an AudioResponse wrapping a ParamAck payload.
    static dsp::v1::AudioResponse makeParamAck(const ParamMap& applied,
                                               const ParamMap& rejected,
                                               const std::string& sessionId);

    // Build an AudioResponse wrapping a processed AudioChunk.
    static dsp::v1::AudioResponse makeAudioResponse(const std::vector<float>& pcm,
                                                    uint64_t seqNum,
                                                    const std::string& sessionId);

    // Populate a GetFiltersResponse with static filter/param metadata.
    static void populateFilterInfo(dsp::v1::GetFiltersResponse* resp);
};

} // namespace ubersdr
