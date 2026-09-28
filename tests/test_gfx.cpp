// Тесты графического модуля: MeshData/Mesh, Texture, Shader, RenderTarget,
// Renderer2D (контуры, заливки, обводки, изображения, текст, клиппинг) и постобработка.
#include "crossrender/gfx/GL.h"
#include "crossrender/gfx/Mesh.h"
#include "crossrender/core/File.h"
#include "crossrender/test/Test.h"
#include "crossrender/text/Font.h"
#include "crossrender/gfx/Shader.h"
#include "crossrender/gfx/Texture.h"
#include "crossrender/gfx/Renderer2D.h"
#include "crossrender/platform/Window.h"
#include "crossrender/gfx/RenderTarget.h"

#include <cmath>

using namespace crossrender;

namespace {

// Читает пиксель из render target как RGBA8 (начало координат внизу слева).
bool ReadTargetPixel(RenderTarget& rt, int x, int y, u8 out[4]) {
    std::vector<u8> pixels;
    if (!rt.ReadPixels(&pixels)) return false;
    // ReadPixels возвращает строки с началом сверху слева, потому что читает сырой
    // GL-буфер снизу вверх; преобразуем к индексации сверху слева.
    usize idx = (static_cast<usize>(rt.Height() - 1 - y) * rt.Width() + x) * 4;
    if (idx + 3 >= pixels.size()) return false;
    out[0] = pixels[idx + 0];
    out[1] = pixels[idx + 1];
    out[2] = pixels[idx + 2];
    out[3] = pixels[idx + 3];
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// MeshData (только CPU)
// ---------------------------------------------------------------------------
ENG_TEST(MeshData, PrimitiveBoundsAndCounts) {
    MeshData cube = MeshData::Cube(2.0f);
    ENG_CHECK_EQ(cube.vertices.size(), 24u);
    ENG_CHECK_EQ(cube.indices.size(), 36u);
    cube.ComputeBounds();
    ENG_CHECK_NEAR(cube.bounds.min.x, -1.0f, 1e-4f);
    ENG_CHECK_NEAR(cube.bounds.max.y, 1.0f, 1e-4f);

    MeshData sphere = MeshData::Sphere(3.0f, 24, 16);
    ENG_CHECK(sphere.vertices.size() > 100);
    sphere.ComputeBounds();
    // Вершины сферы лежат ровно на радиусе; Radius() - половина диагонали
    // AABB, поэтому проверяем экстенты.
    Vec3 e = sphere.bounds.Extents();
    ENG_CHECK_NEAR(e.x, 3.0f, 0.02f);
    ENG_CHECK_NEAR(e.y, 3.0f, 0.02f);
    ENG_CHECK_NEAR(e.z, 3.0f, 0.02f);

    MeshData plane = MeshData::Plane(4.0f, 6.0f, 2);
    ENG_CHECK_EQ(plane.vertices.size(), 9u);
    ENG_CHECK_EQ(plane.indices.size(), 24u);
    for (const Vertex& v : plane.vertices) {
        ENG_CHECK_NEAR(v.normal.x, 0.0f, 1e-6f);
        ENG_CHECK_NEAR(v.normal.y, 1.0f, 1e-6f);
    }

    MeshData cone = MeshData::Cone(1.0f, 2.0f, 12);
    cone.ComputeBounds();
    ENG_CHECK_NEAR(cone.bounds.max.y, 1.0f, 1e-4f);
    ENG_CHECK_NEAR(cone.bounds.min.y, -1.0f, 1e-4f);

    MeshData torus = MeshData::Torus(2.0f, 0.5f, 16, 8);
    torus.ComputeBounds();
    ENG_CHECK_NEAR(torus.bounds.max.x, 2.5f, 0.05f);

    MeshData capsule = MeshData::Capsule(0.5f, 2.0f, 12, 8);
    capsule.ComputeBounds();
    ENG_CHECK_NEAR(capsule.bounds.max.y, 1.0f, 0.05f);

    MeshData ico = MeshData::IcoSphere(1.0f, 1);
    ico.ComputeBounds();
    ENG_CHECK_NEAR(ico.bounds.Extents().x, 1.0f, 0.02f);
    ENG_CHECK_NEAR(ico.bounds.Extents().y, 1.0f, 0.02f);
}

ENG_TEST(MeshData, NormalsAreUnitLength) {
    MeshData sphere = MeshData::Sphere(1.0f, 16, 12);
    sphere.ComputeNormals(true);
    for (const Vertex& v : sphere.vertices) ENG_CHECK_NEAR(Length(v.normal), 1.0f, 1e-3f);

    MeshData cube = MeshData::Cube(1.0f, false);
    cube.ComputeNormals(false);
    for (const Vertex& v : cube.vertices) ENG_CHECK_NEAR(Length(v.normal), 1.0f, 1e-3f);
    // Плоское затенение: все три вершины грани имеют общую нормаль.
    ENG_CHECK_NEAR(Dot(cube.vertices[0].normal, cube.vertices[1].normal), 1.0f, 1e-4f);
}

ENG_TEST(MeshData, TangentsAreOrthogonalToNormals) {
    MeshData sphere = MeshData::Sphere(1.0f, 16, 12);
    sphere.ComputeTangents();
    int checked = 0;
    for (const Vertex& v : sphere.vertices) {
        Vec3 t = v.tangent.xyz();
        if (LengthSq(t) < 0.5f) continue;
        ENG_CHECK_NEAR(Dot(t, v.normal), 0.0f, 1e-2f);
        ++checked;
    }
    ENG_CHECK(checked > 10);
}

ENG_TEST(MeshData, MergeCombinesIndices) {
    MeshData a = MeshData::Quad(1, 1);
    MeshData b = MeshData::Quad(1, 1);
    MeshData m = MeshData::Merge({a, b});
    ENG_CHECK_EQ(m.vertices.size(), a.vertices.size() + b.vertices.size());
    ENG_CHECK_EQ(m.indices.size(), a.indices.size() + b.indices.size());
    u32 maxIndex = 0;
    for (u32 i : m.indices) maxIndex = MaxT(maxIndex, i);
    ENG_CHECK(maxIndex < m.vertices.size());
}

// ---------------------------------------------------------------------------
// Texture
// ---------------------------------------------------------------------------
namespace {
std::vector<u8> MakePng(int w, int h, u8 r, u8 g, u8 b, const char* name = "gfx_test.png") {
    std::vector<u8> pixels(static_cast<usize>(w) * h * 4);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            usize i = (static_cast<usize>(y) * w + x) * 4;
            pixels[i + 0] = static_cast<u8>(r + x);
            pixels[i + 1] = static_cast<u8>(g + y);
            pixels[i + 2] = b;
            pixels[i + 3] = 255;
        }
    }
    std::string path = test::TempFilePath(name);
    if (!Texture::EncodePng(path, w, h, 4, pixels.data())) return {};
    return ReadBinaryFile(path);
}
}  // namespace

