// Receive-side sequence lock, resync and rate control (RxSequenceSync.hpp)
// against the real LockFreeCircularJitterBuffer. The simulation drives the
// same rxConsumeStep()/nextRateAdjustment() RTPReceiver::consumeLoop() uses,
// with a modelled sender, network and Core Audio reader in virtual time.
#include "../NetworkEngine/RTP/RxSequenceSync.hpp"
#include "../NetworkEngine/RTP/LockFreeCircularJitterBuffer.h"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <queue>
#include <random>
#include <tuple>
#include <vector>

using namespace AES67;

static void testDecisions() {
    assert(seqDistance(2, 65535) == 3 && seqDistance(65535, 2) == -3);
    assert(packetsAhead(100, 105) == 6 && packetsAhead(106, 105) == 0);
    assert(decideMissing(106, 105, 256) == RxMissAction::Wait);      // live edge
    assert(decideMissing(105, 105, 256) == RxMissAction::Wait);      // being written
    assert(decideMissing(100, 105, 256) == RxMissAction::SkipLost);  // newer data exists
    assert(decideMissing(65535, 1, 256) == RxMissAction::SkipLost);  // across wraparound
    assert(decideMissing(110, 105, 256) == RxMissAction::Resync);    // consumer ahead of sender
    assert(decideMissing(100, 356, 256) == RxMissAction::Resync);    // slots alias
    std::puts("decisions: wait at edge, skip only with newer data, resync otherwise");
}

static void testTracker() {
    RxSequenceTracker t(256);
    assert(t.generation() == 0);
    t.observe(65534, 7);
    assert(t.generation() == 1 && t.newest() == 65534);
    t.observe(65535, 7); t.observe(1, 7); t.observe(0, 7);   // wrap + reorder
    assert(t.newest() == 1);
    t.observe(1, 7); t.observe(60000, 7);                    // duplicate, stale packet
    assert(t.newest() == 1 && t.generation() == 1);
    t.observe(30000, 7);                                     // single far stray
    t.observe(2, 7);
    assert(t.newest() == 2 && t.generation() == 1);
    for (uint16_t s = 9000; s < 9003; ++s) t.observe(s, 7);
    assert(t.generation() == 1);                             // run not long enough
    t.observe(9003, 7);
    assert(t.generation() == 2 && t.newest() == 9003);       // same-SSRC restart
    t.observe(5, 8);
    assert(t.generation() == 3 && t.newest() == 5);          // new sender instance
    std::puts("tracker: late/duplicate/stray ignored, restarts detected");

    // SSRC lock: a second sender is ignored while the locked one keeps sending.
    RxSequenceTracker lock(256);
    const int64_t ms = 1000000;
    assert(lock.admits(0xA, 0)); lock.observe(10, 0xA);
    assert(!lock.admits(0xB, 1 * ms) && lock.admits(0xA, 2 * ms) && !lock.admits(0xB, 150 * ms));
    assert(lock.generation() == 1);
    assert(lock.admits(0xB, 2 * ms + 200 * ms)); lock.observe(500, 0xB);   // A silent 200 ms
    assert(lock.generation() == 2 && lock.newest() == 500);
    assert(!lock.admits(0xA, 210 * ms));                                // A is now the foreign one
    std::puts("tracker: second sender ignored, takes over only after 200 ms silence");
}

static void testRateAdjustment() {
    // Empty ring wants faster consumption, but not without a cushion.
    assert(nextRateAdjustment(-0.004, 0.0, 6, 6, 0.5, 0.0001, 0.005) == 0.0);
    assert(nextRateAdjustment(0.0, 0.0, 9, 6, 0.5, 0.0001, 0.005) < 0.0);
    assert(nextRateAdjustment(0.0, 1.0, 2, 6, 0.5, 0.0001, 0.005) > 0.0);  // may always slow
    assert(nextRateAdjustment(-0.005, 0.0, 30, 6, 0.5, 0.1, 0.005) == -0.005);
    std::puts("rate controller: speeds up only with cushion, clamped");
}

static void testJitterOccupancy() {
    LockFreeCircularJitterBuffer b(256);
    uint8_t p[4] = {}, out[8];
    size_t len; uint64_t pts;
    assert(b.addPacket(p, 4, 10, 0) && b.addPacket(p, 4, 266, 0));  // replaces unplayed 10
    assert(b.getBufferedPacketCount() == 1);
    assert(b.getNextPacket(out, 8, len, pts, 266) && b.getBufferedPacketCount() == 0);
    std::puts("jitter buffer: replacing a slot keeps the occupancy exact");
}

