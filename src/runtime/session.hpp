#pragma once

#include <cstdint>
#include <memory>

#include <exanima/result.h>
#include <exanima/runtime.h>
#include "hook_broker.hpp"

namespace exanima {

class Session {
public:
    Session() = default;
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    Result start();
    Result stop() noexcept;
    ExaRuntimeStatus status() const noexcept;
    HookBroker* broker() noexcept { return broker_.get(); }
    void activate() noexcept { if (state_ == EXA_RUNTIME_DORMANT) state_ = EXA_RUNTIME_ACTIVE; }
    void fail(Result result) noexcept { last_error_ = result; state_ = EXA_RUNTIME_FAULTED; }

private:
    ExaRuntimeState state_ = EXA_RUNTIME_UNLOADED;
    uint32_t generation_ = 0;
    Result last_error_ = EXA_RESULT_OK;
    std::unique_ptr<HookBroker> broker_;
};

} // namespace exanima
