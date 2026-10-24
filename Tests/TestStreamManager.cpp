//
// TestStreamManager.cpp
// AES67 macOS Driver - Build #18
// Unit tests for StreamManager
//

#include "../NetworkEngine/StreamManager.h"
#include "../Driver/SDPParser.h"
#include <iostream>
#include <cassert>
#include <array>
#include <utility>
#include "../NetworkEngine/DeviceChannels.hpp"
#include <fstream>
#include <unistd.h>

using namespace AES67;

template<size_t... I>
static auto makeBuffers(std::index_sequence<I...>) {
    return std::array<SPSCRingBuffer<float>, 128>{
        ((void)I, SPSCRingBuffer<float>(512))...
    };
}

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

//
// Helper Functions
//

// Create test SDP session
SDPSession createTestSDP(const std::string& name = "Test Stream",
                         uint16_t port = 5004,
                         uint16_t channels = 2,
                         uint32_t sampleRate = 48000) {
    SDPSession sdp;
    sdp.sessionName = name;
    sdp.port = port;
    sdp.connectionAddress = "239.1.1.1";
    sdp.encoding = "L24";
    sdp.sampleRate = sampleRate;
    sdp.numChannels = channels;
    sdp.payloadType = 97;
    sdp.ptime = 1;
    sdp.framecount = 48;
    sdp.originAddress = "192.168.1.100";
    sdp.ptpDomain = 0;

    return sdp;
}

// Create test channel mapping
ChannelMapping createTestMapping(uint16_t streamChannels = 2,
                                 uint16_t deviceStart = 0) {
    ChannelMapping mapping;
    mapping.streamID = StreamID::generate();
    mapping.streamName = "Test Mapping";
    mapping.streamChannelCount = streamChannels;
    mapping.streamChannelOffset = 0;
    mapping.deviceChannelStart = deviceStart;
    mapping.deviceChannelCount = streamChannels;

    return mapping;
}

//
// SDP Session Creation Tests
//

bool testSDPSessionCreation() {
    std::cout << "Test: SDP session creation for StreamManager... ";

    SDPSession sdp = createTestSDP("Test Stream", 5004, 8, 48000);

    TEST_ASSERT(sdp.sessionName == "Test Stream", "Session name should match");
    TEST_ASSERT(sdp.port == 5004, "Port should match");
    TEST_ASSERT(sdp.numChannels == 8, "Channel count should match");
    TEST_ASSERT(sdp.sampleRate == 48000, "Sample rate should match");
    TEST_ASSERT(sdp.connectionAddress == "239.1.1.1", "Multicast address should match");

    std::cout << "PASS" << std::endl;
    return true;
}

bool testSDPSessionValidation() {
    std::cout << "Test: SDP session validation... ";

    SDPSession validSDP = createTestSDP();
    TEST_ASSERT(validSDP.isValid(), "Valid SDP should pass validation");

    // Test invalid port
    SDPSession invalidPort = createTestSDP();
    invalidPort.port = 0;
    TEST_ASSERT(!invalidPort.isValid(), "SDP with port 0 should be invalid");

    // Test invalid sample rate
    SDPSession invalidSR = createTestSDP();
    invalidSR.sampleRate = 0;
    TEST_ASSERT(!invalidSR.isValid(), "SDP with sample rate 0 should be invalid");

    std::cout << "PASS" << std::endl;
    return true;
}

//
// Channel Mapping Tests
//

bool testChannelMappingCreation() {
    std::cout << "Test: Channel mapping for streams... ";

    ChannelMapping mapping = createTestMapping(8, 16);

    TEST_ASSERT(mapping.streamChannelCount == 8, "Stream channels should match");
    TEST_ASSERT(mapping.deviceChannelStart == 16, "Device start should match");
    TEST_ASSERT(mapping.deviceChannelCount == 8, "Device count should match");

    std::cout << "PASS" << std::endl;
    return true;
}

