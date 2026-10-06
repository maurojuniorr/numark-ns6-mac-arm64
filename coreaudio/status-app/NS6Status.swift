import AppKit
import CoreAudio
import IOKit

private let vendorID: UInt16 = 0x15e4
private let productID: UInt16 = 0x0079
private let driverVersion = Bundle.main.infoDictionary?["CFBundleShortVersionString"] as? String ?? "Unknown"
private let driverDeveloper = "Mauro Junior (@maurojuniorr)"
private let audioDeviceUID = "io.github.maurojuniorr.numark-ns6.device"
private let driverBufferProperty: AudioObjectPropertySelector = 0x6e733662 // 'ns6b'
private let driverBufferRestartProperty: AudioObjectPropertySelector = 0x6e733672 // 'ns6r'
private let activeIOFramesProperty: AudioObjectPropertySelector = 0x6e733666 // 'ns6f'
private let activeClientProperty: AudioObjectPropertySelector = 0x6e733663 // 'ns6c'
private let supportedBufferFrames: [UInt32] = [49, 128, 192, 256, 512, 1024]
private let audioPropertyQueue = DispatchQueue(label: "io.github.maurojuniorr.numark-ns6.status.audio-properties", qos: .utility)
private let bufferConfigurationQueue = DispatchQueue(label: "io.github.maurojuniorr.numark-ns6.status.buffer-configuration", qos: .userInitiated)
private let sampleRate = 44100.0

private struct MixxxStreamInfo {
    let frames: UInt32
    let bufferMilliseconds: Double
    let reportedMilliseconds: Double
}

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
    let bufferFrames: UInt32?
    let bufferSettable: Bool
    let bufferRestartState: String?
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

private func driverBufferFrames(_ device: AudioDeviceID) -> UInt32? {
    var address = AudioObjectPropertyAddress(mSelector: driverBufferProperty, mScope: kAudioObjectPropertyScopeGlobal, mElement: kAudioObjectPropertyElementMain)
    var value: Unmanaged<CFString>?
    var size = UInt32(MemoryLayout<Unmanaged<CFString>?>.size)
    guard AudioObjectGetPropertyData(device, &address, 0, nil, &size, &value) == noErr,
          let text = value?.takeRetainedValue() as String?,
          let frames = UInt32(text) else { return nil }
    return frames
}

