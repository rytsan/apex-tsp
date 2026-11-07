#pragma once

#include <atomic>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Engine <-> GUI wire protocol (MASTER §2 directive 6, Sprint 5 M5.1). Little-endian on the wire;
// both supported targets (x86-64 Linux / Windows) are little-endian, so packed structs are copied
// as bytes.
static_assert(std::endian::native == std::endian::little, "wire format assumes a little-endian host");

namespace apex::ipc {

inline constexpr uint16_t kProtocolVersion = 2;
inline constexpr uint16_t kProtocolMin = 2;
inline constexpr uint32_t kHeaderBytes = 8;                 // u32 length + u16 version + u8 type + u8 flags
inline constexpr uint32_t kMaxFrameBytes = 64u << 20;       // larger length fields are malformed
inline constexpr uint8_t kFlagCrc = 0x01;                   // payload followed by CRC32(payload)

enum class MsgType : uint8_t {
    Hello = 0x01,
    Close = 0x02,      // payload: UTF-8 reason
    // GUI -> engine control, payload: u64 seq; answered with Ack(seq)
    Pause = 0x10,
    Resume = 0x11,
    Cancel = 0x12,
    Checkpoint = 0x13,
    Shutdown = 0x14,
    Ack = 0x1F,        // payload: u64 seq
    // engine -> GUI
    Telemetry = 0x20,  // payload: TelemetryPacket
    TourUpdate = 0x21, // payload: u64 shared-tour epoch + u32 count (the tour itself is in SharedTour)
    ClusterMap = 0x22, // payload: u32 count + count x {u32 cluster_id, u8 state}
    Log = 0x23,        // payload: UTF-8 line
    Finished = 0x24,   // payload: the --report JSON, byte-identical
};
bool known_type(uint8_t t);
bool is_control(MsgType t);
bool payload_size_ok(MsgType t, size_t payload_bytes);

uint32_t crc32(std::span<const uint8_t> data, uint32_t crc = 0);

struct Frame {
    uint16_t version = kProtocolVersion;
    MsgType type = MsgType::Log;
    uint8_t flags = 0;
    std::vector<uint8_t> payload;
};

// length = bytes after the length field (4 header bytes + payload + 4 if CRC).
std::vector<uint8_t> encode(const Frame& f);
std::vector<uint8_t> encode(MsgType t, std::span<const uint8_t> payload, bool with_crc = false);

// Streaming decoder. A header is accepted only if length is in range, version is in
// [kProtocolMin, kProtocolVersion], the type is known, no unknown flag bit is set and the
// payload size fits the type (payload_size_ok); a frame
// is dropped if its CRC fails. On any malformed header or CRC failure the decoder skips one byte
// and resynchronises, so garbage never stops later valid frames. Memory is bounded by
// kMaxFrameBytes.
class FrameDecoder {
public:
    void feed(std::span<const uint8_t> bytes);
    std::optional<Frame> next();
    uint64_t dropped_bytes() const { return dropped_bytes_; }
    uint64_t crc_failures() const { return crc_failures_; }
    size_t buffered() const { return buf_.size() - pos_; }

private:
    std::vector<uint8_t> buf_;
    size_t pos_ = 0;
    uint64_t dropped_bytes_ = 0, crc_failures_ = 0;
};

// ---- payloads -----------------------------------------------------------------------------

#pragma pack(push, 1)
// MASTER v2 telemetry struct; the C# client (ApexTsp.Ipc) asserts the same size.
struct TelemetryPacket {
    uint64_t magic_version;   // 'ATSP' << 32 | protocol version
    uint64_t seq;             // monotonic; readers drop duplicates / reordering
    uint64_t timestamp_us;
    uint32_t mode;            // 0 exact, 1 scalable
    uint32_t lb_source;       // 0 lp, 1 lagrangian
    uint32_t phase;
    uint32_t open_nodes;
    uint32_t active_edges;
    uint32_t pruned_edges_rcf;
    uint32_t cut_pool_size;
    double upper_bound;
    double lower_bound;
    double gap_pct;
    uint64_t simplex_iterations;
    uint64_t refactorizations;
    double iter_per_sec;
    uint64_t ram_bytes;
    uint64_t cold_bytes;
    uint32_t threads_lp;
};
#pragma pack(pop)
static_assert(sizeof(TelemetryPacket) == 120, "TelemetryPacket layout is part of the protocol");
inline constexpr uint64_t kTelemetryMagic = (uint64_t{0x41545350} << 32) | kProtocolVersion;  // "ATSP"

struct Hello {
    uint16_t version = kProtocolVersion;
    uint16_t protocol_min = kProtocolMin;
    uint8_t mode = 0;             // engine_config
    uint32_t mem_limit_mb = 0;
    uint32_t threads = 0;
    uint32_t gui_capabilities = 0;  // bit set, 0 from the engine
};
inline constexpr size_t kHelloBytes = 17;

std::vector<uint8_t> pack(const Hello& h);
std::optional<Hello> unpack_hello(std::span<const uint8_t> p);
// Highest version both sides speak, or 0 => send Close("version mismatch ...").
uint16_t negotiate(const Hello& a, const Hello& b);

std::vector<uint8_t> pack(const TelemetryPacket& t);
std::optional<TelemetryPacket> unpack_telemetry(std::span<const uint8_t> p);
std::vector<uint8_t> pack_u64(uint64_t v);
std::optional<uint64_t> unpack_u64(std::span<const uint8_t> p);

// ---- emission policy ----------------------------------------------------------------------

// 1 Hz heartbeat + immediate packet when UB drops or LB rises.
class TelemetryThrottle {
public:
    using Clock = std::chrono::steady_clock;
    bool should_emit(Clock::time_point now, double ub, double lb);

private:
    bool first_ = true;
    Clock::time_point last_{};
    double ub_ = 0, lb_ = 0;
};

// Outgoing queue of the server thread. Coalescing channels (telemetry, tour update, cluster map)
// keep only their newest pending frame: max one in flight per channel. Other frames (log, ack,
// finished, close) are FIFO up to `capacity`; beyond that they are dropped and counted. push()
// never waits for the consumer.
class OutQueue {
public:
    explicit OutQueue(size_t capacity = 256) : cap_(capacity) {}
    bool push(Frame f);                           // false = dropped
    std::optional<Frame> pop(std::chrono::milliseconds wait);
    void close();
    uint64_t dropped() const { return dropped_.load(std::memory_order_relaxed); }
    uint64_t coalesced() const { return coalesced_.load(std::memory_order_relaxed); }

private:
    static int slot(MsgType t);
    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<Frame> fifo_;
    std::optional<Frame> latest_[3];
    uint64_t order_ = 0, latest_order_[3] = {0, 0, 0};
    size_t cap_;
    bool closed_ = false;
    std::atomic<uint64_t> dropped_{0}, coalesced_{0};
};

// ---- shared route memory ------------------------------------------------------------------

// Seqlock over {u64 epoch, u32 count, u32 capacity} + u32 tour[capacity]. Writer: epoch odd,
// write (release stores), epoch even (release). Reader: epoch (acquire), words (acquire), epoch
// again; retries on an odd or changed epoch. All words are atomic, so access is race-free.
struct SharedTourHeader {
    uint64_t epoch;
    uint32_t count;
    uint32_t capacity;
};
static_assert(sizeof(SharedTourHeader) == 16);
inline size_t shared_tour_bytes(uint32_t capacity) { return sizeof(SharedTourHeader) + size_t{capacity} * 4; }

class SharedTourView {
public:
    SharedTourView() = default;
    explicit SharedTourView(void* base) : base_(static_cast<uint8_t*>(base)) {}
    void init(uint32_t capacity);                          // writer, once
    uint32_t capacity() const;
    uint64_t write(std::span<const uint32_t> tour);        // returns the new (even) epoch
    // Consistent snapshot, false if no tour was written yet. Retries at most `max_retries` times.
    bool read(std::vector<uint32_t>& out, uint64_t* epoch = nullptr, uint32_t max_retries = 1000000) const;

private:
    std::atomic_ref<uint64_t> epoch() const;
    uint32_t* words() const { return reinterpret_cast<uint32_t*>(base_ + sizeof(SharedTourHeader)); }
    uint8_t* base_ = nullptr;
};

// mmap-backed file (cold-data policy, MASTER §2 directive 4): CreateFileMapping on Windows,
// mmap(MAP_SHARED) elsewhere.
class MappedFile {
public:
    static std::optional<MappedFile> create(const std::string& path, size_t bytes);
    static std::optional<MappedFile> open(const std::string& path);
    MappedFile(MappedFile&& o) noexcept;
    MappedFile& operator=(MappedFile&& o) noexcept;
    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;
    ~MappedFile();
    void* data() const { return data_; }
    size_t size() const { return size_; }

private:
    MappedFile() = default;
    void* data_ = nullptr;
    size_t size_ = 0;
#ifdef _WIN32
    void* file_ = nullptr;
    void* mapping_ = nullptr;
#else
    int fd_ = -1;
#endif
};

}  // namespace apex::ipc
