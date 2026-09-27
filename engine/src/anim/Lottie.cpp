// Воспроизведение Lottie (bodymovin JSON): разбор, вычисление ключевых кадров,
// состояние плеера, валидация и отрисовка через Renderer2D.
//
// Замечания о представлении (см. LottieInternal.h):
//   * `LottieProperty::EvaluatePath()` возвращает плоский, независимый от
//     трансформаций список вершин из триплетов (вершина, входная касательная,
//     выходная касательная), где касательные хранятся *относительно* своей
//     вершины. Замкнутые пути повторяют первую вершину один раз в конце, чтобы
//     замыкающий сегмент был явным. Полные кубические рычаги лежат в
//     `LottieImpl::paths` — их флэттенирует рендерер; ручные свойства откатываются к плоскому списку.
//   * Стили шейпов (`fl`/`st`/`gf`/`gs`), тримы (`tm`) и offset-пути (`op`)
//     применяются к элементам геометрии, следующим за ними внутри той же группы,
//     что соответствует порядку детей bodymovin.
#include "LottieInternal.h"

#include "crossrender/core/Log.h"
#include "crossrender/core/File.h"
#include "crossrender/text/Font.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <unordered_set>

namespace crossrender {

namespace {

constexpr f32 kMaxAdvance = 3600.0f;

f32 Clampf(f32 v, f32 lo, f32 hi) { return v < lo ? lo : (v > hi ? hi : v); }

bool IsFinite(f32 v) { return v == v && v > -1e30f && v < 1e30f; }

// ---------------------------------------------------------------------------
// Небольшие JSON-хелперы (все защитные: отсутствующие ключи / неверные типы допускаются)
// ---------------------------------------------------------------------------
const JsonValue* Field(const JsonValue& obj, const char* key) {
    return obj.IsObject() ? obj.Find(key) : nullptr;
}

f32 FloatField(const JsonValue& obj, const char* key, f32 def) {
    const JsonValue* v = Field(obj, key);
    return (v && v->IsNumber()) ? v->AsFloat(def) : def;
}

std::string StringField(const JsonValue& obj, const char* key) {
    const JsonValue* v = Field(obj, key);
    return (v && v->IsString()) ? v->AsString() : std::string();
}

bool BoolField(const JsonValue& obj, const char* key, bool def) {
    const JsonValue* v = Field(obj, key);
    return (v && v->IsBool()) ? v->AsBool(def) : def;
}

Vec2 ReadVec2(const JsonValue& v, Vec2 def) {
    if (v.IsNumber()) return {v.AsFloat(def.x), def.y};
    if (v.IsArray()) {
        const JsonArray& a = v.Array();
        if (a.size() >= 2 && a[0].IsNumber() && a[1].IsNumber())
            return {a[0].AsFloat(def.x), a[1].AsFloat(def.y)};
        if (a.size() == 1 && a[0].IsNumber()) return {a[0].AsFloat(def.x), def.y};
    }
    return def;
}

Vec3 ReadVec3(const JsonValue& v, Vec3 def) {
    if (v.IsNumber()) return {v.AsFloat(def.x), def.y, def.z};
    if (v.IsArray()) {
        const JsonArray& a = v.Array();
        if (a.empty()) return def;
        f32 x = a[0].IsNumber() ? a[0].AsFloat(def.x) : def.x;
        f32 y = (a.size() > 1 && a[1].IsNumber()) ? a[1].AsFloat(def.y) : def.y;
        f32 z = (a.size() > 2 && a[2].IsNumber()) ? a[2].AsFloat(def.z) : def.z;
        return {x, y, z};
    }
    return def;
}

// "#rgb", "#rrggbb" и "#rrggbbaa" (также допускается префикс "0x"/"0X").
bool ParseHexColor(const std::string& text, Color* out) {
    std::string h;
    h.reserve(text.size());
    for (char c : text) {
        if (c != '#' && c != ' ' && c != '\t') h.push_back(c);
    }
    if (h.size() > 2 && (h[0] == '0') && (h[1] == 'x' || h[1] == 'X')) h = h.substr(2);
    auto nibble = [](char c, int* v) {
        if (c >= '0' && c <= '9') { *v = c - '0'; return true; }
        if (c >= 'a' && c <= 'f') { *v = c - 'a' + 10; return true; }
        if (c >= 'A' && c <= 'F') { *v = c - 'A' + 10; return true; }
        return false;
    };
    auto byteAt = [&](usize i, int* v) {
        int hi = 0, lo = 0;
        if (!nibble(h[i], &hi) || !nibble(h[i + 1], &lo)) return false;
        *v = hi * 16 + lo;
        return true;
    };
    int r = 0, g = 0, b = 0, a = 255;
    if (h.size() == 3 || h.size() == 4) {
        int rn = 0, gn = 0, bn = 0, an = 0;
        if (!nibble(h[0], &rn) || !nibble(h[1], &gn) || !nibble(h[2], &bn)) return false;
        if (h.size() == 4 && !nibble(h[3], &an)) return false;
        r = rn * 17; g = gn * 17; b = bn * 17; a = h.size() == 4 ? an * 17 : 255;
    } else if (h.size() == 6 || h.size() == 8) {
        if (!byteAt(0, &r) || !byteAt(2, &g) || !byteAt(4, &b)) return false;
        if (h.size() == 8 && !byteAt(6, &a)) return false;
    } else {
        return false;
    }
    *out = Color::FromBytes(static_cast<u8>(r), static_cast<u8>(g), static_cast<u8>(b),
                            static_cast<u8>(a));
    return true;
}

Color ReadColor(const JsonValue& v, const Color& def) {
    if (v.IsString()) {
        Color c;
        return ParseHexColor(v.AsString(), &c) ? c : def;
    }
    if (v.IsArray()) {
        const JsonArray& a = v.Array();
        if (a.empty()) return def;
        f32 r = a[0].IsNumber() ? a[0].AsFloat(def.r) : def.r;
        f32 g = (a.size() > 1 && a[1].IsNumber()) ? a[1].AsFloat(def.g) : def.g;
        f32 b = (a.size() > 2 && a[2].IsNumber()) ? a[2].AsFloat(def.b) : def.b;
        f32 al = (a.size() > 3 && a[3].IsNumber()) ? a[3].AsFloat(1.0f) : 1.0f;
        return {r, g, b, al};
    }
    if (v.IsNumber()) return Color{v.AsFloat(def.r)};
    return def;
}

// Easing хранится либо как "x1,y1,x2,y2", либо как {x,y} со скалярами или
// массивами, либо как простое число.
void ParseEaseString(const std::string& s, f32* x1, f32* y1, f32* x2, f32* y2);

// bodymovin хранит две контрольные точки одной кубической безье на двух ключах:
// *исходящая* касательная `o` на левом ключе и *входящая* касательная `i` на
// правом. Каждая имеет форму {"x": v|[v,v], "y": v|[v,v]} (массивы держат
// касательные для многомерных значений) либо строку "x1,y1,x2,y2".
// `out` получает только пару (x, y), которую держит этот узел.
void ParseEasing(const JsonValue& node, std::string* out) {
    if (!out) return;
    if (node.IsNumber()) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.5g,0", static_cast<double>(node.AsFloat()));
        *out = buf;
        return;
    }
    if (node.IsString()) {
        // Может быть "x1,y1" или полной "x1,y1,x2,y2"; берём первую пару.
        f32 x1, y1, x2, y2;
        ParseEaseString(node.AsString(), &x1, &y1, &x2, &y2);
        char buf[96];
        std::snprintf(buf, sizeof(buf), "%.5g,%.5g", static_cast<double>(x1),
                      static_cast<double>(y1));
        *out = buf;
        return;
    }
    if (!node.IsObject()) return;
    f32 x[2] = {0.0f, 0.0f};
    f32 y[2] = {0.0f, 0.0f};
    int nx = 0, ny = 0;
    const JsonValue* xv = node.Find("x");
    const JsonValue* yv = node.Find("y");
    if (xv && xv->IsArray()) {
        for (usize k = 0; k < xv->Size() && nx < 2; ++k)
            if ((*xv)[k].IsNumber()) x[nx++] = (*xv)[k].AsFloat();
    } else if (xv && xv->IsNumber()) {
        x[nx++] = xv->AsFloat();
    }
    if (yv && yv->IsArray()) {
        for (usize k = 0; k < yv->Size() && ny < 2; ++k)
            if ((*yv)[k].IsNumber()) y[ny++] = (*yv)[k].AsFloat();
    } else if (yv && yv->IsNumber()) {
        y[ny++] = yv->AsFloat();
    }
    if (nx == 0 && ny == 0) return;
    if (ny == 0) y[ny++] = 0.0f;
    char buf[96];
    std::snprintf(buf, sizeof(buf), "%.5g,%.5g", static_cast<double>(x[0]),
                  static_cast<double>(y[0]));
    *out = buf;
}

void ParseEaseString(const std::string& s, f32* x1, f32* y1, f32* x2, f32* y2) {
    // По умолчанию линейная рампа, чтобы не заданный easing никогда не искривлял сегмент.
    *x1 = 0.0f;
    *y1 = 0.0f;
    *x2 = 1.0f;
    *y2 = 1.0f;
    if (s.empty()) return;
    f32 vals[4] = {0, 0, 0, 0};
    int n = 0;
    const char* p = s.c_str();
    while (*p && n < 4) {
        while (*p == ' ' || *p == ',' || *p == ';' || *p == '\t') ++p;
        if (!*p) break;
        char* end = nullptr;
        double d = std::strtod(p, &end);
        if (end == p) break;
        vals[n++] = static_cast<f32>(d);
        p = end;
    }
    if (n >= 4) {
        *x1 = vals[0];
        *y1 = vals[1];
        *x2 = vals[2];
        *y2 = vals[3];
    } else if (n == 2) {
        *x1 = vals[0];
        *y1 = vals[1];
    } else if (n == 1) {
        *x1 = vals[0];
    }
}

int ParseInt(const JsonValue* v, int def) {
    if (!v) return def;
    if (v->IsNumber()) return v->AsInt(def);
    if (v->IsBool()) return v->AsBool() ? 1 : 0;
    return def;
}

// ---------------------------------------------------------------------------
// base64 (встроенные изображения в `data:image/png;base64,...`)
// ---------------------------------------------------------------------------
std::vector<u8> Base64Decode(const std::string& in) {
    static const i8 table[256] = {
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 62,
        -1, -1, -1, 63, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, -1, -1, -1, -2, -1, -1, -1, 0,
        1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22,
        23, 24, 25, -1, -1, -1, -1, -1, -1, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38,
        39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, -1, -1, -1, -1, -1};
    std::vector<u8> out;
    out.reserve(in.size() * 3 / 4 + 4);
    u32 acc = 0;
    int bits = 0;
    for (char c : in) {
        i8 v = table[static_cast<u8>(c)];
        if (v == -1) continue;
        if (v == -2) break;  // выравнивание
        acc = (acc << 6) | static_cast<u32>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<u8>((acc >> bits) & 0xFF));
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Служебные данные путей
// ---------------------------------------------------------------------------
LottiePathHandle MakeHandleFromFlat(const std::vector<Vec2>& pts) {
    LottiePathHandle h;
    if (pts.size() < 6 || pts.size() % 3 != 0) return h;
    usize groups = pts.size() / 3;
    bool closed = groups >= 3;
    if (closed) {
        const Vec2& last = pts[(groups - 1) * 3];
        if (std::fabs(last.x - pts[0].x) > 1e-4f || std::fabs(last.y - pts[0].y) > 1e-4f) closed = false;
    }
    usize n = closed ? groups - 1 : groups;
    if (n < 2) return LottiePathHandle{};
    h.v.resize(n);
    h.i.resize(n);
    h.o.resize(n);
    for (usize k = 0; k < n; ++k) {
        h.v[k] = pts[k * 3];
        h.i[k] = pts[k * 3 + 1];
        h.o[k] = pts[k * 3 + 2];
    }
    h.closed = closed;
    return h;
}

// У замороженного `LottieProperty` нет указателя на владельца, поэтому кубические
// рычаги вычисляемого свойства ищутся через анимацию, которая сейчас
// парсится/рендерится. Достаточно одной глобальной переменной: вычисление и
// отрисовка не реентерабельны, а рекурсия precomp подменяет указатель с сохранением/восстановлением.
const LottieImpl* g_activeImpl = nullptr;

const LottiePathHandle* LookupPath(const LottieProperty* p) {
    if (!p || !g_activeImpl) return nullptr;
    auto it = g_activeImpl->paths.find(p);
    return it == g_activeImpl->paths.end() ? nullptr : &it->second;
}

// шейп пути bodymovin: {"i":[[..]],"o":[[..]],"v":[[..]],"c":bool}
bool ParsePathShape(const JsonValue& node, LottiePathHandle* out) {
    if (!out || !node.IsObject()) return false;
    const JsonValue* vv = node.Find("v");
    if (!vv || !vv->IsArray()) return false;
    const JsonArray& vs = vv->Array();
    usize n = vs.size();
    if (n < 2) return false;
    std::vector<Vec2> v(n), in(n), outt(n);
    for (usize k = 0; k < n; ++k) {
        if (!(vs[k].IsArray() && vs[k].Size() >= 2 && vs[k][0].IsNumber())) return false;
        v[k] = {vs[k][0].AsFloat(), vs[k][1].AsFloat()};
    }
    auto readHandles = [&](const char* key, std::vector<Vec2>& dst) {
        const JsonValue* src = node.Find(key);
        if (!src || !src->IsArray()) return;
        const JsonArray& a = src->Array();
        for (usize k = 0; k < n && k < a.size(); ++k) {
            if (a[k].IsArray() && a[k].Size() >= 2)
                dst[k] = {a[k][0].AsFloat(), a[k][1].AsFloat()};
        }
    };
    readHandles("i", in);
    readHandles("o", outt);

    bool closed = false;
    if (const JsonValue* cv = node.Find("c")) {
        if (cv->IsBool()) closed = cv->AsBool();
        else if (cv->IsNumber()) closed = cv->AsFloat() != 0;
    }
    if (closed && n >= 3 && std::fabs(v[n - 1].x - v[0].x) < 1e-4f &&
        std::fabs(v[n - 1].y - v[0].y) < 1e-4f) {
        // Повторённая замыкающая вершина: её входная касательная переносится на вершину 0.
        if (LengthSq(in[n - 1]) > 1e-10f) in[0] = in[n - 1];
        v.pop_back();
        in.pop_back();
        outt.pop_back();
    }
    if (v.size() < 2) return false;
    out->v = std::move(v);
    out->i = std::move(in);
    out->o = std::move(outt);
    out->closed = closed;
    return true;
}

// Флэттенирует шейп пути в триплеты (вершина, in, out); замкнутые пути повторяют
// первую вершину один раз в конце.
// Пути с ключами оборачивают шейп в одноэлементный массив
// (`"s": [ { "i": ..., "v": ..., "c": true } ]`); статические пути — нет.
const JsonValue* UnwrapPathShape(const JsonValue& node) {
    if (!node.IsObject() && node.IsArray() && node.Size() > 0 && node[0].IsObject()) return &node[0];
    return &node;
}

bool FlatPointsFromPathShape(const JsonValue& node, std::vector<Vec2>* out) {
    LottiePathHandle ph;
    if (!ParsePathShape(*UnwrapPathShape(node), &ph)) return false;
    out->clear();
    out->reserve(ph.v.size() * 3 + 3);
    for (usize k = 0; k < ph.v.size(); ++k) {
        out->push_back(ph.v[k]);
        out->push_back(ph.i[k]);
        out->push_back(ph.o[k]);
    }
    if (ph.closed && !ph.v.empty()) {
        out->push_back(ph.v[0]);
        out->push_back(ph.i[0]);
        out->push_back(ph.o[0]);
    }
    return true;
}

// ---------------------------------------------------------------------------
// Разбор свойств
// ---------------------------------------------------------------------------
enum class PropKind { Scalar, Vec2, Vec3, Color, Path, Gradient };

void StoreValue(const JsonValue& s, PropKind kind, f32 scalar, LottieProperty::Key* key) {
    switch (kind) {
        case PropKind::Scalar:
            key->value = s.IsNumber() ? s.AsFloat(scalar) : scalar;
            break;
        case PropKind::Vec2: {
            Vec2 v = ReadVec2(s, Vec2{scalar, scalar});
            key->v2 = v;
            key->v3 = {v.x, v.y, 0};
            key->value = v.x;
            break;
        }
        case PropKind::Vec3: {
            Vec3 v = ReadVec3(s, Vec3{scalar, scalar, scalar});
            key->v3 = v;
            key->v2 = {v.x, v.y};
            key->value = v.x;
            break;
        }
        case PropKind::Color:
            key->col = ReadColor(s, Color{1, 1, 1, 1});
            break;
        case PropKind::Path:
            FlatPointsFromPathShape(s, &key->pts);
            break;
        case PropKind::Gradient:
            break;
    }
}

void ApplyStatic(const JsonValue& k, PropKind kind, f32 scalar, LottieProperty* out) {
    switch (kind) {
        case PropKind::Scalar:
            out->scalar = k.IsNumber() ? k.AsFloat(scalar) : scalar;
            break;
        case PropKind::Vec2: {
            Vec2 v = ReadVec2(k, Vec2{scalar, scalar});
            out->vec2 = v;
            out->vec3 = {v.x, v.y, 0};
            out->scalar = v.x;
            break;
        }
        case PropKind::Vec3: {
            Vec3 v = ReadVec3(k, Vec3{scalar, scalar, scalar});
            out->vec3 = v;
            out->vec2 = {v.x, v.y};
            out->scalar = v.x;
            break;
        }
        case PropKind::Color:
            out->color = ReadColor(k, Color{1, 1, 1, 1});
            break;
        case PropKind::Path:
            FlatPointsFromPathShape(k, &out->pathPoints);
            break;
        case PropKind::Gradient:
            break;
    }
}

LottieProperty ParseProperty(const JsonValue& node, PropKind kind, f32 scalar = 0.0f) {
    LottieProperty p;
    switch (kind) {
        case PropKind::Scalar: p.kind = LottieProperty::Kind::Scalar; break;
        case PropKind::Vec2: p.kind = LottieProperty::Kind::Vec2; break;
        case PropKind::Vec3: p.kind = LottieProperty::Kind::Vec3; break;
        case PropKind::Color: p.kind = LottieProperty::Kind::Color; break;
        case PropKind::Path: p.kind = LottieProperty::Kind::Path; break;
        case PropKind::Gradient: p.kind = LottieProperty::Kind::Gradient; break;
    }
    p.scalar = scalar;
    p.vec2 = {scalar, scalar};
    p.vec3 = {scalar, scalar, scalar};
    if (!node.IsObject()) {
        ApplyStatic(node, kind, scalar, &p);
        return p;
    }
    const JsonValue* a = Field(node, "a");
    const JsonValue* k = Field(node, "k");
    if (!k) return p;  // ничего пригодного
    bool animated = a && a->IsNumber() && a->AsFloat() != 0.0f;
    if (!animated || !k->IsArray()) {
        ApplyStatic(*k, kind, scalar, &p);
        return p;
    }
    p.animated = true;
    const JsonArray& keys = k->Array();
    for (usize ki = 0; ki < keys.size(); ++ki) {
        const JsonValue& kn = keys[ki];
        if (!kn.IsObject()) continue;
        LottieProperty::Key key;
        key.time = FloatField(kn, "t", 0.0f);
        bool hasStart = false;
        if (const JsonValue* s = Field(kn, "s")) {
            if (!s->IsNull()) {
                StoreValue(*s, kind, scalar, &key);
                hasStart = true;
            }
        }
        // Старые/альтернативные экспорты кладут конец сегмента в `e` на левом ключе.
        if (const JsonValue* e = Field(kn, "e")) {
            if (!e->IsNull()) StoreValue(*e, kind, scalar, &key);
        }
        if (!hasStart && (kind == PropKind::Scalar || kind == PropKind::Vec2 ||
                          kind == PropKind::Vec3)) {
            // Ключ без `s` продолжает предыдущее конечное значение.
            if (!p.keys.empty() && kind == PropKind::Scalar) key.value = p.keys.back().value;
            if (!p.keys.empty() && (kind == PropKind::Vec2 || kind == PropKind::Vec3)) {
                key.v2 = p.keys.back().v2;
                key.v3 = p.keys.back().v3;
            }
        }
        if (const JsonValue* h = Field(kn, "h")) {
            if (h->IsNumber()) key.hold = h->AsFloat() != 0;
            else if (h->IsBool()) key.hold = h->AsBool();
        }
        if (const JsonValue* i = Field(kn, "i")) ParseEasing(*i, &key.easingIn);
        if (const JsonValue* o = Field(kn, "o")) ParseEasing(*o, &key.easingOut);
        p.keys.push_back(std::move(key));
        LottieProperty::SpatialTangent st;
        if (const JsonValue* ti = Field(kn, "ti")) st.inTangent = ReadVec2(*ti, Vec2{});
        if (const JsonValue* to = Field(kn, "to")) st.outTangent = ReadVec2(*to, Vec2{});
        p.spatial.push_back(st);
    }
    if (p.keys.empty()) p.animated = false;
    return p;
}

// Масштабирует времена ключей из фреймов композиции в секунды.
void ScaleKeyTimes(LottieProperty& p, f32 frameRate) {
    if (!p.animated || frameRate <= 0) return;
    f32 inv = 1.0f / frameRate;
    for (LottieProperty::Key& k : p.keys) k.time *= inv;
}

// Трансформации AE по умолчанию: без смещения, единичный масштаб, полная непрозрачность.
LottieTransform DefaultLottieTransform() {
    LottieTransform t;
    t.scale.scalar = 100.0f;
    t.scale.vec2 = {100, 100};
    t.scale.vec3 = {100, 100, 100};
    t.opacity.scalar = 100.0f;
    t.opacity.vec2 = {100, 100};
    t.opacity.vec3 = {100, 100, 100};
    return t;
}

LottieTransform ParseTransform(const JsonValue& ks, f32 frameRate) {
    LottieTransform t = DefaultLottieTransform();
    if (!ks.IsObject()) return t;
    if (const JsonValue* v = Field(ks, "a")) {
        t.anchor = ParseProperty(*v, PropKind::Vec2);
        ScaleKeyTimes(t.anchor, frameRate);
    }
    if (const JsonValue* v = Field(ks, "p")) {
        t.position = ParseProperty(*v, PropKind::Vec2);
        ScaleKeyTimes(t.position, frameRate);
    }
    if (const JsonValue* v = Field(ks, "s")) {
        t.scale = ParseProperty(*v, PropKind::Vec2, 100.0f);
        ScaleKeyTimes(t.scale, frameRate);
    }
    if (const JsonValue* v = Field(ks, "r")) {
        t.rotation = ParseProperty(*v, PropKind::Scalar);
        ScaleKeyTimes(t.rotation, frameRate);
    }
    if (const JsonValue* v = Field(ks, "o")) {
        t.opacity = ParseProperty(*v, PropKind::Scalar, 100.0f);
        ScaleKeyTimes(t.opacity, frameRate);
    }
    if (const JsonValue* v = Field(ks, "sk")) {
        t.skew = ParseProperty(*v, PropKind::Scalar);
        ScaleKeyTimes(t.skew, frameRate);
    }
    if (const JsonValue* v = Field(ks, "sa")) {
        t.skewAxis = ParseProperty(*v, PropKind::Scalar);
        ScaleKeyTimes(t.skewAxis, frameRate);
    }
    return t;
}

// ---------------------------------------------------------------------------
// Разбор стопов градиента: устаревший и современный форматы
// ---------------------------------------------------------------------------
// Устаревший формат: {"p":n,"k":{"a":0,"k":[pos,r,g,b, pos,r,g,b, ...]}}
// Современный: {"p":n,"k":{"a":0,"k":[{"i":..,"o":..,"n":[r,g,b],"t":pos}, ...]}}
void ParseGradientStops(const JsonValue& g, LottieGradient* out) {
    const JsonValue* gk = Field(g, "k");
    const JsonValue* flat = gk ? Field(*gk, "k") : nullptr;
    if (!flat || !flat->IsArray()) return;
    const JsonArray& a = flat->Array();
    int count = ParseInt(Field(g, "p"), 0);
    bool modern = a.size() > 0 && a[0].IsObject();
    if (modern) {
        // Каждая запись сама может быть анимированной; используется только первый фрейм.
        usize n = count > 0 ? static_cast<usize>(count) : a.size();
        for (usize k = 0; k < n && k < a.size(); ++k) {
            const JsonValue& stop = a[k];
            if (!stop.IsObject()) continue;
            const JsonValue* col = Field(stop, "n") ? Field(stop, "n") : stop.Find("s");
            if (!col || !col->IsArray() || col->Size() < 3) continue;
            f32 off = FloatField(stop, "t", 0.0f);
            out->offsets.push_back(off);
            out->colors.push_back(Color{(*col)[0].AsFloat(1), (*col)[1].AsFloat(1),
                                        (*col)[2].AsFloat(1), 1});
            out->opacities.push_back(col->Size() > 3 ? Clampf((*col)[3].AsFloat(1), 0, 1) : 1.0f);
        }
        return;
    }
    // Плоский устаревший массив из `count` стопов, по 4 float на каждый.
    if (count <= 0) count = static_cast<int>(a.size() / 4);
    for (int k = 0; k < count; ++k) {
        usize base = static_cast<usize>(k) * 4;
        if (base + 3 >= a.size()) break;
        if (!a[base].IsNumber()) break;
        out->offsets.push_back(a[base].AsFloat());
        out->colors.push_back(Color{a[base + 1].AsFloat(1), a[base + 2].AsFloat(1),
                                    a[base + 3].AsFloat(1), 1});
        out->opacities.push_back(1.0f);
    }
}

// ---------------------------------------------------------------------------
// Разбор шейпов
// ---------------------------------------------------------------------------
LottieShape::Type MapShapeType(const std::string& ty) {
    if (ty == "gr") return LottieShape::Type::Group;
    if (ty == "rc") return LottieShape::Type::Rectangle;
    if (ty == "el") return LottieShape::Type::Ellipse;
    if (ty == "sh") return LottieShape::Type::Path;
    if (ty == "sr") return LottieShape::Type::PolyStar;
    if (ty == "fl") return LottieShape::Type::Fill;
    if (ty == "st") return LottieShape::Type::Stroke;
    if (ty == "gf") return LottieShape::Type::GradientFill;
    if (ty == "gs") return LottieShape::Type::GradientStroke;
    if (ty == "tr") return LottieShape::Type::Transform;
    if (ty == "mm") return LottieShape::Type::Merge;
    if (ty == "tm") return LottieShape::Type::Trim;
    if (ty == "rp") return LottieShape::Type::Repeater;
    if (ty == "rd") return LottieShape::Type::RoundedCorners;
    return LottieShape::Type::Unknown;
}

// Контекст времени парсинга. Черновые данные собираются по индексам, потому что
// объекты шейпов и слоёв ещё перемещаются (меняя адрес) в процессе сборки.
struct ParseCtx {
    LottieImpl* impl = nullptr;
    f32 frameRate = 60.0f;
    usize shapeCursor = 0;  // расходуется проходом регистрации после парсинга
    usize layerCursor = 0;

    int AddShapeSlot() {
        impl->shapeSlots.emplace_back();
        return static_cast<int>(impl->shapeSlots.size()) - 1;
    }
    LottieShapeScratch& Slot(int index) { return impl->shapeSlots[static_cast<usize>(index)]; }
    LottieLayerScratch& AddLayerSlot() {
        impl->layerSlots.emplace_back();
        return impl->layerSlots.back();
    }
};

// Проход регистрации. Порядок слотов `ParseShape`/`ParseLayer` совпадает с обходом
// финального дерева в прямом порядке, поэтому последовательное расходование
// черновых векторов связывает каждую запись с её финальным (стабильным) адресом.
void RegisterShapeTree(const std::vector<std::unique_ptr<LottieShape>>& shapes, ParseCtx& ctx) {
    for (const std::unique_ptr<LottieShape>& sp : shapes) {
        const LottieShape& s = *sp;
        if (ctx.shapeCursor < ctx.impl->shapeSlots.size()) {
            const LottieShapeScratch& sc = ctx.impl->shapeSlots[ctx.shapeCursor++];
            if (sc.hasPath) ctx.impl->paths[&s.pathData] = sc.path;
            if (sc.hasGradient) ctx.impl->gradients[&s] = sc.gradient;
            if (sc.hasOffset) ctx.impl->offsetPaths[&s] = sc.offset;
            if (sc.mergeMode != 0) ctx.impl->mergeModes[&s] = sc.mergeMode;
        }
        RegisterShapeTree(s.items, ctx);
    }
}

void RegisterShapeTree(const std::vector<LottieShape>& shapes, ParseCtx& ctx) {
    for (const LottieShape& s : shapes) {
        if (ctx.shapeCursor < ctx.impl->shapeSlots.size()) {
            const LottieShapeScratch& sc = ctx.impl->shapeSlots[ctx.shapeCursor++];
            if (sc.hasPath) ctx.impl->paths[&s.pathData] = sc.path;
            if (sc.hasGradient) ctx.impl->gradients[&s] = sc.gradient;
            if (sc.hasOffset) ctx.impl->offsetPaths[&s] = sc.offset;
            if (sc.mergeMode != 0) ctx.impl->mergeModes[&s] = sc.mergeMode;
        }
        RegisterShapeTree(s.items, ctx);
    }
}

void RegisterLayerScratch(const std::vector<LottieLayer>& layers, ParseCtx& ctx) {
    for (const LottieLayer& L : layers) {
        if (ctx.layerCursor < ctx.impl->layerSlots.size()) {
            const LottieLayerScratch& sc = ctx.impl->layerSlots[ctx.layerCursor++];
            if (sc.hasText) ctx.impl->text[&L] = sc.text;
            if (sc.hasTimeRemap) ctx.impl->timeRemap[&L] = sc.timeRemap;
            if (sc.blendMode != 0) ctx.impl->blendModes[&L] = sc.blendMode;
            for (usize m = 0; m < L.masks.size() && m < sc.maskPaths.size(); ++m)
                ctx.impl->paths[&L.masks[m].path] = sc.maskPaths[m];
        }
        RegisterShapeTree(L.shapes, ctx);
    }
}

// Разобранный шейп стоит сохранять, если у него известный тип, имя, дети или
// данные пути (статические *или* анимированные — анимированные пути хранят точки
// в `keys[].pts`, поэтому `pathPoints` у них пуст).
bool ShapeHasContent(const LottieShape& s) {
    return s.type != LottieShape::Type::Unknown || !s.name.empty() || !s.items.empty() ||
           s.pathData.animated || !s.pathData.pathPoints.empty();
}

LottieShape ParseShape(const JsonValue& node, f32 frameRate, ParseCtx* ctx, int depth) {
    LottieShape s;
    if (!node.IsObject() || depth > 12) return s;
    s.transform = DefaultLottieTransform();
    const std::string ty = StringField(node, "ty");
    s.type = MapShapeType(ty);
    s.name = StringField(node, "nm");
    s.hidden = BoolField(node, "hd", false);
    int slotIndex = ctx ? ctx->AddShapeSlot() : -1;

    auto prop = [&](const char* key, PropKind kind, f32 def) {
        LottieProperty p;
        // Засеваем значением по умолчанию AE, чтобы отсутствующий ключ (например,
        // штрих без прозрачности или трим без конца) не стал 0 / невидимым.
        p.scalar = def;
        p.vec2 = {def, def};
        p.vec3 = {def, def, def};
        const JsonValue* v = node.Find(key);
        if (v) {
            p = ParseProperty(*v, kind, def);
            ScaleKeyTimes(p, frameRate);
        }
        return p;
    };
    auto scratch = [&]() -> LottieShapeScratch* {
        return (ctx && slotIndex >= 0) ? &ctx->Slot(slotIndex) : nullptr;
    };

    switch (s.type) {
        case LottieShape::Type::Rectangle:
            s.position = prop("p", PropKind::Vec2, 0);
            s.size = prop("s", PropKind::Vec2, 0);
            s.roundness = prop("r", PropKind::Scalar, 0);
            break;
        case LottieShape::Type::Ellipse:
            s.position = prop("p", PropKind::Vec2, 0);
            s.size = prop("s", PropKind::Vec2, 0);
            break;
        case LottieShape::Type::Path: {
            s.pathData = prop("ks", PropKind::Path, 0);
            // Статическая форма: {"a":0,"k":{...}}. Анимированные пути хранят
            // массивы точек в `keys[].pts`, интерполирует их EvaluatePath().
            if (ctx && !s.pathData.animated) {
                if (const JsonValue* ks = node.Find("ks")) {
                    if (const JsonValue* kk = Field(*ks, "k")) {
                        LottiePathHandle ph;
                        if (ParsePathShape(*UnwrapPathShape(*kk), &ph)) {
                            LottieShapeScratch& slot = ctx->Slot(slotIndex);
                            slot.hasPath = true;
                            slot.path = std::move(ph);
                        }
                    }
                }
            }
            break;
        }
        case LottieShape::Type::PolyStar:
            s.position = prop("p", PropKind::Vec2, 0);
            s.points = prop("pt", PropKind::Scalar, 5);
            s.outerRadius = prop("or", PropKind::Scalar, 0);
            s.innerRadius = prop("ir", PropKind::Scalar, 0);
            s.rotation = prop("r", PropKind::Scalar, 0);
            s.starType = prop("sy", PropKind::Scalar, 1);
            break;
        case LottieShape::Type::Fill:
        case LottieShape::Type::Stroke:
        case LottieShape::Type::GradientFill:
        case LottieShape::Type::GradientStroke: {
            s.colour = prop("c", PropKind::Color, 1);
            s.fillOpacity = prop("o", PropKind::Scalar, 100);
            s.strokeOpacity = s.fillOpacity;
            s.opacity = s.fillOpacity;
            s.strokeWidth = prop("w", PropKind::Scalar, 0);
            s.fillRule = static_cast<int>(FloatField(node, "r", 1));
            s.miterLimit = FloatField(node, "ml", 4);
            switch (static_cast<int>(FloatField(node, "lc", 1))) {
                case 2: s.cap = LineCap::Round; break;
                case 3: s.cap = LineCap::Square; break;
                default: s.cap = LineCap::Butt; break;
            }
            switch (static_cast<int>(FloatField(node, "lj", 1))) {
                case 2: s.join = LineJoin::Round; break;
                case 3: s.join = LineJoin::Bevel; break;
                default: s.join = LineJoin::Miter; break;
            }
            if (const JsonValue* d = node.Find("d")) {
                if (d->IsArray() && d->Size() > 0 && (*d)[0].IsObject()) {
                    const JsonValue& d0 = (*d)[0];
                    if (const JsonValue* n = Field(d0, "n")) {
                        s.dashLength = ParseProperty(*n, PropKind::Scalar, 0);
                        ScaleKeyTimes(s.dashLength, frameRate);
                    }
                    if (const JsonValue* v = Field(d0, "v")) {
                        s.dashGap = ParseProperty(*v, PropKind::Scalar, 0);
                        ScaleKeyTimes(s.dashGap, frameRate);
                    }
                }
            }
            s.gradientType = static_cast<int>(FloatField(node, "t", 1));
            if (s.type == LottieShape::Type::GradientFill ||
                s.type == LottieShape::Type::GradientStroke) {
                s.gradientStart = prop("s", PropKind::Vec2, 0);
                s.gradientEnd = prop("e", PropKind::Vec2, 0);
                if (ctx) {
                    if (const JsonValue* g = node.Find("g")) {
                        if (g->IsObject()) {
                            LottieGradient grad;
                            ParseGradientStops(*g, &grad);
                            if (grad.valid() && scratch()) {
                                scratch()->hasGradient = true;
                                scratch()->gradient = std::move(grad);
                            }
                        }
                    }
                }
            }
            break;
        }
        case LottieShape::Type::Transform:
            s.transform = ParseTransform(node, frameRate);
            break;
        case LottieShape::Type::Trim:
            s.start = prop("s", PropKind::Scalar, 0);
            s.end = prop("e", PropKind::Scalar, 100);
            s.offset = prop("o", PropKind::Scalar, 0);
            s.trimEnabled = true;
            if (scratch()) scratch()->mergeMode = ParseInt(node.Find("m"), 1);
            break;
        case LottieShape::Type::Repeater:
            s.copies = prop("c", PropKind::Scalar, 1);
            if (const JsonValue* tr = node.Find("tr")) s.transform = ParseTransform(*tr, frameRate);
            break;
        case LottieShape::Type::RoundedCorners:
            s.roundness = prop("r", PropKind::Scalar, 0);
            break;
        case LottieShape::Type::Merge:
            if (scratch()) scratch()->mergeMode = ParseInt(node.Find("mm"), 1);
            break;
        default:
            break;
    }

    // Дети группы (`it`), одним из которых обычно является трансформация `tr`.
    if (const JsonValue* it = node.Find("it"); it && it->IsArray()) {
        for (usize k = 0; k < it->Size(); ++k) {
            LottieShape child = ParseShape((*it)[k], frameRate, ctx, depth + 1);
            if (child.type == LottieShape::Type::Transform) {
                s.transform = child.transform;
                continue;
            }
            if (ShapeHasContent(child))
                s.items.push_back(std::make_unique<LottieShape>(std::move(child)));
        }
    }
    // Offset path (`op`) не имеет слота в замороженном enum Type: записываем его
    // и позволяем рендереру применить его к соседней геометрии.
    if (ty == "op" && scratch()) {
        if (const JsonValue* a = node.Find("a")) {
            LottieProperty p = ParseProperty(*a, PropKind::Scalar, 0);
            ScaleKeyTimes(p, frameRate);
            scratch()->hasOffset = true;
            scratch()->offset = std::move(p);
        }
    }
    return s;
}

// ---------------------------------------------------------------------------
// Разбор слоёв
// ---------------------------------------------------------------------------
LottieLayer ParseLayer(const JsonValue& node, f32 frameRate, ParseCtx* ctx, int depth) {
    LottieLayer L;
    if (!node.IsObject() || depth > 8) return L;
    LottieLayerScratch* sc = ctx ? &ctx->AddLayerSlot() : nullptr;
    L.index = static_cast<int>(FloatField(node, "ind", 0));
    L.parent = static_cast<int>(FloatField(node, "parent", -1));
    L.name = StringField(node, "nm");
    switch (static_cast<int>(FloatField(node, "ty", 4))) {
        case 0: L.type = LottieLayerType::Precomp; break;
        case 1: L.type = LottieLayerType::Solid; break;
        case 2: L.type = LottieLayerType::Image; break;
        case 3: L.type = LottieLayerType::Null; break;
        case 4: L.type = LottieLayerType::Shape; break;
        case 5: L.type = LottieLayerType::Text; break;
        case 6: L.type = LottieLayerType::Audio; break;
        default: L.type = LottieLayerType::Unknown; break;
    }
    L.startTime = FloatField(node, "st", 0);
    L.inTime = FloatField(node, "ip", 0);
    L.outTime = FloatField(node, "op", 0);
    f32 sr = FloatField(node, "sr", 1.0f);
    L.timeStretch = (std::fabs(sr) > kEpsilon) ? sr : 1.0f;
    L.hidden = BoolField(node, "hd", false);
    L.is3D = BoolField(node, "ddd", false);
    L.width = FloatField(node, "sw", 0);
    L.height = FloatField(node, "sh", 0);
    if (const JsonValue* sc = node.Find("sc")) L.solidColor = ReadColor(*sc, Color::White);
    if (const JsonValue* ref = node.Find("refId")) {
        if (ref->IsString()) L.imageName = ref->AsString();
        else if (ref->IsNumber()) L.imageName = std::to_string(ref->AsInt());
    }
    L.transform = DefaultLottieTransform();
    if (const JsonValue* ks = node.Find("ks")) L.transform = ParseTransform(*ks, frameRate);
    if (const JsonValue* shapes = node.Find("shapes"); shapes && shapes->IsArray()) {
        for (usize k = 0; k < shapes->Size(); ++k) {
            LottieShape sh = ParseShape((*shapes)[k], frameRate, ctx, depth + 1);
            if (ShapeHasContent(sh)) L.shapes.push_back(std::move(sh));
        }
    }
    if (const JsonValue* masks = node.Find("masksProperties"); masks && masks->IsArray()) {
        for (usize k = 0; k < masks->Size(); ++k) {
            const JsonValue& mn = (*masks)[k];
            if (!mn.IsObject()) continue;
            LottieMask mask;
            if (const JsonValue* pt = mn.Find("pt")) {
                mask.path = ParseProperty(*pt, PropKind::Path);
                ScaleKeyTimes(mask.path, frameRate);
                if (sc) {
                    if (const JsonValue* kk = Field(*pt, "k")) {
                        LottiePathHandle ph;
                        if (ParsePathShape(*UnwrapPathShape(*kk), &ph))
                            sc->maskPaths.push_back(std::move(ph));
                    }
                }
            }
            const std::string mode = StringField(mn, "mode");
            mask.mode = mode.empty() ? 'a' : static_cast<int>(static_cast<unsigned char>(mode[0]));
            mask.inverted = BoolField(mn, "inv", false);
            mask.opacity = FloatField(mn, "o", 100);
            L.masks.push_back(std::move(mask));
        }
        L.hasMask = !L.masks.empty();
    }
    // Track matte: этот слой — мат для слоя под ним.
    L.matteLayer = static_cast<int>(FloatField(node, "tp", -1));
    L.matteMode = ParseInt(node.Find("td"), 0);
    if (sc) sc->blendMode = ParseInt(node.Find("bm"), 0);
    if (const JsonValue* tr = node.Find("tm")) {
        if (sc) {
            LottieProperty p = ParseProperty(*tr, PropKind::Scalar);
            ScaleKeyTimes(p, frameRate);
            sc->hasTimeRemap = true;
            sc->timeRemap = std::move(p);
        }
    }
    // Текст.
    if (const JsonValue* t = node.Find("t"); t && t->IsObject()) {
        LottieTextInfo info;
        if (const JsonValue* d = Field(*t, "d"); d && d->IsArray() && d->Size() > 0) {
            const JsonValue& doc = (*d)[0];
            if (const JsonValue* s = Field(doc, "s"); s && s->IsObject()) {
                info.text = StringField(*s, "t");
                info.fontFamily = StringField(*s, "f");
                info.fontSize = FloatField(*s, "s", 24);
                if (const JsonValue* fc = s->Find("fc")) info.fill = ReadColor(*fc, Color::White);
                if (const JsonValue* sc = s->Find("sc")) info.stroke = ReadColor(*sc, Color::Black);
                info.strokeWidth = FloatField(*s, "sw", 0);
                info.justification = static_cast<int>(FloatField(*s, "j", 0));
                info.tracking = FloatField(*s, "tr", 0);
                info.lineHeight = FloatField(*s, "lh", 0);
                info.strokeOverFill = BoolField(*s, "of", false);
            }
        }
        if (const JsonValue* a = Field(*t, "a"); a && a->IsArray()) {
            for (usize k = 0; k < a->Size(); ++k) {
                const JsonValue& an = (*a)[k];
                if (!an.IsObject()) continue;
                LottieTextAnimator anim;
                if (const JsonValue* r = an.Find("r")) {
                    if (const JsonValue* rs = Field(*r, "s")) {
                        LottieProperty sp = ParseProperty(*rs, PropKind::Scalar, 0);
                        ScaleKeyTimes(sp, frameRate);
                        anim.start = sp.animated && !sp.keys.empty() ? sp.keys.front().value
                                                                      : sp.scalar;
                    }
                    if (const JsonValue* re = Field(*r, "e")) {
                        LottieProperty ep = ParseProperty(*re, PropKind::Scalar, 1);
                        ScaleKeyTimes(ep, frameRate);
                        anim.end =
                            ep.animated && !ep.keys.empty() ? ep.keys.front().value : ep.scalar;
                    }
                }
                if (anim.end <= anim.start) anim.end = anim.start + 1.0f;
                if (const JsonValue* trk = an.Find("t")) {
                    anim.hasTracking = true;
                    anim.tracking = ParseProperty(*trk, PropKind::Scalar, 0);
                    ScaleKeyTimes(anim.tracking, frameRate);
                }
                if (const JsonValue* fc = an.Find("fc")) {
                    anim.hasFill = true;
                    anim.fillColor = ParseProperty(*fc, PropKind::Color);
                    ScaleKeyTimes(anim.fillColor, frameRate);
                }
                if (const JsonValue* op = an.Find("o")) {
                    anim.hasOpacity = true;
                    anim.opacity = ParseProperty(*op, PropKind::Scalar, 100);
                    ScaleKeyTimes(anim.opacity, frameRate);
                }
                info.animators.push_back(std::move(anim));
            }
        }
        if (sc) {
            sc->hasText = true;
            sc->text = info;
        }
        L.text = info.text;
        L.fontFamily = info.fontFamily;
        L.fontSize = info.fontSize;
        L.textColor = info.fill;
        L.textTracking = info.tracking;
        L.textLineHeight = info.lineHeight;
        L.justification = info.justification;
        L.strokeOverFill = info.strokeOverFill;
        L.textStrokeColor = info.stroke;
        L.textStrokeWidth = info.strokeWidth;
        switch (info.justification) {
            case 1: L.textAlign = TextAlign::Right; break;
            case 2: L.textAlign = TextAlign::Center; break;
            default: L.textAlign = TextAlign::Left; break;
        }
    }
    return L;
}

// ---------------------------------------------------------------------------
// Разрешение easing / сегментов при вычислении свойств
// ---------------------------------------------------------------------------
// Суммарный easing для сегмента между `a` и `b`. Каждый ключ держит по одной
// контрольной точке (`a.o` -> первая, `b.i` -> вторая); отсутствие пары означает
// линейный сегмент.
f32 EaseU(const LottieProperty::Key& a, const LottieProperty::Key& b, f32 u) {
    if (u <= 0) return 0;
    if (u >= 1) return 1;
    if (a.easingOut.empty() && b.easingIn.empty()) return u;
    f32 x1, y1, x2, y2;
    ParseEaseString(a.easingOut, &x1, &y1, &x2, &y2);
    f32 ix1, iy1, ix2, iy2;
    ParseEaseString(b.easingIn, &ix1, &iy1, &ix2, &iy2);
    // ParseEaseString по умолчанию линейна; входящую пару берём из `b`.
    x2 = b.easingIn.empty() ? 1.0f : ix1;
    y2 = b.easingIn.empty() ? 1.0f : iy1;
    // bezier(0,0,1,1) — тождественная рампа; ни одна из четырёх точек не попадает
    // в быстрый путь вида (x1==y1 && x2==y2), поэтому спускаемся в решатель.
    return CubicBezierEase(x1, y1, x2, y2, u);
}

bool FindSegment(const std::vector<LottieProperty::Key>& keys, f32 time, usize* lo, f32* u) {
    if (keys.empty()) return false;
    if (time <= keys.front().time) {
        *lo = 0;
        *u = 0.0f;
        return true;
    }
    if (time >= keys.back().time) {
        *lo = keys.size() - 1;
        *u = 0.0f;
        return true;
    }
    for (usize i = 0; i + 1 < keys.size(); ++i) {
        if (time >= keys[i].time && time <= keys[i + 1].time) {
            *lo = i;
            f32 span = keys[i + 1].time - keys[i].time;
            f32 raw = span > kEpsilon ? (time - keys[i].time) / span : 0.0f;
            *u = keys[i].hold ? 0.0f : EaseU(keys[i], keys[i + 1], raw);
            return true;
        }
    }
    *lo = keys.size() - 1;
    *u = 0.0f;
    return true;
}

Vec2 CubicAt(const Vec2& p0, const Vec2& c1, const Vec2& c2, const Vec2& p1, f32 t) {
    f32 u = 1.0f - t;
    return p0 * (u * u * u) + c1 * (3.0f * u * u * t) + c2 * (3.0f * u * t * t) + p1 * (t * t * t);
}

}  // namespace

// ===========================================================================
// Вычисление LottieProperty
// ===========================================================================
f32 LottieProperty::EvaluateScalar(f32 time) const {
    if (!animated || keys.empty()) return scalar;
    usize i = 0;
    f32 u = 0;
    FindSegment(keys, time, &i, &u);
    if (i + 1 >= keys.size() || u <= 0.0f) return keys[i].value;
    return keys[i].value + (keys[i + 1].value - keys[i].value) * u;
}

Vec2 LottieProperty::EvaluateVec2(f32 time) const {
    if (!animated || keys.empty()) return vec2;
    usize i = 0;
    f32 u = 0;
    FindSegment(keys, time, &i, &u);
    const Vec2 a = keys[i].v2;
    if (i + 1 >= keys.size() || u <= 0.0f || keys[i].hold) return a;
    const Vec2 b = keys[i + 1].v2;
    // Криволинейная траектория движения (пространственные безье-касательные `ti`/`to`).
    if (i + 1 < spatial.size()) {
        const Vec2& outT = spatial[i].outTangent;
        const Vec2& inT = spatial[i + 1].inTangent;
        if (LengthSq(outT) > 1e-10f || LengthSq(inT) > 1e-10f)
            return CubicAt(a, a + outT, b + inT, b, u);
    }
    return a + (b - a) * u;
}

Color LottieProperty::EvaluateColor(f32 time) const {
    if (!animated || keys.empty()) return color;
    usize i = 0;
    f32 u = 0;
    FindSegment(keys, time, &i, &u);
    if (i + 1 >= keys.size() || u <= 0.0f || keys[i].hold) return keys[i].col;
    return Lerp(keys[i].col, keys[i + 1].col, u);
}

std::vector<Vec2> LottieProperty::EvaluatePath(f32 time) const {
    if (!animated || keys.empty()) return pathPoints;
    usize i = 0;
    f32 u = 0;
    FindSegment(keys, time, &i, &u);
    const std::vector<Vec2>& a = keys[i].pts;
    if (i + 1 >= keys.size() || u <= 0.0f || keys[i].hold) return a;
    const std::vector<Vec2>& b = keys[i + 1].pts;
    // Несовпадение числа вершин: откат к форме `s`.
    if (a.size() != b.size() || a.size() < 6) return a;
    std::vector<Vec2> out(a.size());
    for (usize k = 0; k < a.size(); ++k) out[k] = a[k] + (b[k] - a[k]) * u;
    return out;
}

// ===========================================================================
// LottieTransform
// ===========================================================================
Mat4 LottieTransform::Result::Matrix() const {
    // Порядок Lottie/AE: translate(position) * rotate * skew * scale *
    // translate(-anchor). `Mat4` компонуется в стиле column-vector (`A * B`
    // применяет B к точке первой), поэтому трансляция anchor — самый внутренний
    // множитель, а position — самый внешний.
    Mat4 m = Mat4::Translate(Vec3{-anchor.x, -anchor.y, 0});
    m = Mat4::Scale(Vec3{scale.x / 100.0f, scale.y / 100.0f, 1}) * m;
    if (std::fabs(skew) > kEpsilon) {
        // Сдвиг вдоль `skewAxis`: R(axis) * ShearX(tan(skew)) * R(-axis).
        Mat4 shear;
        shear.at(1, 0) = std::tan(Radians(skew));
        m = Mat4::RotateZ(Radians(skewAxis)) * shear * Mat4::RotateZ(Radians(-skewAxis)) * m;
    }
    m = Mat4::RotateZ(Radians(rotation)) * m;
    m = Mat4::Translate(Vec3{position.x, position.y, 0}) * m;
    return m;
}

LottieTransform::Result LottieTransform::Evaluate(f32 time) const {
    Result r;
    r.anchor = anchor.EvaluateVec2(time);
    r.position = position.EvaluateVec2(time);
    r.scale = scale.EvaluateVec2(time);
    r.rotation = rotation.EvaluateScalar(time);
    r.opacity = opacity.EvaluateScalar(time);
    r.skew = skew.EvaluateScalar(time);
    r.skewAxis = skewAxis.EvaluateScalar(time);
    return r;
}

Mat4 LottieTransform::Matrix(f32 time) const { return Evaluate(time).Matrix(); }

// ===========================================================================
// LottieAnimation: конструирование / разбор
// ===========================================================================
LottieAnimation::LottieAnimation() : impl_(std::make_unique<Impl>()) {}
LottieAnimation::~LottieAnimation() = default;
LottieAnimation::LottieAnimation(LottieAnimation&&) noexcept = default;
LottieAnimation& LottieAnimation::operator=(LottieAnimation&&) noexcept = default;

void LottieAnimation::Destroy() {
    impl_ = std::make_unique<Impl>();
    raw_ = JsonValue{};
    valid_ = false;
    name_.clear();
    width_ = height_ = 0;
    frameRate_ = 60;
    duration_ = 0;
    totalFrames_ = 0;
    layers_.clear();
    assets_.clear();
    currentFrame_ = 0;
    speed_ = 1.0f;
    segStart_ = 0;
    segEnd_ = -1;
    playing_ = false;
    loop_ = true;
    finished_ = false;
    loopCount_ = 0;
    stats_ = RenderStats{};
}

bool LottieAnimation::LoadFromJson(const std::string& json, std::string* error) {
    std::string err;
    JsonValue root = JsonValue::Parse(json, &err);
    if (root.IsNull() || (!err.empty() && !root.IsObject())) {
        if (error) *error = err.empty() ? std::string("empty lottie document") : err;
        ENG_LOGE("lottie", "LoadFromJson failed: %s", err.c_str());
        return false;
    }
    return Parse(root, error);
}

bool LottieAnimation::LoadFromFile(const std::string& path, std::string* error) {
    std::string text = ReadTextFile(path);
    if (text.empty()) {
        if (error) *error = "cannot read " + path;
        ENG_LOGE("lottie", "LoadFromFile: cannot read '%s'", path.c_str());
        return false;
    }
    return LoadFromJson(text, error);
}

bool LottieAnimation::Parse(const JsonValue& root, std::string* error) {
    Destroy();
    if (!root.IsObject()) {
        if (error) *error = "root is not an object";
        return false;
    }
    raw_ = root;
    name_ = root.GetString("nm");
    frameRate_ = root.GetFloat("fr", 60.0f);
    if (!(frameRate_ > 0.01f)) frameRate_ = 60.0f;
    width_ = root.GetInt("w", 0);
    height_ = root.GetInt("h", 0);
    f32 ip = root.GetFloat("ip", 0);
    f32 op = root.GetFloat("op", 0);
    if (op <= ip) op = ip + 1.0f;
    totalFrames_ = static_cast<int>(std::ceil(op - ip));
    if (totalFrames_ <= 0) totalFrames_ = 1;
    duration_ = static_cast<f32>(totalFrames_) / frameRate_;
    currentFrame_ = ip;
    segStart_ = 0;
    segEnd_ = -1;

    ParseCtx pctx;
    pctx.impl = impl_.get();
    pctx.frameRate = frameRate_;

    // ---- ассеты ---------------------------------------------------------
    if (const JsonValue* assets = root.Find("assets"); assets && assets->IsArray()) {
        for (usize k = 0; k < assets->Size(); ++k) {
            const JsonValue& an = (*assets)[k];
            if (!an.IsObject()) continue;
            LottieAsset a;
            const JsonValue* idv = an.Find("id");
            if (idv && idv->IsNumber()) {
                a.id = idv->AsInt(static_cast<int>(k));
            } else if (idv && idv->IsString()) {
                const std::string& sid = idv->AsString();
                bool numeric = !sid.empty();
                for (char c : sid) {
                    if (!std::isdigit(static_cast<unsigned char>(c)) && c != '-') numeric = false;
                }
                a.id = numeric ? std::atoi(sid.c_str()) : static_cast<int>(k);
            } else {
                a.id = static_cast<int>(k);
            }
            a.name = StringField(an, "nm");
            a.width = static_cast<int>(FloatField(an, "w", 0));
            a.height = static_cast<int>(FloatField(an, "h", 0));
            std::string p = StringField(an, "p");
            const std::string u = StringField(an, "u");
            if (const JsonValue* layers = an.Find("layers"); layers && layers->IsArray()) {
                a.isPrecomp = true;
                for (usize li = 0; li < layers->Size(); ++li)
                    a.layers.push_back(ParseLayer((*layers)[li], frameRate_, &pctx, 0));
            }
            if (!p.empty()) a.imagePath = (p.rfind("data:", 0) == 0) ? p : (u + p);
            if (a.imagePath.rfind("data:", 0) == 0) {
                usize comma = a.imagePath.find(',');
                if (comma != std::string::npos && a.imagePath.find("base64") != std::string::npos)
                    a.imageData = Base64Decode(a.imagePath.substr(comma + 1));
            }
            if (ParseInt(an.Find("e"), 0) != 0)
                ENG_LOGW("lottie", "asset '%s': expression image path is not evaluated",
                         a.name.c_str());
            assets_.push_back(std::move(a));
        }
    }
    for (LottieAsset& a : assets_) impl_->assetsById[a.id] = &a;

    // ---- слои ---------------------------------------------------------
    if (const JsonValue* layers = root.Find("layers"); layers && layers->IsArray()) {
        for (usize k = 0; k < layers->Size(); ++k)
            layers_.push_back(ParseLayer((*layers)[k], frameRate_, &pctx, 0));
    }

    // ---- разрешение ссылок ------------------------------------------
    auto resolveRef = [&](LottieLayer& L) {
        if (L.imageName.empty()) return;
        for (usize k = 0; k < assets_.size(); ++k) {
            if (!assets_[k].name.empty() && assets_[k].name == L.imageName) {
                if (L.type == LottieLayerType::Precomp) L.precompIndex = static_cast<int>(k);
                else L.imageAssetIndex = static_cast<int>(k);
                return;
            }
        }
        for (usize k = 0; k < assets_.size(); ++k) {
            if (std::to_string(assets_[k].id) == L.imageName) {
                if (L.type == LottieLayerType::Precomp) L.precompIndex = static_cast<int>(k);
                else L.imageAssetIndex = static_cast<int>(k);
                return;
            }
        }
    };
    for (LottieLayer& L : layers_) resolveRef(L);
    for (LottieAsset& a : assets_)
        for (LottieLayer& L : a.layers) resolveRef(L);

    // ---- track mattes ---------------------------------------------------
    // `tt` лежит на слое-мате (который стоит прямо над своей целью), а `td` его
    // помечает; учитываются оба варианта.
    auto wireMatte = [&](std::vector<LottieLayer>& list) {
        for (usize k = 0; k < list.size(); ++k) {
            int mode = list[k].matteMode;
            if (mode != 0 && k + 1 < list.size()) {
                list[k].hidden = true;  // источники матов обычно не рисуются
                list[k + 1].matteLayer = list[k].index;
                list[k + 1].matteMode = mode;
            }
        }
    };
    wireMatte(layers_);
    for (LottieAsset& a : assets_) wireMatte(a.layers);
    // Явный `tp` (указатель на родителя-мат) переопределяет неявную привязку.
    for (usize k = 0; k < layers_.size(); ++k) {
        if (layers_[k].matteLayer >= 0 && layers_[k].matteMode == 0) layers_[k].matteMode = 1;
    }
    for (LottieAsset& a : assets_) {
        for (LottieLayer& L : a.layers) {
            if (L.matteLayer >= 0 && L.matteMode == 0) L.matteMode = 1;
        }
    }

    // ---- боковые таблицы ----------------------------------------------------
    g_activeImpl = impl_.get();
    // Слои ассетов разбираются первыми, поэтому и регистрируются первыми —
    // в соответствии с порядком слотов, сложившимся при разборе.
    for (LottieAsset& a : assets_) RegisterLayerScratch(a.layers, pctx);
    RegisterLayerScratch(layers_, pctx);
    impl_->shapeSlots.clear();
    impl_->layerSlots.clear();

    valid_ = !layers_.empty();
    if (!valid_ && error) *error = "composition has no layers";
    playing_ = false;
    finished_ = false;
    loopCount_ = 0;
    stats_ = RenderStats{};
    if (!valid_) ENG_LOGW("lottie", "composition '%s' has no layers", name_.c_str());
    return valid_;
}

usize LottieAnimation::MemoryUsage() const {
    usize total = sizeof(LottieAnimation) + name_.size();
    total += raw_.Dump(-1).size();
    total += layers_.size() * sizeof(LottieLayer);
    for (const LottieLayer& L : layers_) {
        total += L.name.size() + L.text.size() + L.masks.size() * sizeof(LottieMask);
        total += L.shapes.size() * sizeof(LottieShape);
        for (const LottieShape& s : L.shapes) {
            total += s.name.size();
            total += s.items.size() * sizeof(LottieShape);
            total += s.pathData.pathPoints.size() * sizeof(Vec2);
        }
    }
    for (const LottieAsset& a : assets_) {
        total += a.imageData.size() + a.imagePath.size() + a.name.size();
        total += a.layers.size() * sizeof(LottieLayer);
    }
    return total;
}

// ===========================================================================
// Плеер
// ===========================================================================
namespace {

// Конец активного диапазона воспроизведения во фреймах композиции. Плеер
// фиксируется на явном сегменте, если он задан, иначе на длительности композиции.
f32 EffectiveEnd(f32 segEnd, bool hasSegment, int totalFrames) {
    if (hasSegment) return segEnd;
    if (segEnd >= 0.0f) return segEnd;
    return totalFrames > 0 ? static_cast<f32>(totalFrames) : 1.0f;
}

}  // namespace

void LottieAnimation::Play() {
    playing_ = true;
    if (!loop_ && currentFrame_ >= EffectiveEnd(segEnd_, impl_ && impl_->hasSegment, totalFrames_)) {
        currentFrame_ = segStart_;
        finished_ = false;
    }
}

void LottieAnimation::Pause() { playing_ = false; }

void LottieAnimation::Stop() {
    playing_ = false;
    finished_ = false;
    currentFrame_ = segStart_;
}

void LottieAnimation::SetFrame(f32 frame) {
    f32 hi = static_cast<f32>(totalFrames_);
    currentFrame_ = Clampf(frame, 0.0f, hi > 0 ? hi : 0.0f);
    finished_ = false;
}

void LottieAnimation::SetTime(f32 seconds) { SetFrame(seconds * frameRate_); }

void LottieAnimation::SetSegment(f32 startFrame, f32 endFrame) {
    segStart_ = startFrame;
    segEnd_ = endFrame;
    if (segEnd_ < segStart_) std::swap(segStart_, segEnd_);
    impl_->hasSegment = true;
    currentFrame_ = Clampf(currentFrame_, segStart_, segEnd_);
}

f32 LottieAnimation::Progress() const {
    f32 s = segStart_;
    f32 e = EffectiveEnd(segEnd_, impl_ && impl_->hasSegment, totalFrames_);
    f32 span = e - s;
    if (span <= kEpsilon) return 1.0f;
    return Clampf((currentFrame_ - s) / span, 0.0f, 1.0f);
}

void LottieAnimation::Advance(f32 dt) {
    if (!playing_ || dt == 0.0f || !IsFinite(dt)) return;
    f32 end = EffectiveEnd(segEnd_, impl_ && impl_->hasSegment, totalFrames_);
    f32 span = end - segStart_;
    if (span <= kEpsilon) {
        currentFrame_ = segStart_;
        finished_ = true;
        return;
    }
    f32 delta = Clampf(dt * frameRate_ * speed_, -kMaxAdvance, kMaxAdvance);
    f32 f = currentFrame_ + delta;
    if (loop_) {
        if (f >= end) {
            f32 over = f - segStart_;
            loopCount_ += static_cast<int>(over / span);
            f = segStart_ + std::fmod(over, span);
        } else if (f < segStart_) {
            f32 under = f - segStart_;
            f = segStart_ + std::fmod(under, span);
            if (f < segStart_) f += span;
        }
        finished_ = false;
    } else if (f >= end) {
        f = end;
        finished_ = true;
    } else if (f <= segStart_) {
        f = segStart_;
        finished_ = speed_ < 0.0f;
    } else {
        finished_ = false;
    }
    currentFrame_ = f;
}

// ===========================================================================
// Валидация
// ===========================================================================
LottieAnimation::ValidationResult LottieAnimation::Validate() const {
    ValidationResult res;
    std::unordered_set<int> assetIds;
    for (usize k = 0; k < assets_.size(); ++k) {
        const LottieAsset& a = assets_[k];
        if (a.id < 0) {
            res.errors.push_back("asset " + std::to_string(k) + " has a negative id");
        } else if (!assetIds.insert(a.id).second) {
            res.errors.push_back("duplicate asset id " + std::to_string(a.id));
        }
        if (a.width < 0 || a.height < 0)
            res.errors.push_back("asset '" + a.name + "' has negative dimensions");
        if (a.isPrecomp && a.layers.empty())
            res.errors.push_back("precomp asset '" + a.name + "' has no layers");
    }
    for (usize k = 0; k < layers_.size(); ++k) {
        const LottieLayer& L = layers_[k];
        if (L.type == LottieLayerType::Unknown)
            res.errors.push_back("layer " + std::to_string(k) + " has an unknown type");
        if (L.outTime < L.inTime) res.errors.push_back("layer '" + L.name + "' has op < ip");
        if (L.timeStretch <= 0)
            res.errors.push_back("layer '" + L.name + "' has a non-positive time stretch");
        if (L.width < 0 || L.height < 0)
            res.errors.push_back("layer '" + L.name + "' has a negative size");
        if (L.type == LottieLayerType::Precomp) {
            if (L.imageName.empty()) {
                res.errors.push_back("precomp layer '" + L.name + "' has no refId");
            } else if (L.precompIndex < 0) {
                res.errors.push_back("precomp layer '" + L.name + "' references missing asset '" +
                                     L.imageName + "'");
            }
        }
        if (L.type == LottieLayerType::Image && L.imageAssetIndex < 0)
            res.errors.push_back("image layer '" + L.name + "' references missing asset '" +
                                 L.imageName + "'");
        if (L.parent >= 0) {
            bool found = false;
            for (const LottieLayer& p : layers_) {
                if (p.index == L.parent) {
                    found = true;
                    break;
                }
            }
            if (!found)
                res.errors.push_back("layer '" + L.name + "' has a dangling parent " +
                                     std::to_string(L.parent));
        }
        for (const LottieShape& s : L.shapes) {
            if (s.type == LottieShape::Type::Unknown) {
                res.errors.push_back("layer '" + L.name + "' contains an unknown shape type");
                break;
            }
        }
    }
    res.ok = res.errors.empty();
    return res;
}

// ===========================================================================
// Отрисовка
// ===========================================================================
namespace {

// ---- утилиты полилиний ---------------------------------------------------
struct Polyline {
    std::vector<Vec2> pts;
    bool closed = false;
    f32 length = 0;
    Vec2 startTangent{1, 0};
};

void RecalcLength(Polyline& pl) {
    pl.length = 0;
    pl.startTangent = {1, 0};
    for (usize k = 0; k + 1 < pl.pts.size(); ++k) {
        Vec2 d = pl.pts[k + 1] - pl.pts[k];
        f32 l = Length(d);
        pl.length += l;
        if (k == 0 && l > kEpsilon) pl.startTangent = d / l;
    }
}

void FlattenCubic(const Vec2& p0, const Vec2& c1, const Vec2& c2, const Vec2& p1,
                  std::vector<Vec2>* out, int depth) {
    Vec2 d = p1 - p0;
    f32 d1 = std::fabs(Cross(c1 - p0, d));
    f32 d2 = std::fabs(Cross(c2 - p0, d));
    f32 tol = 0.25f * (std::fabs(d.x) + std::fabs(d.y) + 1.0f);
    if (depth >= 10 || (d1 + d2) <= tol) {
        out->push_back(p1);
        return;
    }
    Vec2 p01 = (p0 + c1) * 0.5f;
    Vec2 p12 = (c1 + c2) * 0.5f;
    Vec2 p23 = (c2 + p1) * 0.5f;
    Vec2 p012 = (p01 + p12) * 0.5f;
    Vec2 p123 = (p12 + p23) * 0.5f;
    Vec2 mid = (p012 + p123) * 0.5f;
    FlattenCubic(p0, p01, p012, mid, out, depth + 1);
    FlattenCubic(mid, p123, p23, p1, out, depth + 1);
}

void FlattenHandle(const LottiePathHandle& ph, const Mat4* xform, std::vector<Polyline>* out) {
    if (ph.v.size() < 2) return;
    usize n = ph.v.size();
    auto X = [&](const Vec2& p) {
        if (!xform) return p;
        Vec3 t = xform->TransformPoint(Vec3{p.x, p.y, 0});
        return Vec2{t.x, t.y};
    };
    std::vector<Vec2> pts;
    pts.reserve(n * 8 + 4);
    pts.push_back(X(ph.v[0]));
    usize segs = ph.closed ? n : n - 1;
    for (usize k = 0; k < segs; ++k) {
        usize k1 = (k + 1) % n;
        FlattenCubic(X(ph.v[k]), X(ph.v[k] + ph.o[k]), X(ph.v[k1] + ph.i[k1]), X(ph.v[k1]), &pts, 0);
    }
    std::vector<Vec2> cleaned;
    cleaned.reserve(pts.size());
    for (const Vec2& p : pts) {
        if (!cleaned.empty()) {
            const Vec2& q = cleaned.back();
            if (std::fabs(p.x - q.x) < 1e-4f && std::fabs(p.y - q.y) < 1e-4f) continue;
        }
        cleaned.push_back(p);
    }
    if (ph.closed && cleaned.size() > 2) {
        const Vec2& f = cleaned.front();
        const Vec2& b = cleaned.back();
        if (std::fabs(f.x - b.x) < 1e-4f && std::fabs(f.y - b.y) < 1e-4f) cleaned.pop_back();
    }
    if (cleaned.size() < 2) return;
    Polyline pl;
    pl.pts = std::move(cleaned);
    pl.closed = ph.closed;
    RecalcLength(pl);
    out->push_back(std::move(pl));
}

// ---- модификаторы путей -------------------------------------------------------
struct ArcPoint {
    Vec2 p;
    Vec2 tangent{1, 0};
};

ArcPoint PointAtLength(const Polyline& pl, f32 dist) {
    ArcPoint res;
    if (pl.pts.empty()) return res;
    if (pl.pts.size() == 1) {
        res.p = pl.pts[0];
        return res;
    }
    res.tangent = pl.startTangent;
    f32 d = Clampf(dist, 0.0f, pl.length);
    f32 acc = 0;
    for (usize k = 0; k + 1 < pl.pts.size(); ++k) {
        Vec2 seg = pl.pts[k + 1] - pl.pts[k];
        f32 l = Length(seg);
        if (l <= kEpsilon) continue;
        if (acc + l >= d) {
            f32 t = (d - acc) / l;
            res.p = pl.pts[k] + seg * t;
            res.tangent = seg / l;
            return res;
        }
        acc += l;
    }
    res.p = pl.pts.back();
    Vec2 seg = pl.pts.back() - pl.pts[pl.pts.size() - 2];
    f32 l = Length(seg);
    if (l > kEpsilon) res.tangent = seg / l;
    return res;
}

// Трим по длине дуги. `s`/`e` нормализованы в [0,1]; переход через стык (s > e)
// поддержан для незамкнутых путей.
std::vector<Vec2> TrimPolyline(const Polyline& pl, f32 s, f32 e) {
    std::vector<Vec2> out;
    if (pl.pts.size() < 2 || pl.length <= kEpsilon) return out;
    s = Clampf(s, 0.0f, 1.0f);
    e = Clampf(e, 0.0f, 1.0f);
    if (s > e) {
        std::vector<Vec2> a = TrimPolyline(pl, s, 1.0f);
        std::vector<Vec2> b = TrimPolyline(pl, 0.0f, e);
        out = std::move(a);
        for (const Vec2& p : b) out.push_back(p);
        return out;
    }
    if (e - s <= 1e-6f) return out;
    f32 total = pl.length;
    f32 startD = s * total;
    f32 endD = e * total;
    out.push_back(PointAtLength(pl, startD).p);
    f32 acc = 0;
    for (usize k = 0; k + 1 < pl.pts.size(); ++k) {
        Vec2 seg = pl.pts[k + 1] - pl.pts[k];
        f32 l = Length(seg);
        if (l <= kEpsilon) continue;
        f32 nacc = acc + l;
        if (nacc > startD && nacc < endD) out.push_back(pl.pts[k + 1]);
        acc = nacc;
        if (acc >= endD) break;
    }
    out.push_back(PointAtLength(pl, endD).p);
    return out;
}

std::vector<Vec2> OffsetPolyline(const Polyline& pl, f32 amount) {
    usize n = pl.pts.size();
    if (n < 2 || std::fabs(amount) < 1e-5f) return pl.pts;
    std::vector<Vec2> out(n);
    for (usize k = 0; k < n; ++k) {
        Vec2 nPrev{0, 0}, nNext{0, 0};
        bool hasPrev = false, hasNext = false;
        if (pl.closed || k > 0) {
            Vec2 d = pl.pts[k] - pl.pts[(k + n - 1) % n];
            f32 l = Length(d);
            if (l > kEpsilon) {
                nPrev = Perp(d / l);
                hasPrev = true;
            }
        }
        if (pl.closed || k + 1 < n) {
            Vec2 d = pl.pts[(k + 1) % n] - pl.pts[k];
            f32 l = Length(d);
            if (l > kEpsilon) {
                nNext = Perp(d / l);
                hasNext = true;
            }
        }
        Vec2 nrm{0, 0};
        if (hasPrev && hasNext) nrm = Normalize(nPrev + nNext);
        else if (hasPrev) nrm = nPrev;
        else if (hasNext) nrm = nNext;
        out[k] = pl.pts[k] + nrm * amount;
    }
    return out;
}

// Аппроксимация скруглённых углов `rd` срезанием каждого угла квадратичной дугой.
std::vector<Vec2> RoundPolyline(const Polyline& pl, f32 radius) {
    usize n = pl.pts.size();
    if (n < 3 || radius <= 0.01f) return pl.pts;
    std::vector<Vec2> out;
    out.reserve(n * 6);
    const int steps = 4;
    for (usize k = 0; k < n; ++k) {
        bool hasPrev = pl.closed || k > 0;
        bool hasNext = pl.closed || k + 1 < n;
        if (!hasPrev || !hasNext) {
            out.push_back(pl.pts[k]);
            continue;
        }
        Vec2 prev = pl.pts[(k + n - 1) % n];
        Vec2 cur = pl.pts[k];
        Vec2 next = pl.pts[(k + 1) % n];
        Vec2 d0 = cur - prev;
        Vec2 d1 = next - cur;
        f32 l0 = Length(d0), l1 = Length(d1);
        if (l0 < kEpsilon || l1 < kEpsilon) {
            out.push_back(cur);
            continue;
        }
        d0 = d0 / l0;
        d1 = d1 / l1;
        f32 r = radius;
        f32 maxR = 0.5f * (l0 < l1 ? l0 : l1);
        if (r > maxR) r = maxR;
        Vec2 a = cur - d0 * r;
        Vec2 b = cur + d1 * r;
        out.push_back(a);
        for (int s = 1; s < steps; ++s) {
            f32 t = static_cast<f32>(s) / static_cast<f32>(steps);
            out.push_back(a * ((1 - t) * (1 - t)) + cur * (2 * (1 - t) * t) + b * (t * t));
        }
        out.push_back(b);
    }
    return out;
}

// ---- контекст отрисовки -------------------------------------------------------
struct RenderContext {
    Renderer2D* r = nullptr;
    const LottieAnimation* anim = nullptr;
    const LottieImpl* impl = nullptr;
    const LottieAnimation::ImageLoader* loader = nullptr;
    Font* font = nullptr;
    f32 alpha = 1.0f;
    Color tint = Color::White;
    LottieAnimation::RenderStats* stats = nullptr;
    std::unordered_map<const LottieAsset*, std::unique_ptr<Texture>> textureCache;
};

struct StyleState {
    bool hasFill = false;
    bool hasStroke = false;
    Paint fill;
    Paint stroke;
    f32 strokeWidth = 1;
    LineCap cap = LineCap::Butt;
    LineJoin join = LineJoin::Miter;
    f32 miterLimit = 4;
    bool dashed = false;
    f32 dashLength = 0, dashGap = 0;
    bool fillGradient = false;
    bool strokeGradient = false;
    LottieProperty gradientStart, gradientEnd;
    int gradientType = 1;
    const LottieGradient* gradient = nullptr;
    bool trim = false;
    f32 trimStart = 0, trimEnd = 1, trimOffset = 0;
    int trimMode = 1;
    bool offset = false;
    f32 offsetAmount = 0;
    bool roundness = false;
    f32 roundAmount = 0;
};

Color SampleGradient(const LottieGradient& g, f32 t) {
    if (g.colors.empty() || g.offsets.empty()) return Color::White;
    t = Clampf(t, 0.0f, 1.0f);
    usize n = g.offsets.size();
    if (n == 1 || t <= g.offsets.front()) return g.colors.front();
    if (t >= g.offsets.back()) return g.colors.back();
    for (usize k = 0; k + 1 < n && k + 1 < g.colors.size(); ++k) {
        f32 a = g.offsets[k], b = g.offsets[k + 1];
        if (t >= a && t <= b) {
            f32 u = b - a > kEpsilon ? (t - a) / (b - a) : 0.0f;
            Color c = Lerp(g.colors[k], g.colors[k + 1], u);
            f32 oa = k < g.opacities.size() ? g.opacities[k] : 1.0f;
            f32 ob = k + 1 < g.opacities.size() ? g.opacities[k + 1] : 1.0f;
            c.a *= oa + (ob - oa) * u;
            return c;
        }
    }
    return g.colors.back();
}

// `Paint` в Renderer2D несёт один внутренний и один внешний цвет. Поэтому
// градиент с несколькими стопами аппроксимируется сэмплированием рампы у обоих
// концов (15% / 85%), а не схлопыванием до двух крайних стопов.
void GradientPaint(const LottieGradient& g, Color* inner, Color* outer) {
    *inner = SampleGradient(g, 0.15f);
    *outer = SampleGradient(g, 0.85f);
}

void PushTransform(Renderer2D& r, const Mat4& m) {
    r.Transform(m.at(0, 0), m.at(0, 1), m.at(1, 0), m.at(1, 1), m.at(3, 0), m.at(3, 1));
}

void AppendPolyline(Renderer2D& r, const std::vector<Vec2>& pts, bool closed, bool begin) {
    if (pts.size() < 2) return;
    if (begin) r.BeginPath();
    r.MoveTo(pts[0].x, pts[0].y);
    for (usize k = 1; k < pts.size(); ++k) r.LineTo(pts[k].x, pts[k].y);
    if (closed) r.ClosePath();
}

void StrokeDashed(Renderer2D& r, const std::vector<Vec2>& pts, bool closed, f32 dashLen,
                  f32 gapLen) {
    f32 period = dashLen + gapLen;
    if (dashLen <= 0.01f || period <= 0.01f) {
        AppendPolyline(r, pts, closed, true);
        r.Stroke();
        return;
    }
    usize segs = closed ? pts.size() : pts.size() - 1;
    bool on = true;
    bool open = false;
    f32 remaining = dashLen;
    r.BeginPath();
    for (usize k = 0; k < segs; ++k) {
        Vec2 a = pts[k];
        Vec2 b = pts[(k + 1) % pts.size()];
        Vec2 d = b - a;
        f32 l = Length(d);
        if (l <= kEpsilon) continue;
        Vec2 dir = d / l;
        f32 consumed = 0;
        while (consumed < l - 1e-5f) {
            f32 step = (remaining < l - consumed) ? remaining : (l - consumed);
            if (on) {
                Vec2 p0 = a + dir * consumed;
                Vec2 p1 = a + dir * (consumed + step);
                if (!open) {
                    r.MoveTo(p0.x, p0.y);
                    open = true;
                }
                r.LineTo(p1.x, p1.y);
            } else {
                open = false;
            }
            consumed += step;
            remaining -= step;
            if (remaining <= 1e-5f) {
                on = !on;
                remaining = on ? dashLen : gapLen;
                if (remaining <= 0.01f) remaining = period;
                open = false;
            }
        }
    }
    r.Stroke();
}

// Флэттенирует один узел геометрии в полилинии.
void BuildGeometry(const LottieShape& s, f32 time, std::vector<Polyline>* out) {
    switch (s.type) {
        case LottieShape::Type::Path: {
            if (const LottiePathHandle* h = LookupPath(&s.pathData)) {
                FlattenHandle(*h, nullptr, out);
            } else if (s.pathData.pathPoints.size() >= 6) {
                LottiePathHandle flat = MakeHandleFromFlat(s.pathData.EvaluatePath(time));
                FlattenHandle(flat, nullptr, out);
            }
            break;
        }
        case LottieShape::Type::Ellipse: {
            Vec2 c = s.position.EvaluateVec2(time);
            Vec2 sz = s.size.EvaluateVec2(time);
            f32 rx = std::fabs(sz.x) * 0.5f, ry = std::fabs(sz.y) * 0.5f;
            if (rx <= 0 || ry <= 0) break;
            Polyline pl;
            pl.closed = true;
            const int steps = 48;
            for (int k = 0; k < steps; ++k) {
                f32 a = kTau * static_cast<f32>(k) / static_cast<f32>(steps);
                pl.pts.push_back({c.x + std::cos(a) * rx, c.y + std::sin(a) * ry});
            }
            RecalcLength(pl);
            out->push_back(std::move(pl));
            break;
        }
        case LottieShape::Type::Rectangle: {
            Vec2 c = s.position.EvaluateVec2(time);
            Vec2 sz = s.size.EvaluateVec2(time);
            f32 hw = std::fabs(sz.x) * 0.5f, hh = std::fabs(sz.y) * 0.5f;
            if (hw <= 0 || hh <= 0) break;
            f32 round = s.roundness.EvaluateScalar(time);
            Polyline pl;
            pl.closed = true;
            if (round > 0.01f) {
                f32 rr = std::fmin(round, std::fmin(hw, hh));
                const int steps = 6;
                const Vec2 centers[4] = {{c.x + hw - rr, c.y + hh - rr},
                                         {c.x - hw + rr, c.y + hh - rr},
                                         {c.x - hw + rr, c.y - hh + rr},
                                         {c.x + hw - rr, c.y - hh + rr}};
                for (int ci = 0; ci < 4; ++ci) {
                    f32 a0 = kPi * 0.5f * static_cast<f32>(ci);
                    for (int k = 0; k <= steps; ++k) {
                        f32 a = a0 + kPi * 0.5f * static_cast<f32>(k) / steps;
                        pl.pts.push_back({centers[ci].x + std::cos(a) * rr,
                                          centers[ci].y + std::sin(a) * rr});
                    }
                }
            } else {
                pl.pts = {{c.x - hw, c.y - hh},
                          {c.x + hw, c.y - hh},
                          {c.x + hw, c.y + hh},
                          {c.x - hw, c.y + hh}};
            }
            RecalcLength(pl);
            out->push_back(std::move(pl));
            break;
        }
        case LottieShape::Type::PolyStar: {
            Vec2 c = s.position.EvaluateVec2(time);
            f32 outer = s.outerRadius.EvaluateScalar(time);
            f32 inner = s.innerRadius.EvaluateScalar(time);
            int pts = static_cast<int>(s.points.EvaluateScalar(time));
            int starType = static_cast<int>(s.starType.EvaluateScalar(time));
            f32 rot = s.rotation.EvaluateScalar(time) - 90.0f;
            if (pts < 3) pts = 3;
            if (pts > 512) pts = 512;
            if (outer <= 0) break;
            Polyline pl;
            pl.closed = true;
            int total = starType == 2 ? pts * 2 : pts;
            for (int k = 0; k < total; ++k) {
                f32 r = (starType == 2 && (k % 2)) ? inner : outer;
                f32 a = Radians(rot) + kTau * static_cast<f32>(k) / static_cast<f32>(total);
                pl.pts.push_back({c.x + std::cos(a) * r, c.y + std::sin(a) * r});
            }
            RecalcLength(pl);
            out->push_back(std::move(pl));
            break;
        }
        default:
            break;
    }
}

bool IsGeometry(LottieShape::Type t) {
    return t == LottieShape::Type::Path || t == LottieShape::Type::Ellipse ||
           t == LottieShape::Type::Rectangle || t == LottieShape::Type::PolyStar;
}

Color ApplyTint(const RenderContext& ctx, Color c) {
    // Оттенок умножает RGB; альфой управляет глобальная альфа рендерера.
    return {c.r * ctx.tint.r, c.g * ctx.tint.g, c.b * ctx.tint.b, c.a * ctx.tint.a};
}

void DrawGeometries(RenderContext& ctx, const std::vector<Polyline>& polys, const StyleState& st) {
    Renderer2D& r = *ctx.r;
    for (const Polyline& raw : polys) {
        Polyline pl = raw;
        if (st.trim && pl.pts.size() >= 2) {
            std::vector<Vec2> trimmed = TrimPolyline(pl, st.trimStart, st.trimEnd);
            if (trimmed.size() < 2) continue;
            pl.pts = std::move(trimmed);
            pl.closed = false;
            RecalcLength(pl);
        }
        if (st.offset && std::fabs(st.offsetAmount) > 1e-4f) {
            pl.pts = OffsetPolyline(pl, st.offsetAmount);
            RecalcLength(pl);
        }
        if (st.roundness && st.roundAmount > 0.01f) {
            pl.pts = RoundPolyline(pl, st.roundAmount);
            pl.closed = raw.closed;
            RecalcLength(pl);
        }
        if (pl.pts.size() < 2) continue;
        r.Save();
        if (st.hasFill) {
            r.FillPaint(st.fill);
            AppendPolyline(r, pl.pts, pl.closed, true);
            if (ctx.stats) ctx.stats->shapesDrawn++;
            r.Fill();
        }
        if (st.hasStroke) {
            r.StrokePaint(st.stroke);
            r.StrokeWidth(st.strokeWidth);
            r.LineCap(st.cap);
            r.LineJoin(st.join);
            r.MiterLimit(st.miterLimit);
            if (st.dashed) {
                StrokeDashed(r, pl.pts, pl.closed, st.dashLength, st.dashGap);
                if (ctx.stats) ctx.stats->shapesDrawn++;
            } else {
                AppendPolyline(r, pl.pts, pl.closed, true);
                if (ctx.stats) ctx.stats->shapesDrawn++;
                r.Stroke();
            }
        }
        r.Restore();
    }
}

// Строит `Paint` для шейпа стиля заливки/штриха.
Paint MakePaint(RenderContext& ctx, const LottieShape& s, f32 time, bool isStroke, bool* isGradient) {
    *isGradient = false;
    const LottieGradient* g = nullptr;
    if (ctx.impl) {
        auto it = ctx.impl->gradients.find(&s);
        if (it != ctx.impl->gradients.end()) g = &it->second;
    }
    if (g && g->valid()) {
        *isGradient = true;
        f32 op = (isStroke ? s.strokeOpacity : s.fillOpacity).EvaluateScalar(time) / 100.0f;
        if (op < 0) op = 0;
        Color inner, outer;
        GradientPaint(*g, &inner, &outer);
        inner = ApplyTint(ctx, inner);
        outer = ApplyTint(ctx, outer);
        inner.a *= op;
        outer.a *= op;
        Vec2 p0 = s.gradientStart.EvaluateVec2(time);
        Vec2 p1 = s.gradientEnd.EvaluateVec2(time);
        if (s.gradientType == 2) {
            f32 radius = Length(p1 - p0);
            if (radius < kEpsilon) radius = 1.0f;
            return Paint::Radial(p0, 0.0f, radius, inner, outer);
        }
        return Paint::Linear(p0, p1, inner, outer);
    }
    Color c = s.colour.EvaluateColor(time);
    c = ApplyTint(ctx, c);
    f32 op = (isStroke ? s.strokeOpacity : s.fillOpacity).EvaluateScalar(time) / 100.0f;
    c.a *= Clampf(op, 0.0f, 1.0f);
    return Paint::Solid(c);
}

// ---------------------------------------------------------------------------
// Обход шейпов
// ---------------------------------------------------------------------------
// Групповые трансформации компонуются как `parent * local`. Собственная
// трансформация группы никогда не несёт прозрачность в bodymovin (`tr.o` — несёт);
// прозрачность уровня шейпа применяется выставлением альфы рендерера на время группы.
struct ShapeWalker {
    RenderContext& ctx;
    f32 time = 0;
    int depth = 0;

