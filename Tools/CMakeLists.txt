# Tools CMakeLists.txt
# AES67 macOS Driver - Test and debugging tools

cmake_minimum_required(VERSION 3.15)

# AES67 Test Sender: generates sine wave RTP multicast stream with SAP
add_executable(AES67TestSender
    AES67TestSender.cpp
    ../NetworkEngine/RTP/SimpleRTP.cpp
)

target_include_directories(AES67TestSender PRIVATE
    ${CMAKE_SOURCE_DIR}
)

target_compile_features(AES67TestSender PRIVATE cxx_std_17)

# AES67 Test Receiver: joins multicast group and reports received RTP packets
add_executable(AES67TestReceiver
    AES67TestReceiver.cpp
    ../NetworkEngine/RTP/SimpleRTP.cpp
    ../NetworkEngine/NetworkInterfaceDetection.cpp
)

target_include_directories(AES67TestReceiver PRIVATE
    ${CMAKE_SOURCE_DIR}
)

target_compile_features(AES67TestReceiver PRIVATE cxx_std_17)

message(STATUS "Tools configured:")
message(STATUS "  - AES67TestSender")
message(STATUS "  - AES67TestReceiver")
message(STATUS "")
message(STATUS "Run test sender:")
message(STATUS "  ./Tools/AES67TestSender --ip 239.1.1.1 --port 5004 --channels 8 --freq 1000")
message(STATUS "Run test receiver:")
message(STATUS "  ./Tools/AES67TestReceiver --ip 239.1.1.2 --port 5004 --interface en0 --duration 10")

# Passive stream discovery, runnable without driver installation.
add_executable(AES67Discover AES67Discover.cpp)
target_compile_features(AES67Discover PRIVATE cxx_std_17)
if(BUILD_TESTS)
    add_test(NAME SAPDiscoveryParser COMMAND AES67Discover --self-test)
endif()

if(APPLE)
    add_executable(AES67DriverSmoke AES67DriverSmoke.cpp)
    target_link_libraries(AES67DriverSmoke PRIVATE "-framework CoreAudio" "-framework CoreFoundation")
    if(BUILD_TESTS)
        add_test(NAME DriverBundleSmoke COMMAND AES67DriverSmoke $<TARGET_FILE:AES67Driver>)
        add_test(NAME DriverBundle4In8Out COMMAND AES67DriverSmoke $<TARGET_FILE:AES67Driver> 4 8)
        add_test(NAME DriverBundle1In1Out COMMAND AES67DriverSmoke $<TARGET_FILE:AES67Driver> 1 1)
        add_test(NAME DriverBundle16In2Out COMMAND AES67DriverSmoke $<TARGET_FILE:AES67Driver> 16 2)
        add_test(NAME DriverAudioRTPBridge COMMAND AES67DriverSmoke $<TARGET_FILE:AES67Driver> --audio-loopback)
        set_tests_properties(DriverAudioRTPBridge PROPERTIES TIMEOUT 15)
    endif()
endif()

if(APPLE)
    add_executable(AES67HALProbe AES67HALProbe.cpp)
    target_compile_features(AES67HALProbe PRIVATE cxx_std_17)
    target_link_libraries(AES67HALProbe PRIVATE "-framework CoreAudio" "-framework CoreFoundation")
endif()

if(APPLE)
    add_executable(AES67ClockService AES67ClockService.cpp
        ../NetworkEngine/PTP/PTPSlave.cpp ../NetworkEngine/StreamConfig.cpp
        ../NetworkEngine/StreamChannelMapper.cpp ../Driver/SDPParser.cpp ../Shared/Types.cpp
        ../NetworkEngine/NetworkUtils.cpp)
    target_link_libraries(AES67ClockService PRIVATE "-framework CoreFoundation")
endif()
