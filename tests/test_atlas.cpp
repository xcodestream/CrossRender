// Тесты SpriteAtlas: разбор дескрипторов всех поддерживаемых форматов, shelf-
// пакер, анимации и помощники отрисовки.
//
// Тесты разбора/упаковки сначала работают на CPU (GL-контекст не нужен); тесты
// отрисовки запрашивают контекст через ENG_REQUIRE_GL() и рендерят в RenderTarget.
#include "crossrender/core/File.h"
#include "crossrender/test/Test.h"
#include "crossrender/gfx/Texture.h"
#include "crossrender/gfx/Renderer2D.h"
#include "crossrender/gfx/SpriteAtlas.h"
#include "crossrender/gfx/RenderTarget.h"

#include "gfx/SpriteAtlasInternal.h"

#include <cmath>
#include <string>
#include <vector>

using namespace crossrender;

namespace {

// ---------------------------------------------------------------------------
// Фикстуры
// ---------------------------------------------------------------------------
// Engine JSON: то, что пишет SaveToFile (hash TexturePacker + анимации).
const char* kEngineJson = R"JSON({
  "meta": { "app": "CrossRender", "version": "1", "image": "engine_missing.png",
            "size": {"w": 128, "h": 64}, "scale": "1" },
  "frames": {
    "player_idle_0": {
      "frame": {"x":0,"y":0,"w":32,"h":32},
      "rotated": false, "trimmed": false,
      "spriteSourceSize": {"x":0,"y":0,"w":32,"h":32},
      "sourceSize": {"w":32,"h":32},
      "pivot": {"x":0.5,"y":0.5},
      "duration": 120,
      "polygon": [{"x":0,"y":0},{"x":0.5,"y":0},{"x":1,"y":0.5}]
    },
    "player_idle_1": {
      "frame": {"x":32,"y":0,"w":24,"h":40},
      "rotated": true, "trimmed": true,
      "spriteSourceSize": {"x":2,"y":4,"w":40,"h":24},
      "sourceSize": {"w":42,"h":28},
      "pivot": {"x":0.25,"y":0.75},
      "duration": 80
    }
  },
  "animations": [ { "name": "idle", "frames": ["player_idle_0","player_idle_1"],
                    "durations": [120,80], "loop": true } ]
})JSON";

const char* kTexturePackerHash = R"JSON({
  "frames": {
    "hero_run_0": { "frame": {"x":0,"y":0,"w":16,"h":24}, "rotated": false, "trimmed": true,
                    "spriteSourceSize": {"x":1,"y":2,"w":16,"h":24}, "sourceSize": {"w":18,"h":28},
                    "pivot": {"x":0.5,"y":1.0} },
    "hero_run_1": { "frame": {"x":16,"y":0,"w":16,"h":24}, "rotated": false, "trimmed": false,
                    "spriteSourceSize": {"x":0,"y":0,"w":16,"h":24}, "sourceSize": {"w":16,"h":24} }
  },
  "meta": { "app": "http://www.codeandweb.com/texturepacker", "version": "1.0",
            "image": "tp_missing.png", "format": "RGBA8888",
            "size": {"w":64,"h":64}, "scale": "1" }
})JSON";

const char* kTexturePackerArray = R"JSON({
  "frames": [
    { "filename": "coin_0.png", "frame": {"x":0,"y":0,"w":8,"h":8}, "rotated": false,
      "trimmed": false, "spriteSourceSize": {"x":0,"y":0,"w":8,"h":8},
      "sourceSize": {"w":8,"h":8} },
    { "filename": "coin_1.png", "frame": {"x":8,"y":0,"w":8,"h":8}, "rotated": false,
      "trimmed": false, "spriteSourceSize": {"x":0,"y":0,"w":8,"h":8},
      "sourceSize": {"w":8,"h":8} }
  ],
  "meta": { "app": "http://www.codeandweb.com/texturepacker", "image": "tpa_missing.png",
            "size": {"w":32,"h":32}, "scale": "1" }
})JSON";

// Aseprite: кадры-массив с длительностями по кадру плюс meta.frameTags.
const char* kAseprite = R"JSON({
  "frames": [
    { "filename": "walk_0.png", "frame": {"x":0,"y":0,"w":16,"h":16}, "rotated": false,
      "trimmed": false, "spriteSourceSize": {"x":0,"y":0,"w":16,"h":16},
      "sourceSize": {"w":16,"h":16}, "duration": 100 },
    { "filename": "walk_1.png", "frame": {"x":16,"y":0,"w":16,"h":16}, "rotated": false,
      "trimmed": false, "spriteSourceSize": {"x":0,"y":0,"w":16,"h":16},
      "sourceSize": {"w":16,"h":16}, "duration": 150 },
    { "filename": "walk_2.png", "frame": {"x":32,"y":0,"w":16,"h":16}, "rotated": false,
      "trimmed": false, "spriteSourceSize": {"x":0,"y":0,"w":16,"h":16},
      "sourceSize": {"w":16,"h":16}, "duration": 50 }
  ],
  "meta": { "app": "http://www.aseprite.org/", "version": "1.3", "image": "ase_missing.png",
            "format": "RGBA8888", "size": {"w":64,"h":64}, "scale": "1",
            "frameTags": [
              { "name": "walk", "from": 0, "to": 2, "direction": "forward" },
              { "name": "walk_rev", "from": 0, "to": 2, "direction": "reverse" },
              { "name": "twice", "from": 0, "to": 1, "direction": "forward", "repeat": 2 }
            ] }
})JSON";

const char* kSparrowXml = R"XML(<?xml version="1.0" encoding="UTF-8"?>
<TextureAtlas imagePath="sparrow_missing.png" width="64" height="64">
  <SubTexture name="button" x="0" y="0" width="32" height="16" frameX="-4" frameY="-2" frameWidth="40" frameHeight="20"/>
  <SubTexture name="icon" x="32" y="0" width="16" height="16"/>
  <SubTexture name="rot" x="0" y="16" width="8" height="24" rotated="true"/>
</TextureAtlas>
)XML";

const char* kLibGdx = R"ATLAS(libgdx_missing.png
size: 64, 64
format: RGBA8888
filter: Nearest,Nearest
repeat: none
button
  rotate: false
  xy: 0, 0
  size: 24, 12
  orig: 24, 12
  offset: 0, 0
  index: -1
bar
  rotate: true
  xy: 24, 0
  size: 8, 16
  orig: 10, 18
  offset: 1, 1
  index: 0
bar
  rotate: false
  xy: 40, 0
  size: 8, 16
  orig: 8, 16
  offset: 0, 0
  index: 1
)ATLAS";

// Та же повёрнутая область - баннер 64x16, сохранённый как вырез 16x64 в (0,64)
// страницы 256x256 - записанный во всех поддерживаемых форматах.
const char* kRotEngineJson = R"JSON({
  "meta": { "app": "CrossRender", "version": "1", "image": "cross_missing.png",
            "size": {"w": 256, "h": 256}, "scale": "1" },
  "frames": { "banner_rot": {
      "frame": {"x":0,"y":64,"w":16,"h":64}, "rotated": true, "trimmed": false,
      "spriteSourceSize": {"x":0,"y":0,"w":64,"h":16},
      "sourceSize": {"w":64,"h":16}, "pivot": {"x":0.5,"y":0.5},
      "duration": 100 } }
})JSON";

const char* kRotTexturePackerHash = R"JSON({
  "frames": { "banner_rot": {
      "frame": {"x":0,"y":64,"w":16,"h":64}, "rotated": true, "trimmed": false,
      "spriteSourceSize": {"x":0,"y":0,"w":64,"h":16},
      "sourceSize": {"w":64,"h":16} } },
  "meta": { "app": "http://www.codeandweb.com/texturepacker", "image": "cross_missing.png",
            "size": {"w": 256, "h": 256}, "scale": "1" }
})JSON";

const char* kRotTexturePackerArray = R"JSON({
  "frames": [ { "filename": "banner_rot.png", "frame": {"x":0,"y":64,"w":16,"h":64},
                "rotated": true, "trimmed": false,
                "spriteSourceSize": {"x":0,"y":0,"w":64,"h":16},
                "sourceSize": {"w":64,"h":16}, "duration": 100 } ],
  "meta": { "app": "http://www.codeandweb.com/texturepacker", "image": "cross_missing.png",
            "size": {"w": 256, "h": 256}, "scale": "1" }
})JSON";

const char* kRotAseprite = R"JSON({
  "frames": [ { "filename": "banner_rot.png", "frame": {"x":0,"y":64,"w":16,"h":64},
                "rotated": true, "trimmed": false,
                "spriteSourceSize": {"x":0,"y":0,"w":64,"h":16},
                "sourceSize": {"w":64,"h":16}, "duration": 100 } ],
  "meta": { "app": "http://www.aseprite.org/", "image": "cross_missing.png",
            "size": {"w": 256, "h": 256}, "scale": "1" }
})JSON";

const char* kRotSparrow = R"XML(<TextureAtlas imagePath="cross_missing.png" width="256" height="256">
  <SubTexture name="banner_rot" x="0" y="64" width="16" height="64" frameX="0" frameY="0" frameWidth="64" frameHeight="16" rotated="true"/>
</TextureAtlas>
)XML";

// Ассетный инструмент репозитория пишет вырез страницы в `size`...
const char* kRotLibGdxCut = R"ATLAS(cross_missing.png
size: 256, 256
banner_rot
  rotate: true
  xy: 0, 64
  size: 16, 64
  orig: 64, 16
  offset: 0, 0
  index: -1
)ATLAS";

// ...а собственный TexturePacker libGDX пишет туда неповернутый размер.
const char* kRotLibGdxDisplay = R"ATLAS(cross_missing.png
size: 256, 256
banner_rot
  rotate: true
  xy: 0, 64
  size: 64, 16
  orig: 64, 16
  offset: 0, 0
  index: -1
)ATLAS";

const char* kGarbage = "this is not an atlas at all\njust some free text\n";

// ---------------------------------------------------------------------------
// Вспомогательные функции
// ---------------------------------------------------------------------------
struct Rgba {
    u8 r = 0, g = 0, b = 0, a = 0;
};

