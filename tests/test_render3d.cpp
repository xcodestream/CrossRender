// Тесты crossrender/gfx/Renderer3D.
//
// CPU-часть (отсечение усечённой пирамидой, разбиение каскадов, математика лучей,
// выбор треугольников, упаковка источников света) работает везде; GPU-часть защищена
// ENG_REQUIRE_GL() и пропускается, если контекст создать не удаётся.

#include "crossrender/gfx/Renderer3D.h"

#include "crossrender/gfx/GL.h"
#include "crossrender/core/Log.h"
#include "crossrender/gfx/Mesh.h"
#include "crossrender/test/Test.h"
#include "crossrender/gfx/RenderTarget.h"

#include "gfx/Renderer3DInternal.h"

#include <cmath>
#include <vector>
#include <cstdint>

using namespace crossrender;

namespace {

// Камера для тестов отсечения: в (0,0,5), смотрит вдоль -Z, fov 90 градусов,
// near 1 / far 100. На расстоянии d полуразор усечённой пирамиды равен ровно d.
Camera TestCamera() {
    Camera cam;
    cam.position = {0, 0, 5};
    cam.target = {0, 0, 0};
    cam.up = {0, 1, 0};
    cam.fovY = 90.0f * kDeg2Rad;
    cam.nearZ = 1.0f;
    cam.farZ = 100.0f;
    cam.projection = ProjectionType::Perspective;
    return cam;
}

f32 Luminance(const u8* rgba) {
    return 0.2126f * static_cast<f32>(rgba[0]) + 0.7152f * static_cast<f32>(rgba[1]) +
           0.0722f * static_cast<f32>(rgba[2]);
}

// Количество пикселей, отличающихся от цвета фона более чем на `eps`.
int CountDifferent(const std::vector<u8>& pixels, const u8 bg[4], int eps = 8) {
    int count = 0;
    for (usize i = 0; i + 3 < pixels.size(); i += 4) {
        for (int c = 0; c < 3; ++c) {
            if (std::abs(static_cast<int>(pixels[i + c]) - static_cast<int>(bg[c])) > eps) {
                ++count;
                break;
            }
        }
    }
    return count;
}

}  // namespace

// ---------------------------------------------------------------------------
// Отсечение усечённой пирамидой (frustum culling)
// ---------------------------------------------------------------------------
ENG_TEST(Render3D, FrustumPlaneExtraction) {
    const Camera cam = TestCamera();
    const Mat4 vp = cam.ViewProj(1.0f);

    r3d_internal::Plane planes[6];
    r3d_internal::ExtractFrustumPlanes(vp, planes);

    // Все нормали единичной длины, а near/far-плоскости охватывают начало координат.
    for (int i = 0; i < 6; ++i) ENG_CHECK_NEAR(Length(planes[i].normal), 1.0f, 1e-3);

    // Бокс вокруг начала координат уверенно внутри.
    ENG_CHECK(r3d_internal::AabbInFrustum(planes, Vec3{-0.5f, -0.5f, -0.5f}, Vec3{0.5f, 0.5f, 0.5f}));
    ENG_CHECK(r3d_internal::AabbInFrustum(vp, Vec3{-0.5f, -0.5f, -0.5f}, Vec3{0.5f, 0.5f, 0.5f}));

    // Позади камеры (камера в z = +5, смотрит в сторону -Z).
    ENG_CHECK(!r3d_internal::AabbInFrustum(vp, Vec3{-0.5f, -0.5f, 6.0f}, Vec3{0.5f, 0.5f, 7.0f}));
    // За дальней плоскостью.
    ENG_CHECK(!r3d_internal::AabbInFrustum(vp, Vec3{-0.5f, -0.5f, -120.0f}, Vec3{0.5f, 0.5f, -119.0f}));
    // Далеко слева / справа на комфортной глубине.
    ENG_CHECK(!r3d_internal::AabbInFrustum(vp, Vec3{-9.0f, -0.5f, -0.5f}, Vec3{-8.0f, 0.5f, 0.5f}));
    ENG_CHECK(!r3d_internal::AabbInFrustum(vp, Vec3{8.0f, -0.5f, -0.5f}, Vec3{9.0f, 0.5f, 0.5f}));
    // Выше / ниже усечённой пирамиды с углом 90 градусов.
    ENG_CHECK(!r3d_internal::AabbInFrustum(vp, Vec3{-0.5f, 9.0f, -0.5f}, Vec3{0.5f, 10.0f, 0.5f}));

    // Пересекающий левую плоскость бокс должен быть сохранён (консервативный тест).
    ENG_CHECK(r3d_internal::AabbInFrustum(vp, Vec3{-5.0f, -0.2f, -0.5f}, Vec3{-4.0f, 0.2f, 0.5f}));

    // Отсечение согласуется с мировыми границами вручную добавленного объекта.
    Vec3 lo, hi;
    r3d_internal::TransformAabb(Mat4::Translate(Vec3{0, 0, 6}) * Mat4::Scale(Vec3{2, 2, 2}),
                                Vec3{-0.5f, -0.5f, -0.5f}, Vec3{0.5f, 0.5f, 0.5f}, &lo, &hi);
    ENG_CHECK_NEAR(lo.z, 5.0f, 1e-4);
    ENG_CHECK_NEAR(hi.z, 7.0f, 1e-4);
    ENG_CHECK_NEAR(hi.y, 1.0f, 1e-4);
    // ... этот бокс позади камеры.
    ENG_CHECK(!r3d_internal::AabbInFrustum(vp, lo, hi));
}

