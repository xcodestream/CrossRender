// Ретро-режимы дисплея: низкоразрешённый виртуальный фреймбуфер, разворачиваемый
// в реальный либо как пиксель-арт с квантованием по палитре, либо как сетка символов.
//
// Структура
//   * каждый кадр сцена рендерится в `virtual_` (RGBA8, depth, NEAREST)
//   * EndFrame() рисует эту текстуру обратно в привязанный фреймбуфер —
//     либо пиксельным шейдером, либо ASCII-шейдером ниже
//   * оба шейдера используют один вершинный шейдер и один квад VAO/VBO
//   * CPU-помощники в RetroPalette.cpp / RetroAscii.cpp в точности повторяют
//     математику шейдеров (см. комментарии там).
#include "crossrender/gfx/Retro.h"

#include "RetroInternal.h"

#include "crossrender/gfx/GL.h"
#include "crossrender/core/Log.h"
#include "crossrender/text/Font.h"
#include "crossrender/gfx/Shader.h"
#include "crossrender/gfx/Texture.h"
#include "crossrender/gfx/Renderer2D.h"
#include "crossrender/gfx/RenderTarget.h"

#include <cmath>
#include <memory>
#include <string>
#include <vector>
#include <cstring>

namespace crossrender {

namespace {

// ---------------------------------------------------------------------------
// GLSL
// ---------------------------------------------------------------------------
// `builtin::Preamble()` (добавляется вызовом Shader::Build) подставляет `#version` и
// квалификаторы точности GLES, поэтому эти строки сами её не объявляют.
const char* kRetroQuadVert = R"GLSL(
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aUV;
out vec2 vUV;
void main() {
    vUV = aUV;
    gl_Position = vec4(aPos, 0.0, 1.0);
}
)GLSL";

// Разбор пиксельного режима.  uMode 0 = сплошная letterbox-заливка, 1 = полный разбор.
const char* kRetroPixelFrag = R"GLSL(
in vec2 vUV;
uniform sampler2D uTexture;
uniform sampler2D uPalette;
uniform vec4  uViewport;      // x,y = bottom-left corner in fb px, z,w = size
uniform vec2  uVirtualSize;
uniform vec4  uLetterbox;
uniform int   uMode;
uniform int   uUsePalette;
uniform int   uPaletteSize;
uniform float uColorDepth;    // 0 = off, else bits per channel (2..6)
uniform int   uDither;
uniform int   uDitherMatrix;  // 2, 4 or 8
uniform float uDitherStrength;
uniform int   uScanlines;
uniform float uScanlineStrength;
uniform float uScanlineCount; // 0 = virtual height
uniform int   uCurvature;
uniform float uCurvatureAmount;
uniform int   uMask;
uniform float uMaskStrength;
uniform float uBloom;
uniform float uBrightness;
uniform float uContrast;
uniform float uSaturation;
uniform int   uOverscan;
out vec4 fragColor;

// Classic recursive Bayer matrices.  The CPU twin is BayerThreshold().
float bayer2(vec2 a) { a = floor(a); return fract(a.x / 2.0 + a.y * a.y * 0.75); }
float bayer4(vec2 a) { return bayer2(0.5 * a) * 0.25 + bayer2(a); }
float bayer8(vec2 a) { return bayer4(0.5 * a) * 0.25 + bayer2(a); }
float bayerAt(vec2 p, int size) {
    if (size <= 2) return bayer2(p);
    if (size <= 4) return bayer4(p);
    return bayer8(p);
}

vec3 outsideColor(vec2 uv) {
    vec3 bg = uLetterbox.rgb;
    if (uOverscan != 0) {
        vec2 d = min(uv, 1.0 - uv);          // negative once outside the picture
        float band = min(d.x * uVirtualSize.x, d.y * uVirtualSize.y);
        bg = mix(bg, bg * 1.7 + vec3(0.03), clamp(1.0 + band * 0.35, 0.0, 1.0));
    }
    return bg;
}

void main() {
    if (uMode == 0) { fragColor = uLetterbox; return; }

    vec2 uv = vec2(vUV.x, 1.0 - vUV.y);      // top-down virtual image space
    if (uCurvature != 0) {
        vec2 q = uv * 2.0 - 1.0;
        q *= 1.0 + uCurvatureAmount * dot(q, q);
        uv = q * 0.5 + 0.5;
    }
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) {
        fragColor = vec4(outsideColor(uv), 1.0);
        return;
    }

    vec3 col = texture(uTexture, uv).rgb;    // texture filter is GL_NEAREST

    // sRGB-ish tone controls (display space, no gamma round trip).
    float grey = dot(col, vec3(0.2126, 0.7152, 0.0722));
    col = mix(vec3(grey), col, uSaturation);
    col = (col - 0.5) * uContrast + 0.5;
    col = clamp(col * uBrightness, 0.0, 1.0);

    // Ordered dithering, offset by the virtual pixel so it is resolution
    // stable rather than window stable.  It is only applied when something is
    // actually going to quantise the result; the amplitude is one palette step
    // (or one colour-depth step when there is no palette).
    int depthBits = int(clamp(uColorDepth + 0.5, 2.0, 6.0));
    bool useDepth = uColorDepth >= 0.5;
    if (uDither != 0 && (uUsePalette != 0 || useDepth)) {
        float denom = uUsePalette != 0 ? float(max(uPaletteSize, 2)) : float(1 << depthBits);
        float amp = uDitherStrength / denom;
        float t = (bayerAt(floor(uv * uVirtualSize), uDitherMatrix) - 0.5) * amp;
        col = clamp(col + t, 0.0, 1.0);
    }

    // Colour depth.
    if (useDepth) {
        float levels = float((1 << depthBits) - 1);
        col = floor(col * levels + 0.5) / levels;
    }

    // Palette quantisation: perceptually weighted nearest neighbour.
    if (uUsePalette != 0) {
        vec3 best = col;
        float bestDist = 1e9;
        for (int i = 0; i < 64; ++i) {
            if (i >= uPaletteSize) break;
            vec3 pc = texelFetch(uPalette, ivec2(i, 0), 0).rgb;
            vec3 d = (col - pc) * vec3(0.30, 0.59, 0.11);
            float dist = dot(d, d);
            if (dist < bestDist) { bestDist = dist; best = pc; }
        }
        col = best;
    }

    // Four-tap bloom, added on top of the quantised image.
    if (uBloom > 0.0) {
        vec2 ts = 1.0 / uVirtualSize;
        vec3 g = texture(uTexture, uv + vec2( 1.5,  0.5) * ts).rgb
               + texture(uTexture, uv + vec2(-1.5,  0.5) * ts).rgb
               + texture(uTexture, uv + vec2( 0.5, -1.5) * ts).rgb
               + texture(uTexture, uv + vec2(-0.5, -1.5) * ts).rgb;
        g *= 0.25;
        float crossrender = max(0.0, dot(g, vec3(0.2126, 0.7152, 0.0722)) - 0.5) * 2.0;
        col += g * crossrender * uBloom;
    }

