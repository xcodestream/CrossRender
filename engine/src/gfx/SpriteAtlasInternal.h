//
// CrossRender
// Автор: xcodestream <xcodestream@gmail.com>
// Лицензия: MIT
// Дата: 15.05.2026
// Описание: внутренняя геометрия атласа спрайтов, доступная юнит-тестам.
//
#pragma once

#include "crossrender/core/Base.h"
#include "crossrender/core/Math.h"

#include <cctype>
#include <string>
#include <cstring>

namespace crossrender {
namespace atlas_detail {

// ---------------------------------------------------------------------------
// Естественный ("run_2" перед "run_10") порядок строк
// ---------------------------------------------------------------------------
// Сравнивает две C-строки; последовательности цифр сравниваются численно,
// всё остальное — побайтово. Возвращает <0, 0 или >0, как strcmp.
inline int NaturalCompare(const char* a, const char* b) {
    const unsigned char* pa = reinterpret_cast<const unsigned char*>(a);
    const unsigned char* pb = reinterpret_cast<const unsigned char*>(b);
    while (*pa && *pb) {
        bool da = std::isdigit(*pa) != 0;
        bool db = std::isdigit(*pb) != 0;
        if (da && db) {
            const unsigned char* sa = pa;
            const unsigned char* sb = pb;
            while (*sa == '0') ++sa;
            while (*sb == '0') ++sb;
            const unsigned char* ea = sa;
            const unsigned char* eb = sb;
            while (std::isdigit(*ea)) ++ea;
            while (std::isdigit(*eb)) ++eb;
            usize la = static_cast<usize>(ea - sa);
            usize lb = static_cast<usize>(eb - sb);
            if (la != lb) return la < lb ? -1 : 1;
            if (la > 0) {
                int c = std::memcmp(sa, sb, la);
                if (c != 0) return c < 0 ? -1 : 1;
            }
            // То же числовое значение: меньшее число ведущих нулей идёт первым.
            usize za = static_cast<usize>(sa - pa);
            usize zb = static_cast<usize>(sb - pb);
            if (za != zb) return za < zb ? -1 : 1;
            pa = ea;
            pb = eb;
            continue;
        }
        if (*pa != *pb) return *pa < *pb ? -1 : 1;
        ++pa;
        ++pb;
    }
    if (*pa) return 1;
    if (*pb) return -1;
    return 0;
}

inline bool NaturalLess(const std::string& a, const std::string& b) {
    return NaturalCompare(a.c_str(), b.c_str()) < 0;
}

// ---------------------------------------------------------------------------
// Нарезка nine-patch
// ---------------------------------------------------------------------------
struct NinePatchQuad {
    Rect dst;  // целевой прямоугольник в пространстве назначения
    Rect src;  // под-прямоугольник в логическом пространстве региона (0,0 .. размер региона)
};

// Разбивает `dst` максимум на девять квадов, беря пиксельные отступы
// `left/top/right/bottom` от `region` (прямоугольник, чьи w/h — это
// *отображаемый* размер региона).  Непомещающиеся отступы сжимаются пропорционально,
// чтобы квад не вывернулся. Возвращает число квадов в `out` (9 при обычной нарезке).
inline int NinePatchQuads(const Rect& region, const Rect& dst, f32 left, f32 top, f32 right,
                          f32 bottom, NinePatchQuad out[9]) {
    if (!out || region.w <= 0.0f || region.h <= 0.0f || dst.w <= 0.0f || dst.h <= 0.0f) return 0;
    left = Clamp(left, 0.0f, region.w);
    right = Clamp(right, 0.0f, region.w);
    top = Clamp(top, 0.0f, region.h);
    bottom = Clamp(bottom, 0.0f, region.h);
    if (left + right > region.w && left + right > 0.0f) {
        f32 s = region.w / (left + right);
        left *= s;
        right *= s;
    }
    if (top + bottom > region.h && top + bottom > 0.0f) {
        f32 s = region.h / (top + bottom);
        top *= s;
        bottom *= s;
    }
    f32 dl = left, dr = right, dt = top, db = bottom;
    if (dl + dr > dst.w && dl + dr > 0.0f) {
        f32 s = dst.w / (dl + dr);
        dl *= s;
        dr *= s;
    }
    if (dt + db > dst.h && dt + db > 0.0f) {
        f32 s = dst.h / (dt + db);
        dt *= s;
        db *= s;
    }

    const f32 sx[4] = {region.x, region.x + left, region.x + region.w - right, region.x + region.w};
    const f32 sy[4] = {region.y, region.y + top, region.y + region.h - bottom, region.y + region.h};
    const f32 dx[4] = {dst.x, dst.x + dl, dst.x + dst.w - dr, dst.x + dst.w};
    const f32 dy[4] = {dst.y, dst.y + dt, dst.y + dst.h - db, dst.y + dst.h};

    int n = 0;
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
            Rect d{dx[col], dy[row], dx[col + 1] - dx[col], dy[row + 1] - dy[row]};
            Rect s{sx[col], sy[row], sx[col + 1] - sx[col], sy[row + 1] - sy[row]};
            if (d.w <= 0.0f || d.h <= 0.0f || s.w <= 0.0f || s.h <= 0.0f) continue;
            out[n].dst = d;
            out[n].src = s;
            ++n;
        }
    }
    return n;
}

// ---------------------------------------------------------------------------
// Поиск на повёрнутой странице
// ---------------------------------------------------------------------------
// Упакованные атласы хранят спрайт, повёрнутый на 90 градусов по часовой:
// логический исходный пиксель (x,y) лежит в сохранённом пикселе (H - 1 - y, x), где H
// — логическая высота спрайта.  `size` — это AtlasRegion::Size() (отображаемый
// размер); отображает прямоугольник лог. пространства в его место на странице.
inline Rect StoredRectForLogical(const Rect& logical, const Vec2& size, bool rotated) {
    if (!rotated) return logical;
    return Rect{size.y - (logical.y + logical.h), logical.x, logical.h, logical.w};
}

// Угол поворота рендерера (в радианах), выпрямляющий сохранённый повёрнутый
// квад.  Положительный угол Renderer2D крутит по часовой на экране, поэтому
// выпрямление «90° по часовой» требует отрицательного (против часовой) четверти-оборота.
inline f32 UprightRotation() { return -kPi * 0.5f; }

}  // namespace atlas_detail
}  // namespace crossrender
