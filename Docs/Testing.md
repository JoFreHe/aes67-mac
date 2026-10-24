# Testing

Use these checks to distinguish software, clock, network, and routing problems. Record the macOS version, project build, peer model and firmware, stream parameters, test direction, duration, and observed errors with each result. Do not infer stability from a short successful playback.

## Test matrix

| Direction | Basic check | Record |
| --- | --- | --- |
| Peer to Mac | Select the AES67 device in a CoreAudio application and record a known test signal. | Missing channels, input drops/overruns, duration, and clock state. |
| Mac to peer | Play a known signal from a CoreAudio application to a configured AES67 receive stream. | Output interruptions, stream counters, clock state, and duration. |
| Bidirectional | Run the two checks together only after each direction works on its own. | Any change in drop/overrun counters or timing. |

Keep sample rate, sample format, channel count, packet time, SDP, and PTP domain/profile compatible at both ends. Confirm that multicast and PTP traffic can pass the actual network path. Avoid changing several settings at once.

## Known test coverage

- Behringer X32 with X-DANTE recording and playback have been exercised; this is not a certification or a claim of broad X-DANTE firmware compatibility.
- Yamaha TF5 playback from Mac to console has been reported as tested. The installed card and test conditions are unknown; TF5 recording to Mac has not been tested.
- Software test results apply to the specific build and environment used. They do not prove hardware interoperability or sustained real-time reliability.

## Reading counters

Compare counters at the start and end of a clearly bounded test interval. A receive-ring-full count may mean the host did not consume received packets quickly enough; it does not by itself prove audible loss, and it is not evidence of a transmit failure. Likewise, output underruns or late transmit events are more directly relevant to Mac-to-peer playback, but still require correlation with the audible event and peer-side counters.