    // Scanlines are physical framebuffer rows.
    if (uScanlines != 0) {
        float rows = uScanlineCount > 0.5 ? uScanlineCount : uVirtualSize.y;
        float sy = (gl_FragCoord.y - uViewport.y) / max(uViewport.w, 1.0) * rows;
        col *= 1.0 - uScanlineStrength * step(0.5, fract(sy * 0.5));
    }

    // Aperture grille: one primary per physical column.
    if (uMask != 0) {
        float c = mod(floor(gl_FragCoord.x), 3.0);
        vec3 w = c < 0.5 ? vec3(1.0, 0.62, 0.62)
               : (c < 1.5 ? vec3(0.62, 1.0, 0.62) : vec3(0.62, 0.62, 1.0));
        col *= mix(vec3(1.0), w, uMaskStrength);
    }

    fragColor = vec4(clamp(col, 0.0, 1.0), 1.0);
}
)GLSL";

// Разбор ASCII-режима.
const char* kRetroAsciiFrag = R"GLSL(
in vec2 vUV;
uniform sampler2D uTexture;
uniform sampler2D uAtlas;
uniform vec4  uViewport;
uniform vec2  uVirtualSize;
uniform vec2  uGrid;          // columns, rows
uniform vec2  uAtlasGrid;     // glyph cells in the atlas (columns, rows)
uniform int   uRampCount;
uniform float uGamma;
uniform float uContrast;
uniform float uBrightness;
uniform int   uInvert;
uniform int   uColorGlyphs;
uniform vec4  uInk;
uniform vec4  uPaper;
uniform int   uBackgroundFill;
uniform int   uShowGrid;
uniform int   uScanlines;
uniform float uScanlineStrength;
uniform float uScanlineCount;
uniform vec4  uLetterbox;
uniform int   uMode;
uniform int   uOverscan;
out vec4 fragColor;

vec3 outsideColor(vec2 uv) {
    vec3 bg = uLetterbox.rgb;
    if (uOverscan != 0) {
        vec2 d = min(uv, 1.0 - uv);
        float band = min(d.x * uVirtualSize.x, d.y * uVirtualSize.y);
        bg = mix(bg, bg * 1.7 + vec3(0.03), clamp(1.0 + band * 0.35, 0.0, 1.0));
    }
    return bg;
}

void main() {
    if (uMode == 0) { fragColor = uLetterbox; return; }

    vec2 uv = vec2(vUV.x, 1.0 - vUV.y);
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) {
        fragColor = vec4(outsideColor(uv), 1.0);
        return;
    }

    vec2 cellSize = 1.0 / uGrid;
    vec2 cellUV = uv * uGrid;
    vec2 cell = floor(cellUV);
    vec2 local = fract(cellUV);

    // 3x3 average around the cell centre (matches BuildAsciiGrid).
    vec2 centre = (cell + 0.5) * cellSize;
    vec3 sum = vec3(0.0);
    for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx)
            sum += texture(uTexture, centre + vec2(float(dx), float(dy)) * cellSize * 0.25).rgb;
    vec3 src = sum / 9.0;

    float lum = dot(src, vec3(0.2126, 0.7152, 0.0722));
    lum = pow(clamp(lum, 0.0, 1.0), 1.0 / max(uGamma, 0.01));
    lum = clamp((lum - 0.5) * uContrast + 0.5, 0.0, 1.0);
    lum = clamp(lum + uBrightness, 0.0, 1.0);

    int idx = int(min(floor(lum * float(uRampCount)), float(uRampCount - 1)));
    if (uInvert != 0) idx = uRampCount - 1 - idx;

    int ax = idx % int(uAtlasGrid.x);
    int ay = idx / int(uAtlasGrid.x);
    vec2 auv = (vec2(float(ax), float(ay)) + local) / uAtlasGrid;
    vec4 g = texture(uAtlas, auv);
    float cov = g.a;

    vec3 fg = uColorGlyphs != 0 ? src : uInk.rgb;
    vec3 bg = uBackgroundFill != 0 ? uPaper.rgb : uLetterbox.rgb;
    vec3 col = mix(bg, fg, cov);

    if (uShowGrid != 0) {
        vec2 d = min(local, 1.0 - local) * uGrid;
        col = mix(vec3(0.15, 0.22, 0.15), col, smoothstep(0.0, 0.08, min(d.x, d.y)));
    }
    if (uScanlines != 0) {
        float rows = uScanlineCount > 0.5 ? uScanlineCount : uGrid.y;
        float sy = (gl_FragCoord.y - uViewport.y) / max(uViewport.w, 1.0) * rows;
        col *= 1.0 - uScanlineStrength * step(0.5, fract(sy * 0.5));
    }

    fragColor = vec4(clamp(col, 0.0, 1.0), 1.0);
}
)GLSL";

// ---------------------------------------------------------------------------
// Небольшие GL-помощники
// ---------------------------------------------------------------------------
// Перечисления, которые GL.h движка не объявляет, но нужны glGetIntegerv.
constexpr gl::GLenum kGlTextureBinding2D = 0x8069;
constexpr gl::GLenum kGlActiveTexture = 0x84E0;
constexpr gl::GLenum kGlCurrentProgram = 0x8B8D;
constexpr gl::GLenum kGlVertexArrayBinding = 0x85B5;
constexpr gl::GLenum kGlViewport = 0x0BA2;
constexpr gl::GLenum kGlBlendSrcRgb = 0x80C9;
constexpr gl::GLenum kGlBlendDstRgb = 0x80C8;
constexpr gl::GLenum kGlBlendSrcAlpha = 0x80CB;
constexpr gl::GLenum kGlBlendDstAlpha = 0x80CA;
constexpr gl::GLenum kGlBlendEquationRgb = 0x8009;
constexpr gl::GLenum kGlBlendEquationAlpha = 0x800A;

// Сохраняет и восстанавливает всё GL-состояние, которое трогает EndFrame.
struct GlStateGuard {
    gl::GLboolean blend = 0, depth = 0, cull = 0, scissor = 0, depthMask = 1;
    gl::GLint program = 0, vao = 0, fbo = 0, activeTex = 0, viewport[4] = {0, 0, 0, 0};
    gl::GLint tex[3] = {0, 0, 0};
    gl::GLint blendSrcRgb = 0, blendDstRgb = 0, blendSrcA = 0, blendDstA = 0;
    gl::GLint blendEqRgb = 0, blendEqA = 0;
    bool valid = false;

