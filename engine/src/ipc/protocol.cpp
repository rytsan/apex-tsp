#include "ipc/protocol.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <utility>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace apex::ipc {

namespace {

void put_u16(std::vector<uint8_t>& b, uint16_t v) {
    b.push_back(static_cast<uint8_t>(v));
    b.push_back(static_cast<uint8_t>(v >> 8));
}
void put_u32(std::vector<uint8_t>& b, uint32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>(v >> (8 * i)));
}
uint16_t get_u16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t get_u32(const uint8_t* p) {
    return uint32_t{p[0]} | (uint32_t{p[1]} << 8) | (uint32_t{p[2]} << 16) | (uint32_t{p[3]} << 24);
}

constexpr std::array<uint32_t, 256> make_crc_table() {
    std::array<uint32_t, 256> t{};
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = i;
        for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        t[i] = c;
    }
    return t;
}
constexpr auto kCrcTable = make_crc_table();

}  // namespace

bool known_type(uint8_t t) {
    switch (static_cast<MsgType>(t)) {
        case MsgType::Hello: case MsgType::Close: case MsgType::Pause: case MsgType::Resume:
        case MsgType::Cancel: case MsgType::Checkpoint: case MsgType::Shutdown: case MsgType::Ack:
        case MsgType::Telemetry: case MsgType::TourUpdate: case MsgType::ClusterMap: case MsgType::Log:
        case MsgType::Finished:
            return true;
    }
    return false;
}

// Payload size bounds per type: fixed-size types must match exactly, variable ones are capped.
// A random header then almost never passes (and never claims megabytes), so the decoder cannot
// be held waiting for a frame that garbage started.
bool payload_size_ok(MsgType t, size_t n) {
    switch (t) {
        case MsgType::Hello: return n == kHelloBytes;
        case MsgType::Pause: case MsgType::Resume: case MsgType::Cancel: case MsgType::Checkpoint:
        case MsgType::Shutdown: case MsgType::Ack: return n == 8;
        case MsgType::Telemetry: return n == sizeof(TelemetryPacket);
        case MsgType::TourUpdate: return n == 12;
        case MsgType::Close: return n <= 4096;
        case MsgType::Log: return n <= 65536;
        case MsgType::ClusterMap: case MsgType::Finished: return n <= kMaxFrameBytes - 8;
    }
    return false;
}

bool is_control(MsgType t) { return t >= MsgType::Pause && t <= MsgType::Shutdown; }

