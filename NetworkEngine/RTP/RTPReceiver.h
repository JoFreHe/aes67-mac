/// @file RTPReceiver.h
/// @brief RTP packet receiver with L16/L24 decoding and channel mapping.

#pragma once

#include "../../Shared/Types.h"
#include "../../Shared/RingBuffer.hpp"
#include "../../Driver/SDPParser.h"
#include "../StreamChannelMapper.h"
#include "../NetworkInterfaceDetection.h"
#include "SimpleRTP.h"
#include "LockFreeCircularJitterBuffer.h"
#include "RxSequenceSync.hpp"
#include "../../Driver/AudioThreadPriority.h"
#include <thread>
#include <atomic>
#include <memory>
#include <functional>
#include <chrono>

namespace AES67 {

/// Receives RTP audio packets from a multicast group and writes decoded
/// audio to device ring buffers via channel mapping.
///
/// Uses two threads: receiveLoop() for network I/O into a jitter buffer,
/// and consumeLoop() for paced readout into per-channel ring buffers.
/// Includes adaptive rate matching (P-controller) to compensate for clock drift.
class RTPReceiver {
public:
    using DeviceChannelBuffers = std::array<SPSCRingBuffer<float>, 128>;

    /// Receive-path state for the once-per-second log line and tests.
    struct Diagnostics {
        uint64_t received = 0;   ///< valid packets from the network
        uint64_t played = 0;     ///< packets decoded into the device rings
        uint64_t skipped = 0;    ///< missing packets skipped (newer data existed)
        uint64_t waits = 0;      ///< intervals at the live edge without data
        uint64_t trims = 0;      ///< latency bound exceeded, backlog dropped
        uint64_t resyncs = 0;    ///< sender restarts or lost lock
        uint64_t outOfOrder = 0;
        uint64_t malformed = 0;
        uint64_t jitterOverruns = 0;
        uint64_t ringFull = 0;   ///< packets the device ring could not take completely
        uint64_t foreign = 0;    ///< packets from a second sender on the group, ignored
        uint16_t expected = 0;
        uint16_t newest = 0;
        int ahead = 0;           ///< queued packets from expected to newest
        size_t jitterBuffered = 0;
        size_t ringFill = 0;
        size_t ringCapacity = 0;
        double rateAdjustment = 0.0;
        bool prefilling = true;
    };

    /// @param sdp SDP session describing the stream to receive.
    /// @param mapping Channel mapping from stream channels to device channels.
    /// @param deviceChannels Reference to device input ring buffers.
    /// @param jitterBufferDepth Jitter buffer slots (0=default 256, clamped [32,4096], rounded to power-of-2).
    /// @param networkInterface Interface name ("en0") or IP to bind multicast. Empty = INADDR_ANY.
    RTPReceiver(
        const SDPSession& sdp,
        const ChannelMapping& mapping,
        DeviceChannelBuffers& deviceChannels,
        size_t jitterBufferDepth = 0,
        const std::string& networkInterface = ""
    );

    ~RTPReceiver();

    // Prevent copy/move
    RTPReceiver(const RTPReceiver&) = delete;
    RTPReceiver& operator=(const RTPReceiver&) = delete;

    //
    // Control
    //

    // Start receiving
    bool start();

    // Stop receiving
    void stop();

    // Check if currently receiving
    bool isRunning() const { return running_.load(); }

    //
    // Status
    //

    // Get statistics (returns a non-atomic snapshot)
    StatisticsSnapshot getStatistics() const;

    // Reset statistics
    void resetStatistics();

    // Get connection status
    bool isConnected() const;

    // Get time since last packet (milliseconds)
    int64_t getTimeSinceLastPacket() const;

    // Receive-path diagnostics (safe from any thread)
    Diagnostics getDiagnostics() const;

    //
    // Configuration
    //

    // Update channel mapping (stops and restarts receiver)
    bool updateMapping(const ChannelMapping& newMapping);

    // Get current SDP session
    const SDPSession& getSDPSession() const { return sdp_; }

    // Get current mapping
    const ChannelMapping& getMapping() const { return mapping_; }

private:
    // Network thread function (producer - adds packets to jitter buffer)
    void receiveLoop();

    // Consumer thread function (reads from jitter buffer and writes to ring buffers)
    void consumeLoop();

    // Low-priority thread writing one diagnostics line per second
    void statsLoop();

