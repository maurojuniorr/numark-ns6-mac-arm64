#pragma once

#include "MIDIInputReader.hpp"

struct LocalMIDIForwarderStats final {
    std::uint64_t sentDatagrams = 0;
    std::uint64_t sentBytes = 0;
    std::uint64_t sendFailures = 0;
};

class LocalMIDIForwarder final : public MIDIInputSink {
public:
    ~LocalMIDIForwarder() override;
    bool start() noexcept;
    void stop() noexcept;
    void publish(const std::uint8_t* bytes, std::size_t length) noexcept override;
    LocalMIDIForwarderStats stats() const noexcept;

private:
    int socket_ = -1;
    bool firstSendLogged_ = false;
    std::uint64_t sentDatagrams_ = 0;
    std::uint64_t sentBytes_ = 0;
    std::uint64_t sendFailures_ = 0;
};
