#pragma once

#include <Render/Runtime/EventQueue.h>

#include <cstddef>
#include <functional>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

namespace org::services {

/**
 * @brief A frame slot's CPU output memory. Bump allocation in chunks that never move, so what it hands out stays put until Reset
 * at the slot's retirement. One thread at a time.
 */
class BumpArena {
public:
    static constexpr std::size_t kDefaultChunkBytes = 256 * 1024;

    explicit BumpArena(std::size_t chunkBytes = kDefaultChunkBytes) noexcept : m_chunkBytes(chunkBytes ? chunkBytes : kDefaultChunkBytes) {}
    BumpArena(const BumpArena&) = delete;
    BumpArena& operator=(const BumpArena&) = delete;
    BumpArena(BumpArena&&) = default;
    BumpArena& operator=(BumpArena&&) = default;

    /** @brief `count` uninitialised T, aligned for T. Trivial types only. */
    template <class T>
    T* Make(std::size_t count) {
        return static_cast<T*>(Allocate(count * sizeof(T), alignof(T)));
    }
    /** @brief `bytes` aligned to `align`, a power of two. A request larger than a chunk gets a chunk of its own. */
    void* Allocate(std::size_t bytes, std::size_t align);
    /** @brief Lets go of every allocation when the frame slot retires. The chunks are kept for the slot's next frame. */
    void Reset() noexcept;
    /** @brief The bytes handed out since the last Reset, counting the chunks passed over whole. */
    [[nodiscard]] std::size_t BytesUsed() const noexcept;
    /** @brief The chunks held, used or not. */
    [[nodiscard]] std::size_t Chunks() const noexcept { return m_chunks.size(); }

private:
    struct Chunk {
        std::unique_ptr<std::byte[]> bytes;
        std::size_t size = 0;
    };
    std::size_t m_chunkBytes;
    std::vector<Chunk> m_chunks;
    std::size_t m_current = 0, m_used = 0;
};

/**
 * @brief Pools a frame slot records into, reset off the owning thread. At retirement a slot hands in its used pool (ResetLater)
 * and takes back one already reset (Take), so the owning thread never makes the reset's driver call.
 *
 * Pool: default-constructible and movable, with `std::size_t Used() const` (0: nothing to reset) and `void Reset()`.
 * Any thread hands in; one thread takes. The host runs the resets: `Defer` runs work later on another thread, and a host that
 * refuses (at shutdown) drops the work and the pool with it.
 */
template <class Pool>
class ResetPools {
public:
    using Defer = std::function<void(std::function<void()> work)>;

    explicit ResetPools(Defer defer) : m_defer(std::move(defer)), m_ready(std::make_shared<Ready>()) {
        if (!m_defer) throw std::invalid_argument("ResetPools needs a defer callback");
    }
    ResetPools(const ResetPools&) = delete;
    ResetPools& operator=(const ResetPools&) = delete;

    /** @brief Any thread. Resets `pool` through Defer, then makes it ready. A pool with nothing used is ready at once. */
    void ResetLater(Pool&& pool) {
        if (!pool.Used()) {
            m_ready->Push(std::move(pool));
            return;
        }
        // Shared because a task must be copyable. The ready queue stays alive with the task.
        auto held = std::make_shared<Pool>(std::move(pool));
        m_defer([held, ready = m_ready] {
            held->Reset();
            ready->Push(std::move(*held));
        });
    }

    /** @brief The taking thread. A pool ResetLater made ready, or a new one when none is. */
    [[nodiscard]] Pool Take() {
        Pool pool;
        (void)m_ready->Pop(pool);
        return pool;
    }

    /**
     * @brief The taking thread, at a slot's retirement. When `pool` has anything used, it goes for a reset and a ready one takes
     * its place. Otherwise it stays.
     */
    void Recycle(Pool& pool) {
        if (pool.Used()) ResetLater(std::exchange(pool, Take()));
    }

private:
    using Ready = org::runtime::EventQueue<Pool, 16>;
    Defer m_defer;
    std::shared_ptr<Ready> m_ready;
};

/**
 * @brief One frame slot's CPU recording memory: a BumpArena per worker thread and one pool of command lists. Retire resets the
 * arenas and recycles the pool. The arenas keep their chunks for the slot's next frame.
 */
template <class Pool>
class RecordingSlot {
public:
    RecordingSlot() = default;
    /** @brief `workers` arenas, one per thread that writes into the slot. */
    explicit RecordingSlot(std::size_t workers, std::size_t chunkBytes = BumpArena::kDefaultChunkBytes) : m_workers(workers) {
        m_arenas = std::make_unique<BumpArena[]>(workers);
        for (std::size_t i = 0; i < workers; ++i) m_arenas[i] = BumpArena(chunkBytes);
    }

    [[nodiscard]] std::size_t Workers() const noexcept { return m_workers; }
    /** @brief Worker `index`'s arena. `index` is below Workers(). */
    [[nodiscard]] BumpArena& Arena(std::size_t index) noexcept { return m_arenas[index]; }
    [[nodiscard]] Pool& Lists() noexcept { return m_lists; }

    /** @brief The taking thread, once the slot's frame retired and nothing writes into it. */
    void Retire(ResetPools<Pool>& pools) {
        for (std::size_t i = 0; i < m_workers; ++i) m_arenas[i].Reset();
        pools.Recycle(m_lists);
    }

private:
    std::unique_ptr<BumpArena[]> m_arenas;
    std::size_t m_workers = 0;
    Pool m_lists;
};

} // namespace org::services