    void Save() {
        if (!gl::glGetIntegerv) return;
        blend = gl::glIsEnabled(gl::GL_BLEND);
        depth = gl::glIsEnabled(gl::GL_DEPTH_TEST);
        cull = gl::glIsEnabled(gl::GL_CULL_FACE);
        scissor = gl::glIsEnabled(gl::GL_SCISSOR_TEST);
        gl::glGetBooleanv(gl::GL_DEPTH_WRITEMASK, &depthMask);
        gl::glGetIntegerv(kGlCurrentProgram, &program);
        gl::glGetIntegerv(kGlVertexArrayBinding, &vao);
        gl::glGetIntegerv(gl::GL_FRAMEBUFFER_BINDING, &fbo);
        gl::glGetIntegerv(kGlActiveTexture, &activeTex);
        gl::glGetIntegerv(kGlViewport, viewport);
        gl::glGetIntegerv(kGlBlendSrcRgb, &blendSrcRgb);
        gl::glGetIntegerv(kGlBlendDstRgb, &blendDstRgb);
        gl::glGetIntegerv(kGlBlendSrcAlpha, &blendSrcA);
        gl::glGetIntegerv(kGlBlendDstAlpha, &blendDstA);
        gl::glGetIntegerv(kGlBlendEquationRgb, &blendEqRgb);
        gl::glGetIntegerv(kGlBlendEquationAlpha, &blendEqA);
        for (int i = 0; i < 3; ++i) {
            gl::glActiveTexture(static_cast<gl::GLenum>(gl::GL_TEXTURE0 + i));
            gl::glGetIntegerv(kGlTextureBinding2D, &tex[i]);
        }
        gl::glActiveTexture(static_cast<gl::GLenum>(gl::GL_TEXTURE0));
        valid = true;
    }

    void Restore() const {
        if (!valid) return;
        gl::glUseProgram(static_cast<gl::GLuint>(program));
        gl::glBindVertexArray(static_cast<gl::GLuint>(vao));
        for (int i = 0; i < 3; ++i) {
            gl::glActiveTexture(static_cast<gl::GLenum>(gl::GL_TEXTURE0 + i));
            gl::glBindTexture(gl::GL_TEXTURE_2D, static_cast<gl::GLuint>(tex[i]));
        }
        gl::glActiveTexture(static_cast<gl::GLenum>(activeTex));
        gl::glBlendEquationSeparate(static_cast<gl::GLenum>(blendEqRgb),
                                    static_cast<gl::GLenum>(blendEqA));
        gl::glBlendFuncSeparate(static_cast<gl::GLenum>(blendSrcRgb),
                                static_cast<gl::GLenum>(blendDstRgb),
                                static_cast<gl::GLenum>(blendSrcA),
                                static_cast<gl::GLenum>(blendDstA));
        if (blend)
            gl::glEnable(gl::GL_BLEND);
        else
            gl::glDisable(gl::GL_BLEND);
        if (depth)
            gl::glEnable(gl::GL_DEPTH_TEST);
        else
            gl::glDisable(gl::GL_DEPTH_TEST);
        if (cull)
            gl::glEnable(gl::GL_CULL_FACE);
        else
            gl::glDisable(gl::GL_CULL_FACE);
        if (scissor)
            gl::glEnable(gl::GL_SCISSOR_TEST);
        else
            gl::glDisable(gl::GL_SCISSOR_TEST);
        gl::glDepthMask(depthMask);
        gl::glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
        gl::glBindFramebuffer(gl::GL_FRAMEBUFFER, static_cast<gl::GLuint>(fbo));
    }
};

// Загружает четыре clip-space вершины для прямоугольника в координатах
// фреймбуфера (начало сверху слева, y вниз) и рисует их как triangle strip.
// UV отображают 0..1 на прямоугольник, v = 0 сверху — виртуальное изображение сверху вниз.
void DrawRetroQuad(unsigned int vao, unsigned int vbo, f32 x, f32 y, f32 w, f32 h, f32 fbW,
                   f32 fbH) {
    if (vao == 0 || vbo == 0 || fbW <= 0 || fbH <= 0) return;
    auto clipX = [&](f32 px) { return 2.0f * px / fbW - 1.0f; };
    auto clipY = [&](f32 py) { return 1.0f - 2.0f * py / fbH; };
    const f32 verts[16] = {
        clipX(x),     clipY(y),     0.0f, 0.0f,
        clipX(x + w), clipY(y),     1.0f, 0.0f,
        clipX(x),     clipY(y + h), 0.0f, 1.0f,
        clipX(x + w), clipY(y + h), 1.0f, 1.0f,
    };
    gl::glBindVertexArray(vao);
    gl::glBindBuffer(gl::GL_ARRAY_BUFFER, vbo);
    gl::glBufferSubData(gl::GL_ARRAY_BUFFER, 0, static_cast<gl::GLsizeiptr>(sizeof(verts)), verts);
    gl::glDrawArrays(gl::GL_TRIANGLE_STRIP, 0, 4);
}

// Создаёт (или обновляет) текстуру палитры Nx1 RGBA8 для пиксельного шейдера.
void UploadPalette(Texture& texture, RetroPalette& cached, RetroPalette kind) {
    int n = RetroDisplay::PaletteSize(kind);
    if (n <= 0) return;
    if (texture.Valid() && cached == kind) return;
    std::vector<u8> data(static_cast<usize>(64) * 4u, 0);
    for (int i = 0; i < n; ++i) {
        Color c = RetroDisplay::PaletteColor(kind, i);
        data[static_cast<usize>(i) * 4u + 0] =
            static_cast<u8>(Clamp(c.r * 255.0f + 0.5f, 0.0f, 255.0f));
        data[static_cast<usize>(i) * 4u + 1] =
            static_cast<u8>(Clamp(c.g * 255.0f + 0.5f, 0.0f, 255.0f));
        data[static_cast<usize>(i) * 4u + 2] =
            static_cast<u8>(Clamp(c.b * 255.0f + 0.5f, 0.0f, 255.0f));
        data[static_cast<usize>(i) * 4u + 3] = 255;
    }
    if (!texture.Create(64, 1, PixelFormat::RGBA8, data.data(), TextureFilter::Nearest,
                        TextureWrap::ClampToEdge)) {
        ENG_LOGW("retro", "failed to upload the %s palette", RetroDisplay::PaletteName(kind));
        return;
    }
    cached = kind;
}

// Наибольший целый масштаб, при котором `vw x vh` помещается в `fbW x fbH`.
int IntegerScaleFor(f32 fbW, f32 fbH, f32 vw, f32 vh) {
    if (vw <= 0 || vh <= 0) return 1;
    int k = static_cast<int>(std::floor(std::min(fbW / vw, fbH / vh)));
    return k < 1 ? 1 : k;
}

// Встроенный процедурный битмап-шрифт сообщает, что любой кодпоинт есть (для
// неизвестных символов он синтезирует полую рамку), поэтому глифы блоков и
// брайля доверяем только настоящим файлам шрифтов.
bool GlyphAvailable(const Font& font, u32 codepoint) {
    if (codepoint < 0x80) return true;
    if (font.Format() == FontFormat::Bitmap && font.SourcePath().empty()) return false;
    return font.HasGlyph(codepoint);
}

}  // namespace

// ---------------------------------------------------------------------------
// Impl
// ---------------------------------------------------------------------------
struct RetroDisplay::Impl {
    Shader pixel;
    Shader ascii;
    unsigned int vao = 0, vbo = 0;
    Texture palette;
    RetroPalette paletteKind = RetroPalette::Count;
    bool ready = false;
    bool boundVirtual = false;
    RetroSettings settings{};
    AsciiCharset charset = AsciiCharset::Ramp10;
    std::string customRamp;
    // Ключ кэша атласа, который сейчас находится в `asciiAtlas_`.
    AsciiCharset atlasCharset = AsciiCharset::Count;
    std::string atlasCustom;
    bool atlasFallbackLogged = false;
    bool atlasAutoFailed = false;
};

// ---------------------------------------------------------------------------
// Время жизни
// ---------------------------------------------------------------------------
RetroDisplay::RetroDisplay() : impl_(new Impl()) {}

RetroDisplay::~RetroDisplay() { Shutdown(); }

bool RetroDisplay::Init() {
    if (initialized_ && impl_ && impl_->ready) return true;
    if (!impl_) impl_.reset(new Impl());
    if (!gl::glCreateShader) {
        ENG_LOGE("retro", "no GL context; retro display unavailable");
        return false;
    }
    impl_->ready = impl_->pixel.Build(kRetroQuadVert, kRetroPixelFrag, "retro-pixel") &&
                   impl_->ascii.Build(kRetroQuadVert, kRetroAsciiFrag, "retro-ascii");
    if (!impl_->ready) {
        ENG_LOGE("retro", "shader build failed: %s", impl_->pixel.Log().c_str());
        return false;
    }

    if (impl_->vao == 0 && gl::glGenVertexArrays) {
        gl::glGenVertexArrays(1, &impl_->vao);
        gl::glGenBuffers(1, &impl_->vbo);
        gl::glBindVertexArray(impl_->vao);
        gl::glBindBuffer(gl::GL_ARRAY_BUFFER, impl_->vbo);
        gl::glBufferData(gl::GL_ARRAY_BUFFER, static_cast<gl::GLsizeiptr>(4 * 4 * sizeof(f32)),
                         nullptr, gl::GL_DYNAMIC_DRAW);
        gl::glEnableVertexAttribArray(0);
        gl::glVertexAttribPointer(0, 2, gl::GL_FLOAT, gl::GL_FALSE, 4 * sizeof(f32), nullptr);
        gl::glEnableVertexAttribArray(1);
        gl::glVertexAttribPointer(1, 2, gl::GL_FLOAT, gl::GL_FALSE, 4 * sizeof(f32),
                                  reinterpret_cast<const void*>(2 * sizeof(f32)));
        gl::glBindVertexArray(0);
        gl::glBindBuffer(gl::GL_ARRAY_BUFFER, 0);
    }

    initialized_ = true;
    impl_->paletteKind = RetroPalette::Count;
    ENG_LOGI("retro", "retro display initialised");
    return true;
}

void RetroDisplay::Shutdown() {
    if (!impl_) return;
    if (gl::glDeleteVertexArrays && impl_->vao) gl::glDeleteVertexArrays(1, &impl_->vao);
    if (gl::glDeleteBuffers && impl_->vbo) gl::glDeleteBuffers(1, &impl_->vbo);
    impl_->vao = 0;
    impl_->vbo = 0;
    impl_->pixel.Destroy();
    impl_->ascii.Destroy();
    impl_->palette.Destroy();
    impl_->ready = false;
    impl_->boundVirtual = false;
    impl_->paletteKind = RetroPalette::Count;
    impl_->atlasCharset = AsciiCharset::Count;
    impl_->atlasCustom.clear();
    impl_->atlasAutoFailed = false;

    asciiAtlas_.Destroy();
    asciiFont_ = nullptr;
    asciiCellW_ = asciiCellH_ = 0;
    asciiCellsX_ = asciiCellsY_ = 0;
    asciiRampCount_ = 0;

    virtual_.reset();
    virtualW_ = virtualH_ = 0;
    initialized_ = false;
}

bool RetroDisplay::Valid() const { return initialized_ && impl_ && impl_->ready; }

// ---------------------------------------------------------------------------
// Размеры
// ---------------------------------------------------------------------------
void RetroDisplay::Resize(int fbWidth, int fbHeight, const RetroSettings& settings) {
    if (!Valid() && !Init()) return;
    fbW_ = MaxT(1, fbWidth);
    fbH_ = MaxT(1, fbHeight);
    impl_->settings = settings;
    impl_->charset = settings.charset;
    impl_->customRamp = settings.customRamp;

    int vw = Clamp(settings.virtualWidth, 64, 1024);
    int vh = Clamp(settings.virtualHeight, 64, 1024);
    int cols = Clamp(settings.asciiCols, 1, 400);
    int rows = settings.asciiRows > 0
                   ? Clamp(settings.asciiRows, 1, 200)
                   : retro::DeriveAsciiRows(cols, vw, vh, settings.asciiCellAspect);

    if (!virtual_ || !virtual_->Valid() || virtualW_ != vw || virtualH_ != vh) {
        std::unique_ptr<RenderTarget> target(new RenderTarget());
        RenderTargetDesc desc;
        desc.width = vw;
        desc.height = vh;
        desc.samples = 1;
        desc.depth = true;
        desc.stencil = false;
        desc.colorAsTexture = true;
        desc.colorFormat = PixelFormat::RGBA8;
        desc.filter = TextureFilter::Nearest;
        desc.wrap = TextureWrap::ClampToEdge;
        desc.name = "retro-virtual";
        if (!target->Create(desc)) {
            ENG_LOGE("retro", "failed to create the %dx%d virtual framebuffer", vw, vh);
            virtual_.reset();
            virtualW_ = virtualH_ = 0;
            return;
        }
        target->ColorTexture().SetFilter(TextureFilter::Nearest);
        virtual_ = std::move(target);
    }
    virtualW_ = vw;
    virtualH_ = vh;

    stats_ = Stats{};
    stats_.virtualWidth = vw;
    stats_.virtualHeight = vh;
    stats_.scale = IntegerScaleFor(static_cast<f32>(fbW_), static_cast<f32>(fbH_),
                                   static_cast<f32>(vw), static_cast<f32>(vh));
    stats_.paletteColors = PaletteSize(settings.palette);
    stats_.asciiCols = cols;
    stats_.asciiRows = rows;
    stats_.usedAscii = settings.mode == RetroMode::Ascii;
}

