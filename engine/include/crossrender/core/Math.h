//
// CrossRender
// Автор: xcodestream <xcodestream@gmail.com>
// Лицензия: MIT
// Дата: 15.05.2026
// Описание: математика движка: векторы, матрицы, кватернионы, цвет и прямоугольники.
//
#pragma once

#include "crossrender/core/Base.h"

#include <cmath>
#include <vector>

namespace crossrender {

// ---------------------------------------------------------------------------
// Vec2
// ---------------------------------------------------------------------------
struct Vec2 {
    f32 x = 0, y = 0;
    constexpr Vec2() = default;
    constexpr Vec2(f32 x_, f32 y_) : x(x_), y(y_) {}
    explicit constexpr Vec2(f32 s) : x(s), y(s) {}

    constexpr Vec2 operator+(const Vec2& o) const { return {x + o.x, y + o.y}; }
    constexpr Vec2 operator-(const Vec2& o) const { return {x - o.x, y - o.y}; }
    constexpr Vec2 operator*(f32 s) const { return {x * s, y * s}; }
    constexpr Vec2 operator*(const Vec2& o) const { return {x * o.x, y * o.y}; }
    constexpr Vec2 operator/(f32 s) const { return {x / s, y / s}; }
    constexpr Vec2 operator-() const { return {-x, -y}; }
    Vec2& operator+=(const Vec2& o) { x += o.x; y += o.y; return *this; }
    Vec2& operator-=(const Vec2& o) { x -= o.x; y -= o.y; return *this; }
    Vec2& operator*=(f32 s) { x *= s; y *= s; return *this; }
    constexpr bool operator==(const Vec2& o) const { return x == o.x && y == o.y; }
    constexpr bool operator!=(const Vec2& o) const { return !(*this == o); }
    constexpr f32 operator[](int i) const { return i == 0 ? x : y; }
    f32& operator[](int i) { return i == 0 ? x : y; }
};

[[nodiscard]] constexpr f32 Dot(const Vec2& a, const Vec2& b) { return a.x * b.x + a.y * b.y; }
[[nodiscard]] constexpr f32 Cross(const Vec2& a, const Vec2& b) { return a.x * b.y - a.y * b.x; }
[[nodiscard]] inline f32 Length(const Vec2& v) { return std::sqrt(Dot(v, v)); }
[[nodiscard]] inline f32 LengthSq(const Vec2& v) { return Dot(v, v); }
[[nodiscard]] inline Vec2 Normalize(const Vec2& v) {
    f32 l = Length(v);
    return l > kEpsilon ? v / l : Vec2{};
}
[[nodiscard]] inline Vec2 Lerp(const Vec2& a, const Vec2& b, f32 t) { return a + (b - a) * t; }
[[nodiscard]] inline Vec2 Rotate(const Vec2& v, f32 rad) {
    f32 c = std::cos(rad), s = std::sin(rad);
    return {v.x * c - v.y * s, v.x * s + v.y * c};
}
[[nodiscard]] inline Vec2 Perp(const Vec2& v) { return {-v.y, v.x}; }
[[nodiscard]] inline Vec2 Min(const Vec2& a, const Vec2& b) { return {a.x < b.x ? a.x : b.x, a.y < b.y ? a.y : b.y}; }
[[nodiscard]] inline Vec2 Max(const Vec2& a, const Vec2& b) { return {a.x > b.x ? a.x : b.x, a.y > b.y ? a.y : b.y}; }
[[nodiscard]] inline Vec2 Abs(const Vec2& v) { return {std::fabs(v.x), std::fabs(v.y)}; }

// ---------------------------------------------------------------------------
// Vec3
// ---------------------------------------------------------------------------
struct Vec3 {
    f32 x = 0, y = 0, z = 0;
    constexpr Vec3() = default;
    constexpr Vec3(f32 x_, f32 y_, f32 z_) : x(x_), y(y_), z(z_) {}
    explicit constexpr Vec3(f32 s) : x(s), y(s), z(s) {}
    constexpr Vec3(const Vec2& v, f32 z_) : x(v.x), y(v.y), z(z_) {}

