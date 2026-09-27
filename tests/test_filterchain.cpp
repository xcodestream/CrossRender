// Тесты FilterChain: метаданные/значения по умолчанию/пресеты на CPU, плюс
// настоящие GL-тесты, прогоняющие каждый фильтр по насыщенному тестовому
// изображению и проверяющие непустой, неоднородный результат, изменивший источник.
#include "crossrender/gfx/GL.h"
#include "crossrender/test/Test.h"
#include "crossrender/gfx/Renderer2D.h"
#include "crossrender/gfx/FilterChain.h"
#include "crossrender/gfx/RenderTarget.h"

#include <cmath>
#include <string>
#include <vector>
#include <cstdlib>
#include <cstring>
#include <algorithm>

using namespace crossrender;

namespace {

// ---------------------------------------------------------------------------
// Общие помощники
// ---------------------------------------------------------------------------
struct ImageStats {
    double mean = 0.0;    // средняя яркость в [0,1]
    double stddev = 0.0;  // стандартное отклонение яркости
};

ImageStats Analyse(const std::vector<u8>& rgba) {
    ImageStats s;
    const usize n = rgba.size() / 4;
    if (n == 0) return s;
    double sum = 0.0;
    double sumSq = 0.0;
    for (usize i = 0; i < n; ++i) {
        double l = (0.2126 * rgba[i * 4 + 0] + 0.7152 * rgba[i * 4 + 1] + 0.0722 * rgba[i * 4 + 2]) /
                   255.0;
        sum += l;
        sumSq += l * l;
    }
    s.mean = sum / static_cast<double>(n);
    double var = sumSq / static_cast<double>(n) - s.mean * s.mean;
    s.stddev = var > 0.0 ? std::sqrt(var) : 0.0;
    return s;
}

// Число пикселей, чей RGB отличается от эталона больше чем на tolerance.
int DifferingPixels(const std::vector<u8>& a, const std::vector<u8>& b, int tolerance) {
    const usize n = std::min(a.size(), b.size()) / 4;
    int diff = 0;
    for (usize i = 0; i < n; ++i) {
        int dr = std::abs(static_cast<int>(a[i * 4 + 0]) - static_cast<int>(b[i * 4 + 0]));
        int dg = std::abs(static_cast<int>(a[i * 4 + 1]) - static_cast<int>(b[i * 4 + 1]));
        int db = std::abs(static_cast<int>(a[i * 4 + 2]) - static_cast<int>(b[i * 4 + 2]));
        if (dr > tolerance || dg > tolerance || db > tolerance) ++diff;
    }
    return diff;
}

bool FiniteParams(const FilterParams& p) {
    auto fin = [](f32 v) { return std::isfinite(v); };
    return fin(p.amount) && fin(p.radius) && fin(p.sigma) && fin(p.center.x) && fin(p.center.y) &&
           fin(p.direction.x) && fin(p.direction.y) && fin(p.angle) && fin(p.frequency) &&
           fin(p.amplitude) && fin(p.threshold) && fin(p.levels) && fin(p.tint.r) && fin(p.tint.g) &&
           fin(p.tint.b) && fin(p.tint.a) && fin(p.lift.r) && fin(p.lift.g) && fin(p.lift.b) &&
           fin(p.lift.a) && fin(p.gain.r) && fin(p.gain.g) && fin(p.gain.b) && fin(p.gain.a) &&
           fin(p.gamma) && fin(p.saturation) && fin(p.temperature) && fin(p.vignette) &&
           fin(p.grain) && fin(p.scanlineStrength) && fin(p.curvature) && fin(p.aberration) &&
           fin(p.bloomThreshold) && fin(p.feedback);
}

// Исходное изображение с плавным градиентом, резкой фигурой и мелкой цветной
// шахматной доской: достаточно высокочастотных деталей, чтобы каждый локальный
// фильтр заметно менял сильно больше 5% пикселей.
struct FilterFixture {
    static const int kSize = 128;
    RenderTarget source;
    Renderer2D r2d;
    bool ok = false;