// ---------------------------------------------------------------------------
// Разбиение каскадов
// ---------------------------------------------------------------------------
ENG_TEST(Render3D, CascadeSplits) {
    f32 splits[3] = {0, 0, 0};

    // Практическая схема разбиения с lambda 0.85 (по умолчанию).
    r3d_internal::ComputeCascadeSplits(0.1f, 60.0f, 0.85f, 3, splits);
    ENG_CHECK_GT(splits[0], 0.1f);
    ENG_CHECK(splits[0] < splits[1]);
    ENG_CHECK(splits[1] < splits[2]);
    ENG_CHECK_NEAR(splits[2], 60.0f, 1e-4);
    // Все разбиения остаются в запрошенном диапазоне.
    for (int i = 0; i < 3; ++i) {
        ENG_CHECK(splits[i] > 0.1f);
        ENG_CHECK(splits[i] <= 60.0f + 1e-4f);
    }

    // lambda = 0 — равномерное разбиение.
    r3d_internal::ComputeCascadeSplits(1.0f, 100.0f, 0.0f, 3, splits);
    ENG_CHECK_NEAR(splits[0], 1.0f + 99.0f / 3.0f, 1e-3);
    ENG_CHECK_NEAR(splits[1], 1.0f + 2.0f * 99.0f / 3.0f, 1e-3);
    ENG_CHECK_NEAR(splits[2], 100.0f, 1e-3);

    // lambda = 1 — логарифмическое: первое разбиение гораздо ближе, чем при равномерном.
    r3d_internal::ComputeCascadeSplits(1.0f, 100.0f, 1.0f, 3, splits);
    ENG_CHECK(splits[0] < 1.0f + 99.0f / 3.0f);
    ENG_CHECK_NEAR(splits[2], 100.0f, 1e-3);

    // Один каскад покрывает весь диапазон.
    r3d_internal::ComputeCascadeSplits(0.5f, 40.0f, 0.5f, 1, splits);
    ENG_CHECK_NEAR(splits[0], 40.0f, 1e-4);

    // Вырожденный вход не должен давать NaN или убывающие разбиения.
    r3d_internal::ComputeCascadeSplits(0.0f, 0.0f, 2.0f, 3, splits);
    ENG_CHECK(splits[0] > 0.0f);
    ENG_CHECK(splits[0] <= splits[1] && splits[1] <= splits[2]);
}

// ---------------------------------------------------------------------------
// Математика лучей / треугольников
// ---------------------------------------------------------------------------
ENG_TEST(Render3D, RayAabbAndTriangle) {
    const Vec3 lo{-1, -1, -1};
    const Vec3 hi{1, 1, 1};
    f32 t = 0.0f;

    ENG_CHECK(r3d_internal::RayAabbTest(Vec3{0, 0, 5}, Vec3{0, 0, -1}, lo, hi, &t));
    ENG_CHECK_NEAR(t, 4.0f, 1e-4);
    ENG_CHECK(!r3d_internal::RayAabbTest(Vec3{3, 0, 5}, Vec3{0, 0, -1}, lo, hi, &t));
    ENG_CHECK(!r3d_internal::RayAabbTest(Vec3{0, 0, 5}, Vec3{0, 1, 0}, lo, hi, &t));
    ENG_CHECK(!r3d_internal::RayAabbTest(Vec3{0, 0, 5}, Vec3{0, 0, 1}, lo, hi, &t));
    // Начало внутри: возвращаемое расстояние — до точки выхода.
    ENG_CHECK(r3d_internal::RayAabbTest(Vec3{0, 0, 0}, Vec3{1, 0, 0}, lo, hi, &t));
    ENG_CHECK_NEAR(t, 1.0f, 1e-4);
    // Диагональный луч, входящий через угловую область.
    ENG_CHECK(r3d_internal::RayAabbTest(Vec3{-2, -2, -2}, Normalize(Vec3{1, 1, 1}), lo, hi, &t));
    ENG_CHECK(t > 0.0f);

    const Vec3 a{0, 0, 0};
    const Vec3 b{1, 0, 0};
    const Vec3 c{0, 1, 0};
    f32 u = 0.0f, v = 0.0f;
    ENG_CHECK(r3d_internal::RayTriangle(Vec3{0.25f, 0.25f, 1}, Vec3{0, 0, -1}, a, b, c, &t, &u, &v));
    ENG_CHECK_NEAR(t, 1.0f, 1e-5);
    ENG_CHECK_NEAR(u, 0.25f, 1e-4);
    ENG_CHECK_NEAR(v, 0.25f, 1e-4);
    // Двусторонний: луч с обратной стороны тоже попадает.
    ENG_CHECK(r3d_internal::RayTriangle(Vec3{0.25f, 0.25f, -1}, Vec3{0, 0, 1}, a, b, c, &t, &u, &v));
    // Мимо треугольника.
    ENG_CHECK(!r3d_internal::RayTriangle(Vec3{0.9f, 0.9f, 1}, Vec3{0, 0, -1}, a, b, c, &t, &u, &v));
    // Направлен от плоскости.
    ENG_CHECK(!r3d_internal::RayTriangle(Vec3{0.25f, 0.25f, 1}, Vec3{0, 0, 1}, a, b, c, &t, &u, &v));
    // Параллелен плоскости.
    ENG_CHECK(!r3d_internal::RayTriangle(Vec3{0.25f, 0.25f, 1}, Vec3{1, 0, 0}, a, b, c, &t, &u, &v));
}

