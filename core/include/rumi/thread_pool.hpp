#pragma once

#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <utility>
#include <vector>

namespace rumi {

// Fixed-size pool, one process-global instance shared by every Image. It
// replaces GDAL's own pool, which sits behind headers an out-of-tree build
// can't reach. Work goes in batches and wait() blocks only on its own batch,
// so concurrent reads don't stall each other. Reads are planned and run on
//  the calling thread.
class ThreadPool {
public:
    explicit ThreadPool(unsigned threads);
    ~ThreadPool();

    ThreadPool(const ThreadPool&)            = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    class Batch {
    public:
        explicit Batch(ThreadPool& pool) noexcept : pool_(pool) {}

        void submit(std::function<void()> job);
        void wait();

    private:
        ThreadPool&             pool_;
        std::mutex              mutex_;
        std::condition_variable done_;
        std::size_t             pending_{0};
    };

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
    pool_.enqueue([this, job = std::move(job)]() mutable {
        job();
        std::lock_guard lock(mutex_);
        if (--pending_ == 0) done_.notify_all();
    });
}

inline void ThreadPool::Batch::wait()
{
    std::unique_lock lock(mutex_);
    done_.wait(lock, [this] { return pending_ == 0; });
}


// Sized on first use: the first caller's thread count wins, later callers
// share it regardless of what they ask for.
inline ThreadPool& global_thread_pool(unsigned threads)
{
    static ThreadPool pool(threads);
    return pool;
}

}  // namespace rumi