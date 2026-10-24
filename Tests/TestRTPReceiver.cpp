//
// TestRTPReceiver.cpp
// AES67 macOS Driver - Build #17
// Unit tests for RTP packet receiver
//

#include "../NetworkEngine/RTP/SimpleRTP.h"
#include "../NetworkEngine/RTP/RTPReceiver.h"
#include "../Driver/SDPParser.h"
#include "../NetworkEngine/StreamChannelMapper.h"
#include <iostream>
#include <cassert>
#include <vector>
#include <cmath>
#include <unistd.h>
#include <mach/mach.h>
#include <mach/thread_policy.h>

using namespace AES67;
using namespace AES67::RTP;

// Test result counter
static int testsPassed = 0;
static int testsFailed = 0;

#define TEST_ASSERT(condition, message) \
    if (!(condition)) { \
        std::cerr << "FAIL: " << message << std::endl; \
        testsFailed++; \
        return false; \
    } else { \
        testsPassed++; \
    }

template<size_t... I>
static auto makeBuffers(std::index_sequence<I...>) {
    return RTPReceiver::DeviceChannelBuffers{((void)I, SPSCRingBuffer<float>(2048))...};
}

bool testReceiveSubset(const std::string& encoding, const std::vector<int>& channels) {
    std::cout << "Test: " << encoding << " eight-channel RTP to four device channels... ";
    auto buffers = makeBuffers(std::make_index_sequence<128>{});
    SDPSession sdp;
    sdp.sessionName = "RX subset";
    sdp.connectionAddress = "239.255.42.67";
    sdp.port = static_cast<uint16_t>(23000 + getpid() % 10000);
    sdp.numChannels = 8;
    sdp.sampleRate = 48000;
    sdp.framecount = 48;
    sdp.ptime = 1;
    sdp.encoding = encoding;
    sdp.payloadType = encoding == "L16" ? PT_AES67_L16 : PT_AES67_L24;
    ChannelMapping mapping;
    mapping.streamID = StreamID::generate();
    mapping.streamChannelCount = 8;
    mapping.deviceChannelCount = 4;
    mapping.channelMap = channels;
    RTPReceiver receiver(sdp, mapping, buffers, 0, "127.0.0.1");
    TEST_ASSERT(receiver.start(), "Subset receiver should start");
    RTPSocket sender;
    TEST_ASSERT(sender.openTransmitter(sdp.connectionAddress.c_str(), sdp.port, "127.0.0.1"),
                "Loopback sender should open");

    std::vector<float> samples(48 * 8);
    for (size_t frame = 0; frame < 48; ++frame) {
        for (size_t ch = 0; ch < 8; ++ch) samples[frame * 8 + ch] = (ch + 1) / 16.0f;
    }
    std::vector<uint8_t> payload(samples.size() * (encoding == "L16" ? 2 : 3));
    if (encoding == "L16") L16Codec::encode(samples.data(), samples.size(), payload.data());
    else L24Codec::encode(samples.data(), samples.size(), payload.data());
    RTPPacket packet;
    packet.header.payloadType = sdp.payloadType;
    packet.payload = payload.data();
    packet.payloadSize = payload.size();
    for (uint16_t seq = 0; seq < 12; ++seq) {
        packet.header.sequenceNumber = seq;
        packet.header.timestamp = seq * 48;
        TEST_ASSERT(sender.send(packet) > 0, "RTP test packet should send");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (buffers[3].available() < 48 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    receiver.stop();
    for (size_t streamCh = 0; streamCh < channels.size(); ++streamCh) {
        if (channels[streamCh] < 0) continue;
        float received[48];
        TEST_ASSERT(buffers[channels[streamCh]].read(received, 48) == 48,
                    "Each selected device channel receives a full packet");
        for (float sample : received) {
            TEST_ASSERT(std::abs(sample - (streamCh + 1) / 16.0f) < 0.0001f,
                        "Selected samples retain their source channel and frame stride");
        }
    }
    for (size_t ch = 4; ch < buffers.size(); ++ch) {
        TEST_ASSERT(buffers[ch].available() == 0, "Dropped stream channels do not write device buffers");
    }
    std::cout << "PASS" << std::endl;
    return true;
}

// Two senders on one group (field log 21.10.2026: a Mac hearing its own test
// sender) must not make the receiver resync on every packet.
bool testLiveSecondSender() {
    std::cout << "Test: live RX stays locked to one sender while a second shares the group... ";
    auto buffers = makeBuffers(std::make_index_sequence<128>{});
    SDPSession sdp;
    sdp.sessionName = "RX lock";
    sdp.connectionAddress = "239.255.42.69";
    sdp.port = static_cast<uint16_t>(43000 + getpid() % 10000);
    sdp.numChannels = 2; sdp.sampleRate = 48000; sdp.framecount = 48; sdp.ptime = 1;
    sdp.encoding = "L24"; sdp.payloadType = PT_AES67_L24;
    ChannelMapping mapping;
    mapping.streamID = StreamID::generate();
    mapping.streamChannelCount = 2; mapping.deviceChannelCount = 2; mapping.channelMap = {0, 1};
    RTPReceiver receiver(sdp, mapping, buffers, 0, "127.0.0.1");
    TEST_ASSERT(receiver.start(), "Receiver should start");
    RTPSocket sender;
    TEST_ASSERT(sender.openTransmitter(sdp.connectionAddress.c_str(), sdp.port, "127.0.0.1"), "Loopback sender should open");
    std::atomic<bool> reading{true};
    std::thread reader([&] {
        float block[2048];
        while (reading) { buffers[0].read(block, 2048); buffers[1].read(block, 2048); std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
    });
    std::vector<float> samples(48 * 2, 0.25f);
    std::vector<uint8_t> payload(samples.size() * 3);
    L24Codec::encode(samples.data(), samples.size(), payload.data());
    RTPPacket packet;
    packet.header.payloadType = sdp.payloadType; packet.payload = payload.data(); packet.payloadSize = payload.size();
    for (int i = 0; i < 1500; ++i) {
        for (auto [ssrc, seq] : {std::pair<uint32_t, uint16_t>{0x1111, uint16_t(100 + i)}, {0x2222, uint16_t(30000 + i)}}) {
            packet.header.ssrc = ssrc; packet.header.sequenceNumber = seq; packet.header.timestamp = uint32_t(seq) * 48;
            sender.send(packet);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const auto d = receiver.getDiagnostics();
    receiver.stop(); reading = false; reader.join();
    TEST_ASSERT(d.resyncs == 0, "Second sender caused resyncs");
    TEST_ASSERT(d.foreign >= 1400, "Second sender's packets were not ignored");
    TEST_ASSERT(d.played >= 1400, "Locked sender kept playing");
    std::cout << "PASS (played " << d.played << ", ignored " << d.foreign << ", resyncs " << d.resyncs << ")" << std::endl;
    return true;
}

// The receive/consume/transmit threads must really run in the time-constraint
// class; the old code always reported failure on Apple Silicon.
bool testRealTimePriority() {
    std::cout << "Test: network threads get the real-time time-constraint policy... ";
    bool configured = false, applied = false;
    std::thread worker([&] {
        configured = AudioThreadPriority::configureForRealTime();
        thread_time_constraint_policy_data_t policy{};
        mach_msg_type_number_t count = THREAD_TIME_CONSTRAINT_POLICY_COUNT;
        boolean_t isDefault = FALSE;   // input FALSE: read the thread's current values
        applied = thread_policy_get(pthread_mach_thread_np(pthread_self()), THREAD_TIME_CONSTRAINT_POLICY,
                                    reinterpret_cast<thread_policy_t>(&policy), &count, &isDefault) == KERN_SUCCESS &&
                  !isDefault && policy.period > 0 && policy.computation <= policy.constraint;
    });
    worker.join();
    TEST_ASSERT(configured, "configureForRealTime reports success");
    TEST_ASSERT(applied, "Thread runs with a non-default time-constraint policy");
    std::cout << "PASS" << std::endl;
    return true;
}

// Live receive path with real threads: reordered and duplicated packets plus a
// sender restart (new SSRC, sequence jump) must keep audio flowing.
bool testLiveReorderAndRestart() {
    std::cout << "Test: live RX keeps playing through reorder, duplicates and sender restart... ";
    auto buffers = makeBuffers(std::make_index_sequence<128>{});
    SDPSession sdp;
    sdp.sessionName = "RX resync";
    sdp.connectionAddress = "239.255.42.68";
    sdp.port = static_cast<uint16_t>(33000 + getpid() % 10000);
    sdp.numChannels = 2;
    sdp.sampleRate = 48000;
    sdp.framecount = 48;
    sdp.ptime = 1;
    sdp.encoding = "L24";
    sdp.payloadType = PT_AES67_L24;
    ChannelMapping mapping;
    mapping.streamID = StreamID::generate();
    mapping.streamChannelCount = 2;
    mapping.deviceChannelCount = 2;
    mapping.channelMap = {0, 1};
    RTPReceiver receiver(sdp, mapping, buffers, 0, "127.0.0.1");
    TEST_ASSERT(receiver.start(), "Receiver should start");
    RTPSocket sender;
    TEST_ASSERT(sender.openTransmitter(sdp.connectionAddress.c_str(), sdp.port, "127.0.0.1"),
                "Loopback sender should open");

    // Core Audio stand-in: drain both rings every 5 ms
    std::atomic<bool> reading{true};
    std::atomic<uint64_t> framesWithSignal{0};
    std::thread reader([&] {
        float block[2048];
        while (reading) {
            const size_t n = buffers[0].read(block, 2048);
            buffers[1].read(block, 2048);
            for (size_t i = 0; i < n; ++i) framesWithSignal += block[i] != 0.0f;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    });

    std::vector<float> samples(48 * 2, 0.25f);
    std::vector<uint8_t> payload(samples.size() * 3);
    L24Codec::encode(samples.data(), samples.size(), payload.data());
    RTPPacket packet;
    packet.header.payloadType = sdp.payloadType;
    packet.payload = payload.data();
    packet.payloadSize = payload.size();
    auto sendSeq = [&](uint16_t seq, uint32_t ssrc) {
        packet.header.sequenceNumber = seq;
        packet.header.timestamp = static_cast<uint32_t>(seq) * 48;
        packet.header.ssrc = ssrc;
        return sender.send(packet) > 0;
    };
    // Every 50th pair arrives swapped, every 70th packet twice.
    auto sendRun = [&](uint16_t first, int count, uint32_t ssrc) {
        for (int i = 0; i < count; ++i) {
            const uint16_t seq = static_cast<uint16_t>(first + i);
            if (i % 50 == 10 && i + 1 < count) {
                sendSeq(static_cast<uint16_t>(seq + 1), ssrc);
                sendSeq(seq, ssrc);
                ++i;
            } else {
                sendSeq(seq, ssrc);
                if (i % 70 == 20) sendSeq(seq, ssrc);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    };

    sendRun(60000, 1500, 0x1111);                 // crosses the 16-bit wrap
    const auto before = receiver.getDiagnostics();
    sendRun(12345, 1000, 0x2222);                 // restarted sender
    const auto mid = receiver.getDiagnostics();
    sendRun(13345, 500, 0x2222);
    const auto after = receiver.getDiagnostics();

    const auto stopStart = std::chrono::steady_clock::now();
    receiver.stop();
    const auto stopMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - stopStart).count();
    reading = false;
    reader.join();

    TEST_ASSERT(before.played > 1000, "Audio plays before the restart");
    TEST_ASSERT(after.resyncs >= 1, "Sender restart triggers a resync");
    TEST_ASSERT(after.played - mid.played > 400, "Audio keeps playing long after the restart");
    TEST_ASSERT(after.played >= 2600, "Nearly all packets are played");
    TEST_ASSERT(after.ahead >= 0 && after.ahead <= 64, "Consumer never runs ahead of the sender");
    TEST_ASSERT(framesWithSignal > 2600 * 48 * 9 / 10, "Decoded audio reaches the device ring");
    TEST_ASSERT(stopMs < 500, "Stats thread does not delay stop()");
    std::cout << "PASS (played " << after.played << ", skipped " << after.skipped
              << ", waits " << after.waits << ", resyncs " << after.resyncs << ")" << std::endl;
    return true;
}

//
// Basic RTP Packet Tests
//

bool testRTPPacketStructure() {
    std::cout << "Test: RTP packet structure... ";

    RTPPacket packet;

    // Default values
    TEST_ASSERT(packet.header.version == 2, "RTP version should be 2");
    TEST_ASSERT(packet.header.padding == 0, "Padding should be disabled");
    TEST_ASSERT(packet.header.extension == 0, "Extension should be disabled");
    TEST_ASSERT(packet.header.cc == 0, "CSRC count should be 0");
    TEST_ASSERT(packet.header.marker == 0, "Marker should be 0");
    TEST_ASSERT(packet.header.payloadType == PT_AES67_L16, "Default payload should be L16");

    // Set values
    packet.header.sequenceNumber = 1000;
    packet.header.timestamp = 48000;
    packet.header.ssrc = 0xABCDEF12;

    TEST_ASSERT(packet.header.sequenceNumber == 1000, "Sequence number should be set");
    TEST_ASSERT(packet.header.timestamp == 48000, "Timestamp should be set");
    TEST_ASSERT(packet.header.ssrc == 0xABCDEF12, "SSRC should be set");

    std::cout << "PASS" << std::endl;
    return true;
}

bool testRTPHeaderSize() {
    std::cout << "Test: RTP header size... ";

    TEST_ASSERT(sizeof(RTPHeader) == 12, "RTP header must be exactly 12 bytes");

    std::cout << "PASS" << std::endl;
    return true;
}

bool testSequenceNumberHandling() {
    std::cout << "Test: Sequence number handling... ";

    uint16_t seq = 0;

    // Normal increment
    for (int i = 0; i < 100; ++i) {
        TEST_ASSERT(seq == i, "Sequence should increment normally");
        seq++;
    }

    // Wrap-around
    seq = 65534;
    seq++;
    TEST_ASSERT(seq == 65535, "Sequence at boundary");
    seq++;
    TEST_ASSERT(seq == 0, "Sequence should wrap to 0");
    seq++;
    TEST_ASSERT(seq == 1, "Sequence should continue after wrap");

    std::cout << "PASS" << std::endl;
    return true;
}

//
// Audio Codec Tests
//

bool testL16Encoding() {
    std::cout << "Test: L16 audio encoding/decoding... ";

    // Create test audio: 4 samples
    float audio[4] = {0.5f, -0.5f, 1.0f, -1.0f};

    // Encode to L16
    uint8_t encoded[8];  // 4 samples * 2 bytes
    L16Codec::encode(audio, 4, encoded);

    // Decode back
    float decoded[4];
    L16Codec::decode(encoded, 8, decoded);

    // Verify round-trip (allow small tolerance)
    for (int i = 0; i < 4; ++i) {
        float diff = std::abs(decoded[i] - audio[i]);
        TEST_ASSERT(diff < 0.01f, "L16 round-trip should preserve audio");
    }

    std::cout << "PASS" << std::endl;
    return true;
}

bool testL24Encoding() {
    std::cout << "Test: L24 audio encoding/decoding... ";

    // Create test audio: 4 samples
    float audio[4] = {0.5f, -0.5f, 1.0f, -1.0f};

    // Encode to L24
    uint8_t encoded[12];  // 4 samples * 3 bytes
    L24Codec::encode(audio, 4, encoded);

    // Decode back
    float decoded[4];
    L24Codec::decode(encoded, 12, decoded);

    // Verify round-trip (L24 has better precision)
    for (int i = 0; i < 4; ++i) {
        float diff = std::abs(decoded[i] - audio[i]);
        TEST_ASSERT(diff < 0.001f, "L24 round-trip should preserve audio with high precision");
    }

    std::cout << "PASS" << std::endl;
    return true;
}

//
// SDP Session Tests
//

bool testSDPSessionCreation() {
    std::cout << "Test: SDP session creation... ";

    SDPSession sdp;
    sdp.sessionName = "Test Stream";
    sdp.port = 5004;
    sdp.encoding = "L16";
    sdp.sampleRate = 48000;
    sdp.numChannels = 2;
    sdp.connectionAddress = "239.1.1.1";
    sdp.ttl = 32;
    sdp.payloadType = PT_AES67_L16;

    TEST_ASSERT(sdp.sessionName == "Test Stream", "Session name should be set");
    TEST_ASSERT(sdp.port == 5004, "Port should be set");
    TEST_ASSERT(sdp.encoding == "L16", "Encoding should be set");
    TEST_ASSERT(sdp.sampleRate == 48000, "Sample rate should be set");
    TEST_ASSERT(sdp.numChannels == 2, "Channel count should be set");

    std::cout << "PASS" << std::endl;
    return true;
}

bool testSDPSessionValidation() {
    std::cout << "Test: SDP session validation... ";

    // Create valid SDP
    SDPSession validSDP;
    validSDP.sessionName = "Valid Stream";
    validSDP.port = 5004;
    validSDP.encoding = "L24";
    validSDP.sampleRate = 48000;
    validSDP.numChannels = 8;
    validSDP.connectionAddress = "239.1.1.1";

    bool isValid = validSDP.isValid();
    TEST_ASSERT(isValid, "Valid SDP should pass validation");

    std::cout << "PASS" << std::endl;
    return true;
}

//
// Channel Mapping Tests
//

bool testChannelMappingCreation() {
    std::cout << "Test: Channel mapping creation... ";

    ChannelMapping mapping;
    mapping.streamID = StreamID::generate();
    mapping.streamName = "Test Stream";
    mapping.streamChannelCount = 8;
    mapping.streamChannelOffset = 0;
    mapping.deviceChannelStart = 16;
    mapping.deviceChannelCount = 8;

    TEST_ASSERT(mapping.streamChannelCount == 8, "Stream channel count should be set");
    TEST_ASSERT(mapping.deviceChannelStart == 16, "Device start channel should be set");
    TEST_ASSERT(mapping.deviceChannelCount == 8, "Device channel count should be set");

    std::cout << "PASS" << std::endl;
    return true;
}

bool testChannelMappingValidation() {
    std::cout << "Test: Channel mapping validation... ";

    ChannelMapping validMapping;
    validMapping.streamID = StreamID::generate();
    validMapping.streamName = "Valid Mapping";
    validMapping.streamChannelCount = 4;
    validMapping.deviceChannelStart = 0;
    validMapping.deviceChannelCount = 4;

    bool isValid = validMapping.isValid();
    TEST_ASSERT(isValid, "Valid mapping should pass validation");

    // Invalid mapping (device channels out of range)
    ChannelMapping invalidMapping;
    invalidMapping.streamID = StreamID::generate();
    invalidMapping.streamName = "Invalid Mapping";
    invalidMapping.streamChannelCount = 4;
    invalidMapping.deviceChannelStart = 126;  // Would go to channel 130 (out of range)
    invalidMapping.deviceChannelCount = 4;

    bool isInvalid = !invalidMapping.isValid();
    TEST_ASSERT(isInvalid, "Out-of-range mapping should fail validation");

    std::cout << "PASS" << std::endl;
    return true;
}

//
// Payload Size Tests
//

bool testPayloadSizeCalculations() {
    std::cout << "Test: Payload size calculations... ";

    // L16: 2 channels, 48 samples = 48 * 2 * 2 = 192 bytes
    size_t l16_2ch = 48 * 2 * 2;
    TEST_ASSERT(l16_2ch == 192, "L16 2ch 48 samples = 192 bytes");

    // L24: 2 channels, 48 samples = 48 * 2 * 3 = 288 bytes
    size_t l24_2ch = 48 * 2 * 3;
    TEST_ASSERT(l24_2ch == 288, "L24 2ch 48 samples = 288 bytes");

    // L16: 8 channels, 48 samples = 48 * 8 * 2 = 768 bytes
    size_t l16_8ch = 48 * 8 * 2;
    TEST_ASSERT(l16_8ch == 768, "L16 8ch 48 samples = 768 bytes");

    // L24: 8 channels, 48 samples = 48 * 8 * 3 = 1152 bytes
    size_t l24_8ch = 48 * 8 * 3;
    TEST_ASSERT(l24_8ch == 1152, "L24 8ch 48 samples = 1152 bytes");

    // Check against MTU (1500 bytes - 20 IP - 8 UDP - 12 RTP = 1460 bytes max payload)
    constexpr size_t MAX_PAYLOAD = 1460;
    TEST_ASSERT(l16_2ch < MAX_PAYLOAD, "L16 2ch payload should fit in MTU");
    TEST_ASSERT(l24_2ch < MAX_PAYLOAD, "L24 2ch payload should fit in MTU");
    TEST_ASSERT(l16_8ch < MAX_PAYLOAD, "L16 8ch payload should fit in MTU");
    TEST_ASSERT(l24_8ch < MAX_PAYLOAD, "L24 8ch payload should fit in MTU");

    std::cout << "PASS" << std::endl;
    return true;
}

//
// Timestamp Tests
//

bool testTimestampCalculation() {
    std::cout << "Test: RTP timestamp calculation... ";

    // At 48kHz, 1ms packet = 48 samples
    uint32_t samplesPerPacket = 48;
    uint32_t timestamp = 0;

    // First packet
    TEST_ASSERT(timestamp == 0, "Initial timestamp should be 0");

    // Advance by packet interval
    timestamp += samplesPerPacket;
    TEST_ASSERT(timestamp == 48, "Should advance by samples per packet");

    // Multiple packets
    for (int i = 0; i < 1000; ++i) {
        timestamp += samplesPerPacket;
    }
    TEST_ASSERT(timestamp == 48 * 1001, "Should accumulate correctly");

    // Test timestamp wrap (32-bit)
    uint32_t nearWrap = 0xFFFFFF00;
    nearWrap += 0x200;  // Will wrap
    TEST_ASSERT(nearWrap == 0x100, "Timestamp should wrap around");

    std::cout << "PASS" << std::endl;
    return true;
}

//
// Main Test Runner
//

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "AES67 RTP Receiver Unit Tests" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << std::endl;

    std::cout << "RTP Packet Tests:" << std::endl;
    std::cout << "----------------" << std::endl;
    testRTPPacketStructure();
    testRTPHeaderSize();
    testSequenceNumberHandling();
    std::cout << std::endl;

    std::cout << "Audio Codec Tests:" << std::endl;
    std::cout << "-----------------" << std::endl;
    testL16Encoding();
    testL24Encoding();
    std::cout << std::endl;

    std::cout << "SDP Session Tests:" << std::endl;
    std::cout << "-----------------" << std::endl;
    testSDPSessionCreation();
    testSDPSessionValidation();
    std::cout << std::endl;

    std::cout << "Channel Mapping Tests:" << std::endl;
    std::cout << "---------------------" << std::endl;
    testChannelMappingCreation();
    testChannelMappingValidation();
    testReceiveSubset("L16", {0, 1, 2, 3, -1, -1, -1, -1});
    testReceiveSubset("L24", {-1, 0, -1, 1, -1, 2, -1, 3});
    testLiveReorderAndRestart();
    testLiveSecondSender();
    testRealTimePriority();
    std::cout << std::endl;

    std::cout << "Payload Size Tests:" << std::endl;
    std::cout << "------------------" << std::endl;
    testPayloadSizeCalculations();
    std::cout << std::endl;

    std::cout << "Timestamp Tests:" << std::endl;
    std::cout << "---------------" << std::endl;
    testTimestampCalculation();
    std::cout << std::endl;

    std::cout << "========================================" << std::endl;
    std::cout << "Test Results:" << std::endl;
    std::cout << "  Passed: " << testsPassed << std::endl;
    std::cout << "  Failed: " << testsFailed << std::endl;
    std::cout << "========================================" << std::endl;

    return testsFailed == 0 ? 0 : 1;
}
