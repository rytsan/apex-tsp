#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>

namespace apex {

// Process-wide run control (Sprint 5 M5.2, driven by the IPC server). No new solver semantics:
// - pause: long loops call pause_point() where they already check their time limit (simplex
//   iterations, ILS iterations, B&C batches, Held-Karp, 2-opt rounds); the calling thread sleeps
//   until resume, its state untouched, so an iteration-limited run is identical with or without
//   pauses.
// - cancel: treated as an expired time limit at the same checks (anytime results stay valid).
// - checkpoint: a request flag consumed at the next checkpoint point (B&C batch boundary; the
//   app also writes its own snapshot from the latest published tour).
class EngineControl {
public:
    void pause() { paused_.store(true, std::memory_order_release); }
    void resume() {
        {
            std::lock_guard<std::mutex> lk(mu_);
            paused_.store(false, std::memory_order_release);
        }
        cv_.notify_all();
    }
    void cancel() {
        {
            std::lock_guard<std::mutex> lk(mu_);
            cancel_.store(true, std::memory_order_release);
        }
        cv_.notify_all();
    }
    void request_checkpoint() { checkpoint_.store(true, std::memory_order_release); }
    bool take_checkpoint_request() { return checkpoint_.exchange(false, std::memory_order_acq_rel); }

    bool paused() const { return paused_.load(std::memory_order_acquire); }
    bool cancelled() const { return cancel_.load(std::memory_order_relaxed); }
    uint32_t waiting() const { return waiting_.load(std::memory_order_acquire); }  // threads asleep in pause_point
    uint64_t pauses_served() const { return served_.load(std::memory_order_relaxed); }

    void pause_point() {
        if (!paused_.load(std::memory_order_relaxed)) return;  // fast path: one relaxed load
        std::unique_lock<std::mutex> lk(mu_);
        waiting_.fetch_add(1, std::memory_order_acq_rel);
        served_.fetch_add(1, std::memory_order_relaxed);
        cv_.wait(lk, [&] { return !paused_.load(std::memory_order_relaxed) || cancel_.load(std::memory_order_relaxed); });
        waiting_.fetch_sub(1, std::memory_order_acq_rel);
    }

    void reset() {  // tests
        {
            std::lock_guard<std::mutex> lk(mu_);
            paused_ = false;
            cancel_ = false;
            checkpoint_ = false;
        }
        cv_.notify_all();
    }

private:
    std::atomic<bool> paused_{false}, cancel_{false}, checkpoint_{false};
    std::atomic<uint32_t> waiting_{0};
    std::atomic<uint64_t> served_{0};
    std::mutex mu_;
    std::condition_variable cv_;
};

inline EngineControl& engine_control() {
    static EngineControl c;
    return c;
}
inline void pause_point() { engine_control().pause_point(); }
inline bool cancel_requested() { return engine_control().cancelled(); }

}  // namespace apex
