#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstddef>
#include <string>
#include <vector>

namespace {
constexpr const char* kDeviceUID = "com.aes67.driver.device";
constexpr double kPi = 3.14159265358979323846;
constexpr float kAmplitude = 0.05f;
constexpr double kTone1 = 997.0;
constexpr double kTone2 = 1501.0;
constexpr UInt32 kDurationSeconds = 3;

struct ProbeState {
    std::atomic<uint64_t> outputCallbacks{0};
    std::atomic<uint64_t> inputCallbacks{0};
    std::atomic<uint64_t> outputFrames{0};
    std::atomic<uint64_t> inputFrames{0};
    std::atomic<uint64_t> nonzeroInputSamples{0};
    std::atomic<float> inputPeak{0.0f};
    double corr1Sin = 0.0, corr1Cos = 0.0, energy1 = 0.0;
    double corr2Sin = 0.0, corr2Cos = 0.0, energy2 = 0.0;
    std::atomic<uint32_t> lastInputBuffers{0};
    std::atomic<uint32_t> lastOutputBuffers{0};
    double sampleRate = 48000.0;
    bool useInput = true;
    bool useOutput = true;
};

OSStatus propertySize(AudioObjectID object, AudioObjectPropertySelector selector,
                      AudioObjectPropertyScope scope, UInt32& size) {
    AudioObjectPropertyAddress address{selector, scope, kAudioObjectPropertyElementMain};
    return AudioObjectGetPropertyDataSize(object, &address, 0, nullptr, &size);
}

template <typename T>
bool getProperty(AudioObjectID object, AudioObjectPropertySelector selector,
                 AudioObjectPropertyScope scope, T& value) {
    AudioObjectPropertyAddress address{selector, scope, kAudioObjectPropertyElementMain};
    UInt32 size = sizeof(T);
    return AudioObjectGetPropertyData(object, &address, 0, nullptr, &size, &value) == noErr && size == sizeof(T);
}

std::string cfString(CFStringRef value) {
    if (!value) return {};
    char buffer[1024]{};
    if (CFStringGetCString(value, buffer, sizeof(buffer), kCFStringEncodingUTF8)) return buffer;
    return "<unavailable>";
}

AudioDeviceID findDevice() {
    UInt32 size = 0;
    if (propertySize(kAudioObjectSystemObject, kAudioHardwarePropertyDevices,
                     kAudioObjectPropertyScopeGlobal, size) != noErr || size == 0 || size % sizeof(AudioDeviceID))
        return kAudioObjectUnknown;
    std::vector<AudioDeviceID> devices(size / sizeof(AudioDeviceID));
    AudioObjectPropertyAddress address{kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal,
                                       kAudioObjectPropertyElementMain};
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, 0, nullptr, &size, devices.data()) != noErr)
        return kAudioObjectUnknown;
    for (AudioDeviceID device : devices) {
        CFStringRef uid = nullptr;
        if (getProperty(device, kAudioDevicePropertyDeviceUID, kAudioObjectPropertyScopeGlobal, uid)) {
            const bool matches = CFStringCompare(uid, CFSTR("com.aes67.driver.device"), 0) == kCFCompareEqualTo;
            CFRelease(uid);
            if (matches) return device;
        }
    }
    return kAudioObjectUnknown;
}

bool getFormat(AudioDeviceID device, AudioObjectPropertySelector selector,
               AudioObjectPropertyScope scope, AudioStreamBasicDescription& asbd) {
    return getProperty(device, selector, scope, asbd);
}

bool setStreamUsage(AudioDeviceID device, AudioDeviceIOProcID proc,
                    AudioObjectPropertyScope scope, bool enabled) {
    UInt32 bytes = 0;
    if (propertySize(device, kAudioDevicePropertyStreams, scope, bytes) != noErr || bytes % sizeof(AudioStreamID)) return false;
    const UInt32 count = bytes / sizeof(AudioStreamID);
    const size_t usageBytes = offsetof(AudioHardwareIOProcStreamUsage, mStreamIsOn) + count * sizeof(UInt32);
    std::vector<unsigned char> storage(usageBytes);
    auto* usage = reinterpret_cast<AudioHardwareIOProcStreamUsage*>(storage.data());
    usage->mIOProc = reinterpret_cast<void*>(proc);
    usage->mNumberStreams = count;
    for (UInt32 i = 0; i < count; ++i) usage->mStreamIsOn[i] = enabled ? 1 : 0;
    AudioObjectPropertyAddress address{kAudioDevicePropertyIOProcStreamUsage, scope, kAudioObjectPropertyElementMain};
    return AudioObjectSetPropertyData(device, &address, 0, nullptr, static_cast<UInt32>(usageBytes), usage) == noErr;
}