    bool Init() {
        RenderTargetDesc desc;
        desc.width = kSize;
        desc.height = kSize;
        desc.colorFormat = PixelFormat::RGBA8;
        desc.depth = false;
        desc.stencil = false;
        desc.name = "filter-source";
        if (!source.Create(desc)) return false;
        if (!r2d.Init()) return false;

        source.Bind();
        source.Clear(Color{0, 0, 0, 1}, false, false);
        r2d.BeginFrame(kSize, kSize, 1.0f, &source);
        // Плавный вертикальный градиент по всему изображению.
        r2d.FillRectGradient(Rect{0, 0, static_cast<f32>(kSize), static_cast<f32>(kSize)},
                             Color{0.06f, 0.10f, 0.32f, 1.0f}, Color{0.85f, 0.62f, 0.18f, 1.0f}, true);
        // Мелкая 4px шахматная доска насыщенных цветов (много граней).
        for (int y = 0; y < kSize; y += 4) {
            for (int x = 0; x < kSize; x += 4) {
                if (((x / 4) + (y / 4)) % 2 != 0) continue;
                Color c = Color::FromBytes(static_cast<u8>(30 + (x * 3) % 200),
                                           static_cast<u8>(20 + (y * 2) % 180),
                                           static_cast<u8>(70 + (x + y) % 160), 255);
                r2d.FillRect(Rect{static_cast<f32>(x), static_cast<f32>(y), 4, 4}, c);
            }
        }
        // Фигура с резкими краями.
        r2d.FillCircle(64.0f, 64.0f, 24.0f, Color{0.96f, 0.96f, 0.99f, 1.0f});
        r2d.EndFrame();
        source.Unbind();
        ok = true;
        return true;
    }
};

bool CreateRgbaTarget(RenderTarget& rt, int size, const char* name) {
    RenderTargetDesc desc;
    desc.width = size;
    desc.height = size;
    desc.colorFormat = PixelFormat::RGBA8;
    desc.depth = false;
    desc.stencil = false;
    desc.name = name;
    return rt.Create(desc);
}

}  // namespace

// ---------------------------------------------------------------------------
// Метаданные (только CPU)
// ---------------------------------------------------------------------------
ENG_TEST(FilterMetadata, NamesAndDescriptions) {
    for (int i = 0; i < static_cast<int>(FilterType::Count); ++i) {
        FilterType t = static_cast<FilterType>(i);
        const char* name = FilterChain::FilterName(t);
        const char* desc = FilterChain::FilterDescription(t);
        ENG_CHECK(name != nullptr);
        ENG_CHECK(desc != nullptr);
        if (name) ENG_CHECK(std::strlen(name) > 0);
        if (desc) ENG_CHECK(std::strlen(desc) > 0);
    }
}

ENG_TEST(FilterMetadata, AllFiltersStableAndUnique) {
    std::vector<FilterType> all = FilterChain::AllFilters();
    ENG_CHECK_EQ(static_cast<int>(all.size()), static_cast<int>(FilterType::Count));
    for (int i = 0; i < static_cast<int>(all.size()); ++i) {
        ENG_CHECK_EQ(static_cast<int>(all[static_cast<usize>(i)]), i);  // стабильный порядок enum
        for (int j = i + 1; j < static_cast<int>(all.size()); ++j) {
            ENG_CHECK(all[static_cast<usize>(i)] != all[static_cast<usize>(j)]);
        }
    }
    std::vector<FilterType> again = FilterChain::AllFilters();
    ENG_CHECK_EQ(again.size(), all.size());
    for (usize i = 0; i < all.size(); ++i) ENG_CHECK(all[i] == again[i]);
}

