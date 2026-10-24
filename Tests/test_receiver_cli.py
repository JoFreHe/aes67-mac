"""Exercise the shipped diagnostic with real loopback RTP and known L24 samples."""
import socket
import struct
import subprocess
import sys
import time

receiver = sys.argv[1]
group = "239.69.100.231"


def check(label, sample, expected):
    port = 19521
    process = subprocess.Popen(
        [receiver, "--ip", group, "--port", str(port), "--channels", "2",
         "--interface", "lo0", "--duration", "1"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
    )
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sender:
        sender.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_IF,
                          socket.inet_aton("127.0.0.1"))
        sender.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 0)
        time.sleep(0.15)
        if sample is not None:
            # Channel 1 is 0.25 full scale, channel 2 silence in the audio case.
            payload = (sample.to_bytes(3, "big") + bytes(3)) * 48
            for sequence in range(100):
                header = struct.pack("!BBHII", 0x80, 96, sequence, sequence * 48, 42)
                sender.sendto(header + payload, (group, port))
                time.sleep(0.002)
    stdout, stderr = process.communicate(timeout=4)
    assert f"status={expected}" in stdout, (label, stdout, stderr)
    assert process.returncode == (0 if expected == "AUDIO_DETECTED" else 1)
    if expected == "AUDIO_DETECTED":
        assert "channel1_peak=0.250000" in stdout, stdout
        assert "channel2_peak=0.000000" in stdout, stdout
    print(f"PASS {label}: {stdout.strip()}")


check("no sender", None, "NO_PACKETS")
check("silent sender", 0, "SILENCE")
check("stereo channel isolation", 0x200000, "AUDIO_DETECTED")