// The original stall: at the live edge a late packet the consumer had already
// skipped kept the jitter buffer non-empty, which was read as "newer data
// exists", so the consumer skipped ahead of the sender and never played again.
static void testEdgeWithLateStray() {
    LockFreeCircularJitterBuffer jitter(256);
    RxSequenceTracker tracker(jitter.getMaxBufferSize());
    RxConsumerState state;
    const RxSyncLimits limits{256, 6, 64};
    uint8_t p[4] = {}, out[8];
    size_t len; uint64_t pts;
    auto arrive = [&](uint16_t s) { jitter.addPacket(p, 4, s, 0, tracker.epoch()); tracker.observe(s, 1); };
    auto step = [&] { return rxConsumeStep(jitter, tracker, state, limits, out, sizeof out, len, pts); };

    arrive(100);
    assert(step() == RxStep::Idle && state.expected == 101 && state.prefilling);
    for (uint16_t s = 101; s <= 106; ++s) arrive(s);
    for (uint16_t s = 101; s <= 106; ++s) assert(step() == RxStep::Played);
    arrive(108);                                   // 107 delayed
    assert(step() == RxStep::Skipped && state.expected == 108);
    assert(step() == RxStep::Played && state.expected == 109);
    arrive(107);                                   // late straggler
    assert(jitter.getBufferedPacketCount() > 0);   // the old rule read this as "skip"
    assert(step() == RxStep::Waited && state.expected == 109);   // must not skip 109
    arrive(109);
    assert(step() == RxStep::Played);
    std::puts("live edge: a late straggler never makes the consumer overtake the sender");
}

struct Scenario {
    const char* name;
    double senderPpm = 0;         // + = sender clock faster than Core Audio
    double lateProb = 0;          // packet delayed by 2-6 ms
    double dupProb = 0;
    double seconds = 120;
    double restartAt = -1;        // sender restart: new sequence (and SSRC)
    bool restartNewSsrc = true;
    double stallAt = -1;          // network holds packets, then bursts
    double stallMs = 0;
    double hiccupProb = 0;        // consume thread wakes 2-8 ms late, then catches up
    double secondSenderAt = -1;   // another sender joins the same group (e.g. loopback)
};

struct Result {
    unsigned long sent = 0, played = 0, skipped = 0, trims = 0, resyncs = 0, foreign = 0;
    double maxGapMs = 0;          // longest stretch without playback after start
    int maxAheadAfterStall = 0;
};

static Result simulate(const Scenario& sc, unsigned seed) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> u(0, 1);
    LockFreeCircularJitterBuffer jitter(256);
    RxSequenceTracker tracker(jitter.getMaxBufferSize());
    RxConsumerState state;
    const RxSyncLimits limits{jitter.getMaxBufferSize(), 6, 64};   // as RTPReceiver
    const double interval = 1000.0, ringCap = 2048, caPeriod = 512.0 / 48000.0 * 1e6;
    uint8_t payload[288] = {}, out[1500];
    size_t len; uint64_t pts;

    using Ev = std::tuple<double, uint16_t, uint32_t>;
    std::priority_queue<Ev, std::vector<Ev>, std::greater<Ev>> net;
    const double sendPeriod = interval / (1.0 + sc.senderPpm * 1e-6);
    double nextSend = 0, nextCA = 5000, nextConsume = interval, rate = 0, ring = 0;
    double consumeWake = nextConsume;   // sleep_until wakes at or after the target
    auto wakeAfter = [&](double target, double now) {
        double late = u(rng) < sc.hiccupProb ? 2000 + 6000 * u(rng) : 30 * u(rng);
        return std::max(target, now) + late;
    };
    double lastPlay = -1, end = sc.seconds * 1e6;
    uint16_t seq = 1000, seq2 = 40000; uint32_t ssrc = 0x1234;
    bool restarted = false; int sinceCheck = 0;
    Result r;

    while (true) {
        double t = std::min({nextSend, nextCA, consumeWake,
                             net.empty() ? end : std::get<0>(net.top())});
        if (t >= end) break;
        if (t == nextSend) {
            if (!restarted && sc.restartAt >= 0 && t >= sc.restartAt * 1e6) {
                restarted = true;
                seq = static_cast<uint16_t>(seq + 20000);
                if (sc.restartNewSsrc) ssrc = 0x9999;
            }
            double d = 150 + 300 * u(rng);
            if (u(rng) < sc.lateProb) d += 2000 + 4000 * u(rng);
            double arrival = t + d;
            if (sc.stallAt >= 0 && t >= sc.stallAt * 1e6 && t < sc.stallAt * 1e6 + sc.stallMs * 1e3)
                arrival = sc.stallAt * 1e6 + sc.stallMs * 1e3 + d;
            net.push({arrival, seq, ssrc});
            if (u(rng) < sc.dupProb) net.push({arrival + 50, seq, ssrc});
            if (sc.secondSenderAt >= 0 && t >= sc.secondSenderAt * 1e6) net.push({arrival + 500, seq2++, 0x7777});
            ++seq; ++r.sent;
            nextSend += sendPeriod;
        } else if (!net.empty() && t == std::get<0>(net.top())) {   // receive thread
            auto [at, s, src] = net.top(); net.pop();
            if (!tracker.admits(src, int64_t(at * 1000))) { ++r.foreign; continue; }
            jitter.addPacket(payload, sizeof payload, s, 0, tracker.epoch());
            tracker.observe(s, src);
        } else if (t == nextCA) {                                      // Core Audio IO
            ring = std::max(0.0, ring - 512);
            nextCA += caPeriod;
        } else {                                                       // consume thread
            const bool wasPrefilling = state.prefilling;
            RxStep step = rxConsumeStep(jitter, tracker, state, limits, out, sizeof out, len, pts);
            if (step == RxStep::Played) {
                if (lastPlay >= 0) r.maxGapMs = std::max(r.maxGapMs, (t - lastPlay) / 1000.0);
                lastPlay = t; ++r.played;
                ring = std::min(ringCap, ring + 48);
            }
            r.skipped += step == RxStep::Skipped;
            r.trims += step == RxStep::Trimmed;
            r.resyncs += step == RxStep::Resynced;
            if (sc.stallAt >= 0 && t > (sc.stallAt + 0.5) * 1e6 && tracker.generation())
                r.maxAheadAfterStall = std::max(r.maxAheadAfterStall,
                                                packetsAhead(state.expected, tracker.newest()));
            if (state.prefilling) {
                rate = 0; sinceCheck = 0;
                nextConsume = t + interval;
                consumeWake = wakeAfter(nextConsume, t);
                continue;
            }
            if (wasPrefilling) nextConsume = t;
            if (++sinceCheck >= 48) {
                sinceCheck = 0;
                rate = nextRateAdjustment(rate, ring / ringCap,
                                          packetsAhead(state.expected, tracker.newest()),
                                          limits.prefill, 0.5, 0.0001, 0.005);
            }
            nextConsume += interval * (1.0 + rate);
            consumeWake = wakeAfter(nextConsume, t);
        }
    }
    if (lastPlay >= 0) r.maxGapMs = std::max(r.maxGapMs, (end - lastPlay) / 1000.0);
    return r;
}

