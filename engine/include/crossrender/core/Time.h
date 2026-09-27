//
// CrossRender
// Автор: xcodestream <xcodestream@gmail.com>
// Лицензия: MIT
// Дата: 15.05.2026
// Описание: время: монотонные часы, покадровые дельты, масштаб времени и счётчик FPS.
//
#pragma once

#include "crossrender/core/Base.h"

#include <string>

namespace crossrender {

// Монотонные секунды от произвольной эпохи (высокое разрешение).
f64 NowSeconds();

// Простой покадровый таймер со сглаживанием и счётчиком FPS.
class Clock {
public:
    Clock();

    void Tick();  // вызывать раз в фрейм

    [[nodiscard]] f32 Delta() const { return dt_; }
    [[nodiscard]] f32 UnscaledDelta() const { return rawDt_; }
    [[nodiscard]] f32 Time() const { return time_; }
    [[nodiscard]] u64 Frame() const { return frame_; }
    [[nodiscard]] f32 FPS() const { return fps_; }
    [[nodiscard]] f32 SmoothedDelta() const { return smoothDt_; }
    [[nodiscard]] f64 TotalSeconds() const { return static_cast<f64>(time_); }

    void SetTimeScale(f32 s) { timeScale_ = s; }
    [[nodiscard]] f32 TimeScale() const { return timeScale_; }
    void SetFixedStep(f32 step) { fixedStep_ = step; }
    [[nodiscard]] f32 FixedStep() const { return fixedStep_; }
    [[nodiscard]] u32 ConsumeFixedSteps();
    [[nodiscard]] f32 FixedAlpha() const { return accumulator_ / (fixedStep_ > 0 ? fixedStep_ : 1.0f); }

    // Помощники для тестов: управляют таймером детерминированно.
    void ForceDelta(f32 dt) { forcedDt_ = dt; }

private:
    f64 last_ = 0;
    f32 dt_ = 0, rawDt_ = 0, smoothDt_ = 0, time_ = 0, timeScale_ = 1;
    f32 fixedStep_ = 1.0f / 60.0f, accumulator_ = 0;
    f32 forcedDt_ = -1;
    f32 fps_ = 0, fpsAccum_ = 0;
    int fpsFrames_ = 0;
    u64 frame_ = 0;
};

// Форматирует секунды как mm:ss.mmm
std::string FormatTime(f32 seconds);

}  // namespace crossrender