bool SameColour(const Rgba& a, const Rgba& b, int tolerance = 2) {
    auto near = [tolerance](u8 x, u8 y) {
        return std::abs(static_cast<int>(x) - static_cast<int>(y)) <= tolerance;
    };
    return near(a.r, b.r) && near(a.g, b.g) && near(a.b, b.b) && near(a.a, b.a);
}

template <typename F>
bool WriteImage(const std::string& path, int w, int h, F fn) {
    std::vector<u8> px(static_cast<usize>(w) * h * 4);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const Rgba c = fn(x, y);
            const usize i = (static_cast<usize>(y) * w + x) * 4;
            px[i + 0] = c.r;
            px[i + 1] = c.g;
            px[i + 2] = c.b;
            px[i + 3] = c.a;
        }
    }
    return Texture::EncodePng(path, w, h, 4, px.data());
}

std::string WriteTemp(const char* name, const std::string& text) {
    const std::string path = test::TempFilePath(name);
    WriteTextFile(path, text);
    return path;
}

Rgba PagePixel(const std::vector<u8>& px, int width, int x, int y) {
    const usize i = (static_cast<usize>(y) * width + x) * 4;
    Rgba c;
    c.r = px[i + 0];
    c.g = px[i + 1];
    c.b = px[i + 2];
    c.a = px[i + 3];
    return c;
}

// Читает декодированное CPU-изображение (строки сверху вниз).
Rgba ImagePixel(const Texture::ImageData& img, int x, int y) {
    const usize i = (static_cast<usize>(y) * img.width + x) * 4;
    Rgba c;
    c.r = img.pixels[i + 0];
    c.g = img.pixels[i + 1];
    c.b = img.pixels[i + 2];
    c.a = img.pixels[i + 3];
    return c;
}

// Оффскрин-таргет + Renderer2D, повторяет фикстуру графических тестов.
struct GLTarget {
    RenderTarget rt;
    Renderer2D r2d;

