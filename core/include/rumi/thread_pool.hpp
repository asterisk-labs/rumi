#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <utility>
#include <vector>

#ifndef _WIN32
#  include <sys/types.h>
#  include <unistd.h>
#endif

namespace rumi {

// Fixed-size process-wide pool. Each read submits a batch and waits only for
// that batch.
class ThreadPool {
public:
    explicit ThreadPool(unsigned threads);
    ~ThreadPool();

    ThreadPool(const ThreadPool&)            = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    class Batch {
    public:
        explicit Batch(ThreadPool& pool) noexcept : pool_(pool) {}
        ~Batch() { wait(); }

        Batch(const Batch&)            = delete;
        Batch& operator=(const Batch&) = delete;

        void submit(std::function<void()> job);
        void wait();

    private:
        ThreadPool&             pool_;
        std::mutex              mutex_;
        std::condition_variable done_;
        std::size_t             pending_{0};
    };

    [[nodiscard]] std::size_t size() const noexcept { return workers_.size(); }

private:
    void enqueue(std::function<void()> job);

    std::vector<std::thread>          workers_;
    std::queue<std::function<void()>> jobs_;
    std::mutex                        mutex_;
    std::condition_variable           ready_;
    bool                              stop_{false};
};


inline ThreadPool::ThreadPool(unsigned threads)
{
    if (threads < 1) threads = 1;
    try {
        workers_.reserve(threads);
        for (unsigned i = 0; i < threads; ++i) {
            workers_.emplace_back([this] {
                for (;;) {
                    std::function<void()> job;
                    {
                        std::unique_lock lock(mutex_);
                        ready_.wait(lock, [this] { return stop_ || !jobs_.empty(); });
                        if (stop_ && jobs_.empty()) return;
                        job = std::move(jobs_.front());
                        jobs_.pop();
                    }
                    job();
                }
            });
        }
    } catch (...) {
        // Join workers created before a constructor failure; destroying a
        // joinable std::thread would terminate the process.
        {
            std::lock_guard lock(mutex_);
            stop_ = true;
        }
        ready_.notify_all();
        for (std::thread& w : workers_) {
            if (w.joinable()) w.join();
        }
        throw;
    }
}

inline ThreadPool::~ThreadPool()
{
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
    }
    ready_.notify_all();
    for (std::thread& w : workers_) {
        if (w.joinable()) w.join();
    }
}

inline void ThreadPool::enqueue(std::function<void()> job)
{
    {
        std::lock_guard lock(mutex_);
        jobs_.push(std::move(job));
    }
    ready_.notify_one();
}

inline void ThreadPool::Batch::submit(std::function<void()> job)
{
    {
        std::lock_guard lock(mutex_);
        ++pending_;
    }
    try {
        pool_.enqueue([this, job = std::move(job)]() mutable {
            job();
            std::lock_guard lock(mutex_);
            if (--pending_ == 0) done_.notify_all();
        });
    } catch (...) {
        // Undo pending_ when enqueue rejects this job.
        std::lock_guard lock(mutex_);
        if (--pending_ == 0) done_.notify_all();
        throw;
    }
}

inline void ThreadPool::Batch::wait()
{
    std::unique_lock lock(mutex_);
    done_.wait(lock, [this] { return pending_ == 0; });
}


namespace detail {

#ifdef _WIN32
using pid_type = int;
inline pid_type current_pid() noexcept { return 0; }  // no fork, one owner
#else
using pid_type = ::pid_t;
inline pid_type current_pid() noexcept { return ::getpid(); }
#endif

// After fork, replace the inherited slot before locking it. The child cannot
// destroy the old pool because its worker threads no longer exist.
struct PoolSlot {
    explicit PoolSlot(pid_type pid) noexcept : owner(pid) {}

    ~PoolSlot()
    {
        delete live.exchange(nullptr, std::memory_order_acq_rel);
    }

    const pid_type          owner;
    std::atomic<ThreadPool*> live{nullptr};
    std::atomic<unsigned>    threads{0};
    std::mutex               make;
};

struct GlobalPool {
    std::atomic<PoolSlot*> current{nullptr};

    ~GlobalPool()
    {
        PoolSlot* slot = current.load(std::memory_order_acquire);
        if (slot != nullptr && slot->owner == current_pid()) delete slot;
    }
};

// Avoid inheriting a function-local static initialization guard across fork.
constinit inline GlobalPool g_global_pool;
static_assert(std::atomic<PoolSlot*>::is_always_lock_free,
              "post-fork pool registry must not hide a library mutex");

inline PoolSlot* current_pool_slot() noexcept
{
    PoolSlot* slot = g_global_pool.current.load(std::memory_order_acquire);
    return slot != nullptr && slot->owner == current_pid() ? slot : nullptr;
}

inline PoolSlot& process_pool_slot()
{
    if (PoolSlot* slot = current_pool_slot()) return *slot;

    const pid_type pid = current_pid();
    auto* fresh = new PoolSlot(pid);
    PoolSlot* seen = g_global_pool.current.load(std::memory_order_acquire);
    for (;;) {
        if (seen != nullptr && seen->owner == pid) {
            delete fresh;
            return *seen;
        }
        if (g_global_pool.current.compare_exchange_weak(
                seen, fresh, std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            return *fresh;
        }
    }
}

// Implemented in read.cpp; declared here for deterministic pool tests.
int reserve_thread_count(int requested) noexcept;
void rollback_thread_count(unsigned threads) noexcept;

}  // namespace detail


// Size of the current process's pool, or 0 when none exists.
inline unsigned global_thread_pool_size() noexcept
{
    detail::PoolSlot* slot = detail::current_pool_slot();
    if (slot == nullptr ||
        slot->live.load(std::memory_order_acquire) == nullptr) {
        return 0;
    }
    return slot->threads.load(std::memory_order_acquire);
}

// Serialize pool construction per process and publish only a complete pool.
template <typename ReserveCount, typename RollbackCount, typename MakePool>
inline ThreadPool* global_thread_pool(ReserveCount&& reserve_count,
                                      RollbackCount&& rollback_count,
                                      MakePool&& make_pool)
{
    detail::PoolSlot& slot = detail::process_pool_slot();
    if (ThreadPool* live = slot.live.load(std::memory_order_acquire)) return live;

    std::lock_guard lock(slot.make);
    if (ThreadPool* live = slot.live.load(std::memory_order_acquire)) return live;

    const unsigned threads = static_cast<unsigned>(reserve_count());
    if (threads <= 1) return nullptr;

    ThreadPool* pool = nullptr;
    try {
        pool = make_pool(threads);
    } catch (...) {
        rollback_count(threads);
        throw;
    }
    slot.threads.store(threads, std::memory_order_relaxed);
    slot.live.store(pool, std::memory_order_release);
    return pool;
}

template <typename ReserveCount, typename RollbackCount>
inline ThreadPool* global_thread_pool(ReserveCount&& reserve_count,
                                      RollbackCount&& rollback_count)
{
    return global_thread_pool(
        std::forward<ReserveCount>(reserve_count),
        std::forward<RollbackCount>(rollback_count),
        [](unsigned threads) { return new ThreadPool(threads); });
}

}  // namespace rumi
