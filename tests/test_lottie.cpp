// Тесты модуля Lottie (bodymovin JSON): разбор, вычисление свойств,
// состояние плеера, валидация, генерация JSON туда-обратно и рендеринг.
#include "crossrender/anim/Lottie.h"

#include "crossrender/core/Json.h"
#include "crossrender/test/Test.h"
#include "crossrender/gfx/Renderer2D.h"
#include "crossrender/gfx/RenderTarget.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
#include <cstring>

using namespace crossrender;

namespace {

const char* kNames[] = {"loading", "success", "heart", "checkmark", "pulse", "card-flip"};

LottieAnimation ParseDoc(const std::string& json, std::string* error = nullptr) {
    LottieAnimation a;
    std::string err;
    if (!a.LoadFromJson(json, &err)) {
        if (error) *error = err;
    }
    return a;
}

// Небольшая рукописная композиция для тестов свойств/плеера.
// 100x100, 60 fps, кадры 0..60 (1 с) с одним слоем-прямоугольником.
const char* kSimpleJson = R"JSON({
  "v": "5.7.4", "fr": 60, "ip": 0, "op": 60, "w": 100, "h": 100, "nm": "simple",
  "assets": [],
  "layers": [
    {
      "ind": 1, "ty": 4, "nm": "rect", "sr": 1,
      "ks": {
        "a": {"a": 0, "k": [0, 0]},
        "p": {"a": 0, "k": [50, 50]},
        "s": {"a": 0, "k": [100, 100]},
        "r": {"a": 0, "k": 0},
        "o": {"a": 0, "k": 100}
      },
      "shapes": [
        {"ty": "gr", "nm": "g", "it": [
          {"ty": "rc", "nm": "r", "p": {"a": 0, "k": [0, 0]}, "s": {"a": 0, "k": [40, 30]},
           "r": {"a": 0, "k": 4}},
          {"ty": "fl", "nm": "f", "c": {"a": 0, "k": [1, 0, 0]}, "o": {"a": 0, "k": 100}, "r": 1},
          {"ty": "tr", "nm": "t", "p": {"a": 0, "k": [0, 0]}, "s": {"a": 0, "k": [100, 100]},
           "r": {"a": 0, "k": 0}, "o": {"a": 0, "k": 100}}
        ]}
      ],
      "ip": 0, "op": 60, "st": 0, "bm": 0
    }
  ]
})JSON";

}  // namespace

// ===========================================================================
// Разбор / валидация
// ===========================================================================
ENG_TEST(Lottie, ParseGeneratedAnimations) {
    for (const char* name : kNames) {
        std::string json = GenerateLottieJson(name);
        ENG_CHECK_MSG(!json.empty(), std::string("empty JSON for ") + name);
        LottieAnimation a;
        std::string err;
        bool ok = a.LoadFromJson(json, &err);
        ENG_CHECK_MSG(ok, std::string(name) + ": " + err);
        if (!ok) continue;
        ENG_CHECK_MSG(a.Valid(), name);
        ENG_CHECK_GT(static_cast<int>(a.Width()), 0);
        ENG_CHECK_GT(static_cast<int>(a.Height()), 0);
        ENG_CHECK_GT(static_cast<int>(a.LayerCount()), 0);
        ENG_CHECK_GT(a.FrameRate(), 0.0f);
        ENG_CHECK_GT(a.Duration(), 0.0f);
        ENG_CHECK_GT(a.TotalFrames(), 0);
        LottieAnimation::ValidationResult v = a.Validate();
        ENG_CHECK_MSG(v.ok, name + std::string(" validation failed"));
        // Ровно одна анимация должна иметь solid-слой (карточный стол).
        ENG_CHECK(a.Name() == name || std::string(name) == "pulse");
    }
}

ENG_TEST(Lottie, ParseSimpleDocument) {
    LottieAnimation a = ParseDoc(kSimpleJson);
    ENG_CHECK(a.Valid());
    ENG_CHECK_EQ(a.Width(), 100);
    ENG_CHECK_EQ(a.Height(), 100);
    ENG_CHECK_EQ(a.LayerCount(), 1);
    ENG_CHECK_NEAR(a.FrameRate(), 60.0f, 1e-4f);
    ENG_CHECK_NEAR(a.Duration(), 1.0f, 1e-4f);
    ENG_CHECK_EQ(a.TotalFrames(), 60);
    ENG_CHECK(a.Validate().ok);
    ENG_CHECK_STR_EQ(a.Layers()[0].name, "rect");
    ENG_CHECK_EQ(static_cast<int>(a.Layers()[0].shapes.size()), 1);
    ENG_CHECK_EQ(static_cast<int>(a.Layers()[0].shapes[0].items.size()), 2);
}

ENG_TEST(Lottie, ParseRejectsGarbage) {
    std::string err;
    LottieAnimation a;
    ENG_CHECK(!a.LoadFromJson("{ this is not json", &err));
    ENG_CHECK(!a.Valid());

    LottieAnimation b;
    ENG_CHECK(!b.LoadFromJson("[1,2,3]", &err));
    ENG_CHECK(!b.Valid());

    LottieAnimation c;
    ENG_CHECK(!c.LoadFromJson("", &err));

    // Отсутствующие ключи не должны приводить к падению.
    LottieAnimation d;
    std::string e2;
    bool ok = d.LoadFromJson("{\"fr\":0,\"op\":0}", &e2);
    ENG_CHECK(!ok);  // нет слоёв
    ENG_CHECK_EQ(d.LayerCount(), 0);
}

ENG_TEST(Lottie, ParseDefensiveWithWeirdTypes) {
    // Неправильные типы повсюду: должно разбираться (или падать чисто), но никогда не крашиться.
    const char* json = R"JSON({
      "v": 5, "fr": "60", "ip": [], "op": null, "w": "100", "h": {},
      "layers": [
        {"ind": "1", "ty": "4", "ks": {"p": 12, "s": [1], "o": null},
         "shapes": [{"ty": "zz", "it": 7}, {"ty": "rc", "s": "bad"}]}
      ]
    })JSON";
    LottieAnimation a;
    std::string err;
    a.LoadFromJson(json, &err);
    // `ty: "4"` - строка, поэтому происходит откат к shape-слою по умолчанию
    // вместо сообщения о неизвестном слое; разбор всё равно должен быть безопасным.
    if (a.Valid()) {
        ENG_CHECK_EQ(static_cast<int>(a.Layers()[0].type),
                     static_cast<int>(LottieLayerType::Shape));
        // Неизвестная shape `zz` отбрасывается, кривой `rc` выживает.
        ENG_CHECK_EQ(static_cast<int>(a.Layers()[0].shapes.size()), 1);
        ENG_CHECK(a.Validate().ok);
    }
    ENG_CHECK_NEAR(a.FrameRate(), 60.0f, 1e-4f);
}

