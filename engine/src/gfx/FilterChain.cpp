// ===========================================================================
// Наслаиваемая цепочка фильтров постобработки.
//
// Цепочка владеет двумя ping-pong render target'ами, целью истории (используется
// Feedback) и одной scratch-целью для сепарабельного размытия. Каждый фильтр,
// кроме сепарабельных размытий, исполняется единственным "uber"-фрагментным
// шейдером, выбирающим эффект через `uniform int uFilter`; Blur/GaussianBlur
// идут через отдельный сепарабельный шейдер (сначала горизонталь, потом
// вертикаль), чтобы большие радиусы оставались дешёвыми. Здесь ничего не
// трогает цепочку bloom/tonemap PostProcessor из RenderTarget.cpp.
//
// ---------------------------------------------------------------------------
// Справочник полей FilterParams (каждый фильтр читает только нужные ему поля)
// ---------------------------------------------------------------------------
//   amount            Общая сила / доля смешения в [0..1], если не оговорено.
//                     Масштаб эффекта для Bloom, Sharpen, EdgeDetect, Emboss,
//                     Posterize, Halftone, Dither, Scanlines, FilmGrain,
//                     ChromaticAberration, WaveDistort, Glitch, Fisheye, Swirl,
//                     Bleed, Feedback; дистанция размытия для RadialBlur /
//                     ZoomBlur / MotionBlur (в единицах UV).
//   radius            Радиус размытия в пикселях: Blur, GaussianBlur, Bloom,
//                     Sharpen (unsharp-радиус), Bleed.
//   sigma             Стандартное отклонение Гаусса в пикселях: GaussianBlur
//                     (а также Bloom/Sharpen при гауссовом ядре).
//   center            Нормированное (0..1) начало радиального эффекта:
//                     RadialBlur, ZoomBlur, Vignette, ChromaticAberration,
//                     BarrelDistort, Fisheye, Swirl, Kaleidoscope, Crt.
//   direction         Нормированная 2D-ось: MotionBlur, Emboss.
//   angle             Радианы: HueShift (поворот тона), Kaleidoscope (поворот
//                     сектора), Swirl (дополнительное закручивание).
//   frequency         Частота синуса для WaveDistort (периоды на изображение).
//   amplitude         Амплитуда искажения в единицах UV: WaveDistort.
//   threshold         Порог по яркости в [0..1]: Threshold.
//   levels            Число шагов квантования >= 2: Posterize, Dither.
//   cellSize          Размер блока/ячейки в пикселях >= 1: Pixelate, Halftone.
//   aspect            Дополнительный множитель аспекта для круглых искажений
//                     (1 = использовать собственный аспект цели): Vignette,
//                     BarrelDistort, Fisheye.
//   tint              Множитель RGB: выходной оттенок ColorGrade, тон Sepia.
//   lift              Прибавляется к цвету до гаммы: ColorGrade.
//   gain              Умножается на цвет до гаммы: ColorGrade.
//   gamma             Показатель степени, применяется как pow(c, 1/gamma): ColorGrade.
//   saturation        1 = без изменений, 0 = градации серого: ColorGrade.
//   temperature       Сдвиг в тёплую (+) / холодную (-) сторону: ColorGrade.
//   vignette          Сила затемнения виньетки: Vignette, Crt (неявно).
//   grain             Амплитуда зерна плёнки: FilmGrain.
//   scanlineStrength  Затемнение строк развёртки в [0..1]: Scanlines, Crt.
//   curvature         Баррельная кривизна кинескопа: Crt.
//   aberration        Расщепление RGB в единицах UV: ChromaticAberration, Crt,
//                     Glitch (в Glitch масштабируется через `amount`).
//   bloomThreshold    Порог bright-pass, когда useThreshold истинно: Bloom.
//   useThreshold      false = каждый нечёрный пиксель вносит вклад в Bloom.
//   feedback          Доля предыдущего кадра, подмешиваемая в текущий: Feedback.
//   segments          Число зеркальных секторов >= 3: Kaleidoscope.
//
// Задокументированные приближения:
//   * Bloom — однопроходный 13-tap crossrender, а не цепочка mip-уровней.
//   * Общие случаи Blur/GaussianBlur внутри uber-шейдера — 13-tap
//     приближения; реальную работу делает сепарабельный шейдер размытия.
//   * Bleed — дешёвое edge-aware размазывание по 8 направлениям (соседи с
//     близкой яркостью весят больше), как и описано в заголовке.
//   * EdgeDetect / Emboss / Dither работают в разрешении цели; без истории.
// ===========================================================================
#include "crossrender/gfx/FilterChain.h"

#include "crossrender/gfx/GL.h"
#include "crossrender/core/Log.h"
#include "crossrender/core/Time.h"
#include "crossrender/gfx/Shader.h"
#include "crossrender/gfx/Texture.h"
#include "crossrender/gfx/RenderTarget.h"

#include <cmath>
#include <cstring>
#include <utility>
#include <algorithm>

