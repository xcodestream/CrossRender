// Векторный рендеринг текста на GPU в стиле Slug.
//
// Оригинальная реализация техники из статьи Эрика Ленгьела "GPU-Centered
// Font Rendering Directly from Glyph Outlines" (JCGT 2017) — той самой,
// что стоит за библиотекой Slug. Контуры глифов хранятся как квадратичные
// кривые Безье на CPU и упаковываются в две текстуры данных, а фрагментный
// шейдер восстанавливает покрытие на пиксель, решая квадратичное уравнение
// для луча через пиксель. Ни один глиф не растеризуется в атлас.
//
// ---------------------------------------------------------------------------
// Границы циклов шейдера
// ---------------------------------------------------------------------------
// GLES3/WebGL2 требуют константной границы цикла, поэтому цикл по полосам
//   for (int i = 0; i < SLUG_MAX_CURVES_PER_BAND; ++i) { if (i >= end) break; ... }
// идёт с SLUG_MAX_CURVES_PER_BAND == 128. PrepareGlyph распределяет кривые
// глифа по 8..16 горизонтальным полосам; если последовательность полосы
// превысила бы 128, она передаётся соседней полосе (последовательность которой
// охватывает оба интервала y), а сама полоса остаётся пустой, так что
// константная граница никогда не нарушается молча. 128 выбрано после замера
// реальной 'S' при em 64px: самая плотная её полоса содержит 68 кривых, поэтому
// граница 64 молча теряла кривые и пробивала в глифе дыры. Цена — более длинный
// худший цикл в шейдере; обычный случай выходит по собственному числу кривых полосы.
//
// ---------------------------------------------------------------------------
// Раскладка текстур
// ---------------------------------------------------------------------------
// Текстура кривых: одна 2D RGBA32F-текстура, ширина 4, высота ceil(N/2).
//   Кривая занимает ОДНУ СТРОКУ (2 текселя), чтобы никогда не пересекать её границу:
//       texel (0 + 2*(i%2), i/2) = (p0.x, p0.y, p1.x, p1.y)
//       texel (1 + 2*(i%2), i/2) = (p2.x, p2.y, bandIndex, flags)
//   RGBA32F + TextureFilter::Nearest: GLES3/WebGL2 гарантируют texelFetch для
//   float-текстур, но не *фильтрацию* float-текстур (нужна
//   OES_texture_float_linear), поэтому nearest + texelFetch — переносимый выбор.
//   Четыре столбца (две кривые на строку) вместо двух делают текстуру вдвое
//   ниже: бюджет 65536 кривых требует 16384 строк вместо 32768.
//
// Текстура полос: одна 2D RGBA32F-текстура, ширина 4, высота ceil(M/2), та же
// построчная упаковка, M = общее число полос по всем глифам:
//       texel (0 + 2*(b%2), b/2) = (firstCurve, curveCount, 0, 0)
//   Полосы глифа выделяются подряд в этом глобальном массиве, поэтому
//   SlugGlyph::bandOffsetY хранит *абсолютный индекс* первой полосы глифа
//   (не строку текстуры), а bandOffsetX не используется: шейдер выводит и
//   столбец текселя, и строку из этого единственного индекса. Это должно быть
//   ровно то же отображение, что использовал PrepareGlyph при построении
//   массива, поэтому оба написаны по одной формуле (см. slugBandIndex в GLSL).
//   RGBA32F вместо более очевидного R32F выбрано намеренно: R32F-текстура,
//   созданная через Texture::Create (GL_R32F + GL_RED + GL_FLOAT), корректно
//   загружается, но на контексте GL 3.2, под которым велась разработка,
//   продолжает сэмплироваться нулём, что молча отключало весь поиск по полосам.
//   Обе текстуры данных теперь используют единственный формат, проверенно
//   работающий здесь. Числа float — малые точные целые (< 2^24), шейдер
//   преобразует их через int(v + 0.5), поэтому целочисленный формат не нужен.
//
// ---------------------------------------------------------------------------
// Задокументированные упрощения относительно Slug / статьи
// ---------------------------------------------------------------------------
//   * Отрисовка пакетируется по <= 8 подряд идущим глифам с общими rotation и
//     scale (per-instance uniform-массивы + glDrawArraysInstanced); серия с
//     индивидуальным поворотом (twist / путь / дуга) откатывается к одному draw
//     call на глиф. Slug упаковывает всю серию в несколько вершинных записей.
//   * Отбрасываемая тень — несколько дрожащих, с масштабированной альфой копий
//     квада глифа: дешёвая аппроксимация блюра, а не настоящий Гаусс.
//   * Градиент — локальный для глифа вертикальный переход (внутренний цвет
//     вверху чернильного бокса, внешний внизу). Slug поддерживает произвольную заливку.
//   * Нет depth pre-pass и ключа порядка кривых для перекрывающихся глифов:
//     текст рисуется в порядке строки стандартным алгоритмом художника.
//   * Кривые в y-вниз (2D-соглашение движка). Покрытие считается лучом в +x
//     со стандартным полуоткрытым правилом пересечения [0,1); знак числа
//     обхода не важен, так как покрытие использует |winding|, поэтому
//     соглашение об ориентации контура не имеет значения.
//   * DrawOnArc не раскрывает флаг `clockwise` в замороженном контракте, поэтому
//     дуга всегда проходится против часовой (угол растёт), а `outside`
//     выбирает сторону окружности.
//   * SlugTextStyle::billboard игнорируется: он описывает 3D-текст-билборд и
//     не имеет смысла для 2D-серии в экранных координатах.
//   * Stats::curvesEvaluated считает кривые, сделанные доступными для
//     нарисованных глифов, а не оценки по фрагментам — их считает только GPU.

#include "crossrender/gfx/SlugText.h"

#include "crossrender/gfx/GL.h"
#include "crossrender/core/Log.h"
#include "crossrender/text/Font.h"
#include "crossrender/gfx/Shader.h"
#include "crossrender/gfx/Renderer2D.h"

#include <cmath>
#include <chrono>
#include <string>
#include <vector>
#include <algorithm>