ENG_TEST(FilterMetadata, DefaultsFiniteAndNonDegenerate) {
    for (int i = 0; i < static_cast<int>(FilterType::Count); ++i) {
        FilterType t = static_cast<FilterType>(i);
        FilterParams p = FilterChain::Defaults(t);
        const char* name = FilterChain::FilterName(t);
        ENG_CHECK_MSG(FiniteParams(p), name);
        ENG_CHECK_MSG(p.cellSize >= 1, name);
        ENG_CHECK_MSG(p.segments >= 3, name);
        ENG_CHECK_MSG(p.levels >= 2.0f, name);

        switch (t) {
            case FilterType::Bloom:
                ENG_CHECK_MSG(p.radius > 0.0f && p.amount > 0.0f, name);
                break;
            case FilterType::Blur:
            case FilterType::GaussianBlur:
                ENG_CHECK_MSG(p.radius > 0.0f, name);
                ENG_CHECK_MSG(p.sigma > 0.0f, name);
                break;
            case FilterType::RadialBlur:
            case FilterType::ZoomBlur:
                ENG_CHECK_MSG(p.amount > 0.0f, name);
                ENG_CHECK_MSG(p.center.x > 0.0f && p.center.x < 1.0f, name);
                break;
            case FilterType::MotionBlur:
                ENG_CHECK_MSG(p.amount > 0.0f, name);
                ENG_CHECK_MSG(std::fabs(p.direction.x) + std::fabs(p.direction.y) > 0.0f, name);
                break;
            case FilterType::Sharpen:
                ENG_CHECK_MSG(p.amount > 0.0f && p.radius > 0.0f, name);
                break;
            case FilterType::EdgeDetect:
            case FilterType::Emboss:
                ENG_CHECK_MSG(p.amount > 0.0f, name);
                break;
            case FilterType::Pixelate:
            case FilterType::Halftone:
                ENG_CHECK_MSG(p.cellSize >= 2, name);
                break;
            case FilterType::Posterize:
            case FilterType::Dither:
                ENG_CHECK_MSG(p.levels >= 2.0f, name);
                break;
            case FilterType::Scanlines:
                ENG_CHECK_MSG(p.scanlineStrength > 0.0f, name);
                break;
            case FilterType::Crt:
                ENG_CHECK_MSG(p.curvature > 0.0f, name);
                ENG_CHECK_MSG(p.scanlineStrength > 0.0f, name);
                break;
            case FilterType::Vignette:
                ENG_CHECK_MSG(p.vignette > 0.0f, name);
                break;
            case FilterType::FilmGrain:
                ENG_CHECK_MSG(p.grain > 0.0f, name);
                break;
            case FilterType::ChromaticAberration:
                ENG_CHECK_MSG(p.aberration > 0.0f, name);
                break;
            case FilterType::BarrelDistort:
                ENG_CHECK_MSG(std::fabs(p.amount) > 0.0f, name);
                break;
            case FilterType::WaveDistort:
                ENG_CHECK_MSG(p.amplitude > 0.0f && p.frequency > 0.0f, name);
                break;
            case FilterType::Glitch:
            case FilterType::Fisheye:
            case FilterType::Swirl:
            case FilterType::Invert:
            case FilterType::Bleed:
                ENG_CHECK_MSG(p.amount > 0.0f, name);
                break;
            case FilterType::Kaleidoscope:
                ENG_CHECK_MSG(p.segments >= 3, name);
                break;
            case FilterType::ColorGrade:
                ENG_CHECK_MSG(std::fabs(p.saturation - 1.0f) > 0.0f ||
                                  std::fabs(p.temperature) > 0.0f,
                              name);
                break;
            case FilterType::HueShift:
                ENG_CHECK_MSG(std::fabs(p.angle) > 0.0f, name);
                break;
            case FilterType::Threshold:
                ENG_CHECK_MSG(p.threshold > 0.0f && p.threshold < 1.0f, name);
                break;
            case FilterType::Sepia:
                ENG_CHECK_MSG(p.amount > 0.0f, name);
                break;
            case FilterType::Feedback:
                ENG_CHECK_MSG(p.feedback > 0.0f, name);
                break;
            case FilterType::Count:
            default:
                break;
        }
    }
}

// ---------------------------------------------------------------------------
// Пресеты (только CPU)
// ---------------------------------------------------------------------------
ENG_TEST(FilterPresets, NamesAndContent) {
    std::vector<std::string> names = FilterChain::PresetNames();
    ENG_CHECK(!names.empty());
    ENG_CHECK(names.size() >= 10);
    for (usize i = 0; i < names.size(); ++i) {
        ENG_CHECK(!names[i].empty());
        for (usize j = i + 1; j < names.size(); ++j) ENG_CHECK(names[i] != names[j]);
    }
    const char* required[] = {"Clean",  "Dreamy", "CRT",     "Glitch", "Painterly",
                              "Noir",   "Pixel",  "Underwater", "Kaleido", "Dream"};
    for (const char* want : required) {
        bool found = false;
        for (const std::string& n : names) found = found || (n == want);
        ENG_CHECK_MSG(found, std::string("missing preset ") + want);
    }

    for (const std::string& n : names) {
        FilterChain chain = FilterChain::MakePreset(n);
        if (n == "Clean") {
            ENG_CHECK_EQ(chain.Count(), 0);
            continue;
        }
        ENG_CHECK_MSG(chain.Count() > 0, n);
        for (int i = 0; i < chain.Count(); ++i) {
            const FilterInstance& f = chain.At(i);
            ENG_CHECK(f.enabled);
            ENG_CHECK(static_cast<int>(f.type) >= 0 &&
                      static_cast<int>(f.type) < static_cast<int>(FilterType::Count));
            ENG_CHECK_MSG(FiniteParams(f.params), n);
            const char* fname = FilterChain::FilterName(f.type);
            ENG_CHECK(fname != nullptr && std::strlen(fname) > 0);
        }
    }
}