private func driverBufferRestartState(_ device: AudioDeviceID) -> String? {
    var address = AudioObjectPropertyAddress(mSelector: driverBufferRestartProperty, mScope: kAudioObjectPropertyScopeGlobal, mElement: kAudioObjectPropertyElementMain)
    var value: Unmanaged<CFString>?
    var size = UInt32(MemoryLayout<Unmanaged<CFString>?>.size)
    guard AudioObjectGetPropertyData(device, &address, 0, nil, &size, &value) == noErr,
          let text = value?.takeRetainedValue() as String? else { return nil }
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

private func lastMixxxStreamInfo() -> MixxxStreamInfo? {
    guard NSRunningApplication.runningApplications(withBundleIdentifier: "org.mixxx.mixxx").contains(where: { !$0.isTerminated }) else { return nil }
    let logURL = URL(fileURLWithPath: NSHomeDirectory())
        .appendingPathComponent("Library/Containers/org.mixxx.mixxx/Data/Library/Application Support/Mixxx/mixxx.log")
    guard let modified = try? logURL.resourceValues(forKeys: [.contentModificationDateKey]).contentModificationDate,
          Date().timeIntervalSince(modified) < 600,
          let log = try? String(contentsOf: logURL, encoding: .utf8) else { return nil }
    let lines = log.components(separatedBy: .newlines)
    guard let index = lines.lastIndex(where: { $0.contains("framesPerBuffer:") }) else { return nil }
    let end = min(index + 12, lines.count)
    let streamStart = lines[index..<end].joined(separator: "\n")
    guard let framesText = capture(#"framesPerBuffer:\s*(\d+)"#, from: streamStart),
          let frames = UInt32(framesText),
          let bufferText = capture(#"buffer size:\s*([0-9.]+)\s*ms"#, from: streamStart),
          let bufferMilliseconds = Double(bufferText),
          let reportedText = capture(#"Actual sample rate:.*?latency:\s*([0-9.]+)\s*ms"#, from: streamStart),
          let reportedMilliseconds = Double(reportedText) else { return nil }
    return MixxxStreamInfo(frames: frames, bufferMilliseconds: bufferMilliseconds, reportedMilliseconds: reportedMilliseconds)
}

private func capture(_ pattern: String, from text: String) -> String? {
    guard let regex = try? NSRegularExpression(pattern: pattern),
          let match = regex.firstMatch(in: text, range: NSRange(text.startIndex..., in: text)),
          let range = Range(match.range(at: 1), in: text) else { return nil }
    return String(text[range])
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
    private let bufferPopup = NSPopUpButton()
    private let bufferLatencyLabel = NSTextField(labelWithString: "")
    private let applyBufferButton = NSButton(title: "Apply", target: nil, action: nil)
    private let bufferStatusLabel = NSTextField(labelWithString: "")
    private var updatingBufferPopup = false
    private var refreshInProgress = false
    private var applyingBuffer = false
    private var bufferApplyAttempt: UUID?
    private var currentBufferFrames: UInt32?
    private var pendingBufferFrames: UInt32?
    private var displayedBufferFrames = supportedBufferFrames
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
            ("Firmware Version", "Unavailable (vendor query pending)"),
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
            valueLabels.append(valueLabel)
            let row = NSStackView(views: [label, valueLabel])
            row.orientation = .horizontal
            row.distribution = .fill
            row.alignment = .centerY
            label.widthAnchor.constraint(equalToConstant: 145).isActive = true
            label.setContentHuggingPriority(.required, for: .horizontal)
            valueLabel.setContentCompressionResistancePriority(.required, for: .horizontal)
            details.addArrangedSubview(row)
        }

        let bufferTitle = NSTextField(labelWithString: "USB Startup Buffer")
        bufferTitle.font = .systemFont(ofSize: 14, weight: .semibold)
        bufferTitle.widthAnchor.constraint(equalToConstant: 145).isActive = true
        bufferTitle.setContentHuggingPriority(.required, for: .horizontal)
        for frames in supportedBufferFrames { bufferPopup.addItem(withTitle: "\(frames) samples") }
        bufferPopup.target = self
        bufferPopup.action = #selector(selectBufferSize(_:))
        bufferPopup.widthAnchor.constraint(equalToConstant: 155).isActive = true
        bufferPopup.isEnabled = false
        applyBufferButton.target = self
        applyBufferButton.action = #selector(applyBufferSize(_:))
        applyBufferButton.bezelStyle = .rounded
        applyBufferButton.isEnabled = false
        bufferLatencyLabel.font = .monospacedSystemFont(ofSize: 12, weight: .regular)
        bufferLatencyLabel.textColor = .secondaryLabelColor
        bufferLatencyLabel.stringValue = "Checking driver…"
        bufferStatusLabel.font = .systemFont(ofSize: 12)
        bufferStatusLabel.textColor = .secondaryLabelColor
        bufferStatusLabel.stringValue = "This startup pre-buffer is separate from the active-flow latency shown above and does not change the application's I/O buffer. Apply briefly restarts USB audio."
        let bufferRow = NSStackView(views: [bufferTitle, bufferPopup, bufferLatencyLabel, applyBufferButton])
        bufferRow.orientation = .horizontal
        bufferRow.alignment = .centerY
        bufferRow.spacing = 8
        let bufferSection = NSStackView(views: [bufferRow, bufferStatusLabel])
        bufferSection.orientation = .vertical
        bufferSection.alignment = .leading
        bufferSection.spacing = 6

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

        let stack = NSStackView(views: [logo, title, details, bufferSection, state])
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
        bufferSection.widthAnchor.constraint(equalTo: details.widthAnchor).isActive = true
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
            var currentFrames: UInt32?
            var restartState: String?
            var canSetBuffer = false
            var flowSummary = "No active audio flow"
            if let device {
                var address = AudioObjectPropertyAddress(mSelector: driverBufferProperty, mScope: kAudioObjectPropertyScopeGlobal, mElement: kAudioObjectPropertyElementMain)
                if let frames = driverBufferFrames(device) {
                    currentFrames = frames
                    var settable = DarwinBoolean(false)
                    canSetBuffer = AudioObjectIsPropertySettable(device, &address, &settable) == noErr && settable.boolValue
                }
                restartState = driverBufferRestartState(device)
                let running = uint32Property(device, selector: kAudioDevicePropertyDeviceIsRunning, scope: kAudioObjectPropertyScopeGlobal) != 0
                let client = activeAudioClient(device)
                if let callbackFrames = activeIOFrames(device) {
                    let fixedFrames = uint32Property(device, selector: kAudioDevicePropertyLatency, scope: kAudioObjectPropertyScopeOutput)
                        + uint32Property(device, selector: kAudioDevicePropertySafetyOffset, scope: kAudioObjectPropertyScopeOutput)
                        + firstStreamLatencyFrames(device)
                    let totalFrames = callbackFrames + fixedFrames
                    let milliseconds = Double(totalFrames) * 1000.0 / sampleRate
                    if let client {
                        if client.bundleID == "org.mixxx.mixxx", let mixxx = lastMixxxStreamInfo() {
                            flowSummary = "\(client.displayName) opened \(mixxx.frames)f/\(String(format: "%.2f", mixxx.bufferMilliseconds))ms · reported \(String(format: "%.2f", mixxx.reportedMilliseconds))ms · HAL cycle \(callbackFrames)f"
                        } else {
                            flowSummary = "\(client.displayName) active · app buffer unavailable · HAL cycle \(callbackFrames)f/~\(String(format: "%.2f", milliseconds))ms"
                        }
                    } else {
                        flowSummary = "Active app unavailable · HAL cycle \(callbackFrames)f/~\(String(format: "%.2f", milliseconds))ms"
                    }
                } else if running {
                    flowSummary = "CoreAudio running · callback size not observed yet"
                } else {
                    flowSummary = "No callback observed · CoreAudio reports idle"
                }
            }
            let snapshot = StatusSnapshot(usbConnected: connected, audioDevice: device, bufferFrames: currentFrames, bufferSettable: canSetBuffer, bufferRestartState: restartState, activeFlowSummary: flowSummary)
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
        valueLabels[8].stringValue = snapshot.usbConnected ? "Unavailable (vendor query pending)" : "—"
        valueLabels[9].stringValue = ready ? snapshot.activeFlowSummary : "—"
        guard ready, let frames = snapshot.bufferFrames else {
            bufferPopup.isEnabled = false
            applyBufferButton.isEnabled = false
            bufferLatencyLabel.stringValue = snapshot.usbConnected ? "Driver unavailable" : "Connect NS6"
            return
        }
        currentBufferFrames = frames
        if !applyingBuffer {
            updatingBufferPopup = true
            if !supportedBufferFrames.contains(frames) {
                if !displayedBufferFrames.contains(frames) {
                    displayedBufferFrames.append(frames)
                    bufferPopup.addItem(withTitle: "\(frames) samples (active)")
                }
            } else if displayedBufferFrames.count > supportedBufferFrames.count {
                displayedBufferFrames.removeLast()
                bufferPopup.removeItem(at: supportedBufferFrames.count)
            }
            if pendingBufferFrames == frames {
                pendingBufferFrames = nil
                bufferStatusLabel.stringValue = snapshot.bufferRestartState == "restarted" ? "Applied: \(frames) samples; USB audio restarted successfully." : (snapshot.bufferRestartState == "applied" ? "Applied: \(frames) samples; USB audio was idle." : "Active: \(frames) samples.")
            } else if pendingBufferFrames == nil {
                if snapshot.bufferRestartState == "restarting" { bufferStatusLabel.stringValue = "Restarting USB audio…" }
                else if snapshot.bufferRestartState == "failed" { bufferStatusLabel.stringValue = "USB audio restart failed. Reconnect the NS6 or restart audio." }
                else { bufferStatusLabel.stringValue = "Active: \(frames) samples." }
            }
            let displayFrames = pendingBufferFrames ?? frames
            if let index = displayedBufferFrames.firstIndex(of: displayFrames) { bufferPopup.selectItem(at: index) }
            updatingBufferPopup = false
        }
        bufferPopup.isEnabled = snapshot.bufferSettable && !applyingBuffer
        applyBufferButton.isEnabled = snapshot.bufferSettable && !applyingBuffer && pendingBufferFrames.map(supportedBufferFrames.contains) == true && pendingBufferFrames != frames
        bufferLatencyLabel.stringValue = String(format: "%.1f ms startup", Double(frames) * 1000.0 / sampleRate)
    }

    private var selectedBufferFrames: UInt32? {
        let index = bufferPopup.indexOfSelectedItem
        guard index >= 0, index < displayedBufferFrames.count else { return nil }
        return displayedBufferFrames[index]
    }

    @objc private func selectBufferSize(_ sender: NSPopUpButton) {
        guard !updatingBufferPopup else { return }
        let selected = selectedBufferFrames
        pendingBufferFrames = selected.map(supportedBufferFrames.contains) == true && selected != currentBufferFrames ? selected : nil
        applyBufferButton.isEnabled = pendingBufferFrames != nil && !applyingBuffer
        bufferStatusLabel.stringValue = pendingBufferFrames == nil ? "Active: \(currentBufferFrames ?? 0) samples." : "Pending change — click Apply to restart USB audio and apply it."
    }

    @objc private func applyBufferSize(_ sender: NSButton) {
        guard let frames = pendingBufferFrames, frames != currentBufferFrames, !applyingBuffer else { return }
        applyingBuffer = true
        let attempt = UUID()
        bufferApplyAttempt = attempt
        bufferPopup.isEnabled = false
        applyBufferButton.isEnabled = false
        bufferStatusLabel.stringValue = "Applying…"
        DispatchQueue.main.asyncAfter(deadline: .now() + 20) { [weak self] in
            guard let self, self.bufferApplyAttempt == attempt else { return }
            self.bufferApplyAttempt = nil
            self.applyingBuffer = false
            self.pendingBufferFrames = nil
            self.bufferStatusLabel.stringValue = "Driver did not finish restarting USB audio. Check the connection and reselect the NS6."
            self.refresh()
        }
        bufferConfigurationQueue.async { [weak self] in
            guard let device = audioDeviceID() else {
                DispatchQueue.main.async {
                    guard let self, self.bufferApplyAttempt == attempt else { return }
                    self.applyingBuffer = false
                    self.bufferApplyAttempt = nil
                    self.pendingBufferFrames = nil
                    self.bufferStatusLabel.stringValue = "Could not find the NS6 audio device."
                    self.refresh()
                }
                return
            }
            var address = AudioObjectPropertyAddress(mSelector: driverBufferProperty, mScope: kAudioObjectPropertyScopeGlobal, mElement: kAudioObjectPropertyElementMain)
            var requestedValue = "\(frames)" as CFString
            let status = withUnsafePointer(to: &requestedValue) { valuePointer in
                AudioObjectSetPropertyData(device, &address, 0, nil, UInt32(MemoryLayout<CFString>.size), valuePointer)
            }
            guard status == noErr else {
                DispatchQueue.main.async {
                    guard let self, self.bufferApplyAttempt == attempt else { return }
                    self.applyingBuffer = false
                    self.bufferApplyAttempt = nil
                    self.pendingBufferFrames = nil
                    self.bufferStatusLabel.stringValue = "Driver rejected the setting (\(status)); current value is unchanged."
                    NSSound.beep()
                    self.refresh()
                }
                return
            }
            self?.confirmBufferSize(frames, requestID: attempt, attempt: 0)
        }
    }

    private func confirmBufferSize(_ frames: UInt32, requestID: UUID, attempt: Int) {
        guard attempt < 100 else {
            DispatchQueue.main.async {
                guard self.bufferApplyAttempt == requestID else { return }
                self.applyingBuffer = false
                self.bufferApplyAttempt = nil
                self.pendingBufferFrames = nil
                self.bufferStatusLabel.stringValue = "Driver did not confirm the USB audio restart. Check the connection."
                self.refresh()
            }
            return
        }
        guard let device = audioDeviceID() else {
            bufferConfigurationQueue.asyncAfter(deadline: .now() + 0.2) { [weak self] in self?.confirmBufferSize(frames, requestID: requestID, attempt: attempt + 1) }
            return
        }
        let restartState = driverBufferRestartState(device)
        if restartState == "failed" {
            DispatchQueue.main.async {
                guard self.bufferApplyAttempt == requestID else { return }
                self.applyingBuffer = false
                self.bufferApplyAttempt = nil
                self.pendingBufferFrames = nil
                self.bufferStatusLabel.stringValue = "USB audio restart failed. Reconnect the NS6 or restart audio."
                NSSound.beep()
                self.refresh()
            }
        } else if driverBufferFrames(device) == frames && (restartState == "restarted" || restartState == "applied") {
            DispatchQueue.main.async {
                guard self.bufferApplyAttempt == requestID else { return }
                self.applyingBuffer = false
                self.bufferApplyAttempt = nil
                self.pendingBufferFrames = nil
                self.bufferStatusLabel.stringValue = restartState == "restarted" ? "Applied: \(frames) samples; USB audio restarted successfully." : "Applied: \(frames) samples; USB audio was idle."
                self.refresh()
            }
        } else {
            bufferConfigurationQueue.asyncAfter(deadline: .now() + 0.2) { [weak self] in self?.confirmBufferSize(frames, requestID: requestID, attempt: attempt + 1) }
        }
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
