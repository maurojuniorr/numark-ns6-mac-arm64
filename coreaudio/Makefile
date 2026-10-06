SDK := $(shell xcrun --show-sdk-path)
ADAPTIVE_CLOCK ?= 0
NS6_MIDI_ENABLED ?= 1
NS6_FEEDBACK_READER_ENABLED ?= 1
NS6_USB_OUTPUT_SLOT_COUNT ?= 8
CFLAGS := -Wall -Wextra -Werror -arch arm64 -isysroot $(SDK) -mmacosx-version-min=15.0 -fPIC -DNS6_ADAPTIVE_CLOCK_ENABLED=$(ADAPTIVE_CLOCK) -DNS6_MIDI_ENABLED=$(NS6_MIDI_ENABLED) -DNS6_FEEDBACK_READER_ENABLED=$(NS6_FEEDBACK_READER_ENABLED) -DNS6_USB_OUTPUT_SLOT_COUNT=$(NS6_USB_OUTPUT_SLOT_COUNT)
BUILD := build/NumarkNS6.driver/Contents
APP := build/NumarkNS6Status.app
VERSION ?= 0.2.73
BUNDLE_BUILD_VERSION ?= $(shell echo $(VERSION) | awk -F. '{print $$3}')
PACKAGE_ID := io.github.maurojuniorr.numark-ns6
PACKAGE_ROOT := build/package-root
DIST := dist
SWIFT_MODULE_CACHE := /tmp/ns6-macos-swift-module-cache
FORCE:

.PHONY: FORCE usb-work-interval-regression-test audio-recovery-test zero-timestamp-test

all: $(BUILD)/MacOS/NumarkNS6 $(BUILD)/Info.plist app midi-bridge

$(BUILD)/MacOS/NumarkNS6: FORCE NS6HAL.c NS6Transport.c NS6USB.c NS6USBEndpoint.h NS6USBConfig.h NS6USBTiming.c NS6USBTiming.h NS6USBFrameTimestamps.h NS6USBWorkerQoS.h NS6ClockControl.c NS6MIDI.c NS6MIDIParser.h NS6MIDIRecovery.h NS6BufferFrameSize.h NS6ZeroTimestamp.h NS6AudioRecovery.c NS6AudioRecovery.h
	mkdir -p $(BUILD)/MacOS
	$(CC) $(CFLAGS) -bundle NS6HAL.c NS6Transport.c NS6USB.c NS6USBTiming.c NS6ClockControl.c NS6MIDI.c NS6AudioRecovery.c -o $@ -framework CoreAudio -framework CoreFoundation -framework IOKit
$(BUILD)/Info.plist: Info.plist FORCE
	mkdir -p $(BUILD)
	cp $< $@
	/usr/libexec/PlistBuddy -c 'Set :CFBundleShortVersionString $(VERSION)' $@
	/usr/libexec/PlistBuddy -c 'Set :CFBundleVersion $(BUNDLE_BUILD_VERSION)' $@
app: $(APP)/Contents/MacOS/NumarkNS6Status $(APP)/Contents/MacOS/NumarkNS6MIDIBridge $(APP)/Contents/Info.plist $(APP)/Contents/PkgInfo $(APP)/Contents/Resources/NS6Status.icns
$(APP)/Contents/MacOS/NumarkNS6Status: status-app/NS6Status.swift
	mkdir -p $(APP)/Contents/MacOS
	mkdir -p $(SWIFT_MODULE_CACHE)
	swiftc -O -module-cache-path $(SWIFT_MODULE_CACHE) -target arm64-apple-macosx15.0 $< -o $@ -framework AppKit -framework CoreAudio -framework IOKit
$(APP)/Contents/MacOS/NumarkNS6MIDIBridge: build/NumarkNS6MIDIBridge
	mkdir -p $(APP)/Contents/MacOS
	cp $< $@
$(APP)/Contents/Info.plist: status-app/Info.plist FORCE
	mkdir -p $(APP)/Contents
	cp $< $@
	/usr/libexec/PlistBuddy -c 'Set :CFBundleShortVersionString $(VERSION)' $@
	/usr/libexec/PlistBuddy -c 'Set :CFBundleVersion $(BUNDLE_BUILD_VERSION)' $@
$(APP)/Contents/PkgInfo:
	mkdir -p $(APP)/Contents
	printf 'APPL????' > $@
