#pragma once

#ifdef HAVE_DFNR

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

struct DFState;

namespace AetherSDR {

// ── DFStatePool ───────────────────────────────────────────────────────────────
//
// A fixed-size pool of pre-warmed DFState* instances.
//
// Motivation: df_create() decompresses a 7.7 MB tar.gz, parses three ONNX
// graphs, and initialises ONNX Runtime — typically 1–2 s per call.  By
// pre-creating N states at server startup and handing them to sessions on
// demand, every session open becomes instantaneous.
//
// Thread-safety:
//   acquire() and release() are safe to call from any thread concurrently.
//
// Lifecycle:
//   - Construct once at server startup (process-lifetime singleton via
//     DFStatePool::instance()).
//   - Call init(modelPath, attenLim, poolSize) before the first session.
//   - acquire() returns a ready DFState* or nullptr if the pool is empty
//     (caller must fall back to df_create() inline).
//   - release(state) df_free()s the dirty state and asynchronously spawns a
//     background thread to df_create() a fresh replacement.
//   - shutdown() drains and frees all pooled states; called at server exit.
//
// Pool size:
//   Default 10.  Each slot costs ~18 MB RAM (decompressed ONNX weights).
//   Configurable via --dfnr-pool-size CLI flag or DFNR_POOL_SIZE env var.

class DFStatePool {
public:
    // Process-lifetime singleton.
    static DFStatePool& instance();

    // Initialise the pool.  Must be called once before any acquire().
    // Spawns poolSize background threads, each calling df_create().
    // The server is ready to accept connections immediately; sessions that
    // arrive before the pool is fully warm fall back to inline df_create().
    void init(const std::string& modelPath, float attenLim, int poolSize);

    // Borrow a ready DFState*.
    // Returns nullptr if the pool is currently empty — caller must call
    // df_create() inline and must NOT call release() for that state (call
    // df_free() directly instead).
    DFState* acquire();

    // Return a used (dirty) DFState* to the pool.
    // df_free()s the state immediately, then spawns a background thread to
    // df_create() a fresh replacement and push it back into the pool.
    void release(DFState* state);

    // Drain and free all pooled states.  Called at server shutdown.
    void shutdown();

    // Current number of ready slots (for logging/diagnostics).
    int available() const;

    // Target pool size as configured by init().
    int targetSize() const { return m_targetSize.load(); }

    const std::string& modelPath() const { return m_modelPath; }
    float attenLim() const { return m_attenLim.load(); }

private:
    DFStatePool() = default;
    ~DFStatePool() = default;
    DFStatePool(const DFStatePool&) = delete;
    DFStatePool& operator=(const DFStatePool&) = delete;

    // Spawn one background thread that calls df_create() and pushes the result
    // into m_ready.  Called from init() (N times) and from release().
    void spawnReplenish();

    mutable std::mutex      m_mutex;
    std::vector<DFState*>   m_ready;          // pre-warmed, available states
    std::string             m_modelPath;
    std::atomic<float>      m_attenLim{100.0f};
    std::atomic<int>        m_targetSize{0};
    std::atomic<bool>       m_initialised{false};
    std::atomic<bool>       m_shutdown{false};
};

} // namespace AetherSDR

#endif // HAVE_DFNR