ENG_TEST(Texture, DecodePngFromMemory) {
    std::vector<u8> png = MakePng(16, 8, 10, 20, 30);
    ENG_CHECK(!png.empty());
    Texture::ImageData img;
    ENG_CHECK(Texture::DecodeImage(png.data(), png.size(), &img, false));
    ENG_CHECK_EQ(img.width, 16);
    ENG_CHECK_EQ(img.height, 8);
    ENG_CHECK_EQ(img.channels, 4);
    ENG_CHECK_EQ(img.pixels.size(), 16u * 8u * 4u);
    ENG_CHECK_EQ(static_cast<int>(img.pixels[0]), 10);
    ENG_CHECK_EQ(static_cast<int>(img.pixels[1]), 20);
}

ENG_TEST(Texture, DecodeRejectsGarbage) {
    const char junk[] = "this is definitely not an image file at all";
    Texture::ImageData img;
    ENG_CHECK(!Texture::DecodeImage(junk, sizeof(junk), &img, false));
}

ENG_TEST(Texture, CreateAndUpdate) {
    ENG_REQUIRE_GL();
    Texture tex;
    ENG_CHECK(tex.Create(8, 4, PixelFormat::RGBA8));
    ENG_CHECK(tex.Valid());
    ENG_CHECK_EQ(tex.Width(), 8);
    ENG_CHECK_EQ(tex.Height(), 4);
    ENG_CHECK_EQ(tex.Format(), PixelFormat::RGBA8);

    std::vector<u8> px(8 * 4 * 4, 200);
    tex.Update(px.data());
    tex.SetFilter(TextureFilter::Nearest);
    tex.SetWrap(TextureWrap::Repeat);
    tex.Destroy();
    ENG_CHECK(!tex.Valid());
}

