# Tests CMakeLists.txt
# AES67 macOS Driver - Build #4
# Test suite compilation

cmake_minimum_required(VERSION 3.15)

# Upstream tests use assert(). Keep checks active in Release builds too.
add_compile_options(-UNDEBUG)
if(APPLE)
    link_libraries("-framework CoreAudio" "-framework AudioToolbox" "-framework CoreFoundation")
endif()

# Test: SDP Parser
add_executable(TestSDPParser
    TestSDPParser.cpp
    ../Driver/SDPParser.cpp
    ../Shared/Types.cpp
)

target_include_directories(TestSDPParser PRIVATE
    ${CMAKE_SOURCE_DIR}
)

target_compile_features(TestSDPParser PRIVATE cxx_std_17)

# Test: Channel Mapper
add_executable(TestChannelMapper
    TestChannelMapper.cpp
    ../NetworkEngine/StreamChannelMapper.cpp
    ../Shared/Types.cpp
)

target_include_directories(TestChannelMapper PRIVATE
    ${CMAKE_SOURCE_DIR}
)

target_compile_features(TestChannelMapper PRIVATE cxx_std_17)

# Test: Ring Buffer
add_executable(TestRingBuffer
    TestRingBuffer.cpp
)

target_include_directories(TestRingBuffer PRIVATE
    ${CMAKE_SOURCE_DIR}
)

target_compile_features(TestRingBuffer PRIVATE cxx_std_17)

# Test: RTP Receiver
add_executable(TestRTPReceiver
    TestRTPReceiver.cpp
    ../NetworkEngine/RTP/RTPReceiver.cpp
    ../NetworkEngine/RTP/SimpleRTP.cpp
    ../NetworkEngine/RTP/LockFreeCircularJitterBuffer.cpp
    ../NetworkEngine/RTP/LockFreePacketPool.cpp
    ../Driver/SDPParser.cpp
    ../Driver/AudioThreadPriority.cpp
    ../NetworkEngine/StreamChannelMapper.cpp
    ../NetworkEngine/NetworkInterfaceDetection.cpp
    ../Shared/Types.cpp
)

target_include_directories(TestRTPReceiver PRIVATE
    ${CMAKE_SOURCE_DIR}
)

target_compile_features(TestRTPReceiver PRIVATE cxx_std_17)

# Test: RTP Transmitter
add_executable(TestRTPTransmitter
    TestRTPTransmitter.cpp
    ../NetworkEngine/RTP/RTPTransmitter.cpp
    ../NetworkEngine/RTP/SimpleRTP.cpp
    ../Driver/SDPParser.cpp
    ../Driver/AudioThreadPriority.cpp
    ../NetworkEngine/StreamChannelMapper.cpp
    ../Shared/Types.cpp
)

target_include_directories(TestRTPTransmitter PRIVATE
    ${CMAKE_SOURCE_DIR}
)

target_compile_features(TestRTPTransmitter PRIVATE cxx_std_17)

# Test: PTP Clock
add_executable(TestPTPClock
    TestPTPClock.cpp
    ../NetworkEngine/PTP/PTPClock.cpp
    ../NetworkEngine/PTP/PTPDInterface.cpp
    ../NetworkEngine/PTP/PTPSlave.cpp
    ../NetworkEngine/PTP/PhaseLockedLoop.cpp
    ../NetworkEngine/PTP/CustomSys.cpp
    ../NetworkEngine/NetworkInterfaceDetection.cpp
    ../Driver/SDPParser.cpp
    ../Shared/Types.cpp
)

target_include_directories(TestPTPClock PRIVATE
    ${CMAKE_SOURCE_DIR}
)

target_compile_features(TestPTPClock PRIVATE cxx_std_17)

# Test: Stream Manager
add_executable(TestStreamManager
    TestStreamManager.cpp
    ../NetworkEngine/StreamManager.cpp
    ../NetworkEngine/StreamConfig.cpp
    ../NetworkEngine/NetworkUtils.cpp
    ../NetworkEngine/NetworkInterfaceDetection.cpp
    ../NetworkEngine/RTP/RTPReceiver.cpp
    ../NetworkEngine/RTP/RTPTransmitter.cpp
    ../NetworkEngine/RTP/SimpleRTP.cpp
    ../NetworkEngine/RTP/LockFreeCircularJitterBuffer.cpp
    ../NetworkEngine/RTP/LockFreePacketPool.cpp
    ../Driver/AudioThreadPriority.cpp
    ../Driver/SDPParser.cpp
    ../NetworkEngine/StreamChannelMapper.cpp
    ../Shared/Types.cpp
)

target_include_directories(TestStreamManager PRIVATE
    ${CMAKE_SOURCE_DIR}
)

target_compile_features(TestStreamManager PRIVATE cxx_std_17)

# Integration Test: Multi-Stream Scenarios
add_executable(TestMultiStream
    TestMultiStream.cpp
    ../Driver/SDPParser.cpp
    ../NetworkEngine/StreamChannelMapper.cpp
    ../NetworkEngine/PTP/PTPClock.cpp
    ../NetworkEngine/PTP/PTPDInterface.cpp
    ../NetworkEngine/PTP/PTPSlave.cpp
    ../NetworkEngine/PTP/PhaseLockedLoop.cpp
    ../NetworkEngine/PTP/CustomSys.cpp
    ../NetworkEngine/NetworkInterfaceDetection.cpp
    ../Shared/Types.cpp
)

