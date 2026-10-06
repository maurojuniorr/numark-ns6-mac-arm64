import AppKit
import CoreAudio
import IOKit

private let vendorID: UInt16 = 0x15e4
private let productID: UInt16 = 0x0079
private let driverVersion = Bundle(path: "/Library/Audio/Plug-Ins/HAL/NumarkNS6M1.driver")?
    .object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String ?? "Unavailable"
private let driverDeveloper = "Mauro Junior (@maurojuniorr)"
private let audioDeviceUID = "io.github.maurojuniorr.numark-ns6.device"
private let activeIOFramesProperty: AudioObjectPropertySelector = 0x6e733666 // 'ns6f'
private let activeClientProperty: AudioObjectPropertySelector = 0x6e733663 // 'ns6c'
private let firmwareVersionProperty: AudioObjectPropertySelector = 0x6e733676 // 'ns6v'
private let audioPropertyQueue = DispatchQueue(label: "io.github.maurojuniorr.numark-ns6.status.audio-properties", qos: .utility)
private let sampleRate = 44100.0

private struct ActiveAudioClient {
    let processID: pid_t
    let bundleID: String
    var displayName: String {
        if let app = NSRunningApplication(processIdentifier: processID), let name = app.localizedName { return name }
        return bundleID.isEmpty ? "DJ application" : (bundleID as NSString).lastPathComponent
    }
}

private struct StatusSnapshot {
    let usbConnected: Bool
    let audioDevice: AudioDeviceID?
    let firmwareVersion: String?
    let activeFlowSummary: String
}

private func audioDeviceID() -> AudioDeviceID? {
    var address = AudioObjectPropertyAddress(mSelector: kAudioHardwarePropertyDevices, mScope: kAudioObjectPropertyScopeGlobal, mElement: kAudioObjectPropertyElementMain)
    var byteCount: UInt32 = 0
    guard AudioObjectGetPropertyDataSize(AudioObjectID(kAudioObjectSystemObject), &address, 0, nil, &byteCount) == noErr else { return nil }
    let count = Int(byteCount) / MemoryLayout<AudioDeviceID>.size
    var devices = Array(repeating: AudioDeviceID(0), count: count)
    guard AudioObjectGetPropertyData(AudioObjectID(kAudioObjectSystemObject), &address, 0, nil, &byteCount, &devices) == noErr else { return nil }
    for device in devices {
        var uidAddress = AudioObjectPropertyAddress(mSelector: kAudioDevicePropertyDeviceUID, mScope: kAudioObjectPropertyScopeGlobal, mElement: kAudioObjectPropertyElementMain)
        var uid: Unmanaged<CFString>?
        var size = UInt32(MemoryLayout.size(ofValue: uid))
        if AudioObjectGetPropertyData(device, &uidAddress, 0, nil, &size, &uid) == noErr,
           let value = uid?.takeRetainedValue() as String?, value == audioDeviceUID { return device }
    }
    return nil
}

private func driverFirmwareVersion(_ device: AudioDeviceID) -> String? {
    var address = AudioObjectPropertyAddress(mSelector: firmwareVersionProperty, mScope: kAudioObjectPropertyScopeGlobal, mElement: kAudioObjectPropertyElementMain)
    var value: Unmanaged<CFString>?
    var size = UInt32(MemoryLayout<Unmanaged<CFString>?>.size)
    guard AudioObjectGetPropertyData(device, &address, 0, nil, &size, &value) == noErr,
          let text = value?.takeRetainedValue() as String?,
          text != "Unavailable" else { return nil }
    return text
}