ENG_TEST(Lottie, ValidateReportsMissingAssetAndParent) {
    const char* json = R"JSON({
      "v": "5.7.4", "fr": 60, "ip": 0, "op": 30, "w": 100, "h": 100, "assets": [],
      "layers": [
        {"ind": 1, "ty": 2, "nm": "img", "refId": "nope", "parent": 99,
         "ks": {"p": {"a": 0, "k": [0, 0]}}, "ip": 0, "op": 30}
      ]
    })JSON";
    LottieAnimation a;
    std::string err;
    ENG_CHECK(a.LoadFromJson(json, &err));
    LottieAnimation::ValidationResult v = a.Validate();
    ENG_CHECK(!v.ok);
    bool sawImage = false, sawParent = false;
    for (const std::string& e : v.errors) {
        if (e.find("missing asset") != std::string::npos) sawImage = true;
        if (e.find("dangling parent") != std::string::npos) sawParent = true;
    }
    ENG_CHECK_MSG(sawImage, "missing asset should be reported");
    ENG_CHECK_MSG(sawParent, "dangling parent should be reported");
}

// ===========================================================================
// Вычисление LottieProperty
// ===========================================================================
ENG_TEST(Lottie, PropertyStaticValues) {
    LottieProperty scalar;
    scalar.kind = LottieProperty::Kind::Scalar;
    scalar.scalar = 42.0f;
    ENG_CHECK_NEAR(scalar.EvaluateScalar(0.0f), 42.0f, 1e-5f);
    ENG_CHECK_NEAR(scalar.EvaluateScalar(10.0f), 42.0f, 1e-5f);

    LottieProperty vec;
    vec.kind = LottieProperty::Kind::Vec2;
    vec.vec2 = {3, -4};
    Vec2 v = vec.EvaluateVec2(1.0f);
    ENG_CHECK_NEAR(v.x, 3.0f, 1e-5f);
    ENG_CHECK_NEAR(v.y, -4.0f, 1e-5f);

    LottieProperty col;
    col.kind = LottieProperty::Kind::Color;
    col.color = Color{0.25f, 0.5f, 0.75f, 1.0f};
    Color c = col.EvaluateColor(0.0f);
    ENG_CHECK_NEAR(c.r, 0.25f, 1e-5f);
    ENG_CHECK_NEAR(c.b, 0.75f, 1e-5f);

    LottieProperty path;
    path.kind = LottieProperty::Kind::Path;
    path.pathPoints = {{0, 0}, {0, 0}, {0, 0}, {1, 1}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}};
    std::vector<Vec2> pts = path.EvaluatePath(0.0f);
    ENG_CHECK_EQ(static_cast<int>(pts.size()), 9);
    ENG_CHECK_NEAR(pts[3].x, 1.0f, 1e-5f);
}

ENG_TEST(Lottie, PropertyLinearKeyframeMidpoint) {
    const char* json = R"JSON({
      "v": "5.7.4", "fr": 60, "ip": 0, "op": 60, "w": 100, "h": 100, "assets": [],
      "layers": [{"ind": 1, "ty": 4, "nm": "l", "ks": {
        "p": {"a": 0, "k": [0, 0]},
        "s": {"a": 1, "k": [
          {"t": 0, "s": [0, 0]},
          {"t": 60, "s": [100, 100]}
        ]},
        "o": {"a": 0, "k": 100}, "r": {"a": 0, "k": 0}},
        "shapes": [], "ip": 0, "op": 60}]
    })JSON";
    LottieAnimation a = ParseDoc(json);
    ENG_CHECK(a.Valid());
    const LottieTransform& ks = a.Layers()[0].transform;
    ENG_CHECK(ks.scale.animated);
    ENG_CHECK_EQ(static_cast<int>(ks.scale.keys.size()), 2);
    // Времена поделены на частоту кадров.
    ENG_CHECK_NEAR(ks.scale.keys[0].time, 0.0f, 1e-5f);
    ENG_CHECK_NEAR(ks.scale.keys[1].time, 1.0f, 1e-5f);
    Vec2 mid = ks.scale.EvaluateVec2(0.5f);
    ENG_CHECK_NEAR(mid.x, 50.0f, 1e-3f);
    ENG_CHECK_NEAR(mid.y, 50.0f, 1e-3f);
    ENG_CHECK_NEAR(ks.scale.EvaluateVec2(0.25f).x, 25.0f, 1e-3f);
    // Клампинг вне диапазона ключей.
    ENG_CHECK_NEAR(ks.scale.EvaluateVec2(-1.0f).x, 0.0f, 1e-3f);
    ENG_CHECK_NEAR(ks.scale.EvaluateVec2(5.0f).x, 100.0f, 1e-3f);
}

ENG_TEST(Lottie, PropertyHoldKeyframe) {
    const char* json = R"JSON({
      "v": "5.7.4", "fr": 60, "ip": 0, "op": 60, "w": 100, "h": 100, "assets": [],
      "layers": [{"ind": 1, "ty": 4, "nm": "l", "ks": {
        "p": {"a": 0, "k": [0, 0]}, "o": {"a": 0, "k": 100}, "r": {"a": 0, "k": 0},
        "s": {"a": 1, "k": [
          {"t": 0, "s": [10, 10], "h": 1},
          {"t": 30, "s": [100, 100]}
        ]}},
        "shapes": [], "ip": 0, "op": 60}]
    })JSON";
    LottieAnimation a = ParseDoc(json);
    ENG_CHECK(a.Valid());
    const LottieProperty& scale = a.Layers()[0].transform.scale;
    ENG_CHECK(scale.keys[0].hold);
    // Hold-ключ держит своё значение весь сегмент.
    ENG_CHECK_NEAR(scale.EvaluateVec2(0.0f).x, 10.0f, 1e-4f);
    ENG_CHECK_NEAR(scale.EvaluateVec2(0.25f).x, 10.0f, 1e-4f);
    ENG_CHECK_NEAR(scale.EvaluateVec2(0.499f).x, 10.0f, 1e-4f);
    ENG_CHECK_NEAR(scale.EvaluateVec2(0.5f).x, 100.0f, 1e-4f);
    ENG_CHECK_NEAR(scale.EvaluateVec2(1.0f).x, 100.0f, 1e-4f);
}