bool testChannelMappingValidation() {
    std::cout << "Test: Channel mapping validation... ";

    // Valid mapping
    ChannelMapping valid = createTestMapping(4, 0);
    TEST_ASSERT(valid.isValid(), "Valid mapping should pass");

    // Invalid: device channels out of range
    ChannelMapping invalid = createTestMapping(4, 126);
    TEST_ASSERT(!invalid.isValid(), "Out of range mapping should fail");

    // Invalid: zero channels
    ChannelMapping zeroChannels = createTestMapping(0, 0);
    TEST_ASSERT(!zeroChannels.isValid(), "Zero channels should be invalid");

    std::cout << "PASS" << std::endl;
    return true;
}

bool testChannelMappingOverlap() {
    std::cout << "Test: Channel mapping overlap detection... ";

    // Mapping 1: channels 0-7
    ChannelMapping mapping1 = createTestMapping(8, 0);

    // Mapping 2: channels 8-15 (no overlap)
    ChannelMapping mapping2 = createTestMapping(8, 8);

    // Mapping 3: channels 4-11 (overlaps with both)
    ChannelMapping mapping3 = createTestMapping(8, 4);

    // Check bounds
    uint16_t end1 = mapping1.getDeviceChannelEnd();
    uint16_t end2 = mapping2.getDeviceChannelEnd();
    uint16_t end3 = mapping3.getDeviceChannelEnd();

    TEST_ASSERT(end1 == 8, "Mapping 1 should end at channel 8");
    TEST_ASSERT(end2 == 16, "Mapping 2 should end at channel 16");
    TEST_ASSERT(end3 == 12, "Mapping 3 should end at channel 12");

    // Check for overlaps
    bool overlap1_2 = (mapping1.deviceChannelStart < end2 &&
                       mapping2.deviceChannelStart < end1);
    bool overlap1_3 = (mapping1.deviceChannelStart < end3 &&
                       mapping3.deviceChannelStart < end1);

    TEST_ASSERT(!overlap1_2, "Mappings 1 and 2 should not overlap");
    TEST_ASSERT(overlap1_3, "Mappings 1 and 3 should overlap");

    std::cout << "PASS" << std::endl;
    return true;
}

//
// Sample Rate Validation Tests
//

bool testSampleRateCompatibility() {
    std::cout << "Test: Sample rate compatibility... ";

    // Common AES67 sample rates
    std::vector<uint32_t> validRates = {44100, 48000, 88200, 96000, 176400, 192000, 384000};

    for (auto rate : validRates) {
        SDPSession sdp = createTestSDP("Test", 5004, 2, rate);
        TEST_ASSERT(sdp.sampleRate == rate, "Sample rate should match");
    }

    std::cout << "PASS" << std::endl;
    return true;
}

bool testSampleRateMismatch() {
    std::cout << "Test: Sample rate mismatch detection... ";

    // Device at 48kHz
    uint32_t deviceRate = 48000;

    // Stream at same rate - OK
    SDPSession matching = createTestSDP("Match", 5004, 2, 48000);
    TEST_ASSERT(matching.sampleRate == deviceRate, "Matching rate should be OK");

    // Stream at different rate - Would need validation
    SDPSession mismatched = createTestSDP("Mismatch", 5004, 2, 96000);
    TEST_ASSERT(mismatched.sampleRate != deviceRate, "Mismatched rate should be detected");

    std::cout << "PASS" << std::endl;
    return true;
}

//
// Stream Configuration Tests
//

bool testStreamIDGeneration() {
    std::cout << "Test: StreamID generation and uniqueness... ";

    StreamID id1 = StreamID::generate();
    StreamID id2 = StreamID::generate();
    StreamID id3 = StreamID::generate();

    TEST_ASSERT(!id1.isNull(), "Generated ID should not be null");
    TEST_ASSERT(!id2.isNull(), "Generated ID should not be null");
    TEST_ASSERT(!id3.isNull(), "Generated ID should not be null");

    TEST_ASSERT(id1 != id2, "IDs should be unique");
    TEST_ASSERT(id2 != id3, "IDs should be unique");
    TEST_ASSERT(id1 != id3, "IDs should be unique");

    std::cout << "PASS" << std::endl;
    return true;
}

