# Contributing

Small, focused bug fixes and reproducible hardware test reports are welcome.

Before a pull request, build with `scripts/build-mac.sh` and run the checks relevant to your change. Keep the regression tests: they help protect routing, clocking and the audio path. A passing local test is not evidence of hardware interoperability.

For audio issues, describe the direction (device to Mac or Mac to device), software version, macOS version, console/card and firmware, sample rate, channel count, packet time and PTP state. Include the expected behavior, observed behavior and reproducible steps. Redact names, personal paths, IP/MAC addresses and device identifiers from logs. Avoid full recordings or diagnostic archives in public issues.

Use GitHub's private vulnerability reporting feature if enabled for a security issue; never post credentials or exploit details affecting a private system in a public issue.

Retain upstream license and copyright notices. Contributions are distributed under the project's GPL-3.0 license unless a file has an existing compatible third-party license. Use a GitHub no-reply email for commits if you do not want your personal email in public history.
