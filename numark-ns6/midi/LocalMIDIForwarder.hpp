#pragma once

#include "MIDIInputReader.hpp"

class LocalMIDIForwarder final : public MIDIInputSink {
public:
    ~LocalMIDIForwarder() override;
    bool start() noexcept;
    void stop() noexcept;
    void publish(const std::uint8_t* bytes, std::size_t length) noexcept override;

private:
    int socket_ = -1;
    bool firstSendLogged_ = false;
};