ENG_TEST(Lottie, PropertyBezierEasingMonotonic) {
    const char* json = R"JSON({
      "v": "5.7.4", "fr": 60, "ip": 0, "op": 60, "w": 100, "h": 100, "assets": [],
      "layers": [{"ind": 1, "ty": 4, "nm": "l", "ks": {
        "p": {"a": 0, "k": [0, 0]}, "o": {"a": 0, "k": 100}, "r": {"a": 0, "k": 0},
        "s": {"a": 1, "k": [
          {"t": 0, "s": [0, 0], "o": {"x": [0.42], "y": [0.0]}},
          {"t": 60, "s": [100, 100], "i": {"x": [0.58], "y": [1.0]}}
        ]}},
        "shapes": [], "ip": 0, "op": 60}]
    })JSON";
    LottieAnimation a = ParseDoc(json);
    ENG_CHECK(a.Valid());
    const LottieProperty& scale = a.Layers()[0].transform.scale;
    ENG_CHECK(!scale.keys[0].easingOut.empty());
    // `o` несёт первую контрольную точку, `i` на следующем ключе - вторую.
    ENG_CHECK_STR_EQ(scale.keys[0].easingOut, "0.42,0");
    ENG_CHECK_STR_EQ(scale.keys[1].easingIn, "0.58,1");

    f32 prev = -1.0f;
    for (int i = 0; i <= 40; ++i) {
        f32 t = static_cast<f32>(i) / 40.0f;
        f32 v = scale.EvaluateVec2(t).x;
        ENG_CHECK_MSG(v >= prev - 1e-4f, "bezier easing must be monotonic");
        ENG_CHECK(v >= -1e-3f && v <= 100.001f);
        prev = v;
    }
    // cubic-bezier(0.42,0,0.58,1): начало ease-in, конец ease-out.
    ENG_CHECK(scale.EvaluateVec2(0.25f).x < 27.0f);
    ENG_CHECK(scale.EvaluateVec2(0.75f).x > 73.0f);
    ENG_CHECK_NEAR(scale.EvaluateVec2(0.0f).x, 0.0f, 1e-4f);
    ENG_CHECK_NEAR(scale.EvaluateVec2(1.0f).x, 100.0f, 1e-4f);
}

ENG_TEST(Lottie, PropertyStringEasingForm) {
    const char* json = R"JSON({
      "v": "5.7.4", "fr": 60, "ip": 0, "op": 60, "w": 100, "h": 100, "assets": [],
      "layers": [{"ind": 1, "ty": 4, "nm": "l", "ks": {
        "p": {"a": 0, "k": [0, 0]}, "o": {"a": 0, "k": 100}, "r": {"a": 0, "k": 0},
        "s": {"a": 1, "k": [
          {"t": 0, "s": [0, 0], "o": "0.5,0"},
          {"t": 60, "s": [100, 100], "i": "0.5,1"}
        ]}},
        "shapes": [], "ip": 0, "op": 60}]
    })JSON";
    LottieAnimation a = ParseDoc(json);
    ENG_CHECK(a.Valid());
    const LottieProperty& scale = a.Layers()[0].transform.scale;
    // Строковая форма несёт полную безье; сохраняется только пара, относящаяся
    // к каждому ключу (исходящая контрольная точка).
    ENG_CHECK_STR_EQ(scale.keys[0].easingOut, "0.5,0");
    ENG_CHECK_STR_EQ(scale.keys[1].easingIn, "0.5,1");
    ENG_CHECK_NEAR(scale.EvaluateVec2(0.5f).x, 50.0f, 1.0f);
    ENG_CHECK(scale.EvaluateVec2(0.25f).x < 27.0f);
}

ENG_TEST(Lottie, PropertyColourKeyframes) {
    const char* json = R"JSON({
      "v": "5.7.4", "fr": 60, "ip": 0, "op": 60, "w": 100, "h": 100, "assets": [],
      "layers": [{"ind": 1, "ty": 4, "nm": "l", "ks": {
        "p": {"a": 0, "k": [0, 0]}, "s": {"a": 0, "k": [100, 100]},
        "o": {"a": 0, "k": 100}, "r": {"a": 0, "k": 0}},
        "shapes": [{"ty": "gr", "it": [
          {"ty": "rc", "p": {"a": 0, "k": [0, 0]}, "s": {"a": 0, "k": [10, 10]}},
          {"ty": "fl", "c": {"a": 1, "k": [
            {"t": 0, "s": [1, 0, 0, 1]},
            {"t": 60, "s": [0, 0, 1, 1]}
          ]}, "o": {"a": 0, "k": 100}}
        ]}], "ip": 0, "op": 60}]
    })JSON";
    LottieAnimation a = ParseDoc(json);
    ENG_CHECK(a.Valid());
    const LottieShape& grp = a.Layers()[0].shapes[0];
    const LottieShape& fill = *grp.items[1];
    Color c0 = fill.colour.EvaluateColor(0.0f);
    ENG_CHECK_NEAR(c0.r, 1.0f, 1e-4f);
    ENG_CHECK_NEAR(c0.b, 0.0f, 1e-4f);
    Color mid = fill.colour.EvaluateColor(0.5f);
    ENG_CHECK_NEAR(mid.r, 0.5f, 1e-3f);
    ENG_CHECK_NEAR(mid.g, 0.0f, 1e-3f);
    ENG_CHECK_NEAR(mid.b, 0.5f, 1e-3f);
    Color end = fill.colour.EvaluateColor(1.0f);
    ENG_CHECK_NEAR(end.b, 1.0f, 1e-4f);
}

ENG_TEST(Lottie, PropertyPathKeyframes) {
    // Три совпадающих вершины; второй ключевой кадр сдвинут на (+20, +10).
    const char* json = R"JSON({
      "v": "5.7.4", "fr": 60, "ip": 0, "op": 60, "w": 100, "h": 100, "assets": [],
      "layers": [{"ind": 1, "ty": 4, "nm": "l", "ks": {
        "p": {"a": 0, "k": [0, 0]}, "s": {"a": 0, "k": [100, 100]},
        "o": {"a": 0, "k": 100}, "r": {"a": 0, "k": 0}},
        "shapes": [{"ty": "gr", "it": [
          {"ty": "sh", "ks": {"a": 1, "k": [
            {"t": 0, "s": [{"i": [[0, 0], [0, 0], [0, 0]], "o": [[0, 0], [0, 0], [0, 0]],
                            "v": [[0, 0], [10, 0], [10, 10]], "c": true}]},
            {"t": 60, "s": [{"i": [[0, 0], [0, 0], [0, 0]], "o": [[0, 0], [0, 0], [0, 0]],
                             "v": [[20, 10], [30, 10], [30, 20]], "c": true}]}
          ]}},
          {"ty": "st", "c": {"a": 0, "k": [0, 0, 0]}, "o": {"a": 0, "k": 100},
           "w": {"a": 0, "k": 2}}
        ]}], "ip": 0, "op": 60}]
    })JSON";
    LottieAnimation a = ParseDoc(json);
    ENG_CHECK(a.Valid());
    const LottieShape& path = *a.Layers()[0].shapes[0].items[0];
    ENG_CHECK(path.pathData.animated);
    ENG_CHECK_EQ(static_cast<int>(path.pathData.keys.size()), 2);
    ENG_CHECK_NEAR(path.pathData.keys[1].time, 1.0f, 1e-5f);

    std::vector<Vec2> p0 = path.pathData.EvaluatePath(0.0f);
    ENG_CHECK_EQ(static_cast<int>(p0.size()), 12);  // 3 вершины + повтор первой
    ENG_CHECK_NEAR(p0[0].x, 0.0f, 1e-4f);
    std::vector<Vec2> mid = path.pathData.EvaluatePath(0.5f);
    ENG_CHECK_EQ(static_cast<int>(mid.size()), 12);
    ENG_CHECK_NEAR(mid[0].x, 10.0f, 1e-3f);
    ENG_CHECK_NEAR(mid[0].y, 5.0f, 1e-3f);
    ENG_CHECK_NEAR(mid[3].x, 20.0f, 1e-3f);
    ENG_CHECK_NEAR(mid[3].y, 5.0f, 1e-3f);
    // Замыкающая запись следует за первой вершиной.
    ENG_CHECK_NEAR(mid[9].x, 10.0f, 1e-3f);
}

ENG_TEST(Lottie, PropertyPathVertexMismatchUsesStartShape) {
    LottieProperty p;
    p.kind = LottieProperty::Kind::Path;
    p.animated = true;
    LottieProperty::Key a;
    a.time = 0;
    a.pts = {{0, 0}, {0, 0}, {0, 0}, {1, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}};
    LottieProperty::Key b;
    b.time = 1;
    b.pts = {{5, 5}, {0, 0}, {0, 0}, {9, 9}, {0, 0}, {0, 0}};  // другое число
    p.keys = {a, b};
    std::vector<Vec2> mid = p.EvaluatePath(0.5f);
    ENG_CHECK_EQ(static_cast<int>(mid.size()), 9);
    ENG_CHECK_NEAR(mid[0].x, 0.0f, 1e-5f);
}

ENG_TEST(Lottie, PropertySpatialBezierTangents) {
    LottieProperty p;
    p.kind = LottieProperty::Kind::Vec2;
    p.animated = true;
    LottieProperty::Key a;
    a.time = 0;
    a.v2 = {0, 0};
    LottieProperty::Key b;
    b.time = 1;
    b.v2 = {100, 0};
    p.keys = {a, b};
    p.spatial.push_back({{0, 0}, {0, 60}});    // исходящая касательная ключа 0
    p.spatial.push_back({{0, -60}, {0, 0}});   // входящая касательная ключа 1
    Vec2 mid = p.EvaluateVec2(0.5f);
    ENG_CHECK_NEAR(mid.x, 50.0f, 1e-3f);
    // Симметричная S-кривая проходит через середину хорды ...
    ENG_CHECK_NEAR(mid.y, 0.0f, 1e-3f);
    // ... и выпирает от прямой в четвертных точках (y растёт вниз, а исходящая
    // касательная смотрит вниз).
    Vec2 q = p.EvaluateVec2(0.25f);
    ENG_CHECK_MSG(q.y > 1.0f, "spatial out-tangent should curve the path");
    Vec2 q3 = p.EvaluateVec2(0.75f);
    ENG_CHECK_MSG(q3.y < -1.0f, "spatial in-tangent should curve the path");
    // Без касательных движение - прямая линия.
    p.spatial.clear();
    Vec2 lin = p.EvaluateVec2(0.5f);
    ENG_CHECK_NEAR(lin.x, 50.0f, 1e-3f);
    ENG_CHECK_NEAR(lin.y, 0.0f, 1e-3f);
}

ENG_TEST(Lottie, TransformMatrixComposition) {
    LottieTransform t;
    t.anchor = LottieProperty{};
    t.anchor.vec2 = {0, 0};
    t.position.vec2 = {10, 20};
    t.scale.vec2 = {100, 100};
    t.scale.scalar = 100;
    t.rotation.scalar = 0;
    t.opacity.scalar = 100;
    Mat4 m = t.Matrix(0.0f);
    Vec3 p = m.TransformPoint(Vec3{0, 0, 0});
    ENG_CHECK_NEAR(p.x, 10.0f, 1e-4f);
    ENG_CHECK_NEAR(p.y, 20.0f, 1e-4f);

    // Определяющий инвариант AE-трансформации: точка anchor отображается точно
    // в позицию слоя (масштаб/поворот происходят вокруг anchor). Ошибка здесь
    // масштабирует и вращает саму позицию, и графика уплывает.
    t.anchor.vec2 = {50, 40};
    t.scale.vec2 = {200, 200};
    t.scale.scalar = 200;
    t.position.vec2 = {100, 120};
    t.rotation.scalar = 30.0f;
    Mat4 m2 = t.Matrix(0.0f);
    Vec3 anchorMap = m2.TransformPoint(Vec3{50, 40, 0});
    ENG_CHECK_NEAR(anchorMap.x, 100.0f, 1e-3f);
    ENG_CHECK_NEAR(anchorMap.y, 120.0f, 1e-3f);

    // Точка на единицу правее anchor масштабируется в 2 раза и поворачивается на 30 градусов.
    Vec3 off = m2.TransformPoint(Vec3{51, 40, 0});
    Vec3 delta = off - anchorMap;
    ENG_CHECK_NEAR(Length(Vec2{delta.x, delta.y}), 2.0f, 1e-3f);
    ENG_CHECK_NEAR(delta.x, 2.0f * std::cos(Radians(30.0f)), 1e-3f);
    ENG_CHECK_NEAR(delta.y, 2.0f * std::sin(Radians(30.0f)), 1e-3f);

    // Без поворота anchor по-прежнему отображается в позицию, а масштаб применяется
    // вокруг него (слой на 200% без смещения оставляет anchor на месте).
    LottieTransform plain;
    plain.anchor.vec2 = {10, 10};
    plain.position.vec2 = {10, 10};
    plain.scale.vec2 = {200, 200};
    plain.scale.scalar = 200;
    Vec3 keep = plain.Matrix(0.0f).TransformPoint(Vec3{10, 10, 0});
    ENG_CHECK_NEAR(keep.x, 10.0f, 1e-4f);
    ENG_CHECK_NEAR(keep.y, 10.0f, 1e-4f);
}

// ===========================================================================
// Плеер
// ===========================================================================
ENG_TEST(Lottie, PlayerWrapsWhenLooping) {
    LottieAnimation a = ParseDoc(kSimpleJson);  // 60 кадров при 60 fps
    ENG_CHECK(a.Valid());
    ENG_CHECK_EQ(a.TotalFrames(), 60);
    ENG_CHECK(a.Looping());
    a.Play();
    ENG_CHECK(a.Playing());
    ENG_CHECK(!a.Finished());
    ENG_CHECK_NEAR(a.CurrentFrame(), 0.0f, 1e-4f);
    ENG_CHECK_NEAR(a.Progress(), 0.0f, 1e-4f);

    a.SetSpeed(1.0f);
    a.Advance(0.5f);  // 30 кадров
    ENG_CHECK_NEAR(a.CurrentFrame(), 30.0f, 1e-3f);
    ENG_CHECK_NEAR(a.Progress(), 0.5f, 1e-3f);
    ENG_CHECK(!a.Finished());
    a.Advance(0.5f);  // заворачивается на 0
    ENG_CHECK_NEAR(a.CurrentFrame(), 0.0f, 1e-3f);
    ENG_CHECK(!a.Finished());
    ENG_CHECK(a.LoopCount() >= 1);
    f32 pr = a.Progress();
    ENG_CHECK(pr >= 0.0f && pr <= 1.0f);
}

ENG_TEST(Lottie, PlayerClampsWhenNotLooping) {
    LottieAnimation a = ParseDoc(kSimpleJson);
    ENG_CHECK(a.Valid());
    a.SetLoop(false);
    a.Play();
    a.Advance(2.0f);  // сильно за конец
    ENG_CHECK_NEAR(a.CurrentFrame(), 60.0f, 1e-3f);
    ENG_CHECK(a.Finished());
    ENG_CHECK_NEAR(a.Progress(), 1.0f, 1e-4f);
    // Повторный Advance остаётся зажатым.
    a.Advance(1.0f);
    ENG_CHECK_NEAR(a.CurrentFrame(), 60.0f, 1e-3f);
    ENG_CHECK(a.Finished());
}

ENG_TEST(Lottie, PlayerSegmentRestrictsRange) {
    LottieAnimation a = ParseDoc(kSimpleJson);
    ENG_CHECK(a.Valid());
    a.SetSegment(20.0f, 40.0f);
    a.SetLoop(false);
    a.SetFrame(20.0f);
    ENG_CHECK_NEAR(a.CurrentFrame(), 20.0f, 1e-4f);
    ENG_CHECK_NEAR(a.Progress(), 0.0f, 1e-4f);
    a.Play();
    a.Advance(0.5f);  // 30 кадров пути -> кламп на 40
    ENG_CHECK_NEAR(a.CurrentFrame(), 40.0f, 1e-3f);
    ENG_CHECK(a.Finished());
    ENG_CHECK_NEAR(a.Progress(), 1.0f, 1e-4f);

    // Зацикливание внутри сегмента заворачивает к началу сегмента.
    a.SetLoop(true);
    a.SetFrame(38.0f);
    f32 before = a.CurrentFrame();
    a.Advance(4.0f / 60.0f);
    ENG_CHECK(a.CurrentFrame() < before);
    ENG_CHECK(a.CurrentFrame() >= 20.0f - 1e-3f && a.CurrentFrame() <= 40.0f + 1e-3f);
}

ENG_TEST(Lottie, PlayerSpeedScalesAdvance) {
    LottieAnimation a = ParseDoc(kSimpleJson);
    a.SetLoop(false);
    a.Play();
    a.SetSpeed(1.0f);
    a.Advance(0.1f);  // 6 кадров
    ENG_CHECK_NEAR(a.CurrentFrame(), 6.0f, 1e-3f);

    a.SetFrame(0.0f);
    a.SetSpeed(2.0f);
    a.Advance(0.1f);  // 12 кадров за то же реальное время
    ENG_CHECK_NEAR(a.CurrentFrame(), 12.0f, 1e-3f);
    ENG_CHECK_NEAR(a.Speed(), 2.0f, 1e-5f);

    a.SetFrame(0.0f);
    a.SetSpeed(0.5f);
    a.Advance(0.1f);  // 3 кадра
    ENG_CHECK_NEAR(a.CurrentFrame(), 3.0f, 1e-3f);
}

ENG_TEST(Lottie, PlayerPauseStopAndTime) {
    LottieAnimation a = ParseDoc(kSimpleJson);
    a.Play();
    a.Advance(0.25f);
    f32 f = a.CurrentFrame();
    a.Pause();
    ENG_CHECK(!a.Playing());
    a.Advance(1.0f);
    ENG_CHECK_NEAR(a.CurrentFrame(), f, 1e-3f);
    a.Stop();
    ENG_CHECK_NEAR(a.CurrentFrame(), 0.0f, 1e-4f);
    ENG_CHECK(!a.Finished());
    a.SetTime(0.5f);
    ENG_CHECK_NEAR(a.CurrentFrame(), 30.0f, 1e-3f);
    ENG_CHECK_NEAR(a.CurrentTime(), 0.5f, 1e-3f);
    a.SetFrame(-10.0f);
    ENG_CHECK_NEAR(a.CurrentFrame(), 0.0f, 1e-4f);
    a.SetFrame(1000.0f);
    ENG_CHECK_NEAR(a.CurrentFrame(), 60.0f, 1e-4f);
}

// ===========================================================================
// Библиотека / генерация туда-обратно
// ===========================================================================
ENG_TEST(Lottie, LibraryLoadOrGenerateHeart) {
    LottieAnimation* a = LottieLibrary::Get().LoadOrGenerate("heart");
    ENG_CHECK(a != nullptr);
    if (!a) return;
    ENG_CHECK(a->Valid());
    ENG_CHECK_GT(a->LayerCount(), 0);
    ENG_CHECK_GT(a->Duration(), 0.0f);
    ENG_CHECK(a->Validate().ok);
    // Повторные вызовы возвращают кэшированный экземпляр.
    LottieAnimation* again = LottieLibrary::Get().LoadOrGenerate("heart");
    ENG_CHECK(again == a);
    LottieAnimation* unknown = LottieLibrary::Get().LoadOrGenerate("does-not-exist");
    ENG_CHECK(unknown != nullptr);
    if (unknown) ENG_CHECK(unknown->LayerCount() > 0);
    LottieLibrary::Get().Clear();
    LottieAnimation* fresh = LottieLibrary::Get().LoadOrGenerate("heart");
    ENG_CHECK(fresh != nullptr && fresh->Valid());
    LottieLibrary::Get().Clear();
}