// ---------------------------------------------------------------------------
// Точный выбор по треугольникам через публичный API (GL не нужен)
// ---------------------------------------------------------------------------
ENG_TEST(Render3D, RaycastPicking) {
    MeshData data = MeshData::Cube(2.0f);
    Mesh mesh;
    mesh.SetName("cube");
    // Без GL-контекста Create() возвращает false, но всё равно записывает границы,
    // чего и достаточно CPU-пути выбора.
    mesh.Create(data);
    ENG_CHECK(mesh.Bounds().Valid());
    ENG_CHECK_NEAR(mesh.Bounds().max.x, 1.0f, 1e-4);

    int tag = 42;
    Renderer3D renderer;
    renderer.SubmitPickable(mesh, Mat4::Translate(Vec3{0, 0, 10}), 7, &tag);

    RayHit hit;
    // --- fallback по габаритному боксу (MeshData не зарегистрирован) --------
    ENG_CHECK(renderer.Raycast(Ray{{0, 0, 0}, {0, 0, 1}}, &hit));
    ENG_CHECK(hit.hit);
    ENG_CHECK_EQ(hit.objectId, 7);
    ENG_CHECK_NEAR(hit.distance, 9.0f, 1e-3);
    ENG_CHECK_EQ(hit.triangleIndex, -1);
    ENG_CHECK_STR_EQ(hit.meshName, "cube");
    ENG_CHECK(hit.userData == static_cast<const void*>(&tag));
    ENG_CHECK_NEAR(hit.point.z, 9.0f, 1e-3);
    ENG_CHECK(hit.normal.z < -0.9f);

    // --- точный выбор по треугольникам после регистрации исходного MeshData -
    r3d_internal::RegisterMeshData(&mesh, &data);
    ENG_CHECK(r3d_internal::FindMeshData(&mesh) == &data);
    ENG_CHECK(renderer.Raycast(Ray{{0, 0, 0}, {0, 0, 1}}, &hit));
    ENG_CHECK(hit.triangleIndex >= 0);
    ENG_CHECK(hit.triangleIndex < static_cast<int>(data.indices.size() / 3));
    ENG_CHECK_NEAR(hit.distance, 9.0f, 1e-3);
    ENG_CHECK(hit.normal.z < -0.9f);

    // Луч, полностью промахивающийся мимо куба.
    ENG_CHECK(!renderer.Raycast(Ray{{0, 0, 0}, {1, 0, 0}}, &hit, 1e30f));
    // maxDistance учитывается.
    ENG_CHECK(!renderer.Raycast(Ray{{0, 0, 0}, {0, 0, 1}}, &hit, 5.0f));
    ENG_CHECK(renderer.Raycast(Ray{{0, 0, 0}, {0, 0, 1}}, &hit, 9.5f));

    // Второй, более близкий объект побеждает.
    renderer.SubmitPickable(mesh, Mat4::Translate(Vec3{0, 0, 5}), 9, nullptr);
    ENG_CHECK(renderer.Raycast(Ray{{0, 0, 0}, {0, 0, 1}}, &hit));
    ENG_CHECK_EQ(hit.objectId, 9);
    ENG_CHECK_NEAR(hit.distance, 4.0f, 1e-3);

    // Выбор в экранных координатах идёт через Camera::RayFromScreen.
    Camera cam = TestCamera();
    cam.position = {0, 0, 0};
    cam.target = {0, 0, 1};
    cam.nearZ = 0.05f;
    renderer.SetCamera(cam);
    ENG_CHECK(renderer.PickAtScreen(Vec2{32.0f, 32.0f}, Vec2{64.0f, 64.0f}, &hit));
    ENG_CHECK_EQ(hit.objectId, 9);

    renderer.ClearPickables();
    ENG_CHECK(!renderer.Raycast(Ray{{0, 0, 0}, {0, 0, 1}}, &hit));
    ENG_CHECK(!hit.hit);
    r3d_internal::UnregisterMeshData(&mesh);
    ENG_CHECK(r3d_internal::FindMeshData(&mesh) == nullptr);
}

// ---------------------------------------------------------------------------
// Учёт источников света + упаковка в uniform-данные
// ---------------------------------------------------------------------------
ENG_TEST(Render3D, LightPacking) {
    Renderer3D renderer;
    ENG_CHECK_EQ(renderer.MaxLights(), 8);
    ENG_CHECK_EQ(renderer.LightCount(), 0);
    ENG_CHECK(renderer.GetLight(0) == nullptr);

    const int dirIndex = renderer.AddLight(Light::Directional(Vec3{0, -1, 0}, Color::White, 2.0f));
    ENG_CHECK_EQ(dirIndex, 0);
    ENG_CHECK_EQ(renderer.LightCount(), 1);
    ENG_CHECK(renderer.GetLight(0) != nullptr);
    ENG_CHECK_EQ(renderer.GetLight(0)->id, 1);

    const Light spot = Light::Spot(Vec3{0, 3, 0}, Vec3{0, -1, 0}, Color::Red, 5.0f, 12.0f,
                                   20.0f * kDeg2Rad, 40.0f * kDeg2Rad);
    const int spotIndex = renderer.AddLight(spot);
    ENG_CHECK_EQ(spotIndex, 1);
    ENG_CHECK(renderer.GetLight(1) != nullptr);
    ENG_CHECK(renderer.GetLight(1)->id == 2);
    ENG_CHECK(renderer.GetLight(0)->id != renderer.GetLight(1)->id);

    // maxLights соблюдается: превышение лимита корректно завершается неудачей.
    renderer.Settings().maxLights = 2;
    ENG_CHECK_EQ(renderer.AddLight(Light::Point(Vec3{0, 1, 0}, Color::White, 1.0f, 5.0f)), -1);
    ENG_CHECK_EQ(renderer.LightCount(), 2);
    renderer.Settings().maxLights = 8;
    ENG_CHECK_EQ(renderer.AddLight(Light::Point(Vec3{0, 1, 0}, Color::White, 1.0f, 5.0f)), 2);
    ENG_CHECK_EQ(renderer.LightCount(), 3);

    renderer.ClearLights();
    ENG_CHECK_EQ(renderer.LightCount(), 0);
    // Id продолжают расти после очистки (они идентифицируют источники, а не слоты).
    const int again = renderer.AddLight(Light::Point(Vec3{0, 1, 0}, Color::White, 1.0f, 5.0f));
    ENG_CHECK_EQ(again, 0);
    ENG_CHECK(renderer.GetLight(0)->id > 3);

    // ---- упаковка ----
    r3d_internal::PackedLights packed;
    ENG_CHECK_EQ(r3d_internal::PackLights(nullptr, 0, 8, &packed), 0);
    ENG_CHECK_EQ(packed.count, 0);

    Light lights[4];
    lights[0] = Light::Directional(Vec3{0, -1, 0}, Color{1, 1, 1, 1}, 3.0f);
    lights[1] = spot;
    lights[2] = Light::Point(Vec3{1, 2, 3}, Color{0, 1, 0, 1}, 4.0f, 7.0f);
    lights[3].type = LightType::Area;
    lights[3].position = Vec3{-2, 4, 0};
    lights[3].direction = Vec3{0, -1, 0};
    lights[3].areaSize = Vec2{2, 3};
    lights[3].color = Color{0.5f, 0.5f, 1.0f, 1.0f};
    lights[3].intensity = 1.5f;

    ENG_CHECK_EQ(r3d_internal::PackLights(lights, 4, 8, &packed), 4);
    ENG_CHECK_EQ(packed.count, 4);
    ENG_CHECK_EQ(packed.type[0], 0);
    ENG_CHECK_EQ(packed.type[1], 2);
    ENG_CHECK_EQ(packed.type[2], 1);
    ENG_CHECK_EQ(packed.type[3], 3);
    ENG_CHECK_NEAR(packed.direction[0].y, -1.0f, 1e-4);
    ENG_CHECK_NEAR(packed.intensity[2], 4.0f, 1e-4);
    ENG_CHECK_NEAR(packed.range[2], 7.0f, 1e-4);
    ENG_CHECK_NEAR(packed.area[3].x, 2.0f, 1e-4);
    ENG_CHECK_NEAR(packed.area[3].y, 3.0f, 1e-4);

    // Конусы spot превращаются в косинусы, сначала внутренний, поэтому x > y, как ждёт шейдер.
    ENG_CHECK_NEAR(packed.cone[1].x, std::cos(20.0f * kDeg2Rad), 1e-4);
    ENG_CHECK_NEAR(packed.cone[1].y, std::cos(40.0f * kDeg2Rad), 1e-4);
    ENG_CHECK(packed.cone[1].x > packed.cone[1].y);
    const Vec2 swapped = r3d_internal::SpotConeCosines(70.0f * kDeg2Rad, 10.0f * kDeg2Rad);
    ENG_CHECK(swapped.x >= swapped.y);

    // Ограничение до maxLights сохраняет первые N источников по порядку.
    ENG_CHECK_EQ(r3d_internal::PackLights(lights, 4, 2, &packed), 2);
    ENG_CHECK_EQ(packed.count, 2);
    ENG_CHECK_EQ(packed.type[0], 0);
    ENG_CHECK_EQ(packed.type[1], 2);
    // Массив шейдера ограничен 8, даже если maxLights больше.
    ENG_CHECK_EQ(r3d_internal::PackLights(lights, 4, 100, &packed), 4);
    ENG_CHECK_EQ(r3d_internal::PackLights(lights, 4, 0, &packed), 0);
}