ENG_TEST(FilterPresets, MoveRoundTrip) {
    FilterChain c = FilterChain::MakePreset("CRT");
    const int n = c.Count();
    ENG_CHECK(n >= 4);
    ENG_CHECK(c.At(0).type == FilterType::Crt);

    FilterChain d = std::move(c);
    ENG_CHECK_EQ(d.Count(), n);
    ENG_CHECK(d.At(0).enabled);
    // Цепь после move должна оставаться валидной (пустой) и пригодной к повторному использованию.
    ENG_CHECK_EQ(c.Count(), 0);
    c.Add(FilterType::Invert, FilterChain::Defaults(FilterType::Invert));
    ENG_CHECK_EQ(c.Count(), 1);

    FilterChain e;
    e = std::move(d);
    ENG_CHECK_EQ(e.Count(), n);

    // Самоприсваивание перемещением должно быть безопасным no-op.
    FilterChain* self = &e;
    e = std::move(*self);
    ENG_CHECK_EQ(e.Count(), n);

    // Пресет переживает перемещение и сохраняет каждый фильтр.
    FilterChain f = FilterChain::MakePreset("Noir");
    const int m = f.Count();
    FilterChain g = std::move(f);
    ENG_CHECK_EQ(g.Count(), m);
}

// ---------------------------------------------------------------------------
// Учёт цепи (только CPU)
// ---------------------------------------------------------------------------
ENG_TEST(FilterChain, SafeAccessAndEditing) {
    FilterChain c;
    ENG_CHECK_EQ(c.Count(), 0);
    const FilterChain& cc = c;
    // Доступ вне диапазона возвращает черновую запись вместо чтения за границей.
    ENG_CHECK(cc.At(0).type == FilterType::Blur);
    ENG_CHECK(cc.At(-7).type == FilterType::Blur);
    (void)c.At(1000);
    c.At(1000).type = FilterType::Bloom;  // не должно портить цепь
    ENG_CHECK_EQ(c.Count(), 0);

    c.Add(FilterType::Invert, FilterChain::Defaults(FilterType::Invert));
    c.Add(FilterType::Sepia, FilterChain::Defaults(FilterType::Sepia));
    c.Add(FilterType::Pixelate, FilterChain::Defaults(FilterType::Pixelate));
    ENG_CHECK_EQ(c.Count(), 3);
    ENG_CHECK(c.At(0).type == FilterType::Invert);
    ENG_CHECK(c.At(2).type == FilterType::Pixelate);
    ENG_CHECK_EQ(c.Filters().size(), 3u);

    c.MoveUp(2);
    ENG_CHECK(c.At(1).type == FilterType::Pixelate);
    c.MoveDown(1);
    ENG_CHECK(c.At(2).type == FilterType::Pixelate);
    c.MoveUp(0);  // no-op
    c.MoveDown(99);  // no-op
    c.Remove(-1);  // no-op
    c.Remove(99);  // no-op
    ENG_CHECK_EQ(c.Count(), 3);
    c.Remove(0);
    ENG_CHECK_EQ(c.Count(), 2);
    ENG_CHECK(c.At(0).type == FilterType::Sepia);

    c.Clear();
    ENG_CHECK_EQ(c.Count(), 0);
}

// ---------------------------------------------------------------------------
// Выполнение на GL
// ---------------------------------------------------------------------------
ENG_TEST(FilterChain, EmptyChainCopiesSourceThrough) {
    ENG_REQUIRE_GL();
    FilterFixture fx;
    if (!fx.Init()) ENG_SKIP("Renderer2D unavailable");
    RenderTarget dst;
    if (!CreateRgbaTarget(dst, FilterFixture::kSize, "filter-empty-dst"))
        ENG_SKIP("render target creation failed");

    std::vector<u8> srcPixels;
    ENG_CHECK(fx.source.ReadPixels(&srcPixels));
    ENG_CHECK(!srcPixels.empty());
    ImageStats srcStats = Analyse(srcPixels);
    ENG_CHECK(srcStats.mean > 0.01);
    ENG_CHECK(srcStats.stddev > 0.01);

    FilterChain chain;
    if (!chain.Init()) {
        ENG_TEST_FAIL("FilterChain shaders failed to compile");
        return;
    }
    chain.Clear();
    chain.Apply(fx.source.ColorTexture(), dst.Fbo(), FilterFixture::kSize, FilterFixture::kSize);

    std::vector<u8> out;
    ENG_CHECK(dst.ReadPixels(&out));
    ENG_CHECK_EQ(out.size(), srcPixels.size());
    const int pixels = static_cast<int>(srcPixels.size() / 4);
    // Проходная копия должна быть (почти) попиксельно идентична источнику.
    ENG_CHECK(DifferingPixels(srcPixels, out, 2) < pixels / 50 + 8);
    ImageStats outStats = Analyse(out);
    ENG_CHECK(outStats.mean > 0.01);
    ENG_CHECK(outStats.stddev > 0.01);
}

ENG_TEST(FilterChain, EveryFilterProducesImage) {
    ENG_REQUIRE_GL();
    FilterFixture fx;
    if (!fx.Init()) ENG_SKIP("Renderer2D unavailable");
    RenderTarget dst;
    if (!CreateRgbaTarget(dst, FilterFixture::kSize, "filter-single-dst"))
        ENG_SKIP("render target creation failed");

    std::vector<u8> srcPixels;
    ENG_CHECK(fx.source.ReadPixels(&srcPixels));
    const int pixels = static_cast<int>(srcPixels.size() / 4);
    ENG_CHECK(pixels > 0);
    ImageStats srcStats = Analyse(srcPixels);
    ENG_CHECK(srcStats.mean > 0.01);
    ENG_CHECK(srcStats.stddev > 0.01);

    FilterChain chain;
    if (!chain.Init()) {
        ENG_TEST_FAIL("FilterChain shaders failed to compile");
        return;
    }

    for (int i = 0; i < static_cast<int>(FilterType::Count); ++i) {
        FilterType t = static_cast<FilterType>(i);
        const char* name = FilterChain::FilterName(t);
        FilterInstance inst;
        inst.type = t;
        inst.enabled = true;
        inst.params = FilterChain::Defaults(t);

        chain.ApplySingle(fx.source.ColorTexture(), inst, dst.Fbo(), FilterFixture::kSize,
                          FilterFixture::kSize);

        std::vector<u8> out;
        ENG_CHECK(dst.ReadPixels(&out));
        if (out.size() != srcPixels.size()) {
            ENG_TEST_FAIL(std::string(name) + ": read-back size mismatch");
            continue;
        }

        // (a) непустое изображение, (b) не однородный цвет.
        ImageStats st = Analyse(out);
        ENG_CHECK_MSG(st.mean > 0.01, std::string(name) + ": image is black/empty");
        ENG_CHECK_MSG(st.stddev > 0.01, std::string(name) + ": image is a uniform colour");

        // (c) он изменил источник более чем на 5% пикселей.
        int diff = DifferingPixels(srcPixels, out, 3);
        ENG_CHECK_MSG(diff > pixels / 20, std::string(name) + ": effect changed <5% of pixels");

        if (t == FilterType::Feedback) {
            ENG_CHECK_MSG(chain.GetStats().usedFeedback, "Feedback did not use its history target");
        }
    }
}

ENG_TEST(FilterChain, FeedbackUsesHistoryAcrossFrames) {
    ENG_REQUIRE_GL();
    FilterFixture fx;
    if (!fx.Init()) ENG_SKIP("Renderer2D unavailable");
    RenderTarget dst;
    if (!CreateRgbaTarget(dst, FilterFixture::kSize, "filter-feedback-dst"))
        ENG_SKIP("render target creation failed");

    FilterChain chain;
    if (!chain.Init()) {
        ENG_TEST_FAIL("FilterChain shaders failed to compile");
        return;
    }
    FilterInstance fb;
    fb.type = FilterType::Feedback;
    fb.enabled = true;
    fb.params = FilterChain::Defaults(FilterType::Feedback);
    fb.params.feedback = 0.9f;

    chain.ApplySingle(fx.source.ColorTexture(), fb, dst.Fbo(), FilterFixture::kSize,
                      FilterFixture::kSize);
    ENG_CHECK(chain.GetStats().usedFeedback);
    std::vector<u8> frame1;
    ENG_CHECK(dst.ReadPixels(&frame1));

    chain.ApplySingle(fx.source.ColorTexture(), fb, dst.Fbo(), FilterFixture::kSize,
                      FilterFixture::kSize);
    std::vector<u8> frame2;
    ENG_CHECK(dst.ReadPixels(&frame2));

    ImageStats s1 = Analyse(frame1);
    ImageStats s2 = Analyse(frame2);
    // Кадр 2 подмешивает кадр 1, поэтому эхо накапливает яркость, и кадры
    // не должны быть идентичны.
    ENG_CHECK(s1.mean > 0.005);
    ENG_CHECK_MSG(s2.mean > s1.mean, "feedback history was not mixed in");
    ENG_CHECK(DifferingPixels(frame1, frame2, 2) > 0);
}

