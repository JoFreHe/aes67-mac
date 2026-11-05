# Getting started

AES67 Mac provides a CoreAudio device for sending and receiving AES67 RTP audio over an Ethernet network. It is a community project and does not implement native Dante discovery, control, or audio protocols.

## Requirements

- macOS 11 or later; Intel and Apple silicon builds are intended.
- An Ethernet interface and an AES67-capable peer configured for compatible RTP, multicast, sample-rate, channel, and PTP settings.
- Administrator access to install the system audio driver and clock service.

## Install and configure

### Install the release package

The current release is [v0.2.11](https://github.com/JoFreHe/aes67-mac/releases/tag/v0.2.11). Its installer package is unsigned. The included programs are ad-hoc signed and are not notarized by Apple. Consequently, macOS cannot verify the package as coming from an identified developer or having passed Apple's notarization check and may block it when opened. This warning is expected for this release; it is not evidence that Apple has checked or approved the package.

1. From the release’s **Assets**, download `AES67-Mac-0.2.11-universal.pkg` and `SHA256SUMS.txt`.
2. If you saved both files in Downloads, open Terminal and calculate the package checksum:

   ```sh
   cd ~/Downloads
   shasum -a 256 AES67-Mac-0.2.11-universal.pkg
   ```

   Compare the 64-character hash printed by the command with the line for `AES67-Mac-0.2.11-universal.pkg` in `SHA256SUMS.txt`. Continue only if they match. This compares the package directly; do not use `shasum -c SHA256SUMS.txt`, since that checksum file also lists the source archive, which may not have been downloaded.
3. Open the package once in Finder. If macOS says it cannot verify the package or its developer, dismiss the warning. After this attempt, open **System Settings → Privacy & Security**, scroll to **Security**, and click **Open Anyway** for the AES67 Mac package. Confirm **Open** when the warning returns and enter your password if prompted. On macOS 11 or 12, use **System Preferences → Security & Privacy → General** instead. The **Open Anyway** choice appears only for a limited time after an attempted open; if it is absent, try opening the package again and return to the security settings. A managed Mac may restrict this override. See [Apple's instructions for opening software from an unidentified developer](https://support.apple.com/102445).
4. Install outside an active audio session. The required restart interrupts audio on the Mac. Restart afterward so CoreAudio and the clock service load the installed components.

Only make this exception after confirming you downloaded the intended package from the release page and its checksum matches. The checksum detects a changed download but does not replace Apple's developer identity or notarization checks. Do not disable Gatekeeper or System Integrity Protection, or remove quarantine attributes broadly. If macOS reports that the package is damaged or contains malware, stop, download it again from the release page, verify its checksum, and report the issue before proceeding.

### Configure

1. Open AES67 Manager, select the network interface and channel profile, configure the streams, and apply the changes. Applying settings briefly restarts CoreAudio and interrupts audio.
2. In the macOS audio application, select the AES67 device. Configure the peer’s stream routing and subscriptions separately.
3. Start with a low-risk test signal and verify both endpoints’ sample rate, PTP state, multicast reachability, and channel mapping.

For supported stream settings and a repeatable test approach, see [Testing](Testing.md). For common symptoms, see [Troubleshooting](Troubleshooting.md).

The project has been exercised with a Behringer X32 AES67 setup. Yamaha TF5 playback from Mac to console has also been reported as tested; the card model and test details are not documented, and recording from the TF5 has not been tested. These observations do not establish general compatibility or long-term reliability.