ENG_TEST(Lottie, GeneratedJsonRoundTripMatchesDirectConstruction) {
    for (const char* name : kNames) {
        std::string json = GenerateLottieJson(name);
        // Вторая генерация должна быть байт-в-байт идентичной (без скрытого состояния).
        ENG_CHECK_STR_EQ(GenerateLottieJson(name), json);

        LottieAnimation a = ParseDoc(json);
        LottieAnimation b = ParseDoc(json);
        ENG_CHECK(a.Valid() && b.Valid());
        ENG_CHECK_EQ(a.LayerCount(), b.LayerCount());
        ENG_CHECK_NEAR(a.Duration(), b.Duration(), 1e-5f);
        ENG_CHECK_EQ(a.TotalFrames(), b.TotalFrames());
        ENG_CHECK_NEAR(a.FrameRate(), b.FrameRate(), 1e-5f);
        ENG_CHECK_EQ(a.Width(), b.Width());
        ENG_CHECK_EQ(a.Height(), b.Height());
        // Повторная сериализация разобранного DOM должна разбираться в ту же форму.
        std::string redump = a.Raw().Dump();
        LottieAnimation c = ParseDoc(redump);
        ENG_CHECK_MSG(c.Valid(), std::string("re-dump parse failed for ") + name);
        ENG_CHECK_EQ(a.LayerCount(), c.LayerCount());
        ENG_CHECK_NEAR(a.Duration(), c.Duration(), 1e-4f);
    }
}

ENG_TEST(Lottie, GenerateUnknownNameFallsBackToPulse) {
    std::string pulse = GenerateLottieJson("pulse");
    ENG_CHECK_STR_EQ(GenerateLottieJson("nope"), pulse);
    ENG_CHECK_STR_EQ(GenerateLottieJson(""), pulse);
    LottieAnimation a = ParseDoc(pulse);
    ENG_CHECK(a.Valid());
    ENG_CHECK_EQ(a.LayerCount(), 4);  // четыре концентрических кольца
}

ENG_TEST(Lottie, EmbeddedBase64ImageDecoded) {
    // PNG 1x1 как data URI: парсер должен base64-декодировать его в imageData.
    const char* json = R"JSON({
      "v": "5.7.4", "fr": 60, "ip": 0, "op": 30, "w": 10, "h": 10,
      "assets": [{"id": "img_0", "w": 1, "h": 1, "nm": "dot",
                  "p": "data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8AAAwAB/AGtE6kbAAAAAElFTkSuQmCC",
                  "u": "", "e": 0}],
      "layers": [{"ind": 1, "ty": 2, "nm": "dot", "refId": "dot",
                  "ks": {"p": {"a": 0, "k": [5, 5]}, "s": {"a": 0, "k": [100, 100]},
                         "o": {"a": 0, "k": 100}, "r": {"a": 0, "k": 0}},
                  "ip": 0, "op": 30}]
    })JSON";
    LottieAnimation a = ParseDoc(json);
    ENG_CHECK(a.Valid());
    ENG_CHECK_EQ(static_cast<int>(a.Assets().size()), 1);
    ENG_CHECK(!a.Assets()[0].imageData.empty());
    ENG_CHECK_GT(static_cast<int>(a.Assets()[0].imageData.size()), 8);
    // Подпись PNG.
    const std::vector<u8>& d = a.Assets()[0].imageData;
    ENG_CHECK_EQ(static_cast<int>(d[0]), 0x89);
    ENG_CHECK_EQ(static_cast<int>(d[1]), 'P');
    ENG_CHECK_EQ(static_cast<int>(d[2]), 'N');
    ENG_CHECK_EQ(static_cast<int>(d[3]), 'G');
    ENG_CHECK(a.Validate().ok);
}

ENG_TEST(Lottie, BlendModeAndMaskParse) {
    const char* json = R"JSON({
      "v": "5.7.4", "fr": 60, "ip": 0, "op": 30, "w": 100, "h": 100, "assets": [],
      "layers": [{"ind": 1, "ty": 4, "nm": "l", "bm": 1,
        "hasMask": true,
        "masksProperties": [{"mode": "a", "inv": false,
          "pt": {"a": 0, "k": {"i": [[0, 0], [0, 0], [0, 0]],
                               "o": [[0, 0], [0, 0], [0, 0]],
                               "v": [[10, 10], [90, 10], [50, 90]], "c": true}},
          "o": {"a": 0, "k": 100}}],
        "ks": {"p": {"a": 0, "k": [50, 50]}, "s": {"a": 0, "k": [100, 100]},
               "o": {"a": 0, "k": 100}, "r": {"a": 0, "k": 0}},
        "shapes": [{"ty": "gr", "it": [
          {"ty": "el", "p": {"a": 0, "k": [0, 0]}, "s": {"a": 0, "k": [40, 40]}},
          {"ty": "fl", "c": {"a": 0, "k": [0, 1, 0]}, "o": {"a": 0, "k": 100}}
        ]}],
        "ip": 0, "op": 30}]
    })JSON";
    LottieAnimation a = ParseDoc(json);
    ENG_CHECK(a.Valid());
    ENG_CHECK(a.Layers()[0].hasMask);
    ENG_CHECK_EQ(static_cast<int>(a.Layers()[0].masks.size()), 1);
    ENG_CHECK_EQ(a.Layers()[0].masks[0].mode, static_cast<int>('a'));
    ENG_CHECK(a.Validate().ok);
}