Rect RetroDisplay::ViewportRect(int fbWidth, int fbHeight, const RetroSettings& s) const {
    f32 vw = static_cast<f32>(virtualW_ > 0 ? virtualW_ : Clamp(s.virtualWidth, 64, 1024));
    f32 vh = static_cast<f32>(virtualH_ > 0 ? virtualH_ : Clamp(s.virtualHeight, 64, 1024));
    f32 fbW = static_cast<f32>(MaxT(1, fbWidth));
    f32 fbH = static_cast<f32>(MaxT(1, fbHeight));
    if (vw <= 0 || vh <= 0) return Rect{0, 0, fbW, fbH};

    f32 w = 0, h = 0;
    int k = IntegerScaleFor(fbW, fbH, vw, vh);
    if (s.integerScale && k >= 1 && vw * static_cast<f32>(k) <= fbW &&
        vh * static_cast<f32>(k) <= fbH) {
        w = vw * static_cast<f32>(k);
        h = vh * static_cast<f32>(k);
    } else {
        // Либо запрошено дробное масштабирование, либо даже 1:1 не помещается.
        f32 fit = std::min(fbW / vw, fbH / vh);
        w = vw * fit;
        h = vh * fit;
    }
    f32 x = std::floor((fbW - w) * 0.5f);
    f32 y = std::floor((fbH - h) * 0.5f);
    return Rect{x, y, w, h};
}

Vec2 RetroDisplay::MapToVirtual(Vec2 point, int fbWidth, int fbHeight,
                                const RetroSettings& s) const {
    Rect vp = ViewportRect(fbWidth, fbHeight, s);
    if (vp.w <= 0 || vp.h <= 0) return Vec2{0, 0};
    f32 vw = static_cast<f32>(virtualW_ > 0 ? virtualW_ : Clamp(s.virtualWidth, 64, 1024));
    f32 vh = static_cast<f32>(virtualH_ > 0 ? virtualH_ : Clamp(s.virtualHeight, 64, 1024));
    return Vec2{(point.x - vp.x) / vp.w * vw, (point.y - vp.y) / vp.h * vh};
}

Vec2 RetroDisplay::MapFromVirtual(Vec2 point, int fbWidth, int fbHeight,
                                  const RetroSettings& s) const {
    Rect vp = ViewportRect(fbWidth, fbHeight, s);
    f32 vw = static_cast<f32>(virtualW_ > 0 ? virtualW_ : Clamp(s.virtualWidth, 64, 1024));
    f32 vh = static_cast<f32>(virtualH_ > 0 ? virtualH_ : Clamp(s.virtualHeight, 64, 1024));
    if (vw <= 0 || vh <= 0) return Vec2{vp.x, vp.y};
    return Vec2{vp.x + point.x / vw * vp.w, vp.y + point.y / vh * vp.h};
}

// ---------------------------------------------------------------------------
// Кадры
// ---------------------------------------------------------------------------
Rect RetroDisplay::BeginFrame(Renderer2D& r2d, const RetroSettings& settings, f32 dpiScale) {
    if (!Valid() && !Init()) return Rect{0, 0, 0, 0};
    impl_->settings = settings;
    impl_->charset = settings.charset;
    impl_->customRamp = settings.customRamp;
    f32 dpi = dpiScale > 0 ? dpiScale : 1.0f;

    // Не требуем обязательного Resize(): откатываемся к запрошенному виртуальному
    // размеру как к размеру фреймбуфера, чтобы режим всё равно работал.
    if (settings.mode != RetroMode::Off && (!virtual_ || !virtual_->Valid())) {
        Resize(fbW_ > 0 ? fbW_ : Clamp(settings.virtualWidth, 64, 1024),
               fbH_ > 0 ? fbH_ : Clamp(settings.virtualHeight, 64, 1024), settings);
    }

    if (settings.mode == RetroMode::Off || !virtual_ || !virtual_->Valid()) {
        impl_->boundVirtual = false;
        int w = fbW_ > 0 ? fbW_ : 1;
        int h = fbH_ > 0 ? fbH_ : 1;
        r2d.BeginFrame(w, h, dpi, nullptr);
        return Rect{0, 0, static_cast<f32>(w) / dpi, static_cast<f32>(h) / dpi};
    }

    virtual_->Bind();
    impl_->boundVirtual = true;
    // Внутри виртуального изображения одна логическая единица — ровно один
    // виртуальный пиксель, поэтому сцены верстаются в низких пиксельных координатах.
    r2d.BeginFrame(virtualW_, virtualH_, 1.0f, virtual_.get());
    stats_.usedAscii = settings.mode == RetroMode::Ascii;
    stats_.virtualWidth = virtualW_;
    stats_.virtualHeight = virtualH_;
    stats_.paletteColors = PaletteSize(settings.palette);
    return Rect{0, 0, static_cast<f32>(virtualW_), static_cast<f32>(virtualH_)};
}

void RetroDisplay::EndFrame(Renderer2D& r2d, const RetroSettings& settings, int fbWidth,
                            int fbHeight) {
    if (!Valid()) return;
    if (!impl_->boundVirtual) {
        r2d.EndFrame();
        return;
    }
    r2d.EndFrame();
    if (virtual_) virtual_->Unbind();
    impl_->boundVirtual = false;

    fbW_ = MaxT(1, fbWidth);
    fbH_ = MaxT(1, fbHeight);
    impl_->settings = settings;
    impl_->charset = settings.charset;
    impl_->customRamp = settings.customRamp;
    const bool ascii = settings.mode == RetroMode::Ascii;
    stats_.usedAscii = ascii;
    stats_.virtualWidth = virtualW_;
    stats_.virtualHeight = virtualH_;
    stats_.paletteColors = PaletteSize(settings.palette);
    stats_.scale = IntegerScaleFor(static_cast<f32>(fbW_), static_cast<f32>(fbH_),
                                   static_cast<f32>(MaxT(1, virtualW_)),
                                   static_cast<f32>(MaxT(1, virtualH_)));
    const int cols = Clamp(settings.asciiCols, 1, 400);
    const int rows = settings.asciiRows > 0
                         ? Clamp(settings.asciiRows, 1, 200)
                         : retro::DeriveAsciiRows(cols, MaxT(1, virtualW_), MaxT(1, virtualH_),
                                                  settings.asciiCellAspect);
    stats_.asciiCols = cols;
    stats_.asciiRows = rows;
    if (virtualW_ <= 0 || virtualH_ <= 0 || !virtual_ || !virtual_->Valid()) return;

    // Целевой FBO для разбора: то, что было привязано до BeginFrame.
    gl::GLint targetFbo = 0;
    if (gl::glGetIntegerv) gl::glGetIntegerv(gl::GL_FRAMEBUFFER_BINDING, &targetFbo);

    GlStateGuard state;
    state.Save();

    // Смена набора символов инвалидирует текущий атлас глифов.
    if (ascii && asciiAtlas_.Valid() &&
        (impl_->atlasCharset != settings.charset || impl_->atlasCustom != settings.customRamp)) {
        asciiAtlas_.Destroy();
        asciiFont_ = nullptr;
        asciiCellsX_ = asciiCellsY_ = 0;
        asciiRampCount_ = 0;
    }

    // ASCII-режиму нужен атлас глифов; строим его лениво из шрифта по умолчанию,
    // чтобы режим работал сразу.  Это делается до применения состояния разбора,
    // чтобы изменения GL-состояния откатил state.Restore().
    if (ascii && !asciiAtlas_.Valid() && !impl_->atlasAutoFailed) {
        Font* font = FontManager::Get().DefaultFont();
        Rect vp = ViewportRect(fbW_, fbH_, settings);
        int cellW = Clamp(static_cast<int>(std::lround(vp.w / static_cast<f32>(cols))), 4, 64);
        int cellH = Clamp(static_cast<int>(std::lround(vp.h / static_cast<f32>(rows))), 4, 64);
        if (!font || !BuildAsciiAtlas(font, cellW, cellH)) {
            impl_->atlasAutoFailed = true;
            ENG_LOGW("retro", "ASCII mode requested but no glyph atlas could be built");
        }
    }

    if (ascii && !asciiAtlas_.Valid()) {
        // Рисовать глифы нечем: оставляем кадр нетронутым, а не делаем
        // в нём дыру.
        state.Restore();
        return;
    }

    gl::glDisable(gl::GL_BLEND);
    gl::glDisable(gl::GL_DEPTH_TEST);
    gl::glDepthMask(0);
    gl::glDisable(gl::GL_CULL_FACE);
    gl::glDisable(gl::GL_SCISSOR_TEST);
    gl::glBlendEquation(gl::GL_FUNC_ADD);
    gl::glViewport(0, 0, fbW_, fbH_);
    gl::glBindFramebuffer(gl::GL_FRAMEBUFFER, static_cast<gl::GLuint>(targetFbo));

    Rect vp = ViewportRect(fbW_, fbH_, settings);
    if (vp.w < 1) vp.w = 1;
    if (vp.h < 1) vp.h = 1;
    const f32 fbW = static_cast<f32>(fbW_);
    const f32 fbH = static_cast<f32>(fbH_);
    // Пространство GL viewport/gl_FragCoord (начало снизу слева).
    Vec4 vpGL{vp.x, fbH - (vp.y + vp.h), vp.w, vp.h};

    // 1. Заливка letterbox / фона.
    impl_->pixel.Bind();
    impl_->pixel.Set("uMode", 0);
    impl_->pixel.Set("uLetterbox", settings.letterbox);
    impl_->pixel.Set("uTexture", 0);
    impl_->pixel.Set("uPalette", 1);
    DrawRetroQuad(impl_->vao, impl_->vbo, 0, 0, fbW, fbH, fbW, fbH);

    // 2. Виртуальное изображение, расширенное небольшой рамкой overscan.
    f32 border = 0.0f;
    if (settings.showOverscan) border = MaxT(2.0f, std::floor(MaxT(vp.w, vp.h) * 0.012f));

    if (ascii) {
        Shader& sh = impl_->ascii;
        sh.Bind();
        sh.Set("uMode", 1);
        sh.SetTexture("uTexture", virtual_->ColorTexture(), 0);
        sh.SetTexture("uAtlas", asciiAtlas_, 1);
        sh.Set("uViewport", vpGL);
        sh.Set("uVirtualSize", Vec2{static_cast<f32>(virtualW_), static_cast<f32>(virtualH_)});
        sh.Set("uLetterbox", settings.letterbox);
        sh.Set("uGrid", Vec2{static_cast<f32>(cols), static_cast<f32>(rows)});
        sh.Set("uAtlasGrid", Vec2{static_cast<f32>(MaxT(1, asciiCellsX_)),
                                  static_cast<f32>(MaxT(1, asciiCellsY_))});
        sh.Set("uRampCount", asciiRampCount_ > 0 ? asciiRampCount_ : 1);
        sh.Set("uGamma", settings.asciiGamma);
        sh.Set("uContrast", settings.asciiContrast);
        sh.Set("uBrightness", settings.asciiBrightness);
        sh.Set("uInvert", settings.asciiInvert ? 1 : 0);
        sh.Set("uColorGlyphs", settings.asciiColor ? 1 : 0);
        sh.Set("uInk", settings.asciiInk);
        sh.Set("uPaper", settings.asciiPaper);
        sh.Set("uBackgroundFill", settings.asciiBackgroundFill ? 1 : 0);
        sh.Set("uShowGrid", settings.asciiShowGrid ? 1 : 0);
        sh.Set("uScanlines", settings.asciiScanlines ? 1 : 0);
        sh.Set("uScanlineStrength", settings.asciiScanlineStrength);
        sh.Set("uScanlineCount", 0.0f);
        sh.Set("uOverscan", settings.showOverscan ? 1 : 0);
        DrawRetroQuad(impl_->vao, impl_->vbo, vp.x - border, vp.y - border, vp.w + border * 2.0f,
                      vp.h + border * 2.0f, fbW, fbH);
    } else {
        UploadPalette(impl_->palette, impl_->paletteKind, settings.palette);
        int paletteSize = PaletteSize(settings.palette);
        Shader& sh = impl_->pixel;
        sh.Set("uMode", 1);
        sh.SetTexture("uTexture", virtual_->ColorTexture(), 0);
        if (impl_->palette.Valid()) sh.SetTexture("uPalette", impl_->palette, 1);
        sh.Set("uViewport", vpGL);
        sh.Set("uVirtualSize", Vec2{static_cast<f32>(virtualW_), static_cast<f32>(virtualH_)});
        sh.Set("uLetterbox", settings.letterbox);
        sh.Set("uUsePalette", paletteSize > 0 ? 1 : 0);
        sh.Set("uPaletteSize", paletteSize);
        sh.Set("uColorDepth", settings.colorDepth);
        sh.Set("uDither", settings.dither ? 1 : 0);
        sh.Set("uDitherMatrix",
               settings.ditherMatrix >= 8 ? 8 : (settings.ditherMatrix >= 4 ? 4 : 2));
        sh.Set("uDitherStrength", settings.ditherStrength);
        sh.Set("uScanlines", settings.scanlines ? 1 : 0);
        sh.Set("uScanlineStrength", settings.scanlineStrength);
        sh.Set("uScanlineCount", settings.scanlineCount);
        sh.Set("uCurvature", settings.crtCurvature ? 1 : 0);
        sh.Set("uCurvatureAmount", settings.curvature);
        sh.Set("uMask", settings.crtMask ? 1 : 0);
        sh.Set("uMaskStrength", settings.crtMaskStrength);
        sh.Set("uBloom", settings.bloom);
        sh.Set("uBrightness", settings.brightness);
        sh.Set("uContrast", settings.contrast);
        sh.Set("uSaturation", settings.saturation);
        sh.Set("uOverscan", settings.showOverscan ? 1 : 0);
        DrawRetroQuad(impl_->vao, impl_->vbo, vp.x - border, vp.y - border, vp.w + border * 2.0f,
                      vp.h + border * 2.0f, fbW, fbH);
    }

    gl::glBindVertexArray(0);
    state.Restore();
}