OSStatus ioProc(AudioDeviceID, const AudioTimeStamp*, const AudioBufferList* input,
                const AudioTimeStamp* inputTime, AudioBufferList* output, const AudioTimeStamp* outputTime, void* opaque) {
    auto& state = *static_cast<ProbeState*>(opaque);
    if (output && state.useOutput) {
        state.outputCallbacks.fetch_add(1, std::memory_order_relaxed);
        state.lastOutputBuffers.store(output->mNumberBuffers, std::memory_order_relaxed);
        uint64_t totalFrames = 0;
        for (UInt32 b = 0; b < output->mNumberBuffers; ++b) {
            AudioBuffer& buffer = output->mBuffers[b];
            const UInt32 channels = buffer.mNumberChannels;
            const UInt32 frames = channels ? buffer.mDataByteSize / (channels * sizeof(Float32)) : 0;
            auto* samples = static_cast<Float32*>(buffer.mData);
            if (!samples) continue;
            totalFrames = std::max<uint64_t>(totalFrames, frames);
            const double start = outputTime && (outputTime->mFlags & kAudioTimeStampSampleTimeValid) ? outputTime->mSampleTime : 0.0;
            for (UInt32 frame = 0; frame < frames; ++frame) {
                const double t = (start + frame) / state.sampleRate;
                for (UInt32 channel = 0; channel < channels; ++channel) {
                    float sample = 0.0f;
                    if (channel == 0) sample = kAmplitude * static_cast<float>(std::sin(2.0 * kPi * kTone1 * t));
                    else if (channel == 1) sample = kAmplitude * static_cast<float>(std::sin(2.0 * kPi * kTone2 * t));
                    samples[static_cast<size_t>(frame) * channels + channel] = sample;
                }
            }
        }
        state.outputFrames.fetch_add(totalFrames, std::memory_order_relaxed);
    }
    if (input && state.useInput) {
        state.inputCallbacks.fetch_add(1, std::memory_order_relaxed);
        state.lastInputBuffers.store(input->mNumberBuffers, std::memory_order_relaxed);
        uint64_t totalFrames = 0, nonzero = 0;
        float peak = 0.0f;
        double c1Sin = 0.0, c1Cos = 0.0, c2Sin = 0.0, c2Cos = 0.0;
        double e1 = 0.0, e2 = 0.0;
        const double inputStart = inputTime && (inputTime->mFlags & kAudioTimeStampSampleTimeValid) ? inputTime->mSampleTime : 0.0;
        for (UInt32 b = 0; b < input->mNumberBuffers; ++b) {
            const AudioBuffer& buffer = input->mBuffers[b];
            const UInt32 channels = buffer.mNumberChannels;
            const UInt32 frames = channels ? buffer.mDataByteSize / (channels * sizeof(Float32)) : 0;
            const auto* samples = static_cast<const Float32*>(buffer.mData);
            if (!samples) continue;
            totalFrames = std::max<uint64_t>(totalFrames, frames);
            for (UInt32 frame = 0; frame < frames; ++frame) {
                for (UInt32 channel = 0; channel < channels; ++channel) {
                    const float value = samples[static_cast<size_t>(frame) * channels + channel];
                    const float absValue = std::fabs(value);
                    if (absValue > peak) peak = absValue;
                    if (absValue > 1.0e-5f) ++nonzero;
                    const double t = (inputStart + frame) / state.sampleRate;
                    if (channel == 0) {
                        const double phase = 2.0 * kPi * kTone1 * t;
                        c1Sin += value * std::sin(phase); c1Cos += value * std::cos(phase); e1 += value * value;
                    }
                    if (channel == 1) {
                        const double phase = 2.0 * kPi * kTone2 * t;
                        c2Sin += value * std::sin(phase); c2Cos += value * std::cos(phase); e2 += value * value;
                    }
                }
            }
        }
        state.inputFrames.fetch_add(totalFrames, std::memory_order_relaxed);
        state.nonzeroInputSamples.fetch_add(nonzero, std::memory_order_relaxed);
        float oldPeak = state.inputPeak.load(std::memory_order_relaxed);
        while (oldPeak < peak && !state.inputPeak.compare_exchange_weak(oldPeak, peak, std::memory_order_relaxed)) {}
        state.corr1Sin += c1Sin; state.corr1Cos += c1Cos; state.energy1 += e1;
        state.corr2Sin += c2Sin; state.corr2Cos += c2Cos; state.energy2 += e2;
    }
    return noErr;
}