ENG_TEST(Texture, LoadFromPngMemory) {
    ENG_REQUIRE_GL();
    std::vector<u8> png = MakePng(32, 32, 5, 6, 7);
    Texture tex;
    ENG_CHECK(tex.LoadFromMemory(png.data(), png.size()));
    ENG_CHECK_EQ(tex.Width(), 32);
    ENG_CHECK(tex.Valid());
}

ENG_TEST(Texture, SolidAndCheckerboard) {
    ENG_REQUIRE_GL();
    Texture solid;
    ENG_CHECK(solid.CreateSolid(Color::Red));
    ENG_CHECK_EQ(solid.Width(), 1);
    Texture checker;
    ENG_CHECK(checker.CreateCheckerboard(32));
    ENG_CHECK_EQ(checker.Width(), 32);
}

ENG_TEST(Texture, ThreeDAndCubemap) {
    ENG_REQUIRE_GL();
    Texture vol;
    std::vector<u8> voxels(4 * 4 * 4 * 4, 128);
    ENG_CHECK(vol.Create3D(4, 4, 4, PixelFormat::RGBA8, voxels.data()));
    ENG_CHECK_EQ(vol.Depth(), 4);
    vol.Destroy();

    Texture cube;
    std::vector<u8> face(16 * 16 * 4, 255);
    const void* faces[6] = {face.data(), face.data(), face.data(),
                            face.data(), face.data(), face.data()};
    ENG_CHECK(cube.CreateCubemap(16, PixelFormat::RGBA8, faces));
    ENG_CHECK(cube.IsCubemap());
    cube.Destroy();
}

ENG_TEST(Texture, EncodePngFile) {
    std::vector<u8> pixels(8 * 8 * 4, 90);
    std::string path = test::TempFilePath("gfx_encode.png");
    ENG_CHECK(Texture::EncodePng(path, 8, 8, 4, pixels.data()));
    Texture::ImageData img;
    ENG_CHECK(Texture::DecodeImageFile(path, &img, false));
    ENG_CHECK_EQ(img.width, 8);
    ENG_CHECK_EQ(img.height, 8);
}

// ---------------------------------------------------------------------------
// Shader
// ---------------------------------------------------------------------------
ENG_TEST(Shader, BuildBuiltins) {
    ENG_REQUIRE_GL();
    Shader sprite;
    ENG_CHECK(sprite.Build(builtin::kSpriteVert, builtin::kSpriteFrag, "test-sprite"));
    ENG_CHECK(sprite.Valid());
    sprite.Bind();
    sprite.Set("uViewProj", Mat4::Identity());
    sprite.Set("uType", 0);
    sprite.Set("uInnerColor", Color::White);
    sprite.Set("uGradA", Vec2{0, 0});
    sprite.Set("uRadiusA", 1.0f);
    Shader::Unbind();

    Shader sdf;
    ENG_CHECK(sdf.Build(builtin::kSdfTextVert, builtin::kSdfTextFrag, "test-sdf"));
    ENG_CHECK(sdf.Valid());

    Shader forward;
    ENG_CHECK(forward.Build(builtin::kForwardVert, builtin::kForwardFrag, "test-forward"));
    ENG_CHECK(forward.Valid());

    Shader shadow;
    ENG_CHECK(shadow.Build(builtin::kShadowVert, builtin::kShadowFrag, "test-shadow"));

    Shader post;
    ENG_CHECK(post.Build(builtin::kPostVert, builtin::kTonemapFrag, "test-tonemap"));
}