namespace crossrender {

// Uber-шейдер выбирает эффекты по числовому значению FilterType, поэтому id в
// GLSL-цепочке `uFilter` должны идти в ногу с этим перечислением.
static_assert(static_cast<int>(FilterType::Bloom) == 0 &&
                  static_cast<int>(FilterType::Blur) == 1 &&
                  static_cast<int>(FilterType::GaussianBlur) == 2 &&
                  static_cast<int>(FilterType::RadialBlur) == 3 &&
                  static_cast<int>(FilterType::ZoomBlur) == 4 &&
                  static_cast<int>(FilterType::MotionBlur) == 5 &&
                  static_cast<int>(FilterType::Sharpen) == 6 &&
                  static_cast<int>(FilterType::EdgeDetect) == 7 &&
                  static_cast<int>(FilterType::Emboss) == 8 &&
                  static_cast<int>(FilterType::Pixelate) == 9 &&
                  static_cast<int>(FilterType::Posterize) == 10 &&
                  static_cast<int>(FilterType::Halftone) == 11 &&
                  static_cast<int>(FilterType::Dither) == 12 &&
                  static_cast<int>(FilterType::Scanlines) == 13 &&
                  static_cast<int>(FilterType::Crt) == 14 &&
                  static_cast<int>(FilterType::Vignette) == 15 &&
                  static_cast<int>(FilterType::FilmGrain) == 16 &&
                  static_cast<int>(FilterType::ChromaticAberration) == 17 &&
                  static_cast<int>(FilterType::BarrelDistort) == 18 &&
                  static_cast<int>(FilterType::WaveDistort) == 19 &&
                  static_cast<int>(FilterType::Glitch) == 20 &&
                  static_cast<int>(FilterType::Kaleidoscope) == 21 &&
                  static_cast<int>(FilterType::Fisheye) == 22 &&
                  static_cast<int>(FilterType::Swirl) == 23 &&
                  static_cast<int>(FilterType::ColorGrade) == 24 &&
                  static_cast<int>(FilterType::HueShift) == 25 &&
                  static_cast<int>(FilterType::Invert) == 26 &&
                  static_cast<int>(FilterType::Threshold) == 27 &&
                  static_cast<int>(FilterType::Sepia) == 28 &&
                  static_cast<int>(FilterType::Bleed) == 29 &&
                  static_cast<int>(FilterType::Feedback) == 30 &&
                  static_cast<int>(FilterType::Count) == 31,
              "FilterType ids must match the uFilter chain in the filter shader");

namespace {

// GL_VIEWPORT не входит в GL-поверхность движка; значение совпадает в
// GL 3.3 core и GLES 3.0.
constexpr gl::GLenum kGLViewport = 0x0BA2;

// ---------------------------------------------------------------------------
// Shader sources (builtin::Preamble() добавляет #version + precision)
// ---------------------------------------------------------------------------
const char* const kFilterVert = R"GLSL(
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aUV;
out vec2 vUV;
void main() {
    vUV = aUV;
    gl_Position = vec4(aPos, 0.0, 1.0);
}
)GLSL";

// Универсальный фрагментный шейдер фильтров. Идентификаторы uFilter: см. FilterType.
const char* const kFilterFrag = R"GLSL(
in vec2 vUV;
uniform sampler2D uTexture;
uniform sampler2D uHistory;
uniform vec2 uTexelSize;
uniform vec2 uResolution;
uniform float uTime;
uniform float uAspect;

uniform int uFilter;
uniform float uAmount;
uniform float uRadius;
uniform float uSigma;
uniform vec2 uCenter;
uniform vec2 uDirection;
uniform float uAngle;
uniform float uFrequency;
uniform float uAmplitude;
uniform float uThreshold;
uniform float uLevels;
uniform int uCellSize;
uniform vec4 uTint;
uniform vec4 uLift;
uniform vec4 uGain;
uniform float uGamma;
uniform float uSaturation;
uniform float uTemperature;
uniform float uVignette;
uniform float uGrain;
uniform float uScanlineStrength;
uniform float uCurvature;
uniform float uAberration;
uniform float uBloomThreshold;
uniform int uUseThreshold;
uniform float uFeedback;
uniform int uSegments;

out vec4 fragColor;

const float PI = 3.14159265359;
const float TWO_PI = 6.28318530718;

float luma(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }

float hash01(vec2 p) {
    return fract(sin(dot(p, vec2(12.9898, 78.233))) * 43758.5453123);
}

vec3 sampleUV(vec2 uv) { return texture(uTexture, clamp(uv, vec2(0.0), vec2(1.0))).rgb; }
vec3 sampleOff(vec2 uv, vec2 offPixels) { return sampleUV(uv + offPixels * uTexelSize); }

// 8x8 ordered Bayer threshold in [0,1).
float bayer2(vec2 a) { a = floor(a); return fract(a.x * 0.5 + a.y * a.y * 0.75); }
float bayer4(vec2 a) { return bayer2(a * 0.5) * 0.25 + bayer2(a); }
float bayer8(vec2 a) { return bayer4(a * 0.5) * 0.25 + bayer2(a); }

vec3 rgb2hsv(vec3 c) {
    vec4 K = vec4(0.0, -1.0 / 3.0, 2.0 / 3.0, -1.0);
    vec4 p = mix(vec4(c.bg, K.wz), vec4(c.gb, K.xy), step(c.b, c.g));
    vec4 q = mix(vec4(p.xyw, c.r), vec4(c.r, p.yzx), step(p.x, c.r));
    float d = q.x - min(q.w, q.y);
    float e = 1.0e-10;
    return vec3(abs(q.z + (q.w - q.y) / (6.0 * d + e)), d / (q.x + e), q.x);
}

vec3 hsv2rgb(vec3 c) {
    vec4 K = vec4(1.0, 2.0 / 3.0, 1.0 / 3.0, 3.0);
    vec3 p = abs(fract(c.xxx + K.xyz) * 6.0 - K.www);
    return c.z * mix(K.xxx, clamp(p - K.xxx, 0.0, 1.0), c.y);
}

// 13-tap separable-ish blur approximation used inside the uber shader.
vec3 blurApprox(vec2 uv, float radius, float sigma, int gaussian) {
    vec3 sum = texture(uTexture, uv).rgb;
    float wsum = 1.0;
    for (int ring = 1; ring <= 3; ++ring) {
        float d = radius * float(ring) / 3.0;
        float w = 1.0;
        if (gaussian == 1) w = exp(-0.5 * d * d / max(sigma * sigma, 1e-4));
        sum += sampleOff(uv, vec2(d, 0.0)) * w;
        sum += sampleOff(uv, vec2(-d, 0.0)) * w;
        sum += sampleOff(uv, vec2(0.0, d)) * w;
        sum += sampleOff(uv, vec2(0.0, -d)) * w;
        wsum += 4.0 * w;
    }
    return sum / wsum;
}

vec3 brightPass(vec3 c, float t) {
    float l = luma(c);
    return c * (max(l - t, 0.0) / max(l, 1e-4));
}

// Single-pass 13-tap bloom: centre + three rings of four taps.
vec3 bloom13(vec2 uv, float radius) {
    float t = (uUseThreshold != 0) ? uBloomThreshold : 0.0;
    float r = max(radius, 1.0);
    vec3 sum = brightPass(texture(uTexture, uv).rgb, t) * 0.2;
    float wsum = 0.2;
    for (int i = 1; i <= 3; ++i) {
        float d = r * float(i) * 0.75;
        float w = exp(-0.5 * float(i * i) * 0.5);
        sum += brightPass(sampleOff(uv, vec2(d, 0.0)), t) * w;
        sum += brightPass(sampleOff(uv, vec2(-d, 0.0)), t) * w;
        sum += brightPass(sampleOff(uv, vec2(0.0, d)), t) * w;
        sum += brightPass(sampleOff(uv, vec2(0.0, -d)), t) * w;
        wsum += 4.0 * w;
    }
    return sum / wsum;
}

void main() {
    vec2 uv = vUV;
    vec4 src4 = texture(uTexture, uv);
    vec3 src = src4.rgb;
    float alpha = src4.a;
    vec3 col = src;

    if (uFilter == 0) {  // Bloom
        col = src + bloom13(uv, uRadius) * uAmount * 0.6;
    } else if (uFilter == 1) {  // Blur (box approximation)
        col = mix(src, blurApprox(uv, max(uRadius, 0.5), max(uSigma, 0.5), 0), clamp(uAmount, 0.0, 1.0));
    } else if (uFilter == 2) {  // GaussianBlur (approximation)
        col = mix(src, blurApprox(uv, max(uRadius, 0.5), max(uSigma, 0.5), 1), clamp(uAmount, 0.0, 1.0));
    } else if (uFilter == 3) {  // RadialBlur away from center
        vec3 sum = src;
        float ws = 1.0;
        for (int i = 1; i <= 11; ++i) {
            float t = float(i) / 11.0;
            vec2 p = uv - (uv - uCenter) * (uAmount * t);
            sum += sampleUV(p);
            ws += 1.0;
        }
        col = sum / ws;
    } else if (uFilter == 4) {  // ZoomBlur (radial scaling about center)
        vec3 sum = src;
        float ws = 1.0;
        for (int i = 1; i <= 11; ++i) {
            float t = float(i) / 11.0;
            vec2 p = uCenter + (uv - uCenter) * (1.0 + uAmount * t);
            sum += sampleUV(p);
            ws += 1.0;
        }
        col = sum / ws;
    } else if (uFilter == 5) {  // MotionBlur along direction
        vec2 dir = normalize(uDirection + vec2(1e-6, 0.0));
        vec3 sum = src;
        float ws = 1.0;
        for (int i = 1; i <= 11; ++i) {
            float t = (float(i) / 11.0 - 0.5);
            sum += sampleUV(uv + dir * uAmount * t);
            ws += 1.0;
        }
        col = sum / ws;
    } else if (uFilter == 6) {  // Sharpen (unsharp mask)
        vec3 b = blurApprox(uv, max(uRadius, 1.0), max(uSigma, 1.0), 0);
        col = src + (src - b) * uAmount * 1.5;
    } else if (uFilter == 7) {  // EdgeDetect (Sobel)
        float tl = luma(sampleOff(uv, vec2(-1.0, 1.0)));
        float lm = luma(sampleOff(uv, vec2(-1.0, 0.0)));
        float bl = luma(sampleOff(uv, vec2(-1.0, -1.0)));
        float tm = luma(sampleOff(uv, vec2(0.0, 1.0)));
        float bm = luma(sampleOff(uv, vec2(0.0, -1.0)));
        float tr = luma(sampleOff(uv, vec2(1.0, 1.0)));
        float rm = luma(sampleOff(uv, vec2(1.0, 0.0)));
        float br = luma(sampleOff(uv, vec2(1.0, -1.0)));
        float gx = -tl - 2.0 * lm - bl + tr + 2.0 * rm + br;
        float gy = tl + 2.0 * tm + tr - bl - 2.0 * bm - br;
        float g = clamp(length(vec2(gx, gy)) * max(uAmount, 0.0), 0.0, 1.0);
        col = vec3(g);
    } else if (uFilter == 8) {  // Emboss
        vec2 dir = normalize(uDirection + vec2(1e-6, 0.0));
        float d = luma(sampleOff(uv, dir * 2.0)) - luma(sampleOff(uv, -dir * 2.0));
        col = clamp(src + vec3(d * uAmount * 2.0), 0.0, 1.0);
    } else if (uFilter == 9) {  // Pixelate
        float cs = float(max(uCellSize, 1));
        vec2 p = (floor(uv / (uTexelSize * cs)) + 0.5) * uTexelSize * cs;
        col = mix(src, sampleUV(p), clamp(uAmount, 0.0, 1.0));
    } else if (uFilter == 10) {  // Posterize
        float levels = max(uLevels, 2.0);
        vec3 q = floor(clamp(src, 0.0, 1.0) * levels + 0.5) / levels;
        col = mix(src, q, clamp(uAmount, 0.0, 1.0));
    } else if (uFilter == 11) {  // Halftone
        float cs = float(max(uCellSize, 2));
        vec2 cell = uTexelSize * cs;
        vec2 inCell = fract(uv / cell) - 0.5;
        vec2 centerUV = (floor(uv / cell) + 0.5) * cell;
        float l = luma(sampleUV(centerUV));
        float radius = sqrt(clamp(l, 0.0, 1.0)) * 0.62;
        float m = 1.0 - smoothstep(radius - 0.15, radius + 0.02, length(inCell));
        col = mix(src, vec3(m) * (0.35 + 0.65 * l), clamp(uAmount, 0.0, 1.0));
    } else if (uFilter == 12) {  // Dither (8x8 Bayer)
        vec2 pix = floor(uv / max(uTexelSize, vec2(1e-6)));
        float t = bayer8(pix);
        float levels = max(uLevels, 2.0);
        vec3 c = clamp(src, 0.0, 1.0) * (levels - 1.0);
        vec3 q = floor(c + (t - 0.5)) / (levels - 1.0);
        col = mix(src, clamp(q, 0.0, 1.0), clamp(uAmount, 0.0, 1.0));
    } else if (uFilter == 13) {  // Scanlines
        float y = uv.y / max(uTexelSize.y, 1e-6);
        float s = 0.5 + 0.5 * sin(y * PI);
        float m = 1.0 - clamp(uScanlineStrength, 0.0, 1.0) * s * clamp(uAmount, 0.0, 1.0);
        col = src * m;
    } else if (uFilter == 14) {  // Crt: curvature + scanlines + aperture mask
        vec2 cc = uv - uCenter;
        float r2 = dot(cc, cc);
        vec2 suv = uv + cc * r2 * uCurvature * 4.0;
        if (suv.x < 0.0 || suv.x > 1.0 || suv.y < 0.0 || suv.y > 1.0) {
            col = vec3(0.0);
        } else {
            vec2 shift = cc * uAberration * 6.0;
            vec3 c;
            c.r = sampleUV(suv + shift).r;
            c.g = sampleUV(suv).g;
            c.b = sampleUV(suv - shift).b;
            float y = suv.y / max(uTexelSize.y, 1e-6);
            float sl = 0.5 + 0.5 * sin(y * PI);
            c *= 1.0 - clamp(uScanlineStrength, 0.0, 1.0) * sl;
            int mi = int(mod(floor(suv.x / max(uTexelSize.x, 1e-6)), 3.0));
            vec3 mask = vec3(0.78);
            if (mi == 0) mask.r = 1.4;
            else if (mi == 1) mask.g = 1.4;
            else mask.b = 1.4;
            c *= mask;
            col = c * clamp(1.0 - 0.5 * r2 * 2.0, 0.0, 1.0);
        }
    } else if (uFilter == 15) {  // Vignette
        vec2 d = (uv - uCenter) * vec2(uAspect, 1.0);
        float v = 1.0 - clamp(uVignette, 0.0, 1.5) * dot(d, d) * 2.0;
        col = src * clamp(v, 0.0, 1.0);
    } else if (uFilter == 16) {  // FilmGrain
        float n = hash01(uv * uResolution + vec2(uTime * 37.0, uTime * 17.0)) - 0.5;
        col = src + vec3(n * uGrain * clamp(uAmount, 0.0, 4.0) * 2.0);
    } else if (uFilter == 17) {  // ChromaticAberration
        vec2 d = (uv - uCenter) * uAberration * clamp(uAmount, 0.0, 4.0) * 4.0;
        col = vec3(sampleUV(uv + d).r, src.g, sampleUV(uv - d).b);
    } else if (uFilter == 18) {  // BarrelDistort (pincushion when amount < 0)
        vec2 cc = uv - uCenter;
        vec2 q = cc * vec2(uAspect, 1.0);
        float r2 = dot(q, q);
        vec2 suv = uCenter + cc * (1.0 + uAmount * r2);
        if (suv.x < 0.0 || suv.x > 1.0 || suv.y < 0.0 || suv.y > 1.0) col = vec3(0.0);
        else col = sampleUV(suv);
    } else if (uFilter == 19) {  // WaveDistort
        vec2 suv = uv;
        suv.x += sin(uv.y * uFrequency + uTime * 1.7) * uAmplitude * uAmount;
        suv.y += cos(uv.x * uFrequency * 0.8 + uTime * 1.3) * uAmplitude * uAmount * 0.7;
        col = sampleUV(suv);
    } else if (uFilter == 20) {  // Glitch (block displacement + RGB split)
        float t = floor(uTime * 7.0);
        float band = floor(uv.y * 28.0);
        float h = hash01(vec2(band, t));
        float amt = clamp(uAmount, 0.0, 2.0);
        vec2 suv = uv;
        if (h > 1.0 - amt * 0.35) {
            suv.x += (hash01(vec2(band * 3.7, t + 11.0)) - 0.5) * amt * 0.3;
        }
        vec2 split = vec2(amt * 0.015, 0.0);
        col = vec3(sampleUV(suv + split).r, sampleUV(suv).g, sampleUV(suv - split).b);
        col *= 1.0 - 0.2 * step(0.93, hash01(vec2(band, t + 5.0)));
    } else if (uFilter == 21) {  // Kaleidoscope
        vec2 p = uv - uCenter;
        float r = length(p);
        float a = atan(p.y, p.x) + uAngle;
        float seg = TWO_PI / float(max(uSegments, 3));
        a = mod(a, seg);
        a = abs(a - seg * 0.5);
        col = sampleUV(uCenter + vec2(cos(a), sin(a)) * r);
    } else if (uFilter == 22) {  // Fisheye
        vec2 p = (uv - uCenter) * 2.0;
        p.x *= uAspect;
        float r2 = dot(p, p);
        vec2 q = p * (1.0 + clamp(uAmount, -0.9, 2.0) * r2);
        q.x /= max(uAspect, 1e-4);
        vec2 suv = uCenter + q * 0.5;
        if (suv.x < 0.0 || suv.x > 1.0 || suv.y < 0.0 || suv.y > 1.0) col = vec3(0.0);
        else col = sampleUV(suv);
    } else if (uFilter == 23) {  // Swirl
        vec2 p = uv - uCenter;
        float r = length(p);
        float fall = smoothstep(0.5, 0.0, r);
        float a = uAngle + uAmount * fall * 4.0;
        float cs = cos(a);
        float sn = sin(a);
        col = sampleUV(uCenter + vec2(p.x * cs - p.y * sn, p.x * sn + p.y * cs));
    } else if (uFilter == 24) {  // ColorGrade: lift/gamma/gain + sat + temp
        vec3 c = src * uGain.rgb + uLift.rgb;
        c = pow(max(c, vec3(0.0)), vec3(1.0 / max(uGamma, 0.05)));
        float l = luma(c);
        c = mix(vec3(l), c, max(uSaturation, 0.0));
        c += vec3(uTemperature * 0.12, uTemperature * 0.02, -uTemperature * 0.12);
        c *= max(uTint.rgb, vec3(0.0));
        col = mix(src, c, clamp(uAmount, 0.0, 1.0));
    } else if (uFilter == 25) {  // HueShift
        vec3 hsv = rgb2hsv(clamp(src, 0.0, 1.0));
        hsv.x = fract(hsv.x + uAngle / TWO_PI + (uAmount - 1.0) * 0.25);
        col = hsv2rgb(hsv);
    } else if (uFilter == 26) {  // Invert
        col = mix(src, vec3(1.0) - clamp(src, 0.0, 1.0), clamp(uAmount, 0.0, 1.0));
    } else if (uFilter == 27) {  // Threshold
        float bw = step(uThreshold, luma(src));
        col = mix(src, vec3(bw), clamp(uAmount, 0.0, 1.0));
    } else if (uFilter == 28) {  // Sepia
        vec3 s = vec3(dot(src, vec3(0.393, 0.769, 0.189)),
                      dot(src, vec3(0.349, 0.686, 0.168)),
                      dot(src, vec3(0.272, 0.534, 0.131)));
        col = mix(src, clamp(s * uTint.rgb, 0.0, 1.0), clamp(uAmount, 0.0, 1.0));
    } else if (uFilter == 29) {  // Bleed (cheap edge-aware smear)
        float l = luma(src);
        vec3 sum = src;
        float ws = 1.0;
        float dist = max(uRadius, 1.0) * clamp(uAmount, 0.0, 3.0);
        for (int i = 0; i < 8; ++i) {
            float ang = float(i) * 0.78539816339;
            vec3 s = sampleOff(uv, vec2(cos(ang), sin(ang)) * dist);
            float w = 1.0 / (1.0 + abs(luma(s) - l) * 10.0);
            sum += s * w;
            ws += w;
        }
        vec3 smear = sum / ws;
        col = smear + (src - smear) * 0.35;
    } else if (uFilter == 30) {  // Feedback (mix the previous frame back in)
        vec3 h = texture(uHistory, clamp(uv, vec2(0.0), vec2(1.0))).rgb;
        float f = clamp(uFeedback * clamp(uAmount, 0.0, 1.0), 0.0, 0.98);
        col = mix(src, h, f);
    }

    fragColor = vec4(col, alpha);
}
)GLSL";