namespace crossrender {
namespace {

// ---------------------------------------------------------------------------
// Константы шейдера / раскладки
// ---------------------------------------------------------------------------
constexpr int kMaxCurvesPerBand = 128;  // == SLUG_MAX_CURVES_PER_BAND в GLSL
constexpr int kMinBandCount = 8;
constexpr int kMaxBandCount = 64;

constexpr int kCurveColumns = 4;  // текселей в строке (2 кривые)
constexpr int kBandColumns = 4;   // текселей в строке (2 полосы)

constexpr int kInitialCurveCapacity = 65536;  // кривых -> 16384 строк текстуры
constexpr int kInitialBandCapacity = 16384;   // полос  ->  8192 строк текстуры
constexpr int kMaxBatchSize = 8;              // должно совпадать с SLUG_MAX_BATCH в GLSL

// ---------------------------------------------------------------------------
// Эталонное решение на CPU (в точности отражено в GLSL ниже)
// ---------------------------------------------------------------------------
// Действительные корни a*t^2 + b*t + c == 0 — power-basis-форма координаты y
// кривой (как получаются a, b и c, см. crossrender::AccumulateCurve).
// Линейный запасной вариант t = -c/b покрывает y, зависящий ровно линейно;
// горизонтальный отрезок даёт a = b = 0 и вовсе без корней, и это верно,
// потому что он не может пересечь луч.
//
// Историческая заметка о прежнем виде этого комментария:
//
//     y(t) = a*t^2 + b*t + c,   a = p0.y - 2 p1.y + p2.y
//                               b = -2 (p0.y - p1.y)
//                               c = p0.y - rayY
//
// то есть (a, b, c) = (yA, -2 yB, p0.y - p.y) с сокращениями yA/yB,
// используемыми повсюду; q(1) = yA - 2 yB + y0 = p2.y подтверждает знаки.
// Решатель и вычисление ниже всегда нужно менять вместе: те же коэффициенты
// вычисляют саму кривую, поэтому решение и оценка не могут разойтись. GLSL
// повторяет эту функцию оператор в оператор; держите эпсилоны синхронными.
inline bool QuadraticRoots(f32 qA, f32 qB, f32 qC, f32* r0, f32* r1, int* count) {
    if (std::fabs(qA) > 1e-12f) {
        const f32 disc = qB * qB - 4.0f * qA * qC;
        if (disc < 0.0f) {
            *count = 0;
            return false;
        }
        const f32 sq = std::sqrt(disc);
        *r0 = (-qB - sq) / (2.0f * qA);
        *r1 = (-qB + sq) / (2.0f * qA);
        *count = 2;
        return true;
    }
    if (std::fabs(qB) > 1e-20f) {
        *r0 = -qC / qB;
        *count = 1;
        return true;
    }
    *count = 0;
    return false;
}

// Одна кривая против луча вправо через `p`. Добавляет знаковое пересечение в
// *winding и складывает локальное расстояние до края в *edgeIn (пересечения
// с положительным обходом, т.е. внутренность фигуры) и *edgeOut (противоположные
// пересечения). CoverageAt/WindingAt построены на этом, а во фрагментном шейдере
// та же арифметика — это позволяет тесту сравнивать то и другое.
inline void AccumulateCurve(const SlugCurve& c, const Vec2& p, f32* winding, f32* edgeIn,
                            f32* edgeOut) {
    // Стандартный лучевой тест числа обхода: пускаем луч в +x из `p` и считаем
    // знаковые пересечения кривой с p.y.
    //
    // Координата y кривой в power basis:
    //     y(t) = a t^2 + b t + c,  a = p0.y - 2 p1.y + p2.y (вторая разность)
    //                              b = 2 (p1.y - p0.y)      (из q(1) = p2.y)
    //                              c = p0.y - p.y          (хотим y(t) == p.y)
    // а x(t) использует те же коэффициенты по оси x. Луч должен идти
    // горизонтально: вертикальный луч не может определить, по какую сторону
    // вертикальное ребро (его x пересечения не зависит от луча), из-за чего
    // однажды молча пропали все прямые штрихи в глифе.
    const f32 yA = c.p0.y - 2.0f * c.p1.y + c.p2.y;
    const f32 yB = 2.0f * (c.p1.y - c.p0.y);
    f32 ry0 = 0, ry1 = 0;
    int ryn = 0;
    if (!QuadraticRoots(yA, yB, c.p0.y - p.y, &ry0, &ry1, &ryn)) return;

    const f32 xA = c.p0.x - 2.0f * c.p1.x + c.p2.x;
    const f32 xB = 2.0f * (c.p1.x - c.p0.x);
    for (int k = 0; k < ryn; ++k) {
        // Полуоткрытый [0, 1): t == 1 — конечная точка кривой, она же начало
        // следующей кривой и без этого посчиталась бы дважды, а t == 0
        // (замыкающая точка контура) нужно сохранять, иначе теряется последний
        // сегмент каждого замкнутого контура.
        const f32 t = k == 0 ? ry0 : ry1;
        if (t < 0.0f || t >= 1.0f) continue;
        const f32 x = (xA * t + xB) * t + c.p0.x;
        if (x < p.x) continue;  // пересечение левее точки
        // Направление пересечения: при y вниз dy/dt > 0 означает, что контур
        // через пересечение идёт в сторону роста y, что по стандартному
        // соглашению даёт положительный (по часовой) вклад в обход.
        // Контур, закрученный по часовой в пространстве y вниз — обычная
        // ориентация TrueType — поэтому сообщает winding > 0.
        const f32 dy = 2.0f * yA * t + yB;
        const f32 w = dy > 0.0f ? 1.0f : -1.0f;
        *winding += w;
        // Расстояние от p до пересечения вдоль нормали кривой в корне; край
        // нужно измерять точно в точке пересечения, а не приближать по лучу,
        // чтобы аналитический член сглаживания был корректен.
        const f32 dx = x - p.x;
        const f32 len = std::sqrt(dx * dx);
        if (len > 1e-6f) {
            // x'(t) — x-компонента касательной, dy — её y-компонента; пересечение
            // лежит на луче, поэтому dx — единственное смещение, а
            // перпендикулярное расстояние равно |dx * dy| / |касательная|.
            const f32 xt = 2.0f * xA * t + xB;
            const f32 off = (dx * dy) / std::sqrt(xt * xt + dy * dy);
            const f32 mag = std::fabs(off);
            if (w > 0.0f) {
                if (mag < *edgeIn) *edgeIn = mag;
            } else if (mag < *edgeOut) {
                *edgeOut = mag;
            }
        }
    }
}

// Преобразует (winding, edgeIn, edgeOut) в покрытие, в точности как fillCov
// шейдера. `kNoEdge` заменяет «пересечений нет вовсе»: оно достаточно велико,
// чтобы деление насытилось до 0 или 1.
constexpr f32 kNoEdge = 1e30f;

inline f32 CoverageFromWinding(f32 winding, f32 edgeIn, f32 edgeOut, f32 aaWidth) {
    const f32 inside = std::fabs(winding) > 0.5f;
    const f32 dist = inside ? edgeIn : -edgeOut;
    const f32 aa = aaWidth > 1e-5f ? aaWidth : 1e-5f;
    return Clamp(0.5f + dist / aa, 0.0f, 1.0f);
}

// ---------------------------------------------------------------------------
// GLSL. builtin::Preamble() всегда подставляет #version и квалификаторы точности.
// ---------------------------------------------------------------------------
const char* kSlugVert = R"GLSL(
const int SLUG_MAX_BATCH = 8;

layout(location = 0) in vec2 aCornerUnit;

uniform mat4 uProj;
uniform vec4 uGlyphA[SLUG_MAX_BATCH];   // (basisX.x, basisX.y, basisY.x, basisY.y) in px
uniform vec4 uGlyphB[SLUG_MAX_BATCH];   // (origin.x, origin.y, emCentre.x, emCentre.y)
uniform vec4 uGlyphC[SLUG_MAX_BATCH];   // (firstCurve, curveCount, bandOffsetX, bandOffsetY)
uniform vec4 uGlyphG[SLUG_MAX_BATCH];   // (bandCount, minY, invSpan, 0)
uniform vec4 uGlyphE[SLUG_MAX_BATCH];   // (halfX, halfY) in em (vertex stage)

out vec2 vLocal;
flat out int vIndex;

void main() {
    vIndex = gl_InstanceID;
    int i = vIndex;
    if (i >= SLUG_MAX_BATCH) i = SLUG_MAX_BATCH - 1;
    // aCornerUnit is the unit box corner in [-1,1]^2. uGlyphE scales it to the
    // glyph's padded em box (so vLocal stays in the outline's own em space) and
    // uGlyphA scales it to screen pixels (so the vertex stage is one multiply
    // add). Scaling the local coordinate by the half extent is essential: using
    // the raw unit corner made the fragment shader sample a 2-em box while the
    // quad was only as wide as one glyph, shrinking every glyph by ~1/(2*half).
    vec2 local = uGlyphB[i].zw + aCornerUnit * uGlyphE[i].xy;
    vLocal = local;
    vec2 px = uGlyphB[i].xy + aCornerUnit.x * uGlyphA[i].xy + aCornerUnit.y * uGlyphA[i].zw;
    gl_Position = uProj * vec4(px, 0.0, 1.0);
}
)GLSL";

const char* kSlugFrag = R"GLSL(
const int SLUG_MAX_CURVES_PER_BAND = 128;
const int SLUG_MAX_BATCH = 8;

in vec2 vLocal;
flat in int vIndex;
out vec4 fragColor;

uniform sampler2D uCurveTex;
uniform sampler2D uBandTex;
uniform vec4 uGlyphA[SLUG_MAX_BATCH];   // (basisX.x, basisX.y, basisY.x, basisY.y)
uniform vec4 uGlyphB[SLUG_MAX_BATCH];   // (origin.x, origin.y, emCentre.x, emCentre.y)
uniform vec4 uGlyphC[SLUG_MAX_BATCH];   // (firstCurve, curveCount, bandOffsetX, bandOffsetY)
uniform vec4 uGlyphG[SLUG_MAX_BATCH];   // (bandCount, minY, invSpan, 0)

uniform vec4 uColor;
uniform vec4 uInnerColor;
uniform vec4 uOuterColor;
uniform int  uUseGradient;
uniform vec4 uOutlineColor;
uniform float uOutlineWidth;   // em units (0 = off)
uniform float uAaWidth;        // analytic AA width in em units (== 1 pixel)
uniform float uDilation;       // em units; positive fattens the glyph
uniform float uSoftness;       // pixels (>= 0.25)

// Mirrors crossrender::QuadraticRoots() statement for statement.
int slugRoots(float qA, float qB, float qC, out float r0, out float r1) {
    if (abs(qA) > 1e-12) {
        float disc = qB * qB - 4.0 * qA * qC;
        if (disc < 0.0) return 0;
        float sq = sqrt(disc);
        r0 = (-qB - sq) / (2.0 * qA);
        r1 = (-qB + sq) / (2.0 * qA);
        return 2;
    }
    if (abs(qB) > 1e-20) {
        r0 = -qC / qB;
        return 1;
    }
    return 0;
}

// Same mapping as crossrender::SlugTextRenderer::PrepareGlyph's band assignment.
int slugBandIndex(float y, float minY, float invSpan, float bandCount) {
    float f = floor((y - minY) * invSpan * bandCount);
    return int(clamp(f, 0.0, bandCount - 1.0) + 0.5);
}

vec4 slugCurveA(int index) {
    ivec2 p = ivec2((index % 2) * 2, index / 2);
    return texelFetch(uCurveTex, p, 0);
}

vec4 slugCurveB(int index) {
    ivec2 p = ivec2((index % 2) * 2 + 1, index / 2);
    return texelFetch(uCurveTex, p, 0);
}

void main() {
    int i = vIndex;
    if (i >= SLUG_MAX_BATCH) i = SLUG_MAX_BATCH - 1;
    int firstCurve = int(uGlyphC[i].x + 0.5);
    int curveCount = int(uGlyphC[i].y + 0.5);
    int bandOffsetY = int(uGlyphC[i].w + 0.5);  // absolute first-band index
    float bandCount = uGlyphG[i].x;
    float minY = uGlyphG[i].y;
    float invSpan = uGlyphG[i].z;

    // vLocal is already relative to the pen origin, in em units.
    vec2 p = vLocal;

    // bandOffsetY is this glyph's first band's absolute index in the packed band
    // texture and bandOffsetX is unused: the absolute index alone determines the
    // texel (two bands per 4-wide row).
    int bandIndex = bandOffsetY + slugBandIndex(p.y, minY, invSpan, bandCount);
    int bFirst, bCount;
    {
        ivec2 bp = ivec2((bandIndex % 2) * 2, bandIndex / 2);
        vec4 b = texelFetch(uBandTex, bp, 0);
        bFirst = int(b.x + 0.5);
        bCount = int(b.y + 0.5);
    }

    float winding = 0.0;
    float edgeIn = 1e30;
    float edgeOut = 1e30;
    int n = min(bCount, SLUG_MAX_CURVES_PER_BAND);
    for (int c = 0; c < SLUG_MAX_CURVES_PER_BAND; ++c) {
        if (c >= n) break;
        int ci = bFirst + c;
        if (ci < firstCurve || ci >= firstCurve + curveCount) continue;
        vec4 a = slugCurveA(ci);
        vec4 b = slugCurveB(ci);
        vec2 p0 = a.xy;
        vec2 p1 = a.zw;
        vec2 p2 = b.xy;

        // Power-basis coefficients (mirrored from crossrender::AccumulateCurve):
        // q(t) = A t^2 + B t + p0 with A = p0 - 2 p1 + p2 and B = 2 (p1 - p0).
        float yA = p0.y - 2.0 * p1.y + p2.y;
        float yB = 2.0 * (p1.y - p0.y);
        float r0, r1;
        int nr = slugRoots(yA, yB, p0.y - p.y, r0, r1);
        float xA = p0.x - 2.0 * p1.x + p2.x;
        float xB = 2.0 * (p1.x - p0.x);
        // Standard winding ray test: cast in +x and count signed crossings.
        for (int k = 0; k < 2; ++k) {
            if (k >= nr) break;
            // Half-open [0, 1): see crossrender::AccumulateCurve for why.
            float t = (k == 0) ? r0 : r1;
            if (t < 0.0 || t >= 1.0) continue;
            float x = (xA * t + xB) * t + p0.x;
            if (x < p.x) continue;
            // y-down: dy/dt > 0 is a clockwise (positive) contribution.
            float dy = 2.0 * yA * t + yB;
            float w = (dy > 0.0) ? 1.0 : -1.0;
            winding += w;
            float dx = x - p.x;
            float len = sqrt(dx * dx);
            if (len > 1e-6) {
                float xt = 2.0 * xA * t + xB;
                float off = abs((dx * dy) / sqrt(xt * xt + dy * dy));
                if (w > 0.0) edgeIn = min(edgeIn, off);
                else edgeOut = min(edgeOut, off);
            }
        }
    }

    float dist = (abs(winding) > 0.5) ? edgeIn : -edgeOut;
    float sdf = dist - uDilation;
    float soft = max(uSoftness, 0.25) * uAaWidth;
    float fillCov = clamp(0.5 + sdf / soft, 0.0, 1.0);
    if (fillCov <= 0.0) discard;

    vec4 base = uColor;
    if (uUseGradient != 0) {
        float t = clamp((p.y - minY) * invSpan, 0.0, 1.0);
        base = mix(uInnerColor, uOuterColor, t);
    }

    if (uOutlineWidth > 0.0) {
        float outer = clamp(0.5 + (sdf + uOutlineWidth) / soft, 0.0, 1.0);
        float outCov = outer * (1.0 - fillCov);
        if (outer <= 0.0) discard;
        // Composite the stroke under the fill in premultiplied alpha.
        float aOut = uOutlineColor.a * outCov;
        float aFill = base.a * fillCov;
        vec3 rgb = base.rgb * aFill + uOutlineColor.rgb * aOut;
        float a = aFill + aOut;
        if (a <= 0.0) discard;
        fragColor = vec4(rgb, a);
        return;
    }
    fragColor = vec4(base.rgb * base.a, base.a) * fillCov;
}
)GLSL";