    bool Init(int w = 64, int h = 64) {
        RenderTargetDesc desc;
        desc.width = w;
        desc.height = h;
        desc.colorFormat = PixelFormat::RGBA8;
        desc.depth = false;
        desc.name = "atlas-fixture";
        if (!rt.Create(desc)) return false;
        return r2d.Init();
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
    bool Read(std::vector<u8>* out) { return rt.ReadPixels(out); }
    bool Pixel(const std::vector<u8>& img, int x, int y, Rgba* out) const {
        if (x < 0 || y < 0 || x >= rt.Width() || y >= rt.Height()) return false;
        // ReadPixels возвращает GL-строки снизу вверх; для удобства индексируем сверху вниз.
        const usize i = (static_cast<usize>(rt.Height() - 1 - y) * rt.Width() + x) * 4;
        if (i + 3 >= img.size()) return false;
        out->r = img[i + 0];
        out->g = img[i + 1];
        out->b = img[i + 2];
        out->a = img[i + 3];
        return true;
    }
};

bool InkBounds(const GLTarget& t, const std::vector<u8>& img, int* x0, int* y0, int* x1, int* y1) {
    int minX = t.rt.Width(), minY = t.rt.Height(), maxX = -1, maxY = -1;
    for (int y = 0; y < t.rt.Height(); ++y) {
        for (int x = 0; x < t.rt.Width(); ++x) {
            Rgba c;
            if (!t.Pixel(img, x, y, &c)) continue;
            if (c.r < 30 && c.g < 30 && c.b < 30) continue;
            if (x < minX) minX = x;
            if (y < minY) minY = y;
            if (x > maxX) maxX = x;
            if (y > maxY) maxY = y;
        }
    }
    if (maxX < minX || maxY < minY) return false;
    *x0 = minX;
    *y0 = minY;
    *x1 = maxX;
    *y1 = maxY;
    return true;
}

bool AnyInk(const GLTarget& t, const std::vector<u8>& img) {
    for (int y = 0; y < t.rt.Height(); ++y) {
        for (int x = 0; x < t.rt.Width(); ++x) {
            Rgba c;
            if (!t.Pixel(img, x, y, &c)) continue;
            if (c.r > 30 || c.g > 30 || c.b > 30) return true;
        }
    }
    return false;
}

// Пишет однокадровый engine-JSON дескриптор, указывающий на `pngPath`, и загружает его.
bool LoadSingleFrame(const std::string& descriptorName, const std::string& pngPath, int w, int h,
                     f32 pivotX, f32 pivotY, SpriteAtlas* out) {
    std::string json = "{\n  \"meta\": {\"app\": \"CrossRender\", \"version\": \"1\", "
                       "\"image\": \"" + PathBase(pngPath) + "\", \"size\": {\"w\": " +
                       std::to_string(w) + ", \"h\": " + std::to_string(h) +
                       "}, \"scale\": \"1\"},\n  \"frames\": {\"r\": {\"frame\": {\"x\":0,\"y\":0,"
                       "\"w\":" + std::to_string(w) + ",\"h\":" + std::to_string(h) +
                       "}, \"rotated\": false, \"trimmed\": false, \"spriteSourceSize\": "
                       "{\"x\":0,\"y\":0,\"w\":" + std::to_string(w) + ",\"h\":" +
                       std::to_string(h) + "}, \"sourceSize\": {\"w\":" + std::to_string(w) +
                       ",\"h\":" + std::to_string(h) + "}, \"pivot\": {\"x\": " +
                       std::to_string(pivotX) + ", \"y\": " + std::to_string(pivotY) +
                       "}}}\n}\n";
    const std::string path = WriteTemp(descriptorName.c_str(), json);
    return out->LoadFromFile(path);
}

void CheckRegionFields(const AtlasRegion& a, const AtlasRegion& b) {
    ENG_CHECK_STR_EQ(a.name, b.name);
    ENG_CHECK_NEAR(a.frame.x, b.frame.x, 1e-4f);
    ENG_CHECK_NEAR(a.frame.y, b.frame.y, 1e-4f);
    ENG_CHECK_NEAR(a.frame.w, b.frame.w, 1e-4f);
    ENG_CHECK_NEAR(a.frame.h, b.frame.h, 1e-4f);
    ENG_CHECK_NEAR(a.uv.x, b.uv.x, 1e-4f);
    ENG_CHECK_NEAR(a.uv.y, b.uv.y, 1e-4f);
    ENG_CHECK_NEAR(a.uv.w, b.uv.w, 1e-4f);
    ENG_CHECK_NEAR(a.uv.h, b.uv.h, 1e-4f);
    ENG_CHECK_NEAR(a.pivot.x, b.pivot.x, 1e-4f);
    ENG_CHECK_NEAR(a.pivot.y, b.pivot.y, 1e-4f);
    ENG_CHECK_NEAR(a.sourceSize.x, b.sourceSize.x, 1e-4f);
    ENG_CHECK_NEAR(a.sourceSize.y, b.sourceSize.y, 1e-4f);
    ENG_CHECK_NEAR(a.spriteSourceSize.x, b.spriteSourceSize.x, 1e-4f);
    ENG_CHECK_NEAR(a.spriteSourceSize.y, b.spriteSourceSize.y, 1e-4f);
    ENG_CHECK_NEAR(a.spriteSourceSize.w, b.spriteSourceSize.w, 1e-4f);
    ENG_CHECK_NEAR(a.spriteSourceSize.h, b.spriteSourceSize.h, 1e-4f);
    ENG_CHECK_NEAR(a.durationMs, b.durationMs, 1e-3f);
    ENG_CHECK_EQ(a.rotated, b.rotated);
    ENG_CHECK_EQ(a.trimmed, b.trimmed);
    ENG_CHECK_EQ(a.page, b.page);
    ENG_CHECK_EQ(a.polygon.size(), b.polygon.size());
    for (usize i = 0; i < a.polygon.size() && i < b.polygon.size(); ++i) {
        ENG_CHECK_NEAR(a.polygon[i].x, b.polygon[i].x, 1e-4f);
        ENG_CHECK_NEAR(a.polygon[i].y, b.polygon[i].y, 1e-4f);
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// Определение формата
// ---------------------------------------------------------------------------
ENG_TEST(Atlas, DetectFormatPerFixture) {
    ENG_CHECK(SpriteAtlas::DetectFormat(kEngineJson) == AtlasFormat::EngineJson);
    ENG_CHECK(SpriteAtlas::DetectFormat(kTexturePackerHash) == AtlasFormat::TexturePackerHash);
    ENG_CHECK(SpriteAtlas::DetectFormat(kTexturePackerArray) == AtlasFormat::TexturePackerArray);
    ENG_CHECK(SpriteAtlas::DetectFormat(kAseprite) == AtlasFormat::Aseprite);
    ENG_CHECK(SpriteAtlas::DetectFormat(kSparrowXml) == AtlasFormat::SparrowXml);
    ENG_CHECK(SpriteAtlas::DetectFormat(kLibGdx) == AtlasFormat::LibGdx);
    ENG_CHECK(SpriteAtlas::DetectFormat(kGarbage) == AtlasFormat::Unknown);
    ENG_CHECK(SpriteAtlas::DetectFormat("") == AtlasFormat::Unknown);
    ENG_CHECK(SpriteAtlas::DetectFormat("{ not valid json") == AtlasFormat::Unknown);
    ENG_CHECK(SpriteAtlas::DetectFormat("<!DOCTYPE html><html></html>") == AtlasFormat::Unknown);

    ENG_CHECK_STR_EQ(SpriteAtlas::FormatName(AtlasFormat::EngineJson), "EngineJson");
    ENG_CHECK_STR_EQ(SpriteAtlas::FormatName(AtlasFormat::SparrowXml), "SparrowXml");
    ENG_CHECK_STR_EQ(SpriteAtlas::FormatName(AtlasFormat::Unknown), "Unknown");
    ENG_CHECK_STR_EQ(SpriteAtlas::FormatName(AtlasFormat::PlainImage), "PlainImage");
}

// ---------------------------------------------------------------------------
// Загрузка дескрипторов
// ---------------------------------------------------------------------------
ENG_TEST(Atlas, LoadEngineJson) {
    const std::string path = WriteTemp("atlas_engine.json", kEngineJson);
    SpriteAtlas atlas;
    ENG_CHECK(atlas.LoadFromFile(path));
    ENG_CHECK(atlas.Format() == AtlasFormat::EngineJson);
    ENG_CHECK(atlas.Valid());
    ENG_CHECK_EQ(atlas.RegionCount(), 2);
    ENG_CHECK_EQ(atlas.PageCount(), 1);
    ENG_CHECK(atlas.Has("player_idle_0"));
    ENG_CHECK(atlas.Has("player_idle_1"));
    ENG_CHECK_EQ(atlas.IndexOf("player_idle_0"), 0);
    ENG_CHECK_EQ(atlas.IndexOf("missing"), -1);
    ENG_CHECK(atlas.Find("player_idle_0") != nullptr);
    ENG_CHECK(atlas.Find("missing") == nullptr);

    const AtlasRegion& a = atlas.RegionAt(0);
    ENG_CHECK_NEAR(a.frame.x, 0.0f, 1e-4f);
    ENG_CHECK_NEAR(a.frame.w, 32.0f, 1e-4f);
    ENG_CHECK_NEAR(a.uv.w, 32.0f / 128.0f, 1e-4f);
    ENG_CHECK_NEAR(a.uv.h, 32.0f / 64.0f, 1e-4f);
    ENG_CHECK(!a.rotated);
    ENG_CHECK(!a.trimmed);
    ENG_CHECK_NEAR(a.sourceSize.x, 32.0f, 1e-4f);
    ENG_CHECK_NEAR(a.durationMs, 120.0f, 1e-3f);
    ENG_CHECK_EQ(a.polygon.size(), 3u);

    const AtlasRegion& b = atlas.RegionAt(1);
    ENG_CHECK_NEAR(b.frame.x, 32.0f, 1e-4f);
    ENG_CHECK_NEAR(b.frame.y, 0.0f, 1e-4f);
    ENG_CHECK_NEAR(b.frame.w, 24.0f, 1e-4f);
    ENG_CHECK_NEAR(b.frame.h, 40.0f, 1e-4f);
    ENG_CHECK(b.rotated);
    ENG_CHECK(b.trimmed);
    ENG_CHECK_NEAR(b.pivot.x, 0.25f, 1e-4f);
    ENG_CHECK_NEAR(b.pivot.y, 0.75f, 1e-4f);
    ENG_CHECK_NEAR(b.sourceSize.x, 42.0f, 1e-4f);
    ENG_CHECK_NEAR(b.Size().x, 40.0f, 1e-4f);  // повёрнуто: обмен w/h
    ENG_CHECK_NEAR(b.Size().y, 24.0f, 1e-4f);
    ENG_CHECK_NEAR(b.uv.w, 24.0f / 128.0f, 1e-4f);
    ENG_CHECK_NEAR(b.uv.h, 40.0f / 64.0f, 1e-4f);

    ENG_CHECK_EQ(atlas.AnimationNames().size(), 1u);
    const AtlasAnimation* anim = atlas.FindAnimation("idle");
    ENG_CHECK(anim != nullptr);
    if (anim) {
        ENG_CHECK_EQ(anim->frames.size(), 2u);
        ENG_CHECK_EQ(anim->durations.size(), 2u);
        ENG_CHECK_NEAR(anim->durations[0], 120.0f, 1e-3f);
        ENG_CHECK_NEAR(anim->durations[1], 80.0f, 1e-3f);
        ENG_CHECK(anim->loop);
        ENG_CHECK_NEAR(anim->TotalDuration(), 200.0f, 1e-3f);  // в миллисекундах
    }

    const SpriteAtlas::Stats& s = atlas.GetStats();
    ENG_CHECK_EQ(s.regions, 2);
    ENG_CHECK_EQ(s.pages, 1);
    ENG_CHECK_EQ(s.pagePixels, 128 * 64);
    ENG_CHECK_GT(s.totalPixels, 0);

    // Доступ вне диапазона безопасен и пуст.
    ENG_CHECK(atlas.RegionAt(99).name.empty());
    ENG_CHECK_EQ(atlas.Page(9).width, 0);
    ENG_CHECK(!atlas.PageTexture(9).Valid());
}

ENG_TEST(Atlas, LoadTexturePackerHash) {
    const std::string path = WriteTemp("atlas_tp_hash.json", kTexturePackerHash);
    SpriteAtlas atlas;
    ENG_CHECK(atlas.LoadFromFile(path));
    ENG_CHECK(atlas.Format() == AtlasFormat::TexturePackerHash);
    ENG_CHECK_EQ(atlas.RegionCount(), 2);

    const AtlasRegion* r0 = atlas.Find("hero_run_0");
    const AtlasRegion* r1 = atlas.Find("hero_run_1");
    ENG_CHECK(r0 && r1);
    if (r0) {
        ENG_CHECK_NEAR(r0->frame.w, 16.0f, 1e-4f);
        ENG_CHECK_NEAR(r0->frame.h, 24.0f, 1e-4f);
        ENG_CHECK(r0->trimmed);
        ENG_CHECK_NEAR(r0->pivot.y, 1.0f, 1e-4f);
        ENG_CHECK_NEAR(r0->sourceSize.x, 18.0f, 1e-4f);
        ENG_CHECK_NEAR(r0->spriteSourceSize.x, 1.0f, 1e-4f);
        ENG_CHECK_NEAR(r0->spriteSourceSize.y, 2.0f, 1e-4f);
    }
    if (r1) {
        ENG_CHECK(!r1->trimmed);
        ENG_CHECK_NEAR(r1->pivot.x, 0.5f, 1e-4f);
        ENG_CHECK_NEAR(r1->pivot.y, 0.5f, 1e-4f);
    }
    // Нет frameTags / анимаций -> имена группируются по префиксам.
    ENG_CHECK_EQ(atlas.AnimationNames().size(), 1u);
    ENG_CHECK(atlas.FindAnimation("hero_run") != nullptr);
}

ENG_TEST(Atlas, LoadTexturePackerArray) {
    const std::string path = WriteTemp("atlas_tp_array.json", kTexturePackerArray);
    SpriteAtlas atlas;
    ENG_CHECK(atlas.LoadFromFile(path));
    ENG_CHECK(atlas.Format() == AtlasFormat::TexturePackerArray);
    ENG_CHECK_EQ(atlas.RegionCount(), 2);
    ENG_CHECK(atlas.Has("coin_0"));  // расширение изображения отбрасывается
    ENG_CHECK(atlas.Has("coin_1"));
    ENG_CHECK_NEAR(atlas.RegionAt(1).frame.x, 8.0f, 1e-4f);
    ENG_CHECK_NEAR(atlas.RegionAt(1).uv.y, 0.0f, 1e-4f);
}

ENG_TEST(Atlas, LoadAseprite) {
    const std::string path = WriteTemp("atlas_aseprite.json", kAseprite);
    SpriteAtlas atlas;
    ENG_CHECK(atlas.LoadFromFile(path));
    ENG_CHECK(atlas.Format() == AtlasFormat::Aseprite);
    ENG_CHECK_EQ(atlas.RegionCount(), 3);
    ENG_CHECK(atlas.Has("walk_0") && atlas.Has("walk_1") && atlas.Has("walk_2"));
    ENG_CHECK_NEAR(atlas.RegionAt(0).durationMs, 100.0f, 1e-3f);
    ENG_CHECK_NEAR(atlas.RegionAt(1).durationMs, 150.0f, 1e-3f);
    ENG_CHECK_NEAR(atlas.RegionAt(2).durationMs, 50.0f, 1e-3f);

    const std::vector<std::string> names = atlas.AnimationNames();
    ENG_CHECK_EQ(names.size(), 3u);
    const AtlasAnimation* walk = atlas.FindAnimation("walk");
    const AtlasAnimation* rev = atlas.FindAnimation("walk_rev");
    const AtlasAnimation* twice = atlas.FindAnimation("twice");
    ENG_CHECK(walk && rev && twice);
    if (walk) {
        ENG_CHECK_EQ(walk->frames.size(), 3u);
        ENG_CHECK_NEAR(walk->durations[0], 100.0f, 1e-3f);
        ENG_CHECK_NEAR(walk->durations[1], 150.0f, 1e-3f);
        ENG_CHECK_NEAR(walk->durations[2], 50.0f, 1e-3f);
        ENG_CHECK_EQ(walk->frames[0], atlas.IndexOf("walk_0"));
        ENG_CHECK_EQ(walk->frames[2], atlas.IndexOf("walk_2"));
    }
    if (rev) {
        // direction: reverse переворачивает порядок кадров.
        ENG_CHECK_EQ(rev->frames.size(), 3u);
        ENG_CHECK_EQ(rev->frames[0], atlas.IndexOf("walk_2"));
        ENG_CHECK_EQ(rev->frames[2], atlas.IndexOf("walk_0"));
    }
    if (twice) {
        // repeat масштабирует длительности.
        ENG_CHECK_EQ(twice->frames.size(), 2u);
        ENG_CHECK_NEAR(twice->durations[0], 200.0f, 1e-3f);
        ENG_CHECK_NEAR(twice->durations[1], 300.0f, 1e-3f);
    }
    // Явные анимации не должны заменяться группировкой по префиксам.
    ENG_CHECK(atlas.FindAnimation("walk_0") == nullptr);
}

ENG_TEST(Atlas, LoadSparrowXml) {
    const std::string path = WriteTemp("atlas_sparrow.xml", kSparrowXml);
    SpriteAtlas atlas;
    ENG_CHECK(atlas.LoadFromFile(path));
    ENG_CHECK(atlas.Format() == AtlasFormat::SparrowXml);
    ENG_CHECK_EQ(atlas.RegionCount(), 3);
    ENG_CHECK_EQ(atlas.PageCount(), 1);
    ENG_CHECK_EQ(atlas.Page(0).width, 64);
    ENG_CHECK_EQ(atlas.Page(0).height, 64);

    const AtlasRegion* button = atlas.Find("button");
    ENG_CHECK(button != nullptr);
    if (button) {
        ENG_CHECK_NEAR(button->frame.x, 0.0f, 1e-4f);
        ENG_CHECK_NEAR(button->frame.w, 32.0f, 1e-4f);
        ENG_CHECK_NEAR(button->frame.h, 16.0f, 1e-4f);
        ENG_CHECK(button->trimmed);
        // frameX/frameY - отрицательные смещения обрезанного прямоугольника в источнике.
        ENG_CHECK_NEAR(button->spriteSourceSize.x, 4.0f, 1e-4f);
        ENG_CHECK_NEAR(button->spriteSourceSize.y, 2.0f, 1e-4f);
        ENG_CHECK_NEAR(button->sourceSize.x, 40.0f, 1e-4f);
        ENG_CHECK_NEAR(button->sourceSize.y, 20.0f, 1e-4f);
    }
    const AtlasRegion* icon = atlas.Find("icon");
    ENG_CHECK(icon != nullptr);
    if (icon) {
        ENG_CHECK(!icon->trimmed);
        ENG_CHECK_NEAR(icon->sourceSize.x, 16.0f, 1e-4f);
        ENG_CHECK_NEAR(icon->frame.x, 32.0f, 1e-4f);
    }
    const AtlasRegion* rot = atlas.Find("rot");
    ENG_CHECK(rot != nullptr);
    if (rot) {
        ENG_CHECK(rot->rotated);
        ENG_CHECK_NEAR(rot->frame.w, 8.0f, 1e-4f);
        ENG_CHECK_NEAR(rot->frame.h, 24.0f, 1e-4f);
        ENG_CHECK_NEAR(rot->Size().x, 24.0f, 1e-4f);
        ENG_CHECK_NEAR(rot->Size().y, 8.0f, 1e-4f);
    }
}

ENG_TEST(Atlas, LoadLibGdx) {
    const std::string path = WriteTemp("atlas_libgdx.atlas", kLibGdx);
    SpriteAtlas atlas;
    ENG_CHECK(atlas.LoadFromFile(path));
    ENG_CHECK(atlas.Format() == AtlasFormat::LibGdx);
    ENG_CHECK_EQ(atlas.RegionCount(), 3);
    ENG_CHECK(atlas.Has("button"));
    ENG_CHECK(atlas.Has("bar_0"));  // суффикс индекса
    ENG_CHECK(atlas.Has("bar_1"));

    const AtlasRegion* button = atlas.Find("button");
    ENG_CHECK(button != nullptr);
    if (button) {
        ENG_CHECK_NEAR(button->frame.x, 0.0f, 1e-4f);
        ENG_CHECK_NEAR(button->frame.w, 24.0f, 1e-4f);
        ENG_CHECK_NEAR(button->frame.h, 12.0f, 1e-4f);
        ENG_CHECK(!button->rotated);
        ENG_CHECK(!button->trimmed);
    }
    const AtlasRegion* bar0 = atlas.Find("bar_0");
    ENG_CHECK(bar0 != nullptr);
    if (bar0) {
        ENG_CHECK(bar0->rotated);
        // `size` в libGDX - отображаемый размер; упакованный прямоугольник транспонирован.
        ENG_CHECK_NEAR(bar0->frame.x, 24.0f, 1e-4f);
        ENG_CHECK_NEAR(bar0->frame.w, 16.0f, 1e-4f);
        ENG_CHECK_NEAR(bar0->frame.h, 8.0f, 1e-4f);
        ENG_CHECK_NEAR(bar0->Size().x, 8.0f, 1e-4f);
        ENG_CHECK_NEAR(bar0->Size().y, 16.0f, 1e-4f);
        ENG_CHECK(bar0->trimmed);
        ENG_CHECK_NEAR(bar0->sourceSize.x, 10.0f, 1e-4f);
        ENG_CHECK_NEAR(bar0->sourceSize.y, 18.0f, 1e-4f);
        ENG_CHECK_NEAR(bar0->spriteSourceSize.x, 1.0f, 1e-4f);
    }
    const AtlasRegion* bar1 = atlas.Find("bar_1");
    ENG_CHECK(bar1 != nullptr);
    if (bar1) {
        ENG_CHECK(!bar1->rotated);
        ENG_CHECK(!bar1->trimmed);
        ENG_CHECK_NEAR(bar1->frame.x, 40.0f, 1e-4f);
    }
}

ENG_TEST(Atlas, LoadPlainImage) {
    const std::string png = test::TempFilePath("atlas_plain.png");
    ENG_CHECK(WriteImage(png, 12, 7, [](int, int) { return Rgba{10, 200, 30, 255}; }));
    SpriteAtlas atlas;
    ENG_CHECK(atlas.LoadPlainImage(png));
    ENG_CHECK(atlas.Format() == AtlasFormat::PlainImage);
    ENG_CHECK_EQ(atlas.RegionCount(), 1);
    ENG_CHECK_EQ(atlas.PageCount(), 1);
    ENG_CHECK(atlas.Has("atlas_plain"));
    ENG_CHECK_NEAR(atlas.RegionAt(0).frame.w, 12.0f, 1e-4f);
    ENG_CHECK_NEAR(atlas.RegionAt(0).uv.w, 1.0f, 1e-4f);

    // Голый путь к изображению тоже загружается через LoadFromFile.
    SpriteAtlas second;
    ENG_CHECK(second.LoadFromFile(png));
    ENG_CHECK(second.Format() == AtlasFormat::PlainImage);
    ENG_CHECK(second.Has("atlas_plain"));

    // Текстовый дескриптор с путём к изображению разрешает его относительно своего каталога.
    const std::string rel = WriteTemp("atlas_plain_rel.txt", "atlas_plain.png\n");
    SpriteAtlas third;
    ENG_CHECK(third.LoadFromFile(rel));
    ENG_CHECK(third.Format() == AtlasFormat::PlainImage);
    ENG_CHECK_EQ(third.RegionCount(), 1);
}

ENG_TEST(Atlas, GarbageDescriptorFailsCleanly) {
    const std::string path = WriteTemp("atlas_garbage.bin", kGarbage);
    SpriteAtlas atlas;
    ENG_CHECK(!atlas.LoadFromFile(path));
    ENG_CHECK(!atlas.Warnings().empty());
    ENG_CHECK_EQ(atlas.RegionCount(), 0);
    ENG_CHECK_EQ(atlas.PageCount(), 0);
    ENG_CHECK(!atlas.Valid());
    ENG_CHECK(!atlas.Has("anything"));
    ENG_CHECK(atlas.Find("anything") == nullptr);
    // Доступ вне диапазона не должен приводить к падению.
    ENG_CHECK(atlas.RegionAt(3).name.empty());
    ENG_CHECK_EQ(atlas.Page(3).width, 0);
    ENG_CHECK(!atlas.PageTexture(3).Valid());

    SpriteAtlas missing;
    ENG_CHECK(!missing.LoadFromFile(test::TempFilePath("atlas_does_not_exist.json")));
    ENG_CHECK(!missing.Warnings().empty());

    SpriteAtlas memory;
    ENG_CHECK(!memory.LoadFromMemory("not a descriptor", ""));
    ENG_CHECK(!memory.Warnings().empty());
}

ENG_TEST(Atlas, MissingImageKeepsRegions) {
    const std::string json = R"JSON({
      "meta": { "app": "CrossRender", "image": "no_such_page_12345.png",
                "size": {"w": 64, "h": 64} },
      "frames": { "thing": { "frame": {"x":0,"y":0,"w":16,"h":16}, "rotated": false,
                             "trimmed": false,
                             "spriteSourceSize": {"x":0,"y":0,"w":16,"h":16},
                             "sourceSize": {"w":16,"h":16} } }
    })JSON";
    const std::string path = WriteTemp("atlas_missing_image.json", json);
    SpriteAtlas atlas;
    ENG_CHECK(atlas.LoadFromFile(path));
    ENG_CHECK(atlas.Valid());
    ENG_CHECK_EQ(atlas.RegionCount(), 1);
    ENG_CHECK_EQ(atlas.PageCount(), 1);
    ENG_CHECK(!atlas.Warnings().empty());
    ENG_CHECK(!atlas.PageTexture(0).Valid());
    ENG_CHECK_NEAR(atlas.RegionAt(0).uv.w, 16.0f / 64.0f, 1e-4f);

    // Отрисовка превращается в no-op вместо падения (текстура невалидна).
    Renderer2D r2d;
    ENG_CHECK(!atlas.Draw(r2d, "thing", Rect{0, 0, 8, 8}));
    ENG_CHECK(!atlas.DrawAnchored(r2d, "thing", {4, 4}));
    ENG_CHECK(!atlas.DrawRegion(r2d, 0, Rect{0, 0, 8, 8}, Color::White));
    ENG_CHECK(!atlas.DrawNinePatch(r2d, "thing", Rect{0, 0, 24, 24}, 4.0f));
    ENG_CHECK(!atlas.DrawTiled(r2d, "thing", Rect{0, 0, 24, 24}));
}

ENG_TEST(Atlas, SaveLoadRoundTripsEveryField) {
    SpriteAtlas source;
    ENG_CHECK(source.LoadFromMemory(kEngineJson, "/definitely/not/here", "roundtrip"));
    ENG_CHECK_EQ(source.RegionCount(), 2);

    AtlasRegion extra;
    extra.name = "extra";
    extra.frame = Rect{64, 0, 16, 20};
    extra.uv = Rect{64.0f / 128.0f, 0.0f, 16.0f / 128.0f, 20.0f / 64.0f};
    extra.pivot = {0.25f, 0.9f};
    extra.rotated = false;
    extra.trimmed = true;
    extra.sourceSize = {22, 26};
    extra.spriteSourceSize = Rect{3, 4, 16, 20};
    extra.page = 0;
    extra.durationMs = 33.0f;
    extra.polygon = {{0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}};
    source.AddRegion(extra);
    ENG_CHECK_EQ(source.RegionCount(), 3);

    AtlasAnimation anim;
    anim.name = "extra_anim";
    anim.frames = {0, 1, 2};
    anim.durations = {10.0f, 20.0f, 30.0f};
    anim.loop = false;
    ENG_CHECK_EQ(source.AddAnimation(anim), 1);

    const std::string path = test::TempFilePath("atlas_roundtrip.json");
    ENG_CHECK(source.SaveToFile(path, false));
    ENG_CHECK(FileExists(path));

    SpriteAtlas loaded;
    ENG_CHECK(loaded.LoadFromFile(path));
    ENG_CHECK(loaded.Format() == AtlasFormat::EngineJson);
    ENG_CHECK_EQ(loaded.RegionCount(), source.RegionCount());
    ENG_CHECK_EQ(loaded.PageCount(), source.PageCount());
    ENG_CHECK_EQ(loaded.Page(0).width, source.Page(0).width);
    ENG_CHECK_EQ(loaded.Page(0).height, source.Page(0).height);

    // Сравниваем каждое поле каждой области (по имени: JSON-райтер сортирует ключи).
    for (const AtlasRegion& a : source.Regions()) {
        const AtlasRegion* b = loaded.Find(a.name);
        ENG_CHECK_MSG(b != nullptr, a.name);
        if (b) CheckRegionFields(a, *b);
    }

    ENG_CHECK_EQ(loaded.AnimationNames().size(), source.AnimationNames().size());
    for (const std::string& name : source.AnimationNames()) {
        const AtlasAnimation* a = source.FindAnimation(name);
        const AtlasAnimation* b = loaded.FindAnimation(name);
        ENG_CHECK_MSG(b != nullptr, name);
        if (!a || !b) continue;
        ENG_CHECK_EQ(a->frames.size(), b->frames.size());
        ENG_CHECK_EQ(a->durations.size(), b->durations.size());
        ENG_CHECK_EQ(a->loop, b->loop);
        for (usize i = 0; i < a->frames.size() && i < b->frames.size(); ++i) {
            ENG_CHECK_STR_EQ(source.RegionAt(a->frames[i]).name, loaded.RegionAt(b->frames[i]).name);
            if (i < a->durations.size() && i < b->durations.size())
                ENG_CHECK_NEAR(a->durations[i], b->durations[i], 1e-3f);
        }
    }

    // Анимации переживают повторное сохранение с записью изображений страниц рядом.
    ENG_CHECK(source.SaveToFile(path, true));
    SpriteAtlas withImages;
    ENG_CHECK(withImages.LoadFromFile(path));
    ENG_CHECK_EQ(withImages.RegionCount(), source.RegionCount());
}

// ---------------------------------------------------------------------------
// Анимации по префиксам
// ---------------------------------------------------------------------------
ENG_TEST(Atlas, BuildAnimationsFromPrefixesNaturalOrder) {
    SpriteAtlas atlas;
    const char* names[] = {"idle_0", "idle_1", "idle_10", "run_0", "run_1", "hero"};
    for (const char* n : names) {
        AtlasRegion r;
        r.name = n;
        r.frame = Rect{0, 0, 8, 8};
        r.uv = Rect{0, 0, 0.25f, 0.25f};
        atlas.AddRegion(r);
    }
    ENG_CHECK_EQ(atlas.RegionCount(), 6);
    ENG_CHECK_EQ(atlas.BuildAnimationsFromPrefixes("_"), 2);
    ENG_CHECK_EQ(atlas.AnimationNames().size(), 2u);
    ENG_CHECK_EQ(atlas.AnimationNames()[0], std::string("idle"));
    ENG_CHECK_EQ(atlas.AnimationNames()[1], std::string("run"));

    const AtlasAnimation* idle = atlas.FindAnimation("idle");
    ENG_CHECK(idle != nullptr);
    if (idle) {
        ENG_CHECK_EQ(idle->frames.size(), 3u);
        ENG_CHECK_STR_EQ(atlas.RegionAt(idle->frames[0]).name, "idle_0");
        ENG_CHECK_STR_EQ(atlas.RegionAt(idle->frames[1]).name, "idle_1");
        ENG_CHECK_STR_EQ(atlas.RegionAt(idle->frames[2]).name, "idle_10");
        ENG_CHECK_NEAR(idle->durations[0], 100.0f, 1e-3f);  // значение по умолчанию при отсутствии
    }
    // "hero" без числового суффикса игнорируется.
    ENG_CHECK(atlas.FindAnimation("hero") == nullptr);
    ENG_CHECK_EQ(atlas.BuildAnimationsFromPrefixes(""), 0);

    const std::vector<std::string> prefixed = atlas.NamesWithPrefix("idle");
    ENG_CHECK_EQ(prefixed.size(), 3u);
    ENG_CHECK_STR_EQ(prefixed[0], "idle_0");
    ENG_CHECK_STR_EQ(prefixed[1], "idle_1");
    ENG_CHECK_STR_EQ(prefixed[2], "idle_10");
}

ENG_TEST(Atlas, AnimationFrameAtHonoursDurations) {
    SpriteAtlas atlas;
    for (int i = 0; i < 3; ++i) {
        AtlasRegion r;
        r.name = "f" + std::to_string(i);
        r.frame = Rect{0, 0, 4, 4};
        atlas.AddRegion(r);
    }
    AtlasAnimation anim;
    anim.name = "uneven";
    anim.frames = {0, 1, 2};
    anim.durations = {100.0f, 200.0f, 50.0f};
    anim.loop = true;
    ENG_CHECK_EQ(atlas.AddAnimation(anim), 0);
    ENG_CHECK_NEAR(atlas.FindAnimation("uneven")->TotalDuration(), 350.0f, 1e-3f);  // ms

    ENG_CHECK_EQ(atlas.AnimationFrameAt("uneven", 0.0f), 0);
    ENG_CHECK_EQ(atlas.AnimationFrameAt("uneven", 0.05f), 0);
    ENG_CHECK_EQ(atlas.AnimationFrameAt("uneven", 0.20f), 1);
    ENG_CHECK_EQ(atlas.AnimationFrameAt("uneven", 0.34f), 2);
    // Зацикливается обратно на первый кадр.
    ENG_CHECK_EQ(atlas.AnimationFrameAt("uneven", 0.36f), 0);
    ENG_CHECK_EQ(atlas.AnimationFrameAt("uneven", 0.46f), 1);
    ENG_CHECK_EQ(atlas.AnimationFrameAt("uneven", 0.70f), 0);
    ENG_CHECK_EQ(atlas.AnimationFrameAt("nope", 0.1f), -1);

    // Незацикленная анимация клампится на последнем кадре.
    AtlasAnimation once;
    once.name = "once";
    once.frames = {0, 1, 2};
    once.durations = {100.0f, 200.0f, 50.0f};
    once.loop = false;
    ENG_CHECK_EQ(atlas.AddAnimation(once), 1);
    ENG_CHECK_EQ(atlas.AnimationFrameAt("once", 0.20f), 1);
    ENG_CHECK_EQ(atlas.AnimationFrameAt("once", 0.36f), 2);
    ENG_CHECK_EQ(atlas.AnimationFrameAt("once", 10.0f), 2);  // клампится на конце

    // Замена анимации с тем же именем сохраняет одну запись.
    const usize before = atlas.AnimationNames().size();
    ENG_CHECK_EQ(atlas.AddAnimation(once), 1);  // заменяет по имени
    ENG_CHECK_EQ(atlas.AnimationNames().size(), before);
}

// ---------------------------------------------------------------------------
// Геометрия nine-patch (CPU: GL-контекст не нужен)
// ---------------------------------------------------------------------------
ENG_TEST(Atlas, NinePatchSliceGeometry) {
    atlas_detail::NinePatchQuad q[9];
    const int n = atlas_detail::NinePatchQuads(Rect{0, 0, 30, 30}, Rect{10, 20, 100, 100}, 10, 10, 10,
                                               10, q);
    ENG_CHECK_EQ(n, 9);
    if (n != 9) return;
    // Углы сохраняют свои исходные куски 10x10...
    ENG_CHECK_NEAR(q[0].dst.x, 10.0f, 1e-4f);
    ENG_CHECK_NEAR(q[0].dst.y, 20.0f, 1e-4f);
    ENG_CHECK_NEAR(q[0].dst.w, 10.0f, 1e-4f);
    ENG_CHECK_NEAR(q[0].dst.h, 10.0f, 1e-4f);
    ENG_CHECK_NEAR(q[0].src.x, 0.0f, 1e-4f);
    ENG_CHECK_NEAR(q[0].src.y, 0.0f, 1e-4f);
    ENG_CHECK_NEAR(q[0].src.w, 10.0f, 1e-4f);
    ENG_CHECK_NEAR(q[2].src.x, 20.0f, 1e-4f);
    ENG_CHECK_NEAR(q[2].dst.x, 100.0f, 1e-4f);  // dst.x + dst.w - 10
    ENG_CHECK_NEAR(q[8].dst.x, 100.0f, 1e-4f);
    ENG_CHECK_NEAR(q[8].dst.y, 110.0f, 1e-4f);  // dst.y + dst.h - 10
    ENG_CHECK_NEAR(q[8].src.x, 20.0f, 1e-4f);
    ENG_CHECK_NEAR(q[8].src.y, 20.0f, 1e-4f);
    // ... центр растягивается с 10x10 до 80x80.
    ENG_CHECK_NEAR(q[4].dst.x, 20.0f, 1e-4f);
    ENG_CHECK_NEAR(q[4].dst.w, 80.0f, 1e-4f);
    ENG_CHECK_NEAR(q[4].src.w, 10.0f, 1e-4f);
    ENG_CHECK_NEAR(q[1].dst.h, 10.0f, 1e-4f);  // верхняя кромка сохраняет высоту
    ENG_CHECK_NEAR(q[1].dst.w, 80.0f, 1e-4f);  // и растягивается по горизонтали
    ENG_CHECK_NEAR(q[3].dst.w, 10.0f, 1e-4f);
    ENG_CHECK_NEAR(q[3].dst.h, 80.0f, 1e-4f);

    // Отступы по каждой стороне.
    const int m = atlas_detail::NinePatchQuads(Rect{0, 0, 30, 30}, Rect{0, 0, 100, 100}, 5, 10, 15, 12,
                                               q);
    ENG_CHECK_EQ(m, 9);
    if (m == 9) {
        ENG_CHECK_NEAR(q[0].dst.w, 5.0f, 1e-4f);
        ENG_CHECK_NEAR(q[0].dst.h, 10.0f, 1e-4f);
        ENG_CHECK_NEAR(q[0].src.w, 5.0f, 1e-4f);
        ENG_CHECK_NEAR(q[0].src.h, 10.0f, 1e-4f);
        ENG_CHECK_NEAR(q[2].dst.x, 85.0f, 1e-4f);
        ENG_CHECK_NEAR(q[2].dst.w, 15.0f, 1e-4f);
        ENG_CHECK_NEAR(q[6].dst.y, 88.0f, 1e-4f);
        ENG_CHECK_NEAR(q[6].dst.h, 12.0f, 1e-4f);
    }
    // Отступы, точно поглощающие область, схлопывают средний кусок.
    ENG_CHECK_EQ(atlas_detail::NinePatchQuads(Rect{0, 0, 30, 30}, Rect{0, 0, 100, 100}, 5, 10, 15, 20,
                                              q),
                 6);

    // Вырожденные входы никогда не дают перевёрнутых квадов.
    ENG_CHECK_EQ(atlas_detail::NinePatchQuads(Rect{0, 0, 0, 0}, Rect{0, 0, 10, 10}, 2, 2, 2, 2, q),
                 0);
    const int tight = atlas_detail::NinePatchQuads(Rect{0, 0, 8, 8}, Rect{0, 0, 4, 4}, 10, 10, 10, 10,
                                                   q);
    for (int i = 0; i < tight; ++i) {
        ENG_CHECK_GE(q[i].dst.w, 0.0f);
        ENG_CHECK_GE(q[i].src.w, 0.0f);
    }
}

// ---------------------------------------------------------------------------
// BuildFromFiles (упаковка)
// ---------------------------------------------------------------------------
ENG_TEST(Atlas, BuildFromFilesPacksOnePage) {
    struct Source {
        std::string path;
        Rgba colour;
        int size;
    };
    const Source sources[5] = {
        {test::TempFilePath("atlas_src_a.png"), Rgba{220, 40, 40, 255}, 16},
        {test::TempFilePath("atlas_src_b.png"), Rgba{40, 220, 40, 255}, 16},
        {test::TempFilePath("atlas_src_c.png"), Rgba{40, 40, 220, 255}, 16},
        {test::TempFilePath("atlas_src_d.png"), Rgba{220, 220, 40, 255}, 16},
        {test::TempFilePath("atlas_src_e.png"), Rgba{210, 50, 190, 255}, 22},
    };
    for (usize i = 0; i < 5; ++i) {
        const Rgba c = sources[i].colour;
        const int size = sources[i].size;
        const bool last = i == 4;
        ENG_CHECK(WriteImage(sources[i].path, size, size, [c, size, last](int x, int y) {
            if (last && (x < 3 || y < 3 || x >= size - 3 || y >= size - 3)) return Rgba{0, 0, 0, 0};
            return c;
        }));
    }

    SpriteAtlas::PackOptions opts;
    opts.maxSize = 256;
    opts.padding = 2;
    opts.powerOfTwo = true;
    opts.trim = true;
    SpriteAtlas atlas;
    std::vector<std::string> paths;
    for (const Source& s : sources) paths.push_back(s.path);
    ENG_CHECK(atlas.BuildFromFiles(paths, opts));
    ENG_CHECK(atlas.Valid());
    ENG_CHECK_EQ(atlas.RegionCount(), 5);
    ENG_CHECK_EQ(atlas.PageCount(), 1);

    // Каждый источник даёт ровно одну область (именована по своему стему).
    for (const Source& s : sources) {
        const std::string stem = PathBase(s.path).substr(0, PathBase(s.path).find_last_of('.'));
        int matches = 0;
        for (const AtlasRegion& r : atlas.Regions()) {
            if (r.name == stem) ++matches;
        }
        ENG_CHECK_MSG(matches == 1, stem);
    }

    const AtlasPageDesc& page = atlas.Page(0);
    ENG_CHECK_GT(page.width, 0);
    ENG_CHECK_GT(page.height, 0);
    ENG_CHECK_EQ(page.width, page.height);
    ENG_CHECK_LE(page.width, 256);
    ENG_CHECK((page.width & (page.width - 1)) == 0);  // степень двойки

    std::vector<u8> pixels;
    int pw = 0, ph = 0;
    ENG_CHECK(atlas.PagePixels(0, &pixels, &pw, &ph));
    ENG_CHECK_EQ(pw, page.width);
    ENG_CHECK_EQ(ph, page.height);
    ENG_CHECK_EQ(pixels.size(), static_cast<usize>(pw) * ph * 4);

    // Без перекрытий, всё внутри страницы.
    for (usize i = 0; i < atlas.Regions().size(); ++i) {
        const AtlasRegion& a = atlas.Regions()[i];
        ENG_CHECK_GE(a.frame.x, 0.0f);
        ENG_CHECK_GE(a.frame.y, 0.0f);
        ENG_CHECK_LE(a.frame.Right(), static_cast<f32>(page.width));
        ENG_CHECK_LE(a.frame.Bottom(), static_cast<f32>(page.height));
        for (usize j = i + 1; j < atlas.Regions().size(); ++j) {
            const AtlasRegion& b = atlas.Regions()[j];
            if (a.page != b.page) continue;
            ENG_CHECK_MSG(!a.frame.Intersects(b.frame), a.name + " overlaps " + b.name);
        }
    }

    // Собранные пиксели совпадают с исходными (у обрезанных изображений совпадает
    // центр, потому что рамка однородна).
    for (usize i = 0; i < 5; ++i) {
        const std::string stem = PathBase(sources[i].path).substr(0, PathBase(sources[i].path).find_last_of('.'));
        const AtlasRegion* r = atlas.Find(stem);
        ENG_CHECK(r != nullptr);
        if (!r) continue;
        Texture::ImageData img;
        ENG_CHECK(Texture::DecodeImageFile(sources[i].path, &img, false));
        const Rgba expected = ImagePixel(img, img.width / 2, img.height / 2);
        const Rgba actual = PagePixel(pixels, pw, static_cast<int>(r->frame.x + r->frame.w * 0.5f),
                                      static_cast<int>(r->frame.y + r->frame.h * 0.5f));
        ENG_CHECK_MSG(SameColour(expected, actual), stem);
    }

    // Прозрачная рамка была обрезана.
    const AtlasRegion* e = atlas.Find("atlas_src_e");
    ENG_CHECK(e != nullptr);
    if (e) {
        ENG_CHECK(e->trimmed);
        ENG_CHECK_NEAR(e->frame.w, 16.0f, 1e-4f);
        ENG_CHECK_NEAR(e->frame.h, 16.0f, 1e-4f);
        ENG_CHECK_NEAR(e->sourceSize.x, 22.0f, 1e-4f);
        ENG_CHECK_NEAR(e->sourceSize.y, 22.0f, 1e-4f);
        ENG_CHECK_NEAR(e->spriteSourceSize.x, 3.0f, 1e-4f);
        ENG_CHECK_NEAR(e->spriteSourceSize.y, 3.0f, 1e-4f);
    }
    const AtlasRegion* a = atlas.Find("atlas_src_a");
    ENG_CHECK(a != nullptr);
    if (a) ENG_CHECK(!a->trimmed);
}

ENG_TEST(Atlas, BuildFromFilesMultiPage) {
    std::vector<std::string> paths;
    for (int i = 0; i < 5; ++i) {
        const std::string path = test::TempFilePath(("atlas_page_" + std::to_string(i) + ".png").c_str());
        const Rgba c{static_cast<u8>(40 + i * 30), static_cast<u8>(200 - i * 20), 90, 255};
        ENG_CHECK(WriteImage(path, 24, 24, [c](int, int) { return c; }));
        paths.push_back(path);
    }
    SpriteAtlas::PackOptions opts;
    opts.maxSize = 64;
    opts.padding = 2;
    opts.powerOfTwo = true;
    opts.trim = false;
    SpriteAtlas atlas;
    ENG_CHECK(atlas.BuildFromFiles(paths, opts));
    ENG_CHECK_EQ(atlas.RegionCount(), 5);
    ENG_CHECK_GT(atlas.PageCount(), 1);

    for (const AtlasRegion& r : atlas.Regions()) {
        ENG_CHECK_GE(r.page, 0);
        ENG_CHECK_LT(r.page, atlas.PageCount());
        const AtlasPageDesc& p = atlas.Page(r.page);
        ENG_CHECK_GT(p.width, 0);
        ENG_CHECK_GT(p.height, 0);
        ENG_CHECK_LE(r.frame.Right(), static_cast<f32>(p.width));
        ENG_CHECK_LE(r.frame.Bottom(), static_cast<f32>(p.height));
        std::vector<u8> px;
        int w = 0, h = 0;
        ENG_CHECK(atlas.PagePixels(r.page, &px, &w, &h));
        ENG_CHECK_EQ(w, p.width);
        ENG_CHECK(!px.empty());
    }
    // Каждая страница укладывается в лимит и является степенью двойки.
    for (int i = 0; i < atlas.PageCount(); ++i) {
        ENG_CHECK_LE(atlas.Page(i).width, 64);
        ENG_CHECK((atlas.Page(i).width & (atlas.Page(i).width - 1)) == 0);
    }
}

ENG_TEST(Atlas, PackerRotatesTallSprite) {
    const std::string big = test::TempFilePath("atlas_rot_big.png");
    const std::string tall = test::TempFilePath("atlas_rot_tall.png");
    ENG_CHECK(WriteImage(big, 40, 40, [](int, int) { return Rgba{200, 10, 10, 255}; }));
    // Отличительный левый верхний пиксель, чтобы проверить сохранённую ориентацию.
    ENG_CHECK(WriteImage(tall, 8, 32, [](int x, int y) {
        if (x == 0 && y == 0) return Rgba{1, 2, 3, 255};
        return Rgba{10, 20, 200, 255};
    }));

    SpriteAtlas::PackOptions opts;
    opts.maxSize = 256;
    opts.padding = 2;
    opts.powerOfTwo = true;
    opts.allowRotate = true;
    SpriteAtlas atlas;
    ENG_CHECK(atlas.BuildFromFiles({big, tall}, opts));
    const AtlasRegion* r = atlas.Find("atlas_rot_tall");
    ENG_CHECK(r != nullptr);
    if (!r) return;
    ENG_CHECK(r->rotated);
    ENG_CHECK_NEAR(r->frame.w, 32.0f, 1e-4f);
    ENG_CHECK_NEAR(r->frame.h, 8.0f, 1e-4f);
    ENG_CHECK_NEAR(r->Size().x, 8.0f, 1e-4f);
    ENG_CHECK_NEAR(r->Size().y, 32.0f, 1e-4f);

    std::vector<u8> px;
    int w = 0, h = 0;
    ENG_CHECK(atlas.PagePixels(r->page, &px, &w, &h));
    // Пакер сохраняет повёрнутый спрайт на 90 градусов по часовой, поэтому
    // логический левый верхний пиксель лежит в правом верхнем углу сохранённого прямоугольника.
    const Rgba topRight = PagePixel(px, w, static_cast<int>(r->frame.x + r->frame.w) - 1,
                                    static_cast<int>(r->frame.y));
    ENG_CHECK(SameColour(topRight, Rgba{1, 2, 3, 255}));
    const Rgba topLeft = PagePixel(px, w, static_cast<int>(r->frame.x), static_cast<int>(r->frame.y));
    ENG_CHECK(SameColour(topLeft, Rgba{10, 20, 200, 255}));
}

// ---------------------------------------------------------------------------
// Согласованность между форматами
// ---------------------------------------------------------------------------
ENG_TEST(Atlas, CrossFormatRotatedRegionAgrees) {
    struct Sample {
        const char* name;
        const char* text;
    };
    const Sample samples[] = {
        {"atlas_cross_engine.json", kRotEngineJson},
        {"atlas_cross_tp_hash.json", kRotTexturePackerHash},
        {"atlas_cross_tp_array.json", kRotTexturePackerArray},
        {"atlas_cross_aseprite.json", kRotAseprite},
        {"atlas_cross_sparrow.xml", kRotSparrow},
        {"atlas_cross_libgdx_cut.atlas", kRotLibGdxCut},
        {"atlas_cross_libgdx_display.atlas", kRotLibGdxDisplay},
    };
    for (const Sample& sample : samples) {
        const std::string path = WriteTemp(sample.name, sample.text);
        SpriteAtlas atlas;
        ENG_CHECK_MSG(atlas.LoadFromFile(path), sample.name);
        const AtlasRegion* r = atlas.Find("banner_rot");
        ENG_CHECK_MSG(r != nullptr, sample.name);
        if (!r) continue;
        ENG_CHECK_MSG(r->rotated, sample.name);
        // Вырез страницы (16x64 в 0,64) одинаков во всех форматах...
        ENG_CHECK_NEAR(r->frame.x, 0.0f, 1e-4f);
        ENG_CHECK_NEAR(r->frame.y, 64.0f, 1e-4f);
        ENG_CHECK_NEAR(r->frame.w, 16.0f, 1e-4f);
        ENG_CHECK_NEAR(r->frame.h, 64.0f, 1e-4f);
        // ... и отображаемый размер тоже (обмен происходит ровно один раз).
        ENG_CHECK_NEAR(r->Size().x, 64.0f, 1e-4f);
        ENG_CHECK_NEAR(r->Size().y, 16.0f, 1e-4f);
        ENG_CHECK_NEAR(r->uv.x, 0.0f, 1e-4f);
        ENG_CHECK_NEAR(r->uv.y, 64.0f / 256.0f, 1e-4f);
        ENG_CHECK_NEAR(r->uv.w, 16.0f / 256.0f, 1e-4f);
        ENG_CHECK_NEAR(r->uv.h, 64.0f / 256.0f, 1e-4f);
        // Необрезанный источник - баннер 64x16 в каждом формате.
        ENG_CHECK_MSG(!r->trimmed, sample.name);
        ENG_CHECK_NEAR(r->sourceSize.x, 64.0f, 1e-4f);
        ENG_CHECK_NEAR(r->sourceSize.y, 16.0f, 1e-4f);
        ENG_CHECK_NEAR(r->spriteSourceSize.x, 0.0f, 1e-4f);
        ENG_CHECK_NEAR(r->spriteSourceSize.y, 0.0f, 1e-4f);
        ENG_CHECK_NEAR(r->spriteSourceSize.w, 64.0f, 1e-4f);
        ENG_CHECK_NEAR(r->spriteSourceSize.h, 16.0f, 1e-4f);
        ENG_CHECK_EQ(atlas.Page(0).width, 256);
        ENG_CHECK_EQ(atlas.Page(0).height, 256);
    }
}

ENG_TEST(Atlas, WarningWordingDistinguishesFailures) {
    const char* missingJson = R"JSON({
      "meta": { "app": "CrossRender", "image": "no_such_page_999.png",
                "size": {"w": 64, "h": 64} },
      "frames": { "thing": { "frame": {"x":0,"y":0,"w":8,"h":8}, "rotated": false,
                             "trimmed": false,
                             "spriteSourceSize": {"x":0,"y":0,"w":8,"h":8},
                             "sourceSize": {"w":8,"h":8} } }
    })JSON";
    SpriteAtlas missing;
    ENG_CHECK(missing.LoadFromFile(WriteTemp("atlas_warn_missing.json", missingJson)));
    bool sawMissing = false;
    bool sawMisleading = false;
    for (const std::string& w : missing.Warnings()) {
        if (w.find("page image missing") != std::string::npos) sawMissing = true;
        if (w.find("unreadable") != std::string::npos ||
            w.find("decoded but not uploaded") != std::string::npos)
            sawMisleading = true;
    }
    ENG_CHECK(sawMissing);
    ENG_CHECK(!sawMisleading);

    // Изображение страницы, которое существует, но вовсе не изображение, - "unreadable".
    const std::string junkPng = WriteTemp("atlas_warn_junk.png", "definitely not a png");
    (void)junkPng;
    const char* junkJson = R"JSON({
      "meta": { "app": "CrossRender", "image": "atlas_warn_junk.png",
                "size": {"w": 64, "h": 64} },
      "frames": { "thing": { "frame": {"x":0,"y":0,"w":8,"h":8}, "rotated": false,
                             "trimmed": false,
                             "spriteSourceSize": {"x":0,"y":0,"w":8,"h":8},
                             "sourceSize": {"w":8,"h":8} } }
    })JSON";
    SpriteAtlas junk;
    ENG_CHECK(junk.LoadFromFile(WriteTemp("atlas_warn_junk.json", junkJson)));
    bool sawUnreadable = false;
    bool sawNotUploaded = false;
    for (const std::string& w : junk.Warnings()) {
        if (w.find("unreadable") != std::string::npos) sawUnreadable = true;
        if (w.find("decoded but not uploaded") != std::string::npos) sawNotUploaded = true;
    }
    ENG_CHECK(sawUnreadable);
    ENG_CHECK(!sawNotUploaded);

    // Присутствующее декодируемое изображение страницы вообще не должно давать предупреждений.
    const std::string okPng = test::TempFilePath("atlas_warn_ok.png");
    ENG_CHECK(WriteImage(okPng, 8, 8, [](int, int) { return Rgba{1, 2, 3, 255}; }));
    SpriteAtlas ok;
    ENG_CHECK(LoadSingleFrame("atlas_warn_ok.json", okPng, 8, 8, 0.5f, 0.5f, &ok));
    ENG_CHECK_EQ(ok.RegionCount(), 1);
    if (ok.PageTexture(0).Valid()) ENG_CHECK(ok.Warnings().empty());
}