// Сепарабельное размытие: одна ось на проход, выбирается через uDirection.
const char* const kBlurFrag = R"GLSL(
in vec2 vUV;
uniform sampler2D uTexture;
uniform vec2 uTexelSize;
uniform vec2 uDirection;
uniform float uSpacing;   // pixels between linear-sampled taps
uniform float uSigma;
uniform int uTaps;        // 1..32
uniform int uGaussian;
out vec4 fragColor;

void main() {
    vec3 sum = texture(uTexture, vUV).rgb;
    float wsum = 1.0;
    for (int i = 1; i <= 32; ++i) {
        if (i > uTaps) break;
        float d = uSpacing * float(i);
        float w = 1.0;
        if (uGaussian == 1) w = exp(-0.5 * d * d / max(uSigma * uSigma, 1e-4));
        vec2 off = uDirection * uTexelSize * d;
        sum += texture(uTexture, clamp(vUV + off, vec2(0.0), vec2(1.0))).rgb * w;
        sum += texture(uTexture, clamp(vUV - off, vec2(0.0), vec2(1.0))).rgb * w;
        wsum += 2.0 * w;
    }
    fragColor = vec4(sum / wsum, 1.0);
}
)GLSL";

// ---------------------------------------------------------------------------
// Полноэкранный квад + помощники проходов
// ---------------------------------------------------------------------------
struct QuadGL {
    unsigned int vao = 0;
    unsigned int vbo = 0;
    bool ready = false;