ENG_TEST(Shader, CompileErrorIsReported) {
    ENG_REQUIRE_GL();
    Shader bad;
    bool ok = bad.Build("this is not glsl", "neither is this", "bad");
    ENG_CHECK(!ok);
    ENG_CHECK(!bad.Valid());
    ENG_CHECK(!bad.Log().empty());
}

ENG_TEST(Shader, SetUniformsDoNotCrashWithoutBind) {
    ENG_REQUIRE_GL();
    Shader s;
    ENG_CHECK(s.Build(builtin::kSpriteVert, builtin::kSpriteFrag, "uniforms"));
    // Установка неизвестного uniform должна быть тихим no-op.
    s.Set("uDoesNotExist", 1.0f);
    s.Set("uType", 2);
    ENG_CHECK(s.UniformLocation("uDoesNotExist") < 0);
}

// ---------------------------------------------------------------------------
// RenderTarget
// ---------------------------------------------------------------------------
ENG_TEST(RenderTarget, ClearAndReadBack) {
    ENG_REQUIRE_GL();
    RenderTarget rt;
    RenderTargetDesc desc;
    desc.width = 32;
    desc.height = 32;
    desc.colorFormat = PixelFormat::RGBA8;
    desc.name = "test-rt";
    ENG_CHECK(rt.Create(desc));
    rt.Bind();
    rt.Clear(Color::FromBytes(255, 0, 0, 255), true, false);
    rt.Unbind();

    u8 px[4] = {0, 0, 0, 0};
    ENG_CHECK(ReadTargetPixel(rt, 16, 16, px));
    ENG_CHECK(px[0] > 250);
    ENG_CHECK(px[1] < 5);
    ENG_CHECK(px[2] < 5);
    ENG_CHECK(px[3] > 250);
}

ENG_TEST(RenderTarget, ResizeRecreates) {
    ENG_REQUIRE_GL();
    RenderTarget rt;
    RenderTargetDesc desc;
    desc.width = 16;
    desc.height = 16;
    ENG_CHECK(rt.Create(desc));
    ENG_CHECK(rt.Resize(64, 32));
    ENG_CHECK_EQ(rt.Width(), 64);
    ENG_CHECK_EQ(rt.Height(), 32);
}

ENG_TEST(RenderTarget, MultipleColorAttachments) {
    ENG_REQUIRE_GL();
    RenderTarget rt;
    RenderTargetDesc desc;
    desc.width = 16;
    desc.height = 16;
    desc.colorAttachments = 2;
    desc.depth = false;
    ENG_CHECK(rt.Create(desc));
    ENG_CHECK_EQ(rt.ColorAttachmentCount(), 2);
    ENG_CHECK(rt.TextureAt(1).Valid());
}

ENG_TEST(RenderTarget, ResolveMsaa) {
    ENG_REQUIRE_GL();
    gl::GLint maxSamples = 0;
    gl::glGetIntegerv(gl::GL_MAX_SAMPLES, &maxSamples);
    if (maxSamples < 4) ENG_SKIP("MSAA unavailable");
    RenderTarget rt;
    RenderTargetDesc desc;
    desc.width = 32;
    desc.height = 32;
    desc.samples = 4;
    desc.name = "test-msaa";
    ENG_CHECK(rt.Create(desc));
    rt.Bind();
    rt.Clear(Color::FromBytes(0, 255, 0, 255), true, false);
    rt.Unbind();
    u8 px[4] = {0, 0, 0, 0};
    ENG_CHECK(ReadTargetPixel(rt, 16, 16, px));
    ENG_CHECK(px[1] > 250);
}