// IEEE 802.3 (reflected, poly 0xEDB88320), same as zlib crc32().
uint32_t crc32(std::span<const uint8_t> data, uint32_t crc) {
    crc = ~crc;
    for (uint8_t b : data) crc = kCrcTable[(crc ^ b) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

std::vector<uint8_t> encode(MsgType t, std::span<const uint8_t> payload, bool with_crc) {
    std::vector<uint8_t> b;
    const uint32_t len = 4 + static_cast<uint32_t>(payload.size()) + (with_crc ? 4 : 0);
    b.reserve(4 + len);
    put_u32(b, len);
    put_u16(b, kProtocolVersion);
    b.push_back(static_cast<uint8_t>(t));
    b.push_back(with_crc ? kFlagCrc : 0);
    b.insert(b.end(), payload.begin(), payload.end());
    if (with_crc) put_u32(b, crc32(payload));
    return b;
}

std::vector<uint8_t> encode(const Frame& f) { return encode(f.type, f.payload, (f.flags & kFlagCrc) != 0); }

void FrameDecoder::feed(std::span<const uint8_t> bytes) {
    if (pos_ > 0 && pos_ * 2 >= buf_.size()) {  // compact
        buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(pos_));
        pos_ = 0;
    }
    buf_.insert(buf_.end(), bytes.begin(), bytes.end());
}

std::optional<Frame> FrameDecoder::next() {
    while (buf_.size() - pos_ >= kHeaderBytes) {
        const uint8_t* h = buf_.data() + pos_;
        const uint32_t len = get_u32(h);
        const uint16_t ver = get_u16(h + 4);
        const uint8_t type = h[6], flags = h[7];
        const uint32_t min_len = 4 + ((flags & kFlagCrc) ? 4u : 0u);
        if (len < min_len || len > kMaxFrameBytes || ver < kProtocolMin || ver > kProtocolVersion ||
            !known_type(type) || (flags & ~kFlagCrc) || !payload_size_ok(static_cast<MsgType>(type), len - min_len)) {
            ++pos_;
            ++dropped_bytes_;
            continue;
        }
        if (buf_.size() - pos_ < 4 + size_t{len}) return std::nullopt;  // wait for the rest
        const size_t plen = len - min_len;
        const uint8_t* p = h + kHeaderBytes;
        if ((flags & kFlagCrc) && crc32({p, plen}) != get_u32(p + plen)) {
            ++crc_failures_;
            ++pos_;
            ++dropped_bytes_;
            continue;
        }
        Frame f;
        f.version = ver;
        f.type = static_cast<MsgType>(type);
        f.flags = flags;
        f.payload.assign(p, p + plen);
        pos_ += 4 + size_t{len};
        return f;
    }
    return std::nullopt;
}

// ---- payloads -----------------------------------------------------------------------------

std::vector<uint8_t> pack(const Hello& h) {
    std::vector<uint8_t> b;
    put_u16(b, h.version);
    put_u16(b, h.protocol_min);
    b.push_back(h.mode);
    put_u32(b, h.mem_limit_mb);
    put_u32(b, h.threads);
    put_u32(b, h.gui_capabilities);
    return b;
}

std::optional<Hello> unpack_hello(std::span<const uint8_t> p) {
    if (p.size() != kHelloBytes) return std::nullopt;
    Hello h;
    h.version = get_u16(p.data());
    h.protocol_min = get_u16(p.data() + 2);
    h.mode = p[4];
    h.mem_limit_mb = get_u32(p.data() + 5);
    h.threads = get_u32(p.data() + 9);
    h.gui_capabilities = get_u32(p.data() + 13);
    if (h.protocol_min > h.version) return std::nullopt;
    return h;
}

uint16_t negotiate(const Hello& a, const Hello& b) {
    const uint16_t hi = std::min(a.version, b.version), lo = std::max(a.protocol_min, b.protocol_min);
    return hi >= lo ? hi : 0;
}

std::vector<uint8_t> pack(const TelemetryPacket& t) {
    std::vector<uint8_t> b(sizeof t);
    std::memcpy(b.data(), &t, sizeof t);
    return b;
}

std::optional<TelemetryPacket> unpack_telemetry(std::span<const uint8_t> p) {
    if (p.size() != sizeof(TelemetryPacket)) return std::nullopt;
    TelemetryPacket t;
    std::memcpy(&t, p.data(), sizeof t);
    if (t.magic_version != kTelemetryMagic) return std::nullopt;
    return t;
}

std::vector<uint8_t> pack_u64(uint64_t v) {
    std::vector<uint8_t> b(8);
    std::memcpy(b.data(), &v, 8);
    return b;
}

std::optional<uint64_t> unpack_u64(std::span<const uint8_t> p) {
    if (p.size() != 8) return std::nullopt;
    uint64_t v;
    std::memcpy(&v, p.data(), 8);
    return v;
}

// ---- emission policy ----------------------------------------------------------------------

bool TelemetryThrottle::should_emit(Clock::time_point now, double ub, double lb) {
    const bool emit = first_ || ub < ub_ || lb > lb_ || now - last_ >= std::chrono::seconds(1);
    if (emit) {
        first_ = false;
        last_ = now;
        ub_ = ub;
        lb_ = lb;
    }
    return emit;
}

int OutQueue::slot(MsgType t) {
    switch (t) {
        case MsgType::Telemetry: return 0;
        case MsgType::TourUpdate: return 1;
        case MsgType::ClusterMap: return 2;
        default: return -1;
    }
}

bool OutQueue::push(Frame f) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (closed_) { dropped_.fetch_add(1, std::memory_order_relaxed); return false; }
        const int s = slot(f.type);
        if (s >= 0) {
            if (latest_[s]) coalesced_.fetch_add(1, std::memory_order_relaxed);
            else latest_order_[s] = ++order_;  // keeps its place in line when replaced
            latest_[s] = std::move(f);
        } else {
            if (fifo_.size() >= cap_) { dropped_.fetch_add(1, std::memory_order_relaxed); return false; }
            ++order_;
            fifo_.push_back(std::move(f));
        }
    }
    cv_.notify_one();
    return true;
}