private func activeIOFrames(_ device: AudioDeviceID) -> UInt32? {
    var address = AudioObjectPropertyAddress(mSelector: activeIOFramesProperty, mScope: kAudioObjectPropertyScopeGlobal, mElement: kAudioObjectPropertyElementMain)
    var value: Unmanaged<CFString>?
    var size = UInt32(MemoryLayout<Unmanaged<CFString>?>.size)
    guard AudioObjectGetPropertyData(device, &address, 0, nil, &size, &value) == noErr,
          let text = value?.takeRetainedValue() as String?,
          let frames = UInt32(text), frames > 0 else { return nil }
    return frames
}

private func activeAudioClient(_ device: AudioDeviceID) -> ActiveAudioClient? {
    var address = AudioObjectPropertyAddress(mSelector: activeClientProperty, mScope: kAudioObjectPropertyScopeGlobal, mElement: kAudioObjectPropertyElementMain)
    var value: Unmanaged<CFString>?
    var size = UInt32(MemoryLayout<Unmanaged<CFString>?>.size)
    guard AudioObjectGetPropertyData(device, &address, 0, nil, &size, &value) == noErr,
          let text = value?.takeRetainedValue() as String? else { return nil }
    let pieces = text.split(separator: "|", maxSplits: 1, omittingEmptySubsequences: false)
    guard pieces.count == 2, let processID = pid_t(pieces[0]), processID > 0 else { return nil }
    return ActiveAudioClient(processID: processID, bundleID: String(pieces[1]))
}

private func uint32Property(_ object: AudioObjectID, selector: AudioObjectPropertySelector, scope: AudioObjectPropertyScope) -> UInt32 {
    var address = AudioObjectPropertyAddress(mSelector: selector, mScope: scope, mElement: kAudioObjectPropertyElementMain)
    var value: UInt32 = 0
    var size = UInt32(MemoryLayout<UInt32>.size)
    guard AudioObjectGetPropertyData(object, &address, 0, nil, &size, &value) == noErr else { return 0 }
    return value
}

private func firstStreamLatencyFrames(_ device: AudioDeviceID) -> UInt32 {
    var address = AudioObjectPropertyAddress(mSelector: kAudioDevicePropertyStreams, mScope: kAudioObjectPropertyScopeOutput, mElement: kAudioObjectPropertyElementMain)
    var size: UInt32 = 0
    guard AudioObjectGetPropertyDataSize(device, &address, 0, nil, &size) == noErr,
          size >= UInt32(MemoryLayout<AudioStreamID>.size) else { return 0 }
    var streams = Array(repeating: AudioStreamID(0), count: Int(size) / MemoryLayout<AudioStreamID>.size)
    guard AudioObjectGetPropertyData(device, &address, 0, nil, &size, &streams) == noErr,
          let stream = streams.first else { return 0 }
    return uint32Property(stream, selector: kAudioStreamPropertyLatency, scope: kAudioObjectPropertyScopeOutput)
}

private func usbConnected() -> Bool {
    var iterator: io_iterator_t = 0
    guard IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceMatching("IOUSBHostDevice"), &iterator) == KERN_SUCCESS else { return false }
    defer { IOObjectRelease(iterator) }
    while true {
        let service = IOIteratorNext(iterator)
        if service == 0 { return false }
        defer { IOObjectRelease(service) }
        let vendor = IORegistryEntryCreateCFProperty(service, "idVendor" as CFString, kCFAllocatorDefault, 0)?.takeRetainedValue() as? NSNumber
        let product = IORegistryEntryCreateCFProperty(service, "idProduct" as CFString, kCFAllocatorDefault, 0)?.takeRetainedValue() as? NSNumber
        if vendor?.uint16Value == vendorID && product?.uint16Value == productID { return true }
    }
}

final class StatusController: NSViewController {
    private let statusLabel = NSTextField(labelWithString: "")
    private let stateDot = NSView()
    private var refreshInProgress = false
    private var valueLabels: [NSTextField] = []
    private var timer: Timer?

