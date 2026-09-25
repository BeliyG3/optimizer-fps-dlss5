#pragma once
#include "core/context.h"

namespace ofps::core {
// C++ unwinding and SEH live in separate functions so both cleanup models remain valid.
template <class F> int CallbackCpp(const F &call, int failure) {
    try {
        return static_cast<int>(call());
    } catch (...) {
        return failure;
    }
}
template <class F> int GuardCallback(const F &call, int failure = OFPS_E_DEVICE) {
    __try {
        return CallbackCpp(call, failure);
    } __except (gpu::RecordCrash(GetExceptionInformation(), StageModel, Ctx().crash)) {
        return failure;
    }
}
} // namespace ofps::core