$(APP)/Contents/Resources/NS6Status.icns: status-app/NS6Status.icns
	mkdir -p $(APP)/Contents/Resources
	cp $< $@
clean:
	rm -rf build

usb-test: NS6USBTest.c NS6Transport.c NS6USB.c NS6USBEndpoint.h NS6USBTiming.c NS6USBTiming.h NS6ClockControl.c NS6MIDI.c
	mkdir -p build
	$(CC) $(CFLAGS) NS6USBTest.c NS6Transport.c NS6USB.c NS6USBTiming.c NS6ClockControl.c NS6MIDI.c -o build/ns6-usb-test -framework CoreAudio -framework CoreFoundation -framework IOKit -lm

usb-work-interval-regression-test: $(BUILD)/MacOS/NumarkNS6 NS6USBWorkIntervalRegressionTest.sh
	./NS6USBWorkIntervalRegressionTest.sh $(BUILD)/MacOS/NumarkNS6

usb-realtime-test: NS6USBRealtimeTest.c NS6USBRealtime.h
	mkdir -p build
	$(CC) $(CFLAGS) NS6USBRealtimeTest.c -o build/ns6-usb-realtime-test -framework AudioToolbox
	build/ns6-usb-realtime-test

midi-parser-test: NS6MIDIParserTest.c NS6MIDIParser.h
	$(CC) -Wall -Wextra -Werror NS6MIDIParserTest.c -o build/ns6-midi-parser-test
	build/ns6-midi-parser-test

midi-recovery-test: NS6MIDIRecoveryTest.c NS6MIDIRecovery.h
	$(CC) -Wall -Wextra -Werror NS6MIDIRecoveryTest.c -o build/ns6-midi-recovery-test
	build/ns6-midi-recovery-test

buffer-frame-size-test: NS6BufferFrameSizeTest.c NS6BufferFrameSize.h
	$(CC) -Wall -Wextra -Werror NS6BufferFrameSizeTest.c -o build/ns6-buffer-frame-size-test
	build/ns6-buffer-frame-size-test

zero-timestamp-test: NS6ZeroTimestampTest.c NS6ZeroTimestamp.h
	$(CC) -Wall -Wextra -Werror NS6ZeroTimestampTest.c -o build/ns6-zero-timestamp-test
	build/ns6-zero-timestamp-test

transport-ring-test: NS6TransportRingTest.c NS6Transport.c NS6Transport.h
	$(CC) $(CFLAGS) NS6TransportRingTest.c NS6Transport.c -o build/ns6-transport-ring-test -framework CoreAudio -lm
	build/ns6-transport-ring-test

clock-control-test: NS6ClockControlTest.c NS6ClockControl.c NS6ClockControl.h
	$(CC) -Wall -Wextra -Werror -DNS6_ADAPTIVE_CLOCK_ENABLED=1 NS6ClockControlTest.c NS6ClockControl.c -o build/ns6-clock-control-test
	build/ns6-clock-control-test

clock-control-fixed-test: NS6ClockControlFixedTest.c NS6ClockControl.c NS6ClockControl.h
	$(CC) -Wall -Wextra -Werror NS6ClockControlFixedTest.c NS6ClockControl.c -o build/ns6-clock-control-fixed-test
	build/ns6-clock-control-fixed-test

audio-recovery-test: NS6AudioRecoveryTest.c NS6AudioRecovery.c NS6AudioRecovery.h
	mkdir -p build
	$(CC) -Wall -Wextra -Werror NS6AudioRecoveryTest.c NS6AudioRecovery.c -o build/ns6-audio-recovery-test
	build/ns6-audio-recovery-test

io-metrics-test: NS6IOMetricsTest.c NS6IOMetrics.c NS6IOMetrics.h
	$(CC) -Wall -Wextra -Werror NS6IOMetricsTest.c NS6IOMetrics.c -o build/ns6-io-metrics-test
	build/ns6-io-metrics-test

usb-endpoint-test: NS6USBEndpointTest.c NS6USBEndpoint.h
	mkdir -p build
	$(CC) -Wall -Wextra -Werror NS6USBEndpointTest.c -o build/ns6-usb-endpoint-test
	build/ns6-usb-endpoint-test

