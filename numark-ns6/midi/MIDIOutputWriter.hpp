#pragma once

#include "../usb/NS6DeviceSession.hpp"
#include <atomic>
#include <thread>

class MIDIOutputWriter final {
public:
    ~MIDIOutputWriter();
    IOReturn start(NS6DeviceSession& session) noexcept;
    void stop() noexcept;
    bool stopped() const noexcept { return !worker_.joinable(); }

private:
    void run() noexcept;
    NS6DeviceSession* session_ = nullptr;
    int socket_ = -1;
    std::atomic<bool> running_{false};
    std::thread worker_;
};
