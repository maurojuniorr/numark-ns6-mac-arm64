cmake_minimum_required(VERSION 3.16)
project(NumarkNS6 VERSION 0.2.18 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(NS6_ENABLE_SANITIZERS "Enable address and undefined behavior sanitizers" OFF)

add_executable(audio_out_tests
    audio-feedback/AudioFeedbackPatternPolicy.cpp
    audio-out/AudioOutputRing.cpp
    audio-out/AudioPacketPolicy.cpp
    audio-out/AudioOutPacket.cpp
    audio-out/AudioOutTransferBatch.cpp
    audio-out/AudioOutTransferSlots.cpp
    audio-out/AudioOutSchedule.cpp
    audio-out/NS6AudioOutLayout.cpp
    tests/audio_out_tests.cpp)
target_include_directories(audio_out_tests PRIVATE audio-out)
target_compile_options(audio_out_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
if(NS6_ENABLE_SANITIZERS)
    target_compile_options(audio_out_tests PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(audio_out_tests PRIVATE -fsanitize=address,undefined)
endif()

add_executable(audio_client_lifecycle_tests
    tests/audio_client_lifecycle_tests.cpp
    coreaudio/AudioClientLifecycle.cpp)
target_include_directories(audio_client_lifecycle_tests PRIVATE coreaudio)
target_compile_options(audio_client_lifecycle_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
if(NS6_ENABLE_SANITIZERS)
    target_compile_options(audio_client_lifecycle_tests PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(audio_client_lifecycle_tests PRIVATE -fsanitize=address,undefined)
endif()

add_executable(audio_output_engine_tests
    tests/audio_output_engine_tests.cpp
    coreaudio/AudioOutputEngine.cpp
    audio-out/AudioOutputRing.cpp)
target_include_directories(audio_output_engine_tests PRIVATE coreaudio audio-out)
target_compile_options(audio_output_engine_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
if(NS6_ENABLE_SANITIZERS)
    target_compile_options(audio_output_engine_tests PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(audio_output_engine_tests PRIVATE -fsanitize=address,undefined)
endif()

add_executable(audio_transport_runtime_tests
    tests/audio_transport_runtime_tests.cpp)
target_link_libraries(audio_transport_runtime_tests PRIVATE ns6_usb_session
                                                            "-framework IOKit"
                                                            "-framework CoreFoundation")
target_compile_options(audio_transport_runtime_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
if(NS6_ENABLE_SANITIZERS)
    target_compile_options(audio_transport_runtime_tests PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(audio_transport_runtime_tests PRIVATE -fsanitize=address,undefined)
endif()

add_executable(audio_device_lifecycle_tests
    tests/audio_device_lifecycle_tests.cpp
    coreaudio/AudioDeviceLifecycle.cpp
    coreaudio/AudioClientLifecycle.cpp)
target_include_directories(audio_device_lifecycle_tests PRIVATE coreaudio)
target_compile_options(audio_device_lifecycle_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
if(NS6_ENABLE_SANITIZERS)
    target_compile_options(audio_device_lifecycle_tests PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(audio_device_lifecycle_tests PRIVATE -fsanitize=address,undefined)
endif()

add_executable(audio_input_tests
    audio-in/AudioInputDecoder.cpp
    tests/audio_input_tests.cpp)
target_include_directories(audio_input_tests PRIVATE audio-in)
target_compile_options(audio_input_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
if(NS6_ENABLE_SANITIZERS)
    target_compile_options(audio_input_tests PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(audio_input_tests PRIVATE -fsanitize=address,undefined)
endif()

add_executable(audio_input_engine_tests
    audio-in/AudioInputDecoder.cpp
    coreaudio/AudioInputEngine.cpp
    audio-out/AudioOutputRing.cpp
    tests/audio_input_engine_tests.cpp)
target_include_directories(audio_input_engine_tests PRIVATE coreaudio audio-in audio-out)
target_compile_options(audio_input_engine_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
if(NS6_ENABLE_SANITIZERS)
    target_compile_options(audio_input_engine_tests PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(audio_input_engine_tests PRIVATE -fsanitize=address,undefined)
endif()

add_executable(controller_activation_tests
    midi/ControllerActivation.cpp
    usb/NS6DeviceSession.cpp
    tests/controller_activation_tests.cpp)
target_include_directories(controller_activation_tests PRIVATE midi usb)
target_link_libraries(controller_activation_tests PRIVATE "-framework IOKit"
                                                         "-framework CoreFoundation")
target_compile_options(controller_activation_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
if(NS6_ENABLE_SANITIZERS)
    target_compile_options(controller_activation_tests PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(controller_activation_tests PRIVATE -fsanitize=address,undefined)
endif()

add_executable(audio_keepalive_policy_tests tests/audio_keepalive_policy_tests.cpp)
target_include_directories(audio_keepalive_policy_tests PRIVATE coreaudio)
target_compile_options(audio_keepalive_policy_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_executable(audio_feedback_tests
    audio-feedback/AudioFeedbackParser.cpp
    audio-feedback/AudioFeedbackPatternPolicy.cpp
    audio-feedback/AudioFeedbackSchedule.cpp
    tests/audio_feedback_tests.cpp)
target_include_directories(audio_feedback_tests PRIVATE audio-feedback)
target_compile_options(audio_feedback_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
if(NS6_ENABLE_SANITIZERS)
    target_compile_options(audio_feedback_tests PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(audio_feedback_tests PRIVATE -fsanitize=address,undefined)
endif()

add_library(ns6_usb_session STATIC
    audio-out/AudioOutputRing.cpp
    audio-out/AudioPacketPolicy.cpp
    audio-out/AudioOutPacket.cpp
    audio-out/AudioOutTransferBatch.cpp
    audio-out/AudioOutTransferSlots.cpp
    audio-out/AudioOutSchedule.cpp
    audio-out/AudioOutWorker.cpp
    audio-feedback/AudioFeedbackParser.cpp
    audio-feedback/AudioFeedbackPatternPolicy.cpp
    audio-feedback/AudioFeedbackSchedule.cpp
    audio-feedback/AudioFeedbackReader.cpp
    audio-in/AudioInputDecoder.cpp
    audio-in/AudioInputReader.cpp
    coreaudio/AudioClientLifecycle.cpp
    coreaudio/AudioDeviceLifecycle.cpp
    coreaudio/AudioInputEngine.cpp
    coreaudio/AudioOutputEngine.cpp
    coreaudio/AudioTransportRuntime.cpp
    midi/ControllerActivation.cpp
    usb/NS6DeviceSession.cpp)
target_include_directories(ns6_usb_session PUBLIC audio-out audio-feedback audio-in coreaudio midi usb)
target_compile_options(ns6_usb_session PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_link_libraries(ns6_usb_session PRIVATE "-framework IOKit" "-framework CoreFoundation")

add_executable(ns6_feedback_probe tools/ns6_feedback_probe.cpp)
target_link_libraries(ns6_feedback_probe PRIVATE ns6_usb_session "-framework IOKit"
                                                "-framework CoreFoundation")
target_compile_options(ns6_feedback_probe PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_executable(ns6_audio_input_probe tools/ns6_audio_input_probe.cpp)
target_link_libraries(ns6_audio_input_probe PRIVATE ns6_usb_session "-framework IOKit"
                                                    "-framework CoreFoundation")
target_compile_options(ns6_audio_input_probe PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_library(ns6_audio_driver MODULE
    coreaudio/NS6AudioDriver.cpp)
set_target_properties(ns6_audio_driver PROPERTIES PREFIX "" OUTPUT_NAME "NumarkNS6")
target_include_directories(ns6_audio_driver PRIVATE coreaudio audio-out audio-feedback audio-in midi usb)
target_link_libraries(ns6_audio_driver PRIVATE ns6_usb_session "-framework CoreAudio"
                                               "-framework CoreFoundation" "-framework IOKit")
target_compile_options(ns6_audio_driver PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_executable(ns6_midi_bridge midi/MIDIUserSessionBridge.cpp)
set_target_properties(ns6_midi_bridge PROPERTIES OUTPUT_NAME "NumarkNS6MIDIBridge")
target_include_directories(ns6_midi_bridge PRIVATE coreaudio)
target_link_libraries(ns6_midi_bridge PRIVATE "-framework CoreAudio"
                                              "-framework CoreFoundation"
                                              "-framework CoreMIDI"
                                              "-framework IOKit")
target_compile_options(ns6_midi_bridge PRIVATE -Wall -Wextra -Wpedantic -Werror)

set(NS6_PLUGIN_BUNDLE "${CMAKE_CURRENT_BINARY_DIR}/NumarkNS6.driver")
configure_file(coreaudio/Info.plist.in "${NS6_PLUGIN_BUNDLE}/Contents/Info.plist" @ONLY)
add_custom_target(ns6_audio_driver_bundle ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory "${NS6_PLUGIN_BUNDLE}/Contents/MacOS"
    COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:ns6_audio_driver>"
            "${NS6_PLUGIN_BUNDLE}/Contents/MacOS/NumarkNS6"
    COMMAND /usr/bin/codesign --force --sign - --timestamp=none "${NS6_PLUGIN_BUNDLE}"
    DEPENDS ns6_audio_driver)

enable_testing()
add_test(NAME audio_out_tests COMMAND audio_out_tests)
add_test(NAME audio_feedback_tests COMMAND audio_feedback_tests)
add_test(NAME audio_output_engine_tests COMMAND audio_output_engine_tests)
add_test(NAME audio_transport_runtime_tests COMMAND audio_transport_runtime_tests)
add_test(NAME audio_client_lifecycle_tests COMMAND audio_client_lifecycle_tests)
add_test(NAME audio_device_lifecycle_tests COMMAND audio_device_lifecycle_tests)
add_test(NAME audio_input_tests COMMAND audio_input_tests)
add_test(NAME audio_input_engine_tests COMMAND audio_input_engine_tests)
add_test(NAME controller_activation_tests COMMAND controller_activation_tests)
add_test(NAME audio_keepalive_policy_tests COMMAND audio_keepalive_policy_tests)
add_executable(midi_input_tests
    midi/MIDIInputParser.cpp
    midi/MIDIInputEventEncoder.cpp
    tests/midi_input_tests.cpp)
target_include_directories(midi_input_tests PRIVATE midi)
target_compile_options(midi_input_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_test(NAME midi_input_tests COMMAND midi_input_tests)
add_executable(audio_driver_contract_tests
    tests/audio_driver_contract_tests.cpp
    coreaudio/NS6AudioDriver.cpp)
target_link_libraries(audio_driver_contract_tests PRIVATE ns6_usb_session
    "-framework CoreAudio" "-framework CoreFoundation"
    "-framework IOKit")
target_compile_options(audio_driver_contract_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_test(NAME audio_driver_contract_tests COMMAND audio_driver_contract_tests)
add_test(NAME audio_driver_bundle_signature
    COMMAND /usr/bin/codesign --verify --deep --strict "${NS6_PLUGIN_BUNDLE}")