    static const LottieShape& Ref(const LottieShape& s) { return s; }
    static const LottieShape& Ref(const std::unique_ptr<LottieShape>& s) { return *s; }

    // Накапливает одну запись стиля в `st` (первая запись каждого вида побеждает
    // на этом уровне; собственный стиль уровня всегда заменяет унаследованный).
    void CollectStyle(const LottieShape& s, StyleState* st) {
        switch (s.type) {
            case LottieShape::Type::Fill:
            case LottieShape::Type::GradientFill: {
                bool grad = false;
                st->fill = MakePaint(ctx, s, time, false, &grad);
                st->hasFill = true;
                st->fillGradient = grad;
                break;
            }
            case LottieShape::Type::Stroke:
            case LottieShape::Type::GradientStroke: {
                bool grad = false;
                st->stroke = MakePaint(ctx, s, time, true, &grad);
                st->hasStroke = true;
                st->strokeGradient = grad;
                st->strokeWidth = s.strokeWidth.EvaluateScalar(time);
                if (st->strokeWidth <= 0) st->strokeWidth = 1.0f;
                st->cap = s.cap;
                st->join = s.join;
                st->miterLimit = s.miterLimit > 0 ? s.miterLimit : 4.0f;
                f32 dash = s.dashLength.EvaluateScalar(time);
                f32 gap = s.dashGap.EvaluateScalar(time);
                st->dashed = dash > 0.01f || gap > 0.01f;
                st->dashLength = dash;
                st->dashGap = gap;
                break;
            }
            case LottieShape::Type::Trim: {
                st->trim = true;
                st->trimOffset = s.offset.EvaluateScalar(time) / 360.0f;
                st->trimStart = s.start.EvaluateScalar(time) / 100.0f + st->trimOffset;
                st->trimEnd = s.end.EvaluateScalar(time) / 100.0f + st->trimOffset;
                if (ctx.impl) {
                    auto it = ctx.impl->mergeModes.find(&s);
                    if (it != ctx.impl->mergeModes.end()) st->trimMode = it->second;
                }
                break;
            }
            case LottieShape::Type::RoundedCorners:
                st->roundness = true;
                st->roundAmount = s.roundness.EvaluateScalar(time);
                break;
            default:
                break;
        }
        // Offset path (`op`) — модификатор-сиблинг: сдвигает геометрию этого
        // уровня вдоль её нормалей.
        if (ctx.impl) {
            auto it = ctx.impl->offsetPaths.find(&s);
            if (it != ctx.impl->offsetPaths.end()) {
                st->offset = true;
                st->offsetAmount = it->second.EvaluateScalar(time);
            }
        }
    }

