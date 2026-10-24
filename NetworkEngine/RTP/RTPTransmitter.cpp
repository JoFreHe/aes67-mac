//
// RTPTransmitter.cpp
// AES67 macOS Driver - Build #9
// RTP packet transmitter with L16/L24 encoding and channel mapping
//

#include "RTPTransmitter.h"
#include "SimpleRTP.h"
#include "TxNetworkSupport.hpp"
#include "../../Driver/DebugLog.h"
#include <cstring>
#include <random>
#include <chrono>
#include <cerrno>

namespace AES67 {

RTPTransmitter::RTPTransmitter(
    const SDPSession& sdp,
    const ChannelMapping& mapping,
    DeviceChannelBuffers& deviceChannels,
    const std::string& networkInterface
)
    : sdp_(sdp)
    , mapping_(mapping)
    , deviceChannels_(deviceChannels)
    , networkInterface_(networkInterface)
{
    resetStatistics();

    rateBridge_ = std::make_unique<ClockRateBridge>(sdp_.numChannels);

    // Generate random SSRC
    std::random_device rd;
    ssrc_ = rd();

    // Pre-allocate buffers to avoid allocations in transmitLoop()
    // Audio buffer: max 512 frames × stream channels
    const size_t maxFrames = 512;
    const size_t maxAudioSamples = maxFrames * sdp_.numChannels;
    audioBuffer_.resize(maxAudioSamples);

    // Payload buffer: RTP header (12 bytes) + max audio payload
    // L24 is largest: 3 bytes/sample × channels × frames
    const size_t maxPayloadSize = 3 * sdp_.numChannels * maxFrames;
    payloadBuffer_.resize(12 + maxPayloadSize);

    // Calculate packet interval based on sample rate
    // AES67 standard: 1ms packets = sample_rate / 1000 samples per packet
    // Example: 48000 Hz → 48 samples/packet → 1ms interval
    const uint64_t intervalUs = 1000; // 1ms in microseconds
    packetInterval_ = std::chrono::microseconds(intervalUs);
}

RTPTransmitter::~RTPTransmitter() {
    stop();
}

bool RTPTransmitter::start() {
    if (running_ || rtpSocket_.isOpen()) {
        AES67_LOGF("RTPTransmitter::start: already running or socket open (stream=%s)",
                   sdp_.sessionName.c_str());
        return false; // Already running
    }

    // Validate SDP configuration
    if (sdp_.connectionAddress.empty() || sdp_.port == 0) {
        AES67_LOGF("RTPTransmitter::start: invalid SDP - address='%s' port=%u (stream=%s)",
                   sdp_.connectionAddress.c_str(), sdp_.port, sdp_.sessionName.c_str());
        return false;
    }

    if (sdp_.numChannels == 0 || sdp_.numChannels > 128) {
        AES67_LOGF("RTPTransmitter::start: invalid channel count %u (stream=%s)",
                   sdp_.numChannels, sdp_.sessionName.c_str());
        return false;
    }

    auto reject = [this](const char* reason) {
        AES67_LOGF("RTPTransmitter::start: %s; not sending (stream=%s)", reason, sdp_.sessionName.c_str());
        return false;
    };
    if (!mapping_.channelMap.empty() && mapping_.channelMap.size()!=sdp_.numChannels) return reject("channel map does not match the channel count");
    for (int channel : mapping_.channelMap) if(channel < -1 || channel >= int(deviceChannels_.size())) return reject("channel map points outside the device");
    if (sdp_.framecount == 0 || sdp_.framecount > 512 || sdp_.sampleRate == 0 ||
        (sdp_.encoding != "L16" && sdp_.encoding != "L24") ||
        size_t(sdp_.numChannels) * sdp_.framecount * (sdp_.encoding == "L24" ? 3 : 2) > 1440 ||
        mapping_.deviceChannelStart + sdp_.numChannels > deviceChannels_.size()) return reject("unsupported format or channel range");
    syntheticLocalTest_ = sdp_.mediaClockType == "local-test";
    if (!syntheticLocalTest_ && (sdp_.sampleRate!=48000 || sdp_.framecount!=48 || sdp_.numChannels>8)) return reject("AES67 needs 48 kHz, 48 frames and at most 8 channels");
    auto interface = txInterface(networkInterface_);
    if (!syntheticLocalTest_ && interface.first.empty()) {
        AES67_LOGF("RTPTransmitter::start: interface '%s' not found or without IPv4 address (adapter unplugged or renamed?); not sending (stream=%s)",
                   networkInterface_.c_str(), sdp_.sessionName.c_str());
        return false;
    }
    if (!syntheticLocalTest_ && (sdp_.ptpDomain < 0 || sdp_.ptpDomain > 127)) return reject("PTP domain outside 0-127");
    interfaceIP_ = interface.second;
    if (!syntheticLocalTest_) {
        networkClock_ = txClock(interface.first, sdp_.ptpDomain);
        if (!networkClock_) return reject("clock status reader unavailable");
        sdp_.originAddress = interfaceIP_;
        sdp_.sourceAddress = interfaceIP_;
        sdp_.direction = "sendonly";
    }
    packetInterval_ = std::chrono::microseconds((uint64_t(sdp_.framecount)*1000000ULL)/sdp_.sampleRate);
    // Open RTP transmitter socket on the explicitly selected adapter.
    const char* ifaceIP = interfaceIP_.empty() ? nullptr : interfaceIP_.c_str();
    if (!rtpSocket_.openTransmitter(sdp_.connectionAddress.c_str(), sdp_.port, ifaceIP)) {
        AES67_LOGF("RTPTransmitter::start: socket open failed for %s:%u iface=%s (stream=%s)",
                   sdp_.connectionAddress.c_str(), sdp_.port,
                   networkInterface_.empty() ? "ANY" : networkInterface_.c_str(),
                   sdp_.sessionName.c_str());
        return false;
    }

    rateBridge_->reset(); rateRatio_=1.0; audioPrefilled_=false;

    // Initialize timestamp and sequence number
    timestamp_ = 0;
    sequenceNumber_ = 0;

    // Record start time for precise packet timing
    startTime_ = std::chrono::steady_clock::now();

    AES67_LOGF("RTPTransmitter::start: sending to %s:%u from %s (%s)%s (stream=%s)",
               sdp_.connectionAddress.c_str(), sdp_.port, networkInterface_.c_str(), interfaceIP_.c_str(),
               syntheticLocalTest_ ? ", local test without PTP" : "", sdp_.sessionName.c_str());

    // Start transmit thread (elevated priority to prevent audio dropouts)
    running_ = true;
    transmitThread_ = std::thread([this]() {
        if (!AudioThreadPriority::configureForRealTime()) {
            AES67_LOGF("RTPTransmitter: failed to set RT priority on transmit thread (stream=%s)",
                       sdp_.sessionName.c_str());
        }
        transmitLoop();
    });
    statsThread_ = std::thread([this]() { statsLoop(); });

    return true;
}

void RTPTransmitter::stop() {
    if (!running_) {
        return;
    }

    running_ = false;

    if (transmitThread_.joinable()) {
        transmitThread_.join();
    }
    if (statsThread_.joinable()) {
        statsThread_.join();
    }

    rtpSocket_.close();
    networkClock_.reset();
    AES67_LOGF("RTPTransmitter stopped (stream=%s): sent %llu packets, send errors %llu, audio underruns %llu",
               sdp_.sessionName.c_str(), (unsigned long long)packetsSent_.load(),
               (unsigned long long)stats_.malformedPackets.load(), (unsigned long long)stats_.underruns.load());
}

StatisticsSnapshot RTPTransmitter::getStatistics() const {
    // Return a consistent snapshot of atomic statistics
    return stats_.snapshot();
}

void RTPTransmitter::resetStatistics() {
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
    txTiming_.reset();
}

bool RTPTransmitter::updateMapping(const ChannelMapping& newMapping) {
    // Validate mapping
    if (newMapping.deviceChannelStart + sdp_.numChannels > 128) {
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

void RTPTransmitter::transmitLoop() {
    const size_t frames=sdp_.framecount;
    const uint64_t period=uint64_t(frames)*1000000000ULL/sdp_.sampleRate;
    bool acquired=false, advertised=false, sapOk=false;
    int loggedLock=-1; // unknown: the first state is always logged
    uint64_t nextPTP=0;
    uint64_t packetDeadlinePTP=0;
    auto nextLocal=std::chrono::steady_clock::now();
    auto nextSAP=nextLocal;
    while(running_) {
        if(std::chrono::steady_clock::now()>=nextInterfaceCheck_) {
            nextInterfaceCheck_=std::chrono::steady_clock::now()+std::chrono::seconds(1);
            if(refreshInterfaceAddress()) {
                // Announcements carry the source address; withdraw nothing on the
                // old address (it is gone) and re-announce from the new one.
                advertised=false; nextSAP=std::chrono::steady_clock::now();
            }
        }
        if(!syntheticLocalTest_) {
            const bool locked=networkClock_->clock.isLocked();
            if(int(locked)!=loggedLock) {
                // Transitions only: the reason names the interface/domain mismatch,
                // a stale clock service or a missing PTP master.
                loggedLock=int(locked);
                AES67_LOGF("RTPTransmitter: %s: %s (stream=%s)", locked?"sending":"waiting, not sending",
                           networkClock_->clock.describe().c_str(), sdp_.sessionName.c_str());
            }
            if(!locked) {
                if(advertised){announceTx(sdp_,interfaceIP_,uint16_t(ssrc_),true);advertised=false;}
                acquired=false; audioPrefilled_=false; rateBridge_->reset();
                // Drain stale output while waiting; do not replay old buffers after clock lock.
                readDeviceChannels(audioBuffer_.data(),frames);
                std::this_thread::sleep_for(std::chrono::milliseconds(1));continue;
            }
            uint64_t now=networkClock_->now();
            if(!acquired || now > nextPTP+20*period) {
                if (acquired && now > nextPTP) {
                    txTiming_.recordResync(now - nextPTP);
                }
                nextPTP=(now/period+1)*period;
                timestamp_=ptpSampleTimestamp(nextPTP-period,sdp_.sampleRate);
                acquired=true;
                sdp_.ptpMasterMAC=networkClock_->clock.getGrandmasterID();
                nextSAP=std::chrono::steady_clock::now();
            }
            if(now<nextPTP){std::this_thread::sleep_for(std::chrono::nanoseconds(std::min<uint64_t>(nextPTP-now,1000000)));continue;}
            packetDeadlinePTP=nextPTP;
            nextPTP+=period;
            if(std::chrono::steady_clock::now()>=nextSAP){
                const bool ok=announceTx(sdp_,interfaceIP_,uint16_t(ssrc_),false);
                if(ok!=sapOk) {
                    sapOk=ok;
                    if(ok) AES67_LOGF("RTPTransmitter: SAP announcement sent from %s for %s:%u (stream=%s)",
                                      interfaceIP_.c_str(), sdp_.connectionAddress.c_str(), sdp_.port, sdp_.sessionName.c_str());
                    else AES67_LOGF("RTPTransmitter: SAP announcement failed from %s (errno=%d: %s); Dante Controller will not list this sender (stream=%s)",
                                    interfaceIP_.c_str(), errno, strerror(errno), sdp_.sessionName.c_str());
                }
                advertised=ok;
                nextSAP=std::chrono::steady_clock::now()+std::chrono::seconds(5);
            }
        } else {
            std::this_thread::sleep_until(nextLocal);
            nextLocal+=packetInterval_;
            // Bound catch-up after debugger/scheduler pauses.
            if(std::chrono::steady_clock::now()>nextLocal+std::chrono::milliseconds(20))nextLocal=std::chrono::steady_clock::now();
        }
        if (syntheticLocalTest_) {
            if(!readDeviceChannels(audioBuffer_.data(),frames)) stats_.underruns.fetch_add(1);
        } else {
            size_t available=deviceChannels_[mapping_.deviceChannelStart].available();
            constexpr size_t target=768; // 16ms elasticity for older Macs.
            if(!audioPrefilled_ && available>=target) audioPrefilled_=true;
            if(audioPrefilled_) {
                double desired=1.0+std::clamp((double(available)-double(target))*0.000002,-0.002,0.002);
                rateRatio_ += (desired-rateRatio_)*0.002;
                if(!rateBridge_->render(audioBuffer_.data(),frames,rateRatio_,[this](float* p,size_t n){return readDeviceChannels(p,n);})) {
                    stats_.underruns.fetch_add(1);audioPrefilled_=false;rateBridge_->reset();
                }
            } else std::fill_n(audioBuffer_.data(),frames*sdp_.numChannels,0.0f);
        }
        size_t size=frames*sdp_.numChannels*(sdp_.encoding=="L16"?2:3);
        if(sdp_.encoding=="L16")encodeL16(audioBuffer_.data(),frames,payloadBuffer_.data());
        else encodeL24(audioBuffer_.data(),frames,payloadBuffer_.data());
        if (!syntheticLocalTest_ && packetDeadlinePTP != 0) {
            txTiming_.recordDeadline(packetDeadlinePTP, networkClock_->now(), period);
        }
        sendPacket(payloadBuffer_.data(),size,timestamp_);
        timestamp_+=uint32_t(frames);
    }
    if(advertised)announceTx(sdp_,interfaceIP_,uint16_t(ssrc_),true);
}

bool RTPTransmitter::readDeviceChannels(float* interleavedAudio, size_t frameCount) {
    std::fill_n(interleavedAudio,frameCount*sdp_.numChannels,0.0f);
    // Validate mapping
    const size_t deviceChannelEnd = mapping_.deviceChannelStart + sdp_.numChannels;
    if (deviceChannelEnd > 128) {
        return false;
    }

    // Stack-allocated temporary buffer for reading each channel
    constexpr size_t kMaxFrames = 512;
    if (frameCount > kMaxFrames) {
        return false;
    }

    float channelBuffer[kMaxFrames];
    bool hadUnderrun = false;

    // Read each device channel and interleave into output
    // Result: [ch0_f0, ch1_f0, ch0_f1, ch1_f1, ...]
    for (size_t streamChannel = 0; streamChannel < sdp_.numChannels; ++streamChannel) {
        const int mapped = mapping_.channelMap.empty() ? int(mapping_.deviceChannelStart + streamChannel) : mapping_.channelMap[streamChannel];
        if(mapped<0 || size_t(mapped)>=deviceChannels_.size()) {
            for(size_t frame=0;frame<frameCount;++frame) interleavedAudio[frame*sdp_.numChannels+streamChannel]=0;
            continue;
        }
        const size_t deviceChannel = size_t(mapped);

        // Batch read from ring buffer
        const size_t samplesRead = deviceChannels_[deviceChannel].read(channelBuffer, frameCount);

        if (samplesRead < frameCount) {
            // Ring buffer underrun - fill remainder with silence
            std::memset(&channelBuffer[samplesRead], 0,
                       (frameCount - samplesRead) * sizeof(float));
            hadUnderrun = true;
        }

        // Interleave this channel into output
        for (size_t frame = 0; frame < frameCount; ++frame) {
            interleavedAudio[frame * sdp_.numChannels + streamChannel] = channelBuffer[frame];
        }
    }

    // Return false if we had underrun (indicates audio not ready)
    return !hadUnderrun;
}

void RTPTransmitter::encodeL16(const float* audio, size_t frameCount, uint8_t* payload) {
    // L16: 16-bit big-endian signed PCM
    const size_t totalSamples = frameCount * sdp_.numChannels;

    for (size_t i = 0; i < totalSamples; ++i) {
        // Clamp to [-1.0, 1.0] and convert to int16
        float value = std::max(-1.0f, std::min(1.0f, audio[i]));
        int16_t pcmSample = static_cast<int16_t>(value * 32767.0f);

        // Big-endian encoding
        payload[i * 2 + 0] = (pcmSample >> 8) & 0xFF;
        payload[i * 2 + 1] = pcmSample & 0xFF;
    }
}

void RTPTransmitter::encodeL24(const float* audio, size_t frameCount, uint8_t* payload) {
    // L24: 24-bit big-endian signed PCM
    const size_t totalSamples = frameCount * sdp_.numChannels;

    for (size_t i = 0; i < totalSamples; ++i) {
        // Clamp to [-1.0, 1.0] and convert to int32 (24-bit range)
        float value = std::max(-1.0f, std::min(1.0f, audio[i]));
        int32_t pcmSample = static_cast<int32_t>(value * 8388607.0f); // 2^23 - 1

        // Big-endian 24-bit encoding (only write 3 bytes)
        payload[i * 3 + 0] = (pcmSample >> 16) & 0xFF;
        payload[i * 3 + 1] = (pcmSample >> 8) & 0xFF;
        payload[i * 3 + 2] = pcmSample & 0xFF;
    }
}

void RTPTransmitter::statsLoop() {
    // Every 10 s, deltas: a sender that stopped shows sent=+0 with the reason
    // already logged on the lock transition.
    uint64_t lastSent = packetsSent_.load(), lastErrors = stats_.malformedPackets.load(), lastUnderruns = stats_.underruns.load();
    uint64_t lastLatePackets = txTiming_.latePackets();
    uint64_t lastResyncs = txTiming_.resyncs();
    auto nextLog = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (running_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (std::chrono::steady_clock::now() < nextLog) continue;
        nextLog += std::chrono::seconds(10);
        const uint64_t sent = packetsSent_.load(), errors = stats_.malformedPackets.load(), underruns = stats_.underruns.load();
        const uint64_t latePackets = txTiming_.latePackets();
        const uint64_t resyncs = txTiming_.resyncs();
        const uint64_t maxLatenessNs = txTiming_.maxLatenessNs();
        const size_t fill = mapping_.deviceChannelStart < deviceChannels_.size() ? deviceChannels_[mapping_.deviceChannelStart].available() : 0;
        // Packet readiness before send(), not kernel or on-wire delivery time.
        AES67_LOGF("RTPTransmitter stats (stream=%s): sent=+%llu sendErrors=+%llu audioUnderruns=+%llu readyLate>=period=+%llu scheduleResyncs=+%llu maxLateSinceReset=%.3fms ring=%zu",
                   sdp_.sessionName.c_str(), (unsigned long long)TxTimingDiagnostics::delta(sent, lastSent),
                   (unsigned long long)TxTimingDiagnostics::delta(errors, lastErrors),
                   (unsigned long long)TxTimingDiagnostics::delta(underruns, lastUnderruns),
                   (unsigned long long)TxTimingDiagnostics::delta(latePackets, lastLatePackets),
                   (unsigned long long)TxTimingDiagnostics::delta(resyncs, lastResyncs), double(maxLatenessNs) / 1000000.0, fill);
        lastSent = sent; lastErrors = errors; lastUnderruns = underruns;
        lastLatePackets = latePackets;
        lastResyncs = resyncs;
    }
}

bool RTPTransmitter::refreshInterfaceAddress() {
    const auto current = txInterface(networkInterface_);
    if (current.second.empty() || current.second == interfaceIP_) {
        return false;
    }
    // A lost or changed address (loose cable, DHCP versus link-local) leaves
    // the old socket failing with EADDRNOTAVAIL until it is reopened.
    rtpSocket_.close();
    if (!rtpSocket_.openTransmitter(sdp_.connectionAddress.c_str(), sdp_.port, current.second.c_str())) {
        AES67_LOGF("RTPTransmitter: %s now has %s, reopening socket failed; retrying (stream=%s)",
                   networkInterface_.c_str(), current.second.c_str(), sdp_.sessionName.c_str());
        return false;
    }
    AES67_LOGF("RTPTransmitter: %s address changed %s -> %s, socket reopened (stream=%s)",
               networkInterface_.c_str(), interfaceIP_.c_str(), current.second.c_str(),
               sdp_.sessionName.c_str());
    interfaceIP_ = current.second;
    if (!syntheticLocalTest_) {
        sdp_.originAddress = interfaceIP_;
        sdp_.sourceAddress = interfaceIP_;
    }
    return true;
}

void RTPTransmitter::sendPacket(const uint8_t* payload, size_t payloadSize, uint32_t timestamp) {
    if (!rtpSocket_.isOpen() || !payload || payloadSize == 0) {
        return;
    }

    // Build RTP packet
    RTP::RTPPacket packet;
    packet.header.version = 2;
    packet.header.padding = 0;
    packet.header.extension = 0;
    packet.header.cc = 0;
    packet.header.marker = 0;
    packet.header.payloadType = sdp_.payloadType;
    packet.header.sequenceNumber = sequenceNumber_++;
    packet.header.timestamp = timestamp;
    packet.header.ssrc = ssrc_;
    packet.payload = const_cast<uint8_t*>(payload);
    packet.payloadSize = payloadSize;

    // Send packet
    ssize_t bytesSent = rtpSocket_.send(packet);

    if (bytesSent < 0) {
        // Send failed — count via malformedPackets (repurposed as send error counter for TX)
        uint64_t count = stats_.malformedPackets.fetch_add(1, std::memory_order_relaxed) + 1;
        // Log first occurrence and then every 100th to avoid flooding
        if (count == 1 || count % 100 == 0) {
            AES67_LOGF("RTPTransmitter::sendPacket: send failed #%llu (seq=%u, errno=%d: %s) stream=%s",
                       (unsigned long long)count, sequenceNumber_ - 1,
                       errno, strerror(errno), sdp_.sessionName.c_str());
        }
    } else { stats_.bytesSent.fetch_add(uint64_t(bytesSent)); packetsSent_.fetch_add(1, std::memory_order_relaxed); }
}

} // namespace AES67
