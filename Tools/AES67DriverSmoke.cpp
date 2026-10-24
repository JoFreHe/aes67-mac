// Loads the built bundle in an isolated process. Does not install/register a HAL driver.
#include <CoreAudio/AudioServerPlugIn.h>
#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>
#include <unistd.h>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <cmath>
#include <stdexcept>
#include <string>
#include <vector>
#include <thread>
#include <chrono>
#include <algorithm>

static std::vector<unsigned char> property(AudioServerPlugInDriverRef driver,AudioObjectID id,AudioObjectPropertySelector selector,AudioObjectPropertyScope scope=kAudioObjectPropertyScopeGlobal) {
    AudioObjectPropertyAddress address{selector,scope,kAudioObjectPropertyElementMain};
    UInt32 size=0;
    if((*driver)->GetPropertyDataSize(driver,id,0,&address,0,nullptr,&size)!=noErr || size>65536) throw std::runtime_error("Property size failed");
    std::vector<unsigned char> data(size); UInt32 written=0;
    if((*driver)->GetPropertyData(driver,id,0,&address,0,nullptr,size,&written,data.data())!=noErr || written!=size) throw std::runtime_error("Property read failed");
    return data;
}

static std::string loopbackStream(const char* name, const char* direction, const char* id) {
    return std::string("{\"enabled\":true,\"description\":\"") + name +
        "\",\"createdTimestamp\":1,\"modifiedTimestamp\":1,\"jitterBufferDepth\":0,"
        "\"networkInterface\":\"lo0\",\"sdp\":{\"sessionName\":\"" + name +
        "\",\"sessionInfo\":\"isolated HAL loopback test\",\"sessionID\":1,\"sessionVersion\":1,"
        "\"originUsername\":\"-\",\"originAddress\":\"127.0.0.1\",\"connectionAddress\":\"239.69.69.1\","
        "\"ttl\":1,\"port\":15967,\"payloadType\":97,\"encoding\":\"L24\",\"sampleRate\":48000,"
        "\"numChannels\":2,\"ptime\":1,\"framecount\":48,\"sourceAddress\":\"127.0.0.1\","
        "\"ptpDomain\":0,\"ptpMasterMAC\":\"\",\"mediaClockType\":\"local-test\",\"direction\":\"" + direction +
        "\"},\"mapping\":{\"streamID\":\"" + id + "\",\"streamName\":\"" + name +
        "\",\"streamChannelCount\":2,\"streamChannelOffset\":0,\"deviceChannelStart\":0,"
        "\"deviceChannelCount\":2,\"channelMap\":[]}}";
}

