// Sprint 0 entry point. Full CLI (MASTER §3) arrives in M0.8; for now: load + summarize.
#include <cstdio>
#include <exception>

#include "core/instance.hpp"

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: apex_tsp <instance.tsp>\n");
        return 2;
    }
    try {
        const auto inst = apex::read_tsplib(argv[1]);
        std::printf("event=instance_loaded name=%s n=%u metric=%s\n", inst.name.c_str(), inst.n,
                    apex::metric_name(inst.metric));
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "event=error msg=\"%s\"\n", e.what());
        return 1;
    }
}