ENG_TEST(RenderTarget, ScopedBindingRestoresDefault) {
    ENG_REQUIRE_GL();
    RenderTarget rt;
    RenderTargetDesc desc;
    desc.width = 8;
    desc.height = 8;
    ENG_CHECK(rt.Create(desc));
    {
        ScopedRenderTarget scope(rt);
        rt.Clear(Color::Blue, true, false);
    }
    // После scope должен снова быть привязан фреймбуфер по умолчанию.
    gl::GLint bound = -1;
    gl::glGetIntegerv(gl::GL_FRAMEBUFFER_BINDING, &bound);
    ENG_CHECK_EQ(bound, 0);
}

// ---------------------------------------------------------------------------
// Renderer2D
// ---------------------------------------------------------------------------
namespace {
struct R2DFixture {
    RenderTarget rt;
    Renderer2D r2d;
    bool ok = false;

    bool Init(int w = 64, int h = 64) {
        RenderTargetDesc desc;
        desc.width = w;
        desc.height = h;
        desc.colorFormat = PixelFormat::RGBA8;
        desc.depth = false;
        desc.name = "r2d-fixture";
        if (!rt.Create(desc)) return false;
        if (!r2d.Init()) return false;
        ok = true;
        return true;
    }
    void Begin() {
        rt.Bind();
        rt.Clear(Color::Black, false, false);
        r2d.BeginFrame(rt.Width(), rt.Height(), 1.0f, &rt);
    }
    void End() {
        r2d.EndFrame();
        rt.Unbind();
    }
    bool Pixel(int x, int y, u8 out[4]) { return ReadTargetPixel(rt, x, y, out); }
};
}  // namespace

ENG_TEST(Renderer2D, FillRectCoversPixels) {
    ENG_REQUIRE_GL();
    R2DFixture f;
    if (!f.Init()) ENG_SKIP("no GL context");
    f.Begin();
    f.r2d.FillColor(Color::Red);
    f.r2d.BeginPath();
    f.r2d.Rect(8, 8, 32, 32);
    f.r2d.Fill();
    f.End();

    u8 inside[4] = {0, 0, 0, 0};
    ENG_CHECK(f.Pixel(24, 24, inside));
    ENG_CHECK(inside[0] > 200);
    ENG_CHECK(inside[1] < 60);
    u8 outside[4] = {0, 0, 0, 0};
    ENG_CHECK(f.Pixel(2, 2, outside));
    ENG_CHECK(outside[0] < 60);
    ENG_CHECK_GT(f.r2d.GetStats().paths, 0);
}

ENG_TEST(Renderer2D, StateStackRestoresColor) {
    ENG_REQUIRE_GL();
    R2DFixture f;
    if (!f.Init()) ENG_SKIP("no GL context");
    f.Begin();
    f.r2d.FillColor(Color::Green);
    f.r2d.Save();
    f.r2d.FillColor(Color::Blue);
    f.r2d.Restore();
    f.r2d.BeginPath();
    f.r2d.Rect(0, 0, 64, 64);
    f.r2d.Fill();
    f.End();

    u8 px[4] = {0, 0, 0, 0};
    ENG_CHECK(f.Pixel(32, 32, px));
    ENG_CHECK(px[1] > 200);   // зелёный восстановлен
    ENG_CHECK(px[2] < 80);
}

ENG_TEST(Renderer2D, GlobalAlphaBlends) {
    ENG_REQUIRE_GL();
    R2DFixture f;
    if (!f.Init()) ENG_SKIP("no GL context");
    f.Begin();
    f.r2d.GlobalAlpha(0.5f);
    f.r2d.FillRect(0, 0, 64, 64, Color::White);
    f.End();
    u8 px[4] = {0, 0, 0, 0};
    ENG_CHECK(f.Pixel(32, 32, px));
    ENG_CHECK(px[0] > 100 && px[0] < 160);
}

