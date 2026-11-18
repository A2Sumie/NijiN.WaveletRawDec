// SPDX-License-Identifier: Apache-2.0
#ifndef NIJINWVDC_WORKER_POOL_H
#define NIJINWVDC_WORKER_POOL_H

#include "simd_platform.h"

#include <algorithm>
#include <atomic>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <type_traits>
#include <version>
#include <vector>

namespace nijinwvdc {
namespace detail {

#ifdef __APPLE__
using NativeWaitInt = int64_t;
#else
using NativeWaitInt = int32_t;
#endif

inline constexpr size_t kCacheLine = 128u;

inline void cpu_relax() {
#if defined(_MSC_VER)
#if defined(_M_X64) || defined(_M_IX86)
    _mm_pause();
#elif defined(_M_ARM64)
    __yield();
#endif
#elif defined(__aarch64__)
    asm volatile("yield" ::: "memory");
#elif defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#endif
}

inline int resolve_worker_count(int requested) {
    if (requested > 0) {
        return requested;
    }
    unsigned hw = std::thread::hardware_concurrency();
    return hw > 0u ? static_cast<int>(hw) : 4;
}

class WorkerPool {
public:
    WorkerPool() = default;

    ~WorkerPool() {
        stop_flag_.store(true, std::memory_order_seq_cst);
        const NativeWaitInt next_ep = epoch_.load(std::memory_order_relaxed) + 1;
        epoch_.store(next_ep, std::memory_order_seq_cst);
        epoch_.notify_all();
#if !(defined(__cpp_lib_jthread) && __cpp_lib_jthread >= 201911L)
        for (size_t i = 0; i < threads_.size(); ++i) {
            if (threads_[i].joinable()) {
                threads_[i].join();
            }
        }
#endif
    }

    WorkerPool(const WorkerPool&) = delete;
    WorkerPool& operator=(const WorkerPool&) = delete;

    template <typename Fn>
        requires std::invocable<Fn&, size_t, size_t, int>
    void parallel_for_chunks(size_t total, size_t grain, int num_workers, Fn&& fn) {
        if (total == 0u) {
            return;
        }
        const size_t safe_grain = std::max<size_t>(1u, grain);
        int nw = std::max(1, num_workers);
        const size_t n_tasks = (total + safe_grain - 1u) / safe_grain;
        if (static_cast<size_t>(nw) > n_tasks) {
            nw = static_cast<int>(n_tasks);
        }
        if (nw <= 1) {
            fn(0u, total, 0);
            return;
        }

        ensure_workers(static_cast<size_t>(nw - 1));

        using FnType = std::remove_reference_t<Fn>;
        total_items_ = total;
        grain_ = safe_grain;
        active_bg_ = nw - 1;
        fn_ctx_ = const_cast<void*>(static_cast<const void*>(&fn));
        fn_ptr_ = &invoke_fn<FnType>;
        next_idx_.store(0u, std::memory_order_relaxed);

        const NativeWaitInt participants = static_cast<NativeWaitInt>(1u + threads_.size());
        active_rem_.store(participants, std::memory_order_release);

        const NativeWaitInt next_ep = epoch_.load(std::memory_order_relaxed) + 1;
        epoch_.store(next_ep, std::memory_order_seq_cst);
        if (sleeping_workers_.load(std::memory_order_seq_cst) > 0u) [[unlikely]] {
            epoch_.notify_all();
        }

        run_work(0);

        if (active_rem_.fetch_sub(1, std::memory_order_seq_cst) > 1) {
            for (int spin = 0; spin < 4000; ++spin) {
                if (active_rem_.load(std::memory_order_acquire) == 0) {
                    return;
                }
                cpu_relax();
            }
            main_waiting_.store(true, std::memory_order_seq_cst);
            for (;;) {
                const NativeWaitInt cur = active_rem_.load(std::memory_order_seq_cst);
                if (cur == 0) {
                    break;
                }
                active_rem_.wait(cur, std::memory_order_acquire);
            }
            main_waiting_.store(false, std::memory_order_relaxed);
        }
    }

private:
    template <typename FnType>
    static void invoke_fn(void* ctx, size_t beg, size_t end, int worker_idx) {
        (*static_cast<FnType*>(ctx))(beg, end, worker_idx);
    }

    void ensure_workers(size_t needed_bg) {
        if (threads_.size() >= needed_bg) {
            return;
        }
        const NativeWaitInt init_ep = epoch_.load(std::memory_order_relaxed);
        threads_.reserve(needed_bg);
        while (threads_.size() < needed_bg) {
            const size_t bg_id = threads_.size();
            threads_.emplace_back([this, bg_id, init_ep]() {
                worker_loop(bg_id, init_ep);
            });
        }
    }

    void run_work(int worker_idx) {
        for (;;) {
            const size_t beg = next_idx_.fetch_add(grain_, std::memory_order_relaxed);
            if (beg >= total_items_) {
                break;
            }
            size_t end = beg + grain_;
            if (end > total_items_) {
                end = total_items_;
            }
            fn_ptr_(fn_ctx_, beg, end, worker_idx);
        }
    }

