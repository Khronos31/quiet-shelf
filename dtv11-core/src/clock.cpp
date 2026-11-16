// SPDX-License-Identifier: GPL-2.0-or-later
#include <dtv/internal/clock.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>

#include <dtv/internal/result.hpp>

namespace dtv::internal {

namespace {

extern dtv_runtime_clock kSystemClock;

std::atomic<void (*)(void *)> system_clock_delay_hook_for_test{nullptr};
std::atomic<void *> system_clock_delay_hook_user_for_test{nullptr};

uint64_t monotonic_now_ns(void *)
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

dtv_internal_result is_cancelled(void *, const dtv_cancel *cancel, uint32_t *cancelled)
{
    if (cancelled == nullptr) {
        return dtv_internal_result_make(DTV_INTERNAL_RESULT_INVALID_ARGUMENT, 0);
    }
    if (cancel != nullptr) {
        return dtv_internal_result_make(DTV_INTERNAL_RESULT_UNSUPPORTED, 0);
    }
    *cancelled = 0;
    return dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
}

dtv_internal_result delay_until(void *user, uint64_t wake_at_ns,
                                uint64_t operation_deadline_ns,
                                const dtv_cancel *cancel, uint32_t *cancelled)
{
    if (cancelled == nullptr) {
        return dtv_internal_result_make(DTV_INTERNAL_RESULT_INVALID_ARGUMENT, 0);
    }
    *cancelled = 0;
    const auto hook = system_clock_delay_hook_for_test.load(std::memory_order_acquire);
    if (hook != nullptr)
        hook(system_clock_delay_hook_user_for_test.load(std::memory_order_acquire));
    const auto *clock = static_cast<const dtv_runtime_clock *>(user);
    if (clock == nullptr || clock->monotonic_now_ns == nullptr) {
        return dtv_internal_result_make(DTV_INTERNAL_RESULT_INVALID_ARGUMENT, 0);
    }
    while (true) {
        uint32_t is_cancelled = 0;
        if (clock->is_cancelled != nullptr) {
            const auto result = clock->is_cancelled(clock->user, cancel, &is_cancelled);
            if (result.code != DTV_INTERNAL_RESULT_OK) {
                return result;
            }
        }
        if (is_cancelled != 0) {
            *cancelled = 1;
            return dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
        }
        const auto now = clock->monotonic_now_ns(clock->user);
        if (now >= operation_deadline_ns) {
            return dtv_internal_result_make(DTV_INTERNAL_RESULT_TIMEOUT, 0);
        }
        if (now >= wake_at_ns) {
            return dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
        }
        const auto wake = std::min(wake_at_ns, operation_deadline_ns);
        const auto pause = std::min<uint64_t>(wake - now, 1000000);
        std::this_thread::sleep_for(std::chrono::nanoseconds(pause));
    }
}

dtv_runtime_clock kSystemClock{
    &kSystemClock, monotonic_now_ns, delay_until, is_cancelled
};

}

const dtv_runtime_clock *system_runtime_clock()
{
    return &kSystemClock;
}

void set_system_clock_delay_hook_for_test(void (*hook)(void *), void *user)
{
    system_clock_delay_hook_user_for_test.store(user, std::memory_order_release);
    system_clock_delay_hook_for_test.store(hook, std::memory_order_release);
}

}