std::optional<Frame> OutQueue::pop(std::chrono::milliseconds wait) {
    std::unique_lock<std::mutex> lk(mu_);
    auto ready = [&] { return closed_ || !fifo_.empty() || latest_[0] || latest_[1] || latest_[2]; };
    if (!cv_.wait_for(lk, wait, ready)) return std::nullopt;
    // Oldest coalesced slot first, unless the FIFO has anything; FIFO frames are rare and small.
    if (!fifo_.empty()) {
        Frame f = std::move(fifo_.front());
        fifo_.pop_front();
        return f;
    }
    int best = -1;
    for (int s = 0; s < 3; ++s)
        if (latest_[s] && (best < 0 || latest_order_[s] < latest_order_[best])) best = s;
    if (best < 0) return std::nullopt;  // closed and empty
    Frame f = std::move(*latest_[best]);
    latest_[best].reset();
    return f;
}

void OutQueue::close() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        closed_ = true;
    }
    cv_.notify_all();
}

// ---- shared route memory ------------------------------------------------------------------

std::atomic_ref<uint64_t> SharedTourView::epoch() const {
    return std::atomic_ref<uint64_t>(reinterpret_cast<SharedTourHeader*>(base_)->epoch);
}

void SharedTourView::init(uint32_t capacity) {
    auto* h = reinterpret_cast<SharedTourHeader*>(base_);
    std::atomic_ref<uint32_t>(h->count).store(0, std::memory_order_relaxed);
    std::atomic_ref<uint32_t>(h->capacity).store(capacity, std::memory_order_relaxed);
    epoch().store(0, std::memory_order_release);
}

uint32_t SharedTourView::capacity() const {
    return std::atomic_ref<uint32_t>(reinterpret_cast<SharedTourHeader*>(base_)->capacity).load(std::memory_order_relaxed);
}

uint64_t SharedTourView::write(std::span<const uint32_t> tour) {
    auto* h = reinterpret_cast<SharedTourHeader*>(base_);
    const auto n = static_cast<uint32_t>(std::min<size_t>(tour.size(), capacity()));
    auto ep = epoch();
    const uint64_t e = ep.load(std::memory_order_relaxed);
    // Release stores of the data keep the odd epoch ordered before them (no fences: TSan
    // does not model atomic_thread_fence). On x86 these are plain stores.
    ep.store(e + 1, std::memory_order_relaxed);
    std::atomic_ref<uint32_t>(h->count).store(n, std::memory_order_release);
    uint32_t* w = words();
    for (uint32_t i = 0; i < n; ++i) std::atomic_ref<uint32_t>(w[i]).store(tour[i], std::memory_order_release);
    ep.store(e + 2, std::memory_order_release);
    return e + 2;
}

bool SharedTourView::read(std::vector<uint32_t>& out, uint64_t* epoch_out, uint32_t max_retries) const {
    auto* h = reinterpret_cast<SharedTourHeader*>(base_);
    const uint32_t cap = capacity();
    auto ep = epoch();
    uint32_t* w = words();
    for (uint32_t attempt = 0; attempt <= max_retries; ++attempt) {
        const uint64_t e1 = ep.load(std::memory_order_acquire);
        if (e1 & 1) continue;
        if (e1 == 0) return false;
        // Acquire loads: a word from a newer write makes that write's odd epoch visible below.
        const uint32_t n = std::min(std::atomic_ref<uint32_t>(h->count).load(std::memory_order_acquire), cap);
        out.resize(n);
        for (uint32_t i = 0; i < n; ++i) out[i] = std::atomic_ref<uint32_t>(w[i]).load(std::memory_order_acquire);
        if (ep.load(std::memory_order_relaxed) == e1) {
            if (epoch_out) *epoch_out = e1;
            return true;
        }
    }
    return false;
}

