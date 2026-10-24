/// @file RTPTransmitter.h
/// @brief RTP packet transmitter with L16/L24 encoding and channel mapping.

#pragma once

#include "../../Shared/Types.h"
#include "../../Shared/RingBuffer.hpp"
#include "../../Driver/SDPParser.h"
#include "../StreamChannelMapper.h"
#include "SimpleRTP.h"
#include "ClockRateBridge.hpp"
#include "TxTimingDiagnostics.hpp"
#include "../../Driver/AudioThreadPriority.h"
#include <thread>
#include <atomic>
#include <memory>

namespace AES67 {
struct TxClockShared;

/// Reads audio from device output ring buffers and transmits as RTP multicast packets.
///
/// Transmit pacing follows a measured network clock; hardware validation is required.
/// Sends continuous packets (including silence) for receiver clock recovery.
class RTPTransmitter {
public:
    using DeviceChannelBuffers = std::array<SPSCRingBuffer<float>, 128>;

    /// @param sdp SDP session describing the TX stream configuration.
    /// @param mapping Channel mapping from device channels to stream channels.
    /// @param deviceChannels Reference to device output ring buffers.
    /// @param networkInterface Interface name ("en0") or IP to bind multicast. Empty = default.
    RTPTransmitter(
        const SDPSession& sdp,
        const ChannelMapping& mapping,
        DeviceChannelBuffers& deviceChannels,
        const std::string& networkInterface = ""
    );

    ~RTPTransmitter();

    // Prevent copy/move
    RTPTransmitter(const RTPTransmitter&) = delete;
    RTPTransmitter& operator=(const RTPTransmitter&) = delete;

    //
    // Control
    //

    bool start();
    void stop();
    bool isRunning() const { return running_.load(); }

    //
    // Status
    //

    StatisticsSnapshot getStatistics() const;
    void resetStatistics();

    //
    // Configuration
    //

    bool updateMapping(const ChannelMapping& newMapping);
    const SDPSession& getSDPSession() const { return sdp_; }
    const ChannelMapping& getMapping() const { return mapping_; }

private:
    // Transmit thread function
    void transmitLoop();

    // Low-priority thread writing one statistics line every 10 s
    void statsLoop();

    // Read audio from device channels and interleave
    bool readDeviceChannels(float* interleavedAudio, size_t frameCount);

    // Audio encoding
    void encodeL16(const float* audio, size_t frameCount, uint8_t* payload);
    void encodeL24(const float* audio, size_t frameCount, uint8_t* payload);

    // Send RTP packet
    void sendPacket(const uint8_t* payload, size_t payloadSize, uint32_t timestamp);

    // Reopen the socket when the adapter's IPv4 address changed (cable or
    // DHCP change); returns true after a successful rebind. Transmit thread only.
    bool refreshInterfaceAddress();
    std::chrono::steady_clock::time_point nextInterfaceCheck_{};

    // Configuration
    SDPSession sdp_;
    ChannelMapping mapping_;
    DeviceChannelBuffers& deviceChannels_;
    std::string networkInterface_;

    std::shared_ptr<TxClockShared> networkClock_;
    bool syntheticLocalTest_{false};
    std::string interfaceIP_;

    std::unique_ptr<ClockRateBridge> rateBridge_;
    double rateRatio_{1.0};
    bool audioPrefilled_{false};

    // RTP socket
    RTP::RTPSocket rtpSocket_;

    // Threading
    std::thread transmitThread_;
    std::thread statsThread_;
    std::atomic<uint64_t> packetsSent_{0};
    std::atomic<bool> running_{false};

    // Statistics (atomic operations, no mutex needed for individual updates)
    Statistics stats_;
    TxTimingDiagnostics txTiming_;

    // RTP state
    uint16_t sequenceNumber_{0};
    uint32_t timestamp_{0};
    uint32_t ssrc_{0};

    // Timing
    std::chrono::steady_clock::time_point startTime_;
    std::chrono::microseconds packetInterval_;

    // Audio buffer (reused to avoid allocations)
    std::vector<float> audioBuffer_;
    std::vector<uint8_t> payloadBuffer_;
};

} // namespace AES67