    constexpr Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
    constexpr Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
    constexpr Vec3 operator*(f32 s) const { return {x * s, y * s, z * s}; }
    constexpr Vec3 operator*(const Vec3& o) const { return {x * o.x, y * o.y, z * o.z}; }
    constexpr Vec3 operator/(f32 s) const { return {x / s, y / s, z / s}; }
    constexpr Vec3 operator/(const Vec3& o) const { return {x / o.x, y / o.y, z / o.z}; }
    constexpr Vec3 operator-() const { return {-x, -y, -z}; }
    Vec3& operator+=(const Vec3& o) { x += o.x; y += o.y; z += o.z; return *this; }
    Vec3& operator-=(const Vec3& o) { x -= o.x; y -= o.y; z -= o.z; return *this; }
    Vec3& operator*=(f32 s) { x *= s; y *= s; z *= s; return *this; }
    constexpr bool operator==(const Vec3& o) const { return x == o.x && y == o.y && z == o.z; }
    constexpr bool operator!=(const Vec3& o) const { return !(*this == o); }
    constexpr f32 operator[](int i) const { return i == 0 ? x : (i == 1 ? y : z); }
    f32& operator[](int i) { return i == 0 ? x : (i == 1 ? y : z); }
    [[nodiscard]] constexpr Vec2 xy() const { return {x, y}; }
};

[[nodiscard]] constexpr f32 Dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
[[nodiscard]] constexpr Vec3 Cross(const Vec3& a, const Vec3& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
[[nodiscard]] inline f32 Length(const Vec3& v) { return std::sqrt(Dot(v, v)); }
[[nodiscard]] inline f32 LengthSq(const Vec3& v) { return Dot(v, v); }
[[nodiscard]] inline f32 Distance(const Vec3& a, const Vec3& b) { return Length(b - a); }
[[nodiscard]] inline Vec3 Normalize(const Vec3& v) {
    f32 l = Length(v);
    return l > kEpsilon ? v / l : Vec3{};
}
[[nodiscard]] inline Vec3 Lerp(const Vec3& a, const Vec3& b, f32 t) { return a + (b - a) * t; }
[[nodiscard]] inline Vec3 Min(const Vec3& a, const Vec3& b) {
    return {a.x < b.x ? a.x : b.x, a.y < b.y ? a.y : b.y, a.z < b.z ? a.z : b.z};
}
[[nodiscard]] inline Vec3 Max(const Vec3& a, const Vec3& b) {
    return {a.x > b.x ? a.x : b.x, a.y > b.y ? a.y : b.y, a.z > b.z ? a.z : b.z};
}
[[nodiscard]] inline Vec3 Reflect(const Vec3& v, const Vec3& n) { return v - n * (2.0f * Dot(v, n)); }
[[nodiscard]] inline Vec3 Clamp(const Vec3& v, const Vec3& lo, const Vec3& hi) {
    return Min(Max(v, lo), hi);
}
// Пересечение луча со сферой. Возвращает ближайший положительный t или -1.
[[nodiscard]] inline f32 RaySphere(const Vec3& ro, const Vec3& rd, const Vec3& c, f32 r) {
    Vec3 oc = ro - c;
    f32 b = Dot(oc, rd);
    f32 cc = Dot(oc, oc) - r * r;
    f32 h = b * b - cc;
    if (h < 0) return -1.0f;
    h = std::sqrt(h);
    f32 t = -b - h;
    if (t < 0) t = -b + h;
    return t < 0 ? -1.0f : t;
}
// Пересечение луча с плоскостью: нормаль n, точка p.
[[nodiscard]] inline f32 RayPlane(const Vec3& ro, const Vec3& rd, const Vec3& p, const Vec3& n) {
    f32 denom = Dot(rd, n);
    if (std::fabs(denom) < kEpsilon) return -1.0f;
    return Dot(p - ro, n) / denom;
}
// Slab-тест (границы, выровненные по осям).
[[nodiscard]] inline bool RayAabb(const Vec3& ro, const Vec3& rd, const Vec3& lo, const Vec3& hi, f32* tOut) {
    f32 tmin = -1e30f, tmax = 1e30f;
    for (int i = 0; i < 3; ++i) {
        if (std::fabs(rd[i]) < kEpsilon) {
            if (ro[i] < lo[i] || ro[i] > hi[i]) return false;
        } else {
            f32 inv = 1.0f / rd[i];
            f32 t1 = (lo[i] - ro[i]) * inv;
            f32 t2 = (hi[i] - ro[i]) * inv;
            if (t1 > t2) std::swap(t1, t2);
            tmin = t1 > tmin ? t1 : tmin;
            tmax = t2 < tmax ? t2 : tmax;
            if (tmin > tmax) return false;
        }
    }
    if (tOut) *tOut = tmin >= 0 ? tmin : tmax;
    return tmax >= 0;
}

// ---------------------------------------------------------------------------
// Vec4
// ---------------------------------------------------------------------------
struct Vec4 {
    f32 x = 0, y = 0, z = 0, w = 0;
    constexpr Vec4() = default;
    constexpr Vec4(f32 x_, f32 y_, f32 z_, f32 w_) : x(x_), y(y_), z(z_), w(w_) {}
    constexpr Vec4(const Vec3& v, f32 w_) : x(v.x), y(v.y), z(v.z), w(w_) {}
    explicit constexpr Vec4(f32 s) : x(s), y(s), z(s), w(s) {}

    constexpr Vec4 operator+(const Vec4& o) const { return {x + o.x, y + o.y, z + o.z, w + o.w}; }
    constexpr Vec4 operator-(const Vec4& o) const { return {x - o.x, y - o.y, z - o.z, w - o.w}; }
    constexpr Vec4 operator*(f32 s) const { return {x * s, y * s, z * s, w * s}; }
    constexpr Vec4 operator*(const Vec4& o) const { return {x * o.x, y * o.y, z * o.z, w * o.w}; }
    constexpr Vec4 operator/(f32 s) const { return {x / s, y / s, z / s, w / s}; }
    Vec4& operator+=(const Vec4& o) { x += o.x; y += o.y; z += o.z; w += o.w; return *this; }
    constexpr bool operator==(const Vec4& o) const { return x == o.x && y == o.y && z == o.z && w == o.w; }
    constexpr f32 operator[](int i) const { return i == 0 ? x : (i == 1 ? y : (i == 2 ? z : w)); }
    f32& operator[](int i) { return i == 0 ? x : (i == 1 ? y : (i == 2 ? z : w)); }
    [[nodiscard]] constexpr Vec3 xyz() const { return {x, y, z}; }
    [[nodiscard]] constexpr Vec2 xy() const { return {x, y}; }
};

[[nodiscard]] constexpr f32 Dot(const Vec4& a, const Vec4& b) { return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w; }
[[nodiscard]] inline Vec4 Lerp(const Vec4& a, const Vec4& b, f32 t) { return a + (b - a) * t; }

// ---------------------------------------------------------------------------
// Mat4 (постолбцовая; m[col][row] через m[c*4+r])
// ---------------------------------------------------------------------------
struct Mat4 {
    f32 m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

    constexpr f32& at(int c, int r) { return m[c * 4 + r]; }
    constexpr f32 at(int c, int r) const { return m[c * 4 + r]; }
    const f32* data() const { return m; }
    f32* data() { return m; }

    static Mat4 Identity() { return Mat4{}; }

    static Mat4 Zero() {
        Mat4 r;
        for (int i = 0; i < 16; ++i) r.m[i] = 0;
        return r;
    }

    static Mat4 Translate(const Vec3& t) {
        Mat4 r;
        r.m[12] = t.x; r.m[13] = t.y; r.m[14] = t.z;
        return r;
    }
    static Mat4 Scale(const Vec3& s) {
        Mat4 r;
        r.m[0] = s.x; r.m[5] = s.y; r.m[10] = s.z;
        return r;
    }
    static Mat4 RotateX(f32 a) {
        Mat4 r; f32 c = std::cos(a), s = std::sin(a);
        r.m[5] = c; r.m[6] = s; r.m[9] = -s; r.m[10] = c;
        return r;
    }
    static Mat4 RotateY(f32 a) {
        Mat4 r; f32 c = std::cos(a), s = std::sin(a);
        r.m[0] = c; r.m[2] = -s; r.m[8] = s; r.m[10] = c;
        return r;
    }
    static Mat4 RotateZ(f32 a) {
        Mat4 r; f32 c = std::cos(a), s = std::sin(a);
        r.m[0] = c; r.m[1] = s; r.m[4] = -s; r.m[5] = c;
        return r;
    }
    static Mat4 Rotate(const Vec3& axis, f32 a) {
        Vec3 n = Normalize(axis);
        f32 c = std::cos(a), s = std::sin(a), ic = 1.0f - c;
        Mat4 r;
        r.m[0] = c + n.x * n.x * ic;      r.m[1] = n.y * n.x * ic + n.z * s; r.m[2] = n.z * n.x * ic - n.y * s;
        r.m[4] = n.x * n.y * ic - n.z * s; r.m[5] = c + n.y * n.y * ic;      r.m[6] = n.z * n.y * ic + n.x * s;
        r.m[8] = n.x * n.z * ic + n.y * s; r.m[9] = n.y * n.z * ic - n.x * s; r.m[10] = c + n.z * n.z * ic;
        return r;
    }
    // Углы Эйлера в радианах, применяется Z * Y * X (extrinsic XYZ).
    static Mat4 RotateEuler(const Vec3& e) {
        return RotateZ(e.z) * RotateY(e.y) * RotateX(e.x);
    }

    // Перспективная проекция, reverse depth не используется: near -> -1, far -> +1.
    static Mat4 Perspective(f32 fovYRad, f32 aspect, f32 zn, f32 zf) {
        Mat4 r = Zero();
        f32 f = 1.0f / std::tan(fovYRad * 0.5f);
        r.m[0] = f / aspect;
        r.m[5] = f;
        r.m[10] = (zf + zn) / (zn - zf);
        r.m[11] = -1.0f;
        r.m[14] = (2.0f * zf * zn) / (zn - zf);
        return r;
    }
    static Mat4 Ortho(f32 l, f32 r_, f32 b, f32 t, f32 zn, f32 zf) {
        Mat4 r = Zero();
        r.m[0] = 2.0f / (r_ - l);
        r.m[5] = 2.0f / (t - b);
        r.m[10] = -2.0f / (zf - zn);
        r.m[12] = -(r_ + l) / (r_ - l);
        r.m[13] = -(t + b) / (t - b);
        r.m[14] = -(zf + zn) / (zf - zn);
        r.m[15] = 1.0f;
        return r;
    }
    // 2D-орто: (0,0) в левом верхнем углу, y растёт вниз.
    static Mat4 Ortho2D(f32 width, f32 height) {
        return Ortho(0.0f, width, height, 0.0f, -1.0f, 1.0f);
    }
    static Mat4 LookAt(const Vec3& eye, const Vec3& center, const Vec3& up) {
        Vec3 f = Normalize(center - eye);
        Vec3 s = Normalize(Cross(f, up));
        Vec3 u = Cross(s, f);
        Mat4 r;
        r.m[0] = s.x; r.m[4] = s.y; r.m[8] = s.z;
        r.m[1] = u.x; r.m[5] = u.y; r.m[9] = u.z;
        r.m[2] = -f.x; r.m[6] = -f.y; r.m[10] = -f.z;
        r.m[12] = -Dot(s, eye); r.m[13] = -Dot(u, eye); r.m[14] = Dot(f, eye);
        return r;
    }

    Mat4 operator*(const Mat4& o) const {
        Mat4 r = Zero();
        for (int c = 0; c < 4; ++c)
            for (int rw = 0; rw < 4; ++rw) {
                f32 s = 0;
                for (int k = 0; k < 4; ++k) s += at(k, rw) * o.at(c, k);
                r.at(c, rw) = s;
            }
        return r;
    }
    Vec4 operator*(const Vec4& v) const {
        Vec4 r;
        for (int rw = 0; rw < 4; ++rw)
            r[rw] = at(0, rw) * v.x + at(1, rw) * v.y + at(2, rw) * v.z + at(3, rw) * v.w;
        return r;
    }
    Vec3 TransformPoint(const Vec3& p) const {
        Vec4 r = (*this) * Vec4(p, 1.0f);
        return r.w != 0.0f ? Vec3{r.x / r.w, r.y / r.w, r.z / r.w} : r.xyz();
    }
    Vec3 TransformDir(const Vec3& d) const {
        Vec4 r = (*this) * Vec4(d, 0.0f);
        return r.xyz();
    }

    Mat4 Transposed() const {
        Mat4 r;
        for (int c = 0; c < 4; ++c)
            for (int rw = 0; rw < 4; ++rw) r.at(c, rw) = at(rw, c);
        return r;
    }

    // Общая инверсия 4x4 (разложение по кофакторам).
    Mat4 Inverse() const {
        const f32* a = m;
        f32 inv[16];
        inv[0] = a[5] * a[10] * a[15] - a[5] * a[11] * a[14] - a[9] * a[6] * a[15] + a[9] * a[7] * a[14] +
                 a[13] * a[6] * a[11] - a[13] * a[7] * a[10];
        inv[4] = -a[4] * a[10] * a[15] + a[4] * a[11] * a[14] + a[8] * a[6] * a[15] - a[8] * a[7] * a[14] -
                 a[12] * a[6] * a[11] + a[12] * a[7] * a[10];
        inv[8] = a[4] * a[9] * a[15] - a[4] * a[11] * a[13] - a[8] * a[5] * a[15] + a[8] * a[7] * a[13] +
                 a[12] * a[5] * a[11] - a[12] * a[7] * a[9];
        inv[12] = -a[4] * a[9] * a[14] + a[4] * a[10] * a[13] + a[8] * a[5] * a[14] - a[8] * a[6] * a[13] -
                  a[12] * a[5] * a[10] + a[12] * a[6] * a[9];
        inv[1] = -a[1] * a[10] * a[15] + a[1] * a[11] * a[14] + a[9] * a[2] * a[15] - a[9] * a[3] * a[14] -
                 a[13] * a[2] * a[11] + a[13] * a[3] * a[10];
        inv[5] = a[0] * a[10] * a[15] - a[0] * a[11] * a[14] - a[8] * a[2] * a[15] + a[8] * a[3] * a[14] +
                 a[12] * a[2] * a[11] - a[12] * a[3] * a[10];
        inv[9] = -a[0] * a[9] * a[15] + a[0] * a[11] * a[13] + a[8] * a[1] * a[15] - a[8] * a[3] * a[13] -
                 a[12] * a[1] * a[11] + a[12] * a[3] * a[9];
        inv[13] = a[0] * a[9] * a[14] - a[0] * a[10] * a[13] - a[8] * a[1] * a[14] + a[8] * a[2] * a[13] +
                  a[12] * a[1] * a[10] - a[12] * a[2] * a[9];
        inv[2] = a[1] * a[6] * a[15] - a[1] * a[7] * a[14] - a[5] * a[2] * a[15] + a[5] * a[3] * a[14] +
                 a[13] * a[2] * a[7] - a[13] * a[3] * a[6];
        inv[6] = -a[0] * a[6] * a[15] + a[0] * a[7] * a[14] + a[4] * a[2] * a[15] - a[4] * a[3] * a[14] -
                 a[12] * a[2] * a[7] + a[12] * a[3] * a[6];
        inv[10] = a[0] * a[5] * a[15] - a[0] * a[7] * a[13] - a[4] * a[1] * a[15] + a[4] * a[3] * a[13] +
                  a[12] * a[1] * a[7] - a[12] * a[3] * a[5];
        inv[14] = -a[0] * a[5] * a[14] + a[0] * a[6] * a[13] + a[4] * a[1] * a[14] - a[4] * a[2] * a[13] -
                  a[12] * a[1] * a[6] + a[12] * a[2] * a[5];
        inv[3] = -a[1] * a[6] * a[11] + a[1] * a[7] * a[10] + a[5] * a[2] * a[11] - a[5] * a[3] * a[10] -
                 a[9] * a[2] * a[7] + a[9] * a[3] * a[6];
        inv[7] = a[0] * a[6] * a[11] - a[0] * a[7] * a[10] - a[4] * a[2] * a[11] + a[4] * a[3] * a[10] +
                 a[8] * a[2] * a[7] - a[8] * a[3] * a[6];
        inv[11] = -a[0] * a[5] * a[11] + a[0] * a[7] * a[9] + a[4] * a[1] * a[11] - a[4] * a[3] * a[9] -
                  a[8] * a[1] * a[7] + a[8] * a[3] * a[5];
        inv[15] = a[0] * a[5] * a[10] - a[0] * a[6] * a[9] - a[4] * a[1] * a[10] + a[4] * a[2] * a[9] +
                  a[8] * a[1] * a[6] - a[8] * a[2] * a[5];
        f32 det = a[0] * inv[0] + a[1] * inv[4] + a[2] * inv[8] + a[3] * inv[12];
        Mat4 r;
        if (std::fabs(det) < 1e-12f) return r;  // откат к единичной матрице
        det = 1.0f / det;
        for (int i = 0; i < 16; ++i) r.m[i] = inv[i] * det;
        return r;
    }

    // Верхняя левая 3x3 обратная-транспонированная матрица, для нормалей.
    Mat4 NormalMatrix() const {
        Mat4 r = Inverse();
        for (int c = 0; c < 3; ++c)
            for (int rw = 0; rw < 3; ++rw) {
                f32 t = r.at(c, rw);
                r.at(c, rw) = r.at(rw, c);
                r.at(rw, c) = t;
            }
        r.m[3] = r.m[7] = r.m[11] = 0;
        r.m[12] = r.m[13] = r.m[14] = 0;
        r.m[15] = 1;
        return r;
    }

    static Mat4 TRS(const Vec3& t, const Vec3& eulerRad, const Vec3& s) {
        return Translate(t) * RotateEuler(eulerRad) * Scale(s);
    }
};

// ---------------------------------------------------------------------------
// Quat
// ---------------------------------------------------------------------------
struct Quat {
    f32 x = 0, y = 0, z = 0, w = 1;
    constexpr Quat() = default;
    constexpr Quat(f32 x_, f32 y_, f32 z_, f32 w_) : x(x_), y(y_), z(z_), w(w_) {}

    static Quat Identity() { return {}; }
    static Quat FromAxisAngle(const Vec3& axis, f32 a) {
        Vec3 n = Normalize(axis);
        f32 h = a * 0.5f, s = std::sin(h);
        return {n.x * s, n.y * s, n.z * s, std::cos(h)};
    }
    static Quat FromEuler(const Vec3& e) {  // порядок XYZ, радианы
        f32 cx = std::cos(e.x * 0.5f), sx = std::sin(e.x * 0.5f);
        f32 cy = std::cos(e.y * 0.5f), sy = std::sin(e.y * 0.5f);
        f32 cz = std::cos(e.z * 0.5f), sz = std::sin(e.z * 0.5f);
        return {sx * cy * cz - cx * sy * sz, cx * sy * cz + sx * cy * sz,
                cx * cy * sz - sx * sy * cz, cx * cy * cz + sx * sy * sz};
    }
    // Извлекает поворот из ортонормированного базиса. `m.at(col, row)` —
    // постолбцовый аксессор, поэтому стандартные построчные формулы отображаются как
    //   m[r][c] -> m.at(c, r).
    static Quat FromMat4(const Mat4& m) {
        f32 t = m.at(0, 0) + m.at(1, 1) + m.at(2, 2);
        if (t > 0) {
            f32 s = std::sqrt(t + 1.0f) * 2;
            return {(m.at(1, 2) - m.at(2, 1)) / s, (m.at(2, 0) - m.at(0, 2)) / s,
                    (m.at(0, 1) - m.at(1, 0)) / s, 0.25f * s};
        }
        if (m.at(0, 0) > m.at(1, 1) && m.at(0, 0) > m.at(2, 2)) {
            f32 s = std::sqrt(1.0f + m.at(0, 0) - m.at(1, 1) - m.at(2, 2)) * 2;
            return {0.25f * s, (m.at(0, 1) + m.at(1, 0)) / s, (m.at(0, 2) + m.at(2, 0)) / s,
                    (m.at(1, 2) - m.at(2, 1)) / s};
        }
        if (m.at(1, 1) > m.at(2, 2)) {
            f32 s = std::sqrt(1.0f + m.at(1, 1) - m.at(0, 0) - m.at(2, 2)) * 2;
            return {(m.at(0, 1) + m.at(1, 0)) / s, 0.25f * s, (m.at(1, 2) + m.at(2, 1)) / s,
                    (m.at(2, 0) - m.at(0, 2)) / s};
        }
        f32 s = std::sqrt(1.0f + m.at(2, 2) - m.at(0, 0) - m.at(1, 1)) * 2;
        return {(m.at(0, 2) + m.at(2, 0)) / s, (m.at(1, 2) + m.at(2, 1)) / s, 0.25f * s,
                (m.at(0, 1) - m.at(1, 0)) / s};
    }

    Quat operator*(const Quat& o) const {
        return {w * o.x + x * o.w + y * o.z - z * o.y,
                w * o.y - x * o.z + y * o.w + z * o.x,
                w * o.z + x * o.y - y * o.x + z * o.w,
                w * o.w - x * o.x - y * o.y - z * o.z};
    }
    Vec3 operator*(const Vec3& v) const {
        Vec3 u{x, y, z};
        return u * (2.0f * Dot(u, v)) + v * (w * w - Dot(u, u)) + Cross(u, v) * (2.0f * w);
    }
    Quat Conjugate() const { return {-x, -y, -z, w}; }
    Quat Normalized() const {
        f32 l = std::sqrt(x * x + y * y + z * z + w * w);
        return l > kEpsilon ? Quat{x / l, y / l, z / l, w / l} : Quat{};
    }
    Mat4 ToMat4() const {
        Mat4 r;
        f32 xx = x * x, yy = y * y, zz = z * z;
        f32 xy = x * y, xz = x * z, yz = y * z;
        f32 wx = w * x, wy = w * y, wz = w * z;
        r.m[0] = 1 - 2 * (yy + zz); r.m[1] = 2 * (xy + wz);     r.m[2] = 2 * (xz - wy);
        r.m[4] = 2 * (xy - wz);     r.m[5] = 1 - 2 * (xx + zz); r.m[6] = 2 * (yz + wx);
        r.m[8] = 2 * (xz + wy);     r.m[9] = 2 * (yz - wx);     r.m[10] = 1 - 2 * (xx + yy);
        return r;
    }
    static Quat Slerp(const Quat& a, Quat b, f32 t) {
        f32 d = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
        if (d < 0) { b = {-b.x, -b.y, -b.z, -b.w}; d = -d; }
        if (d > 0.9995f) {
            return Quat{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t,
                        a.w + (b.w - a.w) * t}
                .Normalized();
        }
        f32 theta = std::acos(d);
        f32 s = std::sin(theta);
        f32 wa = std::sin((1 - t) * theta) / s;
        f32 wb = std::sin(t * theta) / s;
        return {a.x * wa + b.x * wb, a.y * wa + b.y * wb, a.z * wa + b.z * wb, a.w * wa + b.w * wb};
    }
    static Quat NLerp(const Quat& a, const Quat& b, f32 t) {
        return Quat{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t,
                    a.w + (b.w - a.w) * t}
            .Normalized();
    }
    static Quat LookRotation(const Vec3& dir, const Vec3& up) {
        Vec3 f = Normalize(dir);
        Vec3 s = Normalize(Cross(f, up));
        Vec3 u = Cross(s, f);
        Mat4 m;
        m.m[0] = s.x; m.m[4] = s.y; m.m[8] = s.z;
        m.m[1] = u.x; m.m[5] = u.y; m.m[9] = u.z;
        m.m[2] = -f.x; m.m[6] = -f.y; m.m[10] = -f.z;
        return FromMat4(m);
    }
};

// ---------------------------------------------------------------------------
// Цвет (почти линейный RGBA float, помощники для hex)
// ---------------------------------------------------------------------------
struct Color {
    f32 r = 1, g = 1, b = 1, a = 1;
    constexpr Color() = default;
    constexpr Color(f32 r_, f32 g_, f32 b_, f32 a_ = 1.0f) : r(r_), g(g_), b(b_), a(a_) {}
    explicit constexpr Color(f32 v) : r(v), g(v), b(v), a(v) {}
    constexpr Color(const Vec3& v, f32 a_ = 1.0f) : r(v.x), g(v.y), b(v.z), a(a_) {}
    constexpr Color(const Vec4& v) : r(v.x), g(v.y), b(v.z), a(v.w) {}

    [[nodiscard]] Vec4 ToVec4() const { return {r, g, b, a}; }
    [[nodiscard]] Vec3 rgb() const { return {r, g, b}; }

    Color operator*(f32 s) const { return {r * s, g * s, b * s, a * s}; }
    Color operator*(const Color& o) const { return {r * o.r, g * o.g, b * o.b, a * o.a}; }
    Color operator+(const Color& o) const { return {r + o.r, g + o.g, b + o.b, a + o.a}; }
    bool operator==(const Color& o) const { return r == o.r && g == o.g && b == o.b && a == o.a; }

    Color WithAlpha(f32 na) const { return {r, g, b, na}; }

    static Color FromBytes(u8 r_, u8 g_, u8 b_, u8 a_ = 255) {
        return {r_ / 255.0f, g_ / 255.0f, b_ / 255.0f, a_ / 255.0f};
    }
    // 0xRRGGBB (alpha = 255) / 0xAARRGGBB
    static Color FromRGB(u32 hex) {
        return FromBytes((hex >> 16) & 0xFF, (hex >> 8) & 0xFF, hex & 0xFF, 255);
    }
    static Color FromARGB(u32 hex) {
        return FromBytes((hex >> 16) & 0xFF, (hex >> 8) & 0xFF, hex & 0xFF, (hex >> 24) & 0xFF);
    }
    static Color HSL(f32 h, f32 s, f32 l, f32 a = 1.0f);
    void ToHSL(f32* h, f32* s, f32* l) const;

    u32 ToRGBA8() const {
        auto q = [](f32 v) { return static_cast<u32>(v <= 0 ? 0 : (v >= 1 ? 255 : v * 255.0f + 0.5f)); };
        return q(r) | (q(g) << 8) | (q(b) << 16) | (q(a) << 24);
    }

    static const Color White;
    static const Color Black;
    static const Color Transparent;
    static const Color Red;
    static const Color Green;
    static const Color Blue;
    static const Color Yellow;
    static const Color Cyan;
    static const Color Magenta;
    static const Color Gray;
};

[[nodiscard]] inline Color Lerp(const Color& a, const Color& b, f32 t) {
    return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t};
}

// ---------------------------------------------------------------------------
// 2D-прямоугольник (начало в левом верхнем углу, y вниз)
// ---------------------------------------------------------------------------
struct Rect {
    f32 x = 0, y = 0, w = 0, h = 0;
    constexpr Rect() = default;
    constexpr Rect(f32 x_, f32 y_, f32 w_, f32 h_) : x(x_), y(y_), w(w_), h(h_) {}

