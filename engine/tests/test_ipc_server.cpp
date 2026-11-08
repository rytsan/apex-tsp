#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

#include "core/control.hpp"
#include "core/instance.hpp"
#include "geo/hilbert.hpp"
#include "graph/candidate_graph.hpp"
#include "ipc/protocol.hpp"
#include "ipc/transport.hpp"
#include "primal/lk_lite.hpp"
#include "primal/two_opt.hpp"

namespace fs = std::filesystem;
using namespace apex::ipc;
using Clock = std::chrono::steady_clock;

namespace {

apex::Instance load(const std::string& name) {
    return apex::read_tsplib(std::string(APEX_DATA_DIR) + "/" + name + ".tsp");
}

double ms_since(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }

std::optional<Frame> next_frame(Transport& t, FrameDecoder& d, double timeout_s) {
    const auto end = Clock::now() + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(timeout_s));
    uint8_t buf[65536];
    for (;;) {
        if (auto f = d.next()) return f;
        if (Clock::now() >= end) return std::nullopt;
        const long got = t.read(buf, sizeof buf, 20);
        if (got < 0) return std::nullopt;
        if (got > 0) d.feed({buf, static_cast<size_t>(got)});
    }
}

void send(Transport& t, MsgType type, uint64_t seq) {
    const auto w = encode(type, pack_u64(seq), true);
    REQUIRE(t.write_all(w.data(), w.size()));
}

std::string as_text(const Frame& f) { return {f.payload.begin(), f.payload.end()}; }

}  // namespace

TEST_CASE("T_control_pause_resume") {
    auto& c = apex::engine_control();
    c.reset();
    const auto inst = load("a280");
    const auto g = apex::build_candidate_graph(inst, {8, true});
    const auto start = apex::hilbert_tour(inst, apex::hilbert_order(inst));
    apex::IlsConfig ic;
    ic.seed = 7;
    ic.max_iterations = 40000;
    apex::Tour ref = start;
    const auto t_ref = Clock::now();
    apex::iterated_local_search(ref, inst, g, ic);
    const double ref_ms = ms_since(t_ref);

    apex::Tour run = start;
    std::atomic<bool> done{false};
    std::thread th([&] {
        apex::iterated_local_search(run, inst, g, ic);
        done = true;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<int>(std::min(50.0, ref_ms / 4))));
    REQUIRE_FALSE(done.load());
    const auto tp = Clock::now();
    c.pause();
    while (c.waiting() == 0 && ms_since(tp) < 1000) std::this_thread::yield();
    const double stall_ms = ms_since(tp);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    const bool still_running = !done.load();  // asleep, not finishing
    const double paused_ms = ms_since(tp);
    c.resume();
    th.join();
    MESSAGE("pause: reference run " << ref_ms << " ms, stalled after " << stall_ms << " ms, held " << paused_ms << " ms");
    CHECK(stall_ms <= 200);
    CHECK(still_running);
    CHECK(c.waiting() == 0);
    // Resume continues the same trajectory: identical to the run without a pause.
    CHECK(run.cost == ref.cost);
    CHECK(run.order == ref.order);
    c.reset();
}

TEST_CASE("T_control_cancel") {
    auto& c = apex::engine_control();
    c.reset();
    const auto inst = load("a280");
    const auto g = apex::build_candidate_graph(inst, {8, true});
    apex::Tour t = apex::hilbert_tour(inst, apex::hilbert_order(inst));
    apex::IlsConfig ic;  // no limit: runs until cancelled
    std::thread th([&] { apex::iterated_local_search(t, inst, g, ic); });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    c.pause();  // cancel also releases a paused solver
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const auto tc = Clock::now();
    c.cancel();
    th.join();
    const double ms = ms_since(tc);
    MESSAGE("cancel: solver returned after " << ms << " ms");
    CHECK(ms <= 200);
    CHECK(apex::tour_cost(inst, t) == t.cost);
    c.reset();
}