ENG_TEST(Renderer2D, TransformRotatesGeometry) {
    ENG_REQUIRE_GL();
    R2DFixture f;
    if (!f.Init()) ENG_SKIP("no GL context");
    f.Begin();
    f.r2d.Save();
    f.r2d.Translate(32, 32);
    f.r2d.Rotate(Radians(45.0f));
    f.r2d.FillRect(-20, -3, 40, 6, Color::Yellow);
    f.r2d.Restore();
    f.End();
    u8 px[4] = {0, 0, 0, 0};
    // Повёрнутая полоса пересекает центр и достигает диагоналей.
    ENG_CHECK(f.Pixel(32, 32, px));
    ENG_CHECK(px[0] > 150 && px[1] > 150);
    u8 corner[4] = {0, 0, 0, 0};
    ENG_CHECK(f.Pixel(2, 2, corner));
    ENG_CHECK(corner[0] < 60);
}

ENG_TEST(Renderer2D, ClipRectHidesGeometry) {
    ENG_REQUIRE_GL();
    R2DFixture f;
    if (!f.Init()) ENG_SKIP("no GL context");
    f.Begin();
    f.r2d.Save();
    f.r2d.ClipRect(0, 0, 16, 64);
    f.r2d.FillRect(0, 0, 64, 64, Color::Cyan);
    f.r2d.Restore();
    f.End();
    u8 inside[4] = {0, 0, 0, 0};
    ENG_CHECK(f.Pixel(8, 32, inside));
    ENG_CHECK(inside[2] > 200);
    u8 outside[4] = {0, 0, 0, 0};
    ENG_CHECK(f.Pixel(48, 32, outside));
    ENG_CHECK(outside[2] < 60);
}

ENG_TEST(Renderer2D, RoundedRectAndCircle) {
    ENG_REQUIRE_GL();
    R2DFixture f;
    if (!f.Init()) ENG_SKIP("no GL context");
    f.Begin();
    f.r2d.FillRoundedRect(Rect{4, 4, 24, 24}, 8.0f, Color::Magenta);
    f.r2d.FillCircle(48, 48, 10, Color::FromRGB(0x00FF80));
    f.End();

    u8 centre[4] = {0, 0, 0, 0};
    ENG_CHECK(f.Pixel(16, 16, centre));
    ENG_CHECK(centre[0] > 150 && centre[2] > 150);
    u8 corner[4] = {0, 0, 0, 0};
    ENG_CHECK(f.Pixel(5, 5, corner));
    ENG_CHECK(corner[0] < 100);   // скруглённый угол не залит
    u8 circle[4] = {0, 0, 0, 0};
    ENG_CHECK(f.Pixel(48, 48, circle));
    ENG_CHECK(circle[1] > 200);
}

ENG_TEST(Renderer2D, GradientAndStroke) {
    ENG_REQUIRE_GL();
    R2DFixture f;
    if (!f.Init()) ENG_SKIP("no GL context");
    f.Begin();
    f.r2d.FillRectGradient(Rect{0, 0, 64, 64}, Color::Black, Color::White, true);
    f.r2d.StrokeRect(Rect{8, 8, 48, 48}, Color::Red, 2.0f);
    f.End();
    u8 top[4] = {0, 0, 0, 0};
    u8 bottom[4] = {0, 0, 0, 0};
    ENG_CHECK(f.Pixel(48, 2, top));
    ENG_CHECK(f.Pixel(48, 61, bottom));
    ENG_CHECK(bottom[0] > top[0] + 60);
    u8 onStroke[4] = {0, 0, 0, 0};
    bool foundRed = false;
    for (int y = 6; y < 14 && !foundRed; ++y) {
        if (f.Pixel(32, y, onStroke) && onStroke[0] > 150 && onStroke[1] < 100) foundRed = true;
    }
    ENG_CHECK(foundRed);
}

