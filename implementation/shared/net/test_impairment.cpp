#include "net/hsm_impairment.h"
#include <cassert>
#include <vector>

int main() {
    using hsm::ImpairmentQueue;
    assert(!hsm::ImpairmentSettings{}.Enabled());
    assert((hsm::ImpairmentSettings{1, 0, 0}.Enabled()));
    assert((hsm::ImpairmentSettings{0, 1, 0}.Enabled()));
    assert((hsm::ImpairmentSettings{0, 0, 1}.Enabled()));

    std::vector<int> sent;
    auto send = [&](int packet) { sent.push_back(packet); };
    ImpairmentQueue<int> off({}, 1);
    off.Push(42, false, 10);
    off.Drain(10, send);
    assert((sent == std::vector<int>{42}));
    sent.clear();

    // Fixed delay preserves order across equal deadlines, channels, and drain batches.
    ImpairmentQueue<int> fixed({100, 0, 0}, 1);
    fixed.Push(1, false, 10);
    fixed.Push(2, true, 10);
    fixed.Push(3, false, 11);
    fixed.Drain(109.99, send);
    assert(sent.empty());
    fixed.Drain(110, send);
    assert((sent == std::vector<int>{1, 2}));
    fixed.Drain(111, send);
    assert((sent == std::vector<int>{1, 2, 3}));
    sent.clear();

    // Even total loss retains every reliable payload and still delays it.
    ImpairmentQueue<int> loss({50, 0, 100}, 2);
    for (int i = 0; i < 100; ++i) loss.Push(i, i % 2 == 0, 0);
    loss.Drain(49, send);
    assert(sent.empty());
    loss.Drain(50, send);
    assert(sent.size() == 50);
    for (int i = 0; i < 50; ++i) assert(sent[i] == i * 2);
    sent.clear();

    ImpairmentQueue<int> partial({0, 0, 25}, 123);
    for (int i = 0; i < 10000; ++i) partial.Push(i, false, 0);
    partial.Drain(0, send);
    assert(sent.size() > 7000 && sent.size() < 8000);
    sent.clear();

    // Jitter can reorder unreliable payloads within lag +/- jitter.
    ImpairmentQueue<int> jitter({100, 50, 0}, 123);
    for (int i = 0; i < 100; ++i) jitter.Push(i, false, 0);
    jitter.Drain(49, send);
    assert(sent.empty());
    jitter.Drain(100, send);
    assert(!sent.empty() && sent.size() < 100);
    jitter.Drain(150, send);
    assert(sent.size() == 100);
    assert(!std::is_sorted(sent.begin(), sent.end()));
    std::sort(sent.begin(), sent.end());
    for (int i = 0; i < 100; ++i) assert(sent[i] == i);
    sent.clear();

    // Reliable payloads retain push order across batches despite jitter and interleaved unreliable payloads.
    ImpairmentQueue<int> reliableJitter({100, 50, 0}, 123);
    auto reliableSend = [&](int packet) { if (packet >= 0) sent.push_back(packet); };
    for (int i = 0; i < 200; ++i) {
        reliableJitter.Push(i, true, i);
        reliableJitter.Push(-i - 1, false, i);
        reliableJitter.Drain(i, reliableSend);
        assert(std::is_sorted(sent.begin(), sent.end()));
    }
    assert(!sent.empty() && sent.size() < 200);
    reliableJitter.Drain(350, reliableSend);
    assert(sent.size() == 200);
    for (int i = 0; i < 200; ++i) assert(sent[i] == i);
    sent.clear();

    // Jitter larger than lag clamps negative delays to zero.
    ImpairmentQueue<int> clamp({0, 50, 0}, 123);
    for (int i = 0; i < 100; ++i) clamp.Push(i, false, 10);
    clamp.Drain(9, send);
    assert(sent.empty());
    clamp.Drain(10, send);
    assert(!sent.empty() && sent.size() < 100);
    clamp.Drain(60, send);
    assert(sent.size() == 100);
    sent.clear();

    // Disconnect cancellation leaves other peers' packets intact.
    ImpairmentQueue<int> cancel({150, 0, 0}, 1);
    cancel.Push(1, true, 0);
    cancel.Push(2, false, 0);
    cancel.RemoveIf([](int peer) { return peer == 1; });
    cancel.Drain(150, send);
    assert((sent == std::vector<int>{2}));
    sent.clear();
    cancel.Drain(1000, send);
    assert(sent.empty());
}
