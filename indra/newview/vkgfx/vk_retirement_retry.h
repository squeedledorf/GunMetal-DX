// Bounded scheduling for refused compatibility retirement waits (#1013).
#pragma once
#include <chrono>

class VkRetirementRetry
{
public:
    using Clock = std::chrono::steady_clock;
    bool ready(Clock::time_point now) const { return !exhausted() && now >= mAfter; }
    bool exhausted() const { return mFailures >= 3; }
    void refused(Clock::time_point now)
    {
        if (!exhausted()) ++mFailures;
        mAfter = now + std::chrono::milliseconds(500 * mFailures);
    }
    void succeeded() { mFailures = 0; mAfter = {}; }
private:
    unsigned mFailures = 0;
    Clock::time_point mAfter{};
};