    static Rect FromMinMax(const Vec2& lo, const Vec2& hi) { return {lo.x, lo.y, hi.x - lo.x, hi.y - lo.y}; }
    [[nodiscard]] f32 Left() const { return x; }
    [[nodiscard]] f32 Top() const { return y; }
    [[nodiscard]] f32 Right() const { return x + w; }
    [[nodiscard]] f32 Bottom() const { return y + h; }
    [[nodiscard]] Vec2 Min() const { return {x, y}; }
    [[nodiscard]] Vec2 Max() const { return {x + w, y + h}; }
    [[nodiscard]] Vec2 Size() const { return {w, h}; }
    [[nodiscard]] Vec2 Center() const { return {x + w * 0.5f, y + h * 0.5f}; }
    [[nodiscard]] bool Contains(const Vec2& p) const {
        return p.x >= x && p.x < x + w && p.y >= y && p.y < y + h;
    }
    [[nodiscard]] bool Intersects(const Rect& o) const {
        return !(o.x >= x + w || o.x + o.w <= x || o.y >= y + h || o.y + o.h <= y);
    }
    [[nodiscard]] Rect Inset(f32 d) const { return {x + d, y + d, w - 2 * d, h - 2 * d}; }
    [[nodiscard]] Rect Inset(f32 dx, f32 dy) const { return {x + dx, y + dy, w - 2 * dx, h - 2 * dy}; }
    [[nodiscard]] Rect Offset(f32 dx, f32 dy) const { return {x + dx, y + dy, w, h}; }
    [[nodiscard]] Rect Union(const Rect& o) const {
        f32 l = x < o.x ? x : o.x, t = y < o.y ? y : o.y;
        f32 r = Right() > o.Right() ? Right() : o.Right();
        f32 b = Bottom() > o.Bottom() ? Bottom() : o.Bottom();
        return {l, t, r - l, b - t};
    }
    [[nodiscard]] Rect Intersect(const Rect& o) const {
        f32 l = x > o.x ? x : o.x, t = y > o.y ? y : o.y;
        f32 r = Right() < o.Right() ? Right() : o.Right();
        f32 b = Bottom() < o.Bottom() ? Bottom() : o.Bottom();
        return {l, t, r - l > 0 ? r - l : 0, b - t > 0 ? b - t : 0};
    }
};

// ---------------------------------------------------------------------------
// Transform (2D + 3D помощники), используется сценами и виджетами
// ---------------------------------------------------------------------------
struct Transform2D {
    Vec2 position{0, 0};
    Vec2 scale{1, 1};
    f32 rotation = 0;
    [[nodiscard]] Mat4 ToMat4() const {
        return Mat4::Translate(Vec3{position.x, position.y, 0}) * Mat4::RotateZ(rotation) *
               Mat4::Scale(Vec3{scale.x, scale.y, 1});
    }
};

struct Bounds {
    Vec3 min{1e30f, 1e30f, 1e30f};
    Vec3 max{-1e30f, -1e30f, -1e30f};
    void Expand(const Vec3& p) {
        min = Min(min, p);
        max = Max(max, p);
    }
    void Expand(const Bounds& b) {
        if (b.Valid()) {
            Expand(b.min);
            Expand(b.max);
        }
    }
    [[nodiscard]] bool Valid() const { return min.x <= max.x; }
    [[nodiscard]] Vec3 Center() const { return (min + max) * 0.5f; }
    [[nodiscard]] Vec3 Extents() const { return (max - min) * 0.5f; }
    [[nodiscard]] f32 Radius() const { return Valid() ? Length(max - min) * 0.5f : 0.0f; }
};

// ---------------------------------------------------------------------------
// Скалярные помощники
// ---------------------------------------------------------------------------
template <typename T>
[[nodiscard]] constexpr T Clamp(T v, T lo, T hi) { return v < lo ? lo : (v > hi ? hi : v); }
template <typename T>
[[nodiscard]] constexpr T MinT(T a, T b) { return a < b ? a : b; }
template <typename T>
[[nodiscard]] constexpr T MaxT(T a, T b) { return a > b ? a : b; }
template <typename T>
[[nodiscard]] constexpr T Lerp(T a, T b, f32 t) { return static_cast<T>(a + (b - a) * t); }
template <typename T>
[[nodiscard]] constexpr T AbsT(T v) { return v < 0 ? -v : v; }

[[nodiscard]] inline f32 SmoothStep(f32 e0, f32 e1, f32 x) {
    f32 t = Clamp((x - e0) / (e1 - e0 + kEpsilon), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}
[[nodiscard]] inline f32 MoveTowards(f32 cur, f32 target, f32 maxDelta) {
    f32 d = target - cur;
    if (std::fabs(d) <= maxDelta) return target;
    return cur + (d > 0 ? maxDelta : -maxDelta);
}
// Решатель easing на кубической кривой Безье (используется Lottie и UI-анимациями).
[[nodiscard]] f32 CubicBezierEase(f32 x1, f32 y1, f32 x2, f32 y2, f32 t);
// Сглаживание в стиле Catmull-Rom, используется для демпфирования камеры.
[[nodiscard]] inline f32 Damp(f32 cur, f32 target, f32 lambda, f32 dt) {
    return Lerp(cur, target, 1.0f - std::exp(-lambda * dt));
}

[[nodiscard]] inline bool NearlyEqual(f32 a, f32 b, f32 eps = 1e-5f) { return std::fabs(a - b) <= eps; }

// Генератор случайных чисел (xorshift128+, детерминированный).
class Random {
public:
    Random() : Random(0x9E3779B97F4A7C15ULL) {}
    explicit Random(u64 seed) { Seed(seed); }
    void Seed(u64 seed) {
        s_[0] = seed ^ 0x9E3779B97F4A7C15ULL;
        s_[1] = seed * 0xBF58476D1CE4E5B9ULL + 1;
        for (int i = 0; i < 8; ++i) NextU64();
    }
    u64 NextU64() {
        u64 x = s_[0], y = s_[1];
        s_[0] = y;
        x ^= x << 23;
        s_[1] = x ^ y ^ (x >> 17) ^ (y >> 26);
        return s_[1] + y;
    }
    u32 NextU32() { return static_cast<u32>(NextU64() >> 32); }
    f32 NextFloat() { return static_cast<f32>(NextU64() >> 40) / 16777216.0f; }
    f32 Range(f32 lo, f32 hi) { return lo + (hi - lo) * NextFloat(); }
    i32 RangeInt(i32 lo, i32 hi) { return lo + static_cast<i32>(NextU32() % static_cast<u32>(hi - lo + 1)); }
    bool Chance(f32 p) { return NextFloat() < p; }
    Vec2 OnUnitCircle() {
        f32 a = Range(0.0f, kTau);
        return {std::cos(a), std::sin(a)};
    }
    Vec3 InUnitSphere() {
        for (int i = 0; i < 16; ++i) {
            Vec3 p{Range(-1, 1), Range(-1, 1), Range(-1, 1)};
            if (LengthSq(p) <= 1.0f) return p;
        }
        return {0, 0, 0};
    }
    // Перемешивание Фишера-Йетса.
    template <typename T>
    void Shuffle(std::vector<T>& v) {
        for (usize i = v.size(); i > 1; --i) {
            usize j = NextU32() % i;
            std::swap(v[i - 1], v[j]);
        }
    }

private:
    u64 s_[2]{};
};

}  // namespace crossrender
