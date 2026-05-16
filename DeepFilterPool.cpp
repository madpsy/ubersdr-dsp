#ifdef HAVE_DFNR

#include "DeepFilterPool.h"
#include "deep_filter.h"

#include <cstdio>
#include <thread>

namespace AetherSDR {

// ── Singleton ─────────────────────────────────────────────────────────────────

DFStatePool& DFStatePool::instance()
{
    static DFStatePool s_instance;
    return s_instance;
}

// ── init ──────────────────────────────────────────────────────────────────────

void DFStatePool::init(const std::string& modelPath, float attenLim, int poolSize)
{
    if (m_initialised.exchange(true)) {
        fprintf(stderr, "DFStatePool: init() called more than once — ignored\n");
        return;
    }

    {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_modelPath = modelPath;
    }
    m_attenLim.store(attenLim);
    m_targetSize.store(poolSize > 0 ? poolSize : 1);
    m_shutdown.store(false);

    fprintf(stderr, "DFStatePool: pre-warming %d slot(s) from %s\n",
            m_targetSize.load(), modelPath.c_str());

    for (int i = 0; i < m_targetSize.load(); ++i)
        spawnReplenish();
}

// ── acquire ───────────────────────────────────────────────────────────────────

DFState* DFStatePool::acquire()
{
    std::lock_guard<std::mutex> lk(m_mutex);
    if (m_ready.empty())
        return nullptr;

    DFState* st = m_ready.back();
    m_ready.pop_back();
    return st;
}

// ── release ───────────────────────────────────────────────────────────────────

void DFStatePool::release(DFState* state)
{
    // Free the dirty state immediately on the calling thread — df_free() is
    // fast (just memory deallocation).
    if (state)
        df_free(state);

    // Don't replenish if we're shutting down or the pool is already at target.
    if (m_shutdown.load())
        return;

    {
        std::lock_guard<std::mutex> lk(m_mutex);
        // If the pool already has enough ready slots (e.g. multiple sessions
        // closed in quick succession), don't over-fill.
        if (static_cast<int>(m_ready.size()) >= m_targetSize.load())
            return;
    }

    spawnReplenish();
}

// ── shutdown ──────────────────────────────────────────────────────────────────

void DFStatePool::shutdown()
{
    m_shutdown.store(true);

    std::vector<DFState*> toFree;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        toFree.swap(m_ready);
    }

    for (DFState* st : toFree)
        df_free(st);

    fprintf(stderr, "DFStatePool: shutdown, freed %zu slot(s)\n", toFree.size());
}

// ── available ─────────────────────────────────────────────────────────────────

int DFStatePool::available() const
{
    std::lock_guard<std::mutex> lk(m_mutex);
    return static_cast<int>(m_ready.size());
}

// ── spawnReplenish ────────────────────────────────────────────────────────────

void DFStatePool::spawnReplenish()
{
    // Capture by value — the pool outlives all threads, but we want the thread
    // to be fully self-contained so it can be detached safely.
    const std::string path     = [&]{ std::lock_guard<std::mutex> lk(m_mutex); return m_modelPath; }();
    const float       attenLim = m_attenLim.load();

    std::thread([this, path, attenLim]() {
        if (m_shutdown.load())
            return;

        DFState* st = df_create(path.c_str(), attenLim, nullptr);
        if (!st) {
            fprintf(stderr, "DFStatePool: df_create() failed during replenishment "
                    "(path=%s)\n", path.c_str());
            return;
        }

        if (m_shutdown.load()) {
            // Server shut down while we were creating — discard immediately.
            df_free(st);
            return;
        }

        {
            std::lock_guard<std::mutex> lk(m_mutex);
            // Don't over-fill if multiple replenishments raced.
            if (static_cast<int>(m_ready.size()) < m_targetSize.load()) {
                m_ready.push_back(st);
                fprintf(stderr, "DFStatePool: slot ready (%zu/%d)\n",
                        m_ready.size(), m_targetSize.load());
                return;
            }
        }

        // We raced and the pool is already full — discard the extra state.
        df_free(st);

    }).detach();
}

} // namespace AetherSDR

#endif // HAVE_DFNR
