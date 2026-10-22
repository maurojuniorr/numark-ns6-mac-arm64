#include "AudioClientLifecycle.hpp"

#include <cassert>

int main() {
    AudioClientLifecycle clients;
    assert(clients.activeStarts() == 0);
    assert(clients.start(101));
    assert(clients.activeStarts() == 1);
    assert(clients.start(202));
    assert(clients.activeStarts() == 2);
    assert(clients.stop(101));
    assert(clients.activeStarts() == 1);
    assert(clients.stop(202));
    assert(clients.activeStarts() == 0);

    assert(clients.start(101));
    assert(clients.start(101));
    assert(clients.activeStarts() == 2);
    assert(clients.stop(101));
    assert(clients.activeStarts() == 1);
    assert(clients.removeClient(101));
    assert(clients.activeStarts() == 0);
    assert(!clients.stop(101));

    assert(!clients.removeClient(303));

    for (std::uint32_t id = 0; id < 64; ++id) assert(clients.start(id));
    assert(clients.activeStarts() == 64);
    assert(!clients.start(64));
    assert(clients.removeClient(32));
    assert(clients.activeStarts() == 63);
    assert(clients.start(64));
    assert(clients.activeStarts() == 64);
}
