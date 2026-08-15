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

// The pool sits behind a pointer so a forked child can walk away from it.
struct GlobalPool {
    std::atomic<ThreadPool*> live{nullptr};
    std::atomic<unsigned>    threads{0};
    std::atomic<pid_type>    owner{0};
    std::mutex               make;
};

// Joins the pool at exit, as a plain static instance would have. One inherited
// across a fork is left alone: its workers are gone and join() would hang.
struct PoolReaper {
    GlobalPool& g;

    ~PoolReaper()
    {
        if (g.owner.load(std::memory_order_acquire) == current_pid()) {
            delete g.live.exchange(nullptr, std::memory_order_acq_rel);
        }
    }
};

inline GlobalPool& global_pool()
{
    static GlobalPool g;
    static PoolReaper reaper{g};  // built after g, so destroyed before it
    (void) reaper;
    return g;
}

}  // namespace detail


// The size of the pool this process owns, 0 when there is none: never built, or
// built before a fork and left behind. getpid() is the whole test, and it is a
// real syscall, glibc dropped its cache in 2.25.
inline unsigned global_thread_pool_size() noexcept
{
    detail::GlobalPool& g = detail::global_pool();
    if (g.live.load(std::memory_order_acquire) == nullptr) return 0;
    if (g.owner.load(std::memory_order_acquire) != detail::current_pid()) return 0;
    return g.threads.load(std::memory_order_acquire);
}

// Sized on first use: the first caller's thread count wins, later callers share
// it regardless of what they ask for. A child that inherited a pool across
// fork() builds its own, because fork() keeps only the calling thread and every
// wait() on the inherited one would hang. The old pool leaks on purpose, since
// its destructor would join workers that no longer exist.
inline ThreadPool& global_thread_pool(unsigned threads)
{
    detail::GlobalPool& g = detail::global_pool();
    if (global_thread_pool_size() != 0) {
        return *g.live.load(std::memory_order_acquire);
    }

    std::lock_guard lock(g.make);
    if (global_thread_pool_size() == 0) {
        auto* pool = new ThreadPool(threads);
        g.threads.store(threads, std::memory_order_release);
        g.owner.store(detail::current_pid(), std::memory_order_release);
        g.live.store(pool, std::memory_order_release);
    }
    return *g.live.load(std::memory_order_acquire);
}

}  // namespace rumi