// Live control loop (G5.2) + FINISHED == --report bytes, against a running apex_tsp.
TEST_CASE("T_control_live_and_T_finished_matches_report_file") {
    const fs::path dir = fs::temp_directory_path() / "apex_tsp_ipc_live";
    fs::remove_all(dir);
    fs::create_directories(dir);
#ifdef _WIN32
    const std::string endpoint = "apex_tsp_ipc_live_test";
#else
    const std::string endpoint = (dir / "engine.sock").string();
#endif
    const fs::path rep = dir / "report.json", ck = dir / "ck", shared = dir / "tour.shm";
    const std::string cmd = std::string(APEX_BIN) + " --instance " + std::string(APEX_DATA_DIR) +
                            "/pr1002.tsp --mode exact --time-limit 120 --threads 1 --ipc " + endpoint +
                            " --shared-tour \"" + shared.string() + "\" --checkpoint \"" + ck.string() +
                            "\" --report \"" + rep.string() + "\" > \"" + (dir / "engine.log").string() + "\" 2>&1";
    int rc = -1;
    std::thread engine([&] { rc = std::system(cmd.c_str()); });

    auto t = connect(endpoint, 10000);
    REQUIRE(t);
    FrameDecoder d;
    auto hello = next_frame(*t, d, 10);
    REQUIRE(hello);
    REQUIRE(hello->type == MsgType::Hello);
    const auto eh = unpack_hello(hello->payload);
    REQUIRE(eh);
    CHECK(eh->mode == 0);
    CHECK(eh->threads == 1);
    Hello gui;
    gui.gui_capabilities = 1;
    const auto hw = encode(MsgType::Hello, pack(gui));
    REQUIRE(t->write_all(hw.data(), hw.size()));

    uint64_t telemetry = 0, tour_updates = 0, last_seq = 0, seq_regressions = 0;
    std::optional<Frame> finished;
    auto pump = [&](const auto& want, double timeout_s) -> std::optional<Frame> {
        const auto end = Clock::now() + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(timeout_s));
        while (Clock::now() < end) {
            auto f = next_frame(*t, d, 0.5);
            if (!f) continue;
            if (f->type == MsgType::Telemetry) {
                ++telemetry;
                const auto p = unpack_telemetry(f->payload);
                REQUIRE(p);
                const uint64_t s = p->seq;
                if (s <= last_seq) ++seq_regressions;
                last_seq = s;
            }
            if (f->type == MsgType::TourUpdate) ++tour_updates;
            if (f->type == MsgType::Finished) finished = f;
            if (want(*f)) return f;
        }
        return std::nullopt;
    };
    auto is_log = [](const char* needle) {
        return [needle](const Frame& f) { return f.type == MsgType::Log && as_text(f).find(needle) != std::string::npos; };
    };
    auto is_ack = [](uint64_t seq) {
        return [seq](const Frame& f) { return f.type == MsgType::Ack && unpack_u64(f.payload) == seq; };
    };

    REQUIRE(pump(is_log("bnc_start"), 120));  // solver inside branch-and-cut
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    send(*t, MsgType::Pause, 1);
    REQUIRE(pump(is_ack(1), 5));
    auto stalled = pump(is_log("paused stalled_threads="), 5);
    REQUIRE(stalled);
    const std::string s = as_text(*stalled);
    MESSAGE("engine: " << s);
    const auto th = std::stoul(s.substr(s.find('=') + 1));
    const double after_ms = std::stod(s.substr(s.find("after_ms=") + 9));
    CHECK(th >= 1);
    CHECK(after_ms <= 200);
    send(*t, MsgType::Pause, 2);  // idempotent
    REQUIRE(pump(is_ack(2), 5));
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    send(*t, MsgType::Resume, 3);
    REQUIRE(pump(is_ack(3), 5));
    send(*t, MsgType::Checkpoint, 4);
    REQUIRE(pump(is_ack(4), 5));
    REQUIRE(pump(is_log("checkpoint_written"), 10));
    CHECK(!fs::is_empty(ck));
    // Shared tour: a consistent permutation of pr1002
    {
        auto mf = MappedFile::open(shared.string());
        REQUIRE(mf);
        SharedTourView v(mf->data());
        std::vector<uint32_t> tour;
        REQUIRE(v.read(tour));
        CHECK(tour.size() == 1002);
        std::vector<uint8_t> seen(1002, 0);
        bool perm = tour.size() == 1002;
        for (uint32_t x : tour) perm = perm && x < 1002 && !seen[x]++;
        CHECK(perm);
    }
    send(*t, MsgType::Cancel, 5);
    REQUIRE(pump(is_ack(5), 5));
    const auto tc = Clock::now();
    if (!finished) pump([](const Frame& f) { return f.type == MsgType::Finished; }, 60);
    const double cancel_s = ms_since(tc) / 1000;
    REQUIRE(finished);
    engine.join();
    MESSAGE("live: telemetry=" << telemetry << " tour_updates=" << tour_updates << " cancel_to_finished_s=" << cancel_s
                               << " rc=" << rc);
    CHECK(telemetry >= 2);
    CHECK(seq_regressions == 0);
    CHECK(tour_updates >= 1);
    std::ifstream f(rep, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    const std::string fin = as_text(*finished);
    CHECK(fin == ss.str());  // byte-identical to --report
    CHECK(fin.find("\"status\":\"CANCELLED\"") != std::string::npos);
    CHECK(fin.find("\"hardware\":") != std::string::npos);
    CHECK(rc == 0);
    t.reset();
    fs::remove_all(dir);
}
