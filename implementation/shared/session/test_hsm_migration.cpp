// Host migration (session/hsm_migration.h).
#include "session/hsm_migration.h"
#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm;
using S = MigrationStep;

int main() {
    const uint64_t oldH = 100, me = 200, other = 300;
    HostMigration m;
    CHECK(!m.Active() && m.Tick(oldH, me, true, true, 0) == S::Idle);

    // this machine is promoted
    m.Start(oldH, 1000);
    CHECK(m.Active() && m.oldHost() == oldH);
    CHECK(m.Tick(oldH, me, true, true, 2000) == S::WaitOwner);         // Steam still names the old host
    CHECK(m.Tick(0, me, true, true, 2100) == S::WaitOwner);            // unknown
    m.Start(other, 2200); CHECK(m.oldHost() == oldH);                  // a second Start is ignored
    CHECK(m.Tick(me, me, true, true, 3000) == S::BecomeHost);
    CHECK(m.newHost() == me && m.step() == S::Hosting);
    CHECK(m.Tick(me, me, true, true, 3100) == S::Hosting);             // returned once
    CHECK(!m.StartDue(3000 + kMigrationStartDelayMs - 1));
    CHECK(m.StartDue(3000 + kMigrationStartDelayMs) && !m.Active());
    CHECK(!m.StartDue(9000000));

    // another member is promoted
    m.Start(oldH, 0);
    CHECK(m.Tick(other, me, true, true, 500) == S::FollowHost && m.newHost() == other);
    CHECK(m.Tick(other, me, true, true, 600) == S::Following);
    m.Welcomed(); CHECK(!m.Active());
    m.Welcomed(); CHECK(!m.Active());

    // the new host never welcomes us
    m.Start(oldH, 0);
    CHECK(m.Tick(other, me, true, true, 100) == S::FollowHost);
    CHECK(m.Tick(other, me, true, true, 100 + kMigrationWelcomeMs - 1) == S::Following);
    CHECK(m.Tick(other, me, true, true, 100 + kMigrationWelcomeMs) == S::GiveUp && !m.Active());

    // the new host leaves as well: wait for the next owner
    m.Start(oldH, 0);
    CHECK(m.Tick(other, me, true, true, 100) == S::FollowHost);
    CHECK(m.Tick(me, me, true, true, 200) == S::WaitOwner && m.oldHost() == other);
    CHECK(m.Tick(me, me, true, true, 300) == S::BecomeHost);
    m.Clear(); CHECK(!m.Active());

    // nobody is named in time
    m.Start(oldH, 50);
    CHECK(m.Tick(oldH, me, true, true, 50 + kMigrationOwnerWaitMs - 1) == S::WaitOwner);
    CHECK(m.Tick(oldH, me, true, true, 50 + kMigrationOwnerWaitMs) == S::GiveUp && !m.Active());

    // off, or out of the lobby
    m.Start(oldH, 0); CHECK(m.Tick(other, me, true, false, 1) == S::GiveUp && !m.Active());
    m.Start(oldH, 0); CHECK(m.Tick(other, me, false, true, 1) == S::GiveUp && !m.Active());
    m.Start(oldH, 0); CHECK(m.Tick(other, me, true, true, 1) == S::FollowHost);
    CHECK(m.Tick(other, me, false, true, 2) == S::GiveUp && !m.Active());

    // the clock wraps
    m.Start(oldH, 0xFFFFFF00u);
    CHECK(m.Tick(oldH, me, true, true, 0x100) == S::WaitOwner);       // 512 ms later
    CHECK(m.Tick(me, me, true, true, 0x200) == S::BecomeHost);

    if (g_fail) { std::printf("test_hsm_migration: %d failure(s)\n", g_fail); return 1; }
    std::printf("test_hsm_migration: all passed\n");
    return 0;
}