void printFormat(const char* label, const AudioStreamBasicDescription& f) {
    std::printf("%s: rate=%.1f channels=%u format=0x%08x bits=%u bytes/frame=%u flags=0x%08x\n",
                label, f.mSampleRate, f.mChannelsPerFrame, static_cast<unsigned>(f.mFormatID),
                f.mBitsPerChannel, f.mBytesPerFrame, static_cast<unsigned>(f.mFormatFlags));
}
}

int main(int argc, char** argv) {
    enum class Mode { Duplex, SendOnly, ReceiveOnly };
    Mode mode = Mode::Duplex;
    if (argc == 2 && std::strcmp(argv[1], "--send-only") == 0) mode = Mode::SendOnly;
    else if (argc == 2 && std::strcmp(argv[1], "--receive-only") == 0) mode = Mode::ReceiveOnly;
    else if (argc != 1) {
        std::fprintf(stderr, "Usage: %s [--send-only|--receive-only]\n", argv[0]);
        return 1;
    }
    const AudioDeviceID device = findDevice();
    if (device == kAudioObjectUnknown) {
        std::fprintf(stderr, "FAIL: no live CoreAudio device with UID %s\n", kDeviceUID);
        return 2;
    }
    CFStringRef name = nullptr;
    if (getProperty(device, kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal, name)) {
        std::printf("Device: id=%u uid=%s name=%s\n", device, kDeviceUID, cfString(name).c_str());
        CFRelease(name);
    } else std::printf("Device: id=%u uid=%s\n", device, kDeviceUID);

    auto readStreamFormat = [](AudioDeviceID d, AudioObjectPropertyScope scope,
                               AudioObjectPropertySelector selector, AudioStreamBasicDescription& format) {
        UInt32 bytes = 0;
        if (propertySize(d, kAudioDevicePropertyStreams, scope, bytes) != noErr || bytes < sizeof(AudioObjectID)) return false;
        std::vector<AudioObjectID> streams(bytes / sizeof(AudioObjectID));
        AudioObjectPropertyAddress address{kAudioDevicePropertyStreams, scope, kAudioObjectPropertyElementMain};
        if (AudioObjectGetPropertyData(d, &address, 0, nullptr, &bytes, streams.data()) != noErr || streams.empty()) return false;
        return getFormat(streams.front(), selector, kAudioObjectPropertyScopeGlobal, format);
    };
    AudioStreamBasicDescription inputVirtual{}, outputVirtual{}, inputPhysical{}, outputPhysical{};
    if (!readStreamFormat(device, kAudioObjectPropertyScopeInput, kAudioStreamPropertyVirtualFormat, inputVirtual) ||
        !readStreamFormat(device, kAudioObjectPropertyScopeOutput, kAudioStreamPropertyVirtualFormat, outputVirtual) ||
        !readStreamFormat(device, kAudioObjectPropertyScopeInput, kAudioStreamPropertyPhysicalFormat, inputPhysical) ||
        !readStreamFormat(device, kAudioObjectPropertyScopeOutput, kAudioStreamPropertyPhysicalFormat, outputPhysical)) {
        std::fprintf(stderr, "FAIL: could not read device virtual/physical stream formats\n");
        return 3;
    }
    printFormat("Input virtual", inputVirtual);
    printFormat("Input physical", inputPhysical);
    printFormat("Output virtual", outputVirtual);
    printFormat("Output physical", outputPhysical);
    if (inputVirtual.mChannelsPerFrame < 2 || outputVirtual.mChannelsPerFrame < 2 ||
        inputVirtual.mSampleRate <= 0 || outputVirtual.mSampleRate <= 0) {
        std::fprintf(stderr, "FAIL: live device does not expose at least two channels in both directions\n");
        return 4;
    }
    const auto isFloat32 = [](const AudioStreamBasicDescription& f) {
        return f.mFormatID == kAudioFormatLinearPCM && (f.mFormatFlags & kAudioFormatFlagIsFloat) &&
               f.mBitsPerChannel == 32;
    };
    if (!isFloat32(inputVirtual) || !isFloat32(outputVirtual)) {
        std::fprintf(stderr, "FAIL: probe requires Float32 linear PCM virtual formats for both directions\n");
        return 9;
    }

    ProbeState state;
    state.sampleRate = outputVirtual.mSampleRate;
    state.useInput = mode != Mode::SendOnly;
    state.useOutput = mode != Mode::ReceiveOnly;
    AudioDeviceIOProcID proc = nullptr;
    OSStatus status = AudioDeviceCreateIOProcID(device, ioProc, &state, &proc);
    if (status != noErr) {
        std::fprintf(stderr, "FAIL: AudioDeviceCreateIOProcID returned %d\n", static_cast<int>(status));
        return 5;
    }
    const bool useInput = mode != Mode::SendOnly;
    const bool useOutput = mode != Mode::ReceiveOnly;
    if (!setStreamUsage(device, proc, kAudioObjectPropertyScopeInput, useInput) ||
        !setStreamUsage(device, proc, kAudioObjectPropertyScopeOutput, useOutput)) {
        std::fprintf(stderr, "FAIL: could not configure IOProc input/output stream usage\n");
        AudioDeviceDestroyIOProcID(device, proc);
        return 8;
    }
    status = AudioDeviceStart(device, proc);
    if (status != noErr) {
        std::fprintf(stderr, "FAIL: AudioDeviceStart returned %d\n", static_cast<int>(status));
        AudioDeviceDestroyIOProcID(device, proc);
        return 6;
    }
    if (state.useOutput)
        std::printf("Probe active for %u seconds (%s): 0.05 amplitude tones %.0f Hz on output ch1, %.0f Hz on ch2; remaining channels zero.\n",
                    kDurationSeconds, mode == Mode::Duplex ? "duplex" : "send-only", kTone1, kTone2);
    else
        std::printf("Probe active for %u seconds (receive-only): output stream disabled; no tones generated.\n", kDurationSeconds);
    std::fflush(stdout);
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, kDurationSeconds, false);
    AudioDeviceStop(device, proc);
    AudioDeviceDestroyIOProcID(device, proc);

    const uint64_t outCalls = state.outputCallbacks.load();
    const uint64_t inCalls = state.inputCallbacks.load();
    const uint64_t inFrames = state.inputFrames.load();
    const uint64_t outFrames = state.outputFrames.load();
    const float peak = state.inputPeak.load();
    const uint64_t nonzero = state.nonzeroInputSamples.load();
    const double normalized1 = state.energy1 > 0 ? std::hypot(state.corr1Sin, state.corr1Cos) / std::sqrt(state.energy1 * (inFrames / 2.0)) : 0.0;
    const double normalized2 = state.energy2 > 0 ? std::hypot(state.corr2Sin, state.corr2Cos) / std::sqrt(state.energy2 * (inFrames / 2.0)) : 0.0;
    std::printf("Callbacks: output=%llu input=%llu; frames: output=%llu input=%llu; buffers last output=%u input=%u\n",
                static_cast<unsigned long long>(outCalls), static_cast<unsigned long long>(inCalls),
                static_cast<unsigned long long>(outFrames), static_cast<unsigned long long>(inFrames),
                state.lastOutputBuffers.load(), state.lastInputBuffers.load());
    if (state.useInput)
        std::printf("Input capture: peak=%.8f nonzero_samples=%llu tone_correlation(ch1@%.0fHz)=%.5f tone_correlation(ch2@%.0fHz)=%.5f\n",
                    peak, static_cast<unsigned long long>(nonzero), kTone1, normalized1, kTone2, normalized2);
    else
        std::printf("Input capture: disabled by mode.\n");
    if ((useInput && (inCalls == 0 || inFrames == 0 || peak < 1.0e-5f || nonzero == 0)) ||
        (useOutput && (outCalls == 0 || outFrames == 0))) {
        std::fprintf(stderr, "FAIL: live HAL callback did not capture nonzero input\n");
        return 7;
    }
    if (state.useInput && state.useOutput)
        std::printf("PASS: live HAL IOProc produced output and captured nonzero input.\n");
    else if (state.useInput)
        std::printf("PASS: live HAL IOProc captured nonzero input with output disabled.\n");
    else
        std::printf("PASS: live HAL IOProc produced output with input disabled.\n");
    return 0;
}