    bool Create() {
        if (ready) return true;
        if (!gl::glGenVertexArrays || !gl::glGenBuffers) return false;
        const f32 verts[] = {
            // pos      uv
            -1, -1, 0, 0, 1, -1, 1, 0, -1, 1, 0, 1, 1, 1, 1, 1,
        };
        gl::glGenVertexArrays(1, &vao);
        gl::glGenBuffers(1, &vbo);
        gl::glBindVertexArray(vao);
        gl::glBindBuffer(gl::GL_ARRAY_BUFFER, vbo);
        gl::glBufferData(gl::GL_ARRAY_BUFFER, sizeof(verts), verts, gl::GL_STATIC_DRAW);
        gl::glEnableVertexAttribArray(0);
        gl::glVertexAttribPointer(0, 2, gl::GL_FLOAT, gl::GL_FALSE, 4 * sizeof(f32), nullptr);
        gl::glEnableVertexAttribArray(1);
        gl::glVertexAttribPointer(1, 2, gl::GL_FLOAT, gl::GL_FALSE, 4 * sizeof(f32),
                                  reinterpret_cast<const void*>(2 * sizeof(f32)));
        gl::glBindVertexArray(0);
        ready = true;
        return true;
    }

    void Draw() const {
        if (!ready) return;
        gl::glBindVertexArray(vao);
        gl::glDrawArrays(gl::GL_TRIANGLE_STRIP, 0, 4);
    }
};

// Сохраняет/восстанавливает GL-состояние, которое меняет полноэкранный проход.
struct ScopedPassState {
    gl::GLboolean depthWas = 0, cullWas = 0, blendWas = 0, depthMaskWas = 1;

    ScopedPassState() {
        depthWas = gl::glIsEnabled(gl::GL_DEPTH_TEST);
        cullWas = gl::glIsEnabled(gl::GL_CULL_FACE);
        blendWas = gl::glIsEnabled(gl::GL_BLEND);
        gl::glGetBooleanv(gl::GL_DEPTH_WRITEMASK, &depthMaskWas);
        gl::glDisable(gl::GL_DEPTH_TEST);
        gl::glDepthMask(0);
        gl::glDisable(gl::GL_CULL_FACE);
        gl::glDisable(gl::GL_BLEND);
    }
    ~ScopedPassState() {
        if (depthWas) gl::glEnable(gl::GL_DEPTH_TEST);
        gl::glDepthMask(depthMaskWas);
        if (cullWas) gl::glEnable(gl::GL_CULL_FACE);
        if (blendWas) gl::glEnable(gl::GL_BLEND);
    }
    ScopedPassState(const ScopedPassState&) = delete;
    ScopedPassState& operator=(const ScopedPassState&) = delete;
};

// Текстурное полноэкранное копирование ("blit") из `src` в `fbo`.
void DrawCopyQuad(const QuadGL& quad, Shader* copyShader, const Texture& src, unsigned int fbo,
                  int width, int height) {
    if (!copyShader || !copyShader->Valid()) return;
    gl::glBindFramebuffer(gl::GL_FRAMEBUFFER, fbo);
    gl::glViewport(0, 0, MaxT(1, width), MaxT(1, height));
    copyShader->Bind();
    copyShader->SetTexture("uTexture", src, 0);
    copyShader->Set("uTint", Color::White);
    quad.Draw();
}

void SetFilterUniforms(Shader& shader, const FilterInstance& filter, const Texture& source,
                       const Texture& history, int width, int height) {
    const FilterParams& p = filter.params;
    shader.SetTexture("uTexture", source, 0);
    shader.SetTexture("uHistory", history, 1);
    shader.Set("uFilter", static_cast<i32>(filter.type));
    shader.Set("uTexelSize", Vec2{1.0f / static_cast<f32>(MaxT(1, width)),
                                  1.0f / static_cast<f32>(MaxT(1, height))});
    shader.Set("uResolution", Vec2{static_cast<f32>(width), static_cast<f32>(height)});
    shader.Set("uTime", static_cast<f32>(NowSeconds()));
    shader.Set("uAspect", static_cast<f32>(MaxT(1, width)) / static_cast<f32>(MaxT(1, height)));

    shader.Set("uAmount", p.amount);
    shader.Set("uRadius", p.radius);
    shader.Set("uSigma", p.sigma);
    shader.Set("uCenter", p.center);
    shader.Set("uDirection", p.direction);
    shader.Set("uAngle", p.angle);
    shader.Set("uFrequency", p.frequency);
    shader.Set("uAmplitude", p.amplitude);
    shader.Set("uThreshold", p.threshold);
    shader.Set("uLevels", p.levels);
    shader.Set("uCellSize", static_cast<i32>(p.cellSize));
    shader.Set("uTint", p.tint);
    shader.Set("uLift", p.lift);
    shader.Set("uGain", p.gain);
    shader.Set("uGamma", p.gamma);
    shader.Set("uSaturation", p.saturation);
    shader.Set("uTemperature", p.temperature);
    shader.Set("uVignette", p.vignette);
    shader.Set("uGrain", p.grain);
    shader.Set("uScanlineStrength", p.scanlineStrength);
    shader.Set("uCurvature", p.curvature);
    shader.Set("uAberration", p.aberration);
    shader.Set("uBloomThreshold", p.bloomThreshold);
    shader.Set("uUseThreshold", p.useThreshold ? 1 : 0);
    shader.Set("uFeedback", p.feedback);
    shader.Set("uSegments", static_cast<i32>(p.segments));
}

void DrawFilterPass(const QuadGL& quad, Shader& shader, const Texture& source,
                    const FilterInstance& filter, unsigned int fbo, int width, int height,
                    const Texture& history) {
    gl::glBindFramebuffer(gl::GL_FRAMEBUFFER, fbo);
    gl::glViewport(0, 0, MaxT(1, width), MaxT(1, height));
    shader.Bind();
    SetFilterUniforms(shader, filter, source, history, width, height);
    quad.Draw();
}

void DrawBlurPass(const QuadGL& quad, Shader& shader, const Texture& source, unsigned int fbo,
                  int width, int height, const Vec2& direction, const FilterParams& params,
                  bool gaussian, int taps, float spacing) {
    gl::glBindFramebuffer(gl::GL_FRAMEBUFFER, fbo);
    gl::glViewport(0, 0, MaxT(1, width), MaxT(1, height));
    shader.Bind();
    shader.SetTexture("uTexture", source, 0);
    shader.Set("uTexelSize", Vec2{1.0f / static_cast<f32>(MaxT(1, width)),
                                  1.0f / static_cast<f32>(MaxT(1, height))});
    shader.Set("uDirection", direction);
    shader.Set("uSpacing", spacing);
    shader.Set("uSigma", MaxT(params.sigma, 0.5f));
    shader.Set("uTaps", static_cast<i32>(taps));
    shader.Set("uGaussian", gaussian ? 1 : 0);
    quad.Draw();
}

// Сначала пробует создать 16F-цель (сохраняет диапазон HDR-источника), при
// недоступности формата откатывается к 8 битам.
bool CreateInternalTarget(RenderTarget& rt, int width, int height, const char* name) {
    RenderTargetDesc desc;
    desc.width = MaxT(1, width);
    desc.height = MaxT(1, height);
    desc.depth = false;
    desc.stencil = false;
    desc.samples = 1;
    desc.colorAsTexture = true;
    desc.colorFormat = PixelFormat::RGBA16F;
    desc.filter = TextureFilter::Linear;
    desc.wrap = TextureWrap::ClampToEdge;
    desc.name = name;
    if (rt.Create(desc)) return true;
    desc.colorFormat = PixelFormat::RGBA8;
    return rt.Create(desc);
}

}  // namespace

// ---------------------------------------------------------------------------
// Impl
// ---------------------------------------------------------------------------
struct FilterChain::Impl {
    QuadGL quad;
    RenderTarget blurScratch;  // промежуточная цель двухпроходного сепарабельного размытия
    bool attempted = false;
};

// ---------------------------------------------------------------------------
// Время жизни
// ---------------------------------------------------------------------------
FilterChain::FilterChain() : impl_(new Impl()) {}

FilterChain::~FilterChain() { Shutdown(); }

FilterChain::FilterChain(FilterChain&& o) noexcept
    : filters_(std::move(o.filters_)),
      a_(std::move(o.a_)),
      b_(std::move(o.b_)),
      history_(std::move(o.history_)),
      filterShader_(std::move(o.filterShader_)),
      blurShader_(std::move(o.blurShader_)),
      copyShader_(std::move(o.copyShader_)),
      width_(o.width_),
      height_(o.height_),
      initialized_(o.initialized_),
      stats_(o.stats_),
      impl_(std::move(o.impl_)) {
    o.width_ = 0;
    o.height_ = 0;
    o.initialized_ = false;
    o.stats_ = Stats{};
}

FilterChain& FilterChain::operator=(FilterChain&& o) noexcept {
    if (this == &o) return *this;
    Shutdown();
    filters_ = std::move(o.filters_);
    a_ = std::move(o.a_);
    b_ = std::move(o.b_);
    history_ = std::move(o.history_);
    filterShader_ = std::move(o.filterShader_);
    blurShader_ = std::move(o.blurShader_);
    copyShader_ = std::move(o.copyShader_);
    width_ = o.width_;
    height_ = o.height_;
    initialized_ = o.initialized_;
    stats_ = o.stats_;
    impl_ = std::move(o.impl_);
    o.width_ = 0;
    o.height_ = 0;
    o.initialized_ = false;
    o.stats_ = Stats{};
    return *this;
}

bool FilterChain::Init() {
    if (!impl_) impl_.reset(new Impl());
    impl_->attempted = true;
    if (!gl::glCreateShader) {
        ENG_LOGW("filter", "no GL context; FilterChain stays in copy-through mode");
        return false;
    }
    impl_->quad.Create();

    if (!filterShader_) filterShader_.reset(new Shader());
    if (!blurShader_) blurShader_.reset(new Shader());
    if (!copyShader_) copyShader_.reset(new Shader());

    bool ok = filterShader_->Build(kFilterVert, kFilterFrag, "filter-chain");
    ok = blurShader_->Build(kFilterVert, kBlurFrag, "filter-chain-blur") && ok;
    copyShader_->Build(kFilterVert, builtin::kBlitFrag, "filter-chain-copy");

    initialized_ = filterShader_->Valid();
    if (ok) {
        ENG_LOGI("filter", "FilterChain ready (%d filters available)",
                 static_cast<int>(FilterType::Count));
    } else {
        ENG_LOGE("filter", "FilterChain shader build failed; falling back to copy-through");
    }
    return initialized_;
}

void FilterChain::Shutdown() {
    a_.reset();
    b_.reset();
    history_.reset();
    filterShader_.reset();
    blurShader_.reset();
    copyShader_.reset();
    if (impl_) impl_->blurScratch.Destroy();
    width_ = 0;
    height_ = 0;
    initialized_ = false;
    stats_ = Stats{};
}

bool FilterChain::Valid() const { return initialized_ && filterShader_ && filterShader_->Valid(); }

// ---------------------------------------------------------------------------
// Редактирование списка
// ---------------------------------------------------------------------------
void FilterChain::Add(FilterType type, const FilterParams& params, bool enabled) {
    FilterInstance f;
    f.type = type;
    f.enabled = enabled;
    f.params = params;
    filters_.push_back(std::move(f));
}

void FilterChain::Remove(int index) {
    if (index < 0 || index >= Count()) return;
    filters_.erase(filters_.begin() + index);
}

void FilterChain::MoveUp(int index) {
    if (index <= 0 || index >= Count()) return;
    std::swap(filters_[static_cast<usize>(index)], filters_[static_cast<usize>(index - 1)]);
}

void FilterChain::MoveDown(int index) {
    if (index < 0 || index + 1 >= Count()) return;
    std::swap(filters_[static_cast<usize>(index)], filters_[static_cast<usize>(index + 1)]);
}

// ---------------------------------------------------------------------------
// Цели рендера
// ---------------------------------------------------------------------------
void FilterChain::EnsureTargets(int width, int height) {
    if (!impl_) impl_.reset(new Impl());
    width = MaxT(1, width);
    height = MaxT(1, height);
    bool sameSize = width_ == width && height_ == height;
    if (sameSize && a_ && a_->Valid() && b_ && b_->Valid() && history_ && history_->Valid() &&
        impl_->blurScratch.Valid()) {
        return;
    }
    width_ = width;
    height_ = height;

    a_.reset(new RenderTarget());
    if (!CreateInternalTarget(*a_, width, height, "filter-a")) {
        ENG_LOGE("filter", "failed to create ping-pong target A (%dx%d)", width, height);
        a_.reset();
        return;
    }
    b_.reset(new RenderTarget());
    if (!CreateInternalTarget(*b_, width, height, "filter-b")) {
        ENG_LOGE("filter", "failed to create ping-pong target B (%dx%d)", width, height);
        b_.reset();
        return;
    }
    history_.reset(new RenderTarget());
    if (!CreateInternalTarget(*history_, width, height, "filter-history")) {
        ENG_LOGW("filter", "failed to create history target (%dx%d); Feedback disabled", width,
                 height);
        history_.reset();
    } else {
        // Известное начальное состояние: неинициализированная история размазала
        // бы мусор в первый кадр Feedback.
        history_->Bind();
        history_->Clear(Color{0, 0, 0, 0}, false, false);
        history_->Unbind();
    }
    impl_->blurScratch.Destroy();
    if (!CreateInternalTarget(impl_->blurScratch, width, height, "filter-blur-scratch")) {
        ENG_LOGW("filter", "failed to create blur scratch target; separable blur falls back");
    }
}

