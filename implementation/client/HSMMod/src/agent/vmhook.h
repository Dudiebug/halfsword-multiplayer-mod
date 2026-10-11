// Agent command "vmhook": probes the script-VM hook (ue/script_hook.h) on a live game. Game thread.
//   vmhook stats                       counters, hooked function count, estimated detour cost per executor call
//   vmhook log <Class> <Function>      log every call's parameters (first 200 calls of the rule)
//   vmhook block <Class> <Function>    block the body (outputs zeroed) and log the call (first 200)
//   vmhook hits                        one line per strike on a fighter (Willie_BP_C "Deal Complex Damage"): who struck
//                                      whom with which component, bones, contacts, peak speed and impulse, real damage
//   vmhook off                         remove the rules vmhook made (other owners' rules, e.g. the stand-in guard, stay)
// The command line splits on whitespace, so '+' in a function name stands for a space ("Deal+Complex+Damage").
#pragma once
#include <functional>
#include <string>
#include <vector>

namespace hsm::ue { struct UObject; }

namespace hsm::agent {
// localFighter returns the local player's fighter (or nullptr); "hits" marks it "(you)". Called on the game thread.
std::string VmHookCommand(const std::vector<std::string>& args, std::function<ue::UObject*()> localFighter);   // JSON object
// Writes out the "hits" strikes still open (call before a level change: their fighters are about to go).
void VmHookFlush();
}
