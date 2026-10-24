#include "AudioThreadPriority.h"
#include "DebugLog.h"
#include <mach/mach.h>
#include <mach/mach_error.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#include <pthread.h>
#include <unistd.h>
#include <errno.h>
#include <cstring>
#include <cstdio>

namespace AES67 {

bool AudioThreadPriority::configureForRealTime() {
    return configureThreadForRealTime(pthread_self());
}

bool AudioThreadPriority::configureThreadForRealTime(pthread_t thread) {
    // The network threads wake once per 1 ms packet and work briefly. The
    // time-constraint class is what CoreAudio uses for its own I/O threads and
    // needs no entitlement. (The former extended/precedence/affinity sequence
    // never made a thread real-time, and affinity is unsupported on Apple
    // Silicon, so every start logged a failure.)
    mach_timebase_info_data_t timebase;
    if (mach_timebase_info(&timebase) != KERN_SUCCESS || timebase.numer == 0) {
        return false;
    }
    auto ticks = [&](double microseconds) {
        return static_cast<uint32_t>(microseconds * 1000.0 * timebase.denom / timebase.numer);
    };
    thread_time_constraint_policy_data_t policy;
    policy.period = ticks(1000.0);
    policy.computation = ticks(300.0);
    policy.constraint = ticks(800.0);
    policy.preemptible = TRUE;

    const kern_return_t result = thread_policy_set(
        pthread_mach_thread_np(thread),
        THREAD_TIME_CONSTRAINT_POLICY,
        reinterpret_cast<thread_policy_t>(&policy),
        THREAD_TIME_CONSTRAINT_POLICY_COUNT
    );
    if (result != KERN_SUCCESS) {
        AES67_ERRF("AES67 AudioThreadPriority: THREAD_TIME_CONSTRAINT_POLICY failed (kern_return=%d: %s)",
                result, mach_error_string(result));
        return false;
    }
    return true;
}

void AudioThreadPriority::restoreNormalPriority() {
    // Back to normal timesharing for this thread only; never renice the
    // process, which in the driver is the shared CoreAudio plug-in host.
    thread_extended_policy_data_t extendedPolicy;
    extendedPolicy.timeshare = TRUE;
    thread_policy_set(
        mach_thread_self(),
        THREAD_EXTENDED_POLICY,
        reinterpret_cast<thread_policy_t>(&extendedPolicy),
        THREAD_EXTENDED_POLICY_COUNT
    );
}

int AudioThreadPriority::getRecommendedSchedulingPolicy() {
    // On macOS, we use Mach thread policies instead of POSIX scheduling
    return SCHED_FIFO;  // This is for reference; actual implementation uses Mach
}

int AudioThreadPriority::getRecommendedPriority() {
    // High priority for audio processing
    return 63;  // Max priority for real-time audio on macOS
}

} // namespace AES67