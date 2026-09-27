// Процедурные bodymovin/Lottie-документы для встроенных демо-анимаций плюс
// кэш путей/имён, используемый LottieLibrary.
//
// Каждый документ здесь создаётся собственным JSON DOM движка и повторно
// разбирается через тот же самый код, что и импортируемый файл, поэтому
// генератор заодно служит тестовым набором для проверки парсера. Анимации
// самодостаточны (без внешних изображений и шрифтов) и используют только реализованное.
#include "LottieInternal.h"

#include "crossrender/core/Log.h"
#include "crossrender/core/File.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <cstdlib>

namespace crossrender {
namespace {

using Arr = JsonArray;
using Obj = JsonObject;

JsonValue Num(f64 v) { return JsonValue(v); }
JsonValue Int(int v) { return JsonValue(static_cast<double>(v)); }
JsonValue Str(std::string s) { return JsonValue(std::move(s)); }
JsonValue BoolV(bool b) { return JsonValue(b); }

JsonValue ArrOf(std::initializer_list<JsonValue> items) {
    JsonArray a;
    a.reserve(items.size());
    for (const JsonValue& v : items) a.push_back(v);
    return JsonValue(std::move(a));
}

JsonValue ObjOf(std::initializer_list<std::pair<const std::string, JsonValue>> items) {
    Obj o;
    for (const auto& kv : items) o[kv.first] = kv.second;
    return JsonValue(std::move(o));
}

// {"a":0,"k":value}
JsonValue Static(JsonValue value) {
    return ObjOf({{"a", Int(0)}, {"k", std::move(value)}});
}

// Ключевой кадр. `easeOut`/`easeIn` — контрольные точки Безье исходящей и
// входящей касательных; опускаются для линейных и hold-ключей.
JsonValue Key(f64 t, JsonValue s, bool hold = false, f64 ox = 0.0, f64 oy = 0.0, f64 ix = 0.0,
              f64 iy = 0.0) {
    Obj k;
    k["t"] = Num(t);
    k["s"] = std::move(s);
    if (hold) {
        k["h"] = Int(1);
        return JsonValue(std::move(k));
    }
    if (ox != 0.0 || oy != 0.0 || ix != 0.0 || iy != 0.0) {
        k["o"] = ObjOf({{"x", ArrOf({Num(ox)})}, {"y", ArrOf({Num(oy)})}});
        k["i"] = ObjOf({{"x", ArrOf({Num(ix)})}, {"y", ArrOf({Num(iy)})}});
    }
    return JsonValue(std::move(k));
}

JsonValue Keyed(Arr keys) { return ObjOf({{"a", Int(1)}, {"k", JsonValue(std::move(keys))}}); }

// ---- общие конструкторы шейпов ---------------------------------------------
JsonValue ShapeNode(const char* ty, const char* nm) {
    return ObjOf({{"ty", Str(ty)}, {"nm", Str(nm)}, {"hd", BoolV(false)}});
}

JsonValue RectShape(const char* nm, Vec2 size, f32 roundness) {
    JsonValue s = ShapeNode("rc", nm);
    s.Set("p", Static(ArrOf({Num(0), Num(0)})));
    s.Set("s", Static(ArrOf({Num(size.x), Num(size.y)})));
    s.Set("r", Static(Num(roundness)));
    return s;
}

JsonValue EllipseShape(const char* nm, Vec2 size) {
    JsonValue s = ShapeNode("el", nm);
    s.Set("p", Static(ArrOf({Num(0), Num(0)})));
    s.Set("s", Static(ArrOf({Num(size.x), Num(size.y)})));
    return s;
}

JsonValue PathShape(const char* nm, const std::vector<Vec2>& verts, const std::vector<Vec2>& in,
                    const std::vector<Vec2>& out, bool closed) {
    JsonArray v, i, o;
    for (usize k = 0; k < verts.size(); ++k) {
        v.push_back(ArrOf({Num(verts[k].x), Num(verts[k].y)}));
        i.push_back(ArrOf({Num(k < in.size() ? in[k].x : 0), Num(k < in.size() ? in[k].y : 0)}));
        o.push_back(ArrOf({Num(k < out.size() ? out[k].x : 0), Num(k < out.size() ? out[k].y : 0)}));
    }
    if (closed && !verts.empty()) {
        v.push_back(ArrOf({Num(verts[0].x), Num(verts[0].y)}));
        i.push_back(ArrOf({Num(0), Num(0)}));
        o.push_back(ArrOf({Num(0), Num(0)}));
    }
    Obj path;
    path["i"] = JsonValue(std::move(i));
    path["o"] = JsonValue(std::move(o));
    path["v"] = JsonValue(std::move(v));
    path["c"] = BoolV(closed);
    JsonValue s = ShapeNode("sh", nm);
    s.Set("ks", Static(JsonValue(std::move(path))));
    return s;
}

JsonValue FillShape(const char* nm, Color c, f32 opacity = 100.0f) {
    JsonValue s = ShapeNode("fl", nm);
    s.Set("c", Static(ArrOf({Num(c.r), Num(c.g), Num(c.b)})));
    s.Set("o", Static(Num(opacity)));
    s.Set("r", Int(1));
    return s;
}

JsonValue StrokeShape(const char* nm, Color c, f32 width, f32 opacity = 100.0f, int cap = 2,
                      int join = 2, bool dashes = false, f32 dash = 0, f32 gap = 0) {
    JsonValue s = ShapeNode("st", nm);
    s.Set("c", Static(ArrOf({Num(c.r), Num(c.g), Num(c.b)})));
    s.Set("o", Static(Num(opacity)));
    s.Set("w", Static(Num(width)));
    s.Set("lc", Int(cap));
    s.Set("lj", Int(join));
    s.Set("ml", Int(4));
    if (dashes) {
        JsonArray d;
        d.push_back(ObjOf({{"n", Str("d")},
                           {"v", Static(Num(dash))},
                           {"nm", Str("dash")}}));
        d.push_back(ObjOf({{"n", Str("g")},
                           {"v", Static(Num(gap))},
                           {"nm", Str("gap")}}));
        s.Set("d", JsonValue(std::move(d)));
    }
    return s;
}

// Линейный градиент с `count` равномерно распределёнными стопами.
JsonValue GradientFillShape(const char* nm, const std::vector<Color>& colors, Vec2 start, Vec2 end,
                            f32 opacity = 100.0f) {
    JsonValue s = ShapeNode("gf", nm);
    s.Set("t", Int(1));
    JsonArray flat;
    f32 step = colors.size() > 1 ? 1.0f / static_cast<f32>(colors.size() - 1) : 0.0f;
    for (usize k = 0; k < colors.size(); ++k) {
        flat.push_back(Num(static_cast<f32>(k) * step));
        flat.push_back(Num(colors[k].r));
        flat.push_back(Num(colors[k].g));
        flat.push_back(Num(colors[k].b));
    }
    Obj g;
    g["p"] = Int(static_cast<int>(colors.size()));
    g["k"] = Static(JsonValue(std::move(flat)));
    s.Set("g", JsonValue(std::move(g)));
    s.Set("s", Static(ArrOf({Num(start.x), Num(start.y)})));
    s.Set("e", Static(ArrOf({Num(end.x), Num(end.y)})));
    s.Set("o", Static(Num(opacity)));
    s.Set("r", Int(1));
    return s;
}

JsonValue TransformShape(const char* nm, Vec2 position, f32 rotationDeg, Vec2 scalePercent,
                         f32 opacity) {
    JsonValue s = ShapeNode("tr", nm);
    s.Set("a", Static(ArrOf({Num(0), Num(0)})));
    s.Set("p", Static(ArrOf({Num(position.x), Num(position.y)})));
    s.Set("s", Static(ArrOf({Num(scalePercent.x), Num(scalePercent.y)})));
    s.Set("r", Static(Num(rotationDeg)));
    s.Set("o", Static(Num(opacity)));
    s.Set("sk", Static(Num(0)));
    s.Set("sa", Static(Num(0)));
    return s;
}

JsonValue TrimShape(const char* nm, JsonValue start, JsonValue end, int mode = 1) {
    JsonValue s = ShapeNode("tm", nm);
    s.Set("s", std::move(start));
    s.Set("e", std::move(end));
    s.Set("o", Static(Num(0)));
    s.Set("m", Int(mode));
    return s;
}

JsonValue GroupShape(const char* nm, JsonArray items) {
    JsonValue g = ShapeNode("gr", nm);
    g.Set("it", JsonValue(std::move(items)));
    g.Set("np", Int(static_cast<int>(items.size())));
    return g;
}

JsonValue RepeaterShape(const char* nm, JsonValue copies, JsonValue transform) {
    JsonValue s = ShapeNode("rp", nm);
    s.Set("c", std::move(copies));
    s.Set("tr", std::move(transform));
    return s;
}

JsonValue RoundedCornersShape(const char* nm, JsonValue radius) {
    JsonValue s = ShapeNode("rd", nm);
    s.Set("r", std::move(radius));
    return s;
}

JsonValue MergeShape(const char* nm, int mode) {
    JsonValue s = ShapeNode("mm", nm);
    s.Set("mm", Int(mode));
    return s;
}

JsonValue OffsetPathShape(const char* nm, JsonValue amount) {
    JsonValue s = ShapeNode("op", nm);
    s.Set("a", std::move(amount));
    s.Set("lj", Int(1));
    s.Set("ml", Int(4));
    return s;
}

// ---- конструкторы трансформаций / слоёв ------------------------------------
JsonValue LayerTransform(Vec2 position, f32 rotationDeg = 0, Vec2 scale = {100, 100},
                         f32 opacity = 100, JsonValue positionKeyed = JsonValue(),
                         JsonValue scaleKeyed = JsonValue(),
                         JsonValue rotationKeyed = JsonValue(),
                         JsonValue opacityKeyed = JsonValue()) {
    Obj ks;
    ks["a"] = Static(ArrOf({Num(0), Num(0)}));
    ks["p"] = positionKeyed.IsNull() ? Static(ArrOf({Num(position.x), Num(position.y)}))
                                     : std::move(positionKeyed);
    ks["s"] = scaleKeyed.IsNull() ? Static(ArrOf({Num(scale.x), Num(scale.y)}))
                                  : std::move(scaleKeyed);
    ks["r"] = rotationKeyed.IsNull() ? Static(Num(rotationDeg)) : std::move(rotationKeyed);
    ks["o"] = opacityKeyed.IsNull() ? Static(Num(opacity)) : std::move(opacityKeyed);
    ks["sk"] = Static(Num(0));
    ks["sa"] = Static(Num(0));
    return JsonValue(std::move(ks));
}

JsonValue ShapeLayer(int ind, const char* nm, JsonValue ks, JsonArray shapes, f32 ip, f32 op,
                     int parent = -1, int bm = 0) {
    Obj layer;
    layer["ddd"] = Int(0);
    layer["ind"] = Int(ind);
    if (parent >= 0) layer["parent"] = Int(parent);
    layer["ty"] = Int(4);
    layer["nm"] = Str(nm);
    layer["sr"] = Int(1);
    layer["ks"] = std::move(ks);
    layer["ao"] = Int(0);
    layer["shapes"] = JsonValue(std::move(shapes));
    layer["ip"] = Num(ip);
    layer["op"] = Num(op);
    layer["st"] = Num(0);
    layer["bm"] = Int(bm);
    return JsonValue(std::move(layer));
}

JsonValue SolidLayer(int ind, const char* nm, JsonValue ks, Vec2 size, Color color, f32 ip, f32 op) {
    char hex[16];
    std::snprintf(hex, sizeof(hex), "#%02x%02x%02x",
                  static_cast<int>(color.r * 255.0f + 0.5f),
                  static_cast<int>(color.g * 255.0f + 0.5f),
                  static_cast<int>(color.b * 255.0f + 0.5f));
    Obj layer;
    layer["ddd"] = Int(0);
    layer["ind"] = Int(ind);
    layer["ty"] = Int(1);
    layer["nm"] = Str(nm);
    layer["sr"] = Int(1);
    layer["ks"] = std::move(ks);
    layer["ao"] = Int(0);
    layer["sw"] = Num(size.x);
    layer["sh"] = Num(size.y);
    layer["sc"] = Str(hex);
    layer["ip"] = Num(ip);
    layer["op"] = Num(op);
    layer["st"] = Num(0);
    layer["bm"] = Int(0);
    return JsonValue(std::move(layer));
}

JsonValue Composition(const char* nm, int w, int h, f32 fr, f32 ip, f32 op, JsonArray layers,
                      JsonArray assets = JsonArray()) {
    Obj doc;
    doc["v"] = Str("5.7.4");
    doc["fr"] = Num(fr);
    doc["ip"] = Num(ip);
    doc["op"] = Num(op);
    doc["w"] = Int(w);
    doc["h"] = Int(h);
    doc["nm"] = Str(nm);
    doc["ddd"] = Int(0);
    doc["assets"] = JsonValue(std::move(assets));
    doc["layers"] = JsonValue(std::move(layers));
    doc["markers"] = JsonValue(JsonArray{});
    return JsonValue(std::move(doc));
}

// Формирует JSON-документ. `JsonValue::Dump` входит в JSON API движка.
std::string DumpDoc(const JsonValue& v) {
    std::string s = v.Dump();
    return s;
}

// ===========================================================================
// Встроенные анимации
// ===========================================================================
// "loading": вращающееся пунктирное кольцо (trim path) с пульсирующей точкой
// в центре. 240x240, 60fps, фреймы 0..120 (цикл 2с).
std::string GenerateLoading() {
    const Color ring{0.36f, 0.72f, 1.0f, 1.0f};
    const Color dot{1.0f, 1.0f, 1.0f, 1.0f};
    // Хвост использует repeater (повтор пунктирной дуги с небольшим поворотом
    // и растущим штрихом иначе выразить нельзя) плюс offset path, слегка
    // приподнимающий кольцо над центром дуги.
    Arr trailItems;
    trailItems.push_back(EllipseShape("trailRing", {160, 160}));
    trailItems.push_back(StrokeShape("trailStroke", ring, 8, 45, 2, 2));
    trailItems.push_back(TrimShape("trailTrim", Static(Num(0)), Static(Num(22))));
    trailItems.push_back(OffsetPathShape("trailOffset", Static(Num(9))));
    trailItems.push_back(RepeaterShape("trailRepeat", Static(Num(4)),
                                       TransformShape("trailTr", {0, 0}, 26, {100, 100}, 100)));
    Arr ringItems;
    ringItems.push_back(EllipseShape("ring", {160, 160}));
    ringItems.push_back(StrokeShape("ringStroke", ring, 12, 100, 2, 2));
    ringItems.push_back(TrimShape("ringTrim", Static(Num(0)), Static(Num(72))));
    ringItems.push_back(GroupShape("trailGroup", std::move(trailItems)));
    Arr ringGroup{GroupShape("ringGroup", std::move(ringItems))};

    JsonValue ringRotation =
        Keyed(Arr{Key(0, Num(0)), Key(120, Num(360)), Key(240, Num(720))});
    JsonValue ringKs = LayerTransform({120, 120}, 0, {100, 100}, 100, JsonValue(),
                                      JsonValue(), std::move(ringRotation));

    Arr dotGroup{GroupShape("dotGroup", Arr{EllipseShape("dot", {44, 44}), FillShape("dotFill", dot)})};
    JsonValue dotScale = Keyed(Arr{Key(0, ArrOf({Num(70), Num(70)}), false, 0.42, 0, 0.58, 1),
                                   Key(60, ArrOf({Num(130), Num(130)}), false, 0.42, 0, 0.58, 1),
                                   Key(120, ArrOf({Num(70), Num(70)}))});
    JsonValue dotOpacity = Keyed(Arr{Key(0, Num(35)), Key(60, Num(100)), Key(120, Num(35))});
    JsonValue dotKs = LayerTransform({120, 120}, 0, {100, 100}, 100, JsonValue(),
                                     std::move(dotScale), JsonValue(), std::move(dotOpacity));

    Arr layers;
    layers.push_back(ShapeLayer(1, "dot", std::move(dotKs), std::move(dotGroup), 0, 120));
    layers.push_back(ShapeLayer(2, "ring", std::move(ringKs), std::move(ringGroup), 0, 120));
    JsonValue doc = Composition("loading", 240, 240, 60, 0, 120, std::move(layers));
    return DumpDoc(doc);
}

// "success": круг, «выстреливающий» появлением, с последующей отрисовкой
// галочки через trim-path. 240x240, 60fps, фреймы 0..90 (1.5с).
std::string GenerateSuccess() {
    const Color green{0.16f, 0.78f, 0.42f, 1.0f};
    const Color white{1.0f, 1.0f, 1.0f, 1.0f};

    Arr circleItems;
    circleItems.push_back(EllipseShape("circle", {170, 170}));
    circleItems.push_back(FillShape("circleFill", green));
    Arr circleGroup{GroupShape("circleGroup", std::move(circleItems))};
    JsonValue popScale = Keyed(Arr{Key(0, ArrOf({Num(0), Num(0)})),
                                   Key(14, ArrOf({Num(112), Num(112)}), false, 0.2, 0, 0.4, 1),
                                   Key(24, ArrOf({Num(100), Num(100)}))});
    JsonValue circleKs = LayerTransform({120, 120}, 0, {100, 100}, 100, JsonValue(),
                                        std::move(popScale));

    std::vector<Vec2> verts = {{-42, 4}, {-12, 34}, {44, -30}};
    std::vector<Vec2> in = {{0, 0}, {-10, 0}, {0, 12}};
    std::vector<Vec2> out = {{0, 0}, {10, 0}, {0, -12}};
    Arr checkItems;
    checkItems.push_back(PathShape("check", verts, in, out, false));
    checkItems.push_back(StrokeShape("checkStroke", white, 16, 100, 2, 2));
    checkItems.push_back(TrimShape("checkTrim", Static(Num(0)),
                                   Keyed(Arr{Key(8, Num(0)), Key(52, Num(100), false, 0.33, 0, 0.67, 1)})));
    Arr checkGroup{GroupShape("checkGroup", std::move(checkItems))};
    JsonValue checkKs = LayerTransform({120, 120});

    Arr layers;
    layers.push_back(ShapeLayer(1, "check", std::move(checkKs), std::move(checkGroup), 0, 90));
    layers.push_back(ShapeLayer(2, "circle", std::move(circleKs), std::move(circleGroup), 0, 90));
    JsonValue doc = Composition("success", 240, 240, 60, 0, 90, std::move(layers));
    return DumpDoc(doc);
}

// "heart": контур сердца, «бьющийся» (масштаб + цвет). 240x240, 60fps, 0..72.
std::string GenerateHeart() {
    const Color red{0.93f, 0.22f, 0.35f, 1.0f};
    const Color pink{1.0f, 0.45f, 0.58f, 1.0f};
    // Контур сердца: две доли радиусом 35px с центрами (+-35,-30), сходящиеся
    // во впадине (0,-30), и остриё в нижней точке (0,78). Кубические дуги
    // берут стандартную константу окружности k = 0.5523 * r, чтобы доли были истинными окружностями.
    const f32 k = 0.5523f * 35.0f;  // 19.33
    std::vector<Vec2> verts = {{0, 78}, {-70, -30}, {-35, -65}, {0, -30}, {35, -65}, {70, -30}};
    std::vector<Vec2> in = {{38, 0}, {0, 38}, {-k, 0}, {0, -k}, {-k, 0}, {0, -k}};
    std::vector<Vec2> out = {{-38, 0}, {0, -k}, {k, 0}, {0, k}, {k, 0}, {0, 38}};

    Arr items;
    items.push_back(PathShape("heartPath", verts, in, out, true));
    JsonValue fillShape = ShapeNode("fl", "heartFill");
    fillShape.Set("c", Keyed(Arr{
                            Key(0, ArrOf({Num(red.r), Num(red.g), Num(red.b)})),
                            Key(18, ArrOf({Num(pink.r), Num(pink.g), Num(pink.b)})),
                            Key(36, ArrOf({Num(red.r), Num(red.g), Num(red.b)})),
                            Key(54, ArrOf({Num(pink.r), Num(pink.g), Num(pink.b)})),
                            Key(72, ArrOf({Num(red.r), Num(red.g), Num(red.b)}))}));
    fillShape.Set("o", Static(Num(100)));
    fillShape.Set("r", Int(1));
    items.push_back(std::move(fillShape));
    items.push_back(MergeShape("heartMerge", 1));  // объединение (аппроксимация отрисовкой обоих)
    Arr group{GroupShape("heartGroup", std::move(items))};

    JsonValue beat = Keyed(Arr{
        Key(0, ArrOf({Num(100), Num(100)})),
        Key(9, ArrOf({Num(118), Num(118)}), false, 0.2, 0, 0.4, 1),
        Key(18, ArrOf({Num(100), Num(100)})),
        Key(30, ArrOf({Num(108), Num(108)}), false, 0.2, 0, 0.4, 1),
        Key(44, ArrOf({Num(100), Num(100)}), false, 0.42, 0, 0.58, 1),
        Key(72, ArrOf({Num(100), Num(100)}))});
    JsonValue ks = LayerTransform({120, 110}, 0, {100, 100}, 100, JsonValue(), std::move(beat));

    Arr layers;
    layers.push_back(ShapeLayer(1, "heart", std::move(ks), std::move(group), 0, 72));
    JsonValue doc = Composition("heart", 240, 240, 60, 0, 72, std::move(layers));
    return DumpDoc(doc);
}

// "checkmark": чистая отрисовка штриха через trim-path. 200x200, 60fps, фреймы 0..60 (1с).
std::string GenerateCheckmark() {
    const Color ink{0.20f, 0.85f, 0.50f, 1.0f};
    std::vector<Vec2> verts = {{-56, 4}, {-16, 44}, {60, -42}};
    std::vector<Vec2> in = {{0, 0}, {-12, 0}, {0, 14}};
    std::vector<Vec2> out = {{0, 0}, {12, 0}, {0, -14}};
    Arr items;
    items.push_back(PathShape("check", verts, in, out, false));
    items.push_back(StrokeShape("checkStroke", ink, 18, 100, 2, 2));
    items.push_back(TrimShape("checkTrim", Static(Num(0)),
                              Keyed(Arr{Key(0, Num(0)), Key(60, Num(100), false, 0.42, 0, 0.58, 1)})));
    items.push_back(RoundedCornersShape("checkRound", Static(Num(10))));
    Arr group{GroupShape("checkGroup", std::move(items))};
    JsonValue ks = LayerTransform({100, 100});
    Arr layers;
    layers.push_back(ShapeLayer(1, "check", std::move(ks), std::move(group), 0, 60));
    JsonValue doc = Composition("checkmark", 200, 200, 60, 0, 60, std::move(layers));
    return DumpDoc(doc);
}

// "pulse": концентрические кольца, расходящиеся и затухающие. 240x240, 60fps, 0..120.
std::string GeneratePulse() {
    Arr layers;
    // Четыре кольца со сдвигом в 15 фреймов, каждое непрерывно расходится на
    // протяжении 2-секундного цикла с двумя импульсами появления/затухания, чтобы рябь не прерывалась.
    const f32 delays[4] = {0.0f, 20.0f, 40.0f, 80.0f};
    const Color colours[4] = {{0.30f, 0.75f, 1.0f, 1.0f},
                              {0.45f, 0.85f, 1.0f, 1.0f},
                              {0.62f, 0.92f, 1.0f, 1.0f},
                              {0.80f, 0.96f, 1.0f, 1.0f}};
    for (int r = 0; r < 4; ++r) {
        f32 base = delays[r];
        f32 end = base + 120.0f;
        Arr items;
        items.push_back(EllipseShape("ring", {40, 40}));
        items.push_back(StrokeShape("ringStroke", colours[r], 8, 100, 2, 2));
        Arr group{GroupShape("ringGroup", std::move(items))};
        JsonValue scale = Keyed(Arr{Key(base, ArrOf({Num(40), Num(40)})),
                                    Key(end, ArrOf({Num(480), Num(480)}))});
        JsonValue opacity =
            Keyed(Arr{Key(base, Num(0)),
                      Key(base + 12, Num(80), false, 0.25, 0, 0.6, 1),
                      Key(base + 60, Num(0)),
                      Key(base + 72, Num(80), false, 0.25, 0, 0.6, 1),
                      Key(end, Num(0))});
        JsonValue ks = LayerTransform({120, 120}, 0, {100, 100}, 100, JsonValue(),
                                      std::move(scale), JsonValue(), std::move(opacity));
        layers.push_back(ShapeLayer(1 + r, "pulse", std::move(ks), std::move(group), 0, 120));
    }
    JsonValue doc = Composition("pulse", 240, 240, 60, 0, 120, std::move(layers));
    return DumpDoc(doc);
}

// "card-flip": скруглённый прямоугольник масштабируется по X с перелётом
// (squash-overshoot) и небольшим поворотом. 300x200, 60fps, фреймы 0..36 (0.6с), для карточной игры.
std::string GenerateCardFlip() {
    const Color front{0.24f, 0.34f, 0.62f, 1.0f};
    // Карта должна оставаться строго внутри композита 300x200 даже в самой
    // широкой фазе (пик сжатия по X + поворот): прямоугольник 220x140 при 104%
    // и повороте на 12 градусов занимает ~x[31..269], оставляя запас с каждой стороны.
    const f32 cardW = 220.0f;
    const f32 cardH = 140.0f;
    Arr items;
    items.push_back(RectShape("card", {cardW, cardH}, 18));
    items.push_back(GradientFillShape("cardFill", {Color{0.30f, 0.42f, 0.78f, 1.0f}, front,
                                                   Color{0.14f, 0.18f, 0.36f, 1.0f}},
                                      {-cardW * 0.5f, -cardH * 0.5f},
                                      {cardW * 0.5f, cardH * 0.5f}));
    Arr group{GroupShape("cardGroup", std::move(items))};

    JsonValue xScale = Keyed(Arr{
        Key(0, ArrOf({Num(0), Num(86)})),
        Key(18, ArrOf({Num(78), Num(112)}), false, 0.2, 0, 0.5, 1),
        Key(30, ArrOf({Num(104), Num(96)}), false, 0.42, 0, 0.58, 1),
        Key(36, ArrOf({Num(100), Num(100)}))});
    JsonValue rot = Keyed(Arr{Key(0, Num(-12), false, 0.2, 0, 0.5, 1), Key(30, Num(4)),
                              Key(36, Num(0))});
    JsonValue ks = LayerTransform({150, 100}, 0, {100, 100}, 100, JsonValue(), std::move(xScale),
                                 std::move(rot));
    Arr layers;
    layers.push_back(ShapeLayer(1, "card", std::move(ks), std::move(group), 0, 36));
    layers.push_back(SolidLayer(2, "floor", LayerTransform({150, 178}), {180, 8},
                                Color{0.10f, 0.13f, 0.20f, 1.0f}, 0, 36));
    JsonValue doc = Composition("card-flip", 300, 200, 60, 0, 36, std::move(layers));
    return DumpDoc(doc);
}

}  // namespace

std::string GenerateLottieJson(const std::string& name) {
    if (name == "loading") return GenerateLoading();
    if (name == "success") return GenerateSuccess();
    if (name == "heart") return GenerateHeart();
    if (name == "checkmark") return GenerateCheckmark();
    if (name == "card-flip") return GenerateCardFlip();
    // Неизвестные имена (и "pulse") откатываются к анимации pulse.
    return GeneratePulse();
}

// ===========================================================================
// LottieLibrary
// ===========================================================================
LottieLibrary& LottieLibrary::Get() {
    static LottieLibrary lib;
    return lib;
}

LottieAnimation* LottieLibrary::Load(const std::string& path) {
    auto it = cache_.find(path);
    if (it != cache_.end()) return it->second;
    auto anim = std::make_unique<LottieAnimation>();
    std::string error;
    if (!anim->LoadFromFile(path, &error)) {
        ENG_LOGW("lottie", "LottieLibrary::Load('%s') failed: %s", path.c_str(), error.c_str());
        return nullptr;
    }
    LottieAnimation* raw = anim.get();
    owned_.push_back(std::move(anim));
    cache_[path] = raw;
    return raw;
}

LottieAnimation* LottieLibrary::LoadOrGenerate(const std::string& name) {
    const std::string key = "gen:" + name;
    auto it = cache_.find(key);
    if (it != cache_.end()) return it->second;

    const std::string json = GenerateLottieJson(name);
    // Сохраняем сгенерированный документ, чтобы пример работал без поставляемых
    // ассетов; сбой записи не фатален (используется копия в памяти).
    std::string dir = PathJoin(GetUserRoot(), "lottie");
    std::string file = PathJoin(dir, name + ".json");
    if (!WriteTextFile(file, json)) {
        ENG_LOGD("lottie", "could not cache generated animation to '%s'", file.c_str());
    }

    auto anim = std::make_unique<LottieAnimation>();
    std::string error;
    if (!anim->LoadFromJson(json, &error)) {
        ENG_LOGE("lottie", "generated animation '%s' failed to parse: %s", name.c_str(),
                 error.c_str());
        return nullptr;
    }
    LottieAnimation* raw = anim.get();
    owned_.push_back(std::move(anim));
    cache_[key] = raw;
    return raw;
}

void LottieLibrary::Clear() {
    cache_.clear();
    owned_.clear();
}

}  // namespace crossrender
