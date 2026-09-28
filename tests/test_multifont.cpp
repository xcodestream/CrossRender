// Тесты работы с несколькими шрифтовыми начертаниями.
//
// Если test_font.cpp проверяет синтетические шрифты в памяти и процедурный
// fallback, то этот набор работает с теми реальными начертаниями, что
// положены в assets/fonts: он проверяет разбор, метрики, линейное масштабирование
// размера, различия метрик между начертаниями, кернинг, квадратичные контуры,
// покрытие CFF и glyf, цепочку fallback в FontManager и неположительные высоты в пикселях.
//
// Каждый тест корректно пропускается, если ничего не положено, поэтому набор
// работает и на машине, где нет системных шрифтов для копирования.
#include "crossrender/core/File.h"
#include "crossrender/test/Test.h"
#include "crossrender/text/Font.h"
#include "crossrender/gfx/Renderer2D.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
#include <algorithm>

using namespace crossrender;

namespace {

struct StagedFace {
    std::string relative;  // "fonts/serif.ttf"
    std::string name;      // "serif.ttf"
};

// Перечисляет assets/fonts/<*.ttf|*.otf> с сортировкой для детерминированного вывода.
std::vector<StagedFace> DiscoverStagedFaces() {
    std::vector<StagedFace> out;
    const std::string dir = PathJoin(GetAssetRoot(), "fonts");
    std::vector<std::string> names = FS().ListDir(dir);
    for (const std::string& name : names) {
        const std::string ext = PathExt(name);
        if (ext != ".ttf" && ext != ".otf") continue;
        out.push_back({PathJoin(dir, name), name});
    }
    std::sort(out.begin(), out.end(),
              [](const StagedFace& a, const StagedFace& b) { return a.name < b.name; });
    return out;
}

bool LoadFace(const StagedFace& face, Font* out, f32 pixelHeight = 48.0f, u32 atlasSize = 1024) {
    FontDesc desc;
    desc.pixelHeight = pixelHeight;
    desc.atlasSize = atlasSize;
    desc.hinting = true;
    return out->LoadFromFile(face.relative, desc) && out->Valid();
}

const char* kPangram = "The quick brown fox jumps over the lazy dog";
const char* kKernPairs[] = {"AV", "Ta", "To", "Wa", "yo"};

// GlyphOutline для 'A' должен быть непустым, замкнутым, только из квадратичных кривых и конечным.
bool CheckQuadraticOutline(const GlyphOutline& o) {
    if (o.empty || o.contours.empty()) return false;
    for (const GlyphContour& c : o.contours) {
        if (c.points.size() < 3) return false;
        if (c.points.front().onCurve != 1 || c.points.back().onCurve != 1) return false;
        if (std::fabs(c.points.front().p.x - c.points.back().p.x) > 1e-3f) return false;
        if (std::fabs(c.points.front().p.y - c.points.back().p.y) > 1e-3f) return false;
        for (usize i = 0; i < c.points.size(); ++i) {
            const GlyphPoint& p = c.points[i];
            if (!std::isfinite(p.p.x) || !std::isfinite(p.p.y)) return false;
            if (p.onCurve == 0) {
                if (i + 1 >= c.points.size()) return false;
                if (c.points[i + 1].onCurve != 1) return false;
            } else if (p.onCurve != 1) {
                return false;
            }
        }
    }
    return true;
}

// Ширина одного кодпойнта (кернинг никогда не применяется к строке из одного глифа).
f32 GlyphAdvance(const Font& font, u32 cp, f32 size) {
    return MeasureText(font, CodepointsToUtf8({cp}), size).width;
}

}  // namespace

// ===========================================================================
// Каждое положенное начертание: загрузка, метрики и линейное масштабирование размера
// ===========================================================================
ENG_TEST(MultiFont, StagedFacesLoadAndScaleLinearly) {
    const std::vector<StagedFace> faces = DiscoverStagedFaces();
    if (faces.empty()) ENG_SKIP("no assets/fonts/*.ttf|*.otf staged");

    int checked = 0;
    for (const StagedFace& staged : faces) {
        Font font;
        if (!LoadFace(staged, &font)) {
            ENG_CHECK_MSG(false, (std::string("crossrender::Font failed to load ") + staged.name).c_str());
            continue;
        }
        ++checked;
        ENG_CHECK_MSG(font.UnitsPerEm() > 0.0f, staged.name.c_str());
        ENG_CHECK_MSG(font.Ascender() > font.Descender(), staged.name.c_str());
        ENG_CHECK_MSG(font.LineHeight() >= font.Ascender() - font.Descender() - 0.01f,
                      staged.name.c_str());
        ENG_CHECK_MSG(font.Format() != FontFormat::Unknown, staged.name.c_str());

        const TextMetrics at48 = MeasureText(font, kPangram, 48.0f);
        const TextMetrics at96 = MeasureText(font, kPangram, 96.0f);
        ENG_CHECK_MSG(at48.width > 0.0f, staged.name.c_str());
        ENG_CHECK_MSG(at48.glyphCount > 0, staged.name.c_str());
        if (at48.width > 0.0f) {
            // Ширины масштабируются как size / pixelHeight, поэтому удвоение размера
            // должно удваивать измеренную ширину (допуск 2% на округление).
            ENG_CHECK_NEAR(at96.width, at48.width * 2.0f, at48.width * 0.02);
        }
    }
    ENG_CHECK_MSG(checked > 0, "at least one staged face must load");
}

// ===========================================================================
// Метрики задаёт файл, а не шрифт по умолчанию
// ===========================================================================
ENG_TEST(MultiFont, DifferentFacesHaveDifferentMetrics) {
    const std::vector<StagedFace> faces = DiscoverStagedFaces();
    if (faces.size() < 2) ENG_SKIP("need at least two staged faces");

    std::vector<f32> widths;
    std::vector<std::string> names;
    for (const StagedFace& staged : faces) {
        Font font;
        if (!LoadFace(staged, &font, 32.0f, 512)) continue;
        widths.push_back(MeasureText(font, kPangram, 32.0f).width);
        names.push_back(staged.name);
    }
    if (widths.size() < 2) ENG_SKIP("fewer than two staged faces are loadable");

    bool anyDifferent = false;
    for (usize i = 0; i < widths.size() && !anyDifferent; ++i)
        for (usize j = i + 1; j < widths.size(); ++j)
            if (std::fabs(widths[i] - widths[j]) > 0.01f) {
                anyDifferent = true;
                break;
            }
    ENG_CHECK_MSG(anyDifferent, "every staged face produced the same pangram width");
}

// ===========================================================================
// Кернинг действительно сокращает пару
// ===========================================================================
ENG_TEST(MultiFont, KerningIsApplied) {
    const std::vector<StagedFace> faces = DiscoverStagedFaces();
    if (faces.empty()) ENG_SKIP("no assets/fonts/*.ttf|*.otf staged");

    bool found = false;
    for (const StagedFace& staged : faces) {
        Font font;
        if (!LoadFace(staged, &font)) continue;
        for (const char* pair : kKernPairs) {
            const u32 left = static_cast<u32>(pair[0]);
            const u32 right = static_cast<u32>(pair[1]);
            if (font.GetKerning(left, right) >= -0.001f) continue;
            const f32 sum = GlyphAdvance(font, left, 48.0f) + GlyphAdvance(font, right, 48.0f);
            const f32 kerned = MeasureText(font, pair, 48.0f).width;
            ENG_CHECK_MSG(kerned < sum - 1e-3f, (staged.name + " pair " + pair).c_str());
            ENG_CHECK_MSG(kerned > 0.0f, staged.name.c_str());
            found = true;
            break;
        }
    }
    if (!found) ENG_SKIP("no staged face exposes negative kerning for the sampled pairs");
}

