#include "session/hsm_stock_foes.h"
#include <cassert>
#include <cstdio>
#include <initializer_list>
using namespace hsm;
int main() {
    for (bool host : {false, true}) for (bool bots : {false, true}) for (bool progression : {false, true}) {
        bool allow = AllowStockFoes(host, bots, progression);
        assert(allow == (host && (bots || progression)));
        assert(!BlockStockSpawn(false, allow, true, false)); // disconnected, menu, hub
        assert(!BlockStockSpawn(true, allow, true, true)); // the stock local player must spawn
        assert(BlockStockSpawn(true, allow, true, false) == !allow); // foe request, including waiting/Countdown
        assert(BlockStockSpawn(true, allow, false, true) == !allow); // unreadable Player flag cannot admit a foe
        assert(!RemoveStockFighter(true, allow, true, false)); // player before entity registration
        assert(!RemoveStockFighter(true, allow, false, true)); // stand-in or announced bot
        assert(RemoveStockFighter(true, allow, false, false) == !allow); // existing or late-spawned stock foe
        assert(!RemoveStockFighter(false, allow, false, false));
    }
    std::puts("stock foe policy: all passed");
}
