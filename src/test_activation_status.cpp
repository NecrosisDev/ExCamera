#ifdef NDEBUG
#error "The tests require assert()."
#endif
#include "activation_status.h"
#include <cassert>
#include <cstdio>
#include <string>

static const char* fields =
    "\"pid\":42,\"revision\":11,\"build\":\"reviewed\",\"process_started\":123456,"
    "\"activation_request\":18446744073709551615,\"activation_state\":\"running\"";
static fp::ActivationStatus parse(const std::string& json) {
    fp::ActivationStatus status;
    assert(fp::readActivationStatus(json.data(), json.size(), status));
    return status;
}
static void rejected(const std::string& json) {
    fp::ActivationStatus status;
    status.pid = 91;
    assert(!fp::readActivationStatus(json.data(), json.size(), status));
    assert(status.pid == 91); // A failed parse cannot publish a partial acknowledgement.
}
int main() {
    using Phase = fp::ActivationPhase;
    using Result = fp::ActivationResult;
    fp::ActivationStatus status = parse(std::string("{ ") + fields + " }\n");
    auto verdict = [&](uint64_t request = UINT64_MAX) {
        return fp::activationResult(status, 42, 123456, request, 11, "reviewed");
    };
    assert(verdict() == Result::ready);
    assert(verdict(1) == Result::waiting && verdict(0) == Result::waiting);
    status.pid = 43;
    assert(verdict() == Result::waiting);
    status.pid = 42;
    ++status.processStarted;
    assert(verdict() == Result::waiting); // A reused PID cannot resurrect a previous process's result.
    --status.processStarted;
    ++status.revision;
    assert(verdict() == Result::versionMismatch);
    --status.revision;
    status.build[0] = 'x';
    assert(verdict() == Result::versionMismatch);
    status.build[0] = 'r';
    for (Phase phase : {Phase::idle, Phase::starting, Phase::stopping, Phase::stopped}) {
        status.phase = phase;
        assert(verdict() == Result::waiting);
    }
    status.phase = Phase::failed;
    assert(verdict() == Result::failed);
    assert(fp::effectiveActivationPhase(Phase::running, true) == Phase::stopping);
    assert(fp::effectiveActivationPhase(Phase::failed, true) == Phase::failed);
    assert(fp::activationNeedsRearm(Phase::stopping) && fp::activationNeedsRearm(Phase::stopped));
    assert(!fp::activationNeedsRearm(Phase::running) && !fp::activationNeedsRearm(Phase::failed));

    const std::string large = "{\"diagnostic\":\"" + std::string(6000, 'x') + "\",\"nested\":{\"pid\":9,"
                              "\"values\":[null, true, false, -1.5e+3, \"\\u1234\\n\\\"\"]}," + fields + "}";
    assert(parse(large).pid == 42);
    assert(parse("{\"activation_state\":\"running\",\"activation_request\":18446744073709551615,"
                 "\"process_started\":123456,\"build\":\"reviewed\",\"revision\":11,\"p\\u0069d\":42}")
               .pid == 42);
    rejected(std::string("{") + fields + ",\"pid\":42}");
    rejected(std::string("{\"nested\":{") + fields + "}}");
    rejected(std::string("{\"text\":\"") + fields + "\"}");
    rejected(std::string("{") + fields + "}garbage");
    rejected(std::string("{") + fields + ",}");
    rejected(std::string("{") + fields + ",\"invalid\":01}");
    rejected(std::string("{") + fields + ",\"invalid\":1e}");
    rejected(std::string("{") + fields + ",\"invalid\":\"\\q\"}");
    rejected(std::string("{") + fields + ",\"invalid\":[true,]}");
    for (const char* bad : {"18446744073709551616", "-1", "1.0", "\"1\"", "null", "01"}) {
        std::string json = std::string("{") + fields + "}";
        const size_t at = json.find("18446744073709551615");
        json.replace(at, 20, bad);
        rejected(json);
    }
    for (size_t size = 0; size < large.size(); ++size) {
        fp::ActivationStatus partial;
        assert(!fp::readActivationStatus(large.data(), size, partial));
    }
    rejected(std::string(65537, ' '));
    puts("ACTIVATION STATUS: fresh identity, process reuse, version/failure phases, nested diagnostics, "
         "large files and malformed/truncated acknowledgements passed.");
}