static void runScenario(const Scenario& sc, double maxGapMs, double minPlayedRatio) {
    for (unsigned seed = 1; seed <= 3; ++seed) {
        Result r = simulate(sc, seed);
        const double ratio = double(r.played) / double(r.sent);
        std::printf("  %-34s seed %u: played %lu/%lu (%.2f%%) skipped %lu trims %lu resyncs %lu maxGap %.1f ms\n",
                    sc.name, seed, r.played, r.sent, ratio * 100, r.skipped, r.trims, r.resyncs, r.maxGapMs);
        assert(r.maxGapMs <= maxGapMs);
        assert(ratio >= minPlayedRatio);
        if (sc.stallAt >= 0) assert(r.maxAheadAfterStall <= 64);
    }
}

int main() {
    testDecisions();
    testTracker();
    testRateAdjustment();
    testJitterOccupancy();
    testEdgeWithLateStray();

    std::puts("simulated receive path (regression for silence after 15 s - 4 min):");
    Scenario late{"late packets 1/2000 + duplicates"};
    late.lateProb = 1.0 / 2000; late.dupProb = 1.0 / 1000;
    runScenario(late, 20, 0.999);

    Scenario heavy{"late packets 1/100 (busy WLAN)"};
    heavy.lateProb = 1.0 / 100;
    runScenario(heavy, 20, 0.995);

    Scenario fast{"sender +100 ppm, late 1/2000"};
    fast.senderPpm = 100; fast.lateProb = 1.0 / 2000;
    runScenario(fast, 20, 0.99);

    Scenario slow{"sender -100 ppm, late 1/2000"};
    slow.senderPpm = -100; slow.lateProb = 1.0 / 2000;
    runScenario(slow, 20, 0.99);

    Scenario hiccups{"consume thread hiccups 1/300, late 1/2000"};
    hiccups.hiccupProb = 1.0 / 300; hiccups.lateProb = 1.0 / 2000;
    runScenario(hiccups, 20, 0.999);

    // A new SSRC is admitted after 200 ms silence of the old sender.
    Scenario restart{"sender restart, new SSRC"};
    restart.restartAt = 30; restart.lateProb = 1.0 / 2000; restart.seconds = 60;
    runScenario(restart, 250, 0.99);

    Scenario second{"second sender on the same group"};
    second.secondSenderAt = 10; second.lateProb = 1.0 / 2000; second.seconds = 60;
    runScenario(second, 20, 0.999);

    Scenario sameSsrc{"sender restart, same SSRC"};
    sameSsrc.restartAt = 30; sameSsrc.restartNewSsrc = false; sameSsrc.seconds = 60;
    runScenario(sameSsrc, 30, 0.99);

    Scenario stall{"network stall 150 ms then burst"};
    stall.stallAt = 20; stall.stallMs = 150; stall.seconds = 40;
    runScenario(stall, 200, 0.98);

    std::puts("All RxSequenceSync tests passed");
    return 0;
}
