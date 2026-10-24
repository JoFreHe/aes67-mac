# AES67 Mac

An experimental AES67 network-audio driver and configuration app for macOS. Send and receive audio between CoreAudio applications and compatible AES67 devices over Ethernet.

**This is not Dante Virtual Soundcard and does not implement native Dante.** Dante-equipped devices must support and enable AES67 interoperability. Routing, firmware support, multicast and PTP configuration remain device-specific.

## Status

This project is under development. Hardware tests have found occasional audio-buffer drops and PTP-lock interruptions. A lost PTP lock pauses transmission. Do not rely on this build as the only recording or playback path for a critical event.

| Hardware | Tested direction | Status |
| --- | --- | --- |
| Behringer X32 with X-DANTE | Recording and playback | Hardware-tested; intermittent buffer and clock issues remain. |
| Yamaha TF5 with an AES67-capable Dante setup | Mac → console playback | Reported as tested by the maintainer. Console → Mac recording has not been tested; card and firmware details are not recorded. |

These are field-test observations, not manufacturer certification or a guarantee for every configuration. [Test guidance and limitations](Docs/Testing.md).

## Features

- CoreAudio device with independently configurable input and output channel counts, up to 32 each.
- AES67 RTP multicast, 48 kHz / 24-bit PCM, up to eight channels per 1 ms hardware stream.
- Manager app for Ethernet-interface selection, stream discovery, channel profiles and configuration.
- PTP clock service, receiver and transmitter counters, and diagnostic logs.
- Universal2 builds for Intel and Apple Silicon, with a macOS 11 deployment target. The deployment target alone does not establish runtime compatibility on every macOS release.

## Install

Download `AES67-Mac-<version>-universal.pkg` from [Releases](https://github.com/JoFreHe/aes67-mac/releases), when available. Install outside an active audio session: installation and applying configuration restart CoreAudio and interrupt audio on the Mac.

The installer package is unsigned. Included programs are ad-hoc signed, not Apple-notarized. Administrator access is required. Follow [Getting started](Docs/Getting-Started.md) for setup and [Troubleshooting](Docs/Troubleshooting.md) for diagnostics.

## Build from source

Requires macOS, Xcode or Apple Command Line Tools, Git, Python 3 and CMake 3.20+. The build script checks out a pinned libASPL dependency when it is not already present.

```sh
git clone https://github.com/JoFreHe/aes67-mac.git
cd aes67-mac
python3 -m venv .build-tools
.build-tools/bin/python -m pip install cmake==3.31.6
./scripts/build-mac.sh
./scripts/package-mac.sh
```

The build runs the C++ regression suite. Packaging creates the installer, corresponding source archive and SHA-256 checksums in `artifacts/`; it does not install the driver. Test source code is retained for reproducibility. Personal test logs, recordings, generated analysis caches and historical project plans are not part of the public source.

The Manager's setup checks can also be run without installation:

```sh
ManagerApp/AES67Manager.app/Contents/MacOS/AES67Manager --self-test /tmp/aes67-manager-test.txt
python3 scripts/test-package-quarantine.py
```

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md). For audio bugs, include the build, macOS version, device/card and firmware, signal direction and a short redacted log excerpt. Remove personal paths, device identifiers and network addresses before posting logs. Do not upload recordings without permission from everyone concerned.

## License and origin

GPL-3.0; see [LICENSE](LICENSE). This project derives from [maxajbarlow/AES67_macos_Driver](https://github.com/maxajbarlow/AES67_macos_Driver). libASPL and other third-party files retain their own licenses and copyright notices. See [Upstream notices](Docs/UPSTREAM-NOTICE.md).

No affiliation with or endorsement by Apple, Audinate, Behringer, Yamaha or other hardware manufacturers is claimed.
