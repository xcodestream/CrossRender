// Тесты модуля core: математика, JSON, файловая система, время, логирование, RNG, easing.
#include "crossrender/core/Log.h"
#include "crossrender/gfx/Mesh.h"
#include "crossrender/core/File.h"
#include "crossrender/core/Json.h"
#include "crossrender/core/Math.h"
#include "crossrender/core/Time.h"
#include "crossrender/test/Test.h"

#include <cmath>
#include <thread>

using namespace crossrender;

ENG_TEST(Math, VecBasics) {
    Vec2 a{3, 4};
    ENG_CHECK_NEAR(Length(a), 5.0f, 1e-5f);
    ENG_CHECK_NEAR(LengthSq(a), 25.0f, 1e-5f);
    Vec2 n = Normalize(a);
    ENG_CHECK_NEAR(Length(n), 1.0f, 1e-5f);
    ENG_CHECK_NEAR(Dot(Vec2{1, 0}, Vec2{0, 1}), 0.0f, 1e-6f);
    ENG_CHECK_NEAR(Cross(Vec2{1, 0}, Vec2{0, 1}), 1.0f, 1e-6f);

    Vec3 u{1, 0, 0}, v{0, 1, 0};
    Vec3 c = Cross(u, v);
    ENG_CHECK_NEAR(c.x, 0.0f, 1e-6f);
    ENG_CHECK_NEAR(c.y, 0.0f, 1e-6f);
    ENG_CHECK_NEAR(c.z, 1.0f, 1e-6f);
    ENG_CHECK_NEAR(Dot(u, v), 0.0f, 1e-6f);
    ENG_CHECK_NEAR(Length(Vec3{1, 2, 2}), 3.0f, 1e-5f);
}

ENG_TEST(Math, MatrixMultiplyAndInverse) {
    Mat4 t = Mat4::Translate({1, 2, 3});
    Mat4 s = Mat4::Scale({2, 2, 2});
    Mat4 ts = t * s;
    Vec3 p = ts.TransformPoint({1, 0, 0});
    ENG_CHECK_NEAR(p.x, 3.0f, 1e-5f);
    ENG_CHECK_NEAR(p.y, 2.0f, 1e-5f);
    ENG_CHECK_NEAR(p.z, 3.0f, 1e-5f);

    Mat4 m = Mat4::TRS({3, -1, 2}, {0.3f, -0.7f, 1.1f}, {1.5f, 0.5f, 2.0f});
    Mat4 inv = m.Inverse();
    Mat4 id = m * inv;
    for (int c = 0; c < 4; ++c) {
        for (int r = 0; r < 4; ++r) {
            f32 expected = (c == r) ? 1.0f : 0.0f;
            ENG_CHECK_NEAR(id.at(c, r), expected, 1e-4f);
        }
    }
    Vec3 src{0.5f, 2.0f, -1.0f};
    Vec3 rt = inv.TransformPoint(m.TransformPoint(src));
    ENG_CHECK_NEAR(rt.x, src.x, 1e-4f);
    ENG_CHECK_NEAR(rt.y, src.y, 1e-4f);
    ENG_CHECK_NEAR(rt.z, src.z, 1e-4f);
}

ENG_TEST(Math, ProjectionAndUnproject) {
    Camera cam;
    cam.position = {0, 0, 5};
    cam.target = {0, 0, 0};
    cam.fovY = Radians(60.0f);
    f32 aspect = 16.0f / 9.0f;
    (void)cam.ViewProj(aspect);
    // Точка на оси взгляда проецируется в центр экрана.
    Vec2 screen;
    ENG_CHECK(cam.WorldToScreen({0, 0, 0}, {1920, 1080}, &screen));
    ENG_CHECK_NEAR(screen.x, 960.0f, 1.0f);
    ENG_CHECK_NEAR(screen.y, 540.0f, 1.0f);

    Vec3 origin, dir;
    cam.RayFromScreen({960, 540}, {1920, 1080}, &origin, &dir);
    ENG_CHECK_NEAR(dir.x, 0.0f, 1e-3f);
    ENG_CHECK_NEAR(dir.y, 0.0f, 1e-3f);
    ENG_CHECK_NEAR(dir.z, -1.0f, 1e-3f);

    // Луч из угла должен смотреть вверх-влево или вверх-вправо, но никогда вдоль +Z.
    cam.RayFromScreen({0, 0}, {1920, 1080}, &origin, &dir);
    ENG_CHECK(dir.z < 0.0f);

    // Ортографическая проекция туда-обратно.
    Camera ortho;
    ortho.projection = ProjectionType::Orthographic;
    ortho.position = {0, 0, 10};
    ortho.target = {0, 0, 0};
    ortho.orthoHeight = 10.0f;
    Vec2 s2;
    ENG_CHECK(ortho.WorldToScreen({0, 0, 0}, {100, 100}, &s2));
    ENG_CHECK_NEAR(s2.x, 50.0f, 0.5f);
    ENG_CHECK_NEAR(s2.y, 50.0f, 0.5f);
}

ENG_TEST(Math, Quaternion) {
    Quat q = Quat::FromAxisAngle({0, 0, 1}, Radians(90.0f));
    Vec3 r = q * Vec3{1, 0, 0};
    ENG_CHECK_NEAR(r.x, 0.0f, 1e-5f);
    ENG_CHECK_NEAR(r.y, 1.0f, 1e-5f);

    // У поворота на 180 градусов два одинаково коротких пути (вокруг +Y или -Y),
    // поэтому проверяем половинный угол и ось, а не конкретную полусферу.
    Quat a = Quat::FromAxisAngle({0, 1, 0}, 0.0f);
    Quat b = Quat::FromAxisAngle({0, 1, 0}, Radians(180.0f));
    Quat mid = Quat::Slerp(a, b, 0.5f);
    ENG_CHECK_NEAR(std::fabs(mid.y), 0.70710678f, 1e-4f);
    ENG_CHECK_NEAR(std::fabs(mid.w), 0.70710678f, 1e-4f);
    ENG_CHECK_NEAR(mid.x, 0.0f, 1e-4f);
    ENG_CHECK_NEAR(mid.z, 0.0f, 1e-4f);
    Vec3 v = mid * Vec3{1, 0, 0};
    ENG_CHECK_NEAR(std::fabs(v.z), 1.0f, 1e-3f);
    // operator* и ToMat4 должны совпадать для каждой формы поворота.
    for (f32 angle : {0.3f, 1.2f, 2.5f, -0.7f}) {
        Quat q = Quat::FromAxisAngle(Normalize(Vec3{0.3f, 1.0f, -0.2f}), angle);
        Vec3 qv = q * Vec3{1, 0.5f, -0.25f};
        Vec3 mv = q.ToMat4().TransformDir({1, 0.5f, -0.25f});
        ENG_CHECK_NEAR(qv.x, mv.x, 1e-4f);
        ENG_CHECK_NEAR(qv.y, mv.y, 1e-4f);
        ENG_CHECK_NEAR(qv.z, mv.z, 1e-4f);
    }

    Mat4 m = Quat::FromEuler({0.4f, -0.2f, 0.9f}).ToMat4();
    Quat back = Quat::FromMat4(m);
    Quat orig = Quat::FromEuler({0.4f, -0.2f, 0.9f});
    f32 d = orig.x * back.x + orig.y * back.y + orig.z * back.z + orig.w * back.w;
    ENG_CHECK_NEAR(d, 1.0f, 1e-4f);  // та же полусфера, не сопряжение

    // Извлечённый кватернион должен воспроизводить поворот матрицы, а не её обратный.
    for (f32 angle : {0.3f, 1.2f, 2.5f, -0.7f}) {
        Mat4 ry = Mat4::RotateY(angle);
        Vec3 expected = ry.TransformDir({1, 0, 0});
        Vec3 actual = Quat::FromMat4(ry) * Vec3{1, 0, 0};
        ENG_CHECK_NEAR(actual.x, expected.x, 1e-4f);
        ENG_CHECK_NEAR(actual.y, expected.y, 1e-4f);
        ENG_CHECK_NEAR(actual.z, expected.z, 1e-4f);
    }
    {
        // Также для поворота с отрицательным следом (180 градусов вокруг X).
        Mat4 rx = Mat4::RotateX(kPi);
        Vec3 expected = rx.TransformDir({0, 1, 0});
        Vec3 actual = Quat::FromMat4(rx) * Vec3{0, 1, 0};
        ENG_CHECK_NEAR(actual.x, expected.x, 1e-3f);
        ENG_CHECK_NEAR(actual.y, expected.y, 1e-3f);
        ENG_CHECK_NEAR(actual.z, expected.z, 1e-3f);
    }
}

ENG_TEST(Math, RayIntersections) {
    f32 t = RaySphere({0, 0, 0}, {0, 0, -1}, {0, 0, -5}, 1.0f);
    ENG_CHECK_NEAR(t, 4.0f, 1e-4f);
    ENG_CHECK_NEAR(RaySphere({0, 0, 0}, {0, 1, 0}, {0, 0, -5}, 1.0f), -1.0f, 1e-4f);

    f32 tp = RayPlane({0, 1, 0}, {0, -1, 0}, {0, 0, 0}, {0, 1, 0});
    ENG_CHECK_NEAR(tp, 1.0f, 1e-4f);

    f32 ta = 0;
    ENG_CHECK(RayAabb({0, 0, 5}, {0, 0, -1}, {-1, -1, -1}, {1, 1, 1}, &ta));
    ENG_CHECK_NEAR(ta, 4.0f, 1e-4f);
    ENG_CHECK(!RayAabb({5, 0, 5}, {0, 0, -1}, {-1, -1, -1}, {1, 1, 1}, nullptr));
}

ENG_TEST(Math, ColorAndHsl) {
    Color c = Color::FromRGB(0xFF8000);
    ENG_CHECK_NEAR(c.r, 1.0f, 1e-3f);
    ENG_CHECK_NEAR(c.g, 128.0f / 255.0f, 1e-3f);
    ENG_CHECK_NEAR(c.b, 0.0f, 1e-3f);

    Color hsl = Color::HSL(0.0f, 1.0f, 0.5f);
    ENG_CHECK_NEAR(hsl.r, 1.0f, 1e-3f);
    ENG_CHECK_NEAR(hsl.g, 0.0f, 1e-3f);

    f32 h, s, l;
    Color(0.0f, 0.0f, 1.0f).ToHSL(&h, &s, &l);
    ENG_CHECK_NEAR(h, 2.0f / 3.0f, 1e-3f);
    ENG_CHECK_NEAR(l, 0.5f, 1e-3f);

    u32 packed = Color::FromBytes(10, 20, 30, 40).ToRGBA8();
    ENG_CHECK_EQ(packed & 0xFF, 10u);
    ENG_CHECK_EQ((packed >> 8) & 0xFF, 20u);
    ENG_CHECK_EQ((packed >> 16) & 0xFF, 30u);
    ENG_CHECK_EQ((packed >> 24) & 0xFF, 40u);
}

ENG_TEST(Math, RectOperations) {
    Rect a{0, 0, 10, 10};
    Rect b{5, 5, 10, 10};
    ENG_CHECK(a.Contains({5, 5}));
    ENG_CHECK(!a.Contains({10, 10}));
    ENG_CHECK(a.Intersects(b));
    Rect i = a.Intersect(b);
    ENG_CHECK_NEAR(i.x, 5, 1e-5f);
    ENG_CHECK_NEAR(i.w, 5, 1e-5f);
    Rect u = a.Union(b);
    ENG_CHECK_NEAR(u.w, 15, 1e-5f);
    ENG_CHECK_NEAR(u.h, 15, 1e-5f);
    Rect inset = a.Inset(2.0f);
    ENG_CHECK_NEAR(inset.x, 2, 1e-5f);
    ENG_CHECK_NEAR(inset.w, 6, 1e-5f);
}

ENG_TEST(Math, RandomDeterminismAndRange) {
    Random a(42), b(42);
    for (int i = 0; i < 100; ++i) {
        ENG_CHECK_NEAR(a.NextFloat(), b.NextFloat(), 0.0f);
    }
    Random c(43);
    bool differs = false;
    Random d1(1), d2(2);
    for (int i = 0; i < 20; ++i)
        if (std::fabs(d1.NextFloat() - d2.NextFloat()) > 1e-6f) differs = true;
    ENG_CHECK(differs);

    Random r(7);
    for (int i = 0; i < 1000; ++i) {
        f32 v = r.NextFloat();
        ENG_CHECK(v >= 0.0f && v < 1.0f);
    }
    for (int i = 0; i < 100; ++i) {
        i32 v = r.RangeInt(3, 7);
        ENG_CHECK(v >= 3 && v <= 7);
    }
    std::vector<int> vals{1, 2, 3, 4, 5, 6, 7, 8};
    Random s1(99), s2(99);
    s1.Shuffle(vals);
    // Детерминизм перемешивания при том же seed.
    std::vector<int> vals2{1, 2, 3, 4, 5, 6, 7, 8};
    s2.Shuffle(vals2);
    ENG_CHECK(vals == vals2);
}

ENG_TEST(Math, CubicBezierEase) {
    for (f32 x = 0.0f; x <= 1.0f; x += 0.05f) {
        f32 y = CubicBezierEase(0.42f, 0.0f, 0.58f, 1.0f, x);
        ENG_CHECK(y >= -1e-4f && y <= 1.0001f);
    }
    ENG_CHECK_NEAR(CubicBezierEase(0.42f, 0.0f, 0.58f, 1.0f, 0.0f), 0.0f, 1e-4f);
    ENG_CHECK_NEAR(CubicBezierEase(0.42f, 0.0f, 0.58f, 1.0f, 1.0f), 1.0f, 1e-4f);
    // Монотонный рост для стандартного ease-in-out.
    f32 prev = -1;
    for (f32 x = 0.0f; x <= 1.0f; x += 0.02f) {
        f32 y = CubicBezierEase(0.42f, 0.0f, 0.58f, 1.0f, x);
        ENG_CHECK(y >= prev - 1e-3f);
        prev = y;
    }
}

