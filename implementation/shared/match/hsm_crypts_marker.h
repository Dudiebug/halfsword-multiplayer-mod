// Crypts: a chip that points a player who holds a key to the nearest locked door (a Discord request). The session lists
// the gate doors it found and whether each is locked; this picks the nearest locked one and words it for the chip beside
// the Crypts line: "Locked door 42 m, ahead-left". Rooms stream in as the player walks, so a locked room that is not
// generated yet cannot be pointed to; the chip says so, and a notice says the rest once a run (KeyLineNeedsNotice). Which
// class the key is and how it is held are read from the game's names (not confirmed in a game yet: docs/HUMAN_TESTING.md).
// Game-independent.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace hsm {

// The Crypts' key classes: the tool key a player carries and the loot key a chest drops.
inline bool IsCryptsKeyClass(const std::string& cls) { return cls == "BP_Weapon_ToolKey_C" || cls == "BP_Loot_Key_C"; }

struct DoorSpot {
    double x = 0, y = 0, z = 0;   // world cm
    bool locked = false;
};

// The nearest locked door to (x, y, z); -1 when none is locked (or the list is empty). Equal: the first.
inline int NearestLockedDoor(const std::vector<DoorSpot>& doors, double x, double y, double z) {
    int best = -1;
    double bestD = 0;
    for (size_t i = 0; i < doors.size(); ++i) {
        const DoorSpot& d = doors[i];
        if (!d.locked) continue;
        const double dx = d.x - x, dy = d.y - y, dz = d.z - z, dd = dx * dx + dy * dy + dz * dz;
        if (!(dd >= 0)) continue;                          // NaN
        if (best < 0 || dd < bestD) { best = int(i); bestD = dd; }
    }
    return best;
}

// "Locked door 42 m": whole metres, rounded; 0 m below half a metre.
inline std::string LockedDoorLabel(double distanceCm) {
    const double m = std::max(0.0, distanceCm) / 100.0;
    char b[40];
    std::snprintf(b, sizeof b, "Locked door %d m", int(std::min(m + 0.5, 99999.0)));
    return b;
}
// The chip's words when no locked door is known. The rest ("A locked room shows once it is built. Rooms appear as you walk.")
// is the notice Id::CryptsKeyLine, told once a run when the chip first reads this.
inline constexpr const char* kNoLockedDoorLine = "Locked room not found yet";
inline bool KeyLineNeedsNotice(const std::string& line, bool told) { return !told && line == kNoLockedDoorLine; }

// The camera's yaw to the target, -180..180 degrees (positive: to the right; UE's +Y is right of +X). camYawDeg: the view's yaw.
inline float RelativeYawDeg(double eyeX, double eyeY, double camYawDeg, double tx, double ty) {
    const double to = std::atan2(ty - eyeY, tx - eyeX) * 57.29577951308232;
    double d = std::fmod(to - camYawDeg + 180.0, 360.0);
    if (d < 0) d += 360.0;
    return float(d - 180.0);
}

// Eight 45-degree sectors around the view: ahead, ahead-right, right, behind-right, behind, and the left ones. NaN: "".
inline const char* DirectionWord(float relYawDeg) {
    if (!(relYawDeg == relYawDeg)) return "";
    const float a = std::fabs(relYawDeg);
    const bool right = relYawDeg > 0;
    if (a <= 22.5f) return "ahead";
    if (a <= 67.5f) return right ? "ahead-right" : "ahead-left";
    if (a <= 112.5f) return right ? "right" : "left";
    if (a <= 157.5f) return right ? "behind-right" : "behind-left";
    return "behind";
}

// The key marker's chip text: empty without a key; the nearest locked door's distance and direction from the camera (eye,
// camYawDeg); kNoLockedDoorLine when none of the doors found is locked.
inline std::string KeyMarkerLine(bool keyHeld, const std::vector<DoorSpot>& doors, double eyeX, double eyeY, double eyeZ, double camYawDeg) {
    if (!keyHeld) return {};
    const int i = NearestLockedDoor(doors, eyeX, eyeY, eyeZ);
    if (i < 0) return kNoLockedDoorLine;
    const DoorSpot& d = doors[size_t(i)];
    const double dx = d.x - eyeX, dy = d.y - eyeY, dz = d.z - eyeZ;
    const std::string where = DirectionWord(RelativeYawDeg(eyeX, eyeY, camYawDeg, d.x, d.y));
    const std::string label = LockedDoorLabel(std::sqrt(dx * dx + dy * dy + dz * dz));
    return where.empty() ? label : label + ", " + where;
}

}  // namespace hsm
