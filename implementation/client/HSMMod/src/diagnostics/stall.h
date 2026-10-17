#pragma once
#include <cstddef>
#include <cstdint>

namespace hsm::diagnostics {
void Heartbeat();
// Runs on the existing ModMain worker after successful startup. Does not call UE,
// suspend threads, or acquire the ordinary logger/hook locks.
void RunWatchdog();
class Scope {
public:
    explicit Scope(const char* stage, uint64_t detail = 0);
    ~Scope();
    void Stage(const char* stage);
    void Name(const char* name);
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
private:
    bool active_;
    size_t slot_ = 0;
};
} // namespace hsm::diagnostics
