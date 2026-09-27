#include "crossrender/core/Time.h"

#include <chrono>
#include <cstdio>

namespace crossrender {

f64 NowSeconds() {
    using clock = std::chrono::steady_clock;
    static const auto start = clock::now();
    return std::chrono::duration<f64>(clock::now() - start).count();
}

f64 HighResTimerSeconds() { return NowSeconds(); }

Clock::Clock() { last_ = NowSeconds(); }

void Clock::Tick() {
    f64 now = NowSeconds();
    f32 raw = static_cast<f32>(now - last_);
    last_ = now;
    if (forcedDt_ >= 0) {
        raw = forcedDt_;
    }
    // Защита от больших простоев (точки останова, перетаскивание окна).
    if (raw > 0.25f) raw = 0.25f;
    if (raw < 0) raw = 0;
    rawDt_ = raw;
    dt_ = raw * timeScale_;
    time_ += dt_;
    smoothDt_ = smoothDt_ == 0 ? dt_ : smoothDt_ + (dt_ - smoothDt_) * 0.1f;
    accumulator_ += dt_;
    ++frame_;

    fpsAccum_ += rawDt_;
    ++fpsFrames_;
    if (fpsAccum_ >= 0.25f) {
        fps_ = fpsFrames_ / fpsAccum_;
        fpsAccum_ = 0;
        fpsFrames_ = 0;
    }
}

u32 Clock::ConsumeFixedSteps() {
    if (fixedStep_ <= 0) return 0;
    u32 steps = 0;
    // Не даём накопителю убегать: не более 8 догоняющих шагов за фрейм.
    while (accumulator_ >= fixedStep_ && steps < 8) {
        accumulator_ -= fixedStep_;
        ++steps;
    }
    if (steps == 8) accumulator_ = 0;
    return steps;
}

std::string FormatTime(f32 seconds) {
    if (seconds < 0) seconds = 0;
    int total = static_cast<int>(seconds);
    int m = total / 60;
    int s = total % 60;
    int ms = static_cast<int>((seconds - static_cast<f32>(total)) * 1000.0f);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%02d:%02d.%03d", m, s, ms);
    return buf;
}

}  // namespace crossrender
