// Update check: once per game start, one HTTPS GET of GitHub's releases list (api.github.com) on a worker thread, with
// a User-Agent of "HSMMod/<version>" and nothing else about the player. The result is logged, shown on the multiplayer
// page and once in the chat of the first fight level. Which release counts as newer is session/hsm_version.h; the
// request goes through net/http_client.h, so WinHTTP is loaded only now. update_check=0 under [net] turns it off.
#pragma once
#include <string>

namespace hsm::update {
// Starts the check (a no-op when disabled or already started). Returns at once; the worker logs the outcome.
void Start(bool enabled);
// Game thread: "HSMMod 0.7.0-pre3 is out (you have 0.7.0-pre2). Get it from GitHub: Dudiebug/halfsword-multiplayer-mod"
// once a newer release is known, else empty.
std::string Notice();
// Notice() the first time it is non-empty, then empty: for the one chat line.
std::string TakeChatNotice();
}  // namespace hsm::update