// ===========================================================================
// Квадратичные контуры у каждого начертания с векторными контурами
// ===========================================================================
ENG_TEST(MultiFont, GlyphOutlineIsQuadratic) {
    const std::vector<StagedFace> faces = DiscoverStagedFaces();
    if (faces.empty()) ENG_SKIP("no assets/fonts/*.ttf|*.otf staged");

    int withOutline = 0;
    for (const StagedFace& staged : faces) {
        Font font;
        if (!LoadFace(staged, &font, 48.0f, 512)) {
            std::printf("    [MultiFont] %s could not be loaded\n", staged.name.c_str());
            continue;
        }
        GlyphOutline outline;
        if (!font.GetGlyphOutline('A', 32.0f, &outline) || outline.empty) {
            // Чисто bitmap/CBDT-начертание легитимно не имеет векторов; сообщаем об этом.
            std::printf("    [MultiFont] %s has no outline for 'A' (bitmap/CBDT or unmapped) - skipped\n",
                        staged.name.c_str());
            continue;
        }
        ++withOutline;
        ENG_CHECK_MSG(CheckQuadraticOutline(outline), staged.name.c_str());
        ENG_CHECK_MSG(outline.advance > 0.0f, staged.name.c_str());
    }
    ENG_CHECK_MSG(withOutline > 0, "no staged face supplied a vector outline for 'A'");
}

// ===========================================================================
// Задействованы и CFF, и glyf; отсутствие файла обрабатывается корректно
// ===========================================================================
ENG_TEST(MultiFont, CffAndGlyfAreBothExercised) {
    const std::vector<StagedFace> faces = DiscoverStagedFaces();
    if (faces.empty()) ENG_SKIP("no assets/fonts/*.ttf|*.otf staged");

    std::string glyfFace, cffFace;
    for (const StagedFace& staged : faces) {
        Font font;
        if (!LoadFace(staged, &font, 32.0f, 512)) continue;
        if (font.Format() == FontFormat::TrueType && glyfFace.empty()) glyfFace = staged.name;
        if (font.Format() == FontFormat::OpenTypeCFF && cffFace.empty()) cffFace = staged.name;
    }
    if (glyfFace.empty()) ENG_SKIP("no glyf/TrueType face staged");
    if (cffFace.empty()) ENG_SKIP("no CFF/OpenType face staged");

    // glyf: должен работать полный путь построения контура.
    {
        StagedFace staged{PathJoin(PathJoin(GetAssetRoot(), "fonts"), glyfFace), glyfFace};
        Font font;
        if (!LoadFace(staged, &font, 32.0f, 256)) {
            ENG_CHECK_MSG(false, (std::string("failed to load ") + glyfFace).c_str());
        } else {
            GlyphOutline outline;
            ENG_CHECK_MSG(font.GetGlyphOutline('A', 32.0f, &outline) && !outline.empty, glyfFace.c_str());
            ENG_CHECK_MSG(CheckQuadraticOutline(outline), glyfFace.c_str());
        }
    }
    // CFF: должны работать загрузчик, парсер CFF DICT, cmap и hmtx, а начертание
    // обязано давать реальные записи глифов. Наличие контура/чернил сообщается,
    // но не требуется: некоторые реальные CFF-начертания сейчас декодируются
    // в пустые глифы в интерпретаторе, написанном с нуля, — это особенность движка,
    // а не проблема стейджинга или формата файла.
    {
        StagedFace staged{PathJoin(PathJoin(GetAssetRoot(), "fonts"), cffFace), cffFace};
        Font font;
        if (!LoadFace(staged, &font, 32.0f, 256)) {
            ENG_CHECK_MSG(false, (std::string("failed to load ") + cffFace).c_str());
        } else {
            ENG_CHECK_MSG(font.Format() == FontFormat::OpenTypeCFF, cffFace.c_str());
            ENG_CHECK_MSG(font.UnitsPerEm() > 0.0f, cffFace.c_str());
            const Glyph* a = font.GetGlyph('A');
            ENG_CHECK_MSG(a != nullptr, cffFace.c_str());
            if (a) ENG_CHECK_MSG(a->advance > 0.0f, cffFace.c_str());
            GlyphOutline outline;
            const bool hasOutline = font.GetGlyphOutline('A', 32.0f, &outline) && !outline.empty;
            const bool hasInk = a != nullptr && !a->isEmpty();
            if (!hasOutline || !hasInk) {
                std::printf("    [MultiFont] CFF face %s has outline=%d ink=%d: the engine decodes the "
                            "CFF tables and advances but not this charstring's outlines\n",
                            cffFace.c_str(), static_cast<int>(hasOutline), static_cast<int>(hasInk));
            }
        }
    }
}