// ---------------------------------------------------------------------------
// Bayer
// ---------------------------------------------------------------------------
f32 RetroDisplay::BayerThreshold(int x, int y, int matrixSize) {
    // M2 = [[0,2],[3,1]]/4 — идентично bayer2() из шейдера.
    auto m2 = [](int px, int py) {
        static const int kM[2][2] = {{0, 2}, {3, 1}};
        return static_cast<f32>(kM[py & 1][px & 1]) * 0.25f;
    };
    int size = matrixSize >= 8 ? 8 : (matrixSize >= 4 ? 4 : 2);
    if (size == 2) return m2(x, y);
    if (size == 4) {
        // bayer4(a) = bayer2(a/2) * 0.25 + bayer2(a)
        return m2(x >> 1, y >> 1) * 0.25f + m2(x, y);
    }
    // bayer8(a) = bayer4(a/2) * 0.25 + bayer2(a)
    f32 b4 = m2(x >> 2, y >> 2) * 0.25f + m2(x >> 1, y >> 1);
    return b4 * 0.25f + m2(x, y);
}

// ---------------------------------------------------------------------------
// ASCII atlas
// ---------------------------------------------------------------------------
// Ряд растеризуется через Renderer2D в offscreen-цель RGBA8 и читается назад
// через ReadPixels («путь чтения назад»): один глиф на ячейку сетки, белые
// премультиплированные чернила на прозрачном, фильтр NEAREST.  Шейдер использует
// только альфа-канал как покрытие, поэтому атлас не зависит от разрешения.
bool RetroDisplay::BuildAsciiAtlas(Font* font, int cellW, int cellH) {
    if (!font || !font->Valid() || cellW <= 0 || cellH <= 0) return false;
    if (!Valid() && !Init()) return false;

    AsciiCharset charset = impl_ ? impl_->charset : AsciiCharset::Ramp10;
    std::string custom = impl_ ? impl_->customRamp : std::string();
    if (asciiAtlas_.Valid() && font == asciiFont_ && cellW == asciiCellW_ && cellH == asciiCellH_ &&
        impl_ && charset == impl_->atlasCharset && custom == impl_->atlasCustom) {
        return true;
    }

    std::vector<u32> ramp = CharsetRamp(charset, custom);
    if (ramp.size() < 2) return false;
    const int count = static_cast<int>(ramp.size());
    const int cellsX = MinT(count, 16);
    const int cellsY = (count + cellsX - 1) / cellsX;
    const int texW = cellsX * cellW;
    const int texH = cellsY * cellH;
    if (texW <= 0 || texH <= 0) return false;

    std::vector<u32> fallback = CharsetRamp(AsciiCharset::Ramp70, std::string());
    if (fallback.empty()) fallback = ramp;

    RenderTarget target;
    RenderTargetDesc desc;
    desc.width = texW;
    desc.height = texH;
    desc.samples = 1;
    desc.depth = false;
    desc.stencil = false;
    desc.colorFormat = PixelFormat::RGBA8;
    desc.filter = TextureFilter::Nearest;
    desc.wrap = TextureWrap::ClampToEdge;
    desc.name = "retro-ascii-atlas";
    if (!target.Create(desc)) return false;
    target.Bind();
    target.Clear(Color{0, 0, 0, 0}, false, false);

    Renderer2D r2d;
    if (!r2d.Init()) {
        target.Unbind();
        return false;
    }
    r2d.BeginFrame(texW, texH, 1.0f, &target);
    const f32 glyphSize = static_cast<f32>(cellH) * 0.86f;
    bool loggedFallback = false;
    for (int i = 0; i < count; ++i) {
        u32 cp = ramp[static_cast<usize>(i)];
        if (!GlyphAvailable(*font, cp)) {
            usize fi = MinT(static_cast<usize>(i), fallback.size() - 1);
            if (!loggedFallback) {
                ENG_LOGW("retro",
                         "font '%s' has no glyph for U+%04X; falling back to the Ramp70 "
                         "character at the same ramp index",
                         font->FamilyName().c_str(), cp);
                loggedFallback = true;
            }
            cp = fallback[fi];
        }
        std::string utf8 = CodepointsToUtf8({cp});
        int cx = i % cellsX;
        int cy = i / cellsX;
        r2d.DrawText(*font, utf8, (static_cast<f32>(cx) + 0.5f) * static_cast<f32>(cellW),
                     (static_cast<f32>(cy) + 0.5f) * static_cast<f32>(cellH), Color::White,
                     glyphSize, TextAlign::Center, TextBaseline::Middle);
    }
    r2d.EndFrame();
    r2d.Shutdown();

    std::vector<u8> pixels;
    bool ok = target.ReadPixels(&pixels);
    target.Unbind();
    if (!ok || pixels.size() < static_cast<usize>(texW) * static_cast<usize>(texH) * 4u) {
        ENG_LOGW("retro", "failed to read back the ASCII atlas");
        return false;
    }

    asciiAtlas_.Destroy();
    if (!asciiAtlas_.Create(texW, texH, PixelFormat::RGBA8, pixels.data(), TextureFilter::Nearest,
                            TextureWrap::ClampToEdge)) {
        return false;
    }
    asciiFont_ = font;
    asciiCellW_ = cellW;
    asciiCellH_ = cellH;
    asciiCellsX_ = cellsX;
    asciiCellsY_ = cellsY;
    asciiRampCount_ = count;
    impl_->atlasCharset = charset;
    impl_->atlasCustom = custom;
    impl_->atlasFallbackLogged = loggedFallback;
    ENG_LOGI("retro", "ASCII atlas: %d glyphs in a %dx%d grid (%dx%d px cells)", count, cellsX,
             cellsY, cellW, cellH);
    return true;
}

