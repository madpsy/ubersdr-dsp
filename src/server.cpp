/*  ubersdr-dsp — gRPC noise-reduction server
 *
 *  Listens on a configurable port (default 50051) and exposes:
 *    DspService.ProcessAudio  — bidirectional streaming audio processing
 *    DspService.GetFilters    — unary filter/param introspection
 *
 *  Usage:
 *    ubersdr-dsp [--grpc-port <port>]
 *
 *  The filter and all its parameters are configured per-stream via the
 *  SessionConfig message; no command-line filter selection is needed.
 */

#include "DspServiceImpl.h"

#include <grpcpp/grpcpp.h>
#include <grpcpp/health_check_service_interface.h>

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

// ── Signal handling ───────────────────────────────────────────────────────────
// gRPC's absl mutex detector fires if Shutdown() is called directly from a
// signal handler (which runs on the same thread as Wait()).  Instead, the
// signal handler just sets a flag; a dedicated watcher thread calls Shutdown().

static std::atomic<bool> g_shutdown{false};
static grpc::Server*     g_server{nullptr};

static void onSignal(int /*sig*/)
{
    g_shutdown.store(true);
    // Do NOT call g_server->Shutdown() here — see watcher thread below.
}

// ── Argument helpers ──────────────────────────────────────────────────────────

static const char* getArg(int argc, char** argv, const char* flag, const char* def = nullptr)
{
    for (int i = 1; i < argc - 1; ++i)
        if (strcmp(argv[i], flag) == 0)
            return argv[i + 1];
    return def;
}

static bool hasFlag(int argc, char** argv, const char* flag)
{
    for (int i = 1; i < argc; ++i)
        if (strcmp(argv[i], flag) == 0)
            return true;
    return false;
}

// ── Main ──────────────────────────────────────────────────────────────────────

int main(int argc, char** argv)
{
    if (hasFlag(argc, argv, "--help") || hasFlag(argc, argv, "-h")) {
        fprintf(stderr,
            "Usage: %s [--grpc-port <port>]\n"
            "\n"
            "  --grpc-port <port>   gRPC listen port (default: 50051)\n"
            "\n"
            "Filter selection and parameters are configured per-stream via\n"
            "the SessionConfig gRPC message. See proto/ubersdr_dsp.proto.\n",
            argv[0]);
        return 0;
    }

    const char* portStr = getArg(argc, argv, "--grpc-port", "50051");
    const int   port    = atoi(portStr);
    if (port <= 0 || port > 65535) {
        fprintf(stderr, "Error: invalid --grpc-port '%s'\n", portStr);
        return 1;
    }

    const std::string listenAddr = "0.0.0.0:" + std::string(portStr);

    // ── Build and start the gRPC server ───────────────────────────────────────
    ubersdr::DspServiceImpl service;

    grpc::EnableDefaultHealthCheckService(true);

    grpc::ServerBuilder builder;
    builder.AddListeningPort(listenAddr, grpc::InsecureServerCredentials());
    builder.RegisterService(&service);

    // Allow large audio chunks (up to 16 MB per message)
    builder.SetMaxReceiveMessageSize(16 * 1024 * 1024);
    builder.SetMaxSendMessageSize(16 * 1024 * 1024);

    std::unique_ptr<grpc::Server> server = builder.BuildAndStart();
    if (!server) {
        fprintf(stderr, "Failed to start gRPC server on %s\n", listenAddr.c_str());
        return 1;
    }

    g_server = server.get();
    fprintf(stderr, "ubersdr-dsp gRPC server listening on %s\n", listenAddr.c_str());

    // ── Signal handlers for graceful shutdown ─────────────────────────────────
    // The signal handler only sets a flag. A watcher thread calls Shutdown()
    // so it runs outside the signal handler context, avoiding the absl mutex
    // deadlock warning that occurs when Shutdown() is called from a signal
    // handler while Wait() holds the same lock.
    std::signal(SIGINT,  onSignal);
    std::signal(SIGTERM, onSignal);

    std::thread watcher([&server]() {
        while (!g_shutdown.load())
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        fprintf(stderr, "ubersdr-dsp shutting down...\n");
        server->Shutdown();
    });

    server->Wait();
    watcher.join();

    fprintf(stderr, "ubersdr-dsp server stopped\n");
    return 0;
}
