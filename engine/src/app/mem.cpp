#include "app/mem.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

#if defined(_WIN32)
#include <windows.h>
#include <psapi.h>
#else
#include <sys/resource.h>
#endif

namespace apex {

uint64_t current_rss_bytes() {
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS pmc{};
    return GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc) ? pmc.WorkingSetSize : 0;
#else
    std::FILE* f = std::fopen("/proc/self/status", "r");
    if (!f) return 0;
    char line[256];
    uint64_t kb = 0;
    while (std::fgets(line, sizeof line, f))
        if (std::strncmp(line, "VmRSS:", 6) == 0) {
            std::sscanf(line + 6, "%llu", reinterpret_cast<unsigned long long*>(&kb));
            break;
        }
    std::fclose(f);
    return kb * 1024;
#endif
}

MemEnforcement enforce_memory_limit(uint64_t limit_bytes) {
    MemEnforcement m;
    if (kSanitizerBuild) {
        m.method = "none (sanitizer build)";
        return m;
    }
#if defined(_WIN32)
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (!job) return m;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
    info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_PROCESS_MEMORY;
    info.ProcessMemoryLimit = static_cast<SIZE_T>(limit_bytes);
    if (SetInformationJobObject(job, JobObjectExtendedLimitInformation, &info, sizeof info) &&
        AssignProcessToJobObject(job, GetCurrentProcess())) {
        m.enforced = true;
        m.method = "job_object";
        m.os_limit_bytes = limit_bytes;
    }
#else
    rlimit rl{};
    if (getrlimit(RLIMIT_AS, &rl) == 0) {
        const rlim_t want = static_cast<rlim_t>(limit_bytes + kAddressSlack);
        rl.rlim_cur = rl.rlim_max == RLIM_INFINITY ? want : std::min(want, rl.rlim_max);
        if (setrlimit(RLIMIT_AS, &rl) == 0) {
            m.enforced = true;
            m.method = "setrlimit(RLIMIT_AS)";
            m.os_limit_bytes = static_cast<uint64_t>(rl.rlim_cur);
        }
    }
#endif
    if (!m.enforced) m.method = "none";
    return m;
}

void check_memory(uint64_t limit_bytes, const char* phase) {
    const uint64_t rss = current_rss_bytes();
    if (limit_bytes > 0 && rss >= limit_bytes / 10 * 9)
        throw MemLimitExceeded(std::string("memory budget exhausted at ") + phase + ": rss " + std::to_string(rss) +
                               " bytes of " + std::to_string(limit_bytes));
}

void LpBudget::update(uint64_t baseline_bytes, uint64_t lp_working_set_bytes) {
    baseline_ = baseline_bytes;
    ws_ = std::max<uint64_t>(1, lp_working_set_bytes);
    const uint64_t free_budget = limit_ > baseline_ ? limit_ - baseline_ : 0;
    tokens_ = static_cast<uint32_t>(std::min<uint64_t>(max_lps_, free_budget / ws_));
}

void LpBudget::acquire() {
    const uint32_t now = active_.fetch_add(1) + 1;
    uint32_t seen = max_seen_.load();
    while (now > seen && !max_seen_.compare_exchange_weak(seen, now)) {
    }
    if (now > tokens_) violations_.fetch_add(1);
}

}  // namespace apex
