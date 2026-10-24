# Troubleshooting

## No audio

- Confirm the selected CoreAudio device and channel mapping in the audio application.
- Check that both endpoints use compatible sample rate, format, channel count, packet time, SDP, and PTP settings.
- Verify the selected Ethernet interface, multicast reachability, and PTP status. A visible stream or subscription alone does not prove that audio is arriving on time.
- Check routing at the peer; this project does not configure console routing or Dante Controller subscriptions for you.

## Clicks or dropouts

Test each direction separately and note the exact time of an audible event. Compare only counters that changed during that interval:

- For peer-to-Mac recording, inspect receive packet/ring counters and CoreAudio input underruns.
- For Mac-to-peer playback, inspect transmit failures, late/underrun counters if available, the PTP lock state, and the peer's receive counters.
- A PTP lock transition near an interruption is relevant evidence, but does not alone identify why the clock became unstable.
- High CPU load, aggressive packet timing, network congestion, interface selection, or clock instability are possible contributors; treat them as hypotheses until a controlled test isolates one.

Capture the project version, macOS version, peer and card/firmware details, stream configuration, test direction, duration, and before/after counter values when reporting an issue. Remove personal paths, network identifiers, and unrelated log contents before sharing diagnostics.

## Installation

After installation, restart the Mac. If the audio device is absent, verify that the package matches the machine and macOS version, then review the installer and system logs. Do not disable macOS security protections as a troubleshooting shortcut.

## Log locations

- `/tmp/aes67driver_debug.log` and `.1`: stream lifecycle, receiver/transmitter counters and CoreAudio buffer events. Keep both files when correlating an incident across rotation.
- `/Library/Application Support/AES67Driver/clock-service.log`: interface selection and PTP lock transitions.
- `~/Library/Logs/AES67Manager.log`: Manager messages and configuration events.

The logs can include network addresses, device IDs and local paths. Review and redact them before sharing. A transmitter readiness counter measures when a packet is ready before the UDP send call, not its arrival time at the receiver. Statistics logged every ten seconds describe an interval; a maximum since reset is not a fresh error in every interval.