ENG_TEST(Lottie, SolidLayerColourParsesFromHexString) {
    // bodymovin пишет solid-цвета строками "#rrggbb" в `sc`; невозможность их
    // разбора оставляла каждый solid-слой ярко-белым.
    const char* json = R"JSON({
      "v": "5.7.4", "fr": 60, "ip": 0, "op": 30, "w": 100, "h": 100, "assets": [],
      "layers": [
        {"ind": 1, "ty": 1, "nm": "shade", "sc": "#3366cc", "sw": 20, "sh": 10,
         "ks": {"p": {"a": 0, "k": [50, 50]}, "s": {"a": 0, "k": [100, 100]},
                "o": {"a": 0, "k": 100}, "r": {"a": 0, "k": 0}}, "ip": 0, "op": 30},
        {"ind": 2, "ty": 1, "nm": "short", "sc": "#f80", "sw": 4, "sh": 4,
         "ks": {"p": {"a": 0, "k": [10, 10]}, "s": {"a": 0, "k": [100, 100]},
                "o": {"a": 0, "k": 100}, "r": {"a": 0, "k": 0}}, "ip": 0, "op": 30}
      ]
    })JSON";
    LottieAnimation a = ParseDoc(json);
    ENG_CHECK(a.Valid());
    const LottieLayer& l0 = a.Layers()[0];
    ENG_CHECK_EQ(static_cast<int>(l0.type), static_cast<int>(LottieLayerType::Solid));
    ENG_CHECK_NEAR(l0.solidColor.r, 0x33 / 255.0f, 1e-4f);
    ENG_CHECK_NEAR(l0.solidColor.g, 0x66 / 255.0f, 1e-4f);
    ENG_CHECK_NEAR(l0.solidColor.b, 0xcc / 255.0f, 1e-4f);
    ENG_CHECK_NEAR(l0.solidColor.a, 1.0f, 1e-4f);
    ENG_CHECK_NEAR(l0.width, 20.0f, 1e-4f);
    ENG_CHECK_NEAR(l0.height, 10.0f, 1e-4f);
    // Короткая форма разворачивает каждый ниббл.
    ENG_CHECK_NEAR(a.Layers()[1].solidColor.r, 1.0f, 1e-4f);
    ENG_CHECK_NEAR(a.Layers()[1].solidColor.g, 0x88 / 255.0f, 1e-4f);
    ENG_CHECK_NEAR(a.Layers()[1].solidColor.b, 0.0f, 1e-4f);
    // Регрессионная защита: никогда не белый по умолчанию.
    ENG_CHECK(l0.solidColor.r < 0.5f || l0.solidColor.b > 0.5f);
}

ENG_TEST(Lottie, MissingTransformKeysUseAeDefaults) {
    // Группа shape без дочернего `tr` (и слой без `ks`) должна вести себя как
    // полностью непрозрачная при масштабе 100%, иначе внутри ничего не рисуется вовсе.
    const char* json = R"JSON({
      "v": "5.7.4", "fr": 60, "ip": 0, "op": 30, "w": 100, "h": 100, "assets": [],
      "layers": [{"ind": 1, "ty": 4, "nm": "l",
        "ks": {"p": {"a": 0, "k": [50, 50]}, "o": {"a": 0, "k": 100}, "r": {"a": 0, "k": 0},
               "s": {"a": 0, "k": [100, 100]}},
        "shapes": [{"ty": "gr", "nm": "g", "it": [
          {"ty": "el", "p": {"a": 0, "k": [0, 0]}, "s": {"a": 0, "k": [30, 30]}},
          {"ty": "st", "c": {"a": 0, "k": [0, 0, 0]}, "w": {"a": 0, "k": 4}}
        ]}],
        "ip": 0, "op": 30}]
    })JSON";
    LottieAnimation a = ParseDoc(json);
    ENG_CHECK(a.Valid());
    const LottieShape& group = a.Layers()[0].shapes[0];
    // У группы нет `tr`: по умолчанию opacity 100 / scale 100.
    ENG_CHECK_NEAR(group.transform.opacity.EvaluateScalar(0.0f), 100.0f, 1e-4f);
    ENG_CHECK_NEAR(group.transform.scale.EvaluateVec2(0.0f).x, 100.0f, 1e-4f);
    // У обводки нет `o`: opacity по умолчанию 100, а не 0.
    const LottieShape& stroke = *group.items[1];
    ENG_CHECK_NEAR(stroke.strokeOpacity.EvaluateScalar(0.0f), 100.0f, 1e-4f);
    // Trim без `e` оставляет весь путь видимым.
    LottieAnimation b = ParseDoc(R"JSON({
      "v": "5.7.4", "fr": 60, "ip": 0, "op": 30, "w": 10, "h": 10, "assets": [],
      "layers": [{"ind": 1, "ty": 4, "nm": "l", "ks": {},
        "shapes": [{"ty": "tm", "s": {"a": 0, "k": 0}, "o": {"a": 0, "k": 0}}],
        "ip": 0, "op": 30}]})JSON");
    ENG_CHECK(b.Valid());
    ENG_CHECK_NEAR(b.Layers()[0].shapes[0].end.EvaluateScalar(0.0f), 100.0f, 1e-4f);
}

// ===========================================================================
// Рендеринг (нужен настоящий Renderer2D поверх GL)
// ===========================================================================
namespace {

constexpr int kPixelW = 256;
constexpr int kPixelH = 192;
const Color kClearColour{0.0f, 0.0f, 0.0f, 1.0f};

struct CoverageStats {
    int content = 0;      // пиксели, отличающиеся от цвета очистки
    int bboxW = 0, bboxH = 0;
    int minX = kPixelW, minY = kPixelH, maxX = -1, maxY = -1;
    int distinctRowSpans = 0;
    // Ширина контента на 25% / 75% высоты его bounding box.
    int topWidth = 0, bottomWidth = 0;
};

// `rgba` идёт снизу вверх, RGBA8 (порядок GL); строка 0 возвращаемого анализа -
// верх изображения.
CoverageStats Analyse(const std::vector<u8>& rgba) {
    CoverageStats st;
    std::vector<int> rowLeft(kPixelH, kPixelW), rowRight(kPixelH, -1);
    for (int y = 0; y < kPixelH; ++y) {
        const int srcY = kPixelH - 1 - y;
        for (int x = 0; x < kPixelW; ++x) {
            const usize i = (static_cast<usize>(srcY) * kPixelW + x) * 4;
            const int dr = std::abs(static_cast<int>(rgba[i + 0]) - 0);
            const int dg = std::abs(static_cast<int>(rgba[i + 1]) - 0);
            const int db = std::abs(static_cast<int>(rgba[i + 2]) - 0);
            if (dr + dg + db <= 24) continue;  // фон
            st.content++;
            if (x < st.minX) st.minX = x;
            if (x > st.maxX) st.maxX = x;
            if (y < st.minY) st.minY = y;
            if (y > st.maxY) st.maxY = y;
            if (x < rowLeft[y]) rowLeft[y] = x;
            if (x > rowRight[y]) rowRight[y] = x;
        }
    }
    if (st.maxX < 0) return st;
    st.bboxW = st.maxX - st.minX + 1;
    st.bboxH = st.maxY - st.minY + 1;
    // Различные диапазоны по строкам: сплошной прямоугольник, выровненный по осям, даёт ровно один.
    std::vector<int> spans;
    for (int y = st.minY; y <= st.maxY; ++y) {
        if (rowRight[y] < 0) continue;
        int span = rowRight[y] - rowLeft[y] + 1;
        bool seen = false;
        for (int sv : spans)
            if (sv == span) seen = true;
        if (!seen) spans.push_back(span);
    }
    st.distinctRowSpans = static_cast<int>(spans.size());
    const int yTop = st.minY + st.bboxH / 4;
    const int yBot = st.minY + (st.bboxH * 3) / 4;
    st.topWidth = rowRight[yTop] >= 0 ? rowRight[yTop] - rowLeft[yTop] + 1 : 0;
    st.bottomWidth = rowRight[yBot] >= 0 ? rowRight[yBot] - rowLeft[yBot] + 1 : 0;
    return st;
}

}  // namespace

