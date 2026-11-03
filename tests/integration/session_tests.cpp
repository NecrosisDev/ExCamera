#include "session.hpp"

#include "../test_support.hpp"

using exanima::Session;

bool starts_dormant_without_game_hooks() {
    Session session;
    CHECK(session.status().state == EXA_RUNTIME_UNLOADED);
    CHECK(session.start() == EXA_RESULT_OK);
    const ExaRuntimeStatus status = session.status();
    CHECK(status.state == EXA_RUNTIME_DORMANT);
    CHECK(status.generation == 1);
    CHECK(status.installed_hooks == 0);
    CHECK(status.active_hooks == 0);
    CHECK(session.start() == EXA_RESULT_INVALID_STATE);
    CHECK(session.stop() == EXA_RESULT_OK);
    CHECK(session.status().state == EXA_RUNTIME_STOPPED);
    CHECK(session.stop() == EXA_RESULT_OK);
    return true;
}

bool restart_generations_never_reuse_state() {
    Session session;
    for (uint32_t cycle = 1; cycle <= 32; ++cycle) {
        CHECK(session.start() == EXA_RESULT_OK);
        const ExaRuntimeStatus running = session.status();
        CHECK(running.generation == cycle);
        CHECK(running.state == EXA_RUNTIME_DORMANT);
        CHECK(running.installed_hooks == 0);
        CHECK(running.active_hooks == 0);
        CHECK(session.stop() == EXA_RESULT_OK);
        const ExaRuntimeStatus stopped = session.status();
        CHECK(stopped.state == EXA_RUNTIME_STOPPED);
        CHECK(stopped.generation == cycle);
        CHECK(stopped.installed_hooks == 0);
        CHECK(stopped.active_hooks == 0);
    }
    return true;
}

int main() {
    return run_tests({
        {"starts dormant without game hooks", starts_dormant_without_game_hooks},
        {"restart generations never reuse state", restart_generations_never_reuse_state},
    });
}