target_include_directories(TestMultiStream PRIVATE
    ${CMAKE_SOURCE_DIR}
)

target_compile_features(TestMultiStream PRIVATE cxx_std_17)

# Benchmark: I/O Handler Performance
add_executable(BenchmarkIOHandler
    BenchmarkIOHandler.cpp
    ../Driver/AES67IOHandler.cpp
    ../Shared/Types.cpp
)

target_include_directories(BenchmarkIOHandler PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${ASPL_INCLUDE_DIR}
)

target_compile_features(BenchmarkIOHandler PRIVATE cxx_std_17)

# Link libASPL for benchmark
if(ASPL_LIBRARY)
    target_link_libraries(BenchmarkIOHandler PRIVATE ${ASPL_LIBRARY})
endif()

# Integration Test: Full Audio Path
add_executable(TestIntegrationAudioPath
    TestIntegrationAudioPath.cpp
    ../NetworkEngine/RTP/RTPReceiver.cpp
    ../NetworkEngine/RTP/RTPTransmitter.cpp
    ../NetworkEngine/RTP/SimpleRTP.cpp
    ../NetworkEngine/RTP/LockFreeCircularJitterBuffer.cpp
    ../NetworkEngine/RTP/LockFreePacketPool.cpp
    ../Driver/SDPParser.cpp
    ../Driver/AudioThreadPriority.cpp
    ../NetworkEngine/StreamChannelMapper.cpp
    ../NetworkEngine/NetworkInterfaceDetection.cpp
    ../Shared/Types.cpp
)

target_include_directories(TestIntegrationAudioPath PRIVATE
    ${CMAKE_SOURCE_DIR}
)

target_compile_features(TestIntegrationAudioPath PRIVATE cxx_std_17)

# Add tests to CTest
enable_testing()
add_test(NAME SDPParser COMMAND TestSDPParser)
add_test(NAME ChannelMapper COMMAND TestChannelMapper)
add_test(NAME RingBuffer COMMAND TestRingBuffer)
add_test(NAME RTPReceiver COMMAND TestRTPReceiver)
add_test(NAME RTPTransmitter COMMAND TestRTPTransmitter)
add_test(NAME PTPClock COMMAND TestPTPClock)
add_test(NAME StreamManager COMMAND TestStreamManager)
add_test(NAME MultiStream COMMAND TestMultiStream)
add_test(NAME IntegrationAudioPath COMMAND TestIntegrationAudioPath)

message(STATUS "Tests configured:")
message(STATUS "  - TestSDPParser")
message(STATUS "  - TestChannelMapper")
message(STATUS "  - TestRingBuffer")
message(STATUS "  - TestRTPReceiver")
message(STATUS "  - TestRTPTransmitter")
message(STATUS "  - TestPTPClock")
message(STATUS "  - TestStreamManager")
message(STATUS "  - TestMultiStream (Integration)")
message(STATUS "  - TestIntegrationAudioPath (Integration)")
message(STATUS "")
message(STATUS "Benchmarks configured:")
message(STATUS "  - BenchmarkIOHandler")
message(STATUS "")
message(STATUS "Run tests with: ctest or make test")
message(STATUS "Run benchmarks manually: ./BenchmarkIOHandler")

add_executable(TestPTPSlave TestPTPSlave.cpp ../NetworkEngine/PTP/PTPSlave.cpp)
add_test(NAME PTPSlaveExchange COMMAND TestPTPSlave)
set_tests_properties(PTPSlaveExchange PROPERTIES TIMEOUT 10)

foreach(t TestRTPTransmitter TestIntegrationAudioPath)
    target_sources(${t} PRIVATE ../NetworkEngine/PTP/PTPSlave.cpp)
endforeach()

add_executable(TestClockRateBridge TestClockRateBridge.cpp)
add_test(NAME ClockRateBridge COMMAND TestClockRateBridge)

add_executable(TestTxTimingDiagnostics TestTxTimingDiagnostics.cpp)
add_test(NAME TxTimingDiagnostics COMMAND TestTxTimingDiagnostics)

add_executable(TestClockStatus TestClockStatus.cpp)
add_test(NAME ClockStatus COMMAND TestClockStatus)

add_executable(TestRxSequenceSync
    TestRxSequenceSync.cpp
    ../NetworkEngine/RTP/LockFreeCircularJitterBuffer.cpp
    ../NetworkEngine/RTP/LockFreePacketPool.cpp
)
target_compile_features(TestRxSequenceSync PRIVATE cxx_std_17)
add_test(NAME RxSequenceSync COMMAND TestRxSequenceSync)

add_executable(TestAsymmetricIO TestAsymmetricIO.cpp ../Driver/AES67IOHandler.cpp)
target_include_directories(TestAsymmetricIO PRIVATE ${ASPL_INCLUDE_DIR})
target_link_libraries(TestAsymmetricIO PRIVATE ${ASPL_LIBRARY})
add_test(NAME AsymmetricIO COMMAND TestAsymmetricIO)
