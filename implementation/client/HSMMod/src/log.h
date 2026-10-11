// Minimal thread-safe file logger: <game exe dir>/HSMMod.log
#pragma once
#include <cstdarg>

namespace hsm {
void LogInit();
void Log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
}  // namespace hsm

#define HSM_LOG(...) ::hsm::Log(__VA_ARGS__)