ENG_TEST(Renderer2D, ImageDrawsTexture) {
    ENG_REQUIRE_GL();
    R2DFixture f;
    if (!f.Init()) ENG_SKIP("no GL context");
    Texture tex;
    ENG_CHECK(tex.CreateSolid(Color::FromBytes(0, 128, 255, 255)));
    f.Begin();
    f.r2d.Image(tex, Rect{0, 0, 64, 64}, Rect{0, 0, 1, 1}, Color::White);
    f.End();
    u8 px[4] = {0, 0, 0, 0};
    ENG_CHECK(f.Pixel(32, 32, px));
    ENG_CHECK(px[1] > 100 && px[2] > 200);
}

ENG_TEST(Renderer2D, NinePatchScales) {
    ENG_REQUIRE_GL();
    R2DFixture f;
    if (!f.Init()) ENG_SKIP("no GL context");
    Texture tex;
    ENG_CHECK(tex.Create(12, 12, PixelFormat::RGBA8));
    std::vector<u8> px(12 * 12 * 4, 255);
    tex.Update(px.data());
    f.Begin();
    f.r2d.Image9(tex, Rect{0, 0, 64, 64}, NinePatch::Uniform(4.0f), Color::White);
    f.End();
    u8 c[4] = {0, 0, 0, 0};
    ENG_CHECK(f.Pixel(32, 32, c));
    ENG_CHECK(c[0] > 200);
    ENG_CHECK_GT(f.r2d.GetStats().drawCalls, 0);
}

ENG_TEST(Renderer2D, TextRendersInk) {
    ENG_REQUIRE_GL();
    R2DFixture f;
    if (!f.Init(128, 64)) ENG_SKIP("no GL context");

    // Загружаем шрифт *после* появления GL-контекста, чтобы его атлас получил
    // настоящую текстуру; при отсутствии файла шрифта откатываемся к процедурному.
    Font loaded;
    Font* font = nullptr;
    for (const char* name : {"fonts/engine.ttf", "fonts/ubuntu.ttf"}) {
        const std::string path = PathJoin(GetAssetRoot(), name);
        if (!FileExists(path)) continue;
        FontDesc desc;
        desc.pixelHeight = 36.0f;
        if (loaded.LoadFromFile(path, desc)) {
            font = &loaded;
            break;
        }
    }
    if (!font) {
        font = FontManager::Get().DefaultFont();
        if (!font || !font->Valid()) ENG_SKIP("no font available");
        ENG_SKIP("no font file staged (run the asset generator)");
    }
    ENG_CHECK(font->Valid());

    f.Begin();
    f.r2d.DrawText(*font, "Hello Дурак", 4, 4, Color::White, 24.0f, TextAlign::Left,
                   TextBaseline::Top);
    f.End();
    ENG_CHECK_GT(f.r2d.GetStats().textGlyphs, 0);

    std::vector<u8> pixels;
    ENG_CHECK(f.rt.ReadPixels(&pixels));
    int lit = 0;
    for (usize i = 0; i + 3 < pixels.size(); i += 4) {
        if (pixels[i] > 100) ++lit;
    }
    ENG_CHECK_GT(lit, 20);
}

ENG_TEST(Renderer2D, MeasureWrapEllipsize) {
    Font* font = FontManager::Get().DefaultFont();
    if (!font || !font->Valid()) ENG_SKIP("no default font");
    TextMetrics m = MeasureText(*font, "Hello", 24.0f);
    ENG_CHECK(m.width > 0);
    ENG_CHECK(m.height > 0);
    TextMetrics m2 = MeasureText(*font, "HelloHello", 24.0f);
    ENG_CHECK(m2.width > m.width);
    ENG_CHECK_EQ(MeasureText(*font, "", 24.0f).width, 0.0f);

    auto lines = WrapText(*font, "one two three four five six seven eight", 80.0f, 16.0f);
    ENG_CHECK(lines.size() >= 3);
    for (const std::string& l : lines) ENG_CHECK(MeasureText(*font, l, 16.0f).width <= 90.0f);

    std::string ell = EllipsizeText(*font, "Hello wonderful world", 40.0f, 16.0f);
    ENG_CHECK(ell.size() < std::string("Hello wonderful world").size());
    ENG_CHECK(MeasureText(*font, ell, 16.0f).width <= 45.0f);
}

