#include "../src/network/lan_transport.h"
#include "../src/extensions/lan_coop/lan_coop_extension.h"
#include <cassert>
#include <cstdio>
#include <iostream>

using namespace ds3sc::network;
using namespace ds3sc::extensions;

extern "C" {
volatile LONG ds3scConnectionMode = 1;
volatile LONG ds3scLanPort = 27015;
}

void TestLanTransportLifecycle() {
    auto& transport = LanTransport::Instance();

    // 1. Initial state
    assert(!transport.IsInitialized());
    assert(transport.BoundPort() == 0);

    // 2. Start hosting session on port 27015
    assert(transport.Initialize(27015, true));
    assert(transport.IsInitialized());
    assert(transport.BoundPort() == 27015);
    assert(transport.IsHost());

    // 3. Dissolve session: port must be closed and state reset
    transport.CloseSession();
    assert(!transport.IsInitialized());
    assert(transport.BoundPort() == 0);

    // 4. Reopen as guest on port 27015
    assert(transport.Initialize(27015, false));
    assert(transport.IsInitialized());
    assert(transport.BoundPort() == 27015);
    assert(!transport.IsHost());

    // 5. Final close
    transport.CloseSession();
    assert(!transport.IsInitialized());
    assert(transport.BoundPort() == 0);

    std::puts("PASS: LanTransport lifecycle (open, dissolve, reopen, close) verified.");
}

void TestLanExtensionSessionManagement() {
    auto ext = CreateLanCoopExtension();
    assert(ext != nullptr);

    auto lanCoop = std::dynamic_pointer_cast<LanCoopExtension>(ext);
    assert(lanCoop != nullptr);

    // Trigger host session opens port
    lanCoop->TriggerHostSession();
    assert(lanCoop->IsHost());
    assert(LanTransport::Instance().IsInitialized());
    assert(LanTransport::Instance().BoundPort() == 27015);

    // Dissolve session closes port
    lanCoop->DissolveSession();
    assert(!lanCoop->IsHost());
    assert(!LanTransport::Instance().IsInitialized());
    assert(LanTransport::Instance().BoundPort() == 0);

    std::puts("PASS: LanCoopExtension session management verified.");
}

int main() {
    TestLanTransportLifecycle();
    TestLanExtensionSessionManagement();
    std::puts("ALL LAN TRANSPORT TESTS PASSED.");
    return 0;
}
