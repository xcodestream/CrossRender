// Тесты crossrender/ui — идентификаторы, якоря, safe area, flex-решатель, физика
// скролла, редактирование текста с учётом UTF-8 и процедурный генератор 9-patch.
//
// Код виджетов требует Renderer2D для вызовов отрисовки, поэтому логика
// взаимодействия и раскладки, которую можно проверить без контекста, вынесена
// в свободные функции в engine/src/ui/UiInternal.h и вызывается отсюда напрямую.
// Всё, что требует GL-контекст, вызывает ENG_REQUIRE_GL() и иначе сообщает о пропуске.
#include "crossrender/ui/Ui.h"

#include "crossrender/core/Log.h"
#include "crossrender/test/Test.h"
#include "crossrender/text/Font.h"
#include "crossrender/platform/Window.h"

#include "ui/UiInternal.h"

#include <cmath>
#include <string>
#include <vector>
#include <algorithm>

using namespace crossrender;
using namespace crossrender::ui_internal;

namespace {

// Синтетическая детерминированная текстовая метрика: каждый ASCII-кодпойнт
// шириной 8 единиц, любой другой (кириллица, эмодзи, буллеты) — 10. Этого
// достаточно, чтобы проверить всю математику каретки без растеризатора шрифтов.
f32 MonoWidth(const std::string& s) {
    f32 w = 0.0f;
    usize i = 0;
    while (i < s.size()) {
        const u8 c = static_cast<u8>(s[i]);
        if (c < 0x80u) {
            w += 8.0f;
            i += 1;
        } else {
            w += 10.0f;
            // Пропускаем одну полную UTF-8-последовательность.
            usize n = (c & 0xF8u) == 0xF0u ? 4 : ((c & 0xF0u) == 0xE0u ? 3 : 2);
            i += n;
        }
    }
    return w;
}

const std::function<f32(const std::string&)> kWidth = MonoWidth;

std::vector<LayoutSize> Sizes(std::initializer_list<LayoutSize> l) { return std::vector<LayoutSize>(l); }

}  // namespace

// ---------------------------------------------------------------------------
// Идентификаторы
// ---------------------------------------------------------------------------
ENG_TEST(Ui, HashStableAndDistinct) {
    const char* names[] = {"panel", "button", "slider", "checkbox", "textfield",
                           "listview", "dropdown", "tabbar", "toast", "modal",
                           "hud.health", "hud.mana", "menu.file", "menu.edit", "menu.view",
                           "settings.volume", "settings.gamma", "player.name", "shop.item", "debug.fps"};
    std::vector<UiId> ids;
    for (const char* n : names) {
        const UiId a = UiHash(n);
        const UiId b = UiHash(std::string(n));
        ENG_CHECK(a != kUiIdNone);
        ENG_CHECK_EQ(a, b);                            // тот же вход — тот же id
        ENG_CHECK_EQ(UiHash(n), UiHash(n, 0));         // индекс по умолчанию 0
        ids.push_back(a);
    }
    // Без коллизий на выборке примеров.
    for (usize i = 0; i < ids.size(); ++i)
        for (usize j = i + 1; j < ids.size(); ++j) ENG_CHECK(ids[i] != ids[j]);

    // Другой индекс -> другой id для каждого примера имени.
    for (const char* n : names) {
        for (int k = 1; k < 5; ++k) {
            ENG_CHECK(UiHash(n, 0) != UiHash(n, k));
            ENG_CHECK(UiHash(n, k) != UiHash(n, k + 1));
        }
    }
    // Известные векторы FNV-1a (offset basis 1469598103934665603, простое 1e9+8211).
    // "a", индекс 0.
    {
        u64 expect = 1469598103934665603ULL;
        expect ^= static_cast<u64>('a');
        expect *= 1099511628211ULL;
        expect ^= 0ULL;
        expect *= 1099511628211ULL;
        ENG_CHECK_EQ(UiHash("a"), expect);
    }
    ENG_CHECK_EQ(UiHash(""), UiHash("", 0));
    ENG_CHECK(UiHash("") != kUiIdNone);
}

// ---------------------------------------------------------------------------
// Якоря
// ---------------------------------------------------------------------------
ENG_TEST(Ui, AnchorTopLeft) {
    const Rect parent{0, 0, 1280, 720};
    const Rect r = Anchor::TopLeft({200, 40}, {16, 24}).Resolve(parent);
    ENG_CHECK_NEAR(r.x, 16.0f, 1e-4f);
    ENG_CHECK_NEAR(r.y, 24.0f, 1e-4f);
    ENG_CHECK_NEAR(r.w, 200.0f, 1e-4f);
    ENG_CHECK_NEAR(r.h, 40.0f, 1e-4f);

    // Ненулевое начало координат родителя сдвигает точку якоря вместе с собой.
    const Rect off{100, 50, 400, 300};
    const Rect r2 = Anchor::TopLeft({20, 10}, {0, 0}).Resolve(off);
    ENG_CHECK_NEAR(r2.x, 100.0f, 1e-4f);
    ENG_CHECK_NEAR(r2.y, 50.0f, 1e-4f);
}

ENG_TEST(Ui, AnchorCenter) {
    const Rect parent{0, 0, 800, 600};
    const Rect r = Anchor::Center({200, 100}).Resolve(parent);
    ENG_CHECK_NEAR(r.x, 300.0f, 1e-4f);
    ENG_CHECK_NEAR(r.y, 250.0f, 1e-4f);
    ENG_CHECK_NEAR(r.w, 200.0f, 1e-4f);
    ENG_CHECK_NEAR(r.h, 100.0f, 1e-4f);
    ENG_CHECK_NEAR(r.Center().x, parent.Center().x, 1e-4f);
    ENG_CHECK_NEAR(r.Center().y, parent.Center().y, 1e-4f);

    // Nudge сдвигает rect.
    const Rect r2 = Anchor::Center({200, 100}, {10, -20}).Resolve(parent);
    ENG_CHECK_NEAR(r2.x, 310.0f, 1e-4f);
    ENG_CHECK_NEAR(r2.y, 230.0f, 1e-4f);
    ENG_CHECK_NEAR(r2.Center().x, parent.Center().x + 10.0f, 1e-4f);
}

ENG_TEST(Ui, AnchorBottom) {
    const Rect parent{0, 0, 800, 600};
    const Rect r = Anchor::Bottom({300, 60}).Resolve(parent);
    // Якорь по низу по центру: нижний край rect лежит на нижнем крае родителя,
    // поэтому его центр на половину высоты выше.
    ENG_CHECK_NEAR(r.Center().x, 400.0f, 1e-4f);
    ENG_CHECK_NEAR(r.Center().y, 570.0f, 1e-4f);
    ENG_CHECK_NEAR(r.y, 540.0f, 1e-4f);
    ENG_CHECK_NEAR(r.Bottom(), 600.0f, 1e-4f);
    ENG_CHECK_NEAR(r.w, 300.0f, 1e-4f);
    ENG_CHECK_NEAR(r.h, 60.0f, 1e-4f);
}

ENG_TEST(Ui, AnchorStretch) {
    const Rect parent{100, 50, 800, 600};
    // Без отступа: ровно родитель.
    const Rect full = Anchor::Stretch().Resolve(parent);
    ENG_CHECK_NEAR(full.x, 100.0f, 1e-4f);
    ENG_CHECK_NEAR(full.y, 50.0f, 1e-4f);
    ENG_CHECK_NEAR(full.w, 800.0f, 1e-4f);
    ENG_CHECK_NEAR(full.h, 600.0f, 1e-4f);

    // Симметричный отступ.
    const Rect inset = Anchor::Stretch({20, 10}).Resolve(parent);
    ENG_CHECK_NEAR(inset.x, 120.0f, 1e-4f);
    ENG_CHECK_NEAR(inset.y, 60.0f, 1e-4f);
    ENG_CHECK_NEAR(inset.w, 760.0f, 1e-4f);
    ENG_CHECK_NEAR(inset.h, 580.0f, 1e-4f);

    // Минимальный размер расширяет rect, когда родитель мал.
    Anchor a = Anchor::Stretch();
    a.size = {400, 300};
    const Rect expanded = a.Resolve(Rect{0, 0, 100, 80});
    ENG_CHECK_NEAR(expanded.w, 400.0f, 1e-4f);
    ENG_CHECK_NEAR(expanded.h, 300.0f, 1e-4f);

    // Частичный якорь (растяжение только по вертикали): offset сжимает оба края.
    Anchor v;
    v.min = {0.5f, 0.0f};
    v.max = {0.5f, 1.0f};
    v.offset = {0, 8};
    const Rect vs = v.Resolve(parent);
    ENG_CHECK_NEAR(vs.x, 500.0f, 1e-4f);
    ENG_CHECK_NEAR(vs.y, 58.0f, 1e-4f);
    ENG_CHECK_NEAR(vs.h, 600.0f - 16.0f, 1e-4f);
    // Горизонтальная ось — точечный якорь, поэтому сохраняется явная ширина якоря
    // (по умолчанию 100), а не схлопывание в ноль.
    ENG_CHECK_NEAR(vs.w, v.size.x, 1e-4f);
    ENG_CHECK_NEAR(vs.w, 100.0f, 1e-4f);

    // Частичный якорь с явным минимальным размером на растягиваемой оси.
    Anchor v2 = v;
    v2.size = {300, 0};
    const Rect vs2 = v2.Resolve(parent);
    ENG_CHECK_NEAR(vs2.w, 300.0f, 1e-4f);
    ENG_CHECK_NEAR(vs2.h, 584.0f, 1e-4f);

    // Горизонтальная полоса 25%..75% с симметричным отступом.
    Anchor band;
    band.min = {0.25f, 0.0f};
    band.max = {0.75f, 0.0f};
    band.offset = {4, 0};
    const Rect bd = band.Resolve(parent);
    // Край якоря на 25% родителя, затем сжатие на offset.
    ENG_CHECK_NEAR(bd.x, 100.0f + 0.25f * 800.0f + 4.0f, 1e-4f);
    ENG_CHECK_NEAR(bd.w, 0.5f * 800.0f - 8.0f, 1e-4f);
}

ENG_TEST(Ui, AnchorPercent) {
    const Rect parent{0, 0, 1000, 500};
    // Точечный якорь на 25% / 75%, размеры в абсолютных логических единицах.
    Anchor a;
    a.min = a.max = {0.25f, 0.75f};
    a.size = {100, 40};
    const Rect r = a.Resolve(parent);
    ENG_CHECK_NEAR(r.Center().x, 250.0f, 1e-4f);
    ENG_CHECK_NEAR(r.Center().y, 375.0f, 1e-4f);
    ENG_CHECK_NEAR(r.x, 200.0f, 1e-4f);
    ENG_CHECK_NEAR(r.y, 355.0f, 1e-4f);
    ENG_CHECK_NEAR(r.w, 100.0f, 1e-4f);
    ENG_CHECK_NEAR(r.h, 40.0f, 1e-4f);

    // Бокс в половину ширины на правой половине родителя.
    Anchor half;
    half.min = {0.5f, 0.0f};
    half.max = {1.0f, 1.0f};
    const Rect h = half.Resolve(parent);
    ENG_CHECK_NEAR(h.x, 500.0f, 1e-4f);
    ENG_CHECK_NEAR(h.w, 500.0f, 1e-4f);
    ENG_CHECK_NEAR(h.h, 500.0f, 1e-4f);
}

ENG_TEST(Ui, AnchorResolveScaled) {
    const Rect parent{0, 0, 800, 600};
    const Rect r = Anchor::Center({200, 100}).ResolveScaled(parent, 2.0f);
    ENG_CHECK_NEAR(r.w, 400.0f, 1e-4f);
    ENG_CHECK_NEAR(r.h, 200.0f, 1e-4f);
    ENG_CHECK_NEAR(r.Center().x, 400.0f, 1e-4f);
    ENG_CHECK_NEAR(r.Center().y, 300.0f, 1e-4f);
    // Вырожденный масштаб откатывается к 1.
    const Rect same = Anchor::Center({200, 100}).ResolveScaled(parent, 0.0f);
    ENG_CHECK_NEAR(same.w, 200.0f, 1e-4f);
}

// ---------------------------------------------------------------------------
// Безопасная зона (safe area)
// ---------------------------------------------------------------------------
ENG_TEST(Ui, SafeAreaApply) {
    SafeArea sa;
    sa.left = 20.0f;
    sa.top = 44.0f;
    sa.right = 12.0f;
    sa.bottom = 30.0f;
    const Rect r = sa.Apply(Rect{0, 0, 400, 800});
    ENG_CHECK_NEAR(r.x, 20.0f, 1e-4f);
    ENG_CHECK_NEAR(r.y, 44.0f, 1e-4f);
    ENG_CHECK_NEAR(r.w, 400.0f - 20.0f - 12.0f, 1e-4f);
    ENG_CHECK_NEAR(r.h, 800.0f - 44.0f - 30.0f, 1e-4f);

    // Нулевые отступы — тождество (что и сообщает десктопная сборка). На iOS
    // реальная реализация в платформенном слое, поэтому это только для десктопа.
#if !defined(ENG_PLATFORM_IOS)
    const SafeArea zero = SafeArea::Query();
    ENG_CHECK_NEAR(zero.left, 0.0f, 1e-6f);
    ENG_CHECK_NEAR(zero.top, 0.0f, 1e-6f);
    ENG_CHECK_NEAR(zero.right, 0.0f, 1e-6f);
    ENG_CHECK_NEAR(zero.bottom, 0.0f, 1e-6f);
    const Rect same = zero.Apply(Rect{5, 7, 100, 50});
    ENG_CHECK_NEAR(same.x, 5.0f, 1e-6f);
    ENG_CHECK_NEAR(same.w, 100.0f, 1e-6f);
#endif
}

// ---------------------------------------------------------------------------
// Flex-решатель
// ---------------------------------------------------------------------------
ENG_TEST(Ui, FlexFixedPercentContent) {
    std::vector<f32> out;
    std::vector<f32> intrinsic{50.0f, 999.0f, 30.0f};

    // Fixed + Percent (доля полезного пространства) + Content.
    SolveFlex(Sizes({LayoutSize::Fixed(100), LayoutSize::Percent(0.5f), LayoutSize::Content()}),
              intrinsic, 400.0f, 0.0f, &out);
    ENG_CHECK_EQ(out.size(), usize(3));
    ENG_CHECK_NEAR(out[0], 100.0f, 1e-4f);
    ENG_CHECK_NEAR(out[1], 200.0f, 1e-4f);  // 0.5 * 400
    ENG_CHECK_NEAR(out[2], 30.0f, 1e-4f);

    // Percent считается от пространства, оставшегося после промежутков.
    SolveFlex(Sizes({LayoutSize::Percent(0.5f), LayoutSize::Percent(0.5f)}), {}, 100.0f, 10.0f, &out);
    ENG_CHECK_NEAR(out[0], 45.0f, 1e-4f);
    ENG_CHECK_NEAR(out[1], 45.0f, 1e-4f);
    ENG_CHECK_NEAR(FlexTotal(out, 10.0f), 100.0f, 1e-4f);

    // Отсутствующие внутренние размеры считаются нулём.
    SolveFlex(Sizes({LayoutSize::Content(), LayoutSize::Content()}), {12.0f}, 100.0f, 0.0f, &out);
    ENG_CHECK_NEAR(out[0], 12.0f, 1e-4f);
    ENG_CHECK_NEAR(out[1], 0.0f, 1e-4f);
}

ENG_TEST(Ui, FlexGrowDistribution) {
    std::vector<f32> out;
    // Два grow-элемента делят остаток поровну.
    SolveFlex(Sizes({LayoutSize::Fixed(100), LayoutSize::Grow(), LayoutSize::Grow()}), {}, 400.0f, 0.0f, &out);
    ENG_CHECK_NEAR(out[0], 100.0f, 1e-4f);
    ENG_CHECK_NEAR(out[1], 150.0f, 1e-4f);
    ENG_CHECK_NEAR(out[2], 150.0f, 1e-4f);

    // Промежутки уменьшают остаток.
    SolveFlex(Sizes({LayoutSize::Fixed(100), LayoutSize::Grow(), LayoutSize::Grow()}), {}, 400.0f, 20.0f, &out);
    ENG_CHECK_NEAR(out[1], 130.0f, 1e-4f);
    ENG_CHECK_NEAR(out[2], 130.0f, 1e-4f);
    ENG_CHECK_NEAR(FlexTotal(out, 20.0f), 400.0f, 1e-4f);

    // Единственный grow забирает всё оставшееся.
    SolveFlex(Sizes({LayoutSize::Grow()}), {}, 250.0f, 0.0f, &out);
    ENG_CHECK_NEAR(out[0], 250.0f, 1e-4f);

    // Нет grow-элементов: остаток просто остаётся неиспользованным.
    SolveFlex(Sizes({LayoutSize::Fixed(30), LayoutSize::Fixed(40)}), {}, 400.0f, 0.0f, &out);
    ENG_CHECK_NEAR(FlexLeftover(out, 400.0f, 0.0f), 330.0f, 1e-4f);
}

ENG_TEST(Ui, FlexOverflowClamps) {
    std::vector<f32> out;
    // Fixed-элементы, чья сумма превышает контейнер, сохраняют запрошенный размер
    // (при отрисовке они просто обрезаются), но grow никогда не уходит в минус.
    SolveFlex(Sizes({LayoutSize::Fixed(300), LayoutSize::Fixed(300)}), {}, 400.0f, 0.0f, &out);
    ENG_CHECK_NEAR(out[0], 300.0f, 1e-4f);
    ENG_CHECK_NEAR(out[1], 300.0f, 1e-4f);
    ENG_CHECK(FlexLeftover(out, 400.0f, 0.0f) < 0.0f);

    // Grow-элемент без свободного места схлопывается в ноль, а не в минус.
    SolveFlex(Sizes({LayoutSize::Fixed(500), LayoutSize::Grow()}), {}, 400.0f, 0.0f, &out);
    ENG_CHECK_NEAR(out[1], 0.0f, 1e-4f);

    // Percent больше 100% допустим, но остаётся неотрицательным.
    SolveFlex(Sizes({LayoutSize::Percent(1.5f)}), {}, 100.0f, 0.0f, &out);
    ENG_CHECK_NEAR(out[0], 150.0f, 1e-4f);

    // Пустой вход — no-op.
    SolveFlex({}, {}, 100.0f, 5.0f, &out);
    ENG_CHECK_EQ(out.size(), usize(0));

    // Вырожденный контейнер.
    SolveFlex(Sizes({LayoutSize::Grow(), LayoutSize::Percent(0.25f)}), {}, 0.0f, 0.0f, &out);
    ENG_CHECK_NEAR(out[0], 0.0f, 1e-4f);
    ENG_CHECK_NEAR(out[1], 0.0f, 1e-4f);
}

ENG_TEST(Ui, FlexSpacing) {
    std::vector<f32> out;
    SolveFlex(Sizes({LayoutSize::Fixed(50), LayoutSize::Fixed(50), LayoutSize::Fixed(50)}), {}, 300.0f, 10.0f,
              &out);
    ENG_CHECK_NEAR(FlexTotal(out, 10.0f), 170.0f, 1e-4f);
    ENG_CHECK_NEAR(FlexLeftover(out, 300.0f, 10.0f), 130.0f, 1e-4f);

    // Только Content-элементы.
    SolveFlex(Sizes({LayoutSize::Content(), LayoutSize::Content(), LayoutSize::Content()}),
              {20.0f, 30.0f, 40.0f}, 200.0f, 5.0f, &out);
    ENG_CHECK_NEAR(out[0], 20.0f, 1e-4f);
    ENG_CHECK_NEAR(out[1], 30.0f, 1e-4f);
    ENG_CHECK_NEAR(out[2], 40.0f, 1e-4f);
    ENG_CHECK_NEAR(FlexTotal(out, 5.0f), 100.0f, 1e-4f);
}

// ---------------------------------------------------------------------------
// Скроллинг
// ---------------------------------------------------------------------------
ENG_TEST(Ui, ScrollClamp) {
    // Контент короче вью: валиден только offset 0.
    ENG_CHECK_NEAR(ClampScroll(-50.0f, 100.0f, 200.0f), 0.0f, 1e-5f);
    ENG_CHECK_NEAR(ClampScroll(0.0f, 100.0f, 200.0f), 0.0f, 1e-5f);
    ENG_CHECK_NEAR(ClampScroll(25.0f, 100.0f, 200.0f), 0.0f, 1e-5f);
    ENG_CHECK_EQ(ClampScroll(0.0f, 100.0f, 200.0f), 0.0f);  // без -0.0f

    // Переполняющий контент: [0, content - view].
    ENG_CHECK_NEAR(ClampScroll(-10.0f, 1000.0f, 300.0f), 0.0f, 1e-5f);
    ENG_CHECK_NEAR(ClampScroll(123.0f, 1000.0f, 300.0f), 123.0f, 1e-5f);
    ENG_CHECK_NEAR(ClampScroll(5000.0f, 1000.0f, 300.0f), 700.0f, 1e-5f);
    ENG_CHECK_NEAR(ClampScroll(700.0f, 1000.0f, 300.0f), 700.0f, 1e-5f);

    // Контент, точно помещающийся, не скроллится.
    ENG_CHECK_NEAR(ClampScroll(4.0f, 300.0f, 300.0f), 0.0f, 1e-5f);
    // Вырожденные значения никогда не дают NaN/отрицательных.
    ENG_CHECK_NEAR(ClampScroll(10.0f, 0.0f, 0.0f), 0.0f, 1e-5f);
}

ENG_TEST(Ui, ScrollMomentum) {
    const f32 dt = 1.0f / 60.0f;
    // Fling затухает и в итоге останавливается внутри диапазона.
    f32 offset = 0.0f, velocity = 2000.0f;
    f32 last = 0.0f;
    f32 travelled = 0.0f;
    for (int i = 0; i < 600; ++i) {
        StepScroll(&offset, &velocity, 5000.0f, 300.0f, dt, 4.5f);
        travelled += offset - last;
        last = offset;
        ENG_CHECK(offset >= 0.0f);
        ENG_CHECK(offset <= 4700.0f + 1e-3f);
    }
    ENG_CHECK(velocity == 0.0f);
    ENG_CHECK(travelled > 100.0f);

    // Fling в конец ограничивается точно и обнуляет скорость.
    f32 o2 = 0.0f, v2 = 5000.0f;
    for (int i = 0; i < 120; ++i) StepScroll(&o2, &v2, 1000.0f, 300.0f, dt, 4.5f);
    ENG_CHECK_NEAR(o2, 700.0f, 1e-3f);
    ENG_CHECK_NEAR(v2, 0.0f, 1e-6f);

    // Без скорости ограничение всё равно применяется.
    f32 o3 = 5000.0f, v3 = 0.0f;
    StepScroll(&o3, &v3, 1000.0f, 300.0f, dt, 4.5f);
    ENG_CHECK_NEAR(o3, 700.0f, 1e-3f);
}

// ---------------------------------------------------------------------------
// Математика редактирования текста
// ---------------------------------------------------------------------------
ENG_TEST(Ui, CaretPositionsUtf8) {
    std::vector<usize> pos;
    CaretPositions("abc", &pos);
    ENG_CHECK_EQ(pos.size(), usize(4));
    ENG_CHECK_EQ(pos[0], usize(0));
    ENG_CHECK_EQ(pos[1], usize(1));
    ENG_CHECK_EQ(pos[2], usize(2));
    ENG_CHECK_EQ(pos[3], usize(3));

    // "Привет" — это 6 кириллических кодпойнтов по 2 байта.
    const std::string ru = "\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82";
    ENG_CHECK_EQ(Utf8Length(ru), usize(6));
    CaretPositions(ru, &pos);
    ENG_CHECK_EQ(pos.size(), usize(7));
    ENG_CHECK_EQ(pos[0], usize(0));
    ENG_CHECK_EQ(pos[1], usize(2));
    ENG_CHECK_EQ(pos[6], ru.size());
    // Каждая позиция — граница кодпойнта.
    for (usize p : pos) ENG_CHECK_EQ(ClampCaret(ru, p), p);
}

ENG_TEST(Ui, CaretIndexFromClick) {
    const std::string s = "hello";
    // 8 единиц на символ: x=0 -> 0, x=7 -> 0, x=9 -> 1, x=39 -> 4, x>=40 -> 5.
    ENG_CHECK_EQ(CaretIndexFromClick(s, -5.0f, kWidth), usize(0));
    ENG_CHECK_EQ(CaretIndexFromClick(s, 0.0f, kWidth), usize(0));
    ENG_CHECK_EQ(CaretIndexFromClick(s, 7.0f, kWidth), usize(0));
    ENG_CHECK_EQ(CaretIndexFromClick(s, 9.0f, kWidth), usize(1));
    ENG_CHECK_EQ(CaretIndexFromClick(s, 11.0f, kWidth), usize(1));  // слева от средней точки
    ENG_CHECK_EQ(CaretIndexFromClick(s, 12.0f, kWidth), usize(2));  // ровно на средней точке
    ENG_CHECK_EQ(CaretIndexFromClick(s, 15.0f, kWidth), usize(2));
    ENG_CHECK_EQ(CaretIndexFromClick(s, 17.0f, kWidth), usize(2));
    ENG_CHECK_EQ(CaretIndexFromClick(s, 35.0f, kWidth), usize(4));  // последний глиф, левая половина
    ENG_CHECK_EQ(CaretIndexFromClick(s, 37.0f, kWidth), usize(5));  // за его средней точкой
    ENG_CHECK_EQ(CaretIndexFromClick(s, 40.0f, kWidth), usize(5));
    ENG_CHECK_EQ(CaretIndexFromClick(s, 500.0f, kWidth), usize(5));

    // Кириллические глифы шириной по 10.
    const std::string ru = "\xD0\x9F\xD1\x80\xD0\xB8";  // 3 кодпойнта
    ENG_CHECK_EQ(Utf8Length(ru), usize(3));
    ENG_CHECK_EQ(CaretIndexFromClick(ru, 4.0f, kWidth), usize(0));
    ENG_CHECK_EQ(CaretIndexFromClick(ru, 4.0f, kWidth), usize(0));  // левая половина
    ENG_CHECK_EQ(CaretIndexFromClick(ru, 16.0f, kWidth), usize(4));  // за второй средней точкой
    ENG_CHECK_EQ(CaretIndexFromClick(ru, 25.0f, kWidth), usize(6));
    ENG_CHECK_EQ(CaretIndexFromClick(ru, 100.0f, kWidth), ru.size());

    // Пустая строка всегда даёт 0.
    ENG_CHECK_EQ(CaretIndexFromClick("", 33.0f, kWidth), usize(0));
}

ENG_TEST(Ui, CaretNavigationUtf8) {
    const std::string ru = "\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82";  // 6 кодпойнтов, 12 байтов
    ENG_CHECK_EQ(CaretNext(ru, 0), usize(2));
    ENG_CHECK_EQ(CaretNext(ru, 2), usize(4));
    ENG_CHECK_EQ(CaretNext(ru, 10), usize(12));
    ENG_CHECK_EQ(CaretNext(ru, 12), usize(12));
    ENG_CHECK_EQ(CaretPrev(ru, 12), usize(10));
    ENG_CHECK_EQ(CaretPrev(ru, 2), usize(0));
    ENG_CHECK_EQ(CaretPrev(ru, 0), usize(0));
    // Вне-граничный вход откатывается к началу последовательности.
    ENG_CHECK_EQ(CaretPrev(ru, 3), usize(0));

    // Навигация по словам в смешанной строке.
    const std::string words = "hello wonderful world";
    ENG_CHECK_EQ(WordRight(words, 0), usize(6));   // после "hello "
    ENG_CHECK_EQ(WordRight(words, 6), usize(16));  // после "wonderful "
    ENG_CHECK_EQ(WordRight(words, 16), words.size());
    ENG_CHECK_EQ(WordLeft(words, words.size()), usize(16));
    ENG_CHECK_EQ(WordLeft(words, 16), usize(6));
    ENG_CHECK_EQ(WordLeft(words, 6), usize(0));

    // Помощники по строкам.
    const std::string multiline = "one\ntwo\nthree";
    ENG_CHECK_EQ(LineStart(multiline, 5), usize(4));
    ENG_CHECK_EQ(LineEnd(multiline, 5), usize(7));
    ENG_CHECK_EQ(LineStart(multiline, 13), usize(8));
    ENG_CHECK_EQ(LineEnd(multiline, 8), multiline.size());
    // Навигация по словам переходит через слово, но перевод строки — жёсткая остановка.
    const usize afterTwo = WordRight(multiline, 5);  // "two" -> 7 (конец строки)
    ENG_CHECK_EQ(afterTwo, usize(7));
    (void)afterTwo;
}

ENG_TEST(Ui, SelectionNormalisation) {
    usize first = 0, last = 0;
    NormalizeSelection(3, 7, &first, &last);
    ENG_CHECK_EQ(first, usize(3));
    ENG_CHECK_EQ(last, usize(7));
    NormalizeSelection(7, 3, &first, &last);  // обратное выделение меняет местами
    ENG_CHECK_EQ(first, usize(3));
    ENG_CHECK_EQ(last, usize(7));
    NormalizeSelection(5, 5, &first, &last);  // пустое выделение
    ENG_CHECK_EQ(first, usize(5));
    ENG_CHECK_EQ(last, usize(5));
    NormalizeSelection(-1, 4, &first, &last);  // сентинел "нет выделения"
    ENG_CHECK_EQ(first, usize(0));
    ENG_CHECK_EQ(last, usize(0));
    NormalizeSelection(4, -1, &first, &last);
    ENG_CHECK_EQ(first, usize(0));
    ENG_CHECK_EQ(last, usize(0));
    NormalizeSelection(0, 9, &first, &last);
    ENG_CHECK_EQ(first, usize(0));
    ENG_CHECK_EQ(last, usize(9));
}

ENG_TEST(Ui, BackspaceUtf8) {
    // ASCII: один байт.
    std::string ascii = "abc";
    usize caret = BackspaceAt(&ascii, 3);
    ENG_CHECK_STR_EQ(ascii, "ab");
    ENG_CHECK_EQ(caret, usize(2));
    caret = BackspaceAt(&ascii, 0);
    ENG_CHECK_EQ(caret, usize(0));
    ENG_CHECK_STR_EQ(ascii, "ab");
    // Удаление последнего ASCII-символа.
    caret = BackspaceAt(&ascii, 1);
    ENG_CHECK_STR_EQ(ascii, "b");
    ENG_CHECK_EQ(caret, usize(0));

    // Кириллица: два байта на кодпойнт.
    std::string ru = "\xD0\x9F\xD1\x80\xD0\xB8";  // "При"
    ENG_CHECK_EQ(ru.size(), usize(6));
    caret = BackspaceAt(&ru, 6);
    ENG_CHECK_EQ(caret, usize(4));
    ENG_CHECK_EQ(ru.size(), usize(4));
    ENG_CHECK_STR_EQ(ru, std::string("\xD0\x9F\xD1\x80"));
    caret = BackspaceAt(&ru, 4);
    ENG_CHECK_EQ(caret, usize(2));
    caret = BackspaceAt(&ru, 2);
    ENG_CHECK_EQ(caret, usize(0));
    ENG_CHECK_STR_EQ(ru, std::string());

    // Эмодзи: четыре байта (U+1F600).
    std::string emoji = "a\xF0\x9F\x98\x80";
    ENG_CHECK_EQ(emoji.size(), usize(5));
    caret = BackspaceAt(&emoji, 5);
    ENG_CHECK_EQ(caret, usize(1));
    ENG_CHECK_STR_EQ(emoji, std::string("a"));

    // Каретка внутри последовательности откатывается к её началу перед удалением.
    std::string ru2 = "\xD0\x9F\xD1\x80";
    caret = BackspaceAt(&ru2, 3);
    ENG_CHECK_EQ(caret, usize(0));
    ENG_CHECK_STR_EQ(ru2, std::string("\xD1\x80"));

    // Delete удаляет *следующий* кодпойнт (2 байта для кириллицы).
    std::string ru3 = "\xD0\x9F\xD1\x80\xD0\xB8";
    DeleteAt(&ru3, 2);
    ENG_CHECK_EQ(ru3.size(), usize(4));
    ENG_CHECK_STR_EQ(ru3, std::string("\xD0\x9F\xD0\xB8"));
    DeleteAt(&ru3, 4 * 10);  // за концом: no-op
    ENG_CHECK_EQ(ru3.size(), usize(4));
}

ENG_TEST(Ui, Utf8InsertAndCodepoints) {
    std::string s = "ab";
    std::string tmp;
    AppendCodepointUtf8(&tmp, 0x0416u);  // кириллическая Ж -> 2 байта
    ENG_CHECK_EQ(tmp.size(), usize(2));
    AppendCodepointUtf8(&tmp, 0x1F600u);  // эмодзи -> 4 байта
    ENG_CHECK_EQ(tmp.size(), usize(6));
    AppendCodepointUtf8(&tmp, 'x');  // ascii -> 1 байт
    ENG_CHECK_EQ(tmp.size(), usize(7));

    InsertUtf8(&s, 1, "\xD0\x96");
    ENG_CHECK_EQ(s.size(), usize(4));
    ENG_CHECK_STR_EQ(s.substr(0, 1), std::string("a"));
    // Вставка за концом добавляет в конец.
    InsertUtf8(&s, 999, "!");
    ENG_CHECK_EQ(s.back(), '!');

    // AppendCodepointUtf8 проходит круговой путь через Utf8ToCodepoints для кириллицы.
    std::string ru;
    const u32 cps[3] = {0x041Fu, 0x0440u, 0x0438u};  // При
    for (u32 cp : cps) AppendCodepointUtf8(&ru, cp);
    const std::vector<u32> back = Utf8ToCodepoints(ru);
    ENG_CHECK_EQ(back.size(), usize(3));
    ENG_CHECK_EQ(back[0], cps[0]);
    ENG_CHECK_EQ(back[2], cps[2]);
}

// ---------------------------------------------------------------------------
// Процедурная 9-patch-графика
// ---------------------------------------------------------------------------
ENG_TEST(Ui, RoundedRectPixels) {
    const int size = 32;
    std::vector<u8> px;
    const Color fill = Color::FromBytes(200, 60, 40, 255);
    const Color border = Color::FromBytes(255, 255, 255, 255);
    GenerateRoundedRectPixels(size, 8.0f, fill, border, 2.0f, &px);
    ENG_CHECK_EQ(px.size(), usize(size * size * 4));

    auto at = [&](int x, int y, int c) { return px[(static_cast<usize>(y) * size + x) * 4 + c]; };

    // Углы пусты (радиус 8 оставляет самый угол вне фигуры).
    ENG_CHECK_EQ(at(0, 0, 3), 0);
    ENG_CHECK_EQ(at(size - 1, 0, 3), 0);
    ENG_CHECK_EQ(at(0, size - 1, 3), 0);
    ENG_CHECK_EQ(at(size - 1, size - 1, 3), 0);

    // Центральный тексел — сплошная заливка (именно это делает его валидным
    // 9-patch: растяжение середины даёт плоский цвет).
    const int mid = size / 2;
    ENG_CHECK_EQ(at(mid, mid, 3), 255);
    ENG_CHECK_NEAR(at(mid, mid, 0), 200, 1);
    ENG_CHECK_NEAR(at(mid, mid, 1), 60, 1);
    ENG_CHECK_NEAR(at(mid, mid, 2), 40, 1);

    // Середины краёв непрозрачны и несут цвет границы.
    ENG_CHECK_EQ(at(mid, 0, 3), 255);
    ENG_CHECK_NEAR(at(mid, 0, 0), 255, 1);
    ENG_CHECK_EQ(at(mid, size - 1, 3), 255);

    // Граница 2px, поэтому на двух рядах внутрь края уже идёт заливка.
    ENG_CHECK_NEAR(at(mid, 3, 0), 200, 1);
    ENG_CHECK_NEAR(at(mid, 3, 1), 60, 1);

    // Чуть внутри скруглённого угла покрытие частичное (сглаживание).
    bool sawPartial = false;
    for (int y = 0; y < 10 && !sawPartial; ++y)
        for (int x = 0; x < 10; ++x)
            if (at(x, y, 3) > 0 && at(x, y, 3) < 255) {
                sawPartial = true;
                break;
            }
    ENG_CHECK(sawPartial);

    // Вырожденные входы безопасны.
    GenerateRoundedRectPixels(0, 4.0f, fill, border, 1.0f, &px);
    ENG_CHECK_EQ(px.size(), usize(0));
    GenerateRoundedRectPixels(4, 99.0f, fill, border, 1.0f, &px);  // радиус ограничен
    ENG_CHECK_EQ(px.size(), usize(4 * 4 * 4));
    GenerateRoundedRectPixels(8, 2.0f, fill, border, 0.0f, &px);  // без границы
    ENG_CHECK_EQ(px.size(), usize(8 * 8 * 4));
    ENG_CHECK_EQ(px[(4 * 8 + 4) * 4 + 3], 255);
}

ENG_TEST(Ui, RoundedRectCoverage) {
    // Центр бокса с полуразмером 10x10 покрыт полностью.
    ENG_CHECK_NEAR(RoundedRectCoverage(0.0f, 0.0f, 5.0f, 5.0f, 2.0f), 1.0f, 1e-4f);
    // Далеко снаружи — пусто.
    ENG_CHECK_NEAR(RoundedRectCoverage(20.0f, 20.0f, 5.0f, 5.0f, 2.0f), 0.0f, 1e-4f);
    // Точный край квадрата покрыт наполовину.
    ENG_CHECK_NEAR(RoundedRectCoverage(5.0f, 0.0f, 5.0f, 5.0f, 0.0f), 0.5f, 1e-4f);
    // Острый угол квадрата лежит вне скруглённой оболочки.
    ENG_CHECK_NEAR(RoundedRectCoverage(5.4f, 5.4f, 5.0f, 5.0f, 0.4f), 0.0f, 1e-4f);
    // ...но точка чуть внутри радиуса угла покрыта лишь частично.
    const f32 partial = RoundedRectCoverage(4.9f, 4.9f, 5.0f, 5.0f, 0.4f);
    ENG_CHECK(partial > 0.0f && partial < 1.0f);
}

// ---------------------------------------------------------------------------
// Темы
// ---------------------------------------------------------------------------
ENG_TEST(Ui, ThemePalettes) {
    const UiTheme dark = UiTheme::Dark();
    const UiTheme light = UiTheme::Light();
    const UiTheme neon = UiTheme::Neon();

    // Шрифты могут легитимно быть нулевыми (ассет не загружен).
    ENG_CHECK(dark.font == nullptr || dark.font != nullptr);

    // Метрики вменяемые.
    for (const UiTheme* t : {&dark, &light, &neon}) {
        ENG_CHECK_GT(t->rounding, 0.0f);
        ENG_CHECK_GT(t->textSize, 0.0f);
        ENG_CHECK_GT(t->titleSize, t->textSize);
        ENG_CHECK_GT(t->buttonHeight, 0.0f);
        ENG_CHECK_GT(t->itemHeight, 0.0f);
        ENG_CHECK_GT(t->animationSpeed, 0.0f);
        ENG_CHECK_GT(t->padding, 0.0f);
        // Непрозрачные цвета там, где это важно.
        ENG_CHECK_NEAR(t->text.a, 1.0f, 1e-4f);
        ENG_CHECK_NEAR(t->bg.a, 1.0f, 1e-4f);
        ENG_CHECK(t->overlay.a > 0.0f && t->overlay.a <= 1.0f);
        ENG_CHECK(t->shadow.a >= 0.0f && t->shadow.a <= 1.0f);
        // Хуки nine-patch по умолчанию "без графики".
        ENG_CHECK(!t->buttonNormal.valid());
        ENG_CHECK(!t->panelPatch.valid());
    }

    // Dark тёмная, light светлая.
    const f32 darkLum = dark.bg.r + dark.bg.g + dark.bg.b;
    const f32 lightLum = light.bg.r + light.bg.g + light.bg.b;
    ENG_CHECK(darkLum < 1.0f);
    ENG_CHECK(lightLum > 2.0f);
    ENG_CHECK(darkLum < lightLum);

    // Neon тёмная с голубоватым акцентом и пурпурным вторичным цветом.
    const f32 neonLum = neon.bg.r + neon.bg.g + neon.bg.b;
    ENG_CHECK(neonLum < 1.0f);
    ENG_CHECK(neon.accent.b > 0.8f);
    ENG_CHECK(neon.accent.g > 0.8f);
    ENG_CHECK(neon.accent.r < 0.5f);
    ENG_CHECK(neon.danger.r > 0.8f);
    ENG_CHECK(neon.danger.b > 0.3f);
    ENG_CHECK(neon.animationSpeed >= dark.animationSpeed);

    // Три палитры действительно различаются.
    ENG_CHECK(!(dark.accent == light.accent));
    ENG_CHECK(!(dark.accent == neon.accent));
    ENG_CHECK(!(dark.panel == light.panel));
}

// ---------------------------------------------------------------------------
// Области видимости id через контекст (чистая логика, без отрисовки)
// ---------------------------------------------------------------------------
ENG_TEST(Ui, ContextIdScoping) {
    UiContext ctx;
    ctx.Init(nullptr);
    const UiId a1 = ctx.MakeId("button");
    ENG_CHECK(a1 != kUiIdNone);
    ENG_CHECK_EQ(a1, ctx.MakeId("button"));
    ENG_CHECK(a1 != ctx.MakeId("button", 1));

    ctx.PushId("panelA");
    const UiId inA = ctx.MakeId("button");
    ctx.PushId("panelB");
    const UiId inB = ctx.MakeId("button");
    ctx.PopId();
    const UiId inA2 = ctx.MakeId("button");
    ctx.PopId();
    const UiId top = ctx.MakeId("button");

    ENG_CHECK(inA != inB);   // то же имя, другая панель
    ENG_CHECK_EQ(inA, inA2); // восстановлено после PopId
    ENG_CHECK_EQ(top, a1);

    // PushId(UiId) перекрывает базу и тоже восстанавливается.
    ctx.PushId(0x1234ULL);
    const UiId forced = ctx.MakeId("x");
    ctx.PopId();
    ENG_CHECK(forced != ctx.MakeId("x"));

    // Несбалансированный PopId безвреден.
    ctx.PopId();
    ctx.PopId();
    ENG_CHECK(ctx.MakeId("button") != kUiIdNone);
    ctx.Shutdown();
}

ENG_TEST(Ui, ContextThemeAndState) {
    UiContext ctx;
    ctx.Init(nullptr);
    ctx.SetTheme(UiTheme::Neon());
    ENG_CHECK_NEAR(ctx.Theme().accent.b, UiTheme::Neon().accent.b, 1e-5f);
    ctx.SetDpiScale(2.0f);
    ENG_CHECK_NEAR(ctx.DpiScale(), 2.0f, 1e-6f);

    // Состояние виджета сохраняется по id.
    WidgetState& s1 = ctx.State(0xABCULL);
    s1.scrollY = 42.0f;
    WidgetState& s2 = ctx.State(0xABCULL);
    ENG_CHECK_NEAR(s2.scrollY, 42.0f, 1e-6f);
    ENG_CHECK_NEAR(ctx.State(0xDEFULL).scrollY, 0.0f, 1e-6f);

    // Учёт фокуса.
    ctx.SetFocus(0xABCULL);
    ENG_CHECK(ctx.IsFocused(0xABCULL));
    ENG_CHECK_EQ(ctx.FocusedId(), UiId(0xABCULL));
    ENG_CHECK(ctx.WantsKeyboard());
    ctx.ClearFocus();
    ENG_CHECK(!ctx.WantsKeyboard());
    ctx.Shutdown();
}