    // bodymovin пишет геометрию *до* стилей, которые её рисуют, поэтому один
    // прямой проход нарисовал бы геометрию с пустым стилем. Стили поэтому
    // собираются собственным проходом по уровню сначала (зеркаля lottie-web,
    // который идёт по списку назад с заменой, так что самая ранняя запись
    // каждого вида оказывается активной).
    template <typename List>
    void CollectStyles(const List& items, StyleState* st) {
        bool fillSet = false, strokeSet = false, trimSet = false, roundSet = false;
        for (const auto& entry : items) {
            const LottieShape& s = Ref(entry);
            if (s.hidden) continue;
            switch (s.type) {
                case LottieShape::Type::Fill:
                case LottieShape::Type::GradientFill:
                    if (fillSet) continue;
                    fillSet = true;
                    break;
                case LottieShape::Type::Stroke:
                case LottieShape::Type::GradientStroke:
                    if (strokeSet) continue;
                    strokeSet = true;
                    break;
                case LottieShape::Type::Trim:
                    if (trimSet) continue;
                    trimSet = true;
                    break;
                case LottieShape::Type::RoundedCorners:
                    if (roundSet) continue;
                    roundSet = true;
                    break;
                default:
                    break;
            }
            CollectStyle(s, st);
        }
    }