// ---------------------------------------------------------------------------
// Отрисовка (GL)
// ---------------------------------------------------------------------------
ENG_TEST(Atlas, DrawProducesInkAndFlipMirrors) {
    ENG_REQUIRE_GL();
    // Источник 8x8: левая половина красная, правая синяя.
    const std::string png = test::TempFilePath("atlas_flip_src.png");
    ENG_CHECK(WriteImage(png, 8, 8, [](int x, int) {
        return x < 4 ? Rgba{255, 0, 0, 255} : Rgba{0, 0, 255, 255};
    }));
    SpriteAtlas atlas;
    ENG_CHECK(atlas.LoadPlainImage(png));
    ENG_CHECK(atlas.PageTexture(0).Valid());

    GLTarget t;
    if (!t.Init(32, 32)) ENG_SKIP("no GL context");

    t.Begin();
    ENG_CHECK(atlas.Draw(t.r2d, "atlas_flip_src", Rect{0, 0, 32, 32}));
    t.End();
    std::vector<u8> normal;
    ENG_CHECK(t.Read(&normal));
    ENG_CHECK(AnyInk(t, normal));
    Rgba left, right;
    ENG_CHECK(t.Pixel(normal, 6, 16, &left));
    ENG_CHECK(t.Pixel(normal, 26, 16, &right));
    ENG_CHECK_MSG(left.r > 200 && left.b < 60, "left half is red");
    ENG_CHECK_MSG(right.b > 200 && right.r < 60, "right half is blue");

    t.Begin();
    ENG_CHECK(atlas.DrawRegion(t.r2d, 0, Rect{0, 0, 32, 32}, Color::White, true, false));
    t.End();
    std::vector<u8> flipped;
    ENG_CHECK(t.Read(&flipped));
    Rgba fleft, fright;
    ENG_CHECK(t.Pixel(flipped, 6, 16, &fleft));
    ENG_CHECK(t.Pixel(flipped, 26, 16, &fright));
    ENG_CHECK_MSG(fleft.b > 200 && fleft.r < 60, "flipped left half is blue");
    ENG_CHECK_MSG(fright.r > 200 && fright.b < 60, "flipped right half is red");

    // Суммы столбцов должны зеркально совпадать.
    for (int x = 0; x < 32; ++x) {
        int sumNormal = 0;
        int sumFlipped = 0;
        for (int y = 0; y < 32; ++y) {
            Rgba a, b;
            ENG_CHECK(t.Pixel(normal, x, y, &a));
            ENG_CHECK(t.Pixel(flipped, 32 - 1 - x, y, &b));
            sumNormal += a.r;
            sumFlipped += b.r;
        }
        ENG_CHECK_NEAR(sumNormal, sumFlipped, 4);
    }
}