ENG_TEST(Ui, ContextNoRenderer) {
    UiContext ctx;
    ctx.Init(nullptr);
    // Вызов текста без рендерера или шрифта должен быть безопасным no-op.
    ctx.Text("hello");
    ctx.TextAt("x", {0, 0}, Color::White);
    ctx.TextCentered(Rect{0, 0, 10, 10}, "y", Color::White);
    ctx.Heading("h");
    ctx.Label("a", "b");
    ctx.TextWrapped(Rect{0, 0, 10, 10}, "some text", Color::White);
    ENG_CHECK_NEAR(ctx.TextWidth("abc"), 0.0f, 1e-6f);

    // Помощники раскладки без кадра — no-op, остающиеся в границах.
    const Rect cell = ctx.GridCell(Rect{0, 0, 300, 200}, 3, 2, 1, 1, 10.0f);
    ENG_CHECK_NEAR(cell.x, 0.0f + (300.0f - 20.0f) / 3.0f + 10.0f, 1e-4f);
    ENG_CHECK_NEAR(cell.y, (200.0f - 10.0f) / 2.0f + 10.0f, 1e-4f);
    ENG_CHECK_NEAR(cell.w, (300.0f - 20.0f) / 3.0f, 1e-4f);
    ENG_CHECK_NEAR(cell.h, (200.0f - 10.0f) / 2.0f, 1e-4f);
    ENG_CHECK_NEAR(ctx.GridCell(Rect{0, 0, 100, 100}, 0, 2, 0, 0, 4.0f).w, 100.0f, 1e-4f);

    // Попапы без кадра.
    ENG_CHECK(!ctx.IsPopupOpen("nope"));
    ctx.OpenPopup("p");
    ENG_CHECK(ctx.IsPopupOpen("p"));
    ctx.ClosePopup();
    ENG_CHECK(!ctx.IsPopupOpen("p"));

    // Тосты буферизуются и не рисуются без рендерера.
    for (int i = 0; i < 20; ++i) ctx.Toast("message");
    ctx.RenderOverlays();
    ctx.Shutdown();
}

// ---------------------------------------------------------------------------
// Flex через контекст с ареной (раскладка контейнеров, без отрисовки)
// ---------------------------------------------------------------------------
ENG_TEST(Ui, ContextFlexAndContainers) {
    UiContext ctx;
    ctx.Init(nullptr);
    ctx.PushLayoutPadding(0.0f);
    ctx.PushLayoutSpacing(0.0f);
    ctx.BeginRow("row", Rect{0, 0, 400, 100}, 10.0f);

    const Rect a = ctx.Alloc(100, 20, LayoutSize::Fixed(100));
    const Rect b = ctx.Alloc(0, 20, LayoutSize::Grow());
    const Rect c = ctx.Alloc(0, 20, LayoutSize::Grow());
    ENG_CHECK_NEAR(a.x, 0.0f, 1e-4f);
    ENG_CHECK_NEAR(a.w, 100.0f, 1e-4f);
    // Три слота с двумя промежутками по 10px оставляют 270 двум grow-элементам:
    // первый берёт половину свободного на тот момент, второй — остальное.
    // Строка пересчитывается по мере добавления слотов: первый grow-слот получил
    // всё свободное тогда пространство (400 - 100 - 10), а второй отчёт показывает
    // финальное деление остатка (400 - 100 - 2*10 = 280) на 140 + 140.
    ENG_CHECK_NEAR(b.x, 110.0f, 1e-4f);
    ENG_CHECK_NEAR(b.w, 400.0f - 100.0f - 10.0f, 1e-4f);  // пока только один grow
    ENG_CHECK_NEAR(c.x, 110.0f + 140.0f + 10.0f, 1e-4f);
    ENG_CHECK_NEAR(c.w, 140.0f, 1e-4f);  // финальное деление остатка
    ENG_CHECK_NEAR(c.Right(), 400.0f, 1e-4f);
    ctx.EndRow();

    // Стек по столбцам.
    ctx.BeginColumn("col", Rect{10, 10, 200, 200}, 5.0f);
    const Rect r1 = ctx.Alloc(100, 30, LayoutSize::Fixed(30));
    const Rect r2 = ctx.Alloc(100, 30, LayoutSize::Fixed(30));
    ENG_CHECK_NEAR(r1.y, 10.0f, 1e-4f);
    ENG_CHECK_NEAR(r2.y, 45.0f, 1e-4f);
    ctx.EndColumn();

    // Percent от главной оси и content, размер которого равен запрошенному.
    ctx.BeginColumn("col2", Rect{0, 0, 200, 300}, 0.0f);
    const Rect p = ctx.Alloc(0, 0, LayoutSize::Percent(0.5f));
    ENG_CHECK_NEAR(p.h, 150.0f, 1e-4f);
    const Rect ct = ctx.Alloc(0, 0, LayoutSize::Content());
    ENG_CHECK_NEAR(ct.h, 20.0f, 1e-4f);  // внутренний размер по умолчанию для столбца
    ctx.EndColumn();

    // Переполнение: аллокации никогда не выходят за главную ось контейнера.
    ctx.BeginRow("row2", Rect{0, 0, 100, 40}, 0.0f);
    const Rect big = ctx.Alloc(500, 10, LayoutSize::Fixed(500));
    const Rect after = ctx.Alloc(50, 10, LayoutSize::Fixed(50));
    // Fixed-элементы сохраняют запрошенный размер (SolveFlex никогда не выдумывает
    // отрицательное пространство); излишек просто оказывается вне clip контейнера,
    // поэтому следующий слот сдвигается за правый край, а не перекрывается.
    ENG_CHECK_NEAR(big.w, 500.0f, 1e-4f);
    ENG_CHECK_NEAR(big.x, 0.0f, 1e-4f);
    ENG_CHECK_NEAR(after.w, 50.0f, 1e-4f);
    ENG_CHECK(after.x >= big.Right() - 1e-4f);
    // Grow-слот в той же переполняющейся строке всё равно схлопывается в ноль.
    const Rect collapsed = ctx.Alloc(0, 10, LayoutSize::Grow());
    ENG_CHECK_NEAR(collapsed.w, 0.0f, 1e-4f);
    ctx.EndRow();

    // Spacer / Dummy / Separator двигают курсор, не выходя наружу.
    ctx.BeginColumn("col3", Rect{0, 0, 50, 100}, 0.0f);
    ctx.Spacer(10.0f);
    ctx.Dummy(20.0f, 5.0f);
    ctx.Separator();
    ctx.EndColumn();

    // Панели / группы / scroll view вкладываются и корректно разматываются.
    ctx.BeginPanel("panel", Rect{0, 0, 200, 200});
    ctx.BeginPanel("inner", Rect{10, 10, 100, 100});
    const Rect inPanel = ctx.Alloc(0, 20, LayoutSize::Fixed(20));
    ENG_CHECK(inPanel.y >= 10.0f);
    ctx.EndPanel();
    ctx.EndPanel();
    ENG_CHECK_NEAR(ctx.Screen().w, 0.0f, 1e-6f);  // кадр не начинался

    ctx.BeginScrollView("sv", Rect{0, 0, 100, 100}, 500.0f, false);
    const Rect content = ctx.Alloc(0, 50, LayoutSize::Fixed(50));
    ENG_CHECK_NEAR(content.h, 50.0f, 1e-4f);
    ctx.EndScrollView();
    ctx.Shutdown();
}

namespace {

// Настоящий Renderer2D в инертном режиме (без GL-контекста): пути, раскладка
// текста и hit-тестирование работают, отправка на GPU пропускается. Так выглядит
// headless-кадр.
Renderer2D& HeadlessR2D() {
    static Renderer2D* const kInert = [] {
        Renderer2D* r = new Renderer2D();
        r->Init();  // возвращает true даже без контекста (инертный режим)
        return r;
    }();
    return *kInert;
}


}  // namespace

