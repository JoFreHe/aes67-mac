/// @file RxSequenceSync.hpp
/// @brief Sequence lock, resynchronisation and rate control for RTPReceiver.
///
/// The receive thread records the newest RTP sequence number; the consume
/// thread decides once per packet interval whether the expected packet plays,
/// is lost (newer data exists), is not due yet (wait) or whether it has to
/// resynchronise. These decisions never use the jitter buffer occupancy: that
/// count includes late packets the consumer already skipped, and treating it
/// as "newer data exists" let the consumer overtake the sender and stay
/// silent for good (two-Mac test, 21.10.2026).

#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace AES67 {

/// Signed wraparound-safe distance a - b of two 16-bit RTP sequence numbers.
inline int seqDistance(uint16_t a, uint16_t b) {
    return static_cast<int16_t>(static_cast<uint16_t>(a - b));
}

/// Packets from expected up to and including newest (0 = consumer is at the
/// live edge, negative = consumer is ahead of the sender).
inline int packetsAhead(uint16_t expected, uint16_t newest) {
    return seqDistance(newest, expected) + 1;
}

enum class RxMissAction { Wait, SkipLost, Resync };

/// What to do when the expected packet is not in the jitter buffer.
/// @param window Jitter buffer slot count; larger distances alias slots.
inline RxMissAction decideMissing(uint16_t expected, uint16_t newest, size_t window) {
    const int newer = seqDistance(newest, expected);
    if (newer < -1 || newer >= static_cast<int>(window)) return RxMissAction::Resync;
    return newer > 0 ? RxMissAction::SkipLost : RxMissAction::Wait;
}

/// One step of the consume-rate integrator. Negative values shorten the
/// consume interval. Speeding up needs more than `cushion` packets queued;
/// without that cushion the consumer only runs into the sender's live edge.
inline double nextRateAdjustment(double current, double fillRatio, int ahead, int cushion,
                                 double target, double gain, double limit) {
    double next = current + (fillRatio - target) * gain;
    if (ahead <= cushion) next = std::max(next, 0.0);
    return std::clamp(next, -limit, limit);
}

/// Shared between the receive thread (observe, epoch) and the consume thread
/// (generation, newest, advanceEpoch). reset() only while both are stopped.
class RxSequenceTracker {
public:
    /// @param window Jitter buffer slot count.
    /// @param restartRun Consecutive far-off packets accepted as a new stream.
    /// @param takeoverNs Silence of the locked sender before another may take over.
    explicit RxSequenceTracker(size_t window, unsigned restartRun = 4, int64_t takeoverNs = 200000000)
        : window_(static_cast<int>(window)), restartRun_(restartRun), takeoverNs_(takeoverNs) {}

    /// Receive thread, before the packet enters the jitter buffer: whether it
    /// comes from the locked sender. Two senders on one group (e.g. a Mac
    /// hearing its own test sender via multicast loopback) otherwise alternate
    /// SSRCs every packet and force a resync each time. Another sender takes
    /// over only after the locked one stayed silent for takeoverNs.
    bool admits(uint32_t ssrc, int64_t nowNs) {
        if (generation_.load(std::memory_order_relaxed) == 0 || ssrc == ssrc_ ||
            nowNs - lastLockedNs_ >= takeoverNs_) {
            lastLockedNs_ = nowNs;
            return true;
        }
        return false;
    }

    /// Receive thread: record a valid packet.
    void observe(uint16_t seq, uint32_t ssrc) {
        if (generation_.load(std::memory_order_relaxed) == 0 || ssrc != ssrc_) {
            accept(seq, ssrc);
            return;
        }
        const int d = seqDistance(seq, newest_.load(std::memory_order_relaxed));
        if (d > -window_ && d < window_) {
            if (d > 0) newest_.store(seq, std::memory_order_release);
            run_ = 0; // late or duplicate packets never move newest backwards
            return;
        }
        // A far jump from the same SSRC is a sender restart only when it
        // continues; a single stray packet must not trigger a resync.
        run_ = (run_ > 0 && seq == static_cast<uint16_t>(runLast_ + 1)) ? run_ + 1 : 1;
        runLast_ = seq;
        if (run_ >= restartRun_) accept(seq, ssrc);
    }