    override func loadView() {
        let root = NSView()
        root.wantsLayer = true
        root.layer?.backgroundColor = NSColor.windowBackgroundColor.cgColor
        view = root

        let logo = NSTextField(labelWithString: "NUMARK")
        logo.font = .systemFont(ofSize: 48, weight: .black)
        logo.textColor = .labelColor
        logo.alignment = .center

        let title = NSTextField(labelWithString: "NS6 Audio Driver")
        title.font = .systemFont(ofSize: 15, weight: .semibold)
        title.textColor = .secondaryLabelColor
        title.alignment = .center

        let rows = [
            ("Device", "Numark NS6"),
            ("Audio Inputs", "Not implemented"),
            ("Audio Outputs", "4"),
            ("MIDI Input", "CoreMIDI bridge"),
            ("Clock Rate", "44.1 kHz"),
            ("Word Length", "24-bit packed"),
            ("Driver Version", driverVersion),
            ("Driver Developer", driverDeveloper),
            ("Firmware Version", "Unavailable"),
            ("Active Audio Flow", "Waiting for audio")
        ]
        let details = NSStackView()
        details.orientation = .vertical
        details.alignment = .leading
        details.spacing = 9
        for (name, value) in rows {
            let label = NSTextField(labelWithString: name)
            label.font = .systemFont(ofSize: 14, weight: .semibold)
            let valueLabel = NSTextField(labelWithString: value)
            valueLabel.font = .monospacedSystemFont(ofSize: 13, weight: .regular)
            valueLabel.alignment = .left
            if name == "Active Audio Flow" {
                valueLabel.cell?.wraps = true
                valueLabel.maximumNumberOfLines = 0
                valueLabel.lineBreakMode = .byWordWrapping
                valueLabel.setContentCompressionResistancePriority(.defaultLow, for: .horizontal)
            } else {
                valueLabel.setContentCompressionResistancePriority(.required, for: .horizontal)
            }
            valueLabels.append(valueLabel)
            let row = NSStackView(views: [label, valueLabel])
            row.orientation = .horizontal
            row.distribution = .fill
            row.alignment = name == "Active Audio Flow" ? .top : .centerY
            label.widthAnchor.constraint(equalToConstant: 145).isActive = true
            label.setContentHuggingPriority(.required, for: .horizontal)
            details.addArrangedSubview(row)
            row.widthAnchor.constraint(equalTo: details.widthAnchor).isActive = true
        }

        stateDot.wantsLayer = true
        stateDot.layer?.cornerRadius = 6
        stateDot.translatesAutoresizingMaskIntoConstraints = false
        NSLayoutConstraint.activate([stateDot.widthAnchor.constraint(equalToConstant: 12), stateDot.heightAnchor.constraint(equalToConstant: 12)])
        statusLabel.font = .systemFont(ofSize: 14, weight: .bold)
        let state = NSStackView(views: [stateDot, statusLabel])
        state.orientation = .horizontal
        state.alignment = .centerY
        state.spacing = 8
        state.edgeInsets = NSEdgeInsets(top: 12, left: 14, bottom: 12, right: 14)
        state.wantsLayer = true
        state.layer?.cornerRadius = 8
        state.layer?.borderWidth = 1
        state.layer?.borderColor = NSColor.separatorColor.cgColor

        let stack = NSStackView(views: [logo, title, details, state])
        stack.orientation = .vertical
        stack.alignment = .centerX
        stack.spacing = 16
        stack.translatesAutoresizingMaskIntoConstraints = false
        root.addSubview(stack)
        NSLayoutConstraint.activate([
            stack.leadingAnchor.constraint(equalTo: root.leadingAnchor, constant: 28),
            stack.trailingAnchor.constraint(equalTo: root.trailingAnchor, constant: -28),
            stack.topAnchor.constraint(equalTo: root.topAnchor, constant: 28),
            stack.bottomAnchor.constraint(equalTo: root.bottomAnchor, constant: -24),
            details.widthAnchor.constraint(equalTo: stack.widthAnchor),
            state.widthAnchor.constraint(equalTo: stack.widthAnchor)
        ])
    }