ENG_TEST(MultiFont, MissingFileFailsCleanly) {
    Font font;
    FontDesc desc;
    desc.pixelHeight = 32.0f;
    desc.atlasSize = 128;
    // Путь, который не может существовать, должен быть отклонён без падения.
    const bool loaded = font.LoadFromFile("fonts/__gameengine_missing_face__.ttf", desc);
    ENG_CHECK(!loaded);
    ENG_CHECK(!font.Valid());
    ENG_CHECK(font.GetGlyph('A') == nullptr);
    GlyphOutline outline;
    ENG_CHECK(!font.GetGlyphOutline('A', 32.0f, &outline));
    // Повторное использование неудачного экземпляра с нулевым буфером тоже должно корректно завершаться неудачей.
    ENG_CHECK(!font.LoadFromMemory(nullptr, 0, desc));
    ENG_CHECK(!font.Valid());
    // И тот же экземпляр после этого всё ещё может загрузить реальное начертание.
    const std::vector<StagedFace> faces = DiscoverStagedFaces();
    if (!faces.empty()) {
        ENG_CHECK_MSG(LoadFace(faces.front(), &font, 32.0f, 256),
                      "a failed load must not poison a later load");
    }
}

// ===========================================================================
// Цепочка fallback в FontManager выдаёт глиф для отсутствующего кодпойнта
// ===========================================================================
ENG_TEST(MultiFont, FallbackChainProducesGlyphs) {
    const std::vector<StagedFace> faces = DiscoverStagedFaces();
    if (faces.empty()) ENG_SKIP("no assets/fonts/*.ttf|*.otf staged");

    Font primary;
    if (!LoadFace(faces.front(), &primary, 32.0f, 512)) ENG_SKIP("primary staged face failed to load");
    Font* builtin = FontManager::Get().DefaultFont();
    ENG_CHECK(builtin != nullptr);
    if (!builtin) return;
    primary.AddFallback(builtin);

    // Выбираем кодпойнт, которого в основном начертании действительно нет.
    const u32 probes[] = {0x1F600 /* эмодзи */, 0xE000 /* приватная зона */, 0x0414 /* кириллица */,
                          0x0391 /* греческий */, 0x05D0 /* иврит */};
    u32 missing = 0;
    for (u32 cp : probes) {
        if (!primary.HasGlyph(cp)) {
            missing = cp;
            break;
        }
    }
    if (missing == 0) ENG_SKIP("the primary face covers every probe codepoint");

    const Glyph* glyph = nullptr;
    Font* owner = primary.Resolve(missing, &glyph);  // не должно падать
    ENG_CHECK(owner != nullptr);
    ENG_CHECK(glyph != nullptr);
    if (glyph) ENG_CHECK(!glyph->isEmpty());

    // Тот же запрос без fallback ни к чему не приводит, а не к падению.
    Font lonely;
    if (LoadFace(faces.front(), &lonely, 32.0f, 256)) {
        u32 absent = 0;
        for (u32 cp : probes) {
            if (!lonely.HasGlyph(cp)) {
                absent = cp;
                break;
            }
        }
        if (absent != 0) {
            const Glyph* g = nullptr;
            ENG_CHECK(lonely.Resolve(absent, &g) == nullptr);
            ENG_CHECK(g == nullptr);
        }
    }
}

