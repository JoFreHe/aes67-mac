# Examples CMakeLists.txt
# AES67 macOS Driver - Build #4
# Example programs compilation

cmake_minimum_required(VERSION 3.15)

# Example: Simple SDP Parser
add_executable(SimpleSDPParse
    SimpleSDPParse.cpp
    ../Driver/SDPParser.cpp
    ../Shared/Types.cpp
)

target_include_directories(SimpleSDPParse PRIVATE
    ${CMAKE_SOURCE_DIR}
)

target_compile_features(SimpleSDPParse PRIVATE cxx_std_17)

# Example: Channel Mapper Demo
add_executable(ChannelMapperDemo
    ChannelMapperDemo.cpp
    ../NetworkEngine/StreamChannelMapper.cpp
    ../Driver/SDPParser.cpp
    ../Shared/Types.cpp
)

target_include_directories(ChannelMapperDemo PRIVATE
    ${CMAKE_SOURCE_DIR}
)

target_compile_features(ChannelMapperDemo PRIVATE cxx_std_17)

message(STATUS "Examples configured:")
message(STATUS "  - SimpleSDPParse")
message(STATUS "  - ChannelMapperDemo")
message(STATUS "")
message(STATUS "Run examples:")
message(STATUS "  ./SimpleSDPParse Docs/Examples/riedel_artist_8ch.sdp")
message(STATUS "  ./ChannelMapperDemo")