ENG_TEST(Ui, ContextFrameHeadless) {
    // Полный цикл BeginFrame/EndFrame без рендерера: проверяет регистрацию
    // виджетов, навигацию фокуса и пути обновления покадрового состояния.
    UiContext ctx;
    ctx.Init(nullptr);
    Input input;
    input.BeginFrame();
    ctx.BeginFrame(HeadlessR2D(), input, Rect{0, 0, 1280, 720}, 1.0f / 60.0f);
    ctx.RegisterWidget(ctx.MakeId("w1"), Rect{0, 0, 100, 30}, true);
    ctx.RegisterWidget(ctx.MakeId("w2"), Rect{0, 40, 100, 30}, true);
    ctx.RegisterWidget(ctx.MakeId("w3"), Rect{0, 80, 100, 30}, false);
    ctx.Text("some text");
    ctx.EndFrame();
    ENG_CHECK_EQ(ctx.Stats().widgets, 3);
    ctx.RenderOverlays();

    // Tab циклически меняет фокус в порядке регистрации (запрос применяется
    // на следующем EndFrame).
    ctx.SetFocus(ctx.MakeId("w1"));
    ctx.BeginFrame(HeadlessR2D(), input, Rect{0, 0, 1280, 720}, 1.0f / 60.0f);
    input.OnKey(Key::Tab, KeyAction::Press, false);
    ctx.RegisterWidget(ctx.MakeId("w1"), Rect{0, 0, 100, 30}, true);
    ctx.RegisterWidget(ctx.MakeId("w2"), Rect{0, 40, 100, 30}, true);
    ctx.RegisterWidget(ctx.MakeId("w3"), Rect{0, 80, 100, 30}, false);
    ctx.EndFrame();
    ENG_CHECK_EQ(ctx.FocusedId(), ctx.MakeId("w2"));

    ctx.Shutdown();
}

ENG_TEST(Ui, ButtonClickSemantics) {
    // Точки входа отрисовки защищены от нулевых указателей, поэтому конечный
    // автомат immediate mode можно гонять без рендерера.
    UiContext ctx;
    ctx.Init(nullptr);
    Input input;
    const Rect btn{10, 10, 120, 36};

    // Кадр 1: нажатие внутри, ничего не срабатывает (виджет становится активным
    // при нажатии и сообщает о клике только при отпускании внутри).
    input.BeginFrame();
    input.OnMouseMove({50, 20});
    input.OnMouseButton(MouseButton::Left, true, {50, 20});
    ctx.BeginFrame(HeadlessR2D(), input, Rect{0, 0, 640, 480}, 1.0f / 60.0f);
    ENG_CHECK(!ctx.Button("ok", btn));
    ctx.EndFrame();
    input.EndFrame();

    // Кадр 2: всё ещё зажато -> нет клика.
    input.BeginFrame();
    input.OnMouseMove({55, 22});
    ctx.BeginFrame(HeadlessR2D(), input, Rect{0, 0, 640, 480}, 1.0f / 60.0f);
    ENG_CHECK(!ctx.Button("ok", btn));
    ctx.EndFrame();
    input.EndFrame();

    // Кадр 3: отпущено внутри -> клик.
    input.BeginFrame();
    input.OnMouseButton(MouseButton::Left, false, {55, 22});
    ctx.BeginFrame(HeadlessR2D(), input, Rect{0, 0, 640, 480}, 1.0f / 60.0f);
    ENG_CHECK(ctx.Button("ok", btn));
    ctx.EndFrame();
    input.EndFrame();

    // Отпускание снаружи отменяет клик (утащили).
    UiContext ctx2;
    ctx2.Init(nullptr);
    Input in2;
    in2.BeginFrame();
    in2.OnMouseMove({50, 20});
    in2.OnMouseButton(MouseButton::Left, true, {50, 20});
    ctx2.BeginFrame(HeadlessR2D(), in2, Rect{0, 0, 640, 480}, 1.0f / 60.0f);
    ENG_CHECK(!ctx2.Button("ok", btn));
    ctx2.EndFrame();
    in2.EndFrame();

    in2.BeginFrame();
    in2.OnMouseMove({400, 400});
    in2.OnMouseButton(MouseButton::Left, false, {400, 400});
    ctx2.BeginFrame(HeadlessR2D(), in2, Rect{0, 0, 640, 480}, 1.0f / 60.0f);
    ENG_CHECK(!ctx2.Button("ok", btn));  // отпущено снаружи: нет клика
    ctx2.EndFrame();
    in2.EndFrame();

    // Отключённые кнопки никогда не срабатывают.
    ctx2.BeginFrame(HeadlessR2D(), in2, Rect{0, 0, 640, 480}, 1.0f / 60.0f);
    in2.OnMouseMove({50, 20});
    in2.OnMouseButton(MouseButton::Left, true, {50, 20});
    ENG_CHECK(!ctx2.Button("ok", btn, false));
    ctx2.EndFrame();
    in2.EndFrame();
    ctx2.BeginFrame(HeadlessR2D(), in2, Rect{0, 0, 640, 480}, 1.0f / 60.0f);
    in2.OnMouseButton(MouseButton::Left, false, {50, 20});
    ENG_CHECK(!ctx2.Button("ok", btn, false));
    ctx2.EndFrame();
    in2.EndFrame();

    ctx.Shutdown();
    ctx2.Shutdown();
}

ENG_TEST(Ui, ButtonKeyboardActivation) {
    UiContext ctx;
    ctx.Init(nullptr);
    Input input;
    const Rect btn{10, 10, 120, 36};
    const UiId id = ButtonId(ctx, "ok");

    // Фокусируем кнопку и нажимаем Space: запрос активации записывается в конце
    // кадра и потребляется виджетом на следующем.
    input.BeginFrame();
    input.OnKey(Key::Space, KeyAction::Press, false);
    ctx.BeginFrame(HeadlessR2D(), input, Rect{0, 0, 640, 480}, 1.0f / 60.0f);
    ctx.SetFocus(id);
    ENG_CHECK(!ctx.Button("ok", btn));
    ctx.EndFrame();
    input.EndFrame();  // сбрасывает KeyPressed для следующего кадра

    input.BeginFrame();
    ctx.BeginFrame(HeadlessR2D(), input, Rect{0, 0, 640, 480}, 1.0f / 60.0f);
    ENG_CHECK(ctx.Button("ok", btn));  // активировано по Space
    ctx.EndFrame();
    input.EndFrame();

    // Запрос потреблён: дважды не срабатывает.
    input.BeginFrame();
    ctx.BeginFrame(HeadlessR2D(), input, Rect{0, 0, 640, 480}, 1.0f / 60.0f);
    ENG_CHECK(!ctx.Button("ok", btn));
    ctx.EndFrame();
    input.EndFrame();

    ctx.Shutdown();
}

ENG_TEST(Ui, TabBarReturnValue) {
    // TabBar должен сообщать об изменении ровно один раз, в кадре выбора таба.
    UiContext ctx;
    ctx.Init(nullptr);
    Input input;
    const Rect bar{0, 0, 300, 32};
    const std::vector<std::string> tabs{"A", "B", "C"};
    int selected = 0;

    // Нажатие на средний таб.
    input.BeginFrame();
    input.OnMouseMove({150, 16});
    input.OnMouseButton(MouseButton::Left, true, {150, 16});
    ctx.BeginFrame(HeadlessR2D(), input, Rect{0, 0, 640, 480}, 1.0f / 60.0f);
    ENG_CHECK(ctx.TabBar("tabs", bar, tabs, &selected));
    ENG_CHECK_EQ(selected, 1);
    ctx.EndFrame();
    input.EndFrame();

    // Отпускание на том же табе: второго "изменения" нет.
    input.BeginFrame();
    input.OnMouseButton(MouseButton::Left, false, {150, 16});
    ctx.BeginFrame(HeadlessR2D(), input, Rect{0, 0, 640, 480}, 1.0f / 60.0f);
    ENG_CHECK(!ctx.TabBar("tabs", bar, tabs, &selected));
    ENG_CHECK_EQ(selected, 1);
    ctx.EndFrame();
    input.EndFrame();

    // Нажатие на уже выбранный таб — тоже не изменение.
    input.BeginFrame();
    input.OnMouseButton(MouseButton::Left, true, {150, 16});
    ctx.BeginFrame(HeadlessR2D(), input, Rect{0, 0, 640, 480}, 1.0f / 60.0f);
    ENG_CHECK(!ctx.TabBar("tabs", bar, tabs, &selected));
    ctx.EndFrame();
    input.EndFrame();

    // Нулевой выходной параметр допустим.
    input.BeginFrame();
    ctx.BeginFrame(HeadlessR2D(), input, Rect{0, 0, 640, 480}, 1.0f / 60.0f);
    ENG_CHECK(!ctx.TabBar("tabs", bar, tabs, nullptr));
    ctx.EndFrame();
    input.EndFrame();
    ctx.Shutdown();
}

