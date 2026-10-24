//
// RTPReceiver.cpp
// AES67 macOS Driver - Build #9
// RTP packet receiver with L16/L24 decoding and channel mapping integration
//

#include "RTPReceiver.h"
#include "SimpleRTP.h"
#include "../../Driver/DebugLog.h"
#include <cstring>
#include <stdexcept>
#include <chrono>
#include <sys/select.h>
#include <arpa/inet.h>

namespace AES67 {

RTPReceiver::RTPReceiver(
    const SDPSession& sdp,
    const ChannelMapping& mapping,
    DeviceChannelBuffers& deviceChannels,
    size_t jitterBufferDepth,
    const std::string& networkInterface
)
    : sdp_(sdp)
    , mapping_(mapping)
    , deviceChannels_(deviceChannels)
    , jitterBuffer_(jitterBufferDepth > 0 ? jitterBufferDepth
                                          : LockFreeCircularJitterBuffer::DEFAULT_BUFFER_SIZE)
    , sequenceTracker_(jitterBuffer_.getMaxBufferSize())
    , networkInterface_(networkInterface)
{
    // Pre-allocate audio buffer to avoid allocations in receiveLoop()
    // Max 512 frames × stream channels (e.g., 512 × 8 = 4096 floats)
    const size_t maxFrames = 512;
    const size_t maxSamples = maxFrames * sdp_.numChannels;
    audioBuffer_.resize(maxSamples);

    // Calculate packet interval from SDP (mirrors RTPTransmitter constructor)
    // Priority: ptime field > framecount/sampleRate > 1ms fallback
    if (sdp_.ptime > 0) {
        packetInterval_ = std::chrono::microseconds(sdp_.ptime * 1000);
    } else if (sdp_.framecount > 0 && sdp_.sampleRate > 0) {
        uint64_t intervalUs = (static_cast<uint64_t>(sdp_.framecount) * 1000000ULL) / sdp_.sampleRate;
        packetInterval_ = std::chrono::microseconds(intervalUs);
    } else {
        packetInterval_ = std::chrono::microseconds(1000); // 1ms default for AES67
    }

    // Resolve network interface name to IP address
    if (!networkInterface_.empty()) {
        bool looksLikeIP = true;
        for (char c : networkInterface_) {
            if (c != '.' && !isdigit(c)) {
                looksLikeIP = false;
                break;
            }
        }

        if (looksLikeIP) {
            resolvedInterfaceIP_ = networkInterface_;
            AES67_LOGF("RTPReceiver: using interface IP %s directly (stream=%s)",
                       resolvedInterfaceIP_.c_str(), sdp_.sessionName.c_str());
        } else {
            resolvedInterfaceIP_ = NetworkInterfaceDetection::getInterfaceIPAddress(networkInterface_);
            if (resolvedInterfaceIP_.empty()) {
                AES67_LOGF("RTPReceiver: failed to resolve interface '%s' to IP (stream=%s)",
                           networkInterface_.c_str(), sdp_.sessionName.c_str());
            } else {
                AES67_LOGF("RTPReceiver: resolved interface '%s' to IP %s (stream=%s)",
                           networkInterface_.c_str(), resolvedInterfaceIP_.c_str(),
                           sdp_.sessionName.c_str());
            }
        }
    }
}

RTPReceiver::~RTPReceiver() {
    stop();
}

bool RTPReceiver::start() {
    if (running_ || rtpSocket_.isOpen()) {
        AES67_LOGF("RTPReceiver::start: already running or socket open (stream=%s)",
                   sdp_.sessionName.c_str());
        return false; // Already running
    }

    // Validate SDP configuration
    if (sdp_.connectionAddress.empty() || sdp_.port == 0) {
        AES67_LOGF("RTPReceiver::start: invalid SDP - address='%s' port=%u (stream=%s)",
                   sdp_.connectionAddress.c_str(), sdp_.port, sdp_.sessionName.c_str());
        return false;
    }

    if (sdp_.numChannels == 0 || sdp_.numChannels > 128) {
        AES67_LOGF("RTPReceiver::start: invalid channel count %u (stream=%s)",
                   sdp_.numChannels, sdp_.sessionName.c_str());
        return false;
    }
    if (!mapping_.isValid() || mapping_.streamChannelCount != sdp_.numChannels) {
        AES67_LOGF("RTPReceiver::start: invalid channel mapping (%u stream channels, SDP has %u) (stream=%s)",
                   mapping_.streamChannelCount, sdp_.numChannels, sdp_.sessionName.c_str());
        return false;
    }

    // An explicitly selected interface must never silently fall back to
    // another NIC; that can mix Dante control and audio networks.
    if (!networkInterface_.empty() && resolvedInterfaceIP_.empty()) {
        AES67_LOGF("RTPReceiver::start: interface '%s' has no IPv4 address; not receiving (stream=%s)",
                   networkInterface_.c_str(), sdp_.sessionName.c_str());
        return false;
    }

    // Open RTP receiver socket, optionally bound to a specific interface
    const char* ifaceIP = resolvedInterfaceIP_.empty() ? nullptr : resolvedInterfaceIP_.c_str();
    if (!rtpSocket_.openReceiver(sdp_.connectionAddress.c_str(), sdp_.port, ifaceIP)) {
        AES67_LOGF("RTPReceiver::start: socket open failed for %s:%u iface=%s (stream=%s)",
                   sdp_.connectionAddress.c_str(), sdp_.port,
                   resolvedInterfaceIP_.empty() ? "ANY" : resolvedInterfaceIP_.c_str(),
                   sdp_.sessionName.c_str());
        return false;
    }

    AES67_LOGF("RTPReceiver::start: opened socket for %s:%u on interface %s (stream=%s)",
               sdp_.connectionAddress.c_str(), sdp_.port,
               resolvedInterfaceIP_.empty() ? "INADDR_ANY" : resolvedInterfaceIP_.c_str(),
               sdp_.sessionName.c_str());

    // Reset jitter buffer, sequence lock and prefill gate
    jitterBuffer_.reset();
    sequenceTracker_.reset();
    expectedSequenceNumber_.store(0, std::memory_order_relaxed);
    prefillComplete_.store(false, std::memory_order_relaxed);
    connected_.store(false, std::memory_order_relaxed);
    loggedGeneration_ = 0;
    firstTimestampSet_.store(false, std::memory_order_relaxed);
    lastPacketTimeNs_.store(0, std::memory_order_relaxed);
    rateAdjustment_.store(0.0, std::memory_order_relaxed);

    // Start receive thread (producer - adds packets to jitter buffer)
    running_ = true;
    receiveThread_ = std::thread([this]() {
        if (!AudioThreadPriority::configureForRealTime()) {
            AES67_LOGF("RTPReceiver: failed to set RT priority on receive thread (stream=%s)",
                       sdp_.sessionName.c_str());
        }
        receiveLoop();
    });

    // Start consume thread (consumer - reads from jitter buffer and writes to ring buffers)
    consumeThread_ = std::thread([this]() {
        if (!AudioThreadPriority::configureForRealTime()) {
            AES67_LOGF("RTPReceiver: failed to set RT priority on consume thread (stream=%s)",
                       sdp_.sessionName.c_str());
        }
        consumeLoop();
    });

    // Diagnostics thread (normal priority, file I/O stays off the audio path)
    statsThread_ = std::thread([this]() { statsLoop(); });

    return true;
}

void RTPReceiver::stop() {
    if (!running_) {
        return;
    }

    running_ = false;

    if (receiveThread_.joinable()) {
        receiveThread_.join();
    }

    if (consumeThread_.joinable()) {
        consumeThread_.join();
    }

    if (statsThread_.joinable()) {
        statsThread_.join();
    }

    rtpSocket_.close();
    connected_ = false;

    const Diagnostics d = getDiagnostics();
    AES67_LOGF("RTPReceiver stopped (stream=%s): received %llu, played %llu, skipped %llu, waits %llu, "
               "trims %llu, resyncs %llu, foreign %llu, malformed %llu",
               sdp_.sessionName.c_str(), (unsigned long long)d.received, (unsigned long long)d.played,
               (unsigned long long)d.skipped, (unsigned long long)d.waits, (unsigned long long)d.trims,
               (unsigned long long)d.resyncs, (unsigned long long)d.foreign, (unsigned long long)d.malformed);
}

StatisticsSnapshot RTPReceiver::getStatistics() const {
    // Return a consistent snapshot of atomic statistics
    return stats_.snapshot();
}

void RTPReceiver::resetStatistics() {
    // Reset all atomic counters
    stats_.packetsReceived.store(0, std::memory_order_relaxed);
    stats_.packetsLost.store(0, std::memory_order_relaxed);
    stats_.malformedPackets.store(0, std::memory_order_relaxed);
    stats_.outOfOrderPackets.store(0, std::memory_order_relaxed);
    stats_.underruns.store(0, std::memory_order_relaxed);
    stats_.overruns.store(0, std::memory_order_relaxed);
    stats_.jitterNs.store(0, std::memory_order_relaxed);
    stats_.latencyNs.store(0, std::memory_order_relaxed);
    stats_.bytesReceived.store(0, std::memory_order_relaxed);
    stats_.bytesSent.store(0, std::memory_order_relaxed);
    lastSequenceNumber_.store(0, std::memory_order_relaxed);
    lastTimestamp_.store(0, std::memory_order_relaxed);
    expectedSequenceNumber_.store(0, std::memory_order_relaxed);
    firstTimestampSet_.store(false, std::memory_order_relaxed);
    firstTimestamp_.store(0, std::memory_order_relaxed);

    // Reset jitter buffer
    jitterBuffer_.reset();
}

bool RTPReceiver::isConnected() const {
    if (!connected_) {
        return false;
    }

    // Consider disconnected if no packet in last 1 second
    int64_t lastNs = lastPacketTimeNs_.load(std::memory_order_acquire);
    if (lastNs == 0) return false;
    auto now = std::chrono::steady_clock::now();
    int64_t nowNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
        now.time_since_epoch()).count();