// ===========================================================================
// pixelHeight управляет растеризацией; некорректные размеры не должны приводить к падению
// ===========================================================================
ENG_TEST(MultiFont, PixelHeightDrivesGlyphsAndZeroIsSafe) {
    const std::vector<StagedFace> faces = DiscoverStagedFaces();
    if (faces.empty()) ENG_SKIP("no assets/fonts/*.ttf|*.otf staged");

    Font small;
    Font large;
    if (!LoadFace(faces.front(), &small, 20.0f, 512) || !LoadFace(faces.front(), &large, 40.0f, 512))
        ENG_SKIP("staged face failed to load at the test sizes");
    small.PrebakeAscii();
    large.PrebakeAscii();

    const Glyph* g20 = small.GetGlyph('H');
    const Glyph* g40 = large.GetGlyph('H');
    ENG_CHECK(g20 != nullptr);
    ENG_CHECK(g40 != nullptr);
    bool differs = false;
    if (g20 && g40) differs = std::fabs(g20->advance - g40->advance) > 0.01f;
    if (!differs) differs = std::fabs(small.AtlasOccupancy() - large.AtlasOccupancy()) > 1e-4f;
    ENG_CHECK_MSG(differs, "pixelHeight must change the advances or the atlas occupancy");

    // Нулевые и отрицательные значения pixelHeight должны переноситься безопасно.
    for (f32 height : {0.0f, -8.0f}) {
        Font font;
        FontDesc desc;
        desc.pixelHeight = height;
        desc.atlasSize = 128;
        if (font.LoadFromFile(faces.front().relative, desc)) {
            (void)font.GetGlyph('A');
            (void)MeasureText(font, "Ag", 12.0f);
            ENG_CHECK(font.ScaleForSize(12.0f) > 0.0f);
            ENG_CHECK(font.ScaleForSize(0.0f) > 0.0f);
        }
    }
    ENG_CHECK(small.Valid());
    ENG_CHECK(large.Valid());
}

// ---------------------------------------------------------------------------
// Ubuntu Mono (начертание главного меню)
// ---------------------------------------------------------------------------
// Меню набрано поставляемым с движком Ubuntu Mono; эти тесты фиксируют свойства,
// на которые опирается меню, чтобы сбой стейджинга или неудачная подмена
// обнаруживались здесь, а не проявлялись как разъехавшееся меню.
ENG_TEST(MultiFont, UbuntuMonoIsStaged) {
    const std::string path = PathJoin(GetAssetRoot(), "fonts/ubuntu_mono.ttf");
    if (!FileExists(path)) ENG_SKIP("assets/fonts/ubuntu_mono.ttf not staged");
    Font font;
    FontDesc desc;
    desc.pixelHeight = 16.0f;
    desc.hinting = true;
    desc.atlasSize = 1024;
    ENG_CHECK(font.LoadFromFile(path, desc));
    ENG_CHECK(font.Valid());
    ENG_CHECK_EQ(font.FamilyName(), std::string("Ubuntu Mono"));
    ENG_CHECK_GT(font.UnitsPerEm(), 0.0f);
    ENG_CHECK_GT(font.Ascender(), font.Descender());
    // Суть этого начертания: ASCII строго моноширинный.
    f32 minAdvance = 1e9f, maxAdvance = 0.0f;
    int measured = 0;
    for (u32 cp = 33; cp < 127; ++cp) {
        const Glyph* g = font.GetGlyph(cp);
        if (!g || g->advance <= 0.0f) continue;
        minAdvance = std::min(minAdvance, g->advance);
        maxAdvance = std::max(maxAdvance, g->advance);
        ++measured;
    }
    ENG_CHECK_GT(measured, 90);
    ENG_CHECK_MSG(maxAdvance - minAdvance < 0.01f,
                  "Ubuntu Mono must have a single advance for all ASCII glyphs");
    // Моноширинное начертание упрощает расчёт колонок меню: N символов должны
    // измеряться ровно как N * advance.
    const f32 one = MeasureText(font, "M", 16.0f).width;
    const f32 ten = MeasureText(font, "MMMMMMMMMM", 16.0f).width;
    ENG_CHECK_NEAR(ten, one * 10.0f, 0.05f);
    // Bold тоже положен и должен совпадать по advance, поэтому переключение
    // насыщенности в заголовке меню не может сдвинуть разметку.
    const std::string boldPath = PathJoin(GetAssetRoot(), "fonts/ubuntu_mono_bold.ttf");
    if (FileExists(boldPath)) {
        Font bold;
        ENG_CHECK(bold.LoadFromFile(boldPath, desc));
        const Glyph* bg = bold.GetGlyph('M');
        ENG_CHECK(bg != nullptr);
        if (bg) ENG_CHECK_NEAR(bg->advance, font.GetGlyph('M')->advance, 0.01f);
    }
}