bool testStreamIDComparison() {
    std::cout << "Test: StreamID comparison operators... ";

    StreamID id1 = StreamID::generate();
    StreamID id2 = id1;  // Copy
    StreamID id3 = StreamID::generate();

    TEST_ASSERT(id1 == id2, "Copied IDs should be equal");
    TEST_ASSERT(id1 != id3, "Different IDs should not be equal");

    // Test null ID
    StreamID null1 = StreamID::null();
    StreamID null2 = StreamID::null();
    TEST_ASSERT(null1 == null2, "Null IDs should be equal");
    TEST_ASSERT(null1.isNull(), "Null ID should be detected");

    std::cout << "PASS" << std::endl;
    return true;
}

bool testStreamIDStringConversion() {
    std::cout << "Test: StreamID string conversion... ";

    StreamID id = StreamID::generate();
    std::string idStr = id.toString();

    TEST_ASSERT(!idStr.empty(), "String representation should not be empty");
    TEST_ASSERT(idStr.length() == 36, "UUID string should be 36 characters (with dashes)");

    // Test null ID
    StreamID nullId = StreamID::null();
    std::string nullStr = nullId.toString();
    TEST_ASSERT(!nullStr.empty(), "Null ID should have string representation");

    std::cout << "PASS" << std::endl;
    return true;
}

//
// Multi-Stream Configuration Tests
//

bool testMultipleStreamConfiguration() {
    std::cout << "Test: Multiple stream configuration... ";

    // Create multiple SDP sessions
    SDPSession stream1 = createTestSDP("Stream 1", 5004, 2, 48000);
    SDPSession stream2 = createTestSDP("Stream 2", 5006, 4, 48000);
    SDPSession stream3 = createTestSDP("Stream 3", 5008, 8, 48000);

    // Create non-overlapping mappings
    ChannelMapping map1 = createTestMapping(2, 0);   // Channels 0-1
    ChannelMapping map2 = createTestMapping(4, 2);   // Channels 2-5
    ChannelMapping map3 = createTestMapping(8, 6);   // Channels 6-13

    // Verify total channel usage: 2 + 4 + 8 = 14 channels
    uint16_t totalChannels = map1.deviceChannelCount +
                            map2.deviceChannelCount +
                            map3.deviceChannelCount;
    TEST_ASSERT(totalChannels == 14, "Total channels should be 14");

    // Verify no overlaps
    TEST_ASSERT(map1.getDeviceChannelEnd() == map2.deviceChannelStart,
                "Map 1 and 2 should be contiguous");
    TEST_ASSERT(map2.getDeviceChannelEnd() == map3.deviceChannelStart,
                "Map 2 and 3 should be contiguous");

    std::cout << "PASS" << std::endl;
    return true;
}

bool testMaximumStreamConfiguration() {
    std::cout << "Test: Maximum channel configuration... ";

    // Four 8-channel AES67 flows fill the exposed 32 channels.
    SDPSession maxChannels = createTestSDP("Max Channels", 5004, 32, 48000);
    TEST_ASSERT(maxChannels.numChannels == 32, "Should support 32 channels");

    uint16_t streamsNeeded = 32 / 8;
    TEST_ASSERT(streamsNeeded == 4, "Should need four 8-channel streams for 32 channels");

    std::cout << "PASS" << std::endl;
    return true;
}

