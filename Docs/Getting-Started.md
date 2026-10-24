# Getting started

AES67 Mac provides a CoreAudio device for sending and receiving AES67 RTP audio over an Ethernet network. It is an experimental community project; it does not implement native Dante discovery, control, or audio protocols.

## Requirements

- macOS 11 or later; Intel and Apple silicon builds are intended.
- An Ethernet interface and an AES67-capable peer configured for compatible RTP, multicast, sample-rate, channel, and PTP settings.
- Administrator access to install the system audio driver and clock service.

## Install and configure

1. Obtain a package from the project’s [releases](https://github.com/JoFreHe/aes67-mac/releases), or follow the repository build instructions.
2. Install the package and restart the Mac so CoreAudio can load the driver.
3. Open AES67 Manager, select the network interface and channel profile, configure the streams, and apply the changes. Applying settings briefly restarts CoreAudio and interrupts audio.
4. In the macOS audio application, select the AES67 device. Configure the peer’s stream routing and subscriptions separately.
5. Start with a low-risk test signal and verify both endpoints’ sample rate, PTP state, multicast reachability, and channel mapping.

For supported stream settings and a repeatable test approach, see [Testing](Testing.md). For common symptoms, see [Troubleshooting](Troubleshooting.md).

The project has been exercised with a Behringer X32 AES67 setup. Yamaha TF5 playback from Mac to console has also been reported as tested; the card model and test details are not documented, and recording from the TF5 has not been tested. These observations do not establish general compatibility or long-term reliability.
