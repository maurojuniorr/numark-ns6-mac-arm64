#include "AudioKeepalivePolicy.hpp"

#include <cassert>
#include <cstdint>

int main() {
    using ns6::AudioKeepaliveAction;
    using ns6::audioKeepaliveAction;

    constexpr std::uint32_t unknown = 0;
    assert(audioKeepaliveAction(unknown, unknown) == AudioKeepaliveAction::None);
    assert(audioKeepaliveAction(unknown, 42) == AudioKeepaliveAction::Start);
    assert(audioKeepaliveAction(42, 42) == AudioKeepaliveAction::None);
    assert(audioKeepaliveAction(42, 42, false) == AudioKeepaliveAction::Stop);
    assert(audioKeepaliveAction(42, unknown) == AudioKeepaliveAction::Stop);
    assert(audioKeepaliveAction(42, 43) == AudioKeepaliveAction::Stop);
}