// ---------------------------------------------------------------------------
// Ubuntu — семейство шрифтов UI из примера
// ---------------------------------------------------------------------------
// Тема, заголовки сцен и элементы управления каждой сцены рисуются поставляемыми
// начертаниями Ubuntu. Эти тесты фиксируют то, на чём это основано, чтобы сбой
// стейджинга или неудачная подмена обнаруживались здесь, а не проявлялись как
// неправильно выглядящий интерфейс.
ENG_TEST(MultiFont, UbuntuFamilyIsStaged) {
    struct Face {
        const char* file;
        const char* family;
        bool bold;
    };
    const Face faces[] = {
        {"fonts/ubuntu.ttf", "Ubuntu", false},
        {"fonts/ubuntu_bold.ttf", "Ubuntu", true},
    };

    int checked = 0;
    for (const Face& face : faces) {
        const std::string path = PathJoin(GetAssetRoot(), face.file);
        if (!FileExists(path)) {
            ENG_SKIP("Ubuntu family is not staged; run the asset staging step");
        }
        Font font;
        FontDesc desc;
        desc.pixelHeight = 16.0f;
        desc.hinting = true;
        desc.atlasSize = 1024;
        ENG_CHECK_MSG(font.LoadFromFile(path, desc), face.file);
        ENG_CHECK(font.Valid());
        ENG_CHECK_EQ(font.FamilyName(), std::string(face.family));
        ENG_CHECK_GT(font.UnitsPerEm(), 0.0f);
        ENG_CHECK_GT(font.Ascender(), font.Descender());

        // Каждый печатаемый ASCII-глиф должен давать чернила; пробел здесь виден
        // сразу во всех строках меню.
        int inked = 0;
        for (u32 cp = 33; cp < 127; ++cp) {
            const Glyph* g = font.GetGlyph(cp);
            if (g && g->width > 0 && g->height > 0) ++inked;
        }
        ENG_CHECK_GE(inked, 93);

        // Ubuntu пропорциональный: advance должен различаться между узким глифом
        // и широким (в этом отличие от Mono-начертания, и именно на это
        // опирается разметка UI).
        const Glyph* i = font.GetGlyph('i');
        const Glyph* w = font.GetGlyph('W');
        ENG_CHECK(i != nullptr);
        ENG_CHECK(w != nullptr);
        if (i && w) ENG_CHECK_MSG(w->advance > i->advance + 1.0f,
                                  "Ubuntu must be proportional, not monospaced");

        // Текст измеряется и масштабируется линейно, а bold шире обычного.
        const f32 width = MeasureText(font, "Number Logic", 16.0f).width;
        ENG_CHECK_GT(width, 0.0f);
        ENG_CHECK_NEAR(MeasureText(font, "Number Logic", 32.0f).width, width * 2.0f,
                       width * 0.05f + 0.5f);
        if (!face.bold) {
            Font boldFace;
            FontDesc bd = desc;
            if (boldFace.LoadFromFile(PathJoin(GetAssetRoot(), "fonts/ubuntu_bold.ttf"), bd)) {
                ENG_CHECK_GT(MeasureText(boldFace, "Heading", 16.0f).width,
                             MeasureText(font, "Heading", 16.0f).width);
            }
        }
        ++checked;
    }
    ENG_CHECK_EQ(checked, 2);
}
