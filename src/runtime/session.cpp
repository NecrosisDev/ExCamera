#include "session.hpp"

#include <new>

namespace exanima {

Session::~Session() {
    (void)stop();
}

Result Session::start() {
    if (state_ != EXA_RUNTIME_UNLOADED && state_ != EXA_RUNTIME_STOPPED) {
        return EXA_RESULT_INVALID_STATE;
    }
    state_ = EXA_RUNTIME_ATTACHING;
    last_error_ = EXA_RESULT_OK;
    try {
        auto broker = std::make_unique<HookBroker>();
        const Result result = broker->initialize();
        if (result != EXA_RESULT_OK) {
            last_error_ = result;
            state_ = EXA_RUNTIME_REFUSED;
            return result;
        }
        broker_ = std::move(broker);
    } catch (...) {
        last_error_ = EXA_RESULT_BACKEND_FAILURE;
        state_ = EXA_RUNTIME_FAULTED;
        return last_error_;
    }
    ++generation_;
    state_ = EXA_RUNTIME_DORMANT;
    return EXA_RESULT_OK;
}

Result Session::stop() noexcept {
    if (state_ == EXA_RUNTIME_STOPPED) return EXA_RESULT_OK;
    state_ = EXA_RUNTIME_STOPPING;
    if (broker_) {
        const Result result = broker_->stop();
        if (result != EXA_RESULT_OK) {
            last_error_ = result;
            state_ = EXA_RUNTIME_FAULTED;
            return result;
        }
        broker_.reset();
    }
    last_error_ = EXA_RESULT_OK;
    state_ = EXA_RUNTIME_STOPPED;
    return EXA_RESULT_OK;
}

ExaRuntimeStatus Session::status() const noexcept {
    ExaRuntimeStatus result{};
    result.struct_size = sizeof(result);
    result.abi_version = EXA_RUNTIME_ABI_VERSION;
    result.state = state_;
    result.generation = generation_;
    result.installed_hooks = broker_ ? static_cast<uint32_t>(broker_->installed_count()) : 0;
    result.active_hooks = broker_ ? static_cast<uint32_t>(broker_->active_count()) : 0;
    result.last_error = last_error_;
    return result;
}

} // namespace exanima