// ---------------------------------------------------------------------------
// Пресеты
// ---------------------------------------------------------------------------
std::vector<RetroPreset> BuiltinRetroPresets() {
    std::vector<RetroPreset> presets;

    {
        RetroSettings s;
        s.mode = RetroMode::Pixel;
        s.virtualWidth = 256;
        s.virtualHeight = 240;
        s.palette = RetroPalette::Nes;
        s.letterbox = Color::FromRGB(0x000000);
        presets.push_back({"NES", s});
    }
    {
        RetroSettings s;
        s.mode = RetroMode::Pixel;
        s.virtualWidth = 160;
        s.virtualHeight = 144;
        s.palette = RetroPalette::GameBoy;
        s.letterbox = Color::FromRGB(0x0F380F);
        presets.push_back({"Game Boy", s});
    }
    {
        RetroSettings s;
        s.mode = RetroMode::Pixel;
        s.virtualWidth = 160;
        s.virtualHeight = 144;
        s.palette = RetroPalette::GameBoyPocket;
        presets.push_back({"Game Boy Pocket", s});
    }
    {
        RetroSettings s;
        s.mode = RetroMode::Pixel;
        s.virtualWidth = 320;
        s.virtualHeight = 200;
        s.palette = RetroPalette::C64;
        s.letterbox = Color::FromRGB(0x352879);
        presets.push_back({"Commodore 64", s});
    }
    {
        RetroSettings s;
        s.mode = RetroMode::Pixel;
        s.virtualWidth = 256;
        s.virtualHeight = 192;
        s.palette = RetroPalette::ZxSpectrum;
        presets.push_back({"ZX Spectrum", s});
    }
    {
        RetroSettings s;
        s.mode = RetroMode::Pixel;
        s.virtualWidth = 128;
        s.virtualHeight = 128;
        s.palette = RetroPalette::Pico8;
        s.letterbox = Color::FromRGB(0x000000);
        presets.push_back({"PICO-8", s});
    }
    {
        RetroSettings s;
        s.mode = RetroMode::Pixel;
        s.virtualWidth = 320;
        s.virtualHeight = 200;
        s.palette = RetroPalette::Ega64;
        s.dither = true;
        s.ditherMatrix = 4;
        s.ditherStrength = 0.9f;
        s.letterbox = Color::FromRGB(0x000000);
        presets.push_back({"EGA 64", s});
    }
    {
        // Классический 1-битный Macintosh: 512x342, упорядоченный дизеринг вместо серого.
        RetroSettings s;
        s.mode = RetroMode::Pixel;
        s.virtualWidth = 512;
        s.virtualHeight = 342;
        s.integerScale = false;
        s.palette = RetroPalette::Mono;
        s.dither = true;
        s.ditherMatrix = 8;
        s.ditherStrength = 1.0f;
        s.scanlines = true;
        s.scanlineStrength = 0.10f;
        presets.push_back({"Mono Mac", s});
    }
    {
        RetroSettings s;
        s.mode = RetroMode::Pixel;
        s.virtualWidth = 320;
        s.virtualHeight = 240;
        s.integerScale = false;
        s.palette = RetroPalette::None;
        s.scanlines = true;
        s.scanlineStrength = 0.32f;
        s.crtCurvature = true;
        s.curvature = 0.08f;
        s.crtMask = true;
        s.crtMaskStrength = 0.30f;
        s.bloom = 0.55f;
        s.showOverscan = true;
        s.letterbox = Color{0.015f, 0.015f, 0.02f, 1.0f};
        presets.push_back({"CRT Arcade", s});
    }
    {
        RetroSettings s;
        s.mode = RetroMode::Ascii;
        s.virtualWidth = 320;
        s.virtualHeight = 200;
        s.asciiCols = 80;
        s.asciiRows = 25;
        s.charset = AsciiCharset::Ramp10;
        s.asciiColor = false;
        s.asciiInk = Color::FromRGB(0x33FF66);
        s.asciiPaper = Color{0.01f, 0.03f, 0.01f, 1.0f};
        s.asciiContrast = 1.25f;
        s.asciiScanlines = true;
        s.asciiScanlineStrength = 0.22f;
        s.letterbox = Color::FromRGB(0x000000);
        presets.push_back({"ASCII Terminal (Green)", s});
    }
    {
        RetroSettings s;
        s.mode = RetroMode::Ascii;
        s.virtualWidth = 320;
        s.virtualHeight = 200;
        s.asciiCols = 80;
        s.asciiRows = 25;
        s.charset = AsciiCharset::Ramp70;
        s.asciiColor = false;
        s.asciiInk = Color::FromRGB(0xFFB000);
        s.asciiPaper = Color{0.04f, 0.02f, 0.0f, 1.0f};
        s.asciiContrast = 1.35f;
        s.asciiBrightness = 0.02f;
        s.asciiScanlines = true;
        s.asciiScanlineStrength = 0.25f;
        presets.push_back({"ASCII Amber", s});
    }
    {
        RetroSettings s;
        s.mode = RetroMode::Ascii;
        s.virtualWidth = 320;
        s.virtualHeight = 200;
        s.asciiCols = 64;
        s.asciiRows = 40;
        s.charset = AsciiCharset::Blocks;
        s.asciiColor = true;
        s.asciiBackgroundFill = false;
        s.asciiContrast = 1.4f;
        s.asciiBrightness = 0.05f;
        presets.push_back({"ASCII Blocks", s});
    }
    {
        RetroSettings s;
        s.mode = RetroMode::Ascii;
        s.virtualWidth = 320;
        s.virtualHeight = 200;
        s.asciiCols = 96;
        s.asciiRows = 0;  // выводится из пропорций виртуального изображения
        s.charset = AsciiCharset::Braille;
        s.asciiColor = true;
        s.asciiBackgroundFill = false;
        s.asciiContrast = 1.2f;
        s.asciiCellAspect = 0.5f;
        presets.push_back({"Braille Art", s});
    }
    {
        RetroSettings s;
        s.mode = RetroMode::Pixel;
        s.virtualWidth = 320;
        s.virtualHeight = 200;
        s.palette = RetroPalette::Amstrad32;
        s.colorDepth = 3.0f;
        presets.push_back({"Amstrad CPC", s});
    }

    return presets;
}

}  // namespace crossrender
