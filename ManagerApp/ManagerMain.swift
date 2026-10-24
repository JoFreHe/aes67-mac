import AppKit
import Foundation
import Darwin
import CoreAudio
import SystemConfiguration

struct EasySetup: Codable, Equatable {
    var interface: String
    var channels: Int = 8
    var selectedSources = [String]()
    var multicastBase = "239.69.250"
    // Console chosen as a whole (SAP origin address). Its announced flows are
    // followed automatically; nil means sources were picked one by one.
    var device: String?
    init(interface: String, channels: Int = 8, selectedSources: [String] = [], multicastBase: String = "239.69.250", device: String? = nil) {
        self.interface = interface; self.channels = channels; self.selectedSources = selectedSources; self.multicastBase = multicastBase; self.device = device
    }
}
struct Config: Codable, Equatable {
    var version = "1.0"
    var inputChannels = 32, outputChannels = 32
    var streams = [Stream]()
    var easySetup: EasySetup?
    enum CodingKeys: String, CodingKey { case version, inputChannels, outputChannels, streams, easySetup }
    init(version: String = "1.0", inputChannels: Int = 32, outputChannels: Int = 32, streams: [Stream] = []) {
        self.version = version; self.inputChannels = inputChannels; self.outputChannels = outputChannels; self.streams = streams
    }
    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        version = try c.decodeIfPresent(String.self, forKey: .version) ?? "1.0"
        inputChannels = try c.decodeIfPresent(Int.self, forKey: .inputChannels) ?? 32
        outputChannels = try c.decodeIfPresent(Int.self, forKey: .outputChannels) ?? 32
        streams = try c.decodeIfPresent([Stream].self, forKey: .streams) ?? []
        easySetup = try c.decodeIfPresent(EasySetup.self, forKey: .easySetup)
    }
}
struct Stream: Codable, Equatable {
    var enabled = true, description = ""
    var createdTimestamp: UInt64, modifiedTimestamp: UInt64
    var jitterBufferDepth = 0, networkInterface: String
    var sdp: SDP; var mapping: Mapping
}
struct SDP: Codable, Equatable {
    var sessionName = "", sessionInfo = ""
    var sessionID: UInt64 = 0, sessionVersion: UInt64 = 0
    var originUsername = "-", originAddress = "", connectionAddress = ""
    var ttl = 32, port = 5004, payloadType = 96
    var encoding = "L24", sampleRate = 48000, numChannels = 8, ptime = 1, framecount = 48
    var sourceAddress = "", ptpDomain = 0, ptpMasterMAC = "", mediaClockType = "direct=0", direction = "recvonly"
}
struct Mapping: Codable, Equatable {
    var streamID: String, streamName: String, streamChannelCount: Int
    var streamChannelOffset = 0, deviceChannelStart: Int, deviceChannelCount: Int
    var channelMap = [Int]()
}
func localLoopbackConfig(inputChannels: Int, outputChannels: Int) -> Config {
    let now = UInt64(Date().timeIntervalSince1970)
    func stream(_ title: String, _ direction: String) -> Stream {
        var s = SDP(); s.sessionName = title; s.connectionAddress = "239.69.100.2"; s.port = 5006
        s.numChannels = 2; s.encoding = "L24"; s.sampleRate = 48000; s.ptime = 1; s.framecount = 48
        s.direction = direction; s.mediaClockType = "local-test"
        if direction == "sendonly" {
            s.originAddress = "127.0.0.1"; s.sourceAddress = "127.0.0.1"; s.sessionID = now; s.sessionVersion = now
        }
        let m = Mapping(streamID: UUID().uuidString.lowercased(), streamName: title,
                        streamChannelCount: 2, deviceChannelStart: 0, deviceChannelCount: 2)
        return Stream(createdTimestamp: now, modifiedTimestamp: now, networkInterface: "lo0", sdp: s, mapping: m)
    }
    return Config(inputChannels: max(2, inputChannels), outputChannels: max(2, outputChannels),
                  streams: [stream("Dieser Mac — AES67-Ausgang", "sendonly"), stream("Dieser Mac — Eingang 1–2", "recvonly")])
}
func backupConfigIfMissing(_ config: Config, at url: URL) throws {
    guard !FileManager.default.fileExists(atPath: url.path) else { return }
    let encoder = JSONEncoder(); encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
    try encoder.encode(config).write(to: url, options: .atomic)
}
struct Port: Equatable { var name: String; var ip: String; var wired = false; var mask = "" }
// The saved adapter wins. USB adapters change their name when replugged
// (en13 → en18); if the saved one is gone and exactly one wired port is active,
// that port takes over and the window says so. With several wired ports the
// Mac never picks a LAN on its own.
func automaticPort(saved: String?, ports: [Port]) -> Port? {
    if let saved, let port = ports.first(where: { $0.name == saved && $0.wired }) { return port }
    let wired = ports.filter(\.wired)
    return wired.count == 1 ? wired[0] : nil
}
func isLinkLocal(_ ip: String) -> Bool { ip.hasPrefix("169.254.") }
func sameSubnet(_ port: Port, _ other: String) -> Bool {
    func value(_ s: String) -> UInt32? {
        let p = s.split(separator: ".").compactMap { UInt32($0) }
        return p.count == 4 && p.allSatisfy({ $0 < 256 }) ? p.reduce(0) { $0 << 8 | $1 } : nil
    }
    guard let a = value(port.ip), let b = value(other) else { return true }
    let mask = value(port.mask) ?? 0xFFFF_FF00
    return a & mask == b & mask
}
// Explains the failure seen after a cable swap: no DHCP lease, so the Mac sits on
// a link-local address while the console stays in the venue network.
func networkWarning(port: Port, console: String?) -> String? {
    if isLinkLocal(port.ip) {
        if let console, !isLinkLocal(console) {
            return "\(port.name) hat nur eine Notadresse (\(port.ip)), das Pult sendet aber aus \(console). Kabel, Switch und DHCP prüfen."
        }
        return console == nil ? "\(port.name) hat nur eine Notadresse (\(port.ip)), also keine Adresse vom Netzwerk. Direkt am Pult ist das normal; in einem verwalteten Netzwerk deutet es auf ein Kabel- oder Switch-Problem hin." : nil
    }
    if let console, !sameSubnet(port, console) {
        return "Mac (\(port.ip)) und Pult (\(console)) liegen in verschiedenen Netzen. Der Dante Controller zeigt die Mac-Sender dann möglicherweise nicht an."
    }
    return nil
}
func suggestedSetupBase(_ port: Port) -> String {
    // Different host addresses on the same subnet get different initial groups.
    // This is a suggestion, not a network-wide multicast address reservation.
    let suffix = port.ip.split(separator: ".").last.flatMap { UInt8($0) } ?? 250
    return "239.69.\(suffix)"
}
// Dante also announces AES67 flows from 239.69.x.x. Keep the preferred base
// unless a console flow already uses one of its group addresses; then take the
// next free 239.69.n base.
func freeSetupBase(_ preferred: String, channels: Int, taken: Set<String>) -> String? {
    let flows = max(1, (channels + 7) / 8)
    func free(_ base: String) -> Bool { (1...flows).allSatisfy { !taken.contains("\(base).\($0)") } }
    if free(preferred) { return preferred }
    let start = preferred.split(separator: ".").last.flatMap { Int($0) } ?? 250
    for step in 1...254 {
        let candidate = "239.69.\((start - 1 + step) % 254 + 1)"
        if free(candidate) { return candidate }
    }
    return nil
}
func newEasySetup(_ port: Port, channels: Int = 8) -> EasySetup {
    EasySetup(interface: port.name, channels: channels, multicastBase: suggestedSetupBase(port))
}
// Test setup has its own adapter choice, independent of the Advanced stream editor.
func testPort(saved: String?, ports: [Port]) -> Port? {
    if let saved { return ports.first { $0.name == saved } }
    if ports.count == 1 { return ports.first }
    return automaticPort(saved: nil, ports: ports)
}
func macTestDraft(_ original: Config) -> Config {
    var result = original
    if result.easySetup != nil { result.streams.removeAll(where: isSetupStream) }
    result.easySetup = nil
    return result
}
let setupTXMarker = "AES67 Manager automatic TX"
let setupRXMarker = "AES67 Manager automatic RX"
func isSetupStream(_ stream: Stream) -> Bool {
    stream.description == setupTXMarker || stream.description == setupRXMarker
}
func sourceIdentity(_ s: SDP) -> String {
    // Version is intentionally excluded: a sender can refresh the same flow.
    "\(s.originAddress)|\(s.sessionID)|\(s.connectionAddress)|\(s.port)|\(s.sessionName)"
}
struct SetupFailure: LocalizedError {
    let message: String
    var errorDescription: String? { message }
}
func plannedSetup(_ original: Config, channels: Int, interface: Port, sources: [String: SDP],
                  base: String, now: UInt64 = UInt64(Date().timeIntervalSince1970)) throws -> Config {
    guard [4, 8, 16].contains(channels), var settings = original.easySetup else {
        throw SetupFailure(message: "Profil 4 × 4, 8 × 8 oder 16 × 16 wählen.")
    }
    let unmanaged = original.streams.filter { !isSetupStream($0) }
    guard let freshTX = preparedTXStreams(channels: channels, interface: interface, existing: unmanaged, base: base, now: now) else {
        throw SetupFailure(message: "TX-Multicast-Basis ungültig oder bereits durch einen manuellen Stream belegt. Unter Erweitert eine andere Basis wählen.")
    }
    var generated = [Stream]()
    let clockDomain = settings.selectedSources.compactMap { sources[$0]?.ptpDomain }.first
        ?? original.streams.first(where: { $0.description == setupRXMarker })?.sdp.ptpDomain ?? 0
    for var stream in freshTX {
        stream.description = setupTXMarker; stream.sdp.ptpDomain = clockDomain
        if let old = original.streams.first(where: { $0.description == setupTXMarker && $0.mapping.deviceChannelStart == stream.mapping.deviceChannelStart }) {
            stream.sdp.sessionName = old.sdp.sessionName; stream.mapping.streamName = old.mapping.streamName
            stream.mapping.streamID = old.mapping.streamID; stream.createdTimestamp = old.createdTimestamp
            stream.sdp.sessionID = old.sdp.sessionID; stream.sdp.sessionVersion = old.sdp.sessionVersion
            stream.modifiedTimestamp = old.modifiedTimestamp
            if stream != old { stream.modifiedTimestamp = now; stream.sdp.sessionVersion = max(now, old.sdp.sessionVersion + 1) }
        }
        generated.append(stream)
    }
    var offset = 0
    for id in settings.selectedSources {
        guard offset < channels else { break }
        let old = original.streams.first { $0.description == setupRXMarker && sourceIdentity($0.sdp) == id }
        guard let actual = sources[id] ?? old?.sdp else { continue }
        guard actual.ptpDomain == clockDomain else { throw SetupFailure(message: "Die ausgewählten Quellen verwenden verschiedene PTP-Domains. Bitte Quellen mit derselben Domain wählen.") }
        let count = min(actual.numChannels, channels - offset)
        guard (1...8).contains(actual.numChannels), count > 0 else { continue }
        // Keep the advertised RTP frame width. Unused channels are discarded by the receiver.
        var mapping = Mapping(streamID: old?.mapping.streamID ?? UUID().uuidString.lowercased(), streamName: actual.sessionName,
                              streamChannelCount: actual.numChannels, deviceChannelStart: offset, deviceChannelCount: count)
        if count < actual.numChannels { mapping.channelMap = (0..<actual.numChannels).map { $0 < count ? offset + $0 : -1 } }
        var stream = Stream(description: setupRXMarker, createdTimestamp: old?.createdTimestamp ?? now,
                            modifiedTimestamp: old?.modifiedTimestamp ?? now, networkInterface: interface.name, sdp: actual, mapping: mapping)
        if let old, stream != old { stream.modifiedTimestamp = now }
        generated.append(stream); offset += count
    }
    for stream in unmanaged where stream.enabled {
        let end = stream.mapping.deviceChannelStart + stream.mapping.deviceChannelCount
        guard end <= channels else { throw SetupFailure(message: "Der manuelle Stream ‚\(stream.sdp.sessionName)‘ belegt Kanal \(end). Er bleibt erhalten; zuerst unter Erweitert anpassen.") }
        for item in generated where (item.sdp.direction == "sendonly") == (stream.sdp.direction == "sendonly") {
            let a = item.mapping.deviceChannelStart, b = a + item.mapping.deviceChannelCount
            if a < end && stream.mapping.deviceChannelStart < b { throw SetupFailure(message: "Manueller Stream ‚\(stream.sdp.sessionName)‘ belegt den benötigten Kanalbereich. Er bleibt erhalten; zuerst unter Erweitert anpassen.") }
        }
    }
    var result = original; result.inputChannels = channels; result.outputChannels = channels
    result.streams = unmanaged + generated
    settings.interface = interface.name; settings.channels = channels; settings.multicastBase = base
    result.easySetup = settings
    return result
}
// A console (an X32 with X-DANTE, any AES67 device) is identified by the unicast
// origin address of its SAP announcements. Its flows become the Logic inputs in
// flow-name order.
struct ConsoleDevice: Equatable {
    var address: String, name: String
    var sources: [String]
    var channels: Int, ptpDomain: Int
}
func consoleName(_ sessionNames: [String]) -> String? {
    // Dante names AES67 flows "<device> : <flow>"; otherwise use the common prefix.
    let stems = Set(sessionNames.map { name -> String in
        for separator in [" : ", " – ", " - "] {
            if let range = name.range(of: separator, options: .backwards), range.lowerBound > name.startIndex { return String(name[..<range.lowerBound]) }
        }
        return name
    })
    if stems.count == 1 { return stems.first }
    guard var prefix = sessionNames.first else { return nil }
    for name in sessionNames { while !name.hasPrefix(prefix) { prefix.removeLast() } }
    let trimmed = prefix.trimmingCharacters(in: CharacterSet.decimalDigits.union(.whitespaces).union(.punctuationCharacters))
    return trimmed.count >= 3 ? trimmed : nil
}
func consoleDevices(_ sources: [String: SDP]) -> [ConsoleDevice] {
    let groups = Dictionary(grouping: sources) { $0.value.originAddress.isEmpty ? $0.value.sourceAddress : $0.value.originAddress }
    return groups.compactMap { address, entries -> ConsoleDevice? in
        guard !address.isEmpty else { return nil }
        let ordered = entries.sorted {
            let order = $0.value.sessionName.localizedStandardCompare($1.value.sessionName)
            return order == .orderedSame ? $0.key < $1.key : order == .orderedAscending
        }
        return ConsoleDevice(address: address, name: consoleName(ordered.map(\.value.sessionName)) ?? "AES67-Gerät \(address)",
                             sources: ordered.map(\.key), channels: ordered.reduce(0) { $0 + $1.value.numChannels },
                             ptpDomain: ordered.first?.value.ptpDomain ?? 0)
    }.sorted { $0.name.localizedStandardCompare($1.name) == .orderedAscending || ($0.name == $1.name && $0.address < $1.address) }
}
func profileChannels(for channels: Int) -> Int { channels <= 4 ? 4 : channels <= 8 ? 8 : 16 }
// Choosing a console subscribes all its flows and sizes the profile to them.
func selectDevice(_ settings: EasySetup, _ device: ConsoleDevice) -> EasySetup {
    var result = settings
    result.device = device.address; result.selectedSources = device.sources
    result.channels = profileChannels(for: device.channels)
    return result
}
// Follow flows the console adds or removes; keep the remembered ones while its
// announcements are temporarily missing (cable out, card rebooting).
func followDevice(_ settings: EasySetup, devices: [ConsoleDevice]) -> EasySetup {
    guard let address = settings.device, let device = devices.first(where: { $0.address == address }) else { return settings }
    var result = settings; result.selectedSources = device.sources
    return result
}
// Read the device actually published by HAL, independently of files on disk.
func publishedDeviceChannels() -> (input: Int, output: Int)? {
    var address = AudioObjectPropertyAddress(mSelector: kAudioHardwarePropertyDevices,
        mScope: kAudioObjectPropertyScopeGlobal, mElement: kAudioObjectPropertyElementMaster)
    var size: UInt32 = 0
    guard AudioObjectGetPropertyDataSize(AudioObjectID(kAudioObjectSystemObject), &address, 0, nil, &size) == noErr else { return nil }
    guard size > 0 else { return nil }
    var devices = [AudioDeviceID](repeating: 0, count: Int(size) / MemoryLayout<AudioDeviceID>.size)
    let readStatus = devices.withUnsafeMutableBytes { buffer in
        AudioObjectGetPropertyData(AudioObjectID(kAudioObjectSystemObject), &address, 0, nil, &size, buffer.baseAddress!)
    }
    guard readStatus == noErr else { return nil }
    for device in devices {
        var uid: Unmanaged<CFString>?
        var uidSize = UInt32(MemoryLayout<Unmanaged<CFString>?>.size)
        var uidAddress = AudioObjectPropertyAddress(mSelector: kAudioDevicePropertyDeviceUID,
            mScope: kAudioObjectPropertyScopeGlobal, mElement: kAudioObjectPropertyElementMaster)
        guard AudioObjectGetPropertyData(device, &uidAddress, 0, nil, &uidSize, &uid) == noErr,
              let deviceUID = uid?.takeRetainedValue(), deviceUID as String == "com.aes67.driver.device" else { continue }
        func channels(_ scope: AudioObjectPropertyScope) -> Int? {
            var property = AudioObjectPropertyAddress(mSelector: kAudioDevicePropertyStreamConfiguration,
                mScope: scope, mElement: kAudioObjectPropertyElementMaster)
            var bytes: UInt32 = 0
            guard AudioObjectGetPropertyDataSize(device, &property, 0, nil, &bytes) == noErr,
                  bytes >= MemoryLayout<AudioBufferList>.size else { return nil }
            let data = UnsafeMutableRawPointer.allocate(byteCount: Int(bytes), alignment: MemoryLayout<AudioBufferList>.alignment)
            defer { data.deallocate() }
            guard AudioObjectGetPropertyData(device, &property, 0, nil, &bytes, data) == noErr else { return nil }
            return UnsafeMutableAudioBufferListPointer(data.assumingMemoryBound(to: AudioBufferList.self)).reduce(0) { $0 + Int($1.mNumberChannels) }
        }
        if let input = channels(kAudioDevicePropertyScopeInput), let output = channels(kAudioDevicePropertyScopeOutput) {
            return (input, output)
        }
    }
    return nil
}
final class CaptureBuffer {
    private let lock = NSLock()
    private var data = Data()
    func append(_ chunk: Data) { lock.lock(); data.append(chunk); lock.unlock() }
    func text() -> String { lock.lock(); defer { lock.unlock() }; return String(data: data, encoding: .utf8) ?? "" }
}
func multicast(_ value: String) -> Bool {
    let fields = value.split(separator: ".")
    guard fields.count == 4, let first = UInt8(fields[0]), first == 239 else { return false }
    return fields.dropFirst().allSatisfy { UInt8($0) != nil }
}
func preparedTXStreams(channels: Int, interface: Port, existing: [Stream], base: String = "239.69.250", now: UInt64 = UInt64(Date().timeIntervalSince1970)) -> [Stream]? {
    guard [4, 8, 16].contains(channels) else { return nil }
    let octets = base.split(separator: ".")
    guard octets.count == 3, octets.allSatisfy({ UInt8($0) != nil }), octets.first == "239" else { return nil }
    let used = Set(existing.filter { $0.enabled && $0.sdp.direction == "sendonly" }.map { "\($0.sdp.connectionAddress):\($0.sdp.port)" })
    var result = [Stream]()
    for offset in stride(from: 0, to: channels, by: 8) {
        let width = min(8, channels - offset)
        let address = "\(base).\(offset / 8 + 1)"
        guard multicast(address), !used.contains("\(address):5004") else { return nil }
        var s = SDP(); s.sessionName = "Mac → Pult · \(offset + 1)–\(offset + width)"; s.connectionAddress = address
        s.port = 5004; s.numChannels = width; s.encoding = "L24"; s.sampleRate = 48000; s.ptime = 1; s.framecount = 48
        s.direction = "sendonly"; s.originAddress = interface.ip; s.sourceAddress = interface.ip
        s.sessionID = now &* 1000 &+ UInt64(offset / 8 + 1); s.sessionVersion = now
        let m = Mapping(streamID: UUID().uuidString.lowercased(), streamName: s.sessionName,
                        streamChannelCount: width, deviceChannelStart: offset, deviceChannelCount: width)
        result.append(Stream(createdTimestamp: now, modifiedTimestamp: now, networkInterface: interface.name, sdp: s, mapping: m))
    }
    return result
}
func interfaces() -> [Port] {
    var head: UnsafeMutablePointer<ifaddrs>?
    guard getifaddrs(&head) == 0 else { return [] }
    defer { freeifaddrs(head) }
    let physical = (SCNetworkInterfaceCopyAll() as? [SCNetworkInterface]) ?? []
    let wired = Set(physical.compactMap { item -> String? in
        guard SCNetworkInterfaceGetInterfaceType(item) == kSCNetworkInterfaceTypeEthernet else { return nil }
        return SCNetworkInterfaceGetBSDName(item) as String?
    })
    var out = [Port](); var cursor = head
    while let p = cursor {
        let x = p.pointee
        if let addr = x.ifa_addr, addr.pointee.sa_family == UInt8(AF_INET),
           (Int32(x.ifa_flags) & IFF_UP) != 0, (Int32(x.ifa_flags) & IFF_LOOPBACK) == 0 {
            func text(_ pointer: UnsafeMutablePointer<sockaddr>?) -> String {
                guard let pointer else { return "" }
                var bytes = [CChar](repeating: 0, count: Int(INET_ADDRSTRLEN))
                var value = pointer.withMemoryRebound(to: sockaddr_in.self, capacity: 1) { $0.pointee.sin_addr }
                return inet_ntop(AF_INET, &value, &bytes, socklen_t(INET_ADDRSTRLEN)) != nil ? String(cString: bytes) : ""
            }
            let ip = text(addr)
            if !ip.isEmpty {
                let name = String(cString: x.ifa_name)
                if !out.contains(where: { $0.name == name }) { out.append(Port(name: name, ip: ip, wired: wired.contains(name), mask: text(x.ifa_netmask))) }
            }
        }
        cursor = x.ifa_next
    }
    return out.sorted { $0.name < $1.name }
}
func importSession(_ input: String) -> SDP? {
    var s = SDP(), media = false, format = false
    for raw in input.components(separatedBy: .newlines) {
        let l = raw.trimmingCharacters(in: .whitespacesAndNewlines)
        if l.hasPrefix("s=") { s.sessionName = String(l.dropFirst(2)) }
        if l.hasPrefix("i=") { s.sessionInfo = String(l.dropFirst(2)) }
        if l.hasPrefix("o=") {
            let p = l.dropFirst(2).split(separator: " ")
            if p.count >= 6 { s.originUsername = String(p[0]); s.sessionID = UInt64(p[1]) ?? 0; s.sessionVersion = UInt64(p[2]) ?? 0; s.originAddress = String(p[5]) }
        }
        if l.hasPrefix("c=IN IP4 ") {
            let p = l.dropFirst(9).split(separator: "/")
            if let first = p.first { s.connectionAddress = String(first) }
            if p.count > 1 { s.ttl = Int(p[1]) ?? 32 }
        }
        if l.hasPrefix("m=audio ") {
            let p = l.dropFirst(8).split(separator: " ")
            if p.count >= 3 { s.port = Int(p[0]) ?? 0; s.payloadType = Int(p[2]) ?? 0; media = true }
        }
        if l.hasPrefix("a=rtpmap:") {
            let p = l.dropFirst(9).split(separator: " ")
            if p.count >= 2, Int(p[0]) == s.payloadType {
                let f = p[1].split(separator: "/")
                if f.count >= 3 { s.encoding = String(f[0]); s.sampleRate = Int(f[1]) ?? 0; s.numChannels = Int(f[2]) ?? 0; format = true }
            }
        }
        if l.hasPrefix("a=source-filter:") {
            let p = l.dropFirst(16).split(separator: " ")
            if p.count >= 5 { s.sourceAddress = String(p[4]) }
        }
        if l.hasPrefix("a=ts-refclk:"), let r = l.range(of: "domain-nmbr=") { s.ptpDomain = Int(l[r.upperBound...]) ?? 0 }
        if l.hasPrefix("a=ts-refclk:ptp=IEEE1588-2008:"), let last = l.split(separator: ":").last, let pd = Int(last) { s.ptpDomain = pd }
        if l.hasPrefix("a=ptime:") { s.ptime = Int(l.dropFirst(8)) ?? 0 }
        if l.hasPrefix("a=framecount:") { s.framecount = Int(l.dropFirst(13)) ?? 48 }
    }
    s.direction = "recvonly" // Sender SDP may say sendonly.
    return media && format && !s.sessionName.isEmpty && multicast(s.connectionAddress) &&
        (1...65535).contains(s.port) && (1...8).contains(s.numChannels) && s.sampleRate == 48000 &&
        (s.encoding == "L16" || s.encoding == "L24") && s.ptime == 1 ? s : nil
}
func testAutomaticSetup() throws {
    func require(_ test: @autoclosure () -> Bool, _ message: String) throws {
        if !test() { throw SetupFailure(message: message) }
    }
    let ethernet = Port(name: "en7", ip: "192.168.1.20", wired: true)
    let wifi = Port(name: "en0", ip: "192.168.2.20")
    try require(automaticPort(saved: nil, ports: [wifi, ethernet]) == ethernet, "Single Ethernet not selected")
    try require(automaticPort(saved: nil, ports: [ethernet, Port(name: "en8", ip: "192.168.3.20", wired: true)]) == nil, "Multiple Ethernet adapters selected silently")
    try require(automaticPort(saved: "en9", ports: [wifi, ethernet]) == ethernet, "Replugged single Ethernet adapter not taken over")
    try require(automaticPort(saved: "en9", ports: [ethernet, Port(name: "en8", ip: "192.168.3.20", wired: true)]) == nil, "Missing saved adapter switched between several LANs")
    try require(automaticPort(saved: nil, ports: [wifi]) == nil && automaticPort(saved: wifi.name, ports: [wifi]) == nil, "Wi-Fi selected automatically")
    try require(suggestedSetupBase(ethernet) != suggestedSetupBase(Port(name: "en7", ip: "192.168.1.21", wired: true)), "Different Mac addresses share a default group")
    var source = SDP(); source.sessionName = "X32 actual flow 1"; source.numChannels = 8
    source.connectionAddress = "239.69.1.1"; source.port = 5012; source.payloadType = 98
    source.sourceAddress = "192.168.1.10"; source.originAddress = source.sourceAddress; source.sessionID = 71; source.ptpDomain = 7
    var second = source; second.sessionID = 72; second.connectionAddress = "239.69.1.2"; second.sessionName = "X32 actual flow 2"
    let ids = [sourceIdentity(source), sourceIdentity(second)]
    let sources = [ids[0]: source, ids[1]: second]
    for count in [4, 8, 16] {
        var original = Config(); original.easySetup = EasySetup(interface: ethernet.name, channels: count, selectedSources: ids)
        let result = try plannedSetup(original, channels: count, interface: ethernet, sources: sources, base: "239.69.250", now: 100)
        let repeated = try plannedSetup(result, channels: count, interface: ethernet, sources: sources, base: "239.69.250", now: 200)
        try require(result == repeated, "Repeated \(count) preset is not idempotent")
        try require(result.inputChannels == count && result.outputChannels == count, "Wrong preset device width")
        let rx = result.streams.filter { $0.sdp.direction == "recvonly" }, tx = result.streams.filter { $0.sdp.direction == "sendonly" }
        try require(tx.count == (count == 16 ? 2 : 1), "Wrong TX flow count")
        try require(rx.reduce(0) { $0 + $1.mapping.deviceChannelCount } == count, "Wrong RX mapping width")
        try require(rx.first?.sdp == source, "RX invented or changed advertised format")
        try require(tx.allSatisfy { $0.sdp.ptpDomain == 7 && $0.networkInterface == ethernet.name }, "TX did not use selected source clock domain")
        if count == 4 { try require(rx[0].mapping.channelMap == [0,1,2,3,-1,-1,-1,-1] && rx[0].mapping.streamChannelCount == 8, "4 of 8 RX mapping is unsafe") }
        if count == 16 { try require(rx[1].mapping.deviceChannelStart == 8 && rx[1].sdp == second, "16 channels did not use real second flow") }
        let missing = try plannedSetup(result, channels: count, interface: ethernet, sources: [:], base: "239.69.250", now: 300)
        try require(missing == result, "Missing announcements destroyed remembered configuration")
        let encoded = try JSONEncoder().encode(result)
        let decoded = try JSONDecoder().decode(Config.self, from: encoded)
        try require(decoded == result && decoded.easySetup?.selectedSources == ids, "Source choices not persisted")
        let small = try plannedSetup(result, channels: 4, interface: ethernet, sources: sources, base: "239.69.250", now: 400)
        try require(small.streams.count == 2 && small.streams.last?.mapping.deviceChannelCount == 4, "Preset shrink retained excess managed flows")
    }
    var original = Config(); original.easySetup = EasySetup(interface: ethernet.name, channels: 8)
    var withManual = try plannedSetup(original, channels: 8, interface: ethernet, sources: [:], base: "239.69.250", now: 100)
    var manual = withManual.streams[0]; manual.description = "User-owned stream"; manual.enabled = false
    withManual.streams.append(manual)
    let preserved = try plannedSetup(withManual, channels: 8, interface: ethernet, sources: [:], base: "239.69.250", now: 200)
    try require(preserved.streams.contains(manual), "Unmanaged stream lost")
    manual.enabled = true; withManual.streams[withManual.streams.count - 1] = manual
    do {
        _ = try plannedSetup(withManual, channels: 8, interface: ethernet, sources: [:], base: "239.69.250", now: 200)
        throw SetupFailure(message: "Unmanaged conflict was silently overwritten")
    } catch let failure as SetupFailure where failure.message != "Unmanaged conflict was silently overwritten" { }
    var automatic = Config(); automatic.easySetup = newEasySetup(ethernet)
    automatic = try plannedSetup(automatic, channels: 8, interface: ethernet, sources: [:], base: "239.69.20", now: 100)
    let legacyTest = localLoopbackConfig(inputChannels: 8, outputChannels: 8).streams[1]
    automatic.streams.append(legacyTest)
    let testDraft = macTestDraft(automatic)
    try require(testDraft.easySetup == nil && testDraft.streams == [legacyTest], "Auto streams blocked test TX or manual test stream was lost")
    try require(macTestDraft(testDraft) == testDraft, "Repeated test transition changed manual streams")
    try require(testPort(saved: wifi.name, ports: [wifi, ethernet]) == wifi, "Mac test could not explicitly use Wi-Fi")
    try require(testPort(saved: "en9", ports: [wifi, ethernet]) == nil, "Mac test silently changed disconnected adapter")
    try require(testPort(saved: nil, ports: [wifi]) == wifi, "Single-interface Mac test changed legacy behavior")
    let restored = try JSONDecoder().decode(Config.self, from: JSONEncoder().encode(automatic))
    try require(restored == automatic && restored.easySetup?.multicastBase == "239.69.20", "Automatic settings failed local-test backup restoration")
    var legacyNames = automatic
    for index in legacyNames.streams.indices where legacyNames.streams[index].description == setupTXMarker {
        legacyNames.streams[index].sdp.sessionName = "Mac → X32 · bestehend"
        legacyNames.streams[index].mapping.streamName = "Mac → X32 · bestehend"
    }
    // Remove the deliberate unmanaged test fixture before rebuilding production setup.
    legacyNames.streams.removeAll { !isSetupStream($0) }
    let upgraded = try plannedSetup(legacyNames, channels: 8, interface: ethernet, sources: [:], base: "239.69.20", now: 200)
    try require(upgraded.streams == legacyNames.streams, "Upgrade renamed or changed an existing published sender")
    try testConsoleSetup()
    print("AUTOMATIC_SETUP_SELF_TEST status=pass presets=4,8,16 idempotent=true real_sdp=true remembered_sources=true unmanaged_preserved=true console_devices=true")
}
func testConsoleSetup() throws {
    func require(_ test: @autoclosure () -> Bool, _ message: String) throws {
        if !test() { throw SetupFailure(message: message) }
    }
    func flow(_ name: String, _ origin: String, _ group: String, _ channels: Int = 8) -> SDP {
        var s = SDP(); s.sessionName = name; s.numChannels = channels; s.connectionAddress = group; s.port = 5004
        s.originAddress = origin; s.sourceAddress = origin; s.sessionID = UInt64(group.split(separator: ".").last.flatMap { UInt64($0) } ?? 1)
        return s
    }
    // Announcement order differs from flow order on purpose.
    let second = flow("X32-DANTE : 2", "192.168.0.50", "239.69.0.2"), first = flow("X32-DANTE : 1", "192.168.0.50", "239.69.0.1")
    let other = flow("Stagebox", "192.168.0.60", "239.69.9.1", 4)
    let sources = Dictionary(uniqueKeysWithValues: [second, other, first].map { (sourceIdentity($0), $0) })
    let devices = consoleDevices(sources)
    try require(devices.map(\.name) == ["Stagebox", "X32-DANTE"], "Consoles not grouped by origin or not named")
    guard let console = devices.first(where: { $0.address == "192.168.0.50" }) else { throw SetupFailure(message: "Console missing") }
    try require(console.sources == [sourceIdentity(first), sourceIdentity(second)] && console.channels == 16, "Console flows not in flow order")
    try require(consoleName(["X32 actual flow 1", "X32 actual flow 2"]) == "X32 actual flow" && consoleName(["A", "B"]) == nil, "Console name fallback wrong")
    let ethernet = Port(name: "en18", ip: "192.168.0.125", wired: true, mask: "255.255.255.0")
    var settings = selectDevice(EasySetup(interface: ethernet.name), console)
    try require(settings.channels == 16 && settings.device == console.address, "Choosing the console did not size the profile")
    var original = Config(); original.easySetup = settings
    let planned = try plannedSetup(original, channels: settings.channels, interface: ethernet, sources: sources, base: "239.69.125")
    let rx = planned.streams.filter { $0.sdp.direction == "recvonly" }
    try require(rx.map(\.sdp.sessionName) == ["X32-DANTE : 1", "X32-DANTE : 2"] && rx.map(\.mapping.deviceChannelStart) == [0, 8],
                "Console flows not mapped to Logic inputs 1–16")
    try require(planned.streams.filter { $0.sdp.direction == "sendonly" }.count == 2, "Console setup did not create both senders")
    // A third flow appears later; the stagebox flow is never mixed in.
    let third = flow("X32-DANTE : 3", "192.168.0.50", "239.69.0.3")
    var grown = sources; grown[sourceIdentity(third)] = third
    settings = followDevice(settings, devices: consoleDevices(grown))
    try require(settings.selectedSources.count == 3 && !settings.selectedSources.contains(sourceIdentity(other)), "New console flow not followed")
    try require(followDevice(settings, devices: []) == settings, "Missing announcements dropped the remembered console")
    // Dante picks AES67 groups from 239.69.x.x as well.
    try require(freeSetupBase("239.69.125", channels: 16, taken: ["239.69.0.1"]) == "239.69.125", "Free base changed")
    try require(freeSetupBase("239.69.125", channels: 8, taken: ["239.69.125.1"]) == "239.69.126", "Collision with console flow not avoided")
    try require(freeSetupBase("239.69.125", channels: 16, taken: ["239.69.125.2", "239.69.126.1"]) == "239.69.127", "Second sender group not checked")
    try require(freeSetupBase("239.69.254", channels: 8, taken: ["239.69.254.1"]) == "239.69.1", "Base search did not wrap")
    // Field failure 22.10.2026: replugged adapter without DHCP.
    try require(networkWarning(port: Port(name: "en18", ip: "169.254.170.30", wired: true, mask: "255.255.0.0"), console: "192.168.0.50") != nil, "Link-local Mac next to DHCP console not flagged")
    try require(networkWarning(port: Port(name: "en18", ip: "169.254.170.30", wired: true, mask: "255.255.0.0"), console: "169.254.20.7") == nil, "Direct link-local cable flagged")
    try require(networkWarning(port: ethernet, console: "192.168.0.50") == nil, "Same subnet flagged")
    try require(networkWarning(port: ethernet, console: "10.0.0.5") != nil, "Different subnet not flagged")
    try require(networkWarning(port: Port(name: "en5", ip: "10.1.2.3", wired: true, mask: "255.255.0.0"), console: "10.1.9.9") == nil, "Netmask ignored")
}
@main struct Main {
    // NSApplication.delegate is non-owning. Keep the delegate alive for the
    // entire event loop; otherwise an optimized build can drop the local.
    static var retainedManager: Manager?
    static func main() {
        if CommandLine.arguments.count == 3 && CommandLine.arguments[1] == "--self-test-receiver" {
            let helper = URL(fileURLWithPath: CommandLine.arguments[2])
            guard FileManager.default.isExecutableFile(atPath: helper.path), let interface = interfaces().first?.name else {
                fputs("Receiver self-test needs an executable helper and an active IPv4 interface.\n", stderr); exit(2)
            }
            let process = Process(), stdout = Pipe(), stderrPipe = Pipe(), drain = DispatchGroup()
            process.executableURL = helper
            process.arguments = ["--ip", "239.69.100.1", "--port", "5004", "--channels", "2", "--encoding", "L24", "--interface", interface, "--duration", "1"]
            process.standardOutput = stdout.fileHandleForWriting; process.standardError = stderrPipe.fileHandleForWriting
            var outputData = Data(), errorData = Data()
            drain.enter(); DispatchQueue.global().async { outputData = stdout.fileHandleForReading.readDataToEndOfFile(); drain.leave() }
            drain.enter(); DispatchQueue.global().async { errorData = stderrPipe.fileHandleForReading.readDataToEndOfFile(); drain.leave() }
            do {
                try process.run()
                try? stdout.fileHandleForWriting.close(); try? stderrPipe.fileHandleForWriting.close()
                process.waitUntilExit(); drain.wait()
            } catch {
                try? stdout.fileHandleForWriting.close(); try? stderrPipe.fileHandleForWriting.close()
                drain.wait(); fputs("Receiver self-test could not start: \(error)\n", stderr); exit(3)
            }
            let result = String(data: outputData, encoding: .utf8) ?? ""
            let diagnostics = String(data: errorData, encoding: .utf8) ?? ""
            let valid = result.split(whereSeparator: \.isNewline).contains { line in
                line.hasPrefix("RESULT status=") && line.contains(" packets=") && line.contains(" duration_ms=") && line.contains(" channel1_peak=")
            }
            if valid { print(result, terminator: ""); if !diagnostics.isEmpty { fputs(diagnostics, stderr) }; return }
            fputs("Receiver self-test did not receive a parseable RESULT line.\n\(result)\(diagnostics)", stderr); exit(4)
        }
        if CommandLine.arguments.count == 3 && CommandLine.arguments[1] == "--self-test" {
            let probe = "v=0\ns=Probe\nc=IN IP4 239.69.1.1/32\nm=audio 5004 RTP/AVP 96\na=rtpmap:96 L24/48000/8\na=ptime:1\na=ts-refclk:ptp=IEEE1588-2008:00-11-22-ff-fe-33-44-55:7\n"
            guard importSession(probe)?.ptpDomain == 7,
                  importSession(probe.replacingOccurrences(of: "48000", with: "44100")) == nil,
                  !multicast("239.999.1.1") else { exit(2) }
            let prepared = preparedTXStreams(channels: 16, interface: Port(name: "en0", ip: "192.168.1.20"), existing: [])
            guard prepared?.count == 2, prepared?.map({ $0.sdp.connectionAddress }) == ["239.69.250.1", "239.69.250.2"],
                  Set(prepared?.map({ $0.sdp.sessionID }) ?? []).count == 2,
                  prepared?.allSatisfy({ $0.sdp.direction == "sendonly" && $0.sdp.numChannels == 8 && $0.sdp.encoding == "L24" && $0.sdp.sampleRate == 48000 && $0.sdp.ptpDomain == 0 && $0.sdp.originAddress == "192.168.1.20" }) == true,
                  preparedTXStreams(channels: 8, interface: Port(name: "en0", ip: "192.168.1.20"), existing: [prepared![0]]) == nil,
                  preparedTXStreams(channels: 12, interface: Port(name: "en0", ip: "192.168.1.20"), existing: []) == nil else { exit(7) }
            do { try testAutomaticSetup() }
            catch { fputs("Automatic setup self-test failed: \(error)\n", stderr); exit(8) }
            let now = UInt64(Date().timeIntervalSince1970)
            func sample(_ name: String, _ direction: String, _ start: Int) -> Stream {
                var s = SDP(); s.sessionName = name; s.direction = direction
                s.connectionAddress = direction == "sendonly" ? "239.69.83.172" : "239.69.83.171"
                s.numChannels = direction == "sendonly" ? 8 : 4
                s.originAddress = "192.168.1.10"; s.sourceAddress = "192.168.1.10"; s.sessionID = now
                let m = Mapping(streamID: UUID().uuidString.lowercased(), streamName: name,
                                streamChannelCount: s.numChannels, deviceChannelStart: start, deviceChannelCount: s.numChannels)
                return Stream(createdTimestamp: now, modifiedTimestamp: now, networkInterface: "en0", sdp: s, mapping: m)
            }
            let value = Config(inputChannels: 4, outputChannels: 8,
                               streams: [sample("RX Test", "recvonly", 0), sample("TX Test", "sendonly", 0)])
            guard let encoded = try? JSONEncoder().encode(value),
                  let roundTrip = try? JSONDecoder().decode(Config.self, from: encoded),
                  roundTrip.inputChannels == 4, roundTrip.outputChannels == 8,
                  let legacy = try? JSONDecoder().decode(Config.self, from: Data("{\"version\":\"1.0\",\"streams\":[]}".utf8)),
                  legacy.inputChannels == 32, legacy.outputChannels == 32 else { exit(3) }
            let local = localLoopbackConfig(inputChannels: 6, outputChannels: 4)
            guard local.streams.count == 2, local.inputChannels == 6, local.outputChannels == 4,
                  local.streams.allSatisfy({ $0.networkInterface == "lo0" && $0.sdp.mediaClockType == "local-test" &&
                      $0.sdp.connectionAddress == "239.69.100.2" && $0.sdp.port == 5006 && $0.sdp.numChannels == 2 &&
                      $0.mapping.deviceChannelStart == 0 && $0.mapping.deviceChannelCount == 2 }),
                  local.streams.contains(where: { $0.sdp.direction == "sendonly" && $0.sdp.sourceAddress == "127.0.0.1" }),
                  local.streams.contains(where: { $0.sdp.direction == "recvonly" }),
                  let localData = try? JSONEncoder().encode(local),
                  let localRoundTrip = try? JSONDecoder().decode(Config.self, from: localData),
                  localRoundTrip.streams.count == 2, localRoundTrip.streams.map(\.networkInterface) == ["lo0", "lo0"] else { exit(4) }
            let backupFolder = FileManager.default.temporaryDirectory.appendingPathComponent("aes67-local-test-\(UUID().uuidString)", isDirectory: true)
            let backupURL = backupFolder.appendingPathComponent("before-local-test.json")
            do {
                try FileManager.default.createDirectory(at: backupFolder, withIntermediateDirectories: true)
                let originalDraft = Config(inputChannels: 1, outputChannels: 1, streams: [])
                try backupConfigIfMissing(originalDraft, at: backupURL)
                let originalBackup = try Data(contentsOf: backupURL)
                var later = local; later.inputChannels = 12
                try backupConfigIfMissing(later, at: backupURL)
                guard try Data(contentsOf: backupURL) == originalBackup,
                      let restored = try? JSONDecoder().decode(Config.self, from: originalBackup),
                      restored.inputChannels == 1, restored.outputChannels == 1 else { exit(5) }
                try FileManager.default.removeItem(at: backupFolder)
            } catch { fputs("Local-test backup self-test failed: \(error)\n", stderr); exit(6) }
            do { try JSONEncoder().encode(value).write(to: URL(fileURLWithPath: CommandLine.arguments[2]), options: .atomic) }
            catch { fputs("Self-test write failed: \(error)\n", stderr); exit(1) }
            return
        }
        let a = NSApplication.shared
        a.setActivationPolicy(.regular)
        let manager = Manager()
        retainedManager = manager
        a.delegate = manager
        a.run()
    }
}
// What the window showed and every message dialog, for field reports:
// ~/Library/Logs/AES67Manager.log, one previous generation at 1 MB.
enum ManagerLog {
    static var enabled = false
    static let url = FileManager.default.homeDirectoryForCurrentUser.appendingPathComponent("Library/Logs/AES67Manager.log")
    private static var last = [String: String]()
    private static let stamp: DateFormatter = { let f = DateFormatter(); f.dateFormat = "yyyy-MM-dd HH:mm:ss"; return f }()
    static func write(_ message: String) {
        guard enabled else { return }
        let fm = FileManager.default
        if let size = (try? fm.attributesOfItem(atPath: url.path))?[.size] as? Int, size > 1_000_000 {
            let previous = url.deletingPathExtension().appendingPathExtension("1.log")
            try? fm.removeItem(at: previous); try? fm.moveItem(at: url, to: previous)
        }
        let line = Data("[\(stamp.string(from: Date()))] \(message.replacingOccurrences(of: "\n", with: " | "))\n".utf8)
        if let handle = try? FileHandle(forWritingTo: url) {
            _ = try? handle.seekToEnd(); try? handle.write(contentsOf: line); try? handle.close()
        } else {
            try? fm.createDirectory(at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
            try? line.write(to: url)
        }
    }
    // Repeated status refreshes log only what changed.
    static func change(_ key: String, _ message: String) {
        guard last[key] != message else { return }
        last[key] = message
        if !message.isEmpty { write(message) }
    }
}
final class SetupDocumentView: NSView {
    override var isFlipped: Bool { true }
}
final class WrappingLabel: NSTextField {
    override func layout() {
        super.layout()
        let width = max(80, bounds.width)
        if abs(preferredMaxLayoutWidth - width) > 1 {
            preferredMaxLayoutWidth = width
            invalidateIntrinsicContentSize()
        }
    }
}

final class Manager: NSObject, NSApplicationDelegate, NSTableViewDataSource, NSTableViewDelegate {
    private let startupSelfTest = CommandLine.arguments.contains("--self-test-window")
    // --snapshot-window <png> [tab] [--demo-console]: render the window to a PNG
    // without screen-recording rights. Never writes drafts or starts discovery.
    private let snapshot: (path: String, tab: Int, demo: Bool)? = {
        let a = CommandLine.arguments
        guard let i = a.firstIndex(of: "--snapshot-window"), a.count > i + 1 else { return nil }
        return (a[i + 1], a.count > i + 2 ? Int(a[i + 2]) ?? 0 : 0, a.contains("--demo-console"))
    }()
    private var headless: Bool { startupSelfTest || snapshot != nil }
    var tabs: NSTabView!
    let stage = FileManager.default.homeDirectoryForCurrentUser.appendingPathComponent("Library/Application Support/AES67Driver/streams.json")
    let system = URL(fileURLWithPath: "/Library/Application Support/AES67Driver/streams.json")
    let driver = "/Library/Audio/Plug-Ins/HAL/AES67Driver.driver"
    var config = Config(), ports = [Port]()
    // Only suppress a repeat activation after this process observed a successful reload.
    var lastActivatedData: Data?
    var window: NSWindow!, table: NSTableView!, status: NSTextField!, receiveCheckButton: NSButton!, receiverTestProcess: Process?, statusTimer: Timer?
    var network: NSPopUpButton!, sourcePicker: NSPopUpButton!, sourceSummary: NSTextField!, testInterface: NSPopUpButton!, testSummary: NSTextField!
    var devicePicker: NSPopUpButton!, deviceSummary: NSTextField!, portStatusLabel: NSTextField!
    var portNotice = "", deviceNotice = "", addressNotice = ""
    var autoSelectDeclined = false   // after "Quellenauswahl zurücksetzen", for this session
    var clockStatusLabel: NSTextField!, configStatusLabel: NSTextField!, nextStepLabel: NSTextField!
    var sessionInterface: String?
    var discovered = [String: (sdp: SDP, seen: Date)]()
    var discoveryProcess: Process?, discoveryDirectory: URL?, discoveryInterface: Port?
    var nextDiscovery = Date.distantPast
    var discoveryMessage = "", setupMessage = ""
    var chosenInterfaceName: String? {
        if let saved = config.easySetup?.interface { return saved }
        if let sessionInterface { return sessionInterface }
        let names = Set(config.streams.filter { $0.sdp.mediaClockType != "local-test" }.map(\.networkInterface))
        return names.count == 1 ? names.first : nil
    }
    var chosenPort: Port? { automaticPort(saved: chosenInterfaceName, ports: ports) }
    var availableSources: [String: SDP] { discovered.mapValues { $0.sdp } }
    var mode: NSPopUpButton!, profile: NSPopUpButton!, iface: NSPopUpButton!, name: NSTextField!, group: NSTextField!, port: NSTextField!, channels: NSTextField!, start: NSTextField!, payload: NSTextField!, domain: NSTextField!, inputCount: NSTextField!, outputCount: NSTextField!, setupSummary: NSTextField!, txBaseField: NSTextField!
    var selected: Int { table.selectedRow }
    func applicationShouldTerminateAfterLastWindowClosed(_ app: NSApplication) -> Bool { true }
    func applicationWillTerminate(_ notification: Notification) { statusTimer?.invalidate(); receiverTestProcess?.terminate(); stopDiscovery() }
    func label(_ v: String) -> NSTextField {
        let t = WrappingLabel(wrappingLabelWithString: v)
        t.font = .systemFont(ofSize: 12); t.lineBreakMode = .byWordWrapping; t.maximumNumberOfLines = 0
        t.preferredMaxLayoutWidth = 840
        t.setContentHuggingPriority(.defaultLow, for: .horizontal)
        t.setContentCompressionResistancePriority(.defaultLow, for: .horizontal)
        return t
    }
    func heading(_ v: String) -> NSTextField { let t = label(v); t.font = .boldSystemFont(ofSize: 14); t.textColor = .labelColor; return t }
    func field(_ v: String, _ width: CGFloat) -> NSTextField { let t = NSTextField(string: v); t.widthAnchor.constraint(equalToConstant: width).isActive = true; return t }
    func button(_ t: String, _ a: Selector) -> NSButton { NSButton(title: t, target: self, action: a) }
    func row(_ v: [NSView]) -> NSStackView { let s = NSStackView(views: v); s.spacing = 8; s.alignment = .centerY; return s }
    func alert(_ t: String, _ d: String) { ManagerLog.write("Meldung: \(t) – \(d)"); let a = NSAlert(); a.messageText = t; a.informativeText = d; a.runModal() }
    func applicationDidFinishLaunching(_ n: Notification) {
        ManagerLog.enabled = !headless
        ManagerLog.write("AES67 Manager \(Bundle.main.infoDictionary?["CFBundleShortVersionString"] as? String ?? "?") build \(Bundle.main.infoDictionary?["CFBundleVersion"] as? String ?? "?") gestartet · \(Bundle.main.bundlePath)")
        installStandardMenus()
        if let icon = Bundle.main.url(forResource: "AppIcon", withExtension: "icns"), let image = NSImage(contentsOf: icon) {
            NSApp.applicationIconImage = image
        }
        window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 880, height: 780), styleMask: [.titled, .closable, .miniaturizable, .resizable], backing: .buffered, defer: false)
        window.title = "AES67 Manager · " + (Bundle.main.infoDictionary?["CFBundleShortVersionString"] as? String ?? "Entwicklung"); window.center(); window.minSize = NSSize(width: 720, height: 560)
        let content = window.contentView!
        let scroll = NSScrollView(); scroll.translatesAutoresizingMaskIntoConstraints = false; scroll.hasVerticalScroller = true; scroll.autohidesScrollers = true; scroll.borderType = .noBorder
        let document = SetupDocumentView(); document.translatesAutoresizingMaskIntoConstraints = false
        let root = NSStackView(); root.orientation = .vertical; root.alignment = .leading; root.spacing = 12; root.translatesAutoresizingMaskIntoConstraints = false
        document.addSubview(root); scroll.documentView = document; content.addSubview(scroll)
        let actions = NSStackView(views: [button("Entwurf speichern", #selector(save)), button("Änderungen anwenden …", #selector(apply))]); actions.orientation = .horizontal; actions.alignment = .centerY; actions.spacing = 10; actions.translatesAutoresizingMaskIntoConstraints = false
        content.addSubview(actions)
        NSLayoutConstraint.activate([
            actions.leadingAnchor.constraint(equalTo: content.leadingAnchor, constant: 22), actions.trailingAnchor.constraint(lessThanOrEqualTo: content.trailingAnchor, constant: -22),
            actions.bottomAnchor.constraint(equalTo: content.bottomAnchor, constant: -16),
            scroll.leadingAnchor.constraint(equalTo: content.leadingAnchor), scroll.trailingAnchor.constraint(equalTo: content.trailingAnchor),
            scroll.topAnchor.constraint(equalTo: content.topAnchor), scroll.bottomAnchor.constraint(equalTo: actions.topAnchor, constant: -12),
            document.widthAnchor.constraint(equalTo: scroll.contentView.widthAnchor),
            root.leadingAnchor.constraint(equalTo: document.leadingAnchor, constant: 22), root.trailingAnchor.constraint(equalTo: document.trailingAnchor, constant: -22),
            root.topAnchor.constraint(equalTo: document.topAnchor, constant: 20), root.bottomAnchor.constraint(equalTo: document.bottomAnchor, constant: -20),
            root.widthAnchor.constraint(equalTo: document.widthAnchor, constant: -44)
        ])
        let title = label("AES67 Manager"); title.font = .boldSystemFont(ofSize: 22); root.addArrangedSubview(title)
        let subtitle = label("Audio zwischen Mac und AES67-Pult verbinden"); subtitle.font = .systemFont(ofSize: 13); subtitle.textColor = .secondaryLabelColor; root.addArrangedSubview(subtitle)
        let version = Bundle.main.infoDictionary?["CFBundleShortVersionString"] as? String ?? "?"
        let build = Bundle.main.infoDictionary?["CFBundleVersion"] as? String ?? "?"
        let identity = label("Version \(version) · Build \(build) · \(Bundle.main.bundlePath)")
        identity.font = .systemFont(ofSize: 10); identity.textColor = .secondaryLabelColor
        identity.isSelectable = true; root.addArrangedSubview(identity)
        inputCount = field("32", 42); outputCount = field("32", 42)
        let channelRow = row([label("Audiokanäle (1–32):"), inputCount, label("Eingänge ×"), outputCount, label("Ausgänge")])
        status = label(""); status.font = .systemFont(ofSize: 12); status.textColor = .secondaryLabelColor
        clockStatusLabel = label(""); configStatusLabel = label(""); nextStepLabel = heading("")
        let health = NSStackView(views: [heading("Verbindungsstatus"), status, clockStatusLabel, configStatusLabel, nextStepLabel])
        health.orientation = .vertical; health.alignment = .leading; health.spacing = 8
        health.edgeInsets = NSEdgeInsets(top: 14, left: 16, bottom: 14, right: 16)
        health.wantsLayer = true; health.layer?.cornerRadius = 10
        health.layer?.backgroundColor = NSColor.controlBackgroundColor.cgColor
        root.addArrangedSubview(health); health.widthAnchor.constraint(equalTo: root.widthAnchor).isActive = true
        for view in health.arrangedSubviews { view.widthAnchor.constraint(equalTo: health.widthAnchor, constant: -32).isActive = true }
        let listScroll = NSScrollView(); listScroll.hasVerticalScroller = true; listScroll.borderType = .bezelBorder; listScroll.heightAnchor.constraint(equalToConstant: 150).isActive = true
        table = NSTableView(); table.delegate = self; table.dataSource = self; table.headerView = NSTableHeaderView()
        for (h, w) in [("Richtung", 80.0), ("Name", 230.0), ("Multicast", 145.0), ("Kanäle", 75.0), ("Logic", 80.0)] { let c = NSTableColumn(identifier: NSUserInterfaceItemIdentifier(h)); c.title = h; c.width = w; table.addTableColumn(c) }
        listScroll.documentView = table
        let streamActions = row([button("SDP importieren …", #selector(importSDP)), button("SDP exportieren …", #selector(exportSDP)), button("Entfernen", #selector(remove)), button("SAP suchen", #selector(discover))])
        mode = NSPopUpButton(); mode.addItems(withTitles: ["Empfang / RX", "Wiedergabe / TX"])
        profile = NSPopUpButton(); profile.addItems(withTitles: ["Pult / AES67 (PTP erforderlich)", "Mac-Test (ohne PTP)", "Dieser Mac (Rückweg zu Eingang 1–2)"]); profile.target = self; profile.action = #selector(profileChanged)
        iface = NSPopUpButton(); name = field("", 180); group = field("", 135); port = field("5004", 60)
        channels = field("8", 40); start = field("1", 40); payload = field("96", 40); domain = field("0", 40)
        receiveCheckButton = button("Ausgewählten Empfang 10 Sekunden prüfen", #selector(checkSelectedReceive))
        let editorViews: [NSView] = [heading("Stream bearbeiten"), label("Feldänderungen zuerst mit „Ausgewählten Stream übernehmen“ in den Entwurf übernehmen. Danach unten anwenden."), row([label("Profil:"), profile]), row([label("Richtung:"), mode]), row([label("Netzwerkanschluss:"), iface, button("Aktualisieren", #selector(refreshPorts))]), row([label("Streamname:"), name]), row([label("Multicast-Adresse:"), group, label("RTP-Port:"), port]), row([label("Kanäle (1–8):"), channels, label("Logic-Start:"), start]), row([label("Payload-Typ:"), payload, label("PTP-Domain:"), domain]), row([button("Stream hinzufügen", #selector(add)), button("Ausgewählten Stream übernehmen", #selector(updateSelected))])]
        testInterface = NSPopUpButton()
        testSummary = label("")
        let testViews: [NSView] = [heading("Audio auf diesem Mac testen"),
            label("Für Musik → Aufnahme auf demselben Mac ist ein Sender plus Empfänger nötig. Dieser Test verwendet lo0 und braucht keinen Netzwerkadapter oder PTP."),
            row([button("Auf diesem Mac vorbereiten …", #selector(prepareLocalLoopback)), button("Vorherigen Entwurf laden", #selector(restoreBeforeLocalTest))]),
            label("Danach Änderungen anwenden, AES67 als Musikausgabe und GarageBand-Eingang wählen. GarageBand-Ausgang auf Lautsprecher stellen und Software-Monitoring ausschalten."),
            testSummary, receiveCheckButton,
            heading("Audio zwischen zwei Macs testen"),
            label("Nur für zwei verschiedene Macs im selben Netzwerk: auf einem Mac Sender, auf dem anderen Empfänger vorbereiten. Beide verwenden 239.69.100.1:5004, 2 Kanäle, 48 kHz und L24 ohne PTP."),
            row([label("Test-Netzwerkanschluss:"), testInterface, button("Aktualisieren", #selector(refreshPorts))]),
            row([button("Empfänger vorbereiten", #selector(addTestReceiver)), button("Sender vorbereiten", #selector(addTestSender))]),
            label("Beim Wechsel zum Zwei-Mac-Test werden automatisch erzeugte Pult-Streams im Entwurf entfernt und einmalig gesichert; manuelle Streams bleiben erhalten. Systemweit ändert sich erst mit „Änderungen anwenden …“ etwas."),
            label("Die Empfangsprüfung misst RTP-Pakete und Audiopegel. Sie bestätigt keinen PTP-Lock oder vollständige AES67-Kompatibilität.")]
        let helpViews: [NSView] = [heading("Clock-Dienst und Sendererkennung"),
            label("Der Pult-Takt wird per PTP empfangen. Der lokale Clock-Dienst synchronisiert den Mac darauf. Ohne aktuellen PTP-Lock und aktives Audio-I/O erscheinen die Mac-Sender nicht im Dante Controller."),
            label("Kein aktueller Clock-Status: Die Datei /Library/LaunchDaemons/org.communityaudio.aes67.clock.plist und die launchd-Protokolle prüfen. Eine Download-Quarantäne auf dieser Datei kann den Dienststart blockieren. Ein Neustart allein entfernt sie nicht. Der Manager verändert keine Sicherheitsmarkierungen."),
            heading("Audio prüfen"), label("Die Statusanzeige bestätigt Konfiguration und Clock. Ob Audio am Pult ankommt, mit einem Testsignal und dem Pult-Pegel prüfen."), heading("Hinweise"), label("Die Kanalzahlen bestimmen, was Logic Pro am AES67-Gerät anzeigt. X-DANTE-Kanäle werden separat im Dante Controller zugeordnet."), label("Format: 48 kHz, L24, 1 ms. Im Pult-Modus benötigt TX eine PTP-Clock. Für Musik am sendenden Mac „AES67 Device“ als Tonausgabe wählen."), label("Bei mehreren Macs kann RX dieselbe Multicast-Adresse verwenden. Jeder TX-Sender benötigt eine eigene Adresse."), label("„Entwurf speichern“ sichert Änderungen nur für dich. „Änderungen anwenden …“ installiert sie für den Treiber und startet den Audiodienst kurz neu.")]
        network = NSPopUpButton(); network.target = self; network.action = #selector(networkChanged)
        setupSummary = label("")
        let presetRow = row([label("Kanalprofil:"), button("4 × 4", #selector(preset4)), button("8 × 8", #selector(preset8)), button("16 × 16", #selector(preset16))])
        let guidedImport = button("SDP-Datei importieren …", #selector(guidedImportSDP))
        txBaseField = field("239.69.250", 120)
        sourcePicker = NSPopUpButton()
        sourceSummary = label("")
        devicePicker = NSPopUpButton(); devicePicker.target = self; devicePicker.action = #selector(deviceChanged)
        deviceSummary = label(""); portStatusLabel = label("")
        let settingsViews: [NSView] = [heading("1 · Anschluss zum Pult"),
            row([label("Netzwerkanschluss:"), network, button("Aktualisieren", #selector(refreshPorts))]), portStatusLabel,
            heading("2 · Pult auswählen"),
            label("Gefundene AES67-Pulte erscheinen von selbst. Die Auswahl legt den Empfang aller Pult-Flows und die Mac-Sender an."),
            row([devicePicker, button("Erneut suchen", #selector(guidedDiscover))]), deviceSummary,
            heading("3 · Kanäle"), presetRow, setupSummary,
            heading("4 · Anwenden und im Dante Controller routen"),
            label("„Änderungen anwenden …“ aktiviert die Streams. Sobald der PTP-Takt steht, erscheinen die Mac-Sender („Mac → Pult …“) im Dante Controller und lassen sich dort auf Pult-Eingänge routen. Der Manager darf danach geschlossen werden.")]
        let sourceViews: [NSView] = [heading("Pult-Quellen einzeln wählen"),
            label("Nur nötig, wenn nicht alle Flows eines Pults gewünscht sind oder ein Flow ohne SAP-Ankündigung per SDP-Datei kommt. Eine Einzelauswahl ersetzt die Pult-Auswahl."),
            row([sourcePicker, button("Quelle hinzufügen", #selector(selectSource))]),
            row([button("Quellenauswahl zurücksetzen", #selector(clearSources)), guidedImport]), sourceSummary]
        tabs = NSTabView(); tabs.translatesAutoresizingMaskIntoConstraints = false
        for (tabTitle, views) in [("Einstellungen", settingsViews), ("Erweitert", sourceViews + [heading("Streams und Kanäle"), channelRow, row([label("TX-Multicast-Basis:"), txBaseField, button("Automatik aktualisieren", #selector(prepareTX))]), label("Die vorgeschlagene Basis wird aus der lokalen IP abgeleitet und gespeichert. Sie ist keine Adressreservierung; besonders in größeren Subnetzen mit gleichen IP-Endziffern brauchen weitere Macs eine andere freie Basis."), listScroll, streamActions] + editorViews), ("Mac-Test", testViews), ("Hinweise", helpViews)] {
            let item = NSTabViewItem(identifier: tabTitle); item.label = tabTitle
            let page = NSScrollView(); page.hasVerticalScroller = true; page.autohidesScrollers = true; page.borderType = .noBorder
            let body = SetupDocumentView(); body.translatesAutoresizingMaskIntoConstraints = false
            let stack = NSStackView(views: views); stack.orientation = .vertical; stack.alignment = .leading; stack.spacing = 16; stack.translatesAutoresizingMaskIntoConstraints = false
            body.addSubview(stack); page.documentView = body
            NSLayoutConstraint.activate([body.widthAnchor.constraint(equalTo: page.contentView.widthAnchor),
                stack.leadingAnchor.constraint(equalTo: body.leadingAnchor, constant: 18), stack.trailingAnchor.constraint(equalTo: body.trailingAnchor, constant: -18),
                stack.topAnchor.constraint(equalTo: body.topAnchor, constant: 24), stack.bottomAnchor.constraint(equalTo: body.bottomAnchor, constant: -24)])
            for view in views where view is NSTextField || view === listScroll { view.widthAnchor.constraint(equalTo: stack.widthAnchor).isActive = true }
            item.view = page; tabs.addTabViewItem(item)
        }
        root.addArrangedSubview(tabs)
        tabs.widthAnchor.constraint(equalTo: root.widthAnchor).isActive = true
        tabs.heightAnchor.constraint(equalToConstant: 420).isActive = true
        root.addArrangedSubview(label("Anwenden benötigt Administratorrechte und unterbricht Audio auf dem gesamten Mac kurz."))
        for view in root.arrangedSubviews where view is NSTextField { view.widthAnchor.constraint(equalTo: root.widthAnchor).isActive = true }
        ports = interfaces(); refreshInterfaceMenu(); group.stringValue = suggestedTXGroup(); load()
        if snapshot?.demo == true {
            // Example setup: one USB Ethernet adapter and a console with two AES67 flows.
            ports = [CommandLine.arguments.contains("--demo-link-local")
                ? Port(name: "en18", ip: "169.254.170.30", wired: true, mask: "255.255.0.0")
                : Port(name: "en18", ip: "192.168.0.125", wired: true, mask: "255.255.255.0")]; config = Config()
            for flow in 1...2 where !CommandLine.arguments.contains("--demo-no-flows") {
                var s = SDP(); s.sessionName = "X32-DANTE : \(flow)"; s.numChannels = 8
                s.connectionAddress = CommandLine.arguments.contains("--demo-collision") ? "239.69.125.\(flow)" : "239.69.0.\(flow)"
                s.originAddress = "192.168.0.50"; s.sourceAddress = s.originAddress; s.sessionID = UInt64(flow)
                discovered[sourceIdentity(s)] = (sdp: s, seen: Date())
            }
        }
        if !startupSelfTest, config.streams.isEmpty && config.easySetup == nil, let chosen = automaticPort(saved: nil, ports: ports) {
            config.easySetup = newEasySetup(chosen)
            _ = rebuildSetup(reportErrors: false)
        }
        if snapshot?.demo == true { collectDiscovery() }
        refreshNetworkMenu(); refreshTestInterfaceMenu(); updateStatus(); serviceDiscovery()
        if snapshot == nil { statusTimer = Timer.scheduledTimer(withTimeInterval: 5, repeats: true) { [weak self] _ in self?.refreshPorts() } }
        window.makeKeyAndOrderFront(nil); NSApp.activate(ignoringOtherApps: true)
        if let snapshot {
            tabs.selectTabViewItem(at: min(snapshot.tab, tabs.numberOfTabViewItems - 1))
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) { [weak self] in
                guard let self, let view = self.window.contentView, let rep = view.bitmapImageRepForCachingDisplay(in: view.bounds) else { exit(12) }
                view.cacheDisplay(in: view.bounds, to: rep)
                guard let png = rep.representation(using: .png, properties: [:]), (try? png.write(to: URL(fileURLWithPath: snapshot.path))) != nil else { exit(13) }
                NSApp.terminate(nil)
            }
        }
        if startupSelfTest {
            // Check after AppKit has had an event-loop turn to order and lay
            // out the window. This exercises the real delegate startup path.
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.25) { [weak self] in
                guard let self else { fputs("WINDOW_SELF_TEST status=fail reason=delegate_released\n", stderr); exit(10) }
                let frame = self.window.frame
                let visible = self.window.isVisible && frame.width > 0 && frame.height > 0 && frame.width <= 1100
                let content = self.window.contentView?.bounds ?? .zero
                if visible && content.width > 0 && content.height > 0 {
                    print("WINDOW_SELF_TEST status=pass visible=true frame=\(Int(frame.width))x\(Int(frame.height)) content=\(Int(content.width))x\(Int(content.height))")
                    fflush(stdout)
                    NSApp.terminate(nil)
                } else {
                    fputs("WINDOW_SELF_TEST status=fail visible=\(self.window.isVisible) frame=\(frame) content=\(content)\n", stderr)
                    fflush(stderr)
                    exit(11)
                }
            }
        }
    }
    func installStandardMenus() {
        let main = NSMenu()
        let appItem = NSMenuItem(); main.addItem(appItem)
        let appMenu = NSMenu(); appItem.submenu = appMenu
        appMenu.addItem(withTitle: "AES67 Manager beenden", action: #selector(NSApplication.terminate(_:)), keyEquivalent: "q")
        let editItem = NSMenuItem(); main.addItem(editItem)
        let editMenu = NSMenu(title: "Bearbeiten"); editItem.submenu = editMenu
        editMenu.addItem(withTitle: "Rückgängig", action: NSSelectorFromString("undo:"), keyEquivalent: "z")
        editMenu.addItem(withTitle: "Wiederholen", action: NSSelectorFromString("redo:"), keyEquivalent: "Z")
        editMenu.addItem(NSMenuItem.separator())
        editMenu.addItem(withTitle: "Ausschneiden", action: #selector(NSText.cut(_:)), keyEquivalent: "x")
        editMenu.addItem(withTitle: "Kopieren", action: #selector(NSText.copy(_:)), keyEquivalent: "c")
        editMenu.addItem(withTitle: "Einsetzen", action: #selector(NSText.paste(_:)), keyEquivalent: "v")
        editMenu.addItem(withTitle: "Alles auswählen", action: #selector(NSText.selectAll(_:)), keyEquivalent: "a")
        NSApp.mainMenu = main
    }
    func refreshNetworkMenu() {
        if chosenInterfaceName == nil, let automatic = automaticPort(saved: nil, ports: ports) { sessionInterface = automatic.name }
        let wanted = chosenPort?.name ?? chosenInterfaceName
        network.removeAllItems(); network.addItem(withTitle: "Ethernet-Anschluss wählen")
        for p in ports where p.wired {
            network.addItem(withTitle: "\(p.name) — \(p.ip)\(p.wired ? " · Ethernet" : "")")
            network.lastItem?.representedObject = p.name
        }
        if let wanted {
            if let item = network.itemArray.first(where: { $0.representedObject as? String == wanted }) { network.select(item) }
            else {
                network.addItem(withTitle: "\(wanted) — kein aktiver Ethernet-Anschluss")
                network.lastItem?.representedObject = wanted; network.selectItem(at: network.numberOfItems - 1)
            }
        }
    }
    @objc func networkChanged() {
        guard let selectedName = network.selectedItem?.representedObject as? String,
              let selectedPort = ports.first(where: { $0.name == selectedName && $0.wired }) else { return }
        stopDiscovery(); discovered.removeAll(); sessionInterface = selectedName
        if config.easySetup != nil {
            config.easySetup?.interface = selectedName
            _ = rebuildSetup(reportErrors: true)
        } else if config.streams.isEmpty {
            config.easySetup = newEasySetup(selectedPort)
            _ = rebuildSetup(reportErrors: true)
        }
        refreshInterfaceMenu(selectedPort.name); table.reloadData(); updateStatus(); serviceDiscovery()
    }
    func refreshInterfaceMenu(_ wanted: String = "") {
        iface.removeAllItems()
        for p in ports { iface.addItem(withTitle: "\(p.name) — \(p.ip)") }
        if profile.indexOfSelectedItem == 2 { iface.addItem(withTitle: "lo0 — 127.0.0.1 (nur lokaler Test)") }
        if let i = ports.firstIndex(where: { $0.name == wanted || $0.ip == wanted }) {
            iface.selectItem(at: i)
        } else if profile.indexOfSelectedItem == 2 && wanted == "lo0" {
            iface.selectItem(at: iface.numberOfItems - 1)
        } else if !wanted.isEmpty {
            iface.select(nil)
        }
    }
    @objc func profileChanged() {
        let wanted = config.streams.indices.contains(selected) ? config.streams[selected].networkInterface : ""
        refreshInterfaceMenu(wanted)
    }
    func refreshTestInterfaceMenu() {
        let previous = testInterface.selectedItem?.representedObject as? String
        let testNames = Set(config.streams.filter { $0.sdp.mediaClockType == "local-test" && $0.networkInterface != "lo0" }.map(\.networkInterface))
        let wanted = previous ?? (testNames.count == 1 ? testNames.first : nil)
        let chosen = testPort(saved: wanted, ports: ports)
        testInterface.removeAllItems(); testInterface.addItem(withTitle: "Test-Anschluss wählen")
        for item in ports {
            testInterface.addItem(withTitle: "\(item.name) — \(item.ip)")
            testInterface.lastItem?.representedObject = item.name
        }
        if let chosen, let item = testInterface.itemArray.first(where: { $0.representedObject as? String == chosen.name }) { testInterface.select(item) }
        else if let wanted {
            testInterface.addItem(withTitle: "\(wanted) — nicht verfügbar")
            testInterface.lastItem?.representedObject = wanted; testInterface.selectItem(at: testInterface.numberOfItems - 1)
        }
    }
    @objc func refreshPorts() {
        // Preserve the form's pending selection rather than resetting edits
        // to the last saved stream every five seconds.
        let index = iface.indexOfSelectedItem
        let old = ports.indices.contains(index) ? ports[index].name :
            (config.streams.indices.contains(selected) ? config.streams[selected].networkInterface : "")
        ports = interfaces(); refreshInterfaceMenu(old); refreshTestInterfaceMenu(); updateStatus(); serviceDiscovery()
    }
    func updateStatus() {
        let installed = FileManager.default.fileExists(atPath: driver)
        let configured = FileManager.default.fileExists(atPath: system.path)
        refreshNetworkMenu()
        let deviceText: String
        let published = publishedDeviceChannels()
        if let counts = published {
            deviceText = "CoreAudio-Gerät verfügbar · \(counts.input) Eingänge × \(counts.output) Ausgänge"
        } else { deviceText = "CoreAudio-Gerät derzeit nicht verfügbar" }
        let clockURL = URL(fileURLWithPath: "/Library/Application Support/AES67Driver/clock-status.txt")
        var clockText = "Clock-Dienst · kein aktueller Status"
        var clockFresh = false, clockReady = false
        let clockPlist = "/Library/LaunchDaemons/org.communityaudio.aes67.clock.plist"
        let quarantined = clockPlist.withCString { path in
            "com.apple.quarantine".withCString { attribute in getxattr(path, attribute, nil, 0, 0, 0) >= 0 }
        }
        if let content = try? String(contentsOf: clockURL, encoding: .utf8) {
            let parts = content.split(whereSeparator: \.isWhitespace).map(String.init)
            let nowNs = UInt64(Date().timeIntervalSince1970 * 1_000_000_000)
            if parts.count >= 8, parts[0] == "1", let stamp = UInt64(parts[1]), nowNs >= stamp,
               nowNs - stamp < 3_000_000_000 {
                clockFresh = true
                let expectedDomain = config.streams.first { $0.enabled && $0.sdp.direction == "sendonly" && $0.sdp.mediaClockType != "local-test" }?.sdp.ptpDomain
                if parts[3] != "1" { clockText = "PTP-Clock-Dienst aktiv · wartet auf Synchronisation" }
                else if let chosen = chosenPort, parts[5] == chosen.name, parts[6] == chosen.ip,
                   expectedDomain == nil || Int(parts[4]) == expectedDomain {
                    clockReady = true
                    clockText = "PTP-Clock: synchronisiert · Domain \(parts[4]) · \(parts[5]) · Grandmaster \(parts[7])"
                } else { clockText = "PTP-Clock: wartet für den ausgewählten Anschluss / die konfigurierte Domain. Entwurf gegebenenfalls anwenden." }
            }
        }
        let localTest = !config.streams.isEmpty && config.streams.allSatisfy({ $0.sdp.mediaClockType == "local-test" })
        if quarantined && !clockFresh { clockText = "Clock-Dienst blockiert · macOS-Quarantäne auf der Dienstdefinition" }
        if localTest {
            clockText = "PTP für Mac-Test nicht erforderlich"
        }
        let networkText = ports.isEmpty ? "Netzwerk: kein aktiver Ethernet-/WLAN-Anschluss mit IPv4-Adresse gefunden" : "Netzwerk: " + ports.map { "\($0.name) · \($0.ip)" }.joined(separator: "     ")
        let missingPort = selected >= 0 && selected < config.streams.count &&
            !(config.streams[selected].sdp.mediaClockType == "local-test" && config.streams[selected].networkInterface == "lo0") &&
            !ports.contains(where: { $0.name == config.streams[selected].networkInterface })
        let streamPortText = missingPort ? "\n! Der ausgewählte Stream ist auf \(config.streams[selected].networkInterface) konfiguriert; dieser Anschluss ist aktuell nicht verfügbar." : ""
        func mark(_ ok: Bool) -> String { ok ? "✓ " : "⚠︎ " }
        let automatic = !localTest && config.easySetup != nil
        var lines = [mark(installed && published != nil) + "\(installed ? "Treiber installiert" : "Treiber fehlt") · \(deviceText)"]
        let found = consoles
        if automatic, let chosen = chosenPort {
            let warning = networkWarning(port: chosen, console: chosenConsole?.address ?? found.first?.address)
            lines.append(mark(warning == nil) + "Anschluss \(chosen.name) · \(chosen.ip)" + (warning == nil ? "" : " · Netzwerk prüfen, siehe Schritt 1"))
            portStatusLabel?.stringValue = [portNotice, warning ?? ""].filter { !$0.isEmpty }.joined(separator: "\n")
            portStatusLabel?.textColor = warning == nil ? .labelColor : .systemOrange
        } else if automatic {
            lines.append(mark(false) + "Kein Ethernet-Anschluss zum Pult")
            portStatusLabel?.stringValue = "Netzwerkkabel zum Pult oder Switch anschließen. Bei mehreren Ethernet-Anschlüssen hier den richtigen wählen."
        } else { lines.append(networkText + streamPortText) }
        if automatic {
            if let console = chosenConsole { lines.append(mark(true) + "Pult \(console.name) · \(console.address) · \(console.sources.count) Flow\(console.sources.count == 1 ? "" : "s")") }
            else if config.easySetup?.device != nil { lines.append(mark(false) + "Gespeichertes Pult sendet derzeit keine Ankündigungen") }
            else if config.easySetup?.selectedSources.isEmpty == false { lines.append(mark(true) + "Pult-Quellen einzeln gewählt (Erweitert)") }
            else { lines.append(mark(false) + (found.isEmpty ? "Noch kein AES67-Pult gefunden" : "Noch kein Pult gewählt")) }
        }
        status.stringValue = lines.joined(separator: "\n")
        ManagerLog.change("status", "Status: " + lines.joined(separator: " | "))
        if let portStatusLabel {
            if !automatic { portStatusLabel.stringValue = "" }
            portStatusLabel.isHidden = portStatusLabel.stringValue.isEmpty
        }
        let sends = config.streams.contains { $0.enabled && $0.sdp.direction == "sendonly" && $0.sdp.mediaClockType != "local-test" }
        if !localTest && !clockReady && sends { clockText += "\nOhne PTP-Takt senden die Mac-Sender nicht und erscheinen nicht im Dante Controller." }
        clockStatusLabel.stringValue = (localTest ? "" : mark(clockReady)) + clockText
        ManagerLog.change("clock", "Clock: " + clockText)
        clockStatusLabel.textColor = clockReady || localTest ? .systemGreen : .systemOrange
        let savedConfig = (try? Data(contentsOf: system)).flatMap { try? JSONDecoder().decode(Config.self, from: $0) }
        let matches = savedConfig == config
        configStatusLabel.stringValue = matches
            ? "Konfiguration · Entwurf entspricht dem gespeicherten Systemstand"
            : configured ? "Konfiguration · Entwurf weicht vom Systemstand ab" : "Konfiguration · noch nicht systemweit gespeichert"
        if !installed { nextStepLabel.stringValue = "Nächster Schritt: AES67-Paket installieren." }
        else if quarantined && !localTest && !clockFresh { nextStepLabel.stringValue = "Nächster Schritt: korrigiertes Installationspaket verwenden. Details unter Hinweise." }
        else if automatic && chosenPort == nil { nextStepLabel.stringValue = "Nächster Schritt: Netzwerkkabel zum Pult anschließen (Schritt 1)." }
        else if automatic, let chosen = chosenPort, networkWarning(port: chosen, console: chosenConsole?.address ?? found.first?.address) != nil {
            nextStepLabel.stringValue = "Nächster Schritt: Netzwerkverbindung prüfen (Schritt 1)."
        }
        else if automatic && found.isEmpty && config.easySetup?.device == nil && config.easySetup?.selectedSources.isEmpty == true {
            nextStepLabel.stringValue = "Nächster Schritt: im Dante Controller AES67 am Pult einschalten und einen AES67-Flow anlegen (Schritt 2)."
        }
        else if automatic && config.easySetup?.device == nil && config.easySetup?.selectedSources.isEmpty == true { nextStepLabel.stringValue = "Nächster Schritt: Pult auswählen (Schritt 2)." }
        else if !matches { nextStepLabel.stringValue = "Nächster Schritt: Einrichtung prüfen, dann Änderungen anwenden." }
        else if !localTest && !clockFresh {
            nextStepLabel.stringValue = quarantined
                ? "Nächster Schritt: korrigiertes Installationspaket verwenden. Details unter Hinweise."
                : "Nächster Schritt: Clock-Dienst prüfen. Details unter Hinweise."
        } else if !localTest && !clockReady { nextStepLabel.stringValue = "Nächster Schritt: Pult-Clock, Ethernet-Anschluss und PTP-Domain prüfen." }
        else { nextStepLabel.stringValue = "Nächster Schritt: AES67 Device in der Audio-App öffnen, im Dante Controller routen und Audio testen." }
        ManagerLog.change("next", nextStepLabel.stringValue)
        for (key, value) in [("port", portNotice), ("device", deviceNotice), ("address", addressNotice), ("setup", setupMessage)] {
            ManagerLog.change(key, value)
        }
        updateSetupSummary()
    }
    func updateSetupSummary() {
        let production = config.streams.filter { $0.enabled && $0.sdp.mediaClockType != "local-test" }
        let rx = production.filter { $0.sdp.direction != "sendonly" }.reduce(0) { $0 + $1.mapping.deviceChannelCount }
        let tx = production.filter { $0.sdp.direction == "sendonly" }.reduce(0) { $0 + $1.mapping.deviceChannelCount }
        let target = config.easySetup?.channels ?? config.inputChannels
        let adapter = chosenPort == nil ? (chosenInterfaceName == nil ? "Ethernet-Anschluss auswählen." : "Gespeicherter Anschluss nicht verfügbar; kein automatischer Wechsel.") : ""
        let senders = production.filter { $0.sdp.direction == "sendonly" }.count
        let waiting = rx < target ? "Für \(target - rx) weitere Eingänge sendet das Pult noch keine Flows." : ""
        let profileText = config.easySetup.map { "Profil \($0.channels) × \($0.channels): \(rx) Logic-Eingänge vom Pult, \(tx) Logic-Ausgänge über \(senders) Mac-Sender." } ?? "Manuelle Konfiguration · Profil zum Vorbereiten auswählen"
        setupSummary?.stringValue = [profileText, adapter, setupMessage, addressNotice, waiting].filter { !$0.isEmpty }.joined(separator: "\n")
        let tests = config.streams.filter { $0.enabled && $0.sdp.mediaClockType == "local-test" }
        let localRX = tests.contains { $0.sdp.direction != "sendonly" }
        let localTX = tests.contains { $0.sdp.direction == "sendonly" }
        let sameMac = localRX && localTX && tests.allSatisfy { $0.networkInterface == "lo0" && $0.sdp.connectionAddress == "239.69.100.2" && $0.sdp.port == 5006 }
        let testText: String
        if sameMac { testText = "Mac-Rückweg vorbereitet: Sender und Empfänger auf lo0 · Ausgang 1–2 → Eingang 1–2. Änderungen anwenden, dann Musik abspielen und GarageBand aufnehmen lassen." }
        else if localTX && !localRX { testText = "Nur Zwei-Mac-Sender vorhanden. Auf diesem Mac ist kein Empfang eingerichtet; für GarageBand auf demselben Mac oben „Auf diesem Mac vorbereiten …“ wählen. Für den Zwei-Mac-Test den Empfänger auf dem anderen Mac vorbereiten." }
        else if localRX && !localTX { testText = "Zwei-Mac-Empfänger vorhanden. Sender auf dem anderen Mac vorbereiten und beide Änderungen anwenden. Für Musik → Aufnahme auf demselben Mac oben den lokalen Rückweg vorbereiten." }
        else if !tests.isEmpty { testText = "Mac-Test-Streams vorhanden. Die Zuordnung unter Erweitert prüfen und Änderungen anwenden." }
        else { testText = "Noch kein Mac-Test eingerichtet. Für Musik → GarageBand auf diesem Mac den lokalen Rückweg vorbereiten." }
        testSummary?.stringValue = testText
        if !production.isEmpty || tests.isEmpty { refreshSources(); return }
        setupSummary?.stringValue = testText + "\nMac-Test benötigt keine Pult-Flows oder PTP-Clock."
        refreshSources()
    }
    // Draft persistence is deliberately separate from privileged activation.
    func persistAutomaticDraft() {
        guard !headless else { return }
        do {
            try FileManager.default.createDirectory(at: stage.deletingLastPathComponent(), withIntermediateDirectories: true)
            let encoder = JSONEncoder(); encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
            try encoder.encode(config).write(to: stage, options: .atomic)
        } catch { setupMessage = "Entwurf konnte nicht gespeichert werden: \(error.localizedDescription)" }
    }
    @discardableResult func rebuildSetup(reportErrors: Bool) -> Bool {
        guard let chosen = chosenPort, var settings = config.easySetup else { return false }
        if settings.interface != chosen.name {
            portNotice = "Der gespeicherte Anschluss \(settings.interface) ist nicht verbunden; \(chosen.name) wurde übernommen. Zum Aktivieren „Änderungen anwenden …“ wählen."
        }
        settings = followDevice(settings, devices: consoles)
        let taken = Set(availableSources.values.map(\.connectionAddress))
        if let free = freeSetupBase(settings.multicastBase, channels: settings.channels, taken: taken), free != settings.multicastBase {
            addressNotice = "Ein Pult-Flow nutzt bereits \(settings.multicastBase).x; die Mac-Sender weichen auf \(free).x aus. Schon im Dante Controller geroutete Mac-Sender nach dem Anwenden neu zuordnen."
            settings.multicastBase = free
        }
        txBaseField.stringValue = settings.multicastBase
        do {
            var draft = config; draft.easySetup = settings
            let planned = try plannedSetup(draft, channels: settings.channels, interface: chosen, sources: availableSources, base: settings.multicastBase)
            setupMessage = ""
            if planned != config { config = planned; persistAutomaticDraft(); table.reloadData(); table.deselectAll(nil) }
            inputCount.stringValue = String(config.inputChannels); outputCount.stringValue = String(config.outputChannels)
            return true
        } catch {
            setupMessage = error.localizedDescription
            if reportErrors { alert("Automatik wartet", error.localizedDescription) }
            return false
        }
    }
    func setPreset(_ count: Int) {
        guard let chosen = chosenPort else { alert("Netzwerkanschluss wählen", "Bitte zuerst den aktiven Ethernet-Anschluss zum Pult wählen."); return }
        let old = config
        var settings = config.easySetup ?? newEasySetup(chosen)
        settings.channels = count
        if config.easySetup != nil { settings.multicastBase = txBaseField.stringValue }
        config.easySetup = settings
        if rebuildSetup(reportErrors: true) { persistAutomaticDraft() } else { config = old }
        updateStatus(); serviceDiscovery()
    }
    @objc func preset4() { setPreset(4) }
    @objc func preset8() { setPreset(8) }
    @objc func preset16() { setPreset(16) }
    @objc func prepareTX() {
        let count = config.easySetup?.channels ?? Int(outputCount.stringValue) ?? 8
        setPreset(count)
    }
    @objc func guidedDiscover() { nextDiscovery = .distantPast; serviceDiscovery() }
    func refreshSources() {
        guard sourcePicker != nil else { return }
        let previous = sourcePicker.selectedItem?.representedObject as? String
        let selectedIDs = config.easySetup?.selectedSources ?? []
        sourcePicker.removeAllItems(); sourcePicker.addItem(withTitle: "Gefundene AES67-Quelle wählen")
        for (id, entry) in discovered.sorted(by: { $0.value.sdp.sessionName < $1.value.sdp.sessionName }) where !selectedIDs.contains(id) {
            let s = entry.sdp
            sourcePicker.addItem(withTitle: "\(s.sessionName) · \(s.numChannels) Kanäle · \(s.connectionAddress):\(s.port)")
            sourcePicker.lastItem?.representedObject = id
        }
        if let previous, let item = sourcePicker.itemArray.first(where: { $0.representedObject as? String == previous }) { sourcePicker.select(item) }
        let labels = selectedIDs.map { id -> String in
            let stream = config.streams.first { $0.description == setupRXMarker && sourceIdentity($0.sdp) == id }
            let s = discovered[id]?.sdp ?? stream?.sdp
            let name = s?.sessionName ?? "Gespeicherte Quelle"
            let range = stream.map { " · Logic \($0.mapping.deviceChannelStart + 1)–\($0.mapping.deviceChannelStart + $0.mapping.deviceChannelCount)" } ?? " · wartet auf freien Profilbereich"
            return "\(name)\(range)\(discovered[id] == nil ? " · derzeit keine aktuelle SAP-Ankündigung" : " · SAP gesehen")"
        }
        sourceSummary?.stringValue = ([discoveryMessage] + labels).filter { !$0.isEmpty }.joined(separator: "\n")
        refreshDevices()
    }
    var consoles: [ConsoleDevice] { consoleDevices(availableSources) }
    var chosenConsole: ConsoleDevice? { config.easySetup?.device.flatMap { address in consoles.first { $0.address == address } } }
    func refreshDevices() {
        guard devicePicker != nil else { return }
        let found = consoles, wanted = config.easySetup?.device
        devicePicker.removeAllItems()
        devicePicker.addItem(withTitle: found.isEmpty ? "Noch kein AES67-Pult gefunden" : "Pult wählen")
        for device in found {
            devicePicker.addItem(withTitle: "\(device.name) · \(device.address) · \(device.sources.count) Flow\(device.sources.count == 1 ? "" : "s") · \(device.channels) Kanäle")
            devicePicker.lastItem?.representedObject = device.address
        }
        if let wanted {
            if let item = devicePicker.itemArray.first(where: { $0.representedObject as? String == wanted }) { devicePicker.select(item) }
            else {
                devicePicker.addItem(withTitle: "Gespeichertes Pult \(wanted) · derzeit keine Ankündigung")
                devicePicker.lastItem?.representedObject = wanted; devicePicker.selectItem(at: devicePicker.numberOfItems - 1)
            }
        }
        let inputs = config.streams.filter { $0.enabled && $0.description == setupRXMarker }.reduce(0) { $0 + $1.mapping.deviceChannelCount }
        let lines: [String]
        if let device = chosenConsole {
            let profile = config.easySetup?.channels ?? device.channels
            lines = [deviceNotice,
                     "\(device.name): \(device.sources.count) Flow\(device.sources.count == 1 ? "" : "s") mit \(device.channels) Kanälen · PTP-Domain \(device.ptpDomain). Empfang auf Logic-Eingänge 1–\(inputs) vorbereitet.",
                     device.channels > profile ? "Das Profil \(profile) × \(profile) nutzt nur die ersten \(profile) Pult-Kanäle; für mehr ein größeres Profil wählen." : ""]
        } else if wanted != nil {
            lines = [deviceNotice, "Das gespeicherte Pult sendet gerade keine Ankündigungen. Die bisherigen Empfangsstreams bleiben erhalten. Kabel und Dante-Karte prüfen; die Suche läuft weiter."]
        } else if found.isEmpty {
            lines = [discoveryMessage,
                     "Das Pult kündigt erst AES67-Sendeflows an, wenn sie im Dante Controller angelegt sind: beim X-DANTE in der Device View unter „AES67 Config“ den AES67-Modus einschalten (die Karte startet neu), dann einen Multicast-Flow mit Haken bei „AES67“ anlegen, z. B. Kanäle 1–8. Er erscheint hier nach einigen Sekunden."]
        } else if config.easySetup?.selectedSources.isEmpty == false {
            lines = ["Quellen wurden unter Erweitert einzeln gewählt. Mit der Auswahl eines Pults hier werden stattdessen alle seine Flows übernommen."]
        } else {
            lines = ["Mehrere AES67-Geräte gefunden. Das Pult wählen, dessen Audio auf dem Mac ankommen soll."]
        }
        deviceSummary?.stringValue = lines.filter { !$0.isEmpty }.joined(separator: "\n")
    }
    @objc func deviceChanged() {
        guard let address = devicePicker.selectedItem?.representedObject as? String,
              let device = consoles.first(where: { $0.address == address }) else { return }
        chooseDevice(device, automatically: false)
    }
    func chooseDevice(_ device: ConsoleDevice, automatically: Bool) {
        guard let chosen = chosenPort else { return }
        let old = config
        config.easySetup = selectDevice(config.easySetup ?? newEasySetup(chosen), device)
        deviceNotice = automatically ? "\(device.name) ist das einzige gefundene AES67-Pult und wurde automatisch gewählt." : ""
        if rebuildSetup(reportErrors: !automatically) { persistAutomaticDraft() } else { config = old }
        updateStatus()
    }
    @objc func selectSource() {
        guard let id = sourcePicker.selectedItem?.representedObject as? String else { return }
        addSource(id)
    }
    func addSource(_ id: String) {
        guard discovered[id] != nil, let chosen = chosenPort else { return }
        let old = config
        if config.easySetup == nil { config.easySetup = newEasySetup(chosen, channels: [4, 8, 16].contains(config.inputChannels) ? config.inputChannels : 8) }
        guard config.easySetup?.selectedSources.contains(id) == false else { return }
        config.easySetup?.device = nil; deviceNotice = ""
        config.easySetup?.selectedSources.append(id)
        if rebuildSetup(reportErrors: true) { persistAutomaticDraft() } else { config = old }
        updateStatus()
    }
    @objc func clearSources() {
        guard config.easySetup != nil else { return }
        let old = config; config.easySetup?.selectedSources = []; config.easySetup?.device = nil; deviceNotice = ""; autoSelectDeclined = true
        if rebuildSetup(reportErrors: true) { persistAutomaticDraft() } else { config = old }
        updateStatus()
    }
    @objc func guidedImportSDP() {
        guard chosenPort != nil else { alert("Schnittstelle wählen", "Wähle zuerst den Netzwerkanschluss zum Pult."); return }
        let panel = NSOpenPanel(); panel.allowedFileTypes = ["sdp"]
        guard panel.runModal() == .OK, let url = panel.url else { return }
        guard let text = try? String(contentsOf: url, encoding: .utf8), let s = importSession(text) else {
            alert("SDP nicht importiert", "Unterstützt: AES67 RX, 48 kHz, L16/L24, 1 ms, maximal 8 Kanäle pro Flow."); return
        }
        let id = sourceIdentity(s)
        // Imported descriptions remain selectable but do not pretend to be a live SAP announcement.
        discovered[id] = (sdp: s, seen: .distantPast)
        addSource(id)
        discovered.removeValue(forKey: id); updateStatus()
    }
    func load() { let url = FileManager.default.fileExists(atPath: stage.path) ? stage : system; if let data = try? Data(contentsOf: url) { if let x = try? JSONDecoder().decode(Config.self, from: data) { config = x } else { alert("Konfiguration ungültig", "Vorhandene streams.json konnte nicht gelesen werden: \(url.path)") } }; inputCount.stringValue = String(config.inputChannels); outputCount.stringValue = String(config.outputChannels); txBaseField.stringValue = config.easySetup?.multicastBase ?? "239.69.250"; table.reloadData() }
    func suggestedTXGroup() -> String {
        let octets = (ports.first?.ip ?? "0.0.83.172").split(separator: ".").compactMap { Int($0) }
        guard octets.count == 4 else { return "239.69.83.172" }
        return "239.69.\(octets[2]).\(octets[3])"
    }
    func requestedCounts() -> (Int, Int)? {
        guard let inputs = Int(inputCount.stringValue), (1...32).contains(inputs),
              let outputs = Int(outputCount.stringValue), (1...32).contains(outputs) else {
            alert("Kanalzahl prüfen", "Logic-Eingänge und Logic-Ausgänge müssen jeweils zwischen 1 und 32 liegen."); return nil
        }
        for stream in config.streams where stream.enabled {
            let limit = stream.sdp.direction == "sendonly" ? outputs : inputs
            if stream.mapping.deviceChannelStart + stream.mapping.deviceChannelCount > limit {
                alert("Kanalzahl zu klein", "\(stream.sdp.sessionName) belegt Logic-Kanal \(stream.mapping.deviceChannelStart + stream.mapping.deviceChannelCount). Den Stream zuerst anpassen oder entfernen; die bisherige Kanalzahl bleibt erhalten.")
                return nil
            }
        }
        return (inputs, outputs)
    }
    func numberOfRows(in v: NSTableView) -> Int { config.streams.count }
    func tableView(_ v: NSTableView, viewFor c: NSTableColumn?, row: Int) -> NSView? {
        let s = config.streams[row], key = c?.identifier.rawValue ?? ""
        let value: String
        switch key { case "Richtung": value = s.sdp.direction == "sendonly" ? "TX →" : "← RX"; case "Name": value = s.sdp.sessionName; case "Multicast": value = s.sdp.connectionAddress; case "Kanäle": value = String(s.sdp.numChannels); default: value = "\(s.mapping.deviceChannelStart + 1)–\(s.mapping.deviceChannelStart + s.mapping.deviceChannelCount)" }
        return label(value)
    }
    func tableViewSelectionDidChange(_ n: Notification) {
        guard selected >= 0, selected < config.streams.count else { return }
        let s = config.streams[selected]; mode.selectItem(at: s.sdp.direction == "sendonly" ? 1 : 0)
        profile.selectItem(at: s.sdp.mediaClockType == "local-test" ? (s.networkInterface == "lo0" && s.sdp.connectionAddress == "239.69.100.2" && s.sdp.port == 5006 ? 2 : 1) : 0)
        name.stringValue = s.sdp.sessionName; group.stringValue = s.sdp.connectionAddress; port.stringValue = String(s.sdp.port)
        channels.stringValue = String(s.sdp.numChannels); start.stringValue = String(s.mapping.deviceChannelStart + 1)
        payload.stringValue = String(s.sdp.payloadType); domain.stringValue = String(s.sdp.ptpDomain); refreshInterfaceMenu(s.networkInterface)
    }
    func candidate(_ old: Stream? = nil) -> Stream? {
        guard let (inputs, outputs) = requestedCounts() else { return nil }
        let localLoopback = profile.indexOfSelectedItem == 2
        guard let chosen = localLoopback ? Port(name: "lo0", ip: "127.0.0.1") : (iface.indexOfSelectedItem >= 0 && iface.indexOfSelectedItem < ports.count ? ports[iface.indexOfSelectedItem] : nil) else { alert("Schnittstelle wählen", "Bitte den Pult-Netzwerkanschluss auswählen."); return nil }
        guard !name.stringValue.isEmpty,
              !name.stringValue.contains("\""), !name.stringValue.contains("\\"),
              !name.stringValue.contains("\n"), !name.stringValue.contains("\r"),
              multicast(group.stringValue),
              let p = Int(port.stringValue), (1...65535).contains(p),
              let count = Int(channels.stringValue), (1...8).contains(count),
              let first = Int(start.stringValue), (1...32).contains(first), first + count <= 33,
              let pt = Int(payload.stringValue), (0...127).contains(pt),
              let pd = Int(domain.stringValue), (0...127).contains(pd) else {
            alert("Eingaben prüfen", "Name, 239.x-Multicast, Port 1–65535, 1–8 Kanäle, Logic-Start 1–32, Payload 0–127 und PTP-Domain 0–127 erforderlich."); return nil
        }
        let now = UInt64(Date().timeIntervalSince1970)
        var s = old?.sdp ?? SDP()
        s.sessionName = name.stringValue; s.connectionAddress = group.stringValue; s.port = p
        s.numChannels = count; s.payloadType = pt; s.ptpDomain = pd
        let localTest = profile.indexOfSelectedItem == 1 || localLoopback
        s.direction = mode.indexOfSelectedItem == 1 ? "sendonly" : "recvonly"
        s.mediaClockType = localTest ? "local-test" : "direct=0"
        if localTest {
            let expectedGroup = localLoopback ? "239.69.100.2" : "239.69.100.1"
            let expectedPort = localLoopback ? 5006 : 5004
            guard group.stringValue == expectedGroup, p == expectedPort, count == 2,
                  s.direction != "sendonly" || first == 1 else {
                alert("Mac-Test-Einstellungen prüfen", "Das gewählte Testprofil erfordert seine festgelegte Multicast-Adresse, Port und 2 Kanäle. Der Sender verwendet für Stereo-Musik die Mac-Ausgänge 1–2."); return nil
            }
        }
        let limit = s.direction == "sendonly" ? outputs : inputs
        guard first + count - 1 <= limit else {
            alert("Kanalbereich zu groß", "Der Stream endet bei Logic-Kanal \(first + count - 1); für diese Richtung sind \(limit) sichtbar."); return nil
        }
        s.encoding = s.direction == "sendonly" ? "L24" : (old?.sdp.encoding ?? "L24")
        s.sampleRate = 48000; s.ptime = 1; s.framecount = 48
        if s.direction == "sendonly" {
            s.sourceAddress = chosen.ip; s.originAddress = chosen.ip
            if s.sessionID == 0 { s.sessionID = now }
            s.sessionVersion = now
        }
        let m = Mapping(streamID: old?.mapping.streamID ?? UUID().uuidString.lowercased(),
                        streamName: s.sessionName, streamChannelCount: count,
                        deviceChannelStart: first - 1, deviceChannelCount: count)
        return Stream(createdTimestamp: old?.createdTimestamp ?? now, modifiedTimestamp: now,
                      networkInterface: chosen.name, sdp: s, mapping: m)
    }
    func validate(_ proposed: Stream, skipping: Int? = nil) -> Bool {
        guard proposed.enabled else { return true }
        let a = proposed.mapping.deviceChannelStart
        let b = a + proposed.mapping.deviceChannelCount
        let tx = proposed.sdp.direction == "sendonly"
        for (i, other) in config.streams.enumerated() where i != skipping && other.enabled {
            if tx && other.sdp.direction == "sendonly" &&
               other.sdp.connectionAddress == proposed.sdp.connectionAddress && other.sdp.port == proposed.sdp.port {
                alert("TX-Adresse belegt", "Jeder TX-Stream braucht eine eigene Multicast-Adresse oder einen eigenen RTP-Port.")
                return false
            }
            if tx && other.sdp.direction == "sendonly" &&
               (other.networkInterface != proposed.networkInterface || other.sdp.ptpDomain != proposed.sdp.ptpDomain) {
                alert("TX-Clock-Konfiguration", "Alle TX-Streams müssen dieselbe Schnittstelle und PTP-Domain nutzen.")
                return false
            }
            if (other.sdp.direction == "sendonly") == tx {
                let x = other.mapping.deviceChannelStart
                let y = x + other.mapping.deviceChannelCount
                if a < y && x < b { alert("Kanalbereich belegt", "Dieser \(tx ? "Ausgang" : "Eingang") überlappt \(other.sdp.sessionName)."); return false }
            }
        }
        return true
    }
    func leaveAutomaticMode() {
        guard config.easySetup != nil else { return }
        config.easySetup = nil
        for i in config.streams.indices where isSetupStream(config.streams[i]) { config.streams[i].description = "" }
        setupMessage = "Manuelle Konfiguration aktiv. Automatikprofile können nach Anpassung der belegten Kanäle erneut gewählt werden."
    }
    @objc func add() { guard let s = candidate(), validate(s) else { return }; leaveAutomaticMode(); config.streams.append(s); table.reloadData(); table.selectRowIndexes(IndexSet(integer: config.streams.count - 1), byExtendingSelection: false) }
    @objc func updateSelected() { guard selected >= 0 else { return }; let i = selected; guard let s = candidate(config.streams[i]), validate(s, skipping: i) else { return }; leaveAutomaticMode(); config.streams[i] = s; table.reloadData(); table.selectRowIndexes(IndexSet(integer: i), byExtendingSelection: false) }
    @objc func remove() { guard selected >= 0 else { return }; leaveAutomaticMode(); config.streams.remove(at: selected); table.reloadData() }
    func addMacTestStream(direction: String) {
        guard let (inputs, outputs) = requestedCounts() else { return }
        guard let adapter = testInterface.selectedItem?.representedObject as? String,
              let chosen = testPort(saved: adapter, ports: ports) else {
            alert("Netzwerkanschluss wählen", "Verbinde beide Macs mit demselben lokalen Netzwerk und wähle hier den passenden Anschluss."); return
        }
        let original = config
        let draft = macTestDraft(original)
        let tx = direction == "sendonly"
        // Both roles share 239.69.100.1:5004. On one Mac the receiver also hears the
        // local sender via multicast loopback and resynchronises on every packet.
        if draft.streams.contains(where: { $0.enabled && $0.sdp.mediaClockType == "local-test" && $0.networkInterface != "lo0" &&
                                           $0.sdp.connectionAddress == "239.69.100.1" && $0.sdp.port == 5004 && ($0.sdp.direction == "sendonly") != tx }) {
            alert("Sender und Empfänger gehören auf zwei Macs",
                  "Auf diesem Mac ist schon der \(tx ? "Empfänger" : "Sender") des Zwei-Mac-Tests eingerichtet. Beide nutzen 239.69.100.1:5004; auf demselben Mac empfängt der Empfänger dann auch den eigenen Sender, und das Audio bricht zusammen. Den \(tx ? "Sender" : "Empfänger") auf dem anderen Mac vorbereiten oder den vorhandenen Stream unter Erweitert entfernen.")
            return
        }
        let used = Set(draft.streams.filter { $0.enabled && ($0.sdp.direction == "sendonly") == tx }.flatMap {
            $0.mapping.deviceChannelStart ..< ($0.mapping.deviceChannelStart + $0.mapping.deviceChannelCount)
        })
        let limit = tx ? outputs : inputs
        if tx && (0..<2).contains(where: { used.contains($0) }) {
            alert("Logic-Ausgänge 1–2 belegt", "Der Mac-Test-Sender ist fest auf Logic-Ausgänge 1–2 gelegt. Passe den vorhandenen TX-Stream an oder entferne ihn, damit der Test nicht unbemerkt auf einen stillen Bereich ausweicht."); return
        }
        guard limit >= 2, let first = tx ? 0 : (0...(limit - 2)).first(where: { n in (n ..< n + 2).allSatisfy { !used.contains($0) } }) else {
            alert("Keine Kanäle frei", "Im Bereich von \(limit) Logic-\(tx ? "Ausgängen" : "Eingängen") sind keine zwei aufeinanderfolgenden Kanäle frei."); return
        }
        let now = UInt64(Date().timeIntervalSince1970)
        var s = SDP(); s.sessionName = tx ? "Mac-Test Sender" : "Mac-Test Empfänger"
        s.connectionAddress = "239.69.100.1"; s.port = 5004; s.numChannels = 2
        s.encoding = "L24"; s.sampleRate = 48000; s.ptime = 1; s.framecount = 48
        s.direction = direction; s.mediaClockType = "local-test"
        if tx { s.originAddress = chosen.ip; s.sourceAddress = chosen.ip; s.sessionID = now; s.sessionVersion = now }
        let m = Mapping(streamID: UUID().uuidString.lowercased(), streamName: s.sessionName,
                        streamChannelCount: 2, deviceChannelStart: first, deviceChannelCount: 2)
        let item = Stream(createdTimestamp: now, modifiedTimestamp: now, networkInterface: chosen.name, sdp: s, mapping: m)
        config = draft
        guard validate(item) else { config = original; return }
        if original.easySetup != nil {
            do {
                try FileManager.default.createDirectory(at: stage.deletingLastPathComponent(), withIntermediateDirectories: true)
                try backupConfigIfMissing(original, at: stage.deletingLastPathComponent().appendingPathComponent("before-local-test.json"))
            } catch { config = original; alert("Mac-Test konnte nicht vorbereitet werden", error.localizedDescription); return }
        }
        stopDiscovery(); discovered.removeAll(); sessionInterface = nil
        config.inputChannels = inputs; config.outputChannels = outputs
        config.streams.append(item); table.reloadData()
        table.selectRowIndexes(IndexSet(integer: config.streams.count - 1), byExtendingSelection: false)
        updateStatus()
    }
    @objc func prepareLocalLoopback() {
        let confirm = NSAlert()
        confirm.messageText = "Lokalen Entwurf durch den Mac-Test ersetzen?"
        confirm.informativeText = "Der Entwurf wird durch genau zwei lokale Streams ersetzt: AES67 TX 1–2 auf 239.69.100.2:5006 und RX 1–2 auf lo0 (127.0.0.1). Einmalige Sicherung: before-local-test.json. Systemweit wird erst mit „Änderungen anwenden …“ geändert."
        confirm.addButton(withTitle: "Sichern und vorbereiten")
        confirm.addButton(withTitle: "Abbrechen")
        guard confirm.runModal() == .alertFirstButtonReturn else { return }

        do {
            try FileManager.default.createDirectory(at: stage.deletingLastPathComponent(), withIntermediateDirectories: true)
            let backup = stage.deletingLastPathComponent().appendingPathComponent("before-local-test.json")
            if !FileManager.default.fileExists(atPath: backup.path) {
                try backupConfigIfMissing(config, at: backup)
            }

            let inputChannels = max(2, min(32, Int(inputCount.stringValue) ?? config.inputChannels))
            let outputChannels = max(2, min(32, Int(outputCount.stringValue) ?? config.outputChannels))
            stopDiscovery(); discovered.removeAll(); sessionInterface = nil
            config = localLoopbackConfig(inputChannels: inputChannels, outputChannels: outputChannels)
            inputCount.stringValue = String(inputChannels); outputCount.stringValue = String(outputChannels)
            profile.selectItem(at: 2); mode.selectItem(at: 0)
            group.stringValue = "239.69.100.2"; port.stringValue = "5006"; channels.stringValue = "2"; start.stringValue = "1"
            refreshInterfaceMenu("lo0")
            table.reloadData(); table.selectRowIndexes(IndexSet(integer: 1), byExtendingSelection: false)
            updateStatus()
            alert("Mac-Test vorbereitet", "Der lokale Entwurf enthält RX und TX auf lo0. Wähle AES67 als Wiedergabe für YouTube/Music und als Eingang in GarageBand. GarageBand-Ausgang auf eingebaute Lautsprecher stellen und Software-Monitoring ausschalten. Danach „Änderungen anwenden …“ wählen. Der Audiodienst wird dabei kurz neu gestartet.")
        } catch {
            alert("Mac-Test konnte nicht vorbereitet werden", error.localizedDescription)
        }
    }
    @objc func restoreBeforeLocalTest() {
        let backup = stage.deletingLastPathComponent().appendingPathComponent("before-local-test.json")
        guard let data = try? Data(contentsOf: backup), let restored = try? JSONDecoder().decode(Config.self, from: data) else {
            alert("Keine Sicherung gefunden", "Es gibt noch keine before-local-test.json im AES67Driver-Benutzerordner."); return
        }
        stopDiscovery(); discovered.removeAll(); sessionInterface = nil
        config = restored
        txBaseField.stringValue = restored.easySetup?.multicastBase ?? "239.69.250"
        inputCount.stringValue = String(restored.inputChannels); outputCount.stringValue = String(restored.outputChannels)
        table.reloadData(); table.deselectAll(nil)
        profile.selectItem(at: 0); refreshInterfaceMenu(restored.easySetup?.interface ?? "")
        refreshTestInterfaceMenu(); updateStatus(); serviceDiscovery()
        alert("Vorheriger Entwurf wiederhergestellt", "Die Sicherung wurde in den lokalen Entwurf geladen. Sie ist noch nicht systemweit angewendet. Zum Aktivieren „Änderungen anwenden …“ wählen. Dabei wird Audio kurz unterbrochen.")
    }
    @objc func addTestReceiver() { profile.selectItem(at: 1); mode.selectItem(at: 0); group.stringValue = "239.69.100.1"; port.stringValue = "5004"; channels.stringValue = "2"; addMacTestStream(direction: "recvonly") }
    @objc func addTestSender() { profile.selectItem(at: 1); mode.selectItem(at: 1); group.stringValue = "239.69.100.1"; port.stringValue = "5004"; channels.stringValue = "2"; addMacTestStream(direction: "sendonly") }
    @objc func checkSelectedReceive() {
        guard receiverTestProcess == nil else { return }
        guard selected >= 0, selected < config.streams.count else {
            alert("RX-Stream auswählen", "Wähle zuerst den Mac-Test-Empfänger in der Streamliste aus."); return
        }
        let entry = config.streams[selected], s = entry.sdp
        guard s.direction != "sendonly", s.mediaClockType == "local-test" else {
            alert("Mac-Test-RX auswählen", "Die 10-Sekunden-Prüfung ist für einen RX-Stream mit dem Profil ‚Mac-Test (ohne PTP)‘ vorgesehen."); return
        }
        guard let tool = Bundle.main.resourceURL?.appendingPathComponent("AES67TestReceiver"), FileManager.default.isExecutableFile(atPath: tool.path) else {
            alert("Empfangsprüfung fehlt", "AES67TestReceiver ist nicht im App-Paket enthalten. Bitte die aktuelle Manager-Installation verwenden."); return
        }
        let outputPipe = Pipe(), errorPipe = Pipe(), drain = DispatchGroup()
        let output = CaptureBuffer(), errors = CaptureBuffer()
        func attach(_ pipe: Pipe, to buffer: CaptureBuffer) {
            drain.enter()
            pipe.fileHandleForReading.readabilityHandler = { handle in
                let chunk = handle.availableData
                if chunk.isEmpty {
                    handle.readabilityHandler = nil
                    try? handle.close()
                    drain.leave()
                } else {
                    buffer.append(chunk)
                }
            }
        }
        // Keep distinct buffers alive until both child pipes reach EOF.
        attach(outputPipe, to: output)
        attach(errorPipe, to: errors)
        let task = Process(); task.executableURL = tool
        task.arguments = ["--ip", s.connectionAddress, "--port", String(s.port), "--channels", String(s.numChannels),
                          "--encoding", s.encoding, "--interface", entry.networkInterface, "--duration", "10"]
        task.standardOutput = outputPipe.fileHandleForWriting; task.standardError = errorPipe.fileHandleForWriting
        receiverTestProcess = task; receiveCheckButton.isEnabled = false
        status.stringValue = "Mac-Test-Empfang läuft: 10 Sekunden auf \(entry.networkInterface) · \(s.connectionAddress):\(s.port) — ohne PTP"
        task.terminationHandler = { proc in
            drain.notify(queue: .main) {
                let stdoutText = output.text()
                let stderrText = errors.text()
                self.receiverTestProcess = nil; self.receiveCheckButton.isEnabled = true; self.updateStatus()
                let fields = stdoutText.split(whereSeparator: \.isWhitespace).dropFirst().compactMap { token -> (String, String)? in
                    let pair = token.split(separator: "=", maxSplits: 1).map(String.init)
                    return pair.count == 2 ? (pair[0], pair[1]) : nil
                }
                let values = Dictionary(fields, uniquingKeysWith: { _, latest in latest })
                let resultLine: String
                switch values["status"] {
                case "AUDIO_DETECTED": resultLine = "Audiosignal erkannt."
                case "SILENCE": resultLine = "RTP-Pakete kommen an, aber es wurde kein Audiosignal erkannt."
                case "NO_PACKETS": resultLine = "Keine RTP-Pakete empfangen. Prüfe Sender, Netzwerkanschluss und Multicast-Freigabe."
                default: resultLine = stdoutText.trimmingCharacters(in: .whitespacesAndNewlines)
                }
                let measurements = ["packets", "duration_ms"].compactMap { key in values[key].map { "\(key == "packets" ? "Pakete" : "Messdauer (ms)"): \($0)" } }
                let peaks = fields.filter { $0.0.hasPrefix("channel") && $0.0.hasSuffix("_peak") }.map { "Kanal \($0.0.dropFirst(7).dropLast(5)) Spitzenpegel: \($0.1)" }
                let diagnostics = stderrText.trimmingCharacters(in: .whitespacesAndNewlines)
                let report = ([resultLine] + measurements + peaks + (values["status"] != nil || diagnostics.isEmpty ? [] : [diagnostics])).filter { !$0.isEmpty }.joined(separator: "\n")
                self.alert(proc.terminationStatus == 0 ? "Empfangsprüfung abgeschlossen · Mac-Test ohne PTP" : "Empfangsprüfung fehlgeschlagen", report.isEmpty ? "Empfänger beendet mit Status \(proc.terminationStatus)." : report)
            }
        }
        do {
            try task.run()
            try? outputPipe.fileHandleForWriting.close(); try? errorPipe.fileHandleForWriting.close()
        } catch {
            receiverTestProcess = nil; receiveCheckButton.isEnabled = true; outputPipe.fileHandleForReading.readabilityHandler = nil; errorPipe.fileHandleForReading.readabilityHandler = nil
            try? outputPipe.fileHandleForReading.close(); try? errorPipe.fileHandleForReading.close()
            try? outputPipe.fileHandleForWriting.close(); try? errorPipe.fileHandleForWriting.close()
            alert("Empfangsprüfung konnte nicht starten", error.localizedDescription)
        }
    }
    @objc func importSDP() {
        let panel = NSOpenPanel(); panel.allowedFileTypes = ["sdp"]
        guard panel.runModal() == .OK, let url = panel.url,
              let content = try? String(contentsOf: url, encoding: .utf8),
              let s = importSession(content) else {
            alert("SDP nicht importiert", "Unterstützt: AES67 RX, 48 kHz, L16/L24, 1 ms, maximal 8 Kanäle pro Stream."); return
        }
        guard ports.indices.contains(iface.indexOfSelectedItem) else { alert("Schnittstelle wählen", "Bitte zuerst Pult-Netzwerkschnittstelle wählen."); return }
        let used = Set(config.streams.filter { $0.sdp.direction != "sendonly" }.flatMap { $0.mapping.deviceChannelStart ..< ($0.mapping.deviceChannelStart + $0.mapping.deviceChannelCount) })
        guard let (inputs, _) = requestedCounts() else { return }
        guard s.numChannels <= inputs,
              let first = (0...(inputs - s.numChannels)).first(where: { n in (n ..< (n + s.numChannels)).allSatisfy { !used.contains($0) } }) else { alert("Keine Kanäle frei", "Kein freier Bereich in den \(inputs) Logic-Eingängen für diesen Stream."); return }
        let now = UInt64(Date().timeIntervalSince1970)
        let m = Mapping(streamID: UUID().uuidString.lowercased(), streamName: s.sessionName, streamChannelCount: s.numChannels, deviceChannelStart: first, deviceChannelCount: s.numChannels)
        config.streams.append(Stream(createdTimestamp: now, modifiedTimestamp: now, networkInterface: ports[iface.indexOfSelectedItem].name, sdp: s, mapping: m))
        table.reloadData(); table.selectRowIndexes(IndexSet(integer: config.streams.count - 1), byExtendingSelection: false)
    }
    @objc func exportSDP() {
        guard selected >= 0, config.streams[selected].sdp.direction == "sendonly" else { alert("TX wählen", "Bitte einen Wiedergabe-Stream wählen."); return }
        let entry = config.streams[selected], s = entry.sdp
        guard s.mediaClockType != "local-test" else { alert("Mac-Test nicht exportieren", "Das Mac-Test-Profil ist ein lokaler RTP-Test ohne PTP. Es kann kein produktives AES67-SDP mit PTP-Referenzclock exportieren."); return }
        let clockURL = URL(fileURLWithPath: "/Library/Application Support/AES67Driver/clock-status.txt")
        guard let clock = try? String(contentsOf: clockURL, encoding: .utf8) else {
            alert("PTP noch nicht bereit", "Ein TX-SDP kann erst nach erfolgreicher Clock-Synchronisation auf diesem Mac exportiert werden."); return
        }
        let parts = clock.split(whereSeparator: \.isWhitespace).map(String.init)
        let nowNs = UInt64(Date().timeIntervalSince1970 * 1_000_000_000)
        guard parts.count >= 8, parts[0] == "1", let stamp = UInt64(parts[1]),
              nowNs >= stamp, nowNs - stamp < 3_000_000_000, parts[3] == "1",
              Int(parts[4]) == s.ptpDomain, parts[5] == entry.networkInterface,
              parts[6] == s.originAddress, parts[7] != "-" else {
            alert("PTP noch nicht bereit", "Clock-Lock fehlt, ist veraltet oder gehört zu einer anderen Schnittstelle/Domain."); return
        }
        let gm = parts[7].replacingOccurrences(of: ":", with: "-")
        guard gm.split(separator: "-").count == 8,
              gm.split(separator: "-").allSatisfy({ $0.count == 2 && UInt8($0, radix: 16) != nil }) else {
            alert("PTP-Identität ungültig", "Clock-Service meldet keine gültige Grandmaster-ID."); return
        }
        let panel = NSSavePanel(); panel.allowedFileTypes = ["sdp"]; panel.nameFieldStringValue = s.sessionName + ".sdp"
        guard panel.runModal() == .OK, let url = panel.url else { return }
        let body = """
        v=0
        o=\(s.originUsername) \(s.sessionID) \(s.sessionVersion) IN IP4 \(s.originAddress)
        s=\(s.sessionName)
        c=IN IP4 \(s.connectionAddress)/\(s.ttl)
        t=0 0
        m=audio \(s.port) RTP/AVP \(s.payloadType)
        a=rtpmap:\(s.payloadType) \(s.encoding)/\(s.sampleRate)/\(s.numChannels)
        a=ptime:\(s.ptime)
        a=framecount:\(s.framecount)
        a=ts-refclk:ptp=IEEE1588-2008:\(gm):\(s.ptpDomain)
        a=mediaclk:\(s.mediaClockType)
        a=sendonly

        """
        do { try body.write(to: url, atomically: true, encoding: .utf8) }
        catch { alert("Export fehlgeschlagen", error.localizedDescription) }
    }
    func store() -> Bool {
        guard let (inputs, outputs) = requestedCounts() else { return false }
        if let settings = config.easySetup, inputs != settings.channels || outputs != settings.channels { leaveAutomaticMode() }
        for (i, s) in config.streams.enumerated() {
            guard validate(s, skipping: i) else { return false }
            let m = s.mapping
            let full = m.deviceChannelCount == s.sdp.numChannels && m.channelMap.isEmpty
            let mapped = m.channelMap.filter { $0 >= 0 }
            let subset = s.sdp.direction != "sendonly" && m.channelMap.count == s.sdp.numChannels &&
                mapped.count == m.deviceChannelCount && Set(mapped).count == mapped.count && m.channelMap.allSatisfy { $0 >= -1 && $0 < inputs }
            guard (1...8).contains(s.sdp.numChannels), m.streamChannelCount == s.sdp.numChannels,
                  m.streamChannelOffset == 0, full || subset,
                  s.sdp.sampleRate == 48000, s.sdp.encoding == "L24" || s.sdp.encoding == "L16" else {
                alert("Konfiguration ungültig", "Stream \(s.sdp.sessionName) hat ein nicht unterstütztes Format."); return false
            }
        }
        do {
            try FileManager.default.createDirectory(at: stage.deletingLastPathComponent(), withIntermediateDirectories: true)
            let coder = JSONEncoder(); coder.outputFormatting = [.prettyPrinted, .sortedKeys]
            var saved = config; saved.inputChannels = inputs; saved.outputChannels = outputs
            try coder.encode(saved).write(to: stage, options: .atomic)
            config = saved
            return true
        } catch { alert("Speichern fehlgeschlagen", error.localizedDescription); return false }
    }
    @objc func save() {
        window.makeFirstResponder(nil)
        if store() { alert("Entwurf gespeichert", "Der Entwurf liegt in deinem Benutzerordner. „Änderungen anwenden …“ aktiviert ihn für den Treiber. Dafür ist kein Mac-Neustart erforderlich.") }
    }
    func quote(_ s: String) -> String { "'" + s.replacingOccurrences(of: "'", with: "'\\''") + "'" }
    @objc func apply() {
        window.makeFirstResponder(nil)
        guard store(), let stagedData = try? Data(contentsOf: stage) else { return }
        if stagedData == lastActivatedData, let installedData = try? Data(contentsOf: system), installedData == stagedData,
           let published = publishedDeviceChannels(), published.input == config.inputChannels, published.output == config.outputChannels {
            alert("Bereits angewendet", "Seit der letzten erfolgreichen Aktivierung gibt es keine Änderungen. Audio läuft weiter.")
            return
        }
        guard FileManager.default.fileExists(atPath: driver) else {
            alert("Treiber fehlt", "Der Entwurf ist gespeichert. Installiere zuerst das AES67-Paket, damit die Einstellungen aktiviert werden können.")
            return
        }
        // Atomic configuration replacement precedes the bounded service reload.
        // The clock helper observes configuration changes itself once per second.
        // Keep partial success distinct so a reload failure can be retried.
        let command = "/usr/bin/install -m 0644 -o root -g wheel " + quote(stage.path) + " " + quote(system.path + ".new") +
            " && /bin/mv -f " + quote(system.path + ".new") + " " + quote(system.path) +
            " && { if /bin/launchctl kickstart -k system/com.apple.audio.coreaudiod >/dev/null 2>&1" +
            " || /usr/bin/killall -TERM coreaudiod >/dev/null 2>&1; then /usr/bin/printf AES67_RELOADED;" +
            " else /usr/bin/printf AES67_SAVED_RELOAD_FAILED; fi; }"
        ManagerLog.write("Änderungen anwenden: \(config.streams.count) Stream(s), \(config.inputChannels) × \(config.outputChannels) Kanäle, Anschluss \(chosenPort?.name ?? "-")")
        let escaped = command.replacingOccurrences(of: "\\", with: "\\\\").replacingOccurrences(of: "\"", with: "\\\"")
        var error: NSDictionary?
        guard let script = NSAppleScript(source: "do shell script \"\(escaped)\" with administrator privileges") else {
            alert("Nicht angewendet", "macOS konnte die Administratorabfrage nicht vorbereiten.")
            return
        }
        let result = script.executeAndReturnError(&error)
        if let e = error {
            alert("Nicht angewendet", String(describing: e[NSAppleScript.errorMessage] ?? "Administratorabfrage abgebrochen."))
        } else if result.stringValue == "AES67_RELOADED" {
            lastActivatedData = stagedData; portNotice = ""; addressNotice = ""
            updateStatus()
            alert("Gespeichert · Audio wird neu geladen", "CoreAudio lädt den Treiber mit den neuen Einstellungen neu. Kein Mac-Neustart nötig. Audio-Apps müssen ihr Gerät gegebenenfalls erneut öffnen oder „AES67 Device“ neu auswählen. PTP benötigt anschließend wieder Zeit zur Synchronisation.")
        } else {
            lastActivatedData = nil
            alert("Gespeichert · Aktivierung noch ausstehend", "Die Systemkonfiguration wurde gespeichert, aber macOS konnte den Audiodienst nicht neu starten. „Änderungen anwenden …“ erneut versuchen. Falls macOS den Neustart weiterhin blockiert, den Mac einmal neu starten. Die neuen Einstellungen sind noch nicht als aktiv bestätigt.")
        }
    }
    @objc func discover() { guidedDiscover() }
    func stopDiscovery() {
        let process = discoveryProcess
        discoveryProcess = nil; discoveryInterface = nil
        process?.terminationHandler = nil
        if process?.isRunning == true { process?.terminate() }
        if let directory = discoveryDirectory { try? FileManager.default.removeItem(at: directory) }
        discoveryDirectory = nil; nextDiscovery = .distantPast
    }
    func collectDiscovery() {
        let files = discoveryDirectory.flatMap { try? FileManager.default.contentsOfDirectory(at: $0, includingPropertiesForKeys: nil) } ?? []
        var changed = false
        for file in files where file.pathExtension == "sdp" {
            guard let data = try? String(contentsOf: file, encoding: .utf8), let s = importSession(data) else { continue }
            // Suppress our own announcements; select remote sender flows only. An
            // announcement from one of this Mac's addresses is ours even after the
            // sender address changed (old SAP still circulating).
            if config.streams.contains(where: { $0.sdp.direction == "sendonly" && $0.sdp.connectionAddress == s.connectionAddress && $0.sdp.port == s.port && $0.sdp.originAddress == s.originAddress }) { continue }
            if ports.contains(where: { $0.ip == s.originAddress }) { continue }
            let id = sourceIdentity(s)
            // File mtime represents a new announcement, not each UI refresh.
            let attrs = try? FileManager.default.attributesOfItem(atPath: file.path)
            let seen = attrs?[.modificationDate] as? Date ?? Date()
            if let previous = discovered[id], previous.sdp.sessionVersion > s.sessionVersion || previous.seen > seen { continue }
            if discovered[id]?.sdp != s { changed = true }
            discovered[id] = (sdp: s, seen: max(seen, discovered[id]?.seen ?? .distantPast))
        }
        let cutoff = Date().addingTimeInterval(-95)
        let fresh = discovered.filter { $0.value.seen >= cutoff }
        if fresh.count != discovered.count { changed = true }; discovered = fresh
        let found = consoles
        if let settings = config.easySetup, settings.device == nil, settings.selectedSources.isEmpty, !autoSelectDeclined, found.count == 1 {
            chooseDevice(found[0], automatically: true)
        } else if changed { _ = rebuildSetup(reportErrors: false) }
        refreshSources()
    }
    func serviceDiscovery() {
        guard !headless else { return }
        guard let chosen = chosenPort,
              !config.streams.allSatisfy({ $0.sdp.mediaClockType == "local-test" }) || config.streams.isEmpty else {
            stopDiscovery(); discovered.removeAll(); discoveryMessage = chosenInterfaceName == nil ? "Wartet auf die Auswahl des Ethernet-Anschlusses." : "Keine SAP-Suche: Anschluss nicht verfügbar oder Mac-Test aktiv."
            refreshSources(); return
        }
        if let previous = discoveryInterface, previous != chosen { stopDiscovery(); discovered.removeAll() }
        if discoveryInterface != chosen { _ = rebuildSetup(reportErrors: false) }
        collectDiscovery()
        guard discoveryProcess == nil, Date() >= nextDiscovery else { return }
        guard let tool = Bundle.main.resourceURL?.appendingPathComponent("AES67Discover"), FileManager.default.isExecutableFile(atPath: tool.path) else {
            discoveryMessage = "AES67Discover fehlt im App-Paket. SDP-Datei importieren oder vollständige App installieren."; refreshSources(); return
        }
        let directory = FileManager.default.temporaryDirectory.appendingPathComponent("AES67-SAP-\(UUID().uuidString)", isDirectory: true)
        do { try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true) }
        catch { discoveryMessage = "SAP-Suche konnte nicht starten: \(error.localizedDescription)"; refreshSources(); return }
        let process = Process(); process.executableURL = tool
        process.arguments = ["--interface", chosen.ip, "--output", directory.path, "--seconds", "30"]
        // The tool reports socket and multicast errors on stderr; keep them.
        let errors = Pipe()
        process.standardOutput = FileHandle.nullDevice; process.standardError = errors
        discoveryProcess = process; discoveryDirectory = directory; discoveryInterface = chosen
        process.terminationHandler = { [weak self] task in
            let stderrText = String(data: errors.fileHandleForReading.readDataToEndOfFile(), encoding: .utf8) ?? ""
            DispatchQueue.main.async {
                if !stderrText.isEmpty { ManagerLog.write("AES67Discover (Status \(task.terminationStatus)): \(stderrText.trimmingCharacters(in: .whitespacesAndNewlines))") }
                else if task.terminationStatus != 0 { ManagerLog.write("AES67Discover beendet mit Status \(task.terminationStatus)") }
                guard let self, self.discoveryProcess === task else { return }
                self.collectDiscovery(); self.discoveryProcess = nil
                if let directory = self.discoveryDirectory { try? FileManager.default.removeItem(at: directory) }
                self.discoveryDirectory = nil
                self.nextDiscovery = Date().addingTimeInterval(task.terminationStatus == 0 ? 1 : 15)
                self.discoveryMessage = task.terminationStatus == 0
                    ? "SAP-Suche auf \(chosen.name) aktiv · \(self.discovered.count) aktuelle Quellen. Wartet auf weitere Ankündigungen."
                    : "SAP-Suche auf \(chosen.name) fehlgeschlagen; erneuter Versuch folgt. Anschluss und Multicast-Freigabe prüfen."
                self.refreshSources()
            }
        }
        do {
            try process.run(); try? errors.fileHandleForWriting.close(); discoveryMessage = "SAP-Suche auf \(chosen.name) läuft im Hintergrund. Wartet auf Pult-Ankündigungen …"
        } catch {
            discoveryProcess = nil; discoveryDirectory = nil
            try? FileManager.default.removeItem(at: directory); nextDiscovery = Date().addingTimeInterval(15)
            discoveryMessage = "SAP-Suche konnte nicht starten: \(error.localizedDescription)"
            ManagerLog.write(discoveryMessage)
        }
        refreshSources()
    }
}