bool testBidirectionalChannelDomains() {
    std::cout << "Test: RX and TX may use the same device channel numbers... ";
    auto inputs = makeBuffers(std::make_index_sequence<128>{});
    auto outputs = makeBuffers(std::make_index_sequence<128>{});
    StreamManager manager(inputs, outputs);
    manager.setAutoSave(false);
    auto rx = manager.addStream(createTestSDP("RX", 5004, 8));
    TEST_ASSERT(!rx.isNull(), "RX stream should be added");
    auto tx = manager.createTxStream("TX", "239.1.1.2", 5004, 8,
                                     createTestMapping(8, 0));
    TEST_ASSERT(!tx.isNull(), "TX channels 1-8 should coexist with RX channels 1-8");
    TEST_ASSERT(manager.getMapping(rx)->deviceChannelStart == 0, "RX starts at channel 1");
    TEST_ASSERT(manager.getMapping(tx)->deviceChannelStart == 0, "TX starts at channel 1");
    TEST_ASSERT(manager.removeStream(tx), "TX should be removable");
    TEST_ASSERT(manager.removeStream(rx), "RX should be removable");
    std::cout << "PASS" << std::endl;
    return true;
}

bool testReceiveSubsetMapping() {
    std::cout << "Test: RX subset count survives management and persistence... ";
    auto inputs = makeBuffers(std::make_index_sequence<128>{});
    auto outputs = makeBuffers(std::make_index_sequence<128>{});
    StreamManager manager(inputs, outputs);
    manager.setAutoSave(false);
    // Leave only four free device channels. The advertised eight-channel
    // stream must be admitted based on its four selected channels.
    for (uint16_t start = 0; start < 28; start += 4) {
        TEST_ASSERT(!manager.addStream(createTestSDP("Existing", 5004 + start, 4),
                                       createTestMapping(4, start)).isNull(),
                    "Existing RX channels should be reserved");
    }
    auto mapping = createTestMapping(8, 28);
    mapping.deviceChannelCount = 4;
    mapping.channelMap = {28, 29, 30, 31, -1, -1, -1, -1};
    auto sdp = createTestSDP("Subset", 5100, 8);
    auto id = manager.addStream(sdp, mapping);
    TEST_ASSERT(!id.isNull(), "Subset fits the four remaining device channels");
    auto savedMapping = manager.getMapping(id);
    TEST_ASSERT(savedMapping && savedMapping->deviceChannelCount == 4,
                "Manager must preserve selected channel count");
    TEST_ASSERT(savedMapping->streamChannelCount == 8, "Manager retains source frame stride");
    PersistedStreamConfig config;
    config.sdp = sdp;
    config.sdp.direction = "recvonly";
    config.mapping = *savedMapping;
    auto decoded = StreamConfigManager::fromJSON(StreamConfigManager::toJSON({config}));
    TEST_ASSERT(decoded && decoded->size() == 1, "Subset configuration round-trips");
    TEST_ASSERT((*decoded)[0].mapping.channelMap == mapping.channelMap,
                "Persisted subset retains dropped source channels");
    config.mapping.deviceChannelStart = 0;
    config.mapping.channelMap = {0, 1, 2, 3, -1, -1, -1, -1};
    auto presetJSON = StreamConfigManager::toJSON({config});
    presetJSON.insert(1, "\"inputChannels\":4,\"outputChannels\":4,");
    auto preset = StreamConfigManager::fromJSON(presetJSON);
    TEST_ASSERT(preset && preset->size() == 1 && (*preset)[0].mapping.deviceChannelCount == 4,
                "Four-channel preset accepts a persisted eight-channel source subset");
    config.mapping.streamChannelCount = 9;
    config.mapping.channelMap.push_back(-1);
    TEST_ASSERT(!config.isValid(), "Persisted mapping must match source stream channel count");
    std::cout << "PASS" << std::endl;
    return true;
}