    // Списки шейпов слоя хранят значения; дети групп — unique_ptrs.
    void Walk(const std::vector<LottieShape>& items, const StyleState& inherited,
              const Mat4& parent) {
        if (depth > 10) return;
        StyleState st = inherited;
        CollectStyles(items, &st);
        for (const LottieShape& s : items) DrawOne(s, st, parent);
    }
    void Walk(const std::vector<std::unique_ptr<LottieShape>>& items, const StyleState& inherited,
              const Mat4& parent) {
        if (depth > 10) return;
        StyleState st = inherited;
        CollectStyles(items, &st);
        for (const std::unique_ptr<LottieShape>& sp : items) {
            if (sp) DrawOne(*sp, st, parent);
        }
    }

    void DrawOne(const LottieShape& s, const StyleState& st, const Mat4& parent) {
        if (s.hidden) return;
        if (s.type == LottieShape::Type::Group) {
            f32 op = s.transform.opacity.EvaluateScalar(time);
            if (!(op > 0.0f)) return;
            Mat4 local = parent * s.transform.Evaluate(time).Matrix();
            bool dim = std::fabs(op - 100.0f) > 0.01f;
            ctx.r->Save();
            PushTransform(*ctx.r, local);
            f32 saved = ctx.alpha;
            if (dim) {
                ctx.alpha = saved * Clampf(op / 100.0f, 0.0f, 1.0f);
                ctx.r->GlobalAlpha(ctx.alpha);
            }
            depth++;
            Walk(s.items, st, Mat4::Identity());
            depth--;
            if (dim) {
                ctx.alpha = saved;
                ctx.r->GlobalAlpha(ctx.alpha);
            }
            ctx.r->Restore();
            return;
        }
        if (IsGeometry(s.type)) {
            std::vector<Polyline> polys;
            BuildGeometry(s, time, &polys);
            if (polys.empty()) return;
            ctx.r->Save();
            PushTransform(*ctx.r, parent);
            DrawGeometries(ctx, polys, st);
            ctx.r->Restore();
            return;
        }
        if (s.type == LottieShape::Type::Repeater) {
            // Repeater (`rp`): оставшиеся сиблинги повторяются с композицией
            // трансформации репитера `index` раз. Копии рисуются сзади наперёд,
            // чтобы первая копия оказалась сверху.
            int copies = static_cast<int>(s.copies.EvaluateScalar(time));
            if (copies < 1) copies = 1;
            if (copies > 64) copies = 64;
            // Копия i стоит в `parent * transform^i`: трансформация репитера
            // повторно применяется в локальном пространстве предыдущей копии.
            Mat4 step = s.transform.Evaluate(time).Matrix();
            Mat4 acc = parent;
            for (int i = 0; i < copies; ++i) {
                ctx.r->Save();
                PushTransform(*ctx.r, acc);
                depth++;
                Walk(s.items, st, Mat4::Identity());
                depth--;
                ctx.r->Restore();
                acc = acc * step;
            }
            return;
        }
        // Стиль / трансформация / неизвестные шейпы сами ничего не рисуют.
    }
};

// ---------------------------------------------------------------------------
// Загрузка изображений (слои-изображения: кэшируются, грузятся при первой отрисовке)
// ---------------------------------------------------------------------------
const Texture* ResolveImage(RenderContext& ctx, const LottieLayer& layer) {
    if (layer.imageAssetIndex < 0) return nullptr;
    const std::vector<LottieAsset>& assets = ctx.anim->Assets();
    if (static_cast<usize>(layer.imageAssetIndex) >= assets.size()) return nullptr;
    const LottieAsset& asset = assets[static_cast<usize>(layer.imageAssetIndex)];
    auto found = ctx.textureCache.find(&asset);
    if (found != ctx.textureCache.end()) return found->second.get();
    const Texture* tex = nullptr;
    if (ctx.loader && *ctx.loader) tex = (*ctx.loader)(layer.imageName);
    if (tex) {
        ctx.textureCache[&asset] = nullptr;  // владение внешнее
        return tex;
    }
    if (!asset.imageData.empty()) {
        auto owned = std::make_unique<Texture>();
        if (owned->LoadFromMemory(asset.imageData.data(), asset.imageData.size())) {
            tex = owned.get();
            ctx.textureCache[&asset] = std::move(owned);
        } else {
            ENG_LOGW("lottie", "embedded image '%s' failed to decode", asset.name.c_str());
            ctx.textureCache[&asset] = nullptr;
        }
        return tex;
    }
    if (!asset.imagePath.empty()) {
        auto owned = std::make_unique<Texture>();
        if (owned->LoadFromFile(asset.imagePath)) {
            tex = owned.get();
            ctx.textureCache[&asset] = std::move(owned);
        } else {
            ctx.textureCache[&asset] = nullptr;
        }
        return tex;
    }
    ctx.textureCache[&asset] = nullptr;
    return nullptr;
}

// ---------------------------------------------------------------------------
// Маски
// ---------------------------------------------------------------------------
// Renderer2D даёт только push/pop клиппинга (без разности путей), поэтому маски
// аппроксимируются: несколько масок пересекаются вложением клипов, `add`
// клипает по контуру маски, а `subtract` клипает по прямоугольнику композиции
// с дыркой на месте маски (обратное направление обхода, правило non-zero).
void ApplyMasks(RenderContext& ctx, const LottieLayer& layer, f32 time) {
    Renderer2D& r = *ctx.r;
    int applied = 0;
    for (const LottieMask& mask : layer.masks) {
        if (mask.inverted) continue;  // обрабатываются после аддитивных масок
        std::vector<Vec2> pts = mask.path.EvaluatePath(time);
        if (pts.size() < 6) continue;
        if (mask.mode == 'n') continue;
        LottiePathHandle h = MakeHandleFromFlat(pts);
        std::vector<Polyline> polys;
        FlattenHandle(h, nullptr, &polys);
        if (polys.empty()) continue;
        r.Save();
        r.BeginPath();
        for (const Polyline& pl : polys) {
            if (pl.pts.size() < 2) continue;
            r.MoveTo(pl.pts[0].x, pl.pts[0].y);
            for (usize k = 1; k < pl.pts.size(); ++k) r.LineTo(pl.pts[k].x, pl.pts[k].y);
            if (pl.closed) r.ClosePath();
        }
        r.ClipPath();
        applied++;
    }
    // Инвертированные маски: весь прямоугольник композиции с маской-дыркой.
    for (const LottieMask& mask : layer.masks) {
        if (!mask.inverted) continue;
        std::vector<Vec2> pts = mask.path.EvaluatePath(time);
        if (pts.size() < 6) continue;
        LottiePathHandle h = MakeHandleFromFlat(pts);
        std::vector<Polyline> polys;
        FlattenHandle(h, nullptr, &polys);
        if (polys.empty()) continue;
        r.Save();
        r.BeginPath();
        f32 w = static_cast<f32>(ctx.anim->Width());
        f32 hgt = static_cast<f32>(ctx.anim->Height());
        if (!(w > 0)) w = 1000.0f;
        if (!(hgt > 0)) hgt = 1000.0f;
        r.PathWinding(Winding::CCW);
        r.MoveTo(0, 0);
        r.LineTo(0, hgt);
        r.LineTo(w, hgt);
        r.LineTo(w, 0);
        r.ClosePath();
        r.PathWinding(Winding::CW);
        for (const Polyline& pl : polys) {
            if (pl.pts.size() < 2) continue;
            r.MoveTo(pl.pts[0].x, pl.pts[0].y);
            for (usize k = 1; k < pl.pts.size(); ++k) r.LineTo(pl.pts[k].x, pl.pts[k].y);
            if (pl.closed) r.ClosePath();
        }
        r.ClipPath();
        applied++;
    }
    if (ctx.stats) ctx.stats->masksApplied += applied;
}

// ---------------------------------------------------------------------------
// Track mattes
// ---------------------------------------------------------------------------
// Без оффскрин RenderTarget мат аппроксимируется клиппингом по силуэту слоя-мата
// (его флэттенированные шейпы, либо ограничивающий прямоугольник для
// solid/изображений). Различие альфа/люма теряется — задокументированное ограничение.
bool ApplyMatteClip(RenderContext& ctx, const LottieLayer& matteLayer, f32 time) {
    Renderer2D& r = *ctx.r;
    std::vector<Polyline> polys;
    for (const LottieShape& s : matteLayer.shapes) {
        if (s.hidden) continue;
        if (IsGeometry(s.type)) BuildGeometry(s, time, &polys);
        for (const std::unique_ptr<LottieShape>& child : s.items) {
            if (IsGeometry(child->type)) BuildGeometry(*child, time, &polys);
        }
    }
    r.Save();
    r.BeginPath();
    if (!polys.empty()) {
        for (const Polyline& pl : polys) {
            if (pl.pts.size() < 2) continue;
            r.MoveTo(pl.pts[0].x, pl.pts[0].y);
            for (usize k = 1; k < pl.pts.size(); ++k) r.LineTo(pl.pts[k].x, pl.pts[k].y);
            if (pl.closed) r.ClosePath();
        }
        r.ClipPath();
    } else {
        // Мат solid / изображения / precomp: клип по прямоугольнику слоя.
        f32 w = matteLayer.width, h = matteLayer.height;
        if (!(w > 0) || !(h > 0)) {
            w = static_cast<f32>(ctx.anim->Width());
            h = static_cast<f32>(ctx.anim->Height());
        }
        if (!(w > 0)) w = 1000.0f;
        if (!(h > 0)) h = 1000.0f;
        r.Rect(0, 0, w, h);
        r.ClipPath();
    }
    if (ctx.stats) ctx.stats->masksApplied++;
    return true;
}

// ---------------------------------------------------------------------------
// Текст
// ---------------------------------------------------------------------------
f32 CharAdvance(Font& font, const std::string& ch, f32 size) {
    static const f32 kFallback = 0.55f;
    if (ch.empty()) return size * kFallback;
    u32 cp = static_cast<unsigned char>(ch[0]);
    if (cp < 0x80) {
        const Glyph* g = font.GetGlyph(cp);
        if (g && g->advance > 0) return g->advance * font.ScaleForSize(size);
    }
    return size * kFallback;
}

void DrawTextLayer(RenderContext& ctx, const LottieLayer& layer, f32 time) {
    Renderer2D& r = *ctx.r;
    Font* font = ctx.font;
    if (!font) return;
    const LottieTextInfo* info = nullptr;
    if (ctx.impl) {
        auto it = ctx.impl->text.find(&layer);
        if (it != ctx.impl->text.end()) info = &it->second;
    }
    std::string text = info ? info->text : layer.text;
    if (text.empty()) return;
    f32 size = info ? info->fontSize : layer.fontSize;
    if (!(size > 0)) size = 24;
    Color fill = info ? info->fill : layer.textColor;
    fill = ApplyTint(ctx, fill);
    f32 tracking = info ? info->tracking : layer.textTracking;
    f32 lineHeight = info ? info->lineHeight : layer.textLineHeight;
    if (!(lineHeight > 0)) {
        // `Font::LineHeight()` в шрифтовых единицах; откат к 1.2x размера em —
        // так поступает большинство текстовых движков для межстрочного интервала по умолчанию.
        f32 units = font->Desc().pixelHeight > 0 ? font->Desc().pixelHeight : size;
        lineHeight = size * 1.2f * (units > 0 ? 1.0f : 1.0f);
    }
    if (!(lineHeight > 0)) lineHeight = size * 1.2f;
    int justification = info ? info->justification : layer.justification;

    // Разбиваем по `\r`, `\n` и CRLF.
    std::vector<std::string> lines;
    std::string cur;
    for (char c : text) {
        if (c == '\r' || c == '\n') {
            if (c == '\n' && !cur.empty() && cur.back() == '\0') cur.pop_back();
            lines.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    lines.push_back(cur);
    // Документы bodymovin обычно привязаны к левому верхнему углу; `j` сдвигает строку.
    f32 y = 0;
    for (const std::string& line : lines) {
        if (line.empty()) {
            y += lineHeight;
            continue;
        }
        f32 width = 0;
        for (usize i = 0; i < line.size();) {
            usize len = 1;
            unsigned char c = static_cast<unsigned char>(line[i]);
            if (c >= 0xF0) len = 4;
            else if (c >= 0xE0) len = 3;
            else if (c >= 0xC0) len = 2;
            f32 adv = CharAdvance(*font, line.substr(i, len), size) + tracking;
            width += adv;
            i += len;
        }
        f32 x = 0;
        if (justification == 1) x = -width;
        else if (justification == 2) x = -width * 0.5f;
        // Построчная отрисовка символов при наличии аниматоров, иначе один вызов.
        bool animated = info && !info->animators.empty();
        if (!animated) {
            r.DrawText(*font, line, x, y, fill, size, TextAlign::Left, TextBaseline::Top, tracking);
            if (ctx.stats) ctx.stats->textDrawn++;
        } else {
            f32 pen = x;
            for (usize i = 0; i < line.size();) {
                usize len = 1;
                unsigned char c = static_cast<unsigned char>(line[i]);
                if (c >= 0xF0) len = 4;
                else if (c >= 0xE0) len = 3;
                else if (c >= 0xC0) len = 2;
                std::string ch = line.substr(i, len);
                f32 adv = CharAdvance(*font, ch, size) + tracking;
                // Нормализованная позиция этого символа в строке.
                f32 t = width > kEpsilon ? Clampf((pen - x) / width, 0.0f, 1.0f) : 0.0f;
                Color col = fill;
                f32 extra = 0;
                for (const LottieTextAnimator& anim : info->animators) {
                    f32 a = anim.start, b = anim.end;
                    if (a > b) std::swap(a, b);
                    bool inRange = (t >= a && t <= b);
                    if (!inRange) continue;
                    if (anim.hasFill) col = ApplyTint(ctx, anim.fillColor.EvaluateColor(time));
                    if (anim.hasOpacity) {
                        f32 op = anim.opacity.EvaluateScalar(time);
                        col.a *= Clampf(op / 100.0f, 0.0f, 1.0f);
                    }
                    if (anim.hasTracking) extra += anim.tracking.EvaluateScalar(time);
                }
                if (col.a > 0.001f)
                    r.DrawText(*font, ch, pen, y, col, size, TextAlign::Left, TextBaseline::Top,
                               0.0f);
                if (ctx.stats) ctx.stats->textDrawn++;
                pen += adv + extra;
                i += len;
            }
        }
        y += lineHeight;
    }
}

// ---------------------------------------------------------------------------
// Отрисовка слоёв
// ---------------------------------------------------------------------------
// Значения `bm` (blend mode): 0 normal, 1 multiply, 2 screen, 3 overlay.
// Renderer2D открывает Alpha/Multiply/Screen/Additive, поэтому остальные режимы
// откатываются к альфе и логируются один раз.
BlendMode MapBlendMode(int bm, bool* fellBack) {
    switch (bm) {
        case 0: return BlendMode::Alpha;
        case 1: return BlendMode::Multiply;
        case 2: return BlendMode::Screen;
        case 14: return BlendMode::Additive;
        default: break;
    }
    if (fellBack && !*fellBack) {
        *fellBack = true;
        ENG_LOGW("lottie", "blend mode %d unsupported by Renderer2D; falling back to alpha", bm);
    }
    return BlendMode::Alpha;
}

void DrawLayerList(RenderContext& ctx, const std::vector<LottieLayer>& layers, const Mat4& compMatrix,
                   const Mat4& chainIn, f32 compTime, int depth) {
    (void)compMatrix;
    if (depth > 8) return;
    bool warnedBlend = false;
    // Массивы слоёв bodymovin упорядочены спереди назад (индекс 0 — верхний
    // слой), поэтому слои рисуются в обратном порядке ради правильного стека.
    for (usize li = layers.size(); li-- > 0;) {
        const LottieLayer& layer = layers[li];
        if (layer.hidden) continue;
        // Видимый диапазон времени (фреймы композиции bodymovin).
        f32 localCompTime = compTime - layer.startTime;
        f32 stretch = std::fabs(layer.timeStretch) > kEpsilon ? layer.timeStretch : 1.0f;
        f32 stretched = localCompTime / stretch;
        f32 time = stretched;
        // Time remap (`tm`) переопределяет выведенное локальное время.
        if (ctx.impl) {
            auto it = ctx.impl->timeRemap.find(&layer);
            if (it != ctx.impl->timeRemap.end()) {
                f32 remapped = it->second.EvaluateScalar(stretched / ctx.anim->FrameRate());
                time = remapped;
            }
        }
        if (time < layer.inTime - 0.001f || time > layer.outTime + 0.001f) continue;

        // Цепочка трансформаций: сначала предки, затем этот слой.
        Mat4 mat;
        {
            Mat4 chain = chainIn;
            std::vector<const LottieLayer*> parents;
            int p = layer.parent;
            for (int guard = 0; guard < 32 && p >= 0; ++guard) {
                const LottieLayer* found = nullptr;
                for (const LottieLayer& cand : layers) {
                    if (cand.index == p) {
                        found = &cand;
                        break;
                    }
                }
                if (!found) break;
                parents.push_back(found);
                p = found->parent;
            }
            for (usize i = parents.size(); i-- > 0;) chain = chain * parents[i]->transform.Matrix(time);
            mat = chain * layer.transform.Matrix(time);
        }

        f32 opacity = Clampf(layer.transform.opacity.EvaluateScalar(time) / 100.0f, 0.0f, 1.0f);
        if (opacity <= 0.001f) continue;
        f32 savedAlpha = ctx.alpha;
        ctx.alpha = savedAlpha * opacity;

        Renderer2D& r = *ctx.r;
        r.Save();
        PushTransform(r, mat);
        r.GlobalAlpha(ctx.alpha);
        if (ctx.stats) ctx.stats->layersDrawn++;

        // Маски (вычисляются в пространстве слоя, после выставления трансформации).
        if (layer.hasMask && !layer.masks.empty()) ApplyMasks(ctx, layer, time);

        // Track matte: клип по силуэту слоя-мата. Без оффскрин RenderTarget это
        // задокументированная аппроксимация.
        bool matteClipped = false;
        if (layer.matteMode != 0 && layer.matteLayer >= 0) {
            const LottieLayer* matte = nullptr;
            for (const LottieLayer& cand : layers) {
                if (cand.index == layer.matteLayer) {
                    matte = &cand;
                    break;
                }
            }
            if (matte) {
                r.Save();
                ApplyMatteClip(ctx, *matte, compTime);
                matteClipped = true;
            } else {
                ENG_LOGW("lottie", "layer '%s' references missing matte %d", layer.name.c_str(),
                         layer.matteLayer);
            }
        }

        int bm = 0;
        if (ctx.impl) {
            auto it = ctx.impl->blendModes.find(&layer);
            if (it != ctx.impl->blendModes.end()) bm = it->second;
        }
        if (bm != 0) r.Composite(MapBlendMode(bm, &warnedBlend));

        switch (layer.type) {
            case LottieLayerType::Solid: {
                f32 w = layer.width > 0 ? layer.width : static_cast<f32>(ctx.anim->Width());
                f32 h = layer.height > 0 ? layer.height : static_cast<f32>(ctx.anim->Height());
                Color c = ApplyTint(ctx, layer.solidColor);
                c.a *= ctx.alpha;
                // Solid-слои AE центрированы на начале слоя (`sw`x`sh` вокруг
                // якоря), а не привязаны своим левым верхним углом.
                r.FillRect(-w * 0.5f, -h * 0.5f, w, h, c);
                if (ctx.stats) ctx.stats->shapesDrawn++;
                break;
            }
            case LottieLayerType::Image: {
                const Texture* tex = ResolveImage(ctx, layer);
                if (tex) {
                    f32 w = static_cast<f32>(tex->Width());
                    f32 h = static_cast<f32>(tex->Height());
                    if (!(w > 0)) w = static_cast<f32>(ctx.anim->Width());
                    if (!(h > 0)) h = static_cast<f32>(ctx.anim->Height());
                    Color tint = ApplyTint(ctx, Color::White);
                    tint.a *= ctx.alpha;
                    r.Image(*tex, Rect{0, 0, w, h}, Rect{0, 0, 1, 1}, tint);
                    if (ctx.stats) ctx.stats->imagesDrawn++;
                }
                break;
            }
            case LottieLayerType::Shape:
            case LottieLayerType::Null: {
                ShapeWalker walker{ctx, time, 0};
                StyleState style;
                walker.Walk(layer.shapes, style, Mat4::Identity());
                break;
            }
            case LottieLayerType::Text:
                DrawTextLayer(ctx, layer, time);
                break;
            case LottieLayerType::Precomp: {
                if (layer.precompIndex < 0 ||
                    static_cast<usize>(layer.precompIndex) >= ctx.anim->Assets().size())
                    break;
                const LottieAsset& asset =
                    ctx.anim->Assets()[static_cast<usize>(layer.precompIndex)];
                // Слои precomp вычисляются на собственной временной шкале precomp;
                // у встроенных ассетов `fr` совпадает с родительской композицией.
                f32 rate = ctx.anim->FrameRate();
                f32 childTime = stretched / (rate > 0 ? rate : 60.0f);
                DrawLayerList(ctx, asset.layers, compMatrix, mat, childTime, depth + 1);
                break;
            }
            default:
                break;
        }

        if (matteClipped) r.Restore();
        if (bm != 0) r.Composite(BlendMode::Alpha);
        r.Restore();
        ctx.alpha = savedAlpha;
    }
}

}  // namespace

// ===========================================================================
// LottieAnimation: rendering
// ===========================================================================
void LottieAnimation::Render(Renderer2D& r, const Rect& dst, f32 alpha, const Color& tint) const {
    stats_ = RenderStats{};
    if (!valid_) return;
    const LottieImpl* savedImpl = g_activeImpl;
    g_activeImpl = impl_.get();
    f32 w = static_cast<f32>(width_ > 0 ? width_ : 1);
    f32 h = static_cast<f32>(height_ > 0 ? height_ : 1);
    // Вписываем композицию в `dst` с сохранением пропорций (letterbox /
    // pillarbox) и центрируем, чтобы графика никогда не растягивалась и не обрезалась.
    f32 scale = dst.w / w;
    if (dst.h / h < scale) scale = dst.h / h;
    if (!(scale > 0.0f)) scale = 1.0f;
    Vec2 offset{dst.x + (dst.w - w * scale) * 0.5f, dst.y + (dst.h - h * scale) * 0.5f};
    Mat4 comp = Mat4::Translate(Vec3{offset.x, offset.y, 0}) * Mat4::Scale(Vec3{scale, scale, 1});
    RenderContext ctx;
    ctx.r = &r;
    ctx.anim = this;
    ctx.impl = impl_.get();
    ctx.loader = &imageLoader_;
    ctx.font = font_;
    if (!ctx.font) ctx.font = FontManager::Get().DefaultFont();
    ctx.alpha = alpha;
    ctx.tint = tint;
    ctx.stats = &stats_;
    DrawLayerList(ctx, layers_, comp, comp, currentFrame_ / (frameRate_ > 0 ? frameRate_ : 1.0f), 0);
    // Сбрасываем состояние клипа/альфы, изменённое масками (каждое было сбалансировано Save).
    r.GlobalAlpha(1.0f);
    g_activeImpl = savedImpl;
}

void LottieAnimation::RenderAt(Renderer2D& r, const Vec2& position, const Vec2& size, f32 rotation,
                               f32 alpha) const {
    stats_ = RenderStats{};
    if (!valid_) return;
    const LottieImpl* savedImpl = g_activeImpl;
    g_activeImpl = impl_.get();
    f32 w = static_cast<f32>(width_ > 0 ? width_ : 1);
    f32 h = static_cast<f32>(height_ > 0 ? height_ : 1);
    r.Save();
    r.Translate(position.x, position.y);
    if (rotation != 0.0f) r.Rotate(rotation);
    r.Translate(-size.x * 0.5f, -size.y * 0.5f);
    r.Scale(size.x / w, size.y / h);
    RenderContext ctx;
    ctx.r = &r;
    ctx.anim = const_cast<LottieAnimation*>(this);
    ctx.impl = impl_.get();
    ctx.font = font_;
    if (!ctx.font) ctx.font = FontManager::Get().DefaultFont();
    ctx.alpha = alpha;
    ctx.tint = Color::White;
    ctx.stats = &stats_;
    DrawLayerList(ctx, layers_, Mat4::Identity(), Mat4::Identity(),
                  currentFrame_ / (frameRate_ > 0 ? frameRate_ : 1.0f), 0);
    r.GlobalAlpha(1.0f);
    r.Restore();
    g_activeImpl = savedImpl;
}

}  // namespace crossrender

