#pragma once

#include <cstdint>

// Late tick start. Mario Kart Wii reads the controller at the start of each tick, works about a
// millisecond, then sleeps until the next retrace before it draws, so the controller read sits
// unused for most of an interval. Holding each tick until shortly before that retrace moves the
// read next to the draw. The game still sees exactly the same retraces.
namespace LateTickStart {

// How long before the next retrace the tick should start: the slowest recent work before the wait,
// plus a 2 ms margin and any miss penalty; at least 3 ms, at most the interval less 2 ms.
// 0 = don't hold.
uint64_t LeadNanos(double workP99Ms, double missPenaltyMs, uint64_t intervalNanos);

// A frame that missed its retrace adds 2 ms of room (up to 8); every frame on time takes 0.05 off.
double NextMissPenalty(double currentMs, bool missedRetrace);

// Tick start -> the drawing thread's first wait, over the last 60 ticks.
class WorkMeter {
public:
    void OnTickStart(uint64_t ns);
    void OnWaitStart(uint64_t ns);
    // Rounds up, so with the full window it is the slowest tick but one. False until 10 ticks.
    bool P99(double& ms) const;

private:
    static constexpr int kWindow = 60;
    double m_samples[kWindow]{};
    int m_count = 0;
    int m_write = 0;
    uint64_t m_tickStart = 0;
    bool m_measured = true;
};

} // namespace LateTickStart