ENG_TEST(Atlas, DrawTiledFillsRect) {
    ENG_REQUIRE_GL();
    const std::string png = test::TempFilePath("atlas_tile_src.png");
    ENG_CHECK(WriteImage(png, 8, 8, [](int, int) { return Rgba{0, 200, 255, 255}; }));
    SpriteAtlas atlas;
    ENG_CHECK(atlas.LoadPlainImage(png));

    GLTarget t;
    if (!t.Init(48, 32)) ENG_SKIP("no GL context");
    t.Begin();
    ENG_CHECK(atlas.DrawTiled(t.r2d, "atlas_tile_src", Rect{0, 0, 32, 16}));
    t.End();
    std::vector<u8> img;
    ENG_CHECK(t.Read(&img));
    // По одному пикселю внутри каждого из четырёх тайлов закрашено...
    const int probes[4][2] = {{2, 2}, {10, 2}, {18, 10}, {30, 14}};
    for (const auto& p : probes) {
        Rgba c;
        ENG_CHECK(t.Pixel(img, p[0], p[1], &c));
        ENG_CHECK_MSG(c.g > 150 && c.b > 150, "tile pixel is inked");
    }
    // ... а вне запрошенного прямоугольника ничего не нарисовано.
    Rgba outside;
    ENG_CHECK(t.Pixel(img, 40, 24, &outside));
    ENG_CHECK(outside.r < 40 && outside.g < 40 && outside.b < 40);
}