    return (nowNs - lastNs) < 1000000000LL; // 1 second in ns
}

int64_t RTPReceiver::getTimeSinceLastPacket() const {
    if (!connected_) {
        return -1;
    }

    int64_t lastNs = lastPacketTimeNs_.load(std::memory_order_acquire);
    if (lastNs == 0) return -1;
    auto now = std::chrono::steady_clock::now();
    int64_t nowNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
        now.time_since_epoch()).count();

    return (nowNs - lastNs) / 1000000; // ns to ms
}

bool RTPReceiver::updateMapping(const ChannelMapping& newMapping) {
    // Validate mapping
    if (!newMapping.isValid() || newMapping.streamChannelCount != sdp_.numChannels) {
        return false;
    }

    // Stop, update, restart
    const bool wasRunning = running_;
    if (wasRunning) {
        stop();
    }

    mapping_ = newMapping;

    if (wasRunning) {
        return start();
    }

    return true;
}

void RTPReceiver::receiveLoop() {
    fd_set readfds;
    struct timeval tv;

    RTP::RTPPacket packet;

    while (running_) {
        // Set up select with 1ms timeout (responsive but not spinning)
        FD_ZERO(&readfds);
        int sockfd = rtpSocket_.getFd();
        if (sockfd < 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        FD_SET(sockfd, &readfds);

        tv.tv_sec = 0;
        tv.tv_usec = 1000;  // 1ms timeout

        int ret = select(sockfd + 1, &readfds, nullptr, nullptr, &tv);

        if (ret < 0 && errno != EINTR) {
            noteReceiveError("select");
        } else if (ret > 0 && FD_ISSET(sockfd, &readfds)) {
            ssize_t bytesReceived = rtpSocket_.receive(packet, receiveBuffer_, sizeof(receiveBuffer_));
            if (bytesReceived > 0) {
                processPacket(packet);
            } else if (bytesReceived < 0) {
                noteReceiveError("receive");
            }
        }
    }
}

void RTPReceiver::noteReceiveError(const char* where) {
    // First occurrence and every 1000th: a dead socket would otherwise spin silently.
    const uint64_t count = receiveErrors_.fetch_add(1, std::memory_order_relaxed) + 1;
    if (count == 1 || count % 1000 == 0) {
        AES67_LOGF("RTPReceiver: %s error #%llu (errno=%d: %s; also counts runt packets) (stream=%s)",
                   where, (unsigned long long)count, errno, strerror(errno), sdp_.sessionName.c_str());
    }
}

void RTPReceiver::consumeLoop() {
    // This thread pulls packets from the jitter buffer in sequence order
    // and writes decoded audio to the device ring buffers.
    //
    // Two phases:
    //   1. Pre-fill: after start and after every resync, wait until enough
    //      new packets are queued so the consumer has a jitter cushion.
    //   2. Paced consumption: consume one packet per packetInterval_ using
    //      sleep_until (mirrors RTPTransmitter::transmitLoop pattern).
    //
    // rxConsumeStep() makes the sequence decisions: it waits at the live
    // edge, skips a packet only when newer data exists and resynchronises
    // after a sender restart, so the consumer can never overtake the sender.

    const size_t window = jitterBuffer_.getMaxBufferSize();
    const RxSyncLimits limits{
        window,
        static_cast<int>(kPrefillPacketCount),
        std::min(kMaxLeadPackets, static_cast<int>(window / 2))
    };
    RxConsumerState state;
    size_t outputLength = 0;
    uint64_t presentationTime = 0;

    // Target absolute wall-clock times so we don't accumulate drift from
    // variable decode/write latency.
    auto nextConsumeTime = std::chrono::steady_clock::now();

    // Adaptive rate matching state
    double rateAdjustment = 0.0;        // current fractional adjustment
    size_t packetsSinceRateCheck = 0;
    auto nextResyncLog = std::chrono::steady_clock::time_point{};

    while (running_) {
        // While (pre)filling, Core Audio reads empty ring buffers and fills
        // silence (existing AES67IOHandler behavior).
        if (state.prefilling) {
            std::this_thread::sleep_for(packetInterval_);
        } else {
            std::this_thread::sleep_until(nextConsumeTime);

            // Advance target time (with rate adjustment applied)
            nextConsumeTime += std::chrono::microseconds(
                static_cast<int64_t>(packetInterval_.count() * (1.0 + rateAdjustment)));
        }

        const bool wasPrefilling = state.prefilling;
        const RxStep step = rxConsumeStep(jitterBuffer_, sequenceTracker_, state, limits,
                                          jitterReadBuffer_, sizeof(jitterReadBuffer_),
                                          outputLength, presentationTime);
        expectedSequenceNumber_.store(state.expected, std::memory_order_relaxed);

        switch (step) {
        case RxStep::Played:
            playedPackets_.fetch_add(1, std::memory_order_relaxed);
            if (sdp_.encoding == "L16") {
                decodeL16(jitterReadBuffer_, outputLength);
            } else if (sdp_.encoding == "L24") {
                decodeL24(jitterReadBuffer_, outputLength);
            }
            break;
        case RxStep::Skipped:
            // Newer packets exist, so the expected one is lost or late.
            skippedPackets_.fetch_add(1, std::memory_order_relaxed);
            stats_.packetsLost.fetch_add(1, std::memory_order_relaxed);
            break;
        case RxStep::Waited:
            // At the sender's live edge: the packet may still arrive.
            edgeWaits_.fetch_add(1, std::memory_order_relaxed);
            stats_.underruns.fetch_add(1, std::memory_order_relaxed);
            break;
        case RxStep::Trimmed:
            latencyTrims_.fetch_add(1, std::memory_order_relaxed);
            break;
        case RxStep::Resynced:
            resyncs_.fetch_add(1, std::memory_order_relaxed);
            // File I/O on the paced thread: at most one line per second, the
            // stats line carries the count.
            if (std::chrono::steady_clock::now() >= nextResyncLog) {
                nextResyncLog = std::chrono::steady_clock::now() + std::chrono::seconds(1);
                AES67_LOGF("RTPReceiver: resync to seq %u (stream=%s)",
                           state.expected, sdp_.sessionName.c_str());
            }
            break;
        case RxStep::Idle:
            break;
        }

        if (state.prefilling) {
            prefillComplete_.store(false, std::memory_order_relaxed);
            rateAdjustment = 0.0;
            rateAdjustment_.store(0.0, std::memory_order_relaxed);
            packetsSinceRateCheck = 0;
            continue;
        }
        if (wasPrefilling) {
            prefillComplete_.store(true, std::memory_order_relaxed);
            nextConsumeTime = std::chrono::steady_clock::now() + packetInterval_;
        }

        // ── Adaptive rate matching ──────────────────────────────────
        // Every kRateCheckIntervalPackets, sample the first mapped channel's
        // ring buffer fill level and nudge the consume rate to keep it near 50%.
        ++packetsSinceRateCheck;
        if (packetsSinceRateCheck >= kRateCheckIntervalPackets) {
            packetsSinceRateCheck = 0;

            const int deviceCh = firstMappedDeviceChannel();
            if (deviceCh >= 0 && deviceCh < static_cast<int>(deviceChannels_.size())) {
                auto& ringBuf = deviceChannels_[deviceCh];
                size_t cap = ringBuf.capacity();
                if (cap > 0) {
                    double fillRatio = static_cast<double>(ringBuf.available()) /
                                       static_cast<double>(cap);

                    // The consumer writes to the Core Audio ring. When it is
                    // too full, slow down; when empty, speed up — but only
                    // while packets beyond the prefill cushion are queued.
                    rateAdjustment = nextRateAdjustment(
                        rateAdjustment, fillRatio,
                        packetsAhead(state.expected, sequenceTracker_.newest()),
                        static_cast<int>(kPrefillPacketCount),
                        kTargetFillRatio, kRateAdjustmentGain, kMaxRateAdjustment);
                    rateAdjustment_.store(rateAdjustment, std::memory_order_relaxed);
                }
            }
        }
    }
}

int RTPReceiver::firstMappedDeviceChannel() const {
    if (mapping_.channelMap.empty()) {
        return mapping_.deviceChannelStart;
    }
    for (int mapped : mapping_.channelMap) {
        if (mapped >= 0) {
            return mapped;
        }
    }
    return -1;
}

RTPReceiver::Diagnostics RTPReceiver::getDiagnostics() const {
    Diagnostics d;
    d.received = stats_.packetsReceived.load(std::memory_order_relaxed);
    d.played = playedPackets_.load(std::memory_order_relaxed);
    d.skipped = skippedPackets_.load(std::memory_order_relaxed);
    d.waits = edgeWaits_.load(std::memory_order_relaxed);
    d.trims = latencyTrims_.load(std::memory_order_relaxed);
    d.resyncs = resyncs_.load(std::memory_order_relaxed);
    d.outOfOrder = stats_.outOfOrderPackets.load(std::memory_order_relaxed);
    d.malformed = stats_.malformedPackets.load(std::memory_order_relaxed);
    d.jitterOverruns = stats_.overruns.load(std::memory_order_relaxed);
    d.ringFull = ringFullPackets_.load(std::memory_order_relaxed);
    d.foreign = foreignPackets_.load(std::memory_order_relaxed);
    d.expected = static_cast<uint16_t>(expectedSequenceNumber_.load(std::memory_order_relaxed));
    d.newest = sequenceTracker_.newest();
    d.prefilling = !prefillComplete_.load(std::memory_order_relaxed);
    d.ahead = sequenceTracker_.generation() == 0 ? 0 : packetsAhead(d.expected, d.newest);
    d.jitterBuffered = jitterBuffer_.getBufferedPacketCount();
    const int deviceCh = firstMappedDeviceChannel();
    if (deviceCh >= 0 && deviceCh < static_cast<int>(deviceChannels_.size())) {
        d.ringFill = deviceChannels_[deviceCh].available();
        d.ringCapacity = deviceChannels_[deviceCh].capacity();
    }
    d.rateAdjustment = rateAdjustment_.load(std::memory_order_relaxed);
    return d;
}

void RTPReceiver::statsLoop() {
    // One line per interval with per-interval deltas, so a stall shows up as
    // played=+0 while rx keeps rising. Short sleeps keep stop() responsive.
    Diagnostics last = getDiagnostics();
    bool wasSilent = false;   // a never-connected receiver reports silence after 1 s
    auto nextLog = std::chrono::steady_clock::now() + kStatsLogInterval;

    while (running_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (std::chrono::steady_clock::now() < nextLog) {
            continue;
        }
        nextLog += kStatsLogInterval;

        const Diagnostics d = getDiagnostics();
        const bool silent = d.received == last.received;
        if (silent != wasSilent) {
            wasSilent = silent;
            if (silent) AES67_LOGF("RTPReceiver: no packets for 1 s on %s:%u (sender stopped, cable, or switch IGMP snooping without querier?) (stream=%s)",
                                   sdp_.connectionAddress.c_str(), sdp_.port, sdp_.sessionName.c_str());
            else AES67_LOGF("RTPReceiver: packets arriving again (stream=%s)", sdp_.sessionName.c_str());
        }
        AES67_LOGF("RTPReceiver stats (stream=%s): rx=+%llu played=+%llu skipped=+%llu "
                   "waits=+%llu ooo=+%llu trims=%llu resyncs=%llu malformed=+%llu "
                   "jbOverrun=+%llu ringFull=+%llu foreign=+%llu exp=%u newest=%u ahead=%d jb=%zu "
                   "ring=%zu/%zu rate=%+.3f%%%s",
                   sdp_.sessionName.c_str(),
                   (unsigned long long)(d.received - last.received),
                   (unsigned long long)(d.played - last.played),
                   (unsigned long long)(d.skipped - last.skipped),
                   (unsigned long long)(d.waits - last.waits),
                   (unsigned long long)(d.outOfOrder - last.outOfOrder),
                   (unsigned long long)d.trims,
                   (unsigned long long)d.resyncs,
                   (unsigned long long)(d.malformed - last.malformed),
                   (unsigned long long)(d.jitterOverruns - last.jitterOverruns),
                   (unsigned long long)(d.ringFull - last.ringFull),
                   (unsigned long long)(d.foreign - last.foreign),
                   d.expected, d.newest, d.ahead, d.jitterBuffered,
                   d.ringFill, d.ringCapacity, d.rateAdjustment * 100.0,
                   d.prefilling ? " prefilling" : "");
        last = d;
    }
}

void RTPReceiver::processPacket(const RTP::RTPPacket& packet) {
    if (!validatePacket(packet)) {
        uint64_t count = stats_.malformedPackets.fetch_add(1, std::memory_order_relaxed) + 1;
        // Log first occurrence and then every 100th to avoid flooding
        if (count == 1 || count % 100 == 0) {
            AES67_LOGF("RTPReceiver: malformed packet #%llu (ver=%u pt=%u size=%zu, expected pt=%u) stream=%s",
                       (unsigned long long)count, packet.header.version,
                       packet.header.payloadType, packet.payloadSize,
                       sdp_.payloadType, sdp_.sessionName.c_str());
        }
        return;
    }

    // Stay with the sender we are locked to; a second sender on the same group
    // must neither enter the jitter buffer nor the loss statistics.
    const auto now = std::chrono::steady_clock::now();
    const int64_t nowNs = std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
    if (!sequenceTracker_.admits(packet.header.ssrc, nowNs)) {
        foreignPackets_.fetch_add(1, std::memory_order_relaxed);
        if (packet.header.ssrc != lastForeignSsrc_) {
            lastForeignSsrc_ = packet.header.ssrc;
            char source[INET_ADDRSTRLEN] = "?";
            in_addr address{}; address.s_addr = packet.sourceAddress;
            inet_ntop(AF_INET, &address, source, sizeof(source));
            AES67_LOGF("RTPReceiver: ignoring second sender ssrc=%08x from %s on %s:%u (stream=%s)",
                       packet.header.ssrc, source, sdp_.connectionAddress.c_str(), sdp_.port,
                       sdp_.sessionName.c_str());
        }
        return;
    }

    // Get RTP header info
    uint16_t sequenceNumber = packet.header.sequenceNumber;
    uint32_t timestamp = packet.header.timestamp;

    // Get payload
    uint8_t* payload = packet.payload;
    size_t payloadSize = packet.payloadSize;

    if (!payload || payloadSize == 0) {
        uint64_t count = stats_.malformedPackets.fetch_add(1, std::memory_order_relaxed) + 1;
        if (count == 1 || count % 100 == 0) {
            AES67_LOGF("RTPReceiver: empty payload in packet #%llu (seq=%u) stream=%s",
                       (unsigned long long)count, sequenceNumber, sdp_.sessionName.c_str());
        }
        return;
    }

    // Update connection state. The consume thread derives its expected
    // sequence from sequenceTracker_, including after sender restarts.
    if (!connected_) {
        connected_ = true;
    }
    lastPacketTimeNs_.store(nowNs, std::memory_order_release);

    // Update statistics
    updateStats(sequenceNumber, payloadSize);

    // Calculate presentation time from RTP timestamp delta (wraparound-safe)
    // RTP timestamps are 32-bit and wrap every ~24h at 48kHz.
    // We track the delta from the first timestamp to avoid overflow when
    // multiplying by 1e9, and to correctly handle 32-bit wraparound.
    uint64_t presentationTime = 0;
    if (sdp_.sampleRate > 0) {
        uint32_t firstTs = firstTimestamp_.load(std::memory_order_relaxed);
        if (firstTimestampSet_.load(std::memory_order_relaxed)) {
            // Wraparound-safe delta: unsigned subtraction handles 32-bit wrap
            uint32_t delta = timestamp - firstTs;
            // delta is at most 2^32-1 (~4.29e9). At 384kHz that's ~11184s.
            // 4.29e9 * 1e9 = 4.29e18 < UINT64_MAX (1.84e19), so no overflow.
            presentationTime = (static_cast<uint64_t>(delta) * 1000000000ULL) / sdp_.sampleRate;
        } else {
            // First packet — record baseline timestamp
            firstTimestamp_.store(timestamp, std::memory_order_relaxed);
            firstTimestampSet_.store(true, std::memory_order_relaxed);
            presentationTime = 0;
        }
    }

    // Add packet to jitter buffer (lock-free operation)
    // The jitter buffer will handle reordering based on sequence numbers
    if (!jitterBuffer_.addPacket(payload, payloadSize, sequenceNumber, presentationTime,
                                 sequenceTracker_.epoch())) {
        // Jitter buffer full or slot already occupied
        stats_.overruns.fetch_add(1, std::memory_order_relaxed);
    }

    // Publish the sequence only after the packet is in the buffer, so the
    // consumer never skips a packet that is still being written.
    sequenceTracker_.observe(sequenceNumber, packet.header.ssrc);
    if (sequenceTracker_.generation() != loggedGeneration_) {
        loggedGeneration_ = sequenceTracker_.generation();
        char source[INET_ADDRSTRLEN] = "?";
        in_addr address{}; address.s_addr = packet.sourceAddress;
        inet_ntop(AF_INET, &address, source, sizeof(source));
        AES67_LOGF("RTPReceiver: %s sender %s ssrc=%08x at seq %u (stream=%s)",
                   loggedGeneration_ == 1 ? "receiving from" : "switched to", source,
                   packet.header.ssrc, sequenceNumber, sdp_.sessionName.c_str());
    }
}

bool RTPReceiver::validatePacket(const RTP::RTPPacket& packet) {
    // SimpleRTP decodes a fixed 12-byte header, so CSRCs, extensions and
    // padding require a different parser and must not be treated as PCM.
    if (packet.header.cc || packet.header.extension || packet.header.padding) {
        return false;
    }
    if (!sdp_.sourceAddress.empty() && sdp_.sourceAddress != "0.0.0.0") {
        struct in_addr source{};
        if (inet_pton(AF_INET, sdp_.sourceAddress.c_str(), &source) != 1 ||
            packet.sourceAddress != source.s_addr) {
            return false;
        }
    }
    // Check RTP version (should be 2)
    if (packet.header.version != 2) {
        return false;
    }

    // Check payload type matches SDP
    if (packet.header.payloadType != sdp_.payloadType) {
        return false;
    }

    // Check payload size is reasonable
    if (packet.payloadSize == 0 || packet.payloadSize > 1500) {
        return false;
    }
    const size_t bytesPerSample = sdp_.encoding == "L24" ? 3 :
                                  sdp_.encoding == "L16" ? 2 : 0;
    if (bytesPerSample == 0 || sdp_.numChannels == 0 ||
        packet.payloadSize % (bytesPerSample * sdp_.numChannels) != 0 ||
        packet.payloadSize / (bytesPerSample * sdp_.numChannels) > 512) {
        return false;
    }

    return true;
}

void RTPReceiver::decodeL16(const uint8_t* payload, size_t payloadSize) {
    // L16: 16-bit big-endian signed PCM
    const size_t bytesPerSample = 2;
    const size_t bytesPerFrame = bytesPerSample * sdp_.numChannels;
    const size_t frameCount = payloadSize / bytesPerFrame;

    if (frameCount == 0 || frameCount > 512) {
        return; // Invalid or excessive frame count
    }

    // Ensure audio buffer is large enough
    const size_t totalSamples = frameCount * sdp_.numChannels;
    if (totalSamples > audioBuffer_.size()) {
        stats_.malformedPackets.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    // Decode: big-endian int16 → float [-1.0, 1.0)
    for (size_t i = 0; i < totalSamples; ++i) {
        const size_t offset = i * bytesPerSample;
        if (offset + bytesPerSample > payloadSize) break;

        // Big-endian 16-bit signed (assemble as unsigned to avoid
        // implementation-defined narrowing from promoted int to int16_t)
        uint16_t rawSample = (static_cast<uint16_t>(payload[offset]) << 8) | payload[offset + 1];
        int16_t pcmSample = static_cast<int16_t>(rawSample);

        // Convert to float: divide by 32768 (2^15)
        audioBuffer_[i] = pcmSample / 32768.0f;
    }

    // Map to device channels
    mapChannelsToDevice(audioBuffer_.data(), frameCount);
}

void RTPReceiver::decodeL24(const uint8_t* payload, size_t payloadSize) {
    // L24: 24-bit big-endian signed PCM
    const size_t bytesPerSample = 3;
    const size_t bytesPerFrame = bytesPerSample * sdp_.numChannels;
    const size_t frameCount = payloadSize / bytesPerFrame;

    if (frameCount == 0 || frameCount > 512) {
        return; // Invalid or excessive frame count
    }

    // Ensure audio buffer is large enough
    const size_t totalSamples = frameCount * sdp_.numChannels;
    if (totalSamples > audioBuffer_.size()) {
        stats_.malformedPackets.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    // Decode: big-endian int24 → float [-1.0, 1.0)
    for (size_t i = 0; i < totalSamples; ++i) {
        const size_t offset = i * bytesPerSample;
        if (offset + bytesPerSample > payloadSize) break;

        // Big-endian 24-bit signed (sign-extend to 32-bit)
        // Cast to uint32_t before shifting to avoid undefined behavior
        // when high byte >= 128 causes signed int overflow on << 24
        uint32_t rawSample = (static_cast<uint32_t>(payload[offset]) << 24) |
                             (static_cast<uint32_t>(payload[offset + 1]) << 16) |
                             (static_cast<uint32_t>(payload[offset + 2]) << 8);
        int32_t pcmSample = static_cast<int32_t>(rawSample);
        pcmSample >>= 8; // Arithmetic right shift preserves sign

        // Convert to float: divide by 8388608 (2^23)
        audioBuffer_[i] = pcmSample / 8388608.0f;
    }

    // Map to device channels
    mapChannelsToDevice(audioBuffer_.data(), frameCount);
}

void RTPReceiver::mapChannelsToDevice(const float* interleavedAudio, size_t frameCount) {
    // Validate mapping
    // Stack-allocated temporary buffer for de-interleaving
    constexpr size_t kMaxFrames = 4096;
    if (frameCount > kMaxFrames) {
        return;
    }

    float channelBuffer[kMaxFrames];

    // Write each stream channel to its mapped device channel
    // This de-interleaves: [ch0_f0, ch1_f0, ch0_f1, ch1_f1, ...]
    //                   → deviceChannels[0]: [ch0_f0, ch0_f1, ...]
    //                   → deviceChannels[1]: [ch1_f0, ch1_f1, ...]

    bool hadUnderrun = false;

    for (size_t streamChannel = 0; streamChannel < sdp_.numChannels; ++streamChannel) {
        const int mappedChannel = mapping_.channelMap.empty()
            ? static_cast<int>(mapping_.deviceChannelStart + streamChannel)
            : mapping_.channelMap[streamChannel];
        if (mappedChannel == -1) continue;
        const size_t deviceChannel = static_cast<size_t>(mappedChannel);
        if (deviceChannel >= StreamChannelMapper::kMaxDeviceChannels) {
            return;
        }

        // Extract this channel from interleaved stream
        for (size_t frame = 0; frame < frameCount; ++frame) {
            channelBuffer[frame] = interleavedAudio[frame * sdp_.numChannels + streamChannel];
        }

        // Write to device ring buffer (batch write)
        const size_t written = deviceChannels_[deviceChannel].write(channelBuffer, frameCount);

        if (written < frameCount && !hadUnderrun) {
            // Ring buffer full - count underrun once per packet
            stats_.underruns.fetch_add(1, std::memory_order_relaxed);
            ringFullPackets_.fetch_add(1, std::memory_order_relaxed);
            hadUnderrun = true;
        }
    }
}

void RTPReceiver::updateStats(uint16_t sequenceNumber, size_t payloadSize) {
    // Atomic operations eliminate need for mutex lock

    // Detect packet loss (sequence number gaps)
    // Use signed 16-bit arithmetic so that the two's-complement result
    // correctly distinguishes forward gaps (lost packets) from backward
    // gaps (reordered/duplicate packets), even across 16-bit wraparound.
    uint64_t currentPacketCount = stats_.packetsReceived.load(std::memory_order_relaxed);
    if (currentPacketCount > 0) {
        uint16_t expected = lastSequenceNumber_.load(std::memory_order_relaxed) + 1;
        if (sequenceNumber != expected) {
            int16_t gap = static_cast<int16_t>(sequenceNumber - expected);
            if (gap > 0) {
                // Forward gap: packets between expected and sequenceNumber were lost
                stats_.packetsLost.fetch_add(static_cast<uint64_t>(gap), std::memory_order_relaxed);
            } else {
                // Negative gap: packet arrived out of order (or duplicate)
                stats_.outOfOrderPackets.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }

    lastSequenceNumber_.store(sequenceNumber, std::memory_order_relaxed);
    stats_.packetsReceived.fetch_add(1, std::memory_order_relaxed);
    stats_.bytesReceived.fetch_add(payloadSize, std::memory_order_relaxed);
}

} // namespace AES67