// ---------------------------------------------------------------------------
// Помощники для путей
// ---------------------------------------------------------------------------
void PathSample(const Vec2* path, const std::vector<f32>& lens, f32 total, f32 d, Vec2* pos, Vec2* tan) {
    const int count = static_cast<int>(lens.size());
    d = Clamp(d, 0.0f, total);
    int seg = count - 2;
    for (int i = 0; i < count - 1; ++i) {
        if (d <= lens[static_cast<usize>(i + 1)]) {
            seg = i;
            break;
        }
    }
    if (seg < 0) seg = 0;
    const f32 a = lens[static_cast<usize>(seg)];
    const f32 b = lens[static_cast<usize>(seg + 1)];
    const f32 local = b > a ? (d - a) / (b - a) : 0.0f;
    *pos = Lerp(path[seg], path[seg + 1], local);
    *tan = Normalize(path[seg + 1] - path[seg]);
}

// Разворачивает один замкнутый контур в квадратичные сегменты в em-единицах (y вниз).
// Контур может содержать цепочки подряд идущих off-curve точек; недостающая
// on-curve точка между двумя из них — их середина (стандартное правило TrueType).
void CollectContourCurves(const GlyphContour& contour, f32 inv, std::vector<SlugCurve>* out, f32* minX,
                          f32* minY, f32* maxX, f32* maxY) {
    const std::vector<GlyphPoint>& pts = contour.points;
    const usize n = pts.size();
    if (n < 2) return;

    auto at = [&](usize i) -> Vec2 { return pts[i].p * inv; };
    auto onCurve = [&](usize i) -> bool { return pts[i].onCurve != 0; };
    // Замкнутый контур: pt[n-1] обычно дублирует pt[0].
    const bool wrapped = n > 2 && pts[0].p == pts[n - 1].p;
    const usize m = wrapped ? n - 1 : n;
    if (m < 2) return;

    usize start = 0;
    for (usize i = 0; i < m; ++i) {
        if (onCurve(i)) {
            start = i;
            break;
        }
    }
    Vec2 pen;
    if (onCurve(start)) {
        pen = at(start);
    } else {
        // Все точки off-curve: первая неявная on-curve точка — середина между
        // последней и первой управляющими точками.
        pen = (at(m - 1) + at(0)) * 0.5f;
    }

    auto emit = [&](const Vec2& p0, const Vec2& p1, const Vec2& p2) {
        SlugCurve c;
        c.p0 = p0;
        c.p1 = p1;
        c.p2 = p2;
        out->push_back(c);
        const Vec2 pts3[3] = {p0, p1, p2};
        for (const Vec2& q : pts3) {
            *minX = MinT(*minX, q.x);
            *minY = MinT(*minY, q.y);
            *maxX = MaxT(*maxX, q.x);
            *maxY = MaxT(*maxY, q.y);
        }
    };

    for (usize k = 1; k <= m; ++k) {
        const usize i0 = (start + k) % m;
        const usize i1 = (start + k + 1) % m;
        if (onCurve(i0)) {
            emit(pen, pen, at(i0));  // прямой отрезок как вырожденная квадратичная кривая
            pen = at(i0);
            continue;
        }
        Vec2 ctrl = at(i0);
        Vec2 end;
        if (onCurve(i1)) {
            end = at(i1);
        } else {
            end = (ctrl + at(i1)) * 0.5f;
            --k;  // неявная on-curve точка синтезирована
        }
        emit(pen, ctrl, end);
        pen = end;
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// Эталонные покрытие / обход на CPU
// ---------------------------------------------------------------------------
f32 SlugTextRenderer::WindingAt(const std::vector<SlugCurve>& curves, Vec2 point) {
    f32 winding = 0.0f;
    f32 edgeIn = kNoEdge, edgeOut = kNoEdge;
    for (const SlugCurve& c : curves) AccumulateCurve(c, point, &winding, &edgeIn, &edgeOut);
    return winding;
}

f32 SlugTextRenderer::CoverageAt(const std::vector<SlugCurve>& curves, Vec2 point, f32 aaWidth) {
    f32 winding = 0.0f;
    f32 edgeIn = kNoEdge, edgeOut = kNoEdge;
    for (const SlugCurve& c : curves) AccumulateCurve(c, point, &winding, &edgeIn, &edgeOut);
    return CoverageFromWinding(winding, edgeIn, edgeOut, aaWidth);
}

// ---------------------------------------------------------------------------
// Impl
// ---------------------------------------------------------------------------
struct SlugTextRenderer::Impl {
    Shader shader;
    unsigned int vao = 0, vbo = 0;
    bool ready = false;
    int maxTextureSize = 16384;
    int curveCapacity = kInitialCurveCapacity;
    int bandCapacity = kInitialBandCapacity;
    int curveRows = 0, bandRows = 0;
    // Отражаются из владельца в Flush(); Upload() — вложенный класс-помощник и
    // не может напрямую читать члены объемлющего объекта.
    int curveCursor = 0, bandCursor = 0;
    bool curveBudgetWarned = false, bandBudgetWarned = false;
    // Промежуточные CPU-буферы (они же источник истины для дампов текстур,
    // используемых утилитами/тестами).
    std::vector<f32> curveData;
    std::vector<f32> bandData;

    bool BuildShader() {
        const std::string vs = std::string(builtin::Preamble()) + kSlugVert;
        const std::string fs = std::string(builtin::Preamble()) + kSlugFrag;
        if (!shader.Build(vs.c_str(), fs.c_str(), "crossrender::SlugText")) {
            ENG_LOGE("slug", "shader build failed:\n%s", shader.Log().c_str());
            return false;
        }
        return true;
    }

    bool InitGL() {
        if (ready) return true;
        if (!gl::glGenVertexArrays || !gl::glCreateShader || !gl::glGenBuffers) return false;
        if (gl::glGetIntegerv) {
            gl::GLint v = 0;
            gl::glGetIntegerv(gl::GL_MAX_TEXTURE_SIZE, &v);
            if (v > 0) {
                maxTextureSize = v;
                setMaxRows(v);
            }
        }
        if (!BuildShader()) return false;

        // Единичный квад: позиция в [-1,1]^2 и тот же угол в [0,1]^2.
        static const f32 kQuad[16] = {
            -1.0f, -1.0f, 0.0f, 1.0f,
             1.0f, -1.0f, 1.0f, 1.0f,
            -1.0f,  1.0f, 0.0f, 0.0f,
             1.0f,  1.0f, 1.0f, 0.0f,
        };
        gl::glGenVertexArrays(1, &vao);
        gl::glGenBuffers(1, &vbo);
        gl::glBindVertexArray(vao);
        gl::glBindBuffer(gl::GL_ARRAY_BUFFER, vbo);
        gl::glBufferData(gl::GL_ARRAY_BUFFER, sizeof(kQuad), kQuad, gl::GL_STATIC_DRAW);
        gl::glEnableVertexAttribArray(0);
        gl::glVertexAttribPointer(0, 2, gl::GL_FLOAT, gl::GL_FALSE, 4 * sizeof(f32),
                                  reinterpret_cast<const void*>(0));
        gl::glEnableVertexAttribArray(1);
        gl::glVertexAttribPointer(1, 2, gl::GL_FLOAT, gl::GL_FALSE, 4 * sizeof(f32),
                                  reinterpret_cast<const void*>(2 * sizeof(f32)));
        gl::glBindVertexArray(0);
        gl::glBindBuffer(gl::GL_ARRAY_BUFFER, 0);
        ready = true;
        return true;
    }

    // Ограничивает бюджет тем, что этот GPU реально может выделить. Вызывается
    // из InitGL (тестам доступно через capacities).
    void setMaxRows(int maxSize) {
        const int maxRows = maxSize > 0 ? maxSize : 16384;
        const int curveRowsCap = std::min(maxRows, kInitialCurveCapacity / 2);
        const int bandRowsCap = std::min(maxRows, kInitialBandCapacity / 2);
        curveCapacity = curveRowsCap * 2;
        bandCapacity = bandRowsCap * 2;
    }

    // (Пере)создаёт две текстуры данных, когда курсор их перерос, и загружает
    // использованный диапазон. Одна перезагрузка на Flush; кривые/полосы только
    // растут, поэтому dirty-флага — вся необходимая инвалидация.
    // Текстуры передаются явно: вложенный класс может называть приватные члены
    // объемлющего класса, но неявного `this` для них у него нет, поэтому без
    // объекта он не тронет curveTexture_/bandTexture_.
    void Upload(Texture* curveTex, Texture* bandTex) {
        curveRows = std::max(1, (curveCursor + 1) / 2);
        bandRows = std::max(1, (bandCursor + 1) / 2);
        // Никогда не запрашиваем больше строк, чем GPU может выделить; ограничение
        // ёмкости в setMaxRows() делает эту проверку дублирующей страховкой.
        if (maxTextureSize > 0) {
            curveRows = std::min(curveRows, maxTextureSize);
            bandRows = std::min(bandRows, maxTextureSize);
        }
        const usize curveFloats = static_cast<usize>(curveRows) * kCurveColumns * 4;
        const usize bandFloats = static_cast<usize>(bandRows) * kBandColumns * 4;
        if (curveData.size() < curveFloats) curveData.resize(curveFloats, 0.0f);
        if (bandData.size() < bandFloats) bandData.resize(bandFloats, 0.0f);
        if (!curveTex || !bandTex) return;

        const bool curveOk = curveTex->Valid() && curveTex->Width() == kCurveColumns &&
                             curveTex->Height() >= curveRows &&
                             curveTex->Format() == PixelFormat::RGBA32F;
        if (!curveOk) {
            curveTex->Destroy();
            if (!curveTex->Create(kCurveColumns, curveRows, PixelFormat::RGBA32F, nullptr,
                                  TextureFilter::Nearest, TextureWrap::ClampToEdge)) {
                ENG_LOGE("slug", "curve texture allocation failed (%dx%d)", kCurveColumns, curveRows);
                return;
            }
            curveTex->SetDebugName("slug-curves");
        }
        const bool bandOk = bandTex->Valid() && bandTex->Width() == kBandColumns &&
                            bandTex->Height() >= bandRows &&
                            bandTex->Format() == PixelFormat::RGBA32F;
        if (!bandOk) {
            bandTex->Destroy();
            if (!bandTex->Create(kBandColumns, bandRows, PixelFormat::RGBA32F, nullptr,
                                 TextureFilter::Nearest, TextureWrap::ClampToEdge)) {
                ENG_LOGE("slug", "band texture allocation failed (%dx%d)", kBandColumns, bandRows);
                return;
            }
            bandTex->SetDebugName("slug-bands");
        }
        if (curveTex->Valid()) curveTex->Update(curveData.data(), 0, 0, kCurveColumns, curveRows);
        if (bandTex->Valid()) bandTex->Update(bandData.data(), 0, 0, kBandColumns, bandRows);
    }

    void DestroyGL() {
        if (vbo && gl::glDeleteBuffers) gl::glDeleteBuffers(1, &vbo);
        if (vao && gl::glDeleteVertexArrays) gl::glDeleteVertexArrays(1, &vao);
        vbo = vao = 0;
        shader.Destroy();
        ready = false;
    }
};

// ---------------------------------------------------------------------------
// Время жизни
// ---------------------------------------------------------------------------
SlugTextRenderer::SlugTextRenderer() : impl_(new Impl()) {}
SlugTextRenderer::~SlugTextRenderer() { Shutdown(); }

bool SlugTextRenderer::Init(Font* font, int emResolution) {
    if (!font || !font->Valid()) {
        ENG_LOGE("slug", "Init: font is null or invalid");
        return false;
    }
    emResolution_ = emResolution < 8 ? 8 : emResolution;
    font_ = font;
    curves_.clear();
    bands_.clear();
    glyphs_.clear();
    glyphIndex_.clear();
    curveCursor_ = bandCursor_ = 0;
    // «Замороженный» заголовок резервирует прямоугольный курсор блока полос
    // (bandOffsetX_/bandOffsetY_); реализация выделяет полосы подряд и берёт
    // тексель из абсолютного индекса полосы, поэтому эти два поля остаются
    // неиспользуемыми (см. заметки о текстуре полос в начале файла).
    bandOffsetX_ = bandOffsetY_ = 0;
    dirty_ = false;
    stats_ = Stats{};
    impl_->curveData.clear();
    impl_->bandData.clear();
    if (!impl_->InitGL()) {
        // InitGL падает из-за отсутствия GL-контекста либо из-за не скомпилировавшегося
        // шейдера; BuildShader логирует вывод компилятора.
        ENG_LOGW("slug", "Init: GL setup or shader build failed; CPU reference path only");
        return false;
    }
    return true;
}

void SlugTextRenderer::Shutdown() {
    impl_->DestroyGL();
    curveTexture_.Destroy();
    bandTexture_.Destroy();
    font_ = nullptr;
    curves_.clear();
    bands_.clear();
    glyphs_.clear();
    glyphIndex_.clear();
    curveCursor_ = bandCursor_ = 0;
    dirty_ = false;
}

bool SlugTextRenderer::Valid() const { return font_ != nullptr && impl_->ready && impl_->shader.Valid(); }

// ---------------------------------------------------------------------------
// Подготовка глифов
// ---------------------------------------------------------------------------
bool SlugTextRenderer::PrepareGlyph(u32 codepoint) {
    if (!font_) return false;
    auto found = glyphIndex_.find(codepoint);
    if (found != glyphIndex_.end()) return !glyphs_[static_cast<usize>(found->second)].empty;

    GlyphOutline outline;
    if (!font_->GetGlyphOutline(codepoint, static_cast<f32>(emResolution_), &outline)) {
        ENG_LOGW("slug", "U+%04X has no outline", codepoint);
        return false;
    }

    SlugGlyph glyph;
    glyph.codepoint = codepoint;
    glyph.advance = outline.advance / static_cast<f32>(emResolution_);
    glyph.firstCurve = curveCursor_;
    glyph.firstBand = bandCursor_;

    // ---- сплющиваем контур в квадратичные кривые в em-единицах ----------
    std::vector<SlugCurve> local;
    f32 minX = 1e30f, minY = 1e30f, maxX = -1e30f, maxY = -1e30f;
    const f32 inv = 1.0f / static_cast<f32>(emResolution_);
    for (const GlyphContour& contour : outline.contours)
        CollectContourCurves(contour, inv, &local, &minX, &minY, &maxX, &maxY);

    if (local.empty()) {
        // Пустой глиф (пробел, перевод строки): регистрируется с флагом empty,
        // чтобы Measure()/Draw() продвигались без повторного запроса к шрифту.
        glyph.empty = true;
        glyph.curveCount = 0;
        glyph.bandCount = 0;
        glyphIndex_[codepoint] = static_cast<int>(glyphs_.size());
        glyphs_.push_back(glyph);
        return false;
    }

    glyph.bounds = Rect{minX, minY, std::max(maxX - minX, 1e-3f), std::max(maxY - minY, 1e-3f)};
    glyph.empty = false;

    if (curveCursor_ + static_cast<int>(local.size()) > impl_->curveCapacity) {
        if (!impl_->curveBudgetWarned) {
            ENG_LOGW("slug", "curve budget exhausted (%d curves); later glyphs are skipped",
                     impl_->curveCapacity);
            impl_->curveBudgetWarned = true;
        }
        glyph.empty = true;
        glyph.curveCount = 0;
        glyph.bandCount = 0;
        glyphIndex_[codepoint] = static_cast<int>(glyphs_.size());
        glyphs_.push_back(glyph);
        return false;
    }

    // ---- полосы ----------------------------------------------------------
    // Полосы выделяются подряд в глобальном массиве полос и пакуются по две
    // в строку текстуры. Начало блока глифа — это просто абсолютный индекс его
    // первой полосы: bandOffsetY хранит этот индекс (не строку текстуры), а
    // bandOffsetX не используется, потому что шейдер выводит и столбец текселя,
    // и строку из абсолютного индекса. Ранняя ревизия давала каждому глифу
    // прямоугольный блок со служебной полосой на каждом конце, из-за чего
    // каждый поиск сдвигался на одну полосу и шейдер читал служебную.
    const int requiredBands = static_cast<int>((local.size() + kMaxCurvesPerBand - 1) /
                                                kMaxCurvesPerBand);
    const int bandCount = Clamp(requiredBands, kMinBandCount, kMaxBandCount);
    const int bandBase = bandCursor_;
    if (bandCursor_ + bandCount > impl_->bandCapacity) {
        if (!impl_->bandBudgetWarned) {
            ENG_LOGW("slug", "band budget exhausted (%d bands); later glyphs are skipped",
                     impl_->bandCapacity);
            impl_->bandBudgetWarned = true;
        }
        glyph.empty = true;
        glyph.curveCount = 0;
        glyph.bandCount = 0;
        glyphIndex_[codepoint] = static_cast<int>(glyphs_.size());
        glyphs_.push_back(glyph);
        return false;
    }

    glyph.bandCount = bandCount;
    glyph.bandOffsetX = 0;
    glyph.bandOffsetY = static_cast<u32>(bandBase);

    const f32 span = maxY - minY;
    const f32 invSpan = span > 1e-6f ? 1.0f / span : 1.0f / 1e-6f;

    // Для каждой полосы запоминаем непрерывную последовательность кривых с любым
    // охватом по y. В неё могут входить кривые, лишь пересекающие полосу, — в
    // этом смысл структуры ускорения. Индекс полосы, который ищет фрагмент,
    // совпадает со значением PrepareGlyph здесь, поэтому оба обязаны применять
    // это одинаковое отображение (см. slugBandIndex в GLSL).
    std::vector<u32> firstOf(static_cast<usize>(bandCount), 0);
    std::vector<u32> countOf(static_cast<usize>(bandCount), 0);
    for (int b = 0; b < bandCount; ++b) {
        const int localBand = b;
        const f32 y0 = minY + span * static_cast<f32>(localBand) / static_cast<f32>(bandCount);
        const f32 y1 = minY + span * static_cast<f32>(localBand + 1) / static_cast<f32>(bandCount);
        int first = -1, last = -1;
        for (usize ci = 0; ci < local.size(); ++ci) {
            const SlugCurve& c = local[ci];
            const f32 cmin = MinT(c.p0.y, MinT(c.p1.y, c.p2.y));
            const f32 cmax = MaxT(c.p0.y, MaxT(c.p1.y, c.p2.y));
            if (cmax < y0 || cmin > y1) continue;
            if (first < 0) first = static_cast<int>(ci);
            last = static_cast<int>(ci);
        }
        if (first < 0) continue;
        firstOf[static_cast<usize>(b)] = static_cast<u32>(first);
        countOf[static_cast<usize>(b)] = static_cast<u32>(last - first + 1);
    }

    // Держим каждую последовательность в пределах константной границы цикла
    // шейдера. Полоса сверх лимита расширяет последовательность *следующей*
    // полосы, чтобы поглотить её; сама полоса опустевает, но её пиксели всё
    // равно попадают на кривые, потому что сосед, владеющий теперь полным
    // охватом, покрывает тот же интервал y. Итерация с первой полосы в сторону
    // последней каскадирует overflow к концу глифа; clamping последней полосы
    // оставляет потерю на менее заметном краю.
    if (bandCount > 1) {
        for (int b = 0; b < bandCount - 1; ++b) {
            if (static_cast<int>(countOf[static_cast<usize>(b)]) <= kMaxCurvesPerBand) continue;
            const int lo = static_cast<int>(firstOf[static_cast<usize>(b)]);
            const int hi = lo + static_cast<int>(countOf[static_cast<usize>(b)]) - 1;
            const int nb = b + 1;
            int nlo = static_cast<int>(firstOf[static_cast<usize>(nb)]);
            if (static_cast<int>(countOf[static_cast<usize>(nb)]) == 0) nlo = lo;
            const int nhi = nlo + static_cast<int>(countOf[static_cast<usize>(nb)]) - 1;
            const int mlo = MinT(lo, nlo);
            const int mhi = MaxT(hi, nhi);
            firstOf[static_cast<usize>(nb)] = static_cast<u32>(mlo);
            countOf[static_cast<usize>(nb)] = static_cast<u32>(mhi - mlo + 1);
            countOf[static_cast<usize>(b)] = 0;
            ENG_LOGW("slug", "band %d of U+%04X needs %d curves (max %d); merged into band %d",
                     b, codepoint, hi - lo + 1, kMaxCurvesPerBand, nb);
        }
    }
    // Проход слияния может оставить объединение, всё ещё превышающее лимит
    // (слияние в уже обработанную полосу), а у последней полосы нет преемника.
    // Проходим ещё раз, чтобы константная граница цикла шейдера была
    // гарантирована: ограничение отбрасывает хвост последовательности этой
    // полосы — бывает лишь у патологически плотного глифа и всегда логируется.
    for (int b = 0; b < bandCount; ++b) {
        if (static_cast<int>(countOf[static_cast<usize>(b)]) <= kMaxCurvesPerBand) continue;
        ENG_LOGW("slug", "band %d of U+%04X needs %d curves (max %d); clamped",
                 b, codepoint, static_cast<int>(countOf[static_cast<usize>(b)]), kMaxCurvesPerBand);
        countOf[static_cast<usize>(b)] = static_cast<u32>(kMaxCurvesPerBand);
    }

    // ---- фиксация ---------------------------------------------------------
    for (const SlugCurve& c : local) curves_.push_back(c);
    for (int b = 0; b < bandCount; ++b) {
        bands_.push_back(static_cast<u32>(glyph.firstCurve) + firstOf[static_cast<usize>(b)]);
        bands_.push_back(countOf[static_cast<usize>(b)]);
    }
    glyph.curveCount = static_cast<int>(local.size());
    const int newCurveCursor = curveCursor_ + glyph.curveCount;
    bandCursor_ += bandCount;

    // ---- промежуточные массивы (что Upload() отправляет на GPU) ----------------
    {
        const int rows = (newCurveCursor + 1) / 2;
        if (static_cast<int>(impl_->curveData.size()) < rows * kCurveColumns * 4)
            impl_->curveData.resize(static_cast<usize>(rows) * kCurveColumns * 4, 0.0f);
        for (int ci = glyph.firstCurve; ci < newCurveCursor; ++ci) {
            const SlugCurve& c = curves_[static_cast<usize>(ci)];
            const usize row = static_cast<usize>(ci / 2);
            const usize x = static_cast<usize>((ci % 2) * 2);
            f32* t0 = &impl_->curveData[(row * kCurveColumns + x) * 4];
            t0[0] = c.p0.x; t0[1] = c.p0.y; t0[2] = c.p1.x; t0[3] = c.p1.y;
            f32* t1 = &impl_->curveData[(row * kCurveColumns + x + 1) * 4];
            t1[0] = c.p2.x; t1[1] = c.p2.y;
            // Абсолютный индекс полосы; шейдеру он не нужен, но делает дамп на
            // стороне CPU самодостаточным для утилит и тестов.
            t1[2] = static_cast<f32>(glyph.firstBand);
            t1[3] = 0.0f;
        }
        for (int b = 0; b < bandCount; ++b) {
            const usize idx = static_cast<usize>(bandBase + b);
            const usize row = idx / 2;
            const usize x = (idx % 2) * 2;
            if (impl_->bandData.size() < (row + 1) * kBandColumns * 4)
                impl_->bandData.resize((row + 1) * kBandColumns * 4, 0.0f);
            f32* t = &impl_->bandData[(row * kBandColumns + x) * 4];
            t[0] = static_cast<f32>(bands_[static_cast<usize>(bandBase + b) * 2 + 0]);
            t[1] = static_cast<f32>(bands_[static_cast<usize>(bandBase + b) * 2 + 1]);
            t[2] = 0.0f;
            t[3] = 0.0f;
        }
    }

    curveCursor_ = newCurveCursor;
    (void)invSpan;
    glyphIndex_[codepoint] = static_cast<int>(glyphs_.size());
    glyphs_.push_back(glyph);
    dirty_ = true;
    return true;
}

const SlugGlyph* SlugTextRenderer::GetGlyph(u32 codepoint) const {
    auto it = glyphIndex_.find(codepoint);
    if (it == glyphIndex_.end()) return nullptr;
    return &glyphs_[static_cast<usize>(it->second)];
}

// ---------------------------------------------------------------------------
// Upload
// ---------------------------------------------------------------------------
void SlugTextRenderer::Flush() {
    if (!dirty_) return;
    dirty_ = false;
    if (!impl_->ready || curves_.empty() || bands_.empty()) return;
    impl_->curveCursor = curveCursor_;
    impl_->bandCursor = bandCursor_;
    const auto t0 = std::chrono::steady_clock::now();
    impl_->Upload(&curveTexture_, &bandTexture_);
    stats_.uploadMs = std::chrono::duration<f32, std::milli>(
                          std::chrono::steady_clock::now() - t0)
                          .count();
}

// ---------------------------------------------------------------------------
// Измерение
// ---------------------------------------------------------------------------
f32 SlugTextRenderer::Measure(const std::string& utf8, f32 letterSpacing, f32 size) {
    if (utf8.empty()) return 0.0f;
    const std::vector<u32> cps = Utf8ToCodepoints(utf8);
    const f32 emInv = 1.0f / static_cast<f32>(emResolution_);
    f32 total = 0.0f;
    u32 prev = 0;
    bool first = true;
    for (u32 cp : cps) {
        const SlugGlyph* g = GetGlyph(cp);
        if (!g) {
            PrepareGlyph(cp);
            g = GetGlyph(cp);
        }
        if (!first) total += font_->GetKerning(prev, cp) * emInv;
        first = false;
        if (g) total += g->advance + letterSpacing;
        prev = cp;
    }
    if (total < 0.0f) total = 0.0f;
    return size > 0.0f ? total * size : total;
}

// ---------------------------------------------------------------------------
// Рисование
// ---------------------------------------------------------------------------
namespace {

struct GlyphDraw {
    const SlugGlyph* glyph = nullptr;
    u32 codepoint = 0;
    Vec2 origin{0, 0};   // позиция пера, переведённая в логические пиксели
    f32 rotation = 0.0f;
    Vec2 scale{1, 1};
    f32 pixelSize = 1.0f;  // масштаб em -> логический пиксель для этого прохода
};

bool SameBasis(const GlyphDraw& a, const GlyphDraw& b) {
    return std::fabs(a.rotation - b.rotation) < 1e-6f && std::fabs(a.scale.x - b.scale.x) < 1e-6f &&
           std::fabs(a.scale.y - b.scale.y) < 1e-6f;
}

// Выполняет один instanced-вызов для серии глифов с общими rotation/scale.
struct SlugDrawPass {
    SlugTextRenderer::Impl* im = nullptr;
    const Texture* curve = nullptr;
    const Texture* band = nullptr;
    SlugTextRenderer::Stats* stats = nullptr;
    bool gradient = false;

    void Submit(const GlyphDraw* draws, int count, const SlugTextStyle& st, f32 softEm) {
        if (count <= 0 || !im || count > kMaxBatchSize) return;
        Vec4 a[kMaxBatchSize], b[kMaxBatchSize], c[kMaxBatchSize], g[kMaxBatchSize];
        Vec4 e[kMaxBatchSize];
        for (int i = 0; i < count; ++i) {
            const SlugGlyph& gph = *draws[i].glyph;
            const f32 pad = std::max(st.outlineWidth, 0.0f) + softEm * 3.0f;
            const f32 span = gph.bounds.h > 1e-6f ? gph.bounds.h : 1e-6f;
            // Полуразмеры padded-бокса в локальном (немасштабированном em) пространстве.
            const f32 hx = gph.bounds.w * 0.5f + pad;
            const f32 hy = gph.bounds.h * 0.5f + pad;
            const f32 rot = draws[i].rotation;
            const f32 cs = std::cos(rot), sn = std::sin(rot);
            const f32 sx = draws[i].scale.x, sy = draws[i].scale.y;
            // Базисные векторы экранного пространства: em x -> (cos*sx, sin*sx) * pixelSize,
            // em y -> (-sin*sy, cos*sy) * pixelSize.
            const Vec2 ex{cs * sx * draws[i].pixelSize, sn * sx * draws[i].pixelSize};
            const Vec2 ey{-sn * sy * draws[i].pixelSize, cs * sy * draws[i].pixelSize};
            const Vec2 emCentre{gph.bounds.x + gph.bounds.w * 0.5f, gph.bounds.y + gph.bounds.h * 0.5f};
            a[i] = Vec4{ex.x * hx, ex.y * hx, ey.x * hy, ey.y * hy};
            b[i] = Vec4{draws[i].origin.x + emCentre.x * ex.x + emCentre.y * ey.x,
                        draws[i].origin.y + emCentre.x * ex.y + emCentre.y * ey.y,
                        emCentre.x, emCentre.y};
            c[i] = Vec4{static_cast<f32>(gph.firstCurve), static_cast<f32>(gph.curveCount),
                        static_cast<f32>(gph.bandOffsetX), static_cast<f32>(gph.bandOffsetY)};
            g[i] = Vec4{static_cast<f32>(gph.bandCount), gph.bounds.y, 1.0f / span, 0.0f};
            e[i] = Vec4{hx, hy, 0.0f, 0.0f};
        }
        // Дозаполняем массивы: неустановленный элемент uniform-массива иначе
        // содержал бы то, что осталось от предыдущего вызова отрисовки.
        for (int i = count; i < kMaxBatchSize; ++i)
            a[i] = b[i] = c[i] = g[i] = e[i] = Vec4{0, 0, 0, 0};
        im->shader.Set("uGlyphA", std::vector<Vec4>(a, a + kMaxBatchSize));
        im->shader.Set("uGlyphB", std::vector<Vec4>(b, b + kMaxBatchSize));
        im->shader.Set("uGlyphC", std::vector<Vec4>(c, c + kMaxBatchSize));
        im->shader.Set("uGlyphG", std::vector<Vec4>(g, g + kMaxBatchSize));
        im->shader.Set("uGlyphE", std::vector<Vec4>(e, e + kMaxBatchSize));
        im->shader.Set("uColor", st.color);
        im->shader.Set("uUseGradient", gradient ? 1 : 0);
        im->shader.Set("uInnerColor", st.color);
        im->shader.Set("uOuterColor", st.color);
        im->shader.Set("uOutlineWidth", st.outlineWidth);
        im->shader.Set("uOutlineColor", st.outlineColor);
        gl::glDrawArraysInstanced(gl::GL_TRIANGLE_STRIP, 0, 4, count);
        stats->quads += count;
        stats->glyphsDrawn += count;
        // Каждая кривая каждого отправленного глифа находится в текстуре полос
        // и потому достижима фрагментным шейдером. Точное число вычислений GPU
        // зависит от того, сколько фрагментов попало в каждую полосу, чего CPU
        // знать не может, поэтому здесь отчёт о сделанных доступными кривых.
        for (int i = 0; i < count; ++i) stats->curvesEvaluated += draws[i].glyph->curveCount;
    }
};

}  // namespace

f32 SlugTextRenderer::Draw(Renderer2D& r2d, const std::string& utf8, Vec2 baseline, f32 pixelSize,
                           const SlugTextStyle& style) {
    if (utf8.empty() || font_ == nullptr || pixelSize <= 0.0f) return 0.0f;
    const std::vector<u32> cps = Utf8ToCodepoints(utf8);
    const f32 emInv = 1.0f / static_cast<f32>(emResolution_);
    const f32 sx = style.scale.x != 0.0f ? style.scale.x : 1.0f;
    const f32 sy = style.scale.y != 0.0f ? style.scale.y : 1.0f;

    // Позиция центра глифа в его собственном em-боксе до поворота.
    std::vector<GlyphDraw> list;
    list.reserve(cps.size());
    f32 pen = 0.0f, penY = 0.0f;
    u32 prev = 0;
    bool first = true;
    for (u32 cp : cps) {
        const SlugGlyph* g = GetGlyph(cp);
        if (!g) {
            PrepareGlyph(cp);
            g = GetGlyph(cp);
        }
        if (!first) pen += font_->GetKerning(prev, cp) * emInv;
        first = false;
        if (!g) {
            prev = cp;
            continue;
        }
        if (cp == '\n') {
            pen = 0.0f;
            penY += font_->LineHeight() * emInv;
            prev = cp;
            continue;
        }
        if (!g->empty) {
            GlyphDraw gd;
            gd.glyph = g;
            gd.codepoint = cp;
            // gd.origin — позиция ПЕРА в логических пикселях. Submit() добавляет
            // собственный центр глифа в em-пространстве через базисные векторы,
            // поэтому центр НЕ нужно учитывать здесь ещё раз (иначе каждый глиф
            // сдвигался на смещение своего центра и обрезался).
            const f32 twistX = pen + g->advance * 0.5f;
            const f32 lx = pen * sx * pixelSize;
            const f32 ly = penY * sy * pixelSize;
            gd.rotation = style.rotation + style.twist * twistX;
            gd.scale = {sx, sy};
            gd.pixelSize = pixelSize;
            gd.origin = baseline + Rotate(Vec2{lx, ly}, gd.rotation);
            list.push_back(gd);
        }
        pen += g->advance;
        prev = cp;
    }

    const f32 advance = pen;
    // PrepareGlyph may reallocate the glyph storage while the run is built;
    // resolve pointers only after all glyphs have been prepared.
    for (GlyphDraw& gd : list) gd.glyph = GetGlyph(gd.codepoint);
    Flush();
    if (list.empty() || !Valid()) return advance * pixelSize;

    Impl& im = *impl_;
    const f32 emPixels = pixelSize;
    const f32 aaWidth = 1.0f / emPixels;  // один логический пиксель в em-единицах

    // ---- сохраняем трогаемое состояние GL -------------------------------------
    // gl::GL.h не раскрывает перечисления для запроса привязок, поэтому этот
    // проход восстанавливает состояние привязкой известных значений по умолчанию
    // (program 0, VAO 0, unit 0, без текстуры), а не запросом. Flush() у
    // Renderer2D перепривязывает свои program, VAO, текстуру и blend-состояние
    // при каждом вызове отрисовки, поэтому сброс к ним между пакетами безопасен.
    const gl::GLboolean blendWasOn = gl::glIsEnabled ? gl::glIsEnabled(gl::GL_BLEND) : 1;
    const gl::GLboolean cullWasOn = gl::glIsEnabled ? gl::glIsEnabled(gl::GL_CULL_FACE) : 0;
    const gl::GLboolean scissorWasOn = gl::glIsEnabled ? gl::glIsEnabled(gl::GL_SCISSOR_TEST) : 0;

    gl::glDisable(gl::GL_CULL_FACE);
    gl::glDisable(gl::GL_SCISSOR_TEST);
    gl::glEnable(gl::GL_BLEND);
    gl::glBlendEquation(gl::GL_FUNC_ADD);
    gl::glBlendFunc(gl::GL_ONE, gl::GL_ONE_MINUS_SRC_ALPHA);

    im.shader.Bind();
    if (gl::glActiveTexture) gl::glActiveTexture(gl::GL_TEXTURE0);
    gl::glBindTexture(gl::GL_TEXTURE_2D, curveTexture_.Id());
    im.shader.Set("uCurveTex", 0);
    if (gl::glActiveTexture) gl::glActiveTexture(gl::GL_TEXTURE1);
    gl::glBindTexture(gl::GL_TEXTURE_2D, bandTexture_.Id());
    im.shader.Set("uBandTex", 1);
    if (gl::glActiveTexture) gl::glActiveTexture(gl::GL_TEXTURE0);

    im.shader.Set("uProj", r2d.Projection());
    im.shader.Set("uAaWidth", aaWidth);
    im.shader.Set("uSoftness", std::max(style.softness, 0.25f));
    im.shader.Set("uDilation", style.dilation);
    gl::glBindVertexArray(im.vao);

    SlugDrawPass pass;
    pass.im = &im;
    pass.stats = &stats_;
    pass.gradient = !(style.color == Color::White);
    const f32 softEm = std::max(style.softness, 0.25f) * aaWidth;

    // Серии глифов с общими rotation/scale пакетируются вместе; всё остальное
    // (twist, раскладка по пути/дуге) деградирует до одного вызова на глиф.
    const int n = static_cast<int>(list.size());
    auto drawRun = [&](const std::vector<GlyphDraw>& src, const SlugTextStyle& st) {
        int i = 0;
        while (i < n) {
            int k = i + 1;
            while (k < n && k - i < kMaxBatchSize && SameBasis(src[static_cast<usize>(i)],
                                                               src[static_cast<usize>(k)]))
                ++k;
            pass.Submit(&src[static_cast<usize>(i)], k - i, st, softEm);
            i = k;
        }
    };

    if (style.shadowOffset.x != 0.0f || style.shadowOffset.y != 0.0f) {
        // Тень: сначала рисуются смещённые копии. `softness > 1` добавляет до
        // четырёх дополнительных дрожащих выборок (дешёвая аппроксимация блюра).
        int taps = style.softness > 1.0f ? 1 + std::min(4, static_cast<int>(style.softness)) : 1;
        const Vec2 baseOffset{style.shadowOffset.x * pixelSize, style.shadowOffset.y * pixelSize};
        for (int t = 0; t < taps; ++t) {
            const f32 ang = 2.39996323f * static_cast<f32>(t);
            const f32 rad = taps > 1 ? (style.softness * 0.5f) * static_cast<f32>(t) /
                                           static_cast<f32>(taps - 1)
                                     : 0.0f;
            const Vec2 jitter{std::cos(ang) * rad, std::sin(ang) * rad};
            SlugTextStyle s = style;
            s.color = style.shadowColor;
            s.color.a = style.shadowColor.a / static_cast<f32>(taps);
            s.outlineWidth = 0.0f;
            std::vector<GlyphDraw> shifted(list);
            for (GlyphDraw& g : shifted) g.origin += baseOffset + jitter * pixelSize;
            drawRun(shifted, s);
        }
    }
    drawRun(list, style);

    // ---- восстановление ---------------------------------------------------------
    gl::glBindVertexArray(0);
    gl::glUseProgram(0);
    if (gl::glActiveTexture) gl::glActiveTexture(gl::GL_TEXTURE1);
    gl::glBindTexture(gl::GL_TEXTURE_2D, 0);
    if (gl::glActiveTexture) gl::glActiveTexture(gl::GL_TEXTURE0);
    gl::glBindTexture(gl::GL_TEXTURE_2D, 0);
    if (cullWasOn) gl::glEnable(gl::GL_CULL_FACE); else gl::glDisable(gl::GL_CULL_FACE);
    if (scissorWasOn) gl::glEnable(gl::GL_SCISSOR_TEST); else gl::glDisable(gl::GL_SCISSOR_TEST);
    if (!blendWasOn) gl::glDisable(gl::GL_BLEND);

    return advance * pixelSize;
}

// ---------------------------------------------------------------------------
// Раскладка по пути / дуге
// ---------------------------------------------------------------------------
f32 SlugTextRenderer::DrawOnPath(Renderer2D& r2d, const std::string& utf8, const Vec2* path,
                                 int pathCount, f32 pixelSize, const SlugTextStyle& style) {
    if (!path || pathCount < 2 || utf8.empty()) return 0.0f;
    std::vector<f32> lens(static_cast<usize>(pathCount), 0.0f);
    for (int i = 1; i < pathCount; ++i)
        lens[static_cast<usize>(i)] = lens[static_cast<usize>(i - 1)] + Length(path[i] - path[i - 1]);
    const f32 total = lens[static_cast<usize>(pathCount - 1)];
    if (total <= 1e-4f) return 0.0f;

    const std::vector<u32> cps = Utf8ToCodepoints(utf8);
    f32 pen = 0.0f;
    for (u32 cp : cps) {
        const SlugGlyph* g = GetGlyph(cp);
        if (!g) {
            PrepareGlyph(cp);
            g = GetGlyph(cp);
        }
        if (!g || g->empty) {
            if (g) pen += g->advance;
            continue;
        }
        Vec2 pos{0, 0}, tan{1, 0};
        PathSample(path, lens, total, pen * pixelSize + g->advance * pixelSize * 0.5f, &pos, &tan);
        SlugTextStyle s = style;
        // Ось y-вниз глифа уже является внутренней нормалью пути, как только
        // глиф повёрнут на угол касательной, поэтому базовая линия лежит на
        // полилинии без дополнительного смещения.
        s.rotation = std::atan2(tan.y, tan.x) + style.rotation;
        s.twist = 0.0f;
        Draw(r2d, CodepointsToUtf8({cp}), pos, pixelSize, s);
        pen += g->advance;
    }
    return pen * pixelSize;
}

f32 SlugTextRenderer::DrawOnArc(Renderer2D& r2d, const std::string& utf8, Vec2 center, f32 radius,
                                f32 startAngle, f32 pixelSize, const SlugTextStyle& style,
                                bool outside) {
    if (radius <= 1e-4f || utf8.empty() || pixelSize <= 0.0f) return 0.0f;
    // В контракте нет флага `clockwise`, поэтому дуга всегда проходится в
    // сторону роста угла (против часовой в пространстве y вниз), а `outside`
    // выбирает, по какую сторону окружности сидят глифы.
    const std::vector<u32> cps = Utf8ToCodepoints(utf8);
    f32 pen = 0.0f;
    for (u32 cp : cps) {
        const SlugGlyph* g = GetGlyph(cp);
        if (!g) {
            PrepareGlyph(cp);
            g = GetGlyph(cp);
        }
        if (!g || g->empty) {
            if (g) pen += g->advance;
            continue;
        }
        const f32 midEm = pen + g->advance * 0.5f;
        const f32 angle = startAngle + (midEm * pixelSize) / radius;
        const Vec2 tang{-std::sin(angle), std::cos(angle)};
        // Поворот на угол касательной направляет ось +y глифа по внутренней
        // нормали; `outside` разворачивает её на внешнюю нормаль.
        const Vec2 pos{center.x + std::cos(angle) * radius, center.y + std::sin(angle) * radius};
        SlugTextStyle s = style;
        s.rotation = std::atan2(tang.y, tang.x) + style.rotation + (outside ? 0.0f : kPi);
        s.twist = 0.0f;
        Draw(r2d, CodepointsToUtf8({cp}), pos, pixelSize, s);
        pen += g->advance;
    }
    return pen * pixelSize;
}

}  // namespace crossrender