static int runAudioLoopback(AudioServerPlugInDriverRef driver, AudioObjectID device) {
    const auto inStreams = property(driver, device, kAudioDevicePropertyStreams, kAudioObjectPropertyScopeInput);
    const auto outStreams = property(driver, device, kAudioDevicePropertyStreams, kAudioObjectPropertyScopeOutput);
    if (inStreams.size() != sizeof(AudioObjectID) || outStreams.size() != sizeof(AudioObjectID))
        throw std::runtime_error("Expected one input and output stream");
    AudioObjectID inputStream = 0, outputStream = 0;
    std::memcpy(&inputStream, inStreams.data(), sizeof(inputStream));
    std::memcpy(&outputStream, outStreams.data(), sizeof(outputStream));

    Boolean willDo = false, inPlace = false;
    if ((*driver)->WillDoIOOperation(driver, device, 1, kAudioServerPlugInIOOperationWriteMix, &willDo, &inPlace) != noErr || !willDo)
        throw std::runtime_error("Plugin does not expose WriteMix");
    willDo = false;
    if ((*driver)->WillDoIOOperation(driver, device, 1, kAudioServerPlugInIOOperationReadInput, &willDo, &inPlace) != noErr || !willDo)
        throw std::runtime_error("Plugin does not expose ReadInput");
    if ((*driver)->StartIO(driver, device, 1) != noErr)
        throw std::runtime_error("Plugin StartIO failed");

    constexpr UInt32 frames = 48;
    constexpr double twoPi = 6.2831853071795864769;
    std::vector<Float32> out(frames * 2), in(frames * 2);
    uint64_t nonzeroSamples = 0;
    uint64_t stereoFrames = 0;
    float peak = 0.0f;
    uint64_t sample = 0;
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    UInt64 cycle = 1;
    while (std::chrono::steady_clock::now() < until) {
        for (UInt32 f = 0; f < frames; ++f, ++sample) {
            const float v = 0.5f * std::sin(twoPi * 997.0 * double(sample) / 48000.0);
            out[2 * f] = v;
            out[2 * f + 1] = -v;
        }
        AudioServerPlugInIOCycleInfo info{};
        info.mIOCycleCounter = cycle++;
        info.mNominalIOBufferFrameSize = frames;
        info.mInputTime.mSampleTime = double(sample);
        info.mOutputTime.mSampleTime = double(sample);
        if ((*driver)->DoIOOperation(driver, device, outputStream, 1,
                kAudioServerPlugInIOOperationWriteMix, frames, &info, out.data(), nullptr) != noErr)
            throw std::runtime_error("Plugin WriteMix callback failed");
        std::fill(in.begin(), in.end(), 0.0f);
        if ((*driver)->DoIOOperation(driver, device, inputStream, 1,
                kAudioServerPlugInIOOperationReadInput, frames, &info, in.data(), nullptr) != noErr)
            throw std::runtime_error("Plugin ReadInput callback failed");
        for (UInt32 f = 0; f < frames; ++f) {
            if (std::abs(in[2*f]) > 0.01f && std::abs(in[2*f] + in[2*f+1]) < 0.00001f) ++stereoFrames;
        }
        for (Float32 v : in) {
            peak = std::max(peak, std::abs(v));
            if (std::abs(v) > 0.01f) ++nonzeroSamples;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if ((*driver)->StopIO(driver, device, 1) != noErr)
        throw std::runtime_error("Plugin StopIO failed");
    std::cout << "Audio bridge: peak=" << peak << ", nonzero samples=" << nonzeroSamples << "\n";
    if (peak < 0.05f || nonzeroSamples < 1000 || stereoFrames < 500)
        throw std::runtime_error("HAL callback -> RTP multicast -> RX callback returned silence");
    return 0;
}

int main(int argc,char** argv) {
    const bool audioLoopback = argc == 3 && std::string(argv[2]) == "--audio-loopback";
    if(argc!=2 && argc!=4 && !audioLoopback) return 2;
    const int inputCount=argc==4?std::stoi(argv[2]):32;
    const int outputCount=argc==4?std::stoi(argv[3]):32;
    if(!audioLoopback && (inputCount<1||inputCount>32||outputCount<1||outputCount>32))return 2;
    char dir[]="/tmp/aes67-smoke-XXXXXX";
    if(!mkdtemp(dir)) return 3;
    std::string config=std::string(dir)+"/streams.json";
    {std::ofstream f(config);
     if (audioLoopback) {
         f << "{\"version\":\"1.0\",\"inputChannels\":2,\"outputChannels\":2,\"streams\":["
           << loopbackStream("isolated-tx", "sendonly", "00000000-0000-0000-0000-000000000001") << ","
           << loopbackStream("isolated-rx", "recvonly", "00000000-0000-0000-0000-000000000002") << "]}";
     } else {
         f<<"{\"version\":\"1.0\",\"streams\":[]";
         if(argc==4)f<<",\"inputChannels\":"<<inputCount<<",\"outputChannels\":"<<outputCount;
         f<<"}";
     }}
    setenv("AES67_CONFIG_PATH",config.c_str(),1);
    int result=0;
    try {
        void* library=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL);
        if(!library) throw std::runtime_error(dlerror());
        auto create=reinterpret_cast<void*(*)(CFAllocatorRef,CFUUIDRef)>(dlsym(library,"Create"));
        if(!create) throw std::runtime_error("Missing factory");
        auto driver=reinterpret_cast<AudioServerPlugInDriverRef>(create(kCFAllocatorDefault,kAudioServerPlugInTypeUUID));
        if(!driver) throw std::runtime_error("Factory returned null");
        auto devices=property(driver,kAudioObjectPlugInObject,kAudioPlugInPropertyDeviceList);
        if(devices.size()!=sizeof(AudioObjectID)) throw std::runtime_error("Expected one device");
        AudioObjectID device;std::memcpy(&device,devices.data(),sizeof(device));
        auto rateBytes=property(driver,device,kAudioDevicePropertyNominalSampleRate);
        Float64 rate=0;
        if(rateBytes.size()!=sizeof(rate)) throw std::runtime_error("Unexpected nominal rate size");
        std::memcpy(&rate,rateBytes.data(),sizeof(rate));
        if(rate!=48000) throw std::runtime_error("Device nominal rate must agree with 48kHz streams");
        auto streams=property(driver,device,kAudioDevicePropertyStreams,kAudioObjectPropertyScopeInput);
        if(streams.size()!=sizeof(AudioObjectID)) throw std::runtime_error("Expected one multichannel input stream");
        AudioObjectID stream;std::memcpy(&stream,streams.data(),sizeof(stream));
        auto bytes=property(driver,stream,kAudioStreamPropertyPhysicalFormat);
        if(bytes.size()!=sizeof(AudioStreamBasicDescription)) throw std::runtime_error("Unexpected format size");
        AudioStreamBasicDescription format;std::memcpy(&format,bytes.data(),sizeof(format));
        const UInt32 expectedInput = static_cast<UInt32>(audioLoopback ? 2 : inputCount);
        const UInt32 expectedOutput = static_cast<UInt32>(audioLoopback ? 2 : outputCount);
        if(format.mChannelsPerFrame!=expectedInput || format.mSampleRate!=48000 || format.mBitsPerChannel!=32 || !(format.mFormatFlags&kAudioFormatFlagIsFloat)) throw std::runtime_error("Expected configured input channels/48kHz/float32");
        streams=property(driver,device,kAudioDevicePropertyStreams,kAudioObjectPropertyScopeOutput);
        if(streams.size()!=sizeof(AudioObjectID)) throw std::runtime_error("Expected one multichannel output stream");
        std::memcpy(&stream,streams.data(),sizeof(stream));
        bytes=property(driver,stream,kAudioStreamPropertyPhysicalFormat);
        std::memcpy(&format,bytes.data(),sizeof(format));
        if(format.mChannelsPerFrame!=expectedOutput || format.mSampleRate!=48000) throw std::runtime_error("Expected configured output channels/48kHz");
        if (audioLoopback) {
            result = runAudioLoopback(driver, device);
            if (result == 0) std::cout << "PASS: exported HAL WriteMix -> RTP multicast -> RTP receive -> ReadInput returned audio in isolated process (no HAL installation).\n";
        } else std::cout<<"PASS: actual bundle factory and property dispatch expose "<<inputCount<<" inputs and "<<outputCount<<" outputs at 48 kHz float32 (isolated process, no HAL installation).\n";
    }catch(const std::exception& e){std::cerr<<e.what()<<"\n";result=1;}
    unlink(config.c_str());rmdir(dir);
    return result;
}