// ---------------------------------------------------------------------------
// Безопасность без контекста: вызов API без контекста не должен приводить к падению.
// ---------------------------------------------------------------------------
ENG_TEST(Render3D, HeadlessSafety) {
    Renderer3D renderer;
    // Без загруженных точек входа GL Init() должна дать сбой, а не упасть.
    if (!gl::glCreateShader) ENG_CHECK(!renderer.Init());

    MeshData data = MeshData::Cube(1.0f);
    Mesh mesh;
    mesh.Create(data);

    renderer.BeginFrame(TestCamera(), 64, 64, nullptr, true, true);
    renderer.Draw(mesh, Material::Default(), Mat4::Identity());
    renderer.DrawSky();
    renderer.DrawGrid();
    renderer.DrawLine(Vec3{0, 0, 0}, Vec3{1, 0, 0}, Color::Red);
    renderer.DrawGizmo(Mat4::Identity(), 1.0f);
    renderer.DrawAabb(mesh.Bounds(), Color::Green);
    renderer.DrawSphere(Vec3{0, 0, 0}, 1.0f, Color::Blue, 8);
    renderer.DrawCapsule(Vec3{0, 0, 0}, Vec3{0, 2, 0}, 0.4f, Color::Yellow);
    renderer.DrawFrustum(TestCamera().ViewProj(1.0f), Color::White);
    renderer.EndFrame();

    // Статистика остаётся вменяемой; пустой draw (невалидный меш) не должен отправляться.
    ENG_CHECK(renderer.GetStats().drawCalls >= 0);
    renderer.ResetStats();
    ENG_CHECK_EQ(renderer.GetStats().drawCalls, 0);
    ENG_CHECK(renderer.SceneColor() == nullptr);
    renderer.Shutdown();
}

// ---------------------------------------------------------------------------
// GPU-тесты
// ---------------------------------------------------------------------------
namespace {

// Рендерит тестовую сцену с тенями в `rt` и читает пиксели обратно.
bool RenderShadowScene(Renderer3D& renderer, RenderTarget& rt, const Mesh& ground, const Mesh& box,
                       bool shadowsOn, std::vector<u8>* pixels) {
    renderer.Shadows().enabled = shadowsOn;
    renderer.ClearLights();
    if (renderer.AddLight(Light::Directional(Vec3{0.18f, -1.0f, 0.24f}, Color::White, 3.0f, true)) != 0)
        return false;

    Camera cam;
    cam.position = {0, 5.0f, 7.0f};
    cam.target = {0, 0.4f, 0};
    cam.up = {0, 1, 0};
    cam.fovY = 50.0f * kDeg2Rad;
    cam.nearZ = 0.1f;
    cam.farZ = 100.0f;

    Material mat = Material::Default();
    mat.baseColor = Color{0.85f, 0.82f, 0.78f, 1.0f};
    mat.roughness = 0.9f;

    renderer.BeginFrame(cam, rt.Width(), rt.Height(), &rt, true, true);
    renderer.Draw(ground, mat, Mat4::Translate(Vec3{0, -0.1f, 0}) * Mat4::Scale(Vec3{20, 0.2f, 20}));
    renderer.Draw(box, mat, Mat4::Translate(Vec3{0, 1.2f, 0}) * Mat4::Scale(Vec3{1.4f, 1.4f, 1.4f}));
    renderer.EndFrame();
    rt.Unbind();
    return rt.ReadPixels(pixels);
}

}  // namespace

ENG_TEST(Render3D, GpuShadowedFrame) {
    ENG_REQUIRE_GL();

    RenderTargetDesc desc;
    desc.width = 64;
    desc.height = 64;
    desc.samples = 1;
    desc.depth = true;
    desc.colorFormat = PixelFormat::RGBA8;
    desc.depthFormat = PixelFormat::Depth24;
    desc.filter = TextureFilter::Nearest;
    desc.name = "r3d_test";
    RenderTarget rt;
    if (!rt.Create(desc)) ENG_SKIP("render target creation failed");

    MeshData cube = MeshData::Cube(1.0f);
    Mesh ground;
    Mesh box;
    if (!ground.Create(cube) || !box.Create(cube)) ENG_SKIP("mesh creation failed");

    Renderer3D renderer;
    if (!renderer.Init()) ENG_SKIP("Renderer3D::Init failed");
    renderer.Shadows().directionalMapSize = 256;
    renderer.Shadows().cascadeCount = 1;
    renderer.Shadows().cascadeDistance = 30.0f;
    renderer.GetEnvironment().drawSky = false;   // оставляем фон плоским
    renderer.GetEnvironment().drawGrid = false;  // изолируем сравнение теней
    renderer.GetEnvironment().ambientIntensity = 0.15f;

    std::vector<u8> shadowPixels;
    std::vector<u8> litPixels;
    if (!RenderShadowScene(renderer, rt, ground, box, true, &shadowPixels))
        ENG_SKIP("shadow frame readback failed");
    if (!RenderShadowScene(renderer, rt, ground, box, false, &litPixels))
        ENG_SKIP("lit frame readback failed");
    ENG_CHECK_EQ(shadowPixels.size(), litPixels.size());
    if (shadowPixels.size() != litPixels.size()) return;

    // 1. Изображение не является сплошным цветом очистки.
    const u8* bg = &shadowPixels[0];
    const int litCount = CountDifferent(shadowPixels, bg, 8);
    ENG_CHECK_GT(litCount, 64);
    f32 minLum = 1e9f;
    f32 maxLum = -1e9f;
    for (usize i = 0; i + 3 < shadowPixels.size(); i += 4) {
        const f32 l = Luminance(&shadowPixels[i]);
        minLum = MinT(minLum, l);
        maxLum = MaxT(maxLum, l);
    }
    ENG_CHECK_GT(maxLum - minLum, 30.0f);

    // 2. Включение теней должно затемнить сцену хотя бы где-то.
    f32 worstDarkening = 0.0f;
    usize worstIndex = 0;
    usize brightestLitIndex = 0;
    f32 brightestLit = -1.0f;
    f32 brightestShadowed = -1.0f;
    for (usize i = 0; i + 3 < shadowPixels.size(); i += 4) {
        const f32 dark = Luminance(&litPixels[i]) - Luminance(&shadowPixels[i]);
        if (dark > worstDarkening) {
            worstDarkening = dark;
            worstIndex = i;
        }
        if (Luminance(&shadowPixels[i]) > brightestShadowed) brightestShadowed = Luminance(&shadowPixels[i]);
        if (Luminance(&litPixels[i]) > brightestLit) {
            brightestLit = Luminance(&litPixels[i]);
            brightestLitIndex = i;
        }
    }
    ENG_CHECK_GT(worstDarkening, 5.0f);
    // Пиксель, потерявший больше всего света, действительно в тени ...
    ENG_CHECK(Luminance(&shadowPixels[worstIndex]) < Luminance(&litPixels[worstIndex]) - 5.0f);
    // ... а освещённая область затенённого изображения остаётся ярче него.
    ENG_CHECK_GT(Luminance(&shadowPixels[brightestLitIndex]), Luminance(&shadowPixels[worstIndex]) + 5.0f);
    ENG_CHECK_GT(brightestShadowed, 0.0f);

    // 3. Проход теней действительно выполнялся.
    ENG_CHECK_GT(renderer.GetStats().drawCalls, 0);
    renderer.Shutdown();
}

ENG_TEST(Render3D, GpuInstancedDraw) {
    ENG_REQUIRE_GL();

    RenderTargetDesc desc;
    desc.width = 64;
    desc.height = 64;
    desc.samples = 1;
    desc.depth = true;
    desc.colorFormat = PixelFormat::RGBA8;
    desc.depthFormat = PixelFormat::Depth24;
    desc.filter = TextureFilter::Nearest;
    desc.name = "r3d_instanced_test";
    RenderTarget rt;
    if (!rt.Create(desc)) ENG_SKIP("render target creation failed");

    MeshData cubeData = MeshData::Cube(0.5f);
    Mesh cube;
    if (!cube.Create(cubeData)) ENG_SKIP("mesh creation failed");

    Renderer3D renderer;
    if (!renderer.Init()) ENG_SKIP("Renderer3D::Init failed");
    renderer.GetEnvironment().drawSky = false;
    renderer.GetEnvironment().drawGrid = false;

    Camera cam;
    cam.position = {0, 1.0f, 6.0f};
    cam.target = {0, 0, 0};
    cam.fovY = 60.0f * kDeg2Rad;
    cam.nearZ = 0.05f;
    cam.farZ = 50.0f;

    const Material mat = Material::Unlit(Color::White);

    auto renderBatch = [&](const Mat4* transforms, int count, std::vector<u8>* pixels) {
        renderer.BeginFrame(cam, rt.Width(), rt.Height(), &rt, true, true);
        renderer.DrawInstanced(cube, mat, transforms, count);
        renderer.EndFrame();
        rt.Unbind();
        return rt.ReadPixels(pixels);
    };

    const Mat4 single[1] = {Mat4::Identity()};
    Mat4 spread[4] = {Mat4::Translate(Vec3{-2.1f, 0, 0}), Mat4::Translate(Vec3{-0.7f, 0, 0}),
                      Mat4::Translate(Vec3{0.7f, 0, 0}), Mat4::Translate(Vec3{2.1f, 0, 0})};

    std::vector<u8> onePixels;
    std::vector<u8> fourPixels;
    if (!renderBatch(single, 1, &onePixels)) ENG_SKIP("readback failed");
    if (!renderBatch(spread, 4, &fourPixels)) ENG_SKIP("readback failed");
    ENG_CHECK_EQ(onePixels.size(), fourPixels.size());
    if (onePixels.size() != fourPixels.size()) return;

    const u8* bg = &onePixels[0];
    const int coveredOne = CountDifferent(onePixels, bg, 8);
    const int coveredFour = CountDifferent(fourPixels, bg, 8);
    ENG_CHECK_GT(coveredOne, 8);
    ENG_CHECK_GT(coveredFour, coveredOne);
    // Пакет (batch) действительно изменил изображение.
    int changed = 0;
    for (usize i = 0; i + 3 < onePixels.size(); i += 4) {
        for (int c = 0; c < 3; ++c) {
            if (std::abs(static_cast<int>(onePixels[i + c]) - static_cast<int>(fourPixels[i + c])) > 8) {
                ++changed;
                break;
            }
        }
    }
    ENG_CHECK_GT(changed, 8);
    ENG_CHECK_GT(renderer.GetStats().instancedDraws, 0);
    renderer.Shutdown();
}

