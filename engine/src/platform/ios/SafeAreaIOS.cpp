// Инсеты safe-area для iOS.
//
// В Ui.cpp (engine/src/ui/Ui.cpp) уже есть *неограждённое* определение
// SafeArea::Query(), которое безусловно возвращает нули на iOS/Android, поэтому
// второе (сильное) определение здесь сломало бы сборку из-за дублирующегося
// символа. Переопределение ниже — поэтому `weak`-определение:
//
//   * сегодня компоновщик оставляет сильное определение из Ui.cpp и раскладка
//     не затронута (инсеты UIKit, записываемые через eng_ios_set_safe_area(),
//     фиксируются, но пока не используются);
//   * как только определение в Ui.cpp будет ограждено (например, `#if
//     !defined(ENG_PLATFORM_IOS) && !defined(ENG_PLATFORM_ANDROID)`), это
//     weak-определение станет единственным и реальные инсеты потекут в
//     раскладку без дальнейших платформенных правок.
//
// Mach-O разрешает weak-определение в пользу сильного без диагностики, поэтому
// iOS-сборка линкуется в обоих случаях.
#include "IOSPlatform.h"

#if defined(ENG_PLATFORM_IOS)

#include "crossrender/ui/Ui.h"
#include "crossrender/core/Log.h"

namespace {
// Записывается из главного потока (колбэки раскладки UIKit), читается тем, кто
// вызывает SafeArea::Query() (на практике тоже главный поток). Обычные float —
// намеренно: разорванное чтение здесь ничего не стоит и избавляет от блокировки
// в горячем пути.
float g_left = 0.0f, g_top = 0.0f, g_right = 0.0f, g_bottom = 0.0f;
bool g_valid = false;
}  // namespace

extern "C" void eng_ios_set_safe_area(float left, float top, float right, float bottom) {
    // Защита от отрицательных значений (UIKit сообщает нули до первого
    // прохода раскладки, а NaN отравил бы прямоугольники UI).
    g_left = left > 0.0f ? left : 0.0f;
    g_top = top > 0.0f ? top : 0.0f;
    g_right = right > 0.0f ? right : 0.0f;
    g_bottom = bottom > 0.0f ? bottom : 0.0f;
    if (!g_valid) {
        g_valid = true;
        ENG_LOGI("platform", "safe area insets: l=%.1f t=%.1f r=%.1f b=%.1f", g_left, g_top,
                 g_right, g_bottom);
    }
}

extern "C" void eng_ios_get_safe_area(float* left, float* top, float* right, float* bottom) {
    if (left) *left = g_left;
    if (top) *top = g_top;
    if (right) *right = g_right;
    if (bottom) *bottom = g_bottom;
}

namespace crossrender {

// См. заголовок файла: weak, чтобы не конфликтовать с сильным определением из Ui.cpp.
__attribute__((weak)) SafeArea SafeArea::Query() {
    SafeArea sa;
    sa.left = g_left;
    sa.top = g_top;
    sa.right = g_right;
    sa.bottom = g_bottom;
    return sa;
}

}  // namespace crossrender

#else

// В чужих сборках эта единица трансляции компилируется в пустоту.
namespace crossrender {}

#endif  // ENG_PLATFORM_IOS
