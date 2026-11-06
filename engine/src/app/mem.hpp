#pragma once

// Memory enforcement + LP concurrency budget (M4.5, MASTER §2.3).

#include <atomic>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace apex {

// Resident set now (Linux VmRSS, Windows WorkingSetSize). Peak: peak_rss_bytes() (VmHWM /
// PeakWorkingSetSize, core/tour_io.hpp).
uint64_t current_rss_bytes();

struct MemEnforcement {
    bool enforced = false;
    std::string method;          // "setrlimit(RLIMIT_AS)" | "job_object" | "none"
    uint64_t os_limit_bytes = 0; // the OS cap actually set
};

// Hard OS cap applied at startup. Linux: RLIMIT_AS = limit + kAddressSlack (address space also
// holds thread stacks, malloc arena reservations and code, which are not resident memory).
// Windows: Job Object with JOB_OBJECT_LIMIT_PROCESS_MEMORY = limit. The OS cap is the backstop;
// the primary path is the graceful MEM_LIMIT stop driven by the budget below.
inline constexpr uint64_t kAddressSlack = uint64_t(1) << 30;
// ASan / TSan reserve terabytes of shadow address space: no RLIMIT_AS in those builds.
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
inline constexpr bool kSanitizerBuild = true;
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
inline constexpr bool kSanitizerBuild = true;
#else
inline constexpr bool kSanitizerBuild = false;
#endif
#else
inline constexpr bool kSanitizerBuild = false;
#endif
MemEnforcement enforce_memory_limit(uint64_t limit_bytes);

// Thrown by guards when the budget is exhausted; callers checkpoint and report MEM_LIMIT.
struct MemLimitExceeded : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// Guard at phase boundaries: throws MemLimitExceeded once RSS reaches 90% of the limit.
void check_memory(uint64_t limit_bytes, const char* phase);

// LP concurrency tokens: floor((limit - baseline) / lp_working_set), capped at max_lps.
// Callers recompute it from live sizes (model structures, LP nnz, factorization nnz) so the
// count is deterministic for a deterministic run. 0 tokens = budget exhausted.
class LpBudget {
public:
    explicit LpBudget(uint64_t limit_bytes, uint32_t max_lps = 6) : limit_(limit_bytes), max_lps_(max_lps) {}
    void update(uint64_t baseline_bytes, uint64_t lp_working_set_bytes);
    uint32_t tokens() const { return tokens_; }
    uint64_t limit() const { return limit_; }
    uint64_t lp_working_set() const { return ws_; }
    uint64_t baseline() const { return baseline_; }
    // Concurrency accounting (thread-safe): acquire before an LP worker starts, release after.
    void acquire();
    void release() { active_.fetch_sub(1); }
    uint32_t max_concurrent() const { return max_seen_.load(); }
    uint64_t violations() const { return violations_.load(); }  // acquires beyond tokens()

private:
    uint64_t limit_, baseline_ = 0, ws_ = 0;
    uint32_t max_lps_, tokens_ = 1;
    std::atomic<uint32_t> active_{0}, max_seen_{0};
    std::atomic<uint64_t> violations_{0};
};

}  // namespace apex
