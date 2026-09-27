#include "crossrender/core/Math.h"

#include <algorithm>

namespace crossrender {

const Color Color::White{1, 1, 1, 1};
const Color Color::Black{0, 0, 0, 1};
const Color Color::Transparent{0, 0, 0, 0};
const Color Color::Red{1, 0, 0, 1};
const Color Color::Green{0, 1, 0, 1};
const Color Color::Blue{0, 0, 1, 1};
const Color Color::Yellow{1, 1, 0, 1};
const Color Color::Cyan{0, 1, 1, 1};
const Color Color::Magenta{1, 0, 1, 1};
const Color Color::Gray{0.5f, 0.5f, 0.5f, 1};

Color Color::HSL(f32 h, f32 s, f32 l, f32 a) {
    h = h - std::floor(h);  // замыкаем в [0,1)
    s = Clamp(s, 0.0f, 1.0f);
    l = Clamp(l, 0.0f, 1.0f);
    if (s <= kEpsilon) return {l, l, l, a};
    auto hue2rgb = [](f32 p, f32 q, f32 t) {
        if (t < 0) t += 1;
        if (t > 1) t -= 1;
        if (t < 1.0f / 6.0f) return p + (q - p) * 6.0f * t;
        if (t < 1.0f / 2.0f) return q;
        if (t < 2.0f / 3.0f) return p + (q - p) * (2.0f / 3.0f - t) * 6.0f;
        return p;
    };
    f32 q = l < 0.5f ? l * (1 + s) : l + s - l * s;
    f32 p = 2 * l - q;
    return {hue2rgb(p, q, h + 1.0f / 3.0f), hue2rgb(p, q, h), hue2rgb(p, q, h - 1.0f / 3.0f), a};
}

void Color::ToHSL(f32* h, f32* s, f32* l) const {
    f32 mx = MaxT(MaxT(r, g), b);
    f32 mn = MinT(MinT(r, g), b);
    f32 ll = (mx + mn) * 0.5f;
    f32 hh = 0, ss = 0;
    if (mx - mn > kEpsilon) {
        f32 d = mx - mn;
        ss = ll > 0.5f ? d / (2.0f - mx - mn) : d / (mx + mn);
        if (mx == r)
            hh = (g - b) / d + (g < b ? 6.0f : 0.0f);
        else if (mx == g)
            hh = (b - r) / d + 2.0f;
        else
            hh = (r - g) / d + 4.0f;
        hh /= 6.0f;
    }
    if (h) *h = hh;
    if (s) *s = ss;
    if (l) *l = ll;
}

// Сглаживание кубической кривой Безье: решаем x(t) = targetX относительно t
// методом Ньютона с откатом к бисекции, затем вычисляем y(t).
f32 CubicBezierEase(f32 x1, f32 y1, f32 x2, f32 y2, f32 x) {
    if (x <= 0) return 0;
    if (x >= 1) return 1;

    auto bez = [](f32 t, f32 a, f32 b) {
        f32 mt = 1 - t;
        // 3*(1-t)^2*t*a + 3*(1-t)*t^2*b + t^3
        return 3.0f * mt * mt * t * a + 3.0f * mt * t * t * b + t * t * t;
    };
    auto bezDeriv = [](f32 t, f32 a, f32 b) {
        f32 mt = 1 - t;
        return 3.0f * mt * mt * a + 6.0f * mt * t * (b - a) + 3.0f * t * t * (1.0f - b);
    };

    // Быстрый путь для линейного случая.
    if (NearlyEqual(x1, y1) && NearlyEqual(x2, y2)) return x;

    f32 t = x;
    for (int i = 0; i < 8; ++i) {
        f32 cx = bez(t, x1, x2) - x;
        if (std::fabs(cx) < 1e-6f) return bez(t, y1, y2);
        f32 d = bezDeriv(t, x1, x2);
        if (std::fabs(d) < 1e-6f) break;
        t -= cx / d;
    }
    // Откат к бисекции.
    f32 lo = 0, hi = 1;
    t = x;
    for (int i = 0; i < 24; ++i) {
        f32 cx = bez(t, x1, x2);
        if (std::fabs(cx - x) < 1e-6f) break;
        if (cx < x)
            lo = t;
        else
            hi = t;
        t = (lo + hi) * 0.5f;
    }
    return bez(t, y1, y2);
}

}  // namespace crossrender