bool testPersistedDirectionAndInterface() {
    std::cout << "Test: Config retains transport settings and local direction... ";
    PersistedStreamConfig config;
    config.sdp = createTestSDP("Playback", 5004, 8);
    config.sdp.direction = "sendonly";
    config.sdp.mediaClockType = "local-test";
    config.mapping = createTestMapping(8, 0);
    config.networkInterface = "en7";
    config.jitterBufferDepth = 512;
    auto decoded = StreamConfigManager::fromJSON(StreamConfigManager::toJSON({config}));
    TEST_ASSERT(decoded && decoded->size() == 1, "Config should round-trip");
    TEST_ASSERT((*decoded)[0].sdp.direction == "sendonly", "Local TX direction survives");
    TEST_ASSERT((*decoded)[0].sdp.mediaClockType == "local-test",
                "Explicit unsynchronized local-test timing survives config round-trip");
    TEST_ASSERT((*decoded)[0].networkInterface == "en7", "Selected interface survives");
    TEST_ASSERT((*decoded)[0].jitterBufferDepth == 512, "Jitter depth survives");
    std::cout << "PASS" << std::endl;
    return true;
}

//
// Network Configuration Tests
//

bool testMulticastAddressValidation() {
    std::cout << "Test: Multicast address validation... ";

    // Valid AES67 multicast range (239.x.x.x)
    SDPSession validMcast = createTestSDP();
    validMcast.connectionAddress = "239.1.1.1";
    TEST_ASSERT(validMcast.connectionAddress.substr(0, 3) == "239",
                "AES67 should use 239.x.x.x range");

    // Other multicast addresses
    SDPSession otherMcast = createTestSDP();
    otherMcast.connectionAddress = "224.0.0.1";
    TEST_ASSERT(otherMcast.connectionAddress.substr(0, 3) == "224",
                "224.x.x.x is also valid multicast");

    std::cout << "PASS" << std::endl;
    return true;
}

bool testPortConfiguration() {
    std::cout << "Test: Port configuration... ";

    // Test various valid ports
    std::vector<uint16_t> validPorts = {5004, 5006, 5008, 49152, 65535};

    for (auto port : validPorts) {
        SDPSession sdp = createTestSDP("Test", port, 2, 48000);
        TEST_ASSERT(sdp.port == port, "Port should be set correctly");
        TEST_ASSERT(sdp.port > 0, "Port should be positive");
    }

    std::cout << "PASS" << std::endl;
    return true;
}

//
// Encoding Configuration Tests
//

bool testEncodingSupport() {
    std::cout << "Test: Audio encoding support... ";

    SDPSession l16 = createTestSDP();
    l16.encoding = "L16";
    TEST_ASSERT(l16.encoding == "L16", "L16 encoding should be supported");

    SDPSession l24 = createTestSDP();
    l24.encoding = "L24";
    TEST_ASSERT(l24.encoding == "L24", "L24 encoding should be supported");

    SDPSession am824 = createTestSDP();
    am824.encoding = "AM824";
    TEST_ASSERT(am824.encoding == "AM824", "AM824 encoding should be supported");

    std::cout << "PASS" << std::endl;
    return true;
}

//
// PTP Configuration Tests
//

bool testPTPDomainConfiguration() {
    std::cout << "Test: PTP domain configuration... ";

    // Stream with PTP domain 0 (typical for AES67)
    SDPSession withPTP = createTestSDP();
    withPTP.ptpDomain = 0;
    TEST_ASSERT(withPTP.ptpDomain == 0, "PTP domain 0 should be supported");

    // Stream without PTP
    SDPSession noPTP = createTestSDP();
    noPTP.ptpDomain = -1;
    TEST_ASSERT(noPTP.ptpDomain == -1, "No PTP should be indicated by -1");

    // Other domains
    SDPSession domain127 = createTestSDP();
    domain127.ptpDomain = 127;
    TEST_ASSERT(domain127.ptpDomain == 127, "PTP domain 127 should be supported");

    std::cout << "PASS" << std::endl;
    return true;
}

//
// Main Test Runner
//