ENG_TEST(Atlas, DrawAnimationChangesWithTime) {
    ENG_REQUIRE_GL();
    const std::string red = test::TempFilePath("blink_0.png");
    const std::string blue = test::TempFilePath("blink_1.png");
    ENG_CHECK(WriteImage(red, 16, 16, [](int, int) { return Rgba{255, 0, 0, 255}; }));
    ENG_CHECK(WriteImage(blue, 16, 16, [](int, int) { return Rgba{0, 0, 255, 255}; }));
    SpriteAtlas atlas;
    ENG_CHECK(atlas.BuildFromFiles({red, blue}));
    ENG_CHECK_EQ(atlas.BuildAnimationsFromPrefixes("_"), 1);
    ENG_CHECK(atlas.FindAnimation("blink") != nullptr);

    GLTarget t;
    if (!t.Init(32, 32)) ENG_SKIP("no GL context");
    t.Begin();
    ENG_CHECK(atlas.DrawAnimation(t.r2d, "blink", Rect{0, 0, 32, 32}, 0.05f));
    t.End();
    std::vector<u8> first;
    ENG_CHECK(t.Read(&first));
    Rgba c0;
    ENG_CHECK(t.Pixel(first, 16, 16, &c0));
    ENG_CHECK_MSG(c0.r > 200 && c0.b < 60, "first frame is red");

    t.Begin();
    ENG_CHECK(atlas.DrawAnimation(t.r2d, "blink", Rect{0, 0, 32, 32}, 0.15f));
    t.End();
    std::vector<u8> second;
    ENG_CHECK(t.Read(&second));
    Rgba c1;
    ENG_CHECK(t.Pixel(second, 16, 16, &c1));
    ENG_CHECK_MSG(c1.b > 200 && c1.r < 60, "second frame is blue");

    t.Begin();
    ENG_CHECK(atlas.DrawAnimationFrame(t.r2d, "blink", 0, Rect{0, 0, 32, 32}, Color::White));
    ENG_CHECK(!atlas.DrawAnimation(t.r2d, "nope", Rect{0, 0, 32, 32}, 0.0f));
    t.End();
}