    override func viewDidAppear() {
        super.viewDidAppear()
        refresh()
        timer = Timer.scheduledTimer(withTimeInterval: 1, repeats: true) { [weak self] _ in self?.refresh() }
    }

    override func viewWillDisappear() { timer?.invalidate(); timer = nil; super.viewWillDisappear() }

    private func refresh() {
        guard !refreshInProgress else { return }
        refreshInProgress = true
        audioPropertyQueue.async { [weak self] in
            let connected = usbConnected()
            let device = audioDeviceID()
            var firmwareVersion: String?
            var flowSummary = "No active audio flow"
            if let device {
                firmwareVersion = driverFirmwareVersion(device)
                if let frames = activeIOFrames(device) {
                    flowSummary = activeAudioClient(device)?.displayName ?? "CoreAudio client"
                        let fixedFrames = uint32Property(device, selector: kAudioDevicePropertyLatency, scope: kAudioObjectPropertyScopeOutput)
                            + uint32Property(device, selector: kAudioDevicePropertySafetyOffset, scope: kAudioObjectPropertyScopeOutput)
                            + firstStreamLatencyFrames(device)
                        let periodMilliseconds = Double(frames) * 1000.0 / sampleRate
                        let reportedMilliseconds = Double(frames + fixedFrames) * 1000.0 / sampleRate
                        flowSummary += String(format: "\nI/O buffer: %u samples (%.2f ms)\nReported latency: ~%.2f ms", frames, periodMilliseconds, reportedMilliseconds)
                }
            }
            let snapshot = StatusSnapshot(usbConnected: connected, audioDevice: device, firmwareVersion: firmwareVersion, activeFlowSummary: flowSummary)
            DispatchQueue.main.async {
                guard let self else { return }
                self.refreshInProgress = false
                self.apply(snapshot)
            }
        }
    }

    private func apply(_ snapshot: StatusSnapshot) {
        let ready = snapshot.usbConnected && snapshot.audioDevice != nil
        statusLabel.stringValue = ready ? "CONNECTED — audio driver ready" : (snapshot.usbConnected ? "CONNECTED — waiting for audio driver" : "NO DEVICE")
        stateDot.layer?.backgroundColor = (ready ? NSColor.systemGreen : (snapshot.usbConnected ? NSColor.systemOrange : NSColor.systemRed)).cgColor
        valueLabels[0].stringValue = snapshot.usbConnected ? "Numark NS6 (USB)" : "—"
        valueLabels[8].stringValue = snapshot.usbConnected ? (snapshot.firmwareVersion ?? "Unavailable") : "—"
        valueLabels[9].stringValue = ready ? snapshot.activeFlowSummary : "—"
    }

}

final class AppDelegate: NSObject, NSApplicationDelegate {
    private var statusWindow: NSWindow?

    func applicationDidFinishLaunching(_ notification: Notification) {
        let controller = StatusController()
        let window = NSWindow(contentViewController: controller)
        window.title = "Numark NS6 Status"
        window.setContentSize(NSSize(width: 600, height: 560))
        window.styleMask = [.titled, .closable, .miniaturizable]
        window.isReleasedWhenClosed = false
        window.center()
        statusWindow = window
        window.makeKeyAndOrderFront(nil)
        NSApp.activate(ignoringOtherApps: true)
        window.orderFrontRegardless()
        window.displayIfNeeded()
    }

    func applicationShouldHandleReopen(_ sender: NSApplication, hasVisibleWindows flag: Bool) -> Bool {
        guard let window = statusWindow else { return false }
        window.makeKeyAndOrderFront(nil)
        sender.activate(ignoringOtherApps: true)
        return true
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { true }
}

let app = NSApplication.shared
let delegate = AppDelegate()
app.delegate = delegate
app.setActivationPolicy(.regular)
app.run()