ENG_TEST(FilterChain, MultiFilterChainProducesImage) {
    ENG_REQUIRE_GL();
    FilterFixture fx;
    if (!fx.Init()) ENG_SKIP("Renderer2D unavailable");
    RenderTarget dst;
    if (!CreateRgbaTarget(dst, FilterFixture::kSize, "filter-multi-dst"))
        ENG_SKIP("render target creation failed");

    FilterChain chain;
    if (!chain.Init()) {
        ENG_TEST_FAIL("FilterChain shaders failed to compile");
        return;
    }
    chain.Add(FilterType::GaussianBlur, FilterChain::Defaults(FilterType::GaussianBlur));
    chain.Add(FilterType::Bloom, FilterChain::Defaults(FilterType::Bloom));
    chain.Add(FilterType::ChromaticAberration,
              FilterChain::Defaults(FilterType::ChromaticAberration));
    chain.Add(FilterType::Vignette, FilterChain::Defaults(FilterType::Vignette));
    ENG_CHECK_EQ(chain.Count(), 4);

    chain.Apply(fx.source.ColorTexture(), dst.Fbo(), FilterFixture::kSize, FilterFixture::kSize);

    const FilterChain::Stats& stats = chain.GetStats();
    ENG_CHECK_MSG(stats.passes >= 4, "chain did not run a pass per filter");
    ENG_CHECK_MSG(stats.pingPong >= 3, "chain did not ping-pong between targets");

    std::vector<u8> out;
    ENG_CHECK(dst.ReadPixels(&out));
    ImageStats st = Analyse(out);
    ENG_CHECK(st.mean > 0.01);
    ENG_CHECK(st.stddev > 0.01);
}

ENG_TEST(FilterChain, PreviewGridRendersCells) {
    ENG_REQUIRE_GL();
    FilterFixture fx;
    if (!fx.Init()) ENG_SKIP("Renderer2D unavailable");
    RenderTarget grid;
    if (!CreateRgbaTarget(grid, FilterFixture::kSize, "filter-preview-grid"))
        ENG_SKIP("render target creation failed");

    FilterChain chain;
    if (!chain.Init()) {
        ENG_TEST_FAIL("FilterChain shaders failed to compile");
        return;
    }
    std::vector<FilterType> types = {FilterType::Bloom, FilterType::Pixelate, FilterType::Invert,
                                     FilterType::Sepia};

    grid.Bind();
    grid.Clear(Color{0, 0, 0, 1}, false, false);
    chain.PreviewGrid(fx.source.ColorTexture(), types, 2, FilterFixture::kSize,
                      FilterFixture::kSize);
    grid.Unbind();

    std::vector<u8> out;
    ENG_CHECK(grid.ReadPixels(&out));
    ImageStats st = Analyse(out);
    ENG_CHECK(st.mean > 0.01);
    ENG_CHECK(st.stddev > 0.01);
    ENG_CHECK_MSG(chain.GetStats().passes >= static_cast<int>(types.size()),
                  "preview grid did not run one pass per cell");

    // Каждый квадрант должен нести контент (сетка 2x2).
    for (int cell = 0; cell < 4; ++cell) {
        const int cx = (cell % 2) * (FilterFixture::kSize / 2);
        const int cy = (cell / 2) * (FilterFixture::kSize / 2);
        double sum = 0.0;
        int count = 0;
        for (int y = cy; y < cy + FilterFixture::kSize / 2; ++y) {
            for (int x = cx; x < cx + FilterFixture::kSize / 2; ++x) {
                usize idx = (static_cast<usize>(y) * FilterFixture::kSize +
                             static_cast<usize>(x)) * 4;
                if (idx + 2 >= out.size()) continue;
                sum += (0.2126 * out[idx] + 0.7152 * out[idx + 1] + 0.0722 * out[idx + 2]) / 255.0;
                ++count;
            }
        }
        ENG_CHECK(count > 0);
        ENG_CHECK_MSG(count > 0 && sum / count > 0.01, "preview cell is empty");
    }
}