ENG_TEST(Renderer2D, BatchCoalescing) {
    ENG_REQUIRE_GL();
    R2DFixture f;
    if (!f.Init(128, 128)) ENG_SKIP("no GL context");
    f.Begin();
    for (int i = 0; i < 200; ++i) {
        f.r2d.FillRect(static_cast<f32>(i % 16) * 8, static_cast<f32>(i / 16) * 8, 6, 6,
                       Color::White);
    }
    f.End();
    // 200 одинаковых белых квадов должны сжаться в очень мало draw calls.
    ENG_CHECK(f.r2d.GetStats().drawCalls > 0);
    ENG_CHECK(f.r2d.GetStats().drawCalls <= 4);
}

ENG_TEST(Renderer2D, FrameResetClearsStats) {
    ENG_REQUIRE_GL();
    R2DFixture f;
    if (!f.Init()) ENG_SKIP("no GL context");
    f.Begin();
    f.r2d.FillRect(0, 0, 10, 10, Color::White);
    f.End();
    ENG_CHECK_GT(f.r2d.GetStats().paths, 0);
    f.Begin();
    ENG_CHECK_EQ(f.r2d.GetStats().paths, 0);
    f.End();
}

// ---------------------------------------------------------------------------
// Постобработка
// ---------------------------------------------------------------------------
ENG_TEST(PostProcess, AppliesTonemapAndBloom) {
    ENG_REQUIRE_GL();
    PostProcessor post;
    if (!post.Init(64, 64)) ENG_SKIP("post processing unavailable");
    if (!post.SceneTarget().Valid()) ENG_SKIP("no scene target");

    post.SceneTarget().Bind();
    post.SceneTarget().Clear(Color{4.0f, 4.0f, 4.0f, 1.0f}, true, false);
    post.SceneTarget().Unbind();

    PostProcessSettings settings;
    settings.bloom = true;
    settings.bloomThreshold = 1.0f;
    settings.tonemap = true;
    settings.exposure = 1.0f;
    settings.fxaa = false;
    post.Apply(post.SceneTarget().ColorTexture(), settings, 64, 64);
    // Без падения, и фреймбуфер по умолчанию снова привязан.
    gl::GLint bound = -1;
    gl::glGetIntegerv(gl::GL_FRAMEBUFFER_BINDING, &bound);
    ENG_CHECK_EQ(bound, 0);
}

// ---------------------------------------------------------------------------
// Контроллер камеры
// ---------------------------------------------------------------------------
ENG_TEST(CameraController, OrbitChangesPosition) {
    Camera cam;
    CameraController ctrl;
    Input input;
    input.BeginFrame();
    ctrl.SetOrbit(0.0f, 0.0f, 10.0f, {0, 0, 0});
    ctrl.Update(cam, input, 1.0f / 60.0f, true);
    ENG_CHECK_NEAR(cam.position.z, 10.0f, 0.01f);

    ctrl.Orbit(100.0f, 0.0f);
    ctrl.Update(cam, input, 1.0f / 60.0f, true);
    ENG_CHECK(std::fabs(cam.position.x) > 0.1f);

    ctrl.Zoom(5.0f);
    ctrl.Update(cam, input, 1.0f / 60.0f, true);
    ENG_CHECK(ctrl.Distance() < 10.0f);

    ctrl.Pan(10.0f, 10.0f);
    ctrl.Update(cam, input, 1.0f / 60.0f, true);
    Vec3 t = ctrl.Target();
    ENG_CHECK(LengthSq(t) > 1e-6f);
}
