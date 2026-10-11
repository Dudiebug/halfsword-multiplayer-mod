// Stack sampler for the agent bridge: suspends one thread for a moment, unwinds its call stack with the x64
// unwind tables (RtlVirtualUnwind) and reports "module+0xRVA" frames. Runs on the bridge thread.
#pragma once
#include <string>

namespace hsm::agent {
std::string SampleStackJson(unsigned long threadId, int maxFrames);
std::string ThreadsJson();          // every thread of this process: Windows id + engine thread name
}