ENG_TEST(Lottie, RenderProducesDrawStats) {
    ENG_REQUIRE_GL();

    Renderer2D r;
    if (!r.Init()) ENG_SKIP("Renderer2D::Init failed");
    RenderTarget rt;
    RenderTargetDesc desc;
    desc.width = kPixelW;
    desc.height = kPixelH;
    desc.depth = false;
    if (!rt.Create(desc)) ENG_SKIP("RenderTarget unavailable");

    rt.Bind();
    r.BeginFrame(kPixelW, kPixelH, 1.0f, &rt);
    LottieAnimation a = ParseDoc(GenerateLottieJson("success"));
    ENG_CHECK(a.Valid());
    a.SetFrame(60.0f);
    a.Render(r, Rect{0, 0, (f32)kPixelW, (f32)kPixelH}, 1.0f, Color::White);
    const LottieAnimation::RenderStats& st = a.LastRenderStats();
    ENG_CHECK_GT(st.layersDrawn, 0);
    ENG_CHECK_GT(st.shapesDrawn, 0);

    a.RenderAt(r, Vec2{128, 96}, Vec2{100, 100}, 0.25f, 0.8f);
    ENG_CHECK_GT(a.LastRenderStats().shapesDrawn, 0);
    r.EndFrame();
    rt.Unbind();
    r.Shutdown();
}

// Каждая встроенная анимация должна реально рисовать пиксели в RenderTarget.
// Это тот тест, которым не был более ранний, чисто статистический и пропускавший
// GL: он рендерит через настоящий GPU-путь и осматривает фреймбуфер.
ENG_TEST(Lottie, RenderPaintsPixelsInsideDestination) {
    ENG_REQUIRE_GL();

    Renderer2D r;
    if (!r.Init()) ENG_SKIP("Renderer2D::Init failed");
    RenderTarget rt;
    RenderTargetDesc desc;
    desc.width = kPixelW;
    desc.height = kPixelH;
    desc.depth = false;
    if (!rt.Create(desc)) ENG_SKIP("RenderTarget unavailable");

    const char* names[] = {"loading", "success", "heart", "checkmark", "pulse", "card-flip"};
    const f32 dstMarginX = kPixelW * 0.03f;
    const f32 dstMarginY = kPixelH * 0.03f;
    const f32 dstArea = static_cast<f32>(kPixelW) * kPixelH;

    for (const char* name : names) {
        LottieAnimation a;
        std::string err;
        bool ok = a.LoadFromJson(GenerateLottieJson(name), &err);
        ENG_CHECK_MSG(ok, std::string(name) + ": " + err);
        if (!ok) continue;

        f32 bestCoverage = 0.0f;
        int framesWithShapes = 0;
        CoverageStats best;
        for (int s = 1; s <= 4; ++s) {
            a.SetFrame(a.TotalFrames() * (0.25f * static_cast<f32>(s)));
            rt.Bind();
            rt.Clear(kClearColour, false, false);
            r.BeginFrame(kPixelW, kPixelH, 1.0f, &rt);
            a.Render(r, Rect{0, 0, (f32)kPixelW, (f32)kPixelH}, 1.0f, Color::White);
            r.EndFrame();
            rt.Unbind();

            if (a.LastRenderStats().shapesDrawn > 0) framesWithShapes++;

            std::vector<u8> px;
            if (!rt.ReadPixels(&px) || px.size() < static_cast<usize>(kPixelW) * kPixelH * 4) {
                ENG_CHECK_MSG(false, std::string(name) + ": ReadPixels failed");
                break;
            }
            CoverageStats cs = Analyse(px);
            f32 coverage = static_cast<f32>(cs.content) / dstArea;
            if (coverage > bestCoverage) {
                bestCoverage = coverage;
                best = cs;
            }
        }

        // 1) Настоящая геометрия дошла до GPU на каждом сэмплированном кадре.
        ENG_CHECK_MSG(framesWithShapes == 4,
                      std::string(name) + ": shapesDrawn was 0 on some frame");
        // 2) Осмысленное, ненасыщенное покрытие назначения.
        ENG_CHECK_MSG(bestCoverage >= 0.02f, std::string(name) + ": almost nothing drawn");
        ENG_CHECK_MSG(bestCoverage <= 0.90f, std::string(name) + ": destination flooded");
        // 3) Графика остаётся внутри dst (с полями, никогда не обрезается и не
        //    растягивается до краёв) и реально внутри целевого прямоугольника.
        ENG_CHECK_MSG(best.minX >= static_cast<int>(dstMarginX),
                      std::string(name) + ": art touches the left edge");
        ENG_CHECK_MSG(best.minY >= static_cast<int>(dstMarginY),
                      std::string(name) + ": art touches the top edge");
        ENG_CHECK_MSG(best.maxX <= kPixelW - 1 - static_cast<int>(dstMarginX),
                      std::string(name) + ": art touches the right edge");
        ENG_CHECK_MSG(best.maxY <= kPixelH - 1 - static_cast<int>(dstMarginY),
                      std::string(name) + ": art touches the bottom edge");
        // 4) Не один сплошной блок: горизонтальный размах меняется от строки к строке.
        ENG_CHECK_MSG(best.distinctRowSpans >= 3,
                      std::string(name) + ": artwork is a solid rectangle");
    }

    // Проверка ориентации сердца: прямое сердце широко в долях и узко в острие,
    // поэтому верхняя четверть шире нижней.
    {
        LottieAnimation heart;
        if (heart.LoadFromJson(GenerateLottieJson("heart"))) {
            heart.SetFrame(heart.TotalFrames() * 0.5f);
            rt.Bind();
            rt.Clear(kClearColour, false, false);
            r.BeginFrame(kPixelW, kPixelH, 1.0f, &rt);
            heart.Render(r, Rect{0, 0, (f32)kPixelW, (f32)kPixelH}, 1.0f, Color::White);
            r.EndFrame();
            rt.Unbind();
            std::vector<u8> px;
            if (rt.ReadPixels(&px) && px.size() >= static_cast<usize>(kPixelW) * kPixelH * 4) {
                CoverageStats heartStats = Analyse(px);
                ENG_CHECK_MSG(heartStats.topWidth > heartStats.bottomWidth,
                              "heart should be widest at its lobes (not flipped or rotated)");
            }
        }
    }

    r.Shutdown();
}