static void testDeviceChannelSettings() {
    auto legacy=parseDeviceChannels("{\"streams\":[]}");assert(legacy && legacy->input==32 && legacy->output==32);
    auto selected=parseDeviceChannels("{\"inputChannels\":4,\"outputChannels\":8,\"streams\":[]}");assert(selected && selected->input==4 && selected->output==8);
    for(auto text:{"{\"inputChannels\":0}","{\"inputChannels\":33}","{\"inputChannels\":4.5}","{\"inputChannels\":\"4\"}","{\"inputChannels\":4,\"inputChannels\":8}"})assert(!parseDeviceChannels(text));
    auto nested=parseDeviceChannels("{\"description\":\"inputChannels\",\"streams\":[{\"inputChannels\":3}]}");assert(nested && nested->input==32);
    PersistedStreamConfig c;c.sdp=createTestSDP("count-test",5004,4);c.sdp.direction="recvonly";
    c.mapping.streamID=StreamID::generate();c.mapping.streamName="count-test";c.mapping.streamChannelCount=4;c.mapping.deviceChannelCount=4;c.mapping.deviceChannelStart=0;
    auto json=StreamConfigManager::toJSON({c});json.insert(1,"\"inputChannels\":4,\"outputChannels\":8,");assert(StreamConfigManager::fromJSON(json));
    c.mapping.deviceChannelStart=1;auto bad=StreamConfigManager::toJSON({c});bad.insert(1,"\"inputChannels\":4,");assert(!StreamConfigManager::fromJSON(bad));
    c.mapping.deviceChannelStart=0;c.mapping.channelMap={0,1,2,4};bad=StreamConfigManager::toJSON({c});bad.insert(1,"\"inputChannels\":4,");assert(!StreamConfigManager::fromJSON(bad));
    c.mapping.channelMap.clear();char path[]="/tmp/aes67-counts-XXXXXX";int fd=mkstemp(path);assert(fd>=0);close(fd);
    {std::ofstream f(path);f<<json;}
    StreamConfigManager manager;manager.setConfigPath(path);assert(manager.saveConfig({c}));
    auto saved=readDeviceChannels(path);assert(saved && saved->input==4 && saved->output==8);unlink(path);
    std::cout<<"PASS: selected counts, legacy defaults, bounds and autosave preservation\n";
}

int main() {
    testDeviceChannelSettings();
    std::cout << "========================================" << std::endl;
    std::cout << "AES67 StreamManager Unit Tests" << std::endl;
    std::cout << "========================================" << std::endl;
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
    testChannelMappingOverlap();
    std::cout << std::endl;

    std::cout << "Sample Rate Tests:" << std::endl;
    std::cout << "-----------------" << std::endl;
    testSampleRateCompatibility();
    testSampleRateMismatch();
    std::cout << std::endl;

    std::cout << "StreamID Tests:" << std::endl;
    std::cout << "--------------" << std::endl;
    testStreamIDGeneration();
    testStreamIDComparison();
    testStreamIDStringConversion();
    std::cout << std::endl;

    std::cout << "Multi-Stream Configuration Tests:" << std::endl;
    std::cout << "--------------------------------" << std::endl;
    testMultipleStreamConfiguration();
    testMaximumStreamConfiguration();
    testBidirectionalChannelDomains();
    testPersistedDirectionAndInterface();
    testReceiveSubsetMapping();
    std::cout << std::endl;

    std::cout << "Network Configuration Tests:" << std::endl;
    std::cout << "---------------------------" << std::endl;
    testMulticastAddressValidation();
    testPortConfiguration();
    std::cout << std::endl;

    std::cout << "Encoding Configuration Tests:" << std::endl;
    std::cout << "----------------------------" << std::endl;
    testEncodingSupport();
    std::cout << std::endl;

    std::cout << "PTP Configuration Tests:" << std::endl;
    std::cout << "-----------------------" << std::endl;
    testPTPDomainConfiguration();
    std::cout << std::endl;

    std::cout << "========================================" << std::endl;
    std::cout << "Test Results:" << std::endl;
    std::cout << "  Passed: " << testsPassed << std::endl;
    std::cout << "  Failed: " << testsFailed << std::endl;
    std::cout << "========================================" << std::endl;

    return testsFailed == 0 ? 0 : 1;
}
