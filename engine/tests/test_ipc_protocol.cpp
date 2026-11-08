#include <doctest/doctest.h>

#include <atomic>
#include <filesystem>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "ipc/protocol.hpp"

using namespace apex::ipc;
namespace fs = std::filesystem;

namespace {
std::span<const uint8_t> bytes(std::string_view s) { return {reinterpret_cast<const uint8_t*>(s.data()), s.size()}; }

TelemetryPacket sample_packet(uint64_t seq) {
    TelemetryPacket t{};
    t.magic_version = kTelemetryMagic;
    t.seq = seq;
    t.upper_bound = 143332990;
    t.lower_bound = 141590000.5;
    t.gap_pct = 1.217;
    t.threads_lp = 5;
    return t;
}
}  // namespace

TEST_CASE("T_crc32_known_vectors") {
    CHECK(crc32(bytes("")) == 0u);
    CHECK(crc32(bytes("a")) == 0xE8B7BE43u);
    CHECK(crc32(bytes("123456789")) == 0xCBF43926u);
    CHECK(crc32(bytes("The quick brown fox jumps over the lazy dog")) == 0x414FA339u);
    // incremental == one shot
    CHECK(crc32(bytes("6789"), crc32(bytes("12345"))) == 0xCBF43926u);
}

TEST_CASE("T_frame_roundtrip_and_handshake") {
    FrameDecoder d;
    const auto tel = pack(sample_packet(7));
    for (bool crc : {false, true}) {
        const auto wire = encode(MsgType::Telemetry, tel, crc);
        // feed one byte at a time: partial frames wait
        for (size_t i = 0; i + 1 < wire.size(); ++i) {
            d.feed({&wire[i], 1});
            CHECK_FALSE(d.next().has_value());
        }
        d.feed({&wire.back(), 1});
        auto f = d.next();
        REQUIRE(f.has_value());
        CHECK(f->type == MsgType::Telemetry);
        CHECK(f->flags == (crc ? kFlagCrc : 0));
        auto t = unpack_telemetry(f->payload);
        REQUIRE(t.has_value());
        const uint64_t seq = t->seq;  // copies: no references into the packed struct
        const double gap = t->gap_pct;
        CHECK(seq == 7);
        CHECK(gap == 1.217);
    }
    CHECK(d.dropped_bytes() == 0);

    Hello eng{.mode = 1, .mem_limit_mb = 4096, .threads = 5}, gui{.gui_capabilities = 3};
    auto he = unpack_hello(pack(eng));
    REQUIRE(he.has_value());
    CHECK(he->mem_limit_mb == 4096);
    CHECK(he->threads == 5);
    CHECK(he->mode == 1);
    CHECK(negotiate(*he, gui) == kProtocolVersion);
    Hello old{.version = 1, .protocol_min = 1};
    CHECK(negotiate(eng, old) == 0);  // mismatch => Close
    Hello bad{.version = 1, .protocol_min = 2};
    CHECK_FALSE(unpack_hello(pack(bad)).has_value());
    CHECK_FALSE(unpack_hello(std::vector<uint8_t>(3)).has_value());
}

TEST_CASE("T_framing_fuzz") {
    std::mt19937_64 rng(2026);
    FrameDecoder d;
    uint64_t frames = 0;
    // 1 MB of random bytes, in random chunk sizes
    std::vector<uint8_t> junk(1 << 20);
    for (auto& b : junk) b = static_cast<uint8_t>(rng());
    for (size_t i = 0; i < junk.size();) {
        const size_t k = std::min<size_t>(junk.size() - i, 1 + rng() % 4096);
        d.feed({junk.data() + i, k});
        while (d.next()) ++frames;
        i += k;
    }
    // corrupted valid frames: bit flips in CRC frames, truncations, wrong payload sizes
    const auto tel = pack(sample_packet(1));
    for (int i = 0; i < 2000; ++i) {
        auto w = encode(MsgType::Telemetry, tel, true);
        w[8 + rng() % tel.size()] ^= static_cast<uint8_t>(1u << (rng() % 8));
        d.feed(w);
        while (auto f = d.next()) {
            ++frames;
            CHECK(f->type != MsgType::Telemetry);  // flipped payload never passes the CRC
        }
    }
    CHECK(d.crc_failures() >= 1);
    // wrong payload sizes for fixed-size types are rejected at the header
    const uint64_t before = d.dropped_bytes();
    for (auto bad : {encode(MsgType::Telemetry, std::vector<uint8_t>(5)), encode(MsgType::Hello, std::vector<uint8_t>(2)),
                     encode(MsgType::Pause, std::vector<uint8_t>(9))}) {
        d.feed(bad);
        while (d.next()) ++frames;
    }
    CHECK(d.dropped_bytes() > before);
    MESSAGE("fuzz: frames accepted from garbage=" << frames << " dropped_bytes=" << d.dropped_bytes()
                                                    << " crc_failures=" << d.crc_failures() << " buffered=" << d.buffered());
    CHECK(d.buffered() < kHeaderBytes);  // no frame left pending on garbage
    CHECK(d.crc_failures() == 2000);      // every flipped frame was seen and rejected
    // still alive: a clean frame after the garbage decodes (drain any frame a garbage header started)
    const auto ok = encode(MsgType::Telemetry, pack(sample_packet(99)), true);
    bool seen = false;
    for (int rep = 0; rep < 64 && !seen; ++rep) {
        d.feed(ok);
        while (auto f = d.next())
            if (f->type == MsgType::Telemetry && unpack_telemetry(f->payload) && unpack_telemetry(f->payload)->seq == 99) seen = true;
    }
    CHECK(seen);
}