    // First mapped device channel (rate controller and diagnostics)
    int firstMappedDeviceChannel() const;

    // Packet processing
    void processPacket(const RTP::RTPPacket& packet);
    bool validatePacket(const RTP::RTPPacket& packet);

    // Audio decoding
    void decodeL16(const uint8_t* payload, size_t payloadSize);
    void decodeL24(const uint8_t* payload, size_t payloadSize);

    // Channel mapping: stream audio → device channels
    void mapChannelsToDevice(const float* interleavedAudio, size_t frameCount);

    // Statistics tracking
    void updateStats(uint16_t sequenceNumber, size_t payloadSize);

    // Consumer pacing (mirrors RTPTransmitter::packetInterval_)
    std::chrono::microseconds packetInterval_;

    // Pre-fill gate: after start and every resync the consumer waits for this
    // many new packets before paced consumption, keeping a jitter cushion
    std::atomic<bool> prefillComplete_{false};
    static constexpr size_t kPrefillPacketCount = 6;

    // Latency bound: more queued packets than this drops the backlog
    // (sender clock faster than Core Audio over a long run)
    static constexpr int kMaxLeadPackets = 64;

    // Adaptive rate matching: lightweight P-controller to compensate for
    // clock drift between network sender and local Core Audio clock.
    // Adjusts consume interval based on ring buffer fill level; it may only
    // speed up while the jitter buffer holds more than the prefill cushion.
    static constexpr size_t kRateCheckIntervalPackets = 48;   // check every ~48ms
    static constexpr double kMaxRateAdjustment = 0.005;       // +/- 0.5%
    static constexpr double kTargetFillRatio = 0.5;           // 50% ring buffer fill
    static constexpr double kRateAdjustmentGain = 0.0001;     // very gentle P-controller

    static constexpr std::chrono::seconds kStatsLogInterval{1};

    // Configuration
    SDPSession sdp_;
    ChannelMapping mapping_;
    DeviceChannelBuffers& deviceChannels_;

    // RTP socket
    RTP::RTPSocket rtpSocket_;

    // Jitter buffer for packet reordering
    LockFreeCircularJitterBuffer jitterBuffer_;

    // Newest received sequence, stream restarts and jitter buffer epoch
    RxSequenceTracker sequenceTracker_;

    // Threading
    std::thread receiveThread_;
    std::thread consumeThread_;
    std::thread statsThread_;
    std::atomic<bool> running_{false};

    // Expected sequence number of the consumer (published for diagnostics)
    std::atomic<uint32_t> expectedSequenceNumber_{0};

    // Consumer diagnostics (written by the consume thread only)
    std::atomic<uint64_t> playedPackets_{0};
    std::atomic<uint64_t> skippedPackets_{0};
    std::atomic<uint64_t> edgeWaits_{0};
    std::atomic<uint64_t> latencyTrims_{0};
    std::atomic<uint64_t> resyncs_{0};
    std::atomic<uint64_t> ringFullPackets_{0};
    std::atomic<uint64_t> foreignPackets_{0};
    uint32_t lastForeignSsrc_ = 0;   // receive thread only, for one log line per sender
    uint32_t loggedGeneration_ = 0;  // receive thread only: logs each sender lock
    std::atomic<uint64_t> receiveErrors_{0};
    void noteReceiveError(const char* where);
    std::atomic<double> rateAdjustment_{0.0};

    // Statistics (atomic operations, no mutex needed for individual updates)
    Statistics stats_;
    std::atomic<uint16_t> lastSequenceNumber_{0};
    std::atomic<uint32_t> lastTimestamp_{0};

    // Connection state
    std::atomic<bool> connected_{false};
    std::atomic<int64_t> lastPacketTimeNs_{0};

    // RTP timestamp wraparound tracking
    std::atomic<uint32_t> firstTimestamp_{0};
    std::atomic<bool> firstTimestampSet_{false};

    // Audio buffer (reused to avoid allocations)
    std::vector<float> audioBuffer_;

    // Receive buffer for network packets (max MTU 1500 bytes)
    uint8_t receiveBuffer_[2048];

    // Jitter buffer read buffer
    uint8_t jitterReadBuffer_[1500];

    // Network interface binding
    std::string networkInterface_;   // Interface name or IP from config
    std::string resolvedInterfaceIP_; // Resolved IP address (empty = INADDR_ANY)
};

} // namespace AES67