// ---------------------------------------------------------------------------
// Проходы
// ---------------------------------------------------------------------------
void FilterChain::RunPass(const Texture& source, const FilterInstance& filter, RenderTarget& dst,
                          const Texture* history, int width, int height) {
    if (!impl_ || !filterShader_ || !filterShader_->Valid() || !dst.Valid()) return;
    const Texture& hist = (history && history->Valid()) ? *history : source;
    DrawFilterPass(impl_->quad, *filterShader_, source, filter, dst.Fbo(), width, height, hist);
    ++stats_.passes;
}

void FilterChain::BlurPass(const Texture& source, RenderTarget& dst, const FilterParams& params,
                           int width, int height, bool gaussian) {
    if (!impl_) return;
    RenderTarget& scratch = impl_->blurScratch;
    if (!blurShader_ || !blurShader_->Valid() || !scratch.Valid() || !dst.Valid()) {
        // Нет сепарабельного шейдера/scratch: откатываемся к 13-tap приближению
        // uber-шейдера, чтобы фильтр всё же давал осмысленный результат.
        FilterInstance f;
        f.type = gaussian ? FilterType::GaussianBlur : FilterType::Blur;
        f.params = params;
        RunPass(source, f, dst, nullptr, width, height);
        return;
    }
    // Линейная выборка между текселями позволяет одному отсчёту покрыть два
    // пикселя: box-размытию радиуса R хватает ceil(R/2) отсчётов (лимит 32).
    const float spacing = 2.0f;
    int taps = static_cast<int>(std::ceil(MaxT(params.radius, 0.5f) / spacing));
    taps = Clamp(taps, 1, 32);

    DrawBlurPass(impl_->quad, *blurShader_, source, scratch.Fbo(), width, height, Vec2{1.0f, 0.0f},
                 params, gaussian, taps, spacing);
    ++stats_.passes;
    DrawBlurPass(impl_->quad, *blurShader_, scratch.ColorTexture(), dst.Fbo(), width, height,
                 Vec2{0.0f, 1.0f}, params, gaussian, taps, spacing);
    ++stats_.passes;
}

// ---------------------------------------------------------------------------
// Исполнение
// ---------------------------------------------------------------------------
void FilterChain::Apply(const Texture& source, unsigned int targetFbo, int width, int height) {
    stats_ = Stats{};
    if (!impl_) impl_.reset(new Impl());
    if (!impl_->attempted) Init();

    int w = MaxT(1, width);
    int h = MaxT(1, height);
    if (!source.Valid()) return;

    ScopedPassState state;
    if (!filterShader_ || !filterShader_->Valid()) {
        // Дегенеративный случай: нет пригодных шейдеров. Всё равно копируем
        // источник, чтобы у вызывающего не осталось нетронутого/чёрного кадра.
        DrawCopyQuad(impl_->quad, copyShader_.get(), source, targetFbo, w, h);
        return;
    }
    EnsureTargets(w, h);
    if (!a_ || !a_->Valid() || !b_ || !b_->Valid()) {
        DrawCopyQuad(impl_->quad, copyShader_.get(), source, targetFbo, w, h);
        return;
    }

    const Texture* current = &source;
    RenderTarget* result = nullptr;
    int lastDst = -1;
    for (const FilterInstance& f : filters_) {
        if (!f.enabled) continue;
        RenderTarget* dst = (lastDst == 0) ? b_.get() : a_.get();
        int idx = (dst == a_.get()) ? 0 : 1;
        if (lastDst >= 0 && idx != lastDst) ++stats_.pingPong;

        if (f.type == FilterType::Blur || f.type == FilterType::GaussianBlur) {
            BlurPass(*current, *dst, f.params, w, h, f.type == FilterType::GaussianBlur);
        } else {
            const Texture* hist = nullptr;
            if (f.type == FilterType::Feedback) {
                stats_.usedFeedback = true;
                if (history_ && history_->Valid()) hist = &history_->ColorTexture();
            }
            RunPass(*current, f, *dst, hist, w, h);
            if (f.type == FilterType::Feedback && history_ && history_->Valid()) {
                // Запоминаем этот кадр для следующего.
                DrawCopyQuad(impl_->quad, copyShader_.get(), dst->ColorTexture(), history_->Fbo(), w,
                             h);
            }
        }
        current = &dst->ColorTexture();
        result = dst;
        lastDst = idx;
    }

    if (!result) {
        // Пустая/дегенеративная цепочка: пропускаем источник без изменений.
        DrawCopyQuad(impl_->quad, copyShader_.get(), source, targetFbo, w, h);
        return;
    }
    if (targetFbo != result->Fbo()) {
        DrawCopyQuad(impl_->quad, copyShader_.get(), result->ColorTexture(), targetFbo, w, h);
    }
}

void FilterChain::Apply(const RenderTarget& source, unsigned int targetFbo) {
    if (!source.Valid()) return;
    // RenderTarget::ColorTexture(), к сожалению, не const, поэтому перегрузке с
    // const-ссылкой нужен cast, чтобы достать семплируемую текстуру.
    RenderTarget& rt = const_cast<RenderTarget&>(source);
    Apply(rt.ColorTexture(), targetFbo, rt.Width(), rt.Height());
}

void FilterChain::ApplySingle(const Texture& source, const FilterInstance& filter,
                              unsigned int targetFbo, int width, int height) {
    stats_ = Stats{};
    if (!impl_) impl_.reset(new Impl());
    if (!impl_->attempted) Init();

    int w = MaxT(1, width);
    int h = MaxT(1, height);
    if (!source.Valid()) return;

    ScopedPassState state;
    if (!filterShader_ || !filterShader_->Valid()) {
        DrawCopyQuad(impl_->quad, copyShader_.get(), source, targetFbo, w, h);
        return;
    }
    EnsureTargets(w, h);
    if (!a_ || !a_->Valid()) {
        DrawCopyQuad(impl_->quad, copyShader_.get(), source, targetFbo, w, h);
        return;
    }

    // Всегда исполняем во внутреннюю рабочую цель, чтобы Feedback мог снять
    // результат даже когда цель вызывающего — default framebuffer.
    RenderTarget& work = *a_;
    if (filter.type == FilterType::Blur || filter.type == FilterType::GaussianBlur) {
        BlurPass(source, work, filter.params, w, h, filter.type == FilterType::GaussianBlur);
    } else {
        const Texture* hist = nullptr;
        if (filter.type == FilterType::Feedback) {
            stats_.usedFeedback = true;
            if (history_ && history_->Valid()) hist = &history_->ColorTexture();
        }
        RunPass(source, filter, work, hist, w, h);
        if (filter.type == FilterType::Feedback && history_ && history_->Valid()) {
            DrawCopyQuad(impl_->quad, copyShader_.get(), work.ColorTexture(), history_->Fbo(), w, h);
        }
    }

    if (targetFbo != work.Fbo()) {
        DrawCopyQuad(impl_->quad, copyShader_.get(), work.ColorTexture(), targetFbo, w, h);
    }
}