    void worker_loop(size_t bg_id, NativeWaitInt seen_epoch) {
        for (;;) {
            NativeWaitInt cur = epoch_.load(std::memory_order_acquire);
            if (cur == seen_epoch) {
                for (int spin = 0; spin < 2000; ++spin) {
                    cpu_relax();
                    if (stop_flag_.load(std::memory_order_acquire)) [[unlikely]] {
                        return;
                    }
                    cur = epoch_.load(std::memory_order_acquire);
                    if (cur != seen_epoch) {
                        break;
                    }
                }
                while (cur == seen_epoch) {
                    if (stop_flag_.load(std::memory_order_acquire)) [[unlikely]] {
                        return;
                    }
                    sleeping_workers_.fetch_add(1u, std::memory_order_seq_cst);
                    cur = epoch_.load(std::memory_order_seq_cst);
                    if (cur == seen_epoch && !stop_flag_.load(std::memory_order_seq_cst)) {
                        epoch_.wait(seen_epoch, std::memory_order_acquire);
                    }
                    sleeping_workers_.fetch_sub(1u, std::memory_order_relaxed);
                    cur = epoch_.load(std::memory_order_acquire);
                }
            }
            if (stop_flag_.load(std::memory_order_acquire)) [[unlikely]] {
                return;
            }
            seen_epoch = cur;
            if (static_cast<int>(bg_id) < active_bg_) {
                run_work(static_cast<int>(bg_id + 1u));
            }
            if (active_rem_.fetch_sub(1, std::memory_order_seq_cst) == 1) {
                if (main_waiting_.load(std::memory_order_seq_cst)) [[unlikely]] {
                    active_rem_.notify_one();
                }
            }
        }
    }

    // Group 1: Read-only per-epoch dispatch metadata + wakeup epoch
    alignas(kCacheLine) std::atomic<NativeWaitInt> epoch_{0};
    std::atomic<bool> stop_flag_{false};
    std::atomic<uint32_t> sleeping_workers_{0};
    std::atomic<bool> main_waiting_{false};
    size_t total_items_ = 0;
    size_t grain_ = 1;
    int active_bg_ = 0;
    void* fn_ctx_ = nullptr;
    void (*fn_ptr_)(void*, size_t, size_t, int) = nullptr;

    // Group 2: Contended work-stealing counter isolated on its own coherency line
    alignas(kCacheLine) std::atomic<size_t> next_idx_{0};

    // Group 3: Completion counter isolated on its own coherency line
    alignas(kCacheLine) std::atomic<NativeWaitInt> active_rem_{0};

#if defined(__cpp_lib_jthread) && __cpp_lib_jthread >= 201911L
    std::vector<std::jthread> threads_;
#else
    std::vector<std::thread> threads_;
#endif
};

inline std::mutex& global_pool_dispatch_mutex() {
    static std::mutex mtx;
    return mtx;
}

inline WorkerPool& global_worker_pool_instance() {
    static WorkerPool pool;
    return pool;
}

struct PoolLease {
    std::unique_lock<std::mutex> global_lk;
    std::unique_ptr<WorkerPool> fallback_pool;
    WorkerPool* pool = nullptr;

    explicit PoolLease(int num_workers, WorkerPool* external_pool = nullptr) {
        if (num_workers > 1) {
            if (external_pool != nullptr) {
                pool = external_pool;
                return;
            }
            std::unique_lock<std::mutex> lk(global_pool_dispatch_mutex(), std::try_to_lock);
            if (lk.owns_lock()) {
                global_lk = std::move(lk);
                pool = &global_worker_pool_instance();
                return;
            }
            fallback_pool.reset(new WorkerPool());
            pool = fallback_pool.get();
        }
    }

    template <typename Fn>
        requires std::invocable<Fn&, size_t, size_t, int>
    void parallel_chunks(size_t total, size_t grain, int num_workers, Fn&& fn) {
        if (total == 0u) {
            return;
        }
        if (!pool || num_workers <= 1 || total <= grain) {
            fn(0u, total, 0);
            return;
        }
        pool->parallel_for_chunks(total, grain, num_workers, fn);
    }

    template <typename Fn>
        requires std::invocable<Fn&, size_t, size_t, int>
    void parallel_strips(size_t total_rows, size_t min_rows, int num_workers, Fn&& fn) {
        if (total_rows == 0u) {
            return;
        }
        if (!pool || num_workers <= 1 || total_rows <= min_rows) {
            fn(0u, total_rows, 0);
            return;
        }
        const size_t max_chunks = std::max<size_t>(1u, total_rows / std::max<size_t>(1u, min_rows));
        const size_t chunks = std::min<size_t>(max_chunks, static_cast<size_t>(num_workers) * 4u);
        if (chunks <= 1u) {
            fn(0u, total_rows, 0);
            return;
        }
        parallel_chunks(chunks, 1u, num_workers, [&](size_t c0, size_t c1, int worker_idx) {
            for (size_t c = c0; c < c1; ++c) {
                const size_t r0 = (c * total_rows) / chunks;
                const size_t r1 = ((c + 1u) * total_rows) / chunks;
                if (r0 < r1) {
                    fn(r0, r1, worker_idx);
                }
            }
        });
    }
};

} // namespace detail
} // namespace nijinwvdc

#endif // NIJINWVDC_WORKER_POOL_H