ENG_TEST(Json, ParseScalarsAndContainers) {
    std::string err;
    JsonValue v = JsonValue::Parse(
        R"({"name":"engine","version":2,"pi":3.5,"ok":true,"none":null,"list":[1,2,3],"nested":{"a":1}})",
        &err);
    ENG_CHECK(err.empty());
    ENG_CHECK(v.IsObject());
    ENG_CHECK_STR_EQ(v.GetString("name"), "engine");
    ENG_CHECK_EQ(v.GetInt("version"), 2);
    ENG_CHECK_NEAR(v.GetFloat("pi"), 3.5f, 1e-6f);
    ENG_CHECK(v.GetBool("ok"));
    ENG_CHECK(v["list"].IsArray());
    ENG_CHECK_EQ(v["list"].Size(), 3u);
    ENG_CHECK_EQ(v["list"][1].AsInt(), 2);
    ENG_CHECK_EQ(v["nested"].GetInt("a"), 1);
    ENG_CHECK(v["missing"].IsNull());
    ENG_CHECK_EQ(v.GetInt("missing", -7), -7);
}

ENG_TEST(Json, ParseErrorsAndEdgeCases) {
    std::string err;
    JsonValue::Parse("{", &err);
    ENG_CHECK(!err.empty());

    err.clear();
    JsonValue::Parse("[1,2", &err);
    ENG_CHECK(!err.empty());

    // Глубокая вложенность + escape-последовательности + unicode-escape.
    err.clear();
    JsonValue v = JsonValue::Parse(R"({"s":"a\"b\\c\n\t","u":"\u0414\u0443\u0440\u0430\u043a"})", &err);
    ENG_CHECK(err.empty());
    ENG_CHECK_STR_EQ(v.GetString("s"), "a\"b\\c\n\t");
    ENG_CHECK_STR_EQ(v.GetString("u"), "Дурак");

    // Пустые контейнеры.
    JsonValue e = JsonValue::Parse("{\"a\":[],\"b\":{}}");
    ENG_CHECK(e["a"].IsArray());
    ENG_CHECK_EQ(e["a"].Size(), 0u);
    ENG_CHECK(e["b"].IsObject());
    ENG_CHECK_EQ(e["b"].Size(), 0u);

    for (const char* invalid : {"+1", "1.", "1e", "1e+", "01", "\"\\uD800\\u0000\""}) {
        err.clear();
        JsonValue::Parse(invalid, &err);
        ENG_CHECK_MSG(!err.empty(), invalid);
    }
    err.clear();
    JsonValue pair = JsonValue::Parse("\"\\uD834\\uDD1E\"", &err);
    ENG_CHECK(err.empty());
    ENG_CHECK_EQ(pair.AsString().size(), usize{4});
}

ENG_TEST(Json, DumpRoundTrip) {
    JsonValue root(JsonObject{});
    root.Set("i", JsonValue(5));
    root.Set("f", JsonValue(1.25));
    root.Set("s", JsonValue(std::string("he\"llo\n")));
    root.Set("b", JsonValue(true));
    JsonValue arr(JsonArray{});
    arr.Push(JsonValue(1));
    arr.Push(JsonValue(2));
    root.Set("arr", arr);

    std::string text = root.Dump();
    std::string err;
    JsonValue back = JsonValue::Parse(text, &err);
    ENG_CHECK(err.empty());
    ENG_CHECK_EQ(back.GetInt("i"), 5);
    ENG_CHECK_NEAR(back.GetFloat("f"), 1.25f, 1e-9f);
    ENG_CHECK_STR_EQ(back.GetString("s"), "he\"llo\n");
    ENG_CHECK(back.GetBool("b"));
    ENG_CHECK_EQ(back["arr"].Size(), 2u);
}

ENG_TEST(Json, WriteAndParseFile) {
    std::string path = test::TempFilePath("core_json.json");
    JsonValue root(JsonObject{});
    root.Set("scene", JsonValue(std::string("Durak")));
    root.Set("players", JsonValue(2));
    ENG_CHECK(WriteTextFile(path, root.Dump(2)));

    JsonValue loaded;
    std::string err;
    ENG_CHECK(JsonValue::ParseFile(path, &loaded, &err));
    ENG_CHECK(err.empty());
    ENG_CHECK_STR_EQ(loaded.GetString("scene"), "Durak");
    ENG_CHECK_EQ(loaded.GetInt("players"), 2);
}