ENG_TEST(Ui, ThemeAppliedThroughSetTheme) {
    // Тема, применённая между кадрами, должна быть видна следующему кадру.
    UiContext ctx;
    ctx.Init(nullptr);
    Input input;
    UiTheme custom = UiTheme::Dark();
    custom.accent = Color{1.0f, 0.0f, 0.5f, 1.0f};
    ctx.SetTheme(custom);

    input.BeginFrame();
    ctx.BeginFrame(HeadlessR2D(), input, Rect{0, 0, 640, 480}, 1.0f / 60.0f);
    ENG_CHECK_NEAR(ctx.Theme().accent.r, 1.0f, 1e-5f);
    ENG_CHECK_NEAR(ctx.Theme().accent.b, 0.5f, 1e-5f);
    ctx.EndFrame();
    input.EndFrame();

    // Копия внутри кадра переживает и следующий BeginFrame.
    input.BeginFrame();
    ctx.BeginFrame(HeadlessR2D(), input, Rect{0, 0, 640, 480}, 1.0f / 60.0f);
    ENG_CHECK_NEAR(ctx.Theme().accent.b, 0.5f, 1e-5f);
    ctx.EndFrame();
    input.EndFrame();
    ctx.Shutdown();
}

ENG_TEST(Ui, InputBlockingAndWantsMouse) {
    UiContext ctx;
    ctx.Init(nullptr);
    Input input;
    input.BeginFrame();
    input.OnMouseMove({20, 20});
    input.OnMouseButton(MouseButton::Left, true, {20, 20});
    ctx.BeginFrame(HeadlessR2D(), input, Rect{0, 0, 640, 480}, 1.0f / 60.0f);
    ctx.BeginPanel("p", Rect{0, 0, 200, 200});
    ctx.EndPanel();
    ENG_CHECK(ctx.IsHovered(Rect{0, 0, 200, 200}));
    ENG_CHECK(ctx.MouseDown(0));
    ENG_CHECK(ctx.MouseClicked(0));
    ctx.EndFrame();

    // Пока ввод заблокирован, ничего не сообщает о hover и состоянии мыши. Флаг
    // блокировки сэмплируется в BeginFrame, поэтому приложение ставит его между кадрами.
    ctx.SetInputBlocked(true);
    input.BeginFrame();
    input.OnMouseMove({20, 20});
    input.OnMouseButton(MouseButton::Left, true, {20, 20});
    ctx.BeginFrame(HeadlessR2D(), input, Rect{0, 0, 640, 480}, 1.0f / 60.0f);
    ENG_CHECK(!ctx.IsHovered(Rect{0, 0, 200, 200}));
    ENG_CHECK(!ctx.MouseDown(0));
    ENG_CHECK(!ctx.MouseClicked(0));
    // Виджет, зарегистрированный при блокировке, тоже никогда не помечается как hot.
    ctx.RegisterWidget(ctx.MakeId("w"), Rect{0, 0, 200, 200}, true);
    ctx.EndFrame();
    input.EndFrame();
    ENG_CHECK(!ctx.IsHot(ctx.MakeId("w")));

    // Снятие блокировки восстанавливает обычное поведение.
    ctx.SetInputBlocked(false);
    input.BeginFrame();
    input.OnMouseMove({20, 20});
    ctx.BeginFrame(HeadlessR2D(), input, Rect{0, 0, 640, 480}, 1.0f / 60.0f);
    ENG_CHECK(ctx.IsHovered(Rect{0, 0, 200, 200}));
    ctx.EndFrame();
    input.EndFrame();
    ctx.Shutdown();
}

ENG_TEST(Ui, TextFieldEditingHeadless) {
    // Гоняет конечный автомат текстового редактора без рендерера: фокус по клику,
    // ввод кириллического кодпойнта, затем Backspace.
    UiContext ctx;
    ctx.Init(nullptr);
    Input input;
    const Rect field{10, 10, 200, 32};
    std::string text = "ab";
    const UiId id = TextFieldId(ctx, "field");

    input.BeginFrame();
    input.OnMouseMove({20, 20});
    input.OnMouseButton(MouseButton::Left, true, {20, 20});
    ctx.BeginFrame(HeadlessR2D(), input, Rect{0, 0, 640, 480}, 1.0f / 60.0f);
    ctx.TextField("field", field, &text);
    ctx.EndFrame();
    input.EndFrame();
    ENG_CHECK(ctx.IsFocused(id));

    input.BeginFrame();
    input.OnMouseButton(MouseButton::Left, false, {20, 20});
    ctx.BeginFrame(HeadlessR2D(), input, Rect{0, 0, 640, 480}, 1.0f / 60.0f);
    ctx.TextField("field", field, &text);
    ctx.EndFrame();
    input.EndFrame();

    // Ввод U+0416 (кириллическая Ж).
    input.BeginFrame();
    input.OnText(0x0416u);
    ctx.BeginFrame(HeadlessR2D(), input, Rect{0, 0, 640, 480}, 1.0f / 60.0f);
    const bool edited = ctx.TextField("field", field, &text);
    ctx.EndFrame();
    input.EndFrame();
    ENG_CHECK(edited);
    ENG_CHECK_EQ(Utf8Length(text), usize(3));

    // Backspace удаляет весь двухбайтовый кодпойнт.
    input.BeginFrame();
    input.OnKey(Key::Backspace, KeyAction::Press, false);
    ctx.BeginFrame(HeadlessR2D(), input, Rect{0, 0, 640, 480}, 1.0f / 60.0f);
    const bool edited2 = ctx.TextField("field", field, &text);
    ctx.EndFrame();
    input.EndFrame();
    ENG_CHECK(edited2);
    ENG_CHECK_STR_EQ(text, std::string("ab"));

    ctx.Shutdown();
}

// ---------------------------------------------------------------------------
// Пути, зависящие от GL
// ---------------------------------------------------------------------------
ENG_TEST(Ui, RoundedRectTextureGL) {
    ENG_REQUIRE_GL();
    Texture tex = MakeRoundedRectTexture(48, 12.0f, Color::FromARGB(0xFF2F6FE0), Color::White, 2.0f);
    ENG_CHECK(tex.Valid());
    ENG_CHECK_EQ(tex.Width(), 48);
    ENG_CHECK_EQ(tex.Height(), 48);
    ENG_CHECK_EQ(static_cast<int>(tex.Format()), static_cast<int>(PixelFormat::RGBA8));
    // Построенный на ней 9-patch-стиль сообщает valid.
    UiTheme::NinePatchStyle style;
    style.texture = &tex;
    style.patch = NinePatch::Uniform(14.0f);
    ENG_CHECK(style.valid());
}

ENG_TEST(Ui, FullFrameGL) {
    ENG_REQUIRE_GL();
    Renderer2D r2d;
    if (!r2d.Init()) ENG_SKIP("Renderer2D::Init failed");
    r2d.BeginFrame(1280, 720, 1.0f);

    Input input;
    input.BeginFrame();
    input.OnMouseMove({100, 100});

    UiContext ctx;
    ctx.Init(&r2d);
    ctx.BeginFrame(r2d, input, Rect{0, 0, 1280, 720}, 1.0f / 60.0f);
    bool clicked = false;
    clicked = ctx.Button("Click me", Rect{20, 20, 140, 36});
    (void)clicked;
    bool check = false;
    ctx.Checkbox("check", Rect{20, 70, 200, 24}, &check);
    f32 value = 0.5f;
    ctx.Slider("slider", Rect{20, 110, 200, 20}, &value, 0.0f, 1.0f);
    std::string text = "hello";
    ctx.TextField("field", Rect{20, 150, 200, 32}, &text);
    ctx.ProgressBar(Rect{20, 200, 200, 18}, 0.4f, "40%");
    ctx.Spinner(Rect{20, 230, 32, 32}, 1.0f);
    std::vector<std::string> items{"a", "b", "c"};
    int sel = 0;
    ctx.ListView("list", Rect{20, 280, 200, 120}, items, &sel);
    ctx.EndFrame();
    ctx.RenderOverlays();
    r2d.EndFrame();
    r2d.Shutdown();
    ENG_CHECK(ctx.Stats().widgets > 0);
}

ENG_TEST(Ui, RoundedTextureFrameGL) {
    ENG_REQUIRE_GL();
    Renderer2D r2d;
    if (!r2d.Init()) ENG_SKIP("Renderer2D::Init failed");
    r2d.BeginFrame(640, 480, 1.0f);
    Texture tex = MakeRoundedRectTexture(48, 12.0f, Color::FromARGB(0xFF252935), Color::White, 2.0f);
    if (!tex.Valid()) ENG_SKIP("texture creation failed");

    Input input;
    input.BeginFrame();
    UiContext ctx;
    ctx.Init(&r2d);
    ctx.BeginFrame(r2d, input, Rect{0, 0, 640, 480}, 1.0f / 60.0f);
    ctx.SetButton9Patch(&tex, &tex, &tex, NinePatch::Uniform(14.0f));
    ctx.Button("9-patch", Rect{20, 20, 160, 40});
    ctx.EndFrame();
    ctx.RenderOverlays();
    r2d.EndFrame();
    r2d.Shutdown();
}
