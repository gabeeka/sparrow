#pragma once

#include <chrono>

namespace Nest {

//! Structure that holds some statistics about the current compilation process.
//! It's used to assess the performance of the compiler at a higher level.
struct CompilerStats {
    static CompilerStats& instance() {
        static CompilerStats theInstance;
        return theInstance;
    }

    bool enabled = false;
    int numCtEvals = 0;
    chrono::microseconds timeCtEvals{};
    chrono::microseconds timeImplicitLib{};
    chrono::microseconds timeOpt{};
    chrono::microseconds timeLlc{};
    chrono::microseconds timeLink{};

private:
    CompilerStats() = default;
};

//! Helper class that can accumulate time
class ScopedTimeCapture {
    bool enabled;
    chrono::microseconds& acc;
    chrono::steady_clock::time_point startTime;

public:
    ScopedTimeCapture(bool enabled_, chrono::microseconds& acc_)
        : enabled(enabled_)
        , acc(acc_) {
        if (enabled)
            startTime = chrono::steady_clock::now();
    }
    ~ScopedTimeCapture() {
        if (enabled) {
            acc = acc + chrono::duration_cast<chrono::microseconds>(
                                chrono::steady_clock::now() - startTime);
        }
    }

    ScopedTimeCapture(const ScopedTimeCapture&) = delete;
    ScopedTimeCapture(ScopedTimeCapture&&) = delete;
    const ScopedTimeCapture& operator=(const ScopedTimeCapture&) = delete;
    const ScopedTimeCapture& operator=(ScopedTimeCapture&&) = delete;
};

} // namespace Nest