usb-timing-test: NS6USBTiming.c NS6USBTiming.h NS6USBTimingTest.c NS6USBFrameTimestamps.h NS6USBFrameTimestampsTest.c
	$(CC) -std=c11 -Wall -Wextra -Werror NS6USBTiming.c NS6USBTimingTest.c -o build/ns6-usb-timing-test
	build/ns6-usb-timing-test
	$(CC) $(CFLAGS) NS6USBFrameTimestampsTest.c -o build/ns6-usb-frame-timestamps-test -framework IOKit
	build/ns6-usb-frame-timestamps-test
	$(CC) -std=c11 -Wall -Wextra -Werror NS6USBWorkerQoSTest.c -o build/ns6-usb-worker-qos-test
	build/ns6-usb-worker-qos-test

usb-config-test: NS6USBConfig.h NS6USBConfigTest.c
	mkdir -p build
	$(CC) -Wall -Wextra -Werror -DNS6_MIDI_ENABLED=1 -DNS6_FEEDBACK_READER_ENABLED=1 -DNS6_USB_OUTPUT_SLOT_COUNT=8 NS6USBConfigTest.c -o build/ns6-usb-config-default-test
	build/ns6-usb-config-default-test
	$(CC) -Wall -Wextra -Werror -DNS6_MIDI_ENABLED=0 -DNS6_FEEDBACK_READER_ENABLED=0 -DNS6_USB_OUTPUT_SLOT_COUNT=32 NS6USBConfigTest.c -o build/ns6-usb-config-diagnostic-test
	build/ns6-usb-config-diagnostic-test

midi-bridge: build/NumarkNS6MIDIBridge
build/NumarkNS6MIDIBridge: NS6MIDIBridge.c
	mkdir -p build
	$(CC) $(CFLAGS) -Wno-deprecated-declarations NS6MIDIBridge.c -o $@ -framework CoreMIDI -framework CoreFoundation

audio-list: AudioList.c
	$(CC) $(CFLAGS) AudioList.c -o build/audio-list -framework CoreAudio -framework CoreFoundation

audio-default: AudioDefault.c
	$(CC) $(CFLAGS) AudioDefault.c -o build/audio-default -framework CoreAudio -framework CoreFoundation

sign: all
	codesign --force --sign - --timestamp=none build/NumarkNS6.driver
	codesign --verify --strict --verbose=2 build/NumarkNS6.driver
	codesign --force --sign - --timestamp=none $(APP)/Contents/MacOS/NumarkNS6MIDIBridge
	codesign --force --sign - --timestamp=none $(APP)
	codesign --verify --strict --verbose=2 $(APP)

install: sign
	sudo ditto build/NumarkNS6.driver /Library/Audio/Plug-Ins/HAL/NumarkNS6M1.driver
	sudo ditto $(APP) '/Applications/Numark NS6 Status.app'
	sudo cp launch-agents/io.github.maurojuniorr.numark-ns6-midi-bridge.plist /Library/LaunchAgents/
	-sudo killall -m '^Core Audio Driver \(NumarkNS6M1\.driver\)$$'
	sudo killall coreaudiod

package: usb-work-interval-regression-test sign
	rm -rf $(PACKAGE_ROOT) $(DIST)
	mkdir -p $(PACKAGE_ROOT)/Library/Audio/Plug-Ins/HAL $(PACKAGE_ROOT)/Library/LaunchAgents $(PACKAGE_ROOT)/Applications $(DIST)
	ditto --norsrc --noextattr build/NumarkNS6.driver $(PACKAGE_ROOT)/Library/Audio/Plug-Ins/HAL/NumarkNS6M1.driver
	ditto --norsrc --noextattr $(APP) '$(PACKAGE_ROOT)/Applications/Numark NS6 Status.app'
	cp launch-agents/io.github.maurojuniorr.numark-ns6-midi-bridge.plist $(PACKAGE_ROOT)/Library/LaunchAgents/
	xattr -cr $(PACKAGE_ROOT)
	pkgbuild --root $(PACKAGE_ROOT) --scripts package-scripts --identifier $(PACKAGE_ID) --version $(VERSION) --install-location / $(DIST)/NumarkNS6-$(VERSION).pkg

uninstall:
	sudo rm -rf /Library/Audio/Plug-Ins/HAL/NumarkNS6M1.driver
	-sudo killall -m '^Core Audio Driver \(NumarkNS6M1\.driver\)$$'
	sudo killall coreaudiod