ENG_TEST(Atlas, DrawAnchoredHonoursPivot) {
    ENG_REQUIRE_GL();
    const std::string png = test::TempFilePath("atlas_anchor_src.png");
    ENG_CHECK(WriteImage(png, 16, 16, [](int, int) { return Rgba{255, 255, 255, 255}; }));

    SpriteAtlas centred;
    ENG_CHECK(LoadSingleFrame("atlas_anchor_centre.json", png, 16, 16, 0.5f, 0.5f, &centred));
    SpriteAtlas corner;
    ENG_CHECK(LoadSingleFrame("atlas_anchor_topleft.json", png, 16, 16, 0.0f, 0.0f, &corner));

    GLTarget t;
    if (!t.Init(64, 64)) ENG_SKIP("no GL context");

    t.Begin();
    ENG_CHECK(centred.DrawAnchored(t.r2d, "r", {32, 32}));
    t.End();
    std::vector<u8> img;
    ENG_CHECK(t.Read(&img));
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    ENG_CHECK(InkBounds(t, img, &x0, &y0, &x1, &y1));
    ENG_CHECK_NEAR(x0, 24.0f, 1.0f);
    ENG_CHECK_NEAR(y0, 24.0f, 1.0f);
    ENG_CHECK_NEAR(x1, 39.0f, 1.0f);
    ENG_CHECK_NEAR(y1, 39.0f, 1.0f);

    t.Begin();
    ENG_CHECK(corner.DrawAnchored(t.r2d, "r", {32, 32}));
    t.End();
    std::vector<u8> img2;
    ENG_CHECK(t.Read(&img2));
    ENG_CHECK(InkBounds(t, img2, &x0, &y0, &x1, &y1));
    ENG_CHECK_NEAR(x0, 32.0f, 1.0f);
    ENG_CHECK_NEAR(y0, 32.0f, 1.0f);
    ENG_CHECK_NEAR(x1, 47.0f, 1.0f);
    ENG_CHECK_NEAR(y1, 47.0f, 1.0f);
}

