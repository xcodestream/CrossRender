//
// CrossRender
// Автор: xcodestream <xcodestream@gmail.com>
// Лицензия: MIT
// Дата: 15.05.2026
// Описание: оффскрин-рендеринг: фреймбуферы с цветом и глубиной, чтение пикселей, постобработка.
//
#pragma once

#include "crossrender/core/Base.h"
#include "crossrender/core/Math.h"
#include "crossrender/gfx/Texture.h"

#include <memory>
#include <vector>

namespace crossrender {

struct RenderTargetDesc {
    int width = 0;
    int height = 0;
    int samples = 1;                       // число сэмплов MSAA (1 = без MSAA)
    bool depth = true;
    bool stencil = false;
    bool colorAsTexture = true;            // разворачивать в сэмплируемую текстуру
    PixelFormat colorFormat = PixelFormat::RGBA8;
    PixelFormat depthFormat = PixelFormat::Depth24Stencil8;
    int colorAttachments = 1;              // MRT (deferred / цепочки bloom)
    TextureFilter filter = TextureFilter::Linear;
    TextureWrap wrap = TextureWrap::ClampToEdge;
    bool generateMipmaps = false;
    std::string name;
};

// Фреймбуфер с опциональным MSAA и resolve-текстурой.
class RenderTarget {
public:
    RenderTarget();
    ~RenderTarget();
    RenderTarget(RenderTarget&&) noexcept;
    RenderTarget& operator=(RenderTarget&&) noexcept;
    RenderTarget(const RenderTarget&) = delete;
    RenderTarget& operator=(const RenderTarget&) = delete;

    bool Create(const RenderTargetDesc& desc);
    // Пересоздаёт при изменении размера (no-op, если размер не изменился).
    bool Resize(int width, int height);
    void Destroy();

    // Привязывает для отрисовки и задаёт вьюпорт.
    void Bind();
    // Привязывает для чтения/сэмплирования; разворачивает MSAA в цветовую текстуру.
    void Unbind();
// Запоминает вьюпорт фреймбуфера по умолчанию, чтобы вложенные Bind/Unbind
// восстанавливали его после оффскрин-прохода. Движок вызывает это каждый кадр.
    static void SetDefaultViewport(int width, int height);
    // Разворачивает MSAA в сэмплируемую текстуру (неявно выполняется в Unbind).
    void Resolve();

    void Clear(const Color& c, bool depth = true, bool stencil = false);
    void SetViewport(int x, int y, int w, int h);

    [[nodiscard]] bool Valid() const { return fbo_ != 0; }
    [[nodiscard]] unsigned int Fbo() const { return fbo_; }
    [[nodiscard]] int Width() const { return desc_.width; }
    [[nodiscard]] int Height() const { return desc_.height; }
    [[nodiscard]] f32 Aspect() const {
        return desc_.height ? static_cast<f32>(desc_.width) / static_cast<f32>(desc_.height) : 1.0f;
    }
    [[nodiscard]] const RenderTargetDesc& Desc() const { return desc_; }
    // Сэмплируемая цветовая текстура; для MRT используйте TextureAt(index).
    [[nodiscard]] Texture& ColorTexture();
    [[nodiscard]] Texture& TextureAt(int index);
    [[nodiscard]] Texture& DepthTexture();
    [[nodiscard]] int ColorAttachmentCount() const { return desc_.colorAttachments; }
    [[nodiscard]] bool HasDepthTexture() const { return depthTextureValid_; }

    // Читает цветовое вложение в пиксели RGBA8 (начало координат слева сверху).
    bool ReadPixels(std::vector<u8>* outRGBA);

private:
    RenderTargetDesc desc_{};
    unsigned int fbo_ = 0, resolveFbo_ = 0, depthRbo_ = 0;
    std::vector<Texture> colorTextures_;
    Texture depthTexture_;
    std::vector<unsigned int> msaaColorRbo_;
    bool depthTextureValid_ = false;
    static unsigned int boundFboStackTop();
};

// Удобная RAII-привязка на область видимости (восстанавливает прежнюю цель).
class ScopedRenderTarget {
public:
    explicit ScopedRenderTarget(RenderTarget& rt);
    ~ScopedRenderTarget();
    ScopedRenderTarget(const ScopedRenderTarget&) = delete;
    ScopedRenderTarget& operator=(const ScopedRenderTarget&) = delete;

private:
    RenderTarget* rt_;
};

// ---------------------------------------------------------------------------
// Цепочка постобработки: bloom + тонмаппинг + FXAA, все шейдеры оригинальные.
// ---------------------------------------------------------------------------
struct PostProcessSettings {
    bool enabled = true;
    bool bloom = true;
    f32 bloomThreshold = 1.0f;
    f32 bloomIntensity = 0.6f;
    int bloomMips = 4;
    bool tonemap = true;
    f32 exposure = 1.0f;
    int tonemapMode = 0;  // 0 = ACES, 1 = Reinhard, 2 = Uncharted2, 3 = нет
    bool fxaa = true;
    bool vignette = false;
    f32 vignetteIntensity = 0.35f;
    f32 gamma = 2.2f;
    bool filmGrain = false;
    f32 grainAmount = 0.04f;
    bool chromaticAberration = false;
    f32 aberrationAmount = 0.002f;
};

class PostProcessor {
public:
    PostProcessor();
    ~PostProcessor();
    bool Init(int width, int height);
    void Shutdown();
    void Resize(int width, int height);

// Рисует `scene` (уже отрисованную в HDR-цель) в `targetFbo`
// (0 = фреймбуфер по умолчанию), применяя настроенные эффекты. `scene`
// должна быть HDR (RGBA16F).
    void Apply(const Texture& scene, const PostProcessSettings& settings, int fbWidth, int fbHeight,
               unsigned int targetFbo = 0);

    [[nodiscard]] RenderTarget& SceneTarget() { return scene_; }
    [[nodiscard]] bool Valid() const { return valid_; }

private:
    RenderTarget scene_;
    std::vector<RenderTarget> bloomChain_;
    int width_ = 0, height_ = 0;
    bool valid_ = false;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace crossrender