// Покрывает оставшиеся GPU-пути (небо, сетка, отладочные линии, alpha blend/mask,
// внешние instance-буферы, суб-вьюпорты) и фиксирует любую ошибку GL.
ENG_TEST(Render3D, GpuPipelineSmoke) {
    ENG_REQUIRE_GL();

    RenderTargetDesc desc;
    desc.width = 64;
    desc.height = 64;
    desc.samples = 1;
    desc.depth = true;
    desc.colorFormat = PixelFormat::RGBA8;
    desc.depthFormat = PixelFormat::Depth24;
    desc.filter = TextureFilter::Nearest;
    desc.name = "r3d_smoke_test";
    RenderTarget rt;
    if (!rt.Create(desc)) ENG_SKIP("render target creation failed");

    MeshData cubeData = MeshData::Cube(1.0f);
    Mesh cube;
    Mesh sphere;
    if (!cube.Create(cubeData) || !sphere.Create(MeshData::Sphere(0.6f, 16, 12)))
        ENG_SKIP("mesh creation failed");

    Renderer3D renderer;
    if (!renderer.Init()) ENG_SKIP("Renderer3D::Init failed");
    renderer.Shadows().directionalMapSize = 256;
    renderer.Shadows().cascadeCount = 3;
    renderer.Shadows().cascadeDistance = 40.0f;
    Environment& env = renderer.GetEnvironment();
    env.drawSky = true;
    env.fogEnabled = true;
    env.fogMode = 1;

    Camera cam;
    cam.position = {4, 3, 6};
    cam.target = {0, 0.5f, 0};
    cam.fovY = 55.0f * kDeg2Rad;
    cam.nearZ = 0.1f;
    cam.farZ = 100.0f;

    renderer.AddLight(Light::Directional(Vec3{-0.4f, -1.0f, -0.3f}, Color::White, 3.0f, true));
    renderer.AddLight(Light::Point(Vec3{2, 2, 2}, Color{1, 0.6f, 0.3f, 1}, 12.0f, 8.0f));
    renderer.AddLight(Light::Spot(Vec3{-3, 4, 0}, Vec3{0, -1, 0}, Color{0.4f, 0.7f, 1, 1}, 20.0f, 12.0f,
                                  15.0f * kDeg2Rad, 30.0f * kDeg2Rad, true));
    renderer.Settings().depthPrepass = true;

    std::vector<u8> pixels;
    renderer.BeginFrame(cam, rt.Width(), rt.Height(), &rt, true, true);
    renderer.Draw(cube, Material::Metal(Color{0.9f, 0.8f, 0.4f, 1}, 0.25f), Mat4::Translate(Vec3{-1.2f, 0.5f, 0}));
    Material blend = Material::Default();
    blend.alphaMode = AlphaMode::Blend;
    blend.baseColor = Color{0.2f, 0.6f, 1.0f, 0.45f};
    blend.doubleSided = true;
    renderer.Draw(sphere, blend, Mat4::Translate(Vec3{1.4f, 1.0f, 0}));
    Material masked = Material::Default();
    masked.alphaMode = AlphaMode::Mask;
    renderer.Draw(cube, masked, Mat4::Translate(Vec3{0, 2.5f, 0}) * Mat4::Scale(Vec3{0.4f, 0.4f, 0.4f}));
    renderer.DrawSky();
    renderer.DrawGrid(20.0f, 20);
    renderer.DrawGizmo(Mat4::Translate(Vec3{-1.2f, 0.5f, 0}), 1.0f, false);
    renderer.DrawLine(Vec3{0, 0, 0}, Vec3{0, 3, 0}, Color::Red);
    renderer.DrawSphere(Vec3{0, 0, 0}, 2.0f, Color{0.2f, 0.9f, 0.4f, 1}, 12);
    renderer.DrawCapsule(Vec3{2, 0, 0}, Vec3{2, 2, 0}, 0.4f, Color::Yellow);
    renderer.DrawFrustum(cam.ViewProj(1.0f), Color::Cyan);
    renderer.EndFrame();
    ENG_CHECK(gl::glGetError() == 0);
    ENG_CHECK_GT(renderer.GetStats().drawCalls, 3);
    ENG_CHECK_GT(renderer.GetStats().lines, 0);
    ENG_CHECK_EQ(renderer.GetStats().lights, 3);
    ENG_CHECK_GT(renderer.GetStats().shadowCasters, 0);
    rt.Unbind();
    if (!rt.ReadPixels(&pixels)) ENG_SKIP("readback failed");
    ENG_CHECK_GT(CountDifferent(pixels, &pixels[0], 8), 200);

    // Каркасный режим + отладка каскадов не должны ломаться или вызывать ошибки GL.
    renderer.Settings().wireframe = true;
    renderer.Settings().showShadowCascades = true;
    renderer.BeginFrame(cam, rt.Width(), rt.Height(), &rt, true, true);
    renderer.Draw(cube, Material::Default(), Mat4::Translate(Vec3{0, 0.5f, 0}));
    renderer.EndFrame();
    ENG_CHECK(gl::glGetError() == 0);
    ENG_CHECK_GT(renderer.GetStats().lines, 0);
    renderer.Settings().wireframe = false;
    renderer.Settings().showShadowCascades = false;
    rt.Unbind();

    // Суб-вьюпорт ограничивает и очистку, и отрисовку.
    env.drawSky = false;
    std::vector<u8> outside;
    renderer.SetViewport(0, 0, 32, 32);
    renderer.BeginFrame(cam, rt.Width(), rt.Height(), &rt, true, true);
    renderer.EndFrame();
    rt.Unbind();
    if (!rt.ReadPixels(&outside)) ENG_SKIP("readback failed");
    renderer.BeginFrame(cam, rt.Width(), rt.Height(), &rt, true, true);
    renderer.Draw(cube, Material::Unlit(Color::White),
                  Mat4::Translate(Vec3{0, 0.5f, 0}) * Mat4::Scale(Vec3{8, 8, 8}));
    renderer.EndFrame();
    ENG_CHECK(gl::glGetError() == 0);
    rt.Unbind();
    std::vector<u8> vp;
    if (!rt.ReadPixels(&vp)) ENG_SKIP("readback failed");
    renderer.ResetViewport();
    // Прямоугольники viewport отсчитываются сверху вниз (y-down), поэтому (0,0,32,32) —
    // верхний левый квадрант изображения, т.е. строки 32..63 bottom-up буфера glReadPixels.
    int changedInside = 0;
    int changedOutside = 0;
    for (int y = 0; y < 64; ++y) {
        for (int x = 0; x < 64; ++x) {
            const usize i = static_cast<usize>(y * 64 + x) * 4;
            bool diff = false;
            for (int c = 0; c < 3; ++c)
                if (std::abs(static_cast<int>(vp[i + c]) - static_cast<int>(outside[i + c])) > 6) diff = true;
            if (y >= 32 && x < 32) {
                if (diff) ++changedInside;
            } else if (diff) {
                ++changedOutside;
            }
        }
    }
    ENG_CHECK_GT(changedInside, 400);
    ENG_CHECK_EQ(changedOutside, 0);

    // Внешне управляемый instance-буфер (4 столбца vec4 на экземпляр).
    env.drawSky = false;
    u32 instanceVbo = 0;
    gl::glGenBuffers(1, &instanceVbo);
    ENG_CHECK(instanceVbo != 0);
    if (instanceVbo != 0) {
        Mat4 instances[4] = {Mat4::Translate(Vec3{-2.2f, 0.5f, 0}), Mat4::Translate(Vec3{-0.7f, 0.5f, 0}),
                             Mat4::Translate(Vec3{0.7f, 0.5f, 0}), Mat4::Translate(Vec3{2.2f, 0.5f, 0})};
        gl::glBindBuffer(gl::GL_ARRAY_BUFFER, instanceVbo);
        gl::glBufferData(gl::GL_ARRAY_BUFFER, sizeof(instances), instances, gl::GL_STATIC_DRAW);
        gl::glBindBuffer(gl::GL_ARRAY_BUFFER, 0);
        renderer.BeginFrame(cam, rt.Width(), rt.Height(), &rt, true, true);
        renderer.DrawInstancedBuffer(cube, Material::Unlit(Color::White), instanceVbo, 4);
        renderer.EndFrame();
        ENG_CHECK(gl::glGetError() == 0);
        rt.Unbind();
        std::vector<u8> batched;
        if (!rt.ReadPixels(&batched)) ENG_SKIP("readback failed");
        ENG_CHECK_GT(CountDifferent(batched, &batched[0], 8), 4 * 8);
        gl::glDeleteBuffers(1, &instanceVbo);
    }
    renderer.Shutdown();
}
