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

// Fixed-size pool, one process-global instance shared by every read. Work goes
// in batches, and each read waits only for its own submitted work. Reads are
// planned and run on the calling thread.
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
        // A partially built vector contains joinable std::threads. Letting its
        // destructor see them would call std::terminate instead of reporting
        // the resource failure to the read that requested the pool.
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
        // Keep pending_ honest. The Batch destructor waits for jobs already
        // accepted by the pool, so their capture of this remains valid while
        // the submit exception unwinds the caller.
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

// Every process that touches the pool installs its own slot. The mutex belongs
// to the slot, not to the registry: after fork the child replaces the inherited
// slot before it can touch a mutex that another parent thread may have held.
// The inherited slot leaks in the child on purpose; destroying its pool would
// try to join workers that fork did not preserve.
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

// An inline object avoids a function-local static guard. Such a guard could
// itself be inherited mid-initialisation by a child.
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

// Thread-count reservation lives in read.cpp; these internal hooks let pool
// construction keep reservation and rollback under PoolSlot::make.
int reserve_thread_count(int requested) noexcept;
void rollback_thread_count(unsigned threads) noexcept;

}  // namespace detail


// The size of the pool this process owns, 0 when there is none: never built, or
// built before a fork and left behind. getpid() is the whole test, and it is a
// real syscall, glibc dropped its cache in 2.25.
inline unsigned global_thread_pool_size() noexcept
{
    detail::PoolSlot* slot = detail::current_pool_slot();
    if (slot == nullptr ||
        slot->live.load(std::memory_order_acquire) == nullptr) {
        return 0;
    }
    return slot->threads.load(std::memory_order_acquire);
}

// Build under the current process's mutex. reserve_count runs only after the
// mutex is held, so a failed constructor can roll its reservation back before
// another builder advances. Once live is published, every caller shares it.
// A forked child first installs a fresh slot and therefore never locks the
// inherited mutex or waits on inherited workers.
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