    /// Number of stream (re)starts seen; 0 until the first packet.
    uint32_t generation() const { return generation_.load(std::memory_order_acquire); }
    uint16_t newest() const { return newest_.load(std::memory_order_acquire); }

    /// Jitter buffer epoch: packets tagged with an older epoch are discarded.
    uint32_t epoch() const { return epoch_.load(std::memory_order_acquire); }
    void advanceEpoch() { epoch_.fetch_add(1, std::memory_order_acq_rel); }

    void reset() {
        generation_.store(0, std::memory_order_relaxed);
        newest_.store(0, std::memory_order_relaxed);
        epoch_.store(0, std::memory_order_relaxed);
        ssrc_ = 0;
        run_ = 0;
        lastLockedNs_ = 0;
    }

private:
    void accept(uint16_t seq, uint32_t ssrc) {
        ssrc_ = ssrc;
        run_ = 0;
        newest_.store(seq, std::memory_order_relaxed);
        generation_.fetch_add(1, std::memory_order_release);
    }

    const int window_;
    const unsigned restartRun_;
    const int64_t takeoverNs_;
    std::atomic<uint32_t> generation_{0};
    std::atomic<uint16_t> newest_{0};
    std::atomic<uint32_t> epoch_{0};
    // Receive thread only
    uint32_t ssrc_ = 0;
    unsigned run_ = 0;
    uint16_t runLast_ = 0;
    int64_t lastLockedNs_ = 0;
};

/// Consume thread state.
struct RxConsumerState {
    uint16_t expected = 0;
    uint32_t generation = 0;
    bool prefilling = true;
};

struct RxSyncLimits {
    size_t window;   ///< jitter buffer slots
    int prefill;     ///< packets queued before playback (re)starts
    int maxLead;     ///< latency bound in packets; beyond it the backlog is dropped
};

enum class RxStep { Idle, Played, Skipped, Waited, Trimmed, Resynced };

/// One consume interval. On Played the packet payload is in `out`.
template<class JitterBuffer>
RxStep rxConsumeStep(JitterBuffer& jitter, RxSequenceTracker& tracker, RxConsumerState& state,
                     const RxSyncLimits& limits, uint8_t* out, size_t outSize,
                     size_t& outLength, uint64_t& presentationTime) {
    const uint32_t generation = tracker.generation();
    if (generation == 0) return RxStep::Idle;
    const uint16_t newest = tracker.newest();
    auto restart = [&] {
        tracker.advanceEpoch();
        state.expected = static_cast<uint16_t>(newest + 1);
        state.prefilling = true;
    };
    if (generation != state.generation) {
        const bool first = state.generation == 0;
        state.generation = generation;
        restart();
        return first ? RxStep::Idle : RxStep::Resynced;
    }

    const int ahead = packetsAhead(state.expected, newest);
    if (state.prefilling) {
        if (ahead < limits.prefill) return RxStep::Idle;
        state.prefilling = false;
    }
    if (ahead > limits.maxLead) {
        // Keep the newest `prefill` packets; the skipped ones expire in place.
        state.expected = static_cast<uint16_t>(newest - (limits.prefill - 1));
        return RxStep::Trimmed;
    }

    if (jitter.getNextPacket(out, outSize, outLength, presentationTime,
                             state.expected, tracker.epoch())) {
        ++state.expected;
        return RxStep::Played;
    }
    switch (decideMissing(state.expected, newest, limits.window)) {
    case RxMissAction::SkipLost:
        ++state.expected;
        return RxStep::Skipped;
    case RxMissAction::Wait:
        return RxStep::Waited;
    case RxMissAction::Resync:
        break;
    }
    restart();
    return RxStep::Resynced;
}

} // namespace AES67