void FilterChain::PreviewGrid(const Texture& source, const std::vector<FilterType>& types,
                              int columns, int width, int height) {
    stats_ = Stats{};
    if (!impl_) impl_.reset(new Impl());
    if (!impl_->attempted) Init();
    if (!source.Valid() || types.empty()) return;

    const int w = MaxT(1, width);
    const int h = MaxT(1, height);
    if (!filterShader_ || !filterShader_->Valid()) return;

    // Сначала запоминаем цель/вьюпорт вызывающего: EnsureTargets может
    // пересоздать render target'ы (RenderTarget::Create отвязывает в 0).
    gl::GLint prevFbo = 0;
    gl::glGetIntegerv(gl::GL_FRAMEBUFFER_BINDING, &prevFbo);
    gl::GLint prevViewport[4] = {0, 0, 0, 0};
    gl::glGetIntegerv(kGLViewport, prevViewport);

    EnsureTargets(w, h);
    if (!a_ || !a_->Valid()) return;

    const int cols = MaxT(1, columns);
    const int count = static_cast<int>(types.size());
    const int rows = MaxT(1, (count + cols - 1) / cols);
    const int cellW = MaxT(1, w / cols);
    const int cellH = MaxT(1, h / rows);

    // ApplySingle сбрасывает Stats при каждом вызове, поэтому копим по ячейкам.
    int totalPasses = 0;
    int totalPingPong = 0;
    bool usedFeedback = false;

    // Нейтральная подложка, чтобы ячейки читались как сетка даже там, где
    // фильтр оставляет углы прозрачными.
    gl::glEnable(gl::GL_SCISSOR_TEST);
    gl::glScissor(0, 0, cellW * cols, cellH * rows);
    gl::glClearColor(0.06f, 0.06f, 0.08f, 1.0f);
    gl::glClear(gl::GL_COLOR_BUFFER_BIT);
    gl::glDisable(gl::GL_SCISSOR_TEST);

    for (int i = 0; i < count; ++i) {
        FilterInstance inst;
        inst.type = types[static_cast<usize>(i)];
        inst.enabled = true;
        inst.params = Defaults(inst.type);
        // Исполняем фильтр во внутреннюю цель (без обратного копирования:
        // результат в ячейку переносим сами).
        ApplySingle(source, inst, a_->Fbo(), w, h);
        totalPasses += stats_.passes;
        totalPingPong += stats_.pingPong;
        usedFeedback = usedFeedback || stats_.usedFeedback;

        const int col = i % cols;
        const int row = i / cols;
        const int x = col * cellW;
        const int y = h - (row + 1) * cellH;  // начало координат вьюпорта GL — внизу слева
        gl::glBindFramebuffer(gl::GL_FRAMEBUFFER, static_cast<unsigned int>(prevFbo));
        gl::glViewport(x, y, cellW, cellH);
        if (copyShader_ && copyShader_->Valid()) {
            copyShader_->Bind();
            copyShader_->SetTexture("uTexture", a_->ColorTexture(), 0);
            copyShader_->Set("uTint", Color::White);
            impl_->quad.Draw();
        }
    }

    stats_.passes = totalPasses;
    stats_.pingPong = totalPingPong;
    stats_.usedFeedback = usedFeedback;

    gl::glBindFramebuffer(gl::GL_FRAMEBUFFER, static_cast<unsigned int>(prevFbo));
    gl::glViewport(prevViewport[0], prevViewport[1], prevViewport[2], prevViewport[3]);
}

// ---------------------------------------------------------------------------
// Метаданные
// ---------------------------------------------------------------------------
namespace {

struct FilterInfo {
    FilterType type;
    const char* name;
    const char* description;
};

// Стабильный порядок перечисления; AllFilters() идёт по нему буквально.
const FilterInfo kFilterInfo[] = {
    {FilterType::Bloom, "Bloom", "Soft single-pass crossrender around bright areas"},
    {FilterType::Blur, "Blur", "Separable box blur of the whole image"},
    {FilterType::GaussianBlur, "GaussianBlur", "Separable gaussian blur with a configurable sigma"},
    {FilterType::RadialBlur, "RadialBlur", "Blur that grows with distance from a centre point"},
    {FilterType::ZoomBlur, "ZoomBlur", "Radial scaling smear towards a centre point"},
    {FilterType::MotionBlur, "MotionBlur", "Linear blur along a direction"},
    {FilterType::Sharpen, "Sharpen", "Unsharp-mask contrast boost"},
    {FilterType::EdgeDetect, "EdgeDetect", "Sobel gradient magnitude"},
    {FilterType::Emboss, "Emboss", "Directional relief shading"},
    {FilterType::Pixelate, "Pixelate", "Snaps the image to a coarse cell grid"},
    {FilterType::Posterize, "Posterize", "Quantises colours to a fixed number of levels"},
    {FilterType::Halftone, "Halftone", "Dot-screen print look driven by luminance"},
    {FilterType::Dither, "Dither", "8x8 ordered Bayer dithering"},
    {FilterType::Scanlines, "Scanlines", "Alternating dark horizontal lines"},
    {FilterType::Crt, "Crt", "Barrel curvature + scanlines + aperture mask"},
    {FilterType::Vignette, "Vignette", "Darkens the image towards the edges"},
    {FilterType::FilmGrain, "FilmGrain", "Animated luminance noise"},
    {FilterType::ChromaticAberration, "ChromaticAberration", "Per-channel radial RGB split"},
    {FilterType::BarrelDistort, "BarrelDistort", "Radial lens distortion (pincushion when negative)"},
    {FilterType::WaveDistort, "WaveDistort", "Sine ripple distortion"},
    {FilterType::Glitch, "Glitch", "Block displacement + RGB split glitch"},
    {FilterType::Kaleidoscope, "Kaleidoscope", "Mirrored radial wedges"},
    {FilterType::Fisheye, "Fisheye", "Barrel lens bulge"},
    {FilterType::Swirl, "Swirl", "Rotational twist around a centre"},
    {FilterType::ColorGrade, "ColorGrade", "Lift/gamma/gain + saturation + temperature"},
    {FilterType::HueShift, "HueShift", "Rotates hue while keeping saturation"},
    {FilterType::Invert, "Invert", "Inverts colours"},
    {FilterType::Threshold, "Threshold", "Hard black/white cut at a luminance threshold"},
    {FilterType::Sepia, "Sepia", "Warm monochrome photographic tone"},
    {FilterType::Bleed, "Bleed", "Cheap edge-aware smear (approximation)"},
    {FilterType::Feedback, "Feedback", "Mixes the previous frame back in (echo/trails)"},
};

const FilterInfo* FindInfo(FilterType t) {
    for (const FilterInfo& info : kFilterInfo) {
        if (info.type == t) return &info;
    }
    return nullptr;
}

}  // namespace

const char* FilterChain::FilterName(FilterType t) {
    const FilterInfo* info = FindInfo(t);
    return info ? info->name : "Unknown";
}

const char* FilterChain::FilterDescription(FilterType t) {
    const FilterInfo* info = FindInfo(t);
    return info ? info->description : "Unknown filter";
}

std::vector<FilterType> FilterChain::AllFilters() {
    std::vector<FilterType> out;
    out.reserve(sizeof(kFilterInfo) / sizeof(kFilterInfo[0]));
    for (const FilterInfo& info : kFilterInfo) out.push_back(info.type);
    return out;
}

FilterParams FilterChain::Defaults(FilterType t) {
    FilterParams p;
    switch (t) {
        case FilterType::Bloom:
            p.amount = 0.9f;
            p.radius = 8.0f;
            p.sigma = 4.0f;
            p.bloomThreshold = 0.6f;
            p.useThreshold = true;
            break;
        case FilterType::Blur:
            p.radius = 6.0f;
            p.sigma = 3.0f;
            break;
        case FilterType::GaussianBlur:
            p.radius = 8.0f;
            p.sigma = 4.0f;
            break;
        case FilterType::RadialBlur:
            p.amount = 0.03f;
            p.radius = 3.0f;
            p.center = Vec2{0.5f, 0.5f};
            break;
        case FilterType::ZoomBlur:
            p.amount = 0.04f;
            p.radius = 4.0f;
            p.center = Vec2{0.5f, 0.5f};
            break;
        case FilterType::MotionBlur:
            p.amount = 0.02f;
            p.direction = Vec2{1.0f, 0.0f};
            p.angle = 0.0f;
            break;
        case FilterType::Sharpen:
            p.amount = 1.0f;
            p.radius = 1.5f;
            p.sigma = 1.0f;
            break;
        case FilterType::EdgeDetect:
            p.amount = 1.0f;
            break;
        case FilterType::Emboss:
            p.amount = 1.0f;
            p.angle = 0.7853981634f;
            p.direction = Vec2{0.70710678f, 0.70710678f};
            break;
        case FilterType::Pixelate:
            p.cellSize = 8;
            p.amount = 1.0f;
            break;
        case FilterType::Posterize:
            p.levels = 6.0f;
            p.amount = 1.0f;
            break;
        case FilterType::Halftone:
            p.cellSize = 6;
            p.amount = 1.0f;
            break;
        case FilterType::Dither:
            p.levels = 3.0f;
            p.amount = 1.0f;
            break;
        case FilterType::Scanlines:
            p.scanlineStrength = 0.35f;
            p.amount = 1.0f;
            break;
        case FilterType::Crt:
            p.curvature = 0.08f;
            p.scanlineStrength = 0.35f;
            p.aberration = 0.0035f;
            p.center = Vec2{0.5f, 0.5f};
            p.amount = 1.0f;
            break;
        case FilterType::Vignette:
            p.vignette = 0.35f;
            p.center = Vec2{0.5f, 0.5f};
            p.amount = 1.0f;
            break;
        case FilterType::FilmGrain:
            p.grain = 0.06f;
            p.amount = 1.0f;
            break;
        case FilterType::ChromaticAberration:
            p.aberration = 0.004f;
            p.center = Vec2{0.5f, 0.5f};
            p.amount = 1.0f;
            break;
        case FilterType::BarrelDistort:
            p.amount = 0.15f;
            p.center = Vec2{0.5f, 0.5f};
            p.aspect = 1.0f;
            break;
        case FilterType::WaveDistort:
            p.amplitude = 0.01f;
            p.frequency = 20.0f;
            p.amount = 1.0f;
            break;
        case FilterType::Glitch:
            p.amount = 0.5f;
            break;
        case FilterType::Kaleidoscope:
            p.segments = 6;
            p.angle = 0.0f;
            p.center = Vec2{0.5f, 0.5f};
            break;
        case FilterType::Fisheye:
            p.amount = 0.5f;
            p.center = Vec2{0.5f, 0.5f};
            p.aspect = 1.0f;
            break;
        case FilterType::Swirl:
            p.amount = 1.0f;
            p.angle = 0.0f;
            p.center = Vec2{0.5f, 0.5f};
            break;
        case FilterType::ColorGrade:
            p.lift = Color{0.01f, 0.01f, 0.01f, 0.0f};
            p.gain = Color{1.05f, 1.03f, 1.0f, 1.0f};
            p.gamma = 1.0f;
            p.saturation = 1.15f;
            p.temperature = 0.15f;
            p.tint = Color{1, 1, 1, 1};
            p.amount = 1.0f;
            break;
        case FilterType::HueShift:
            p.angle = 0.6f;
            p.amount = 1.0f;
            break;
        case FilterType::Invert:
            p.amount = 1.0f;
            break;
        case FilterType::Threshold:
            p.threshold = 0.5f;
            p.amount = 1.0f;
            break;
        case FilterType::Sepia:
            p.tint = Color{1.05f, 0.98f, 0.88f, 1.0f};
            p.amount = 1.0f;
            break;
        case FilterType::Bleed:
            p.radius = 6.0f;
            p.amount = 1.0f;
            break;
        case FilterType::Feedback:
            p.feedback = 0.85f;
            p.amount = 1.0f;
            break;
        case FilterType::Count:
        default:
            break;
    }
    return p;
}

