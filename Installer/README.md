# macOS installer

The installer packages the AES67 Manager, CoreAudio driver, and clock service. Use a package from the project's [releases](https://github.com/JoFreHe/aes67-mac/releases), or follow the repository build instructions to create one locally.

The installed system components require administrator authorization. Restart the Mac after installation so CoreAudio can load the driver. Applying configuration in AES67 Manager briefly restarts CoreAudio and interrupts audio. Peer-side stream routing remains a separate configuration step.

See [Getting Started](../Docs/Getting-Started.md), [Testing](../Docs/Testing.md), [Troubleshooting](../Docs/Troubleshooting.md), and [Upstream and licensing notice](../Docs/UPSTREAM-NOTICE.md).
