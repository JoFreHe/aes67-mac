#include "../NetworkEngine/RTP/TxTimingDiagnostics.hpp"
#include <iostream>

int main() {
    AES67::TxTimingDiagnostics timing;
    constexpr uint64_t periodNs = 1000000;
    timing.recordDeadline(1000, 1000, periodNs); // on time
    timing.recordDeadline(2000, 2010, periodNs); // normal small wakeup delay
    timing.recordDeadline(3000, 2999, periodNs); // early
    if (timing.latePackets() != 0 || timing.maxLatenessNs() != 10) return 1;

    timing.recordDeadline(4000, 1004000, periodNs); // at least one full period late
    timing.recordDeadline(5000, 1004999, periodNs); // just below the threshold
    if (timing.latePackets() != 1 || timing.maxLatenessNs() != periodNs) return 2;
    timing.recordDeadline(6000, 1006001, periodNs); // just above the threshold
    if (timing.latePackets() != 2 || timing.maxLatenessNs() != periodNs + 1) return 5;

    timing.recordResync(24000000); // long scheduler stall, no packet counted
    if (timing.resyncs() != 1 || timing.latePackets() != 2 ||
        timing.maxLatenessNs() != 24000000) return 3;

    timing.reset();
    if (timing.latePackets() != 0 || timing.maxLatenessNs() != 0 || timing.resyncs() != 0) return 4;
    if (AES67::TxTimingDiagnostics::delta(3, 100) != 3 ||
        AES67::TxTimingDiagnostics::delta(103, 100) != 3) return 6;
    std::cout << "PASS: TX deadline threshold, maximum lateness and resync accounting\n";
}