// ---- mapped file --------------------------------------------------------------------------

MappedFile::MappedFile(MappedFile&& o) noexcept { *this = std::move(o); }

MappedFile& MappedFile::operator=(MappedFile&& o) noexcept {
    if (this == &o) return *this;
    this->~MappedFile();
    data_ = std::exchange(o.data_, nullptr);
    size_ = std::exchange(o.size_, 0);
#ifdef _WIN32
    file_ = std::exchange(o.file_, nullptr);
    mapping_ = std::exchange(o.mapping_, nullptr);
#else
    fd_ = std::exchange(o.fd_, -1);
#endif
    return *this;
}

#ifdef _WIN32

std::optional<MappedFile> MappedFile::create(const std::string& path, size_t bytes) {
    HANDLE f = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return std::nullopt;
    const auto size = static_cast<ULONGLONG>(bytes);
    HANDLE m = CreateFileMappingA(f, nullptr, PAGE_READWRITE, static_cast<DWORD>(size >> 32),
                                  static_cast<DWORD>(size & 0xFFFFFFFFu), nullptr);
    if (!m) { CloseHandle(f); return std::nullopt; }
    void* d = MapViewOfFile(m, FILE_MAP_ALL_ACCESS, 0, 0, bytes);
    if (!d) { CloseHandle(m); CloseHandle(f); return std::nullopt; }
    std::memset(d, 0, bytes);
    MappedFile mf;
    mf.data_ = d;
    mf.size_ = bytes;
    mf.file_ = f;
    mf.mapping_ = m;
    return mf;
}

std::optional<MappedFile> MappedFile::open(const std::string& path) {
    HANDLE f = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return std::nullopt;
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(f, &sz) || sz.QuadPart < static_cast<LONGLONG>(sizeof(SharedTourHeader))) {
        CloseHandle(f);
        return std::nullopt;
    }
    HANDLE m = CreateFileMappingA(f, nullptr, PAGE_READWRITE, 0, 0, nullptr);
    if (!m) { CloseHandle(f); return std::nullopt; }
    void* d = MapViewOfFile(m, FILE_MAP_ALL_ACCESS, 0, 0, 0);
    if (!d) { CloseHandle(m); CloseHandle(f); return std::nullopt; }
    MappedFile mf;
    mf.data_ = d;
    mf.size_ = static_cast<size_t>(sz.QuadPart);
    mf.file_ = f;
    mf.mapping_ = m;
    return mf;
}

MappedFile::~MappedFile() {
    if (data_) UnmapViewOfFile(data_);
    if (mapping_) CloseHandle(mapping_);
    if (file_) CloseHandle(file_);
    data_ = mapping_ = file_ = nullptr;
}

#else

std::optional<MappedFile> MappedFile::create(const std::string& path, size_t bytes) {
    const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return std::nullopt;
    if (::ftruncate(fd, static_cast<off_t>(bytes)) != 0) { ::close(fd); return std::nullopt; }
    void* d = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (d == MAP_FAILED) { ::close(fd); return std::nullopt; }
    MappedFile mf;
    mf.data_ = d;
    mf.size_ = bytes;
    mf.fd_ = fd;
    return mf;  // ftruncate zero-fills
}

std::optional<MappedFile> MappedFile::open(const std::string& path) {
    const int fd = ::open(path.c_str(), O_RDWR);
    if (fd < 0) return std::nullopt;
    struct stat st{};
    if (::fstat(fd, &st) != 0 || st.st_size < static_cast<off_t>(sizeof(SharedTourHeader))) {
        ::close(fd);
        return std::nullopt;
    }
    const auto bytes = static_cast<size_t>(st.st_size);
    void* d = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (d == MAP_FAILED) { ::close(fd); return std::nullopt; }
    MappedFile mf;
    mf.data_ = d;
    mf.size_ = bytes;
    mf.fd_ = fd;
    return mf;
}

MappedFile::~MappedFile() {
    if (data_) ::munmap(data_, size_);
    if (fd_ >= 0) ::close(fd_);
    data_ = nullptr;
    fd_ = -1;
}

#endif

}  // namespace apex::ipc