// ---------------------------------------------------------------------------
// Пресеты
// ---------------------------------------------------------------------------
std::vector<std::string> FilterChain::PresetNames() {
    return {"Clean",  "Dreamy", "CRT",     "Glitch", "Painterly",
            "Noir",   "Pixel",  "Underwater", "Kaleido", "Dream"};
}

FilterChain FilterChain::MakePreset(const std::string& name) {
    FilterChain chain;
    auto add = [&chain](FilterType type, const FilterParams& params) {
        FilterInstance f;
        f.type = type;
        f.enabled = true;
        f.params = params;
        chain.Add(std::move(f));
    };

    if (name == "Clean") {
        return chain;  // сквозной пропуск
    }
    if (name == "Dreamy") {
        FilterParams blur = Defaults(FilterType::GaussianBlur);
        blur.radius = 10.0f;
        blur.sigma = 5.0f;
        blur.amount = 0.75f;
        add(FilterType::GaussianBlur, blur);
        FilterParams bloom = Defaults(FilterType::Bloom);
        bloom.radius = 10.0f;
        bloom.amount = 0.7f;
        add(FilterType::Bloom, bloom);
        FilterParams vig = Defaults(FilterType::Vignette);
        vig.vignette = 0.3f;
        add(FilterType::Vignette, vig);
        return chain;
    }
    if (name == "CRT") {
        FilterParams crt = Defaults(FilterType::Crt);
        crt.curvature = 0.09f;
        crt.scanlineStrength = 0.4f;
        add(FilterType::Crt, crt);
        FilterParams scan = Defaults(FilterType::Scanlines);
        scan.scanlineStrength = 0.25f;
        add(FilterType::Scanlines, scan);
        FilterParams ca = Defaults(FilterType::ChromaticAberration);
        ca.aberration = 0.0025f;
        add(FilterType::ChromaticAberration, ca);
        FilterParams bloom = Defaults(FilterType::Bloom);
        bloom.amount = 0.4f;
        bloom.bloomThreshold = 0.7f;
        add(FilterType::Bloom, bloom);
        return chain;
    }
    if (name == "Glitch") {
        FilterParams glitch = Defaults(FilterType::Glitch);
        glitch.amount = 0.6f;
        add(FilterType::Glitch, glitch);
        FilterParams ca = Defaults(FilterType::ChromaticAberration);
        ca.aberration = 0.005f;
        add(FilterType::ChromaticAberration, ca);
        FilterParams scan = Defaults(FilterType::Scanlines);
        scan.scanlineStrength = 0.2f;
        add(FilterType::Scanlines, scan);
        return chain;
    }
    if (name == "Painterly") {
        FilterParams bleed = Defaults(FilterType::Bleed);
        bleed.radius = 5.0f;
        add(FilterType::Bleed, bleed);
        FilterParams half = Defaults(FilterType::Halftone);
        half.cellSize = 5;
        half.amount = 0.6f;
        add(FilterType::Halftone, half);
        FilterParams grade = Defaults(FilterType::ColorGrade);
        grade.saturation = 1.25f;
        grade.temperature = 0.1f;
        add(FilterType::ColorGrade, grade);
        return chain;
    }
    if (name == "Noir") {
        FilterParams grade = Defaults(FilterType::ColorGrade);
        grade.saturation = 0.0f;
        grade.gain = Color{1.1f, 1.1f, 1.1f, 1.0f};
        grade.gamma = 0.95f;
        grade.temperature = -0.05f;
        add(FilterType::ColorGrade, grade);
        FilterParams grain = Defaults(FilterType::FilmGrain);
        grain.grain = 0.07f;
        add(FilterType::FilmGrain, grain);
        FilterParams vig = Defaults(FilterType::Vignette);
        vig.vignette = 0.45f;
        add(FilterType::Vignette, vig);
        return chain;
    }
    if (name == "Pixel") {
        FilterParams px = Defaults(FilterType::Pixelate);
        px.cellSize = 6;
        add(FilterType::Pixelate, px);
        FilterParams post = Defaults(FilterType::Posterize);
        post.levels = 5.0f;
        add(FilterType::Posterize, post);
        FilterParams dither = Defaults(FilterType::Dither);
        dither.levels = 3.0f;
        dither.amount = 0.8f;
        add(FilterType::Dither, dither);
        return chain;
    }
    if (name == "Underwater") {
        FilterParams wave = Defaults(FilterType::WaveDistort);
        wave.amplitude = 0.008f;
        wave.frequency = 14.0f;
        add(FilterType::WaveDistort, wave);
        FilterParams radial = Defaults(FilterType::RadialBlur);
        radial.amount = 0.02f;
        add(FilterType::RadialBlur, radial);
        FilterParams grade = Defaults(FilterType::ColorGrade);
        grade.temperature = -0.35f;
        grade.saturation = 0.9f;
        grade.tint = Color{0.72f, 0.95f, 1.25f, 1.0f};
        add(FilterType::ColorGrade, grade);
        return chain;
    }
    if (name == "Kaleido") {
        FilterParams kal = Defaults(FilterType::Kaleidoscope);
        kal.segments = 6;
        add(FilterType::Kaleidoscope, kal);
        FilterParams swirl = Defaults(FilterType::Swirl);
        swirl.amount = 0.8f;
        swirl.angle = 0.3f;
        add(FilterType::Swirl, swirl);
        return chain;
    }
    if (name == "Dream") {
        FilterParams fb = Defaults(FilterType::Feedback);
        fb.feedback = 0.8f;
        add(FilterType::Feedback, fb);
        FilterParams bloom = Defaults(FilterType::Bloom);
        bloom.radius = 10.0f;
        bloom.amount = 0.8f;
        bloom.bloomThreshold = 0.5f;
        add(FilterType::Bloom, bloom);
        FilterParams blur = Defaults(FilterType::Blur);
        blur.radius = 2.0f;
        add(FilterType::Blur, blur);
        return chain;
    }

    ENG_LOGW("filter", "unknown filter preset '%s'; returning the pass-through chain",
             name.c_str());
    return chain;
}

}  // namespace crossrender