ENG_TEST(Atlas, RotatedRegionDrawsUpright) {
    ENG_REQUIRE_GL();
    // Источник 8x32 с четырьмя цветными квадрантами.
    const int W = 8, H = 32;
    const Rgba quad[4] = {{255, 0, 0, 255}, {0, 255, 0, 255}, {0, 0, 255, 255}, {255, 255, 0, 255}};
    auto source = [&](int x, int y) {
        return quad[(x < W / 2 ? 0 : 1) + (y < H / 2 ? 0 : 2)];
    };
    const std::string png = test::TempFilePath("atlas_rotated_page.png");
    // Сохраняем спрайт повёрнутым на 90 градусов по часовой: (x,y) -> (H-1-y, x).
    ENG_CHECK(WriteImage(png, H, W, [&](int sx, int sy) {
        const int x = sy;
        const int y = H - 1 - sx;
        return source(x, y);
    }));

    const std::string json =
        "{\n  \"meta\": {\"app\": \"CrossRender\", \"image\": \"" + PathBase(png) +
        "\", \"size\": {\"w\": " + std::to_string(H) + ", \"h\": " + std::to_string(W) + "}},\n"
        "  \"frames\": {\"r\": {\"frame\": {\"x\":0,\"y\":0,\"w\":" + std::to_string(H) +
        ",\"h\":" + std::to_string(W) +
        "}, \"rotated\": true, \"trimmed\": false, \"spriteSourceSize\": {\"x\":0,\"y\":0,\"w\":" +
        std::to_string(W) + ",\"h\":" + std::to_string(H) + "}, \"sourceSize\": {\"w\":" +
        std::to_string(W) + ",\"h\":" + std::to_string(H) + "}}}\n}\n";
    const std::string descriptor = WriteTemp("atlas_rotated.json", json);

    SpriteAtlas atlas;
    ENG_CHECK(atlas.LoadFromFile(descriptor));
    const AtlasRegion* r = atlas.Find("r");
    ENG_CHECK(r != nullptr);
    if (!r) return;
    ENG_CHECK(r->rotated);
    ENG_CHECK_NEAR(r->Size().x, 8.0f, 1e-4f);
    ENG_CHECK_NEAR(r->Size().y, 32.0f, 1e-4f);
    ENG_CHECK(atlas.PageTexture(0).Valid());

    GLTarget t;
    if (!t.Init(32, 32)) ENG_SKIP("no GL context");
    t.Begin();
    ENG_CHECK(atlas.Draw(t.r2d, "r", Rect{0, 0, 8, 32}));
    t.End();
    std::vector<u8> img;
    ENG_CHECK(t.Read(&img));
    // Центры четырёх квадрантов должны оказаться там же, где у *неповернутого* спрайта.
    struct Probe {
        int x, y;
        Rgba expected;
    };
    const Probe probes[4] = {{2, 8, quad[0]}, {6, 8, quad[1]}, {2, 24, quad[2]}, {6, 24, quad[3]}};
    for (const Probe& p : probes) {
        Rgba c;
        ENG_CHECK(t.Pixel(img, p.x, p.y, &c));
        ENG_CHECK_MSG(SameColour(c, p.expected, 30), "rotated quadrant colour");
    }
    // Был затронут только прямоугольник 8x32.
    Rgba outside;
    ENG_CHECK(t.Pixel(img, 20, 20, &outside));
    ENG_CHECK(outside.r < 30 && outside.g < 30 && outside.b < 30);
}

ENG_TEST(Atlas, DrawNinePatchKeepsCorners) {
    ENG_REQUIRE_GL();
    const int S = 30;
    // Отдельный цвет для каждого углового блока 10x10 и центра.
    const std::string png = test::TempFilePath("atlas_nine_src.png");
    ENG_CHECK(WriteImage(png, S, S, [](int x, int y) {
        const int cx = x < 10 ? 0 : (x < 20 ? 1 : 2);
        const int cy = y < 10 ? 0 : (y < 20 ? 1 : 2);
        if (cx == 1 && cy == 1) return Rgba{255, 255, 255, 255};
        if (cx == 0 && cy == 0) return Rgba{255, 0, 0, 255};
        if (cx == 2 && cy == 0) return Rgba{0, 255, 0, 255};
        if (cx == 0 && cy == 2) return Rgba{0, 0, 255, 255};
        if (cx == 2 && cy == 2) return Rgba{255, 255, 0, 255};
        return Rgba{120, 120, 120, 255};
    }));
    SpriteAtlas atlas;
    ENG_CHECK(atlas.LoadPlainImage(png));

    GLTarget t;
    if (!t.Init(100, 100)) ENG_SKIP("no GL context");
    t.Begin();
    ENG_CHECK(atlas.DrawNinePatch(t.r2d, "atlas_nine_src", Rect{0, 0, 100, 100}, 10.0f));
    t.End();
    std::vector<u8> img;
    ENG_CHECK(t.Read(&img));
    Rgba tl, tr, bl, br, mid;
    ENG_CHECK(t.Pixel(img, 4, 4, &tl));
    ENG_CHECK(t.Pixel(img, 95, 4, &tr));
    ENG_CHECK(t.Pixel(img, 4, 95, &bl));
    ENG_CHECK(t.Pixel(img, 95, 95, &br));
    ENG_CHECK(t.Pixel(img, 50, 50, &mid));
    ENG_CHECK_MSG(tl.r > 200 && tl.g < 60 && tl.b < 60, "top-left corner stays red");
    ENG_CHECK_MSG(tr.g > 200 && tr.r < 60, "top-right corner stays green");
    ENG_CHECK_MSG(bl.b > 200 && bl.r < 60, "bottom-left corner stays blue");
    ENG_CHECK_MSG(br.r > 200 && br.g > 200 && br.b < 60, "bottom-right corner stays yellow");
    ENG_CHECK_MSG(mid.r > 200 && mid.g > 200 && mid.b > 200, "centre stays white");
}