ENG_TEST(File, ReadWriteRoundTrip) {
    std::string path = test::TempFilePath("core_file.bin");
    std::vector<u8> data(1024);
    for (usize i = 0; i < data.size(); ++i) data[i] = static_cast<u8>(i * 7 + 3);
    ENG_CHECK(WriteBinaryFile(path, data.data(), data.size()));
    ENG_CHECK(FileExists(path));
    ByteBuffer read = ReadBinaryFile(path);
    ENG_CHECK_EQ(read.size(), data.size());
    ENG_CHECK(read == data);

    ENG_CHECK(WriteTextFile(path, "hello engine"));
    ENG_CHECK_STR_EQ(ReadTextFile(path), "hello engine");
    ENG_CHECK(!FileExists(test::TempFilePath("does_not_exist_12345.bin")));
}

ENG_TEST(File, PathHelpers) {
    ENG_CHECK_STR_EQ(PathJoin("a/b", "c.png"), "a/b/c.png");
    ENG_CHECK_STR_EQ(PathJoin("a/b/", "c.png"), "a/b/c.png");
    ENG_CHECK_STR_EQ(PathJoin("", "c.png"), "c.png");
    ENG_CHECK_STR_EQ(PathBase("assets/tex/rock.png"), "rock.png");
    ENG_CHECK_STR_EQ(PathDir("assets/tex/rock.png"), "assets/tex");
    ENG_CHECK_STR_EQ(PathExt("assets/tex/rock.PNG"), ".png");
    ENG_CHECK_STR_EQ(PathExt("noext"), "");
    ENG_CHECK_STR_EQ(PathNormalize("a/b/../c/"), "a/c");
}

ENG_TEST(File, UserRootIsWritable) {
    const std::string& root = GetUserRoot();
    ENG_CHECK(!root.empty());
    std::string path = PathJoin(root, "writable_probe.txt");
    ENG_CHECK(WriteTextFile(path, "ok"));
    ENG_CHECK(FileExists(path));
}

ENG_TEST(Time, ClockDeltaAndFixedSteps) {
    Clock clock;
    clock.ForceDelta(1.0f / 60.0f);
    for (int i = 0; i < 10; ++i) clock.Tick();
    ENG_CHECK_NEAR(clock.Delta(), 1.0f / 60.0f, 1e-5f);
    ENG_CHECK_EQ(clock.Frame(), 10u);
    ENG_CHECK_NEAR(clock.Time(), 10.0f / 60.0f, 1e-4f);

    clock.SetTimeScale(0.5f);
    clock.Tick();
    ENG_CHECK_NEAR(clock.Delta(), 1.0f / 120.0f, 1e-5f);

    clock.SetTimeScale(1.0f);
    clock.SetFixedStep(1.0f / 60.0f);
    clock.Tick();
    u32 steps = clock.ConsumeFixedSteps();
    ENG_CHECK(steps >= 1 && steps <= 8);
    ENG_CHECK_STR_EQ(FormatTime(0.0f), "00:00.000");
    ENG_CHECK_STR_EQ(FormatTime(61.5f), "01:01.500");
}

ENG_TEST(Time, MonotonicNow) {
    f64 a = NowSeconds();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    f64 b = NowSeconds();
    ENG_CHECK(b > a);
    ENG_CHECK(b - a < 1.0);
}

ENG_TEST(Log, SinkAndHistory) {
    struct Capture {
        int count = 0;
        LogLevel last = LogLevel::Off;
        std::string message;
    } capture;
    LogAddSink(
        [](LogLevel level, const char*, const char* msg, void* user) {
            auto* c = static_cast<Capture*>(user);
            c->count++;
            c->last = level;
            c->message = msg;
        },
        &capture);

    LogEnableHistory(true);
    LogSetLevel(LogLevel::Debug);
    ENG_LOGW("test", "hello %d", 42);
    ENG_CHECK_EQ(capture.count, 1);
    ENG_CHECK(capture.last == LogLevel::Warn);
    ENG_CHECK_STR_EQ(capture.message, "hello 42");
    ENG_CHECK(!LogHistory().empty());
    LogClearSinks();
    LogEnableHistory(false);

    capture.count = 0;
    LogSetLevel(LogLevel::Error);
    ENG_LOGI("test", "filtered out");
    ENG_CHECK_EQ(capture.count, 0);
    LogSetLevel(LogLevel::Info);
}