TEST_CASE("T_backpressure_drop_count") {
    OutQueue q(4);
    // coalescing channel: 1000 telemetry pushes keep one pending frame
    for (uint64_t s = 1; s <= 1000; ++s) {
        Frame f;
        f.type = MsgType::Telemetry;
        f.payload = pack(sample_packet(s));
        CHECK(q.push(std::move(f)));
    }
    CHECK(q.coalesced() == 999);
    // FIFO channel: capacity 4, the rest dropped and counted
    for (int i = 0; i < 10; ++i) {
        Frame f;
        f.type = MsgType::Log;
        f.payload.assign(1, static_cast<uint8_t>(i));
        q.push(std::move(f));
    }
    CHECK(q.dropped() == 6);
    int logs = 0, tels = 0;
    uint64_t last_seq = 0;
    while (auto f = q.pop(std::chrono::milliseconds(0))) {
        if (f->type == MsgType::Log) {
            CHECK(f->payload[0] == logs);  // FIFO order
            ++logs;
        } else {
            ++tels;
            last_seq = unpack_telemetry(f->payload).value().seq;
        }
    }
    CHECK(logs == 4);
    CHECK(tels == 1);
    CHECK(last_seq == 1000);  // newest wins

    // push never waits for a stalled consumer
    OutQueue slow(8);
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 100000; ++i) {
        Frame f;
        f.type = (i & 1) ? MsgType::Telemetry : MsgType::Log;
        slow.push(std::move(f));
    }
    CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(2));
    CHECK(slow.dropped() == 50000 - 8);
    slow.close();
    CHECK_FALSE(slow.push(Frame{}));
}

TEST_CASE("T_telemetry_throttle") {
    TelemetryThrottle th;
    const auto t0 = TelemetryThrottle::Clock::now();
    using std::chrono::milliseconds;
    CHECK(th.should_emit(t0, 100, 50));                       // first
    CHECK_FALSE(th.should_emit(t0 + milliseconds(10), 100, 50));
    CHECK(th.should_emit(t0 + milliseconds(20), 99, 50));      // UB improved: immediate
    CHECK(th.should_emit(t0 + milliseconds(30), 99, 51));      // LB improved: immediate
    CHECK_FALSE(th.should_emit(t0 + milliseconds(900), 99, 51));
    CHECK(th.should_emit(t0 + milliseconds(1030), 99, 51));    // heartbeat
}

TEST_CASE("T_seqlock_reader_consistent") {
    constexpr uint32_t cap = 64;
    const fs::path path = fs::temp_directory_path() / "apex_tsp_seqlock_test.tour";
    auto mf = MappedFile::create(path.string(), shared_tour_bytes(cap));
    REQUIRE(mf.has_value());
    SharedTourView w(mf->data());
    w.init(cap);
    auto rf = MappedFile::open(path.string());  // second mapping of the same file, as the GUI does
    REQUIRE(rf.has_value());
    SharedTourView r(rf->data());
    CHECK(r.capacity() == cap);
    std::vector<uint32_t> snap;
    CHECK_FALSE(r.read(snap));  // nothing written yet

    std::atomic<bool> done{false};
    std::thread writer([&] {
        std::vector<uint32_t> tour;
        for (uint32_t k = 1; !done.load(std::memory_order_relaxed); ++k) {
            tour.assign(1 + k % cap, k);  // count and contents both encode k
            w.write(tour);
        }
    });
    uint64_t torn = 0, last_epoch = 0, regressions = 0, reads = 0;
    for (int i = 0; i < 1000000; ++i) {
        uint64_t e = 0;
        if (!r.read(snap, &e)) continue;
        ++reads;
        const uint32_t k = snap[0];
        bool ok = snap.size() == 1 + k % cap;
        for (uint32_t v : snap) ok = ok && v == k;
        if (!ok) ++torn;
        if (e < last_epoch) ++regressions;
        last_epoch = e;
    }
    done = true;
    writer.join();
    MESSAGE("seqlock: reads=" << reads << " torn=" << torn << " last_epoch=" << last_epoch);
    CHECK(reads > 0);
    CHECK(torn == 0);
    CHECK(regressions == 0);
    CHECK(last_epoch % 2 == 0);
    rf.reset();
    mf.reset();
    std::error_code ec;
    fs::remove(path, ec);  // Windows may still hold the just-unmapped file
}
