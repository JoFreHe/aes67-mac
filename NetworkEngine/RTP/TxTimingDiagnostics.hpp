#pragma once

#include <atomic>
#include <cstdint>

namespace AES67 {

// Lock-free counters for packet-start deadline diagnostics. Recording does no
// logging or allocation, so it is safe to call from the transmit thread.
class TxTimingDiagnostics {
public:
    static_assert(std::atomic<uint64_t>::is_always_lock_free,
                  "TX diagnostics require lock-free counters");

    // Counters may be reset between periodic reports; never wrap the delta.
    static uint64_t delta(uint64_t current, uint64_t previous) noexcept {
        return current >= previous ? current - previous : current;
    }

    void recordDeadline(uint64_t scheduledNs, uint64_t actualNs, uint64_t periodNs) noexcept {
        if (actualNs <= scheduledNs) return;
        const uint64_t lateBy = actualNs - scheduledNs;
        if (periodNs != 0 && lateBy >= periodNs) {
            latePackets_.fetch_add(1, std::memory_order_relaxed);
        }
        recordMaximum(lateBy);
    }

    // Resync events have no packet send associated with them. Keep them out of
    // latePackets while preserving their delay in the maximum diagnostic.
    void recordResync(uint64_t latenessNs) noexcept {
        resyncs_.fetch_add(1, std::memory_order_relaxed);
        recordMaximum(latenessNs);
    }

    uint64_t latePackets() const noexcept { return latePackets_.load(std::memory_order_relaxed); }
    uint64_t maxLatenessNs() const noexcept { return maxLatenessNs_.load(std::memory_order_relaxed); }
    uint64_t resyncs() const noexcept { return resyncs_.load(std::memory_order_relaxed); }

    void reset() noexcept {
        latePackets_.store(0, std::memory_order_relaxed);
        maxLatenessNs_.store(0, std::memory_order_relaxed);
        resyncs_.store(0, std::memory_order_relaxed);
    }

private:
    void recordMaximum(uint64_t lateBy) noexcept {
        uint64_t previous = maxLatenessNs_.load(std::memory_order_relaxed);
        while (previous < lateBy &&
               !maxLatenessNs_.compare_exchange_weak(previous, lateBy,
                                                     std::memory_order_relaxed)) {}
    }
    std::atomic<uint64_t> latePackets_{0};
    std::atomic<uint64_t> maxLatenessNs_{0};
    std::atomic<uint64_t> resyncs_{0};
};

} // namespace AES67
