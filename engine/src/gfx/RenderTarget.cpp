#include "crossrender/gfx/RenderTarget.h"

#include "crossrender/gfx/GL.h"
#include "crossrender/core/Log.h"
#include "crossrender/core/Time.h"
#include "crossrender/gfx/Shader.h"

#include <algorithm>

namespace crossrender {

// ---------------------------------------------------------------------------
// RenderTarget
// ---------------------------------------------------------------------------
RenderTarget::RenderTarget() = default;
RenderTarget::~RenderTarget() { Destroy(); }

RenderTarget::RenderTarget(RenderTarget&& o) noexcept { *this = std::move(o); }

RenderTarget& RenderTarget::operator=(RenderTarget&& o) noexcept {
    if (this == &o) return *this;
    Destroy();
    desc_ = o.desc_;
    fbo_ = o.fbo_;
    resolveFbo_ = o.resolveFbo_;
    depthRbo_ = o.depthRbo_;
    colorTextures_ = std::move(o.colorTextures_);
    depthTexture_ = std::move(o.depthTexture_);
    msaaColorRbo_ = std::move(o.msaaColorRbo_);
    depthTextureValid_ = o.depthTextureValid_;
    o.fbo_ = o.resolveFbo_ = o.depthRbo_ = 0;
    o.depthTextureValid_ = false;
    return *this;
}

bool RenderTarget::Create(const RenderTargetDesc& desc) {
    Destroy();
    desc_ = desc;
    if (desc.width <= 0 || desc.height <= 0) return false;
    if (!gl::glGenFramebuffers) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            ENG_LOGW("rt", "no GL context; render targets stay on the CPU (first was '%s', "
                           "this warning is shown once)",
                     desc.name.c_str());
        }
        return false;
    }
    int attachments = Clamp(desc.colorAttachments, 1, 4);
    desc_.colorAttachments = attachments;
    colorTextures_.resize(static_cast<usize>(attachments));

    bool msaa = desc.samples > 1;
    if (msaa) {
        gl::GLint maxSamples = 0;
        gl::glGetIntegerv(gl::GL_MAX_SAMPLES, &maxSamples);
        if (maxSamples < desc.samples) {
            ENG_LOGW("rt", "requested %d MSAA samples, device supports %d", desc.samples, maxSamples);
            desc_.samples = MaxT(1, maxSamples);
            msaa = desc_.samples > 1;
        }
    }

    gl::glGenFramebuffers(1, &fbo_);
    gl::glBindFramebuffer(gl::GL_FRAMEBUFFER, fbo_);

    std::vector<gl::GLenum> drawBuffers;
    if (msaa) {
        msaaColorRbo_.resize(static_cast<usize>(attachments), 0);
        gl::glGenRenderbuffers(static_cast<gl::GLsizei>(attachments), msaaColorRbo_.data());
        for (int i = 0; i < attachments; ++i) {
            gl::glBindRenderbuffer(gl::GL_RENDERBUFFER, msaaColorRbo_[static_cast<usize>(i)]);
            gl::glRenderbufferStorageMultisample(gl::GL_RENDERBUFFER, desc_.samples,
                                                 desc.colorFormat == PixelFormat::RGBA16F
                                                     ? gl::GL_RGBA16F
                                                     : gl::GL_RGBA8,
                                                 desc.width, desc.height);
            gl::glFramebufferRenderbuffer(gl::GL_FRAMEBUFFER,
                                          static_cast<gl::GLenum>(gl::GL_COLOR_ATTACHMENT0 + i),
                                          gl::GL_RENDERBUFFER, msaaColorRbo_[static_cast<usize>(i)]);
            drawBuffers.push_back(static_cast<gl::GLenum>(gl::GL_COLOR_ATTACHMENT0 + i));
        }
        gl::glDrawBuffers(static_cast<gl::GLsizei>(drawBuffers.size()), drawBuffers.data());
        // Цель для resolve (одна цветовая текстура, без глубины).
        gl::glGenFramebuffers(1, &resolveFbo_);
        gl::glBindFramebuffer(gl::GL_FRAMEBUFFER, resolveFbo_);
        for (int i = 0; i < attachments; ++i) {
            Texture& t = colorTextures_[static_cast<usize>(i)];
            t.Create(desc.width, desc.height, desc.colorFormat, nullptr, desc.filter, desc.wrap,
                     desc.generateMipmaps);
            gl::glFramebufferTexture2D(gl::GL_FRAMEBUFFER,
                                       static_cast<gl::GLenum>(gl::GL_COLOR_ATTACHMENT0 + i),
                                       gl::GL_TEXTURE_2D, t.Id(), 0);
            drawBuffers[static_cast<usize>(i)] = static_cast<gl::GLenum>(gl::GL_COLOR_ATTACHMENT0 + i);
        }
        gl::glDrawBuffers(static_cast<gl::GLsizei>(drawBuffers.size()), drawBuffers.data());
        gl::glBindFramebuffer(gl::GL_FRAMEBUFFER, fbo_);
        // Для MSAA к мультисемплному FBO нужен подключённый depth renderbuffer.
        if (desc.depth) {
            gl::glGenRenderbuffers(1, &depthRbo_);
            gl::glBindRenderbuffer(gl::GL_RENDERBUFFER, depthRbo_);
            gl::GLenum fmt = desc.stencil ? gl::GL_DEPTH24_STENCIL8 : gl::GL_DEPTH_COMPONENT24;
            gl::glRenderbufferStorageMultisample(gl::GL_RENDERBUFFER, desc_.samples, fmt, desc.width,
                                                 desc.height);
            gl::glFramebufferRenderbuffer(gl::GL_FRAMEBUFFER,
                                          desc.stencil ? gl::GL_DEPTH_STENCIL_ATTACHMENT
                                                       : gl::GL_DEPTH_ATTACHMENT,
                                          gl::GL_RENDERBUFFER, depthRbo_);
        }
    } else {
        for (int i = 0; i < attachments; ++i) {
            Texture& t = colorTextures_[static_cast<usize>(i)];
            t.Create(desc.width, desc.height, desc.colorFormat, nullptr, desc.filter, desc.wrap,
                     desc.generateMipmaps);
            gl::glFramebufferTexture2D(gl::GL_FRAMEBUFFER,
                                       static_cast<gl::GLenum>(gl::GL_COLOR_ATTACHMENT0 + i),
                                       gl::GL_TEXTURE_2D, t.Id(), 0);
            drawBuffers.push_back(static_cast<gl::GLenum>(gl::GL_COLOR_ATTACHMENT0 + i));
        }
        gl::glDrawBuffers(static_cast<gl::GLsizei>(drawBuffers.size()), drawBuffers.data());
        if (desc.depth) {
            // Предпочитаем сэмплируемую depth-текстуру (нужна для soft particles и чтения глубины).
            if (depthTexture_.Create(desc.width, desc.height, desc.depthFormat, nullptr,
                                     TextureFilter::Nearest, TextureWrap::ClampToEdge, false)) {
                gl::glFramebufferTexture2D(gl::GL_FRAMEBUFFER,
                                           desc.stencil ? gl::GL_DEPTH_STENCIL_ATTACHMENT
                                                        : gl::GL_DEPTH_ATTACHMENT,
                                           gl::GL_TEXTURE_2D, depthTexture_.Id(), 0);
                depthTextureValid_ = true;
            } else {
                gl::glGenRenderbuffers(1, &depthRbo_);
                gl::glBindRenderbuffer(gl::GL_RENDERBUFFER, depthRbo_);
                gl::glRenderbufferStorage(gl::GL_RENDERBUFFER,
                                          desc.stencil ? gl::GL_DEPTH24_STENCIL8
                                                       : gl::GL_DEPTH_COMPONENT24,
                                          desc.width, desc.height);
                gl::glFramebufferRenderbuffer(gl::GL_FRAMEBUFFER,
                                              desc.stencil ? gl::GL_DEPTH_STENCIL_ATTACHMENT
                                                           : gl::GL_DEPTH_ATTACHMENT,
                                              gl::GL_RENDERBUFFER, depthRbo_);
            }
        }
    }

    gl::GLenum status = gl::glCheckFramebufferStatus(gl::GL_FRAMEBUFFER);
    gl::glBindFramebuffer(gl::GL_FRAMEBUFFER, 0);
    if (status != gl::GL_FRAMEBUFFER_COMPLETE) {
        ENG_LOGE("rt", "framebuffer '%s' incomplete (0x%04X)", desc.name.c_str(), status);
        Destroy();
        return false;
    }
    return true;
}

bool RenderTarget::Resize(int width, int height) {
    if (width == desc_.width && height == desc_.height && fbo_ != 0) return true;
    RenderTargetDesc d = desc_;
    d.width = width;
    d.height = height;
    return Create(d);
}

void RenderTarget::Destroy() {
    if (colorTextures_.size()) {
        for (Texture& t : colorTextures_) t.Destroy();
        colorTextures_.clear();
    }
    depthTexture_.Destroy();
    depthTextureValid_ = false;
    if (!gl::glDeleteFramebuffers) {
        fbo_ = resolveFbo_ = depthRbo_ = 0;
        msaaColorRbo_.clear();
        return;
    }
    if (depthRbo_) {
        gl::glDeleteRenderbuffers(1, &depthRbo_);
        depthRbo_ = 0;
    }
    if (!msaaColorRbo_.empty()) {
        gl::glDeleteRenderbuffers(static_cast<gl::GLsizei>(msaaColorRbo_.size()),
                                  msaaColorRbo_.data());
        msaaColorRbo_.clear();
    }
    if (resolveFbo_) {
        gl::glDeleteFramebuffers(1, &resolveFbo_);
        resolveFbo_ = 0;
    }
    if (fbo_) {
        gl::glDeleteFramebuffers(1, &fbo_);
        fbo_ = 0;
    }
}

namespace {
// Стек привязанных render target'ов, чтобы вложенные области Bind/Unbind
// корректно восстанавливались. Вьюпорт — часть привязки: внеэкранный проход
// меняет его, и иначе любой последующий draw обрезался бы до размера offscreen.
struct FboBinding {
    unsigned int fbo = 0;
    int viewportW = 0;
    int viewportH = 0;
};
std::vector<FboBinding> g_fboStack;
unsigned int g_currentFbo = 0;
unsigned int g_currentViewportW = 1, g_currentViewportH = 1;
// Вьюпорт default framebuffer: движок записывает его каждый кадр, чтобы
// Unbind() мог восстановить его после самого внешнего offscreen-уровня.
int g_defaultViewportW = 0, g_defaultViewportH = 0;
}  // namespace

void RenderTarget::Bind() {
    g_fboStack.push_back(FboBinding{g_currentFbo, static_cast<int>(g_currentViewportW),
                                    static_cast<int>(g_currentViewportH)});
    g_currentFbo = fbo_;
    gl::glBindFramebuffer(gl::GL_FRAMEBUFFER, fbo_);
    gl::glViewport(0, 0, desc_.width, desc_.height);
    g_currentViewportW = static_cast<unsigned int>(desc_.width);
    g_currentViewportH = static_cast<unsigned int>(desc_.height);
}

void RenderTarget::Unbind() {
    Resolve();
    FboBinding restore{};
    if (!g_fboStack.empty()) {
        restore = g_fboStack.back();
        g_fboStack.pop_back();
    }
    // Восстановление default framebuffer после самого внешнего уровня:
    // используем записанный движком для него вьюпорт (в записи стека лежит
    // состояние до области, устаревшее после изменения размера окна).
    if (restore.fbo == 0) {
        restore.viewportW = g_defaultViewportW;
        restore.viewportH = g_defaultViewportH;
    }
    if (restore.viewportW <= 0 || restore.viewportH <= 0) {
        restore.viewportW = static_cast<int>(g_currentViewportW);
        restore.viewportH = static_cast<int>(g_currentViewportH);
    }
    g_currentFbo = restore.fbo;
    gl::glBindFramebuffer(gl::GL_FRAMEBUFFER, restore.fbo);
    gl::glViewport(0, 0, restore.viewportW, restore.viewportH);
    g_currentViewportW = static_cast<unsigned int>(restore.viewportW);
    g_currentViewportH = static_cast<unsigned int>(restore.viewportH);
}

void RenderTarget::SetDefaultViewport(int width, int height) {
    g_defaultViewportW = width;
    g_defaultViewportH = height;
}

void RenderTarget::Resolve() {
    if (desc_.samples <= 1 || resolveFbo_ == 0) return;
    gl::glBindFramebuffer(gl::GL_READ_FRAMEBUFFER, fbo_);
    gl::glBindFramebuffer(gl::GL_DRAW_FRAMEBUFFER, resolveFbo_);
    for (int i = 0; i < desc_.colorAttachments; ++i) {
        const auto attachment = static_cast<gl::GLenum>(gl::GL_COLOR_ATTACHMENT0 + i);
        gl::glReadBuffer(attachment);
        gl::glDrawBuffer(attachment);
        gl::glBlitFramebuffer(0, 0, desc_.width, desc_.height, 0, 0, desc_.width, desc_.height,
                              gl::GL_COLOR_BUFFER_BIT, gl::GL_LINEAR);
    }
    gl::glBindFramebuffer(gl::GL_FRAMEBUFFER, g_currentFbo);
}

unsigned int RenderTarget::boundFboStackTop() { return g_currentFbo; }

void RenderTarget::Clear(const Color& c, bool depth, bool stencil) {
    gl::GLbitfield mask = gl::GL_COLOR_BUFFER_BIT;
    if (depth) mask |= gl::GL_DEPTH_BUFFER_BIT;
    if (stencil) mask |= gl::GL_STENCIL_BUFFER_BIT;
    bool depthWasEnabled = gl::glIsEnabled(gl::GL_DEPTH_TEST) != 0;
    gl::GLboolean depthMask = 1;
    gl::glGetBooleanv(gl::GL_DEPTH_WRITEMASK, &depthMask);
    if (depth) {
        gl::glDepthMask(1);
        gl::glEnable(gl::GL_DEPTH_TEST);
    }
    gl::glClearColor(c.r, c.g, c.b, c.a);
    gl::glClear(mask);
    if (depth) {
        gl::glDepthMask(depthMask);
        if (!depthWasEnabled) gl::glDisable(gl::GL_DEPTH_TEST);
    }
}

void RenderTarget::SetViewport(int x, int y, int w, int h) {
    gl::glViewport(x, y, w, h);
}

Texture& RenderTarget::ColorTexture() {
    static Texture dummy;
    return colorTextures_.empty() ? dummy : colorTextures_[0];
}

Texture& RenderTarget::TextureAt(int index) {
    static Texture dummy;
    if (index < 0 || index >= static_cast<int>(colorTextures_.size())) return dummy;
    return colorTextures_[static_cast<usize>(index)];
}

Texture& RenderTarget::DepthTexture() { return depthTexture_; }

bool RenderTarget::ReadPixels(std::vector<u8>* outRGBA) {
    if (!outRGBA || desc_.width <= 0 || desc_.height <= 0) return false;
    if (!gl::glReadPixels) return false;
    unsigned int prev = g_currentFbo;
    gl::glBindFramebuffer(gl::GL_FRAMEBUFFER, resolveFbo_ ? resolveFbo_ : fbo_);
    outRGBA->resize(static_cast<usize>(desc_.width) * desc_.height * 4);
    gl::glPixelStorei(gl::GL_PACK_ALIGNMENT, 1);
    gl::glReadPixels(0, 0, desc_.width, desc_.height, gl::GL_RGBA, gl::GL_UNSIGNED_BYTE,
                     outRGBA->data());
    gl::glBindFramebuffer(gl::GL_FRAMEBUFFER, prev);
    return true;
}

ScopedRenderTarget::ScopedRenderTarget(RenderTarget& rt) : rt_(&rt) { rt_->Bind(); }
ScopedRenderTarget::~ScopedRenderTarget() { rt_->Unbind(); }

// ---------------------------------------------------------------------------
// PostProcessor
// ---------------------------------------------------------------------------
namespace {

// Общий полноэкранный triangle strip в clip space с подходящими UV.
struct FullscreenQuad {
    unsigned int vao = 0, vbo = 0;
    bool valid = false;

    bool Create() {
        if (valid) return true;
        if (!gl::glGenVertexArrays) return false;
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
        valid = true;
        return true;
    }

    void Draw() const {
        if (!valid) return;
        gl::glBindVertexArray(vao);
        gl::glDrawArrays(gl::GL_TRIANGLE_STRIP, 0, 4);
    }
};

FullscreenQuad& SharedQuad() {
    static FullscreenQuad q;
    return q;
}

}  // namespace

struct PostProcessor::Impl {
    Shader blit;
    Shader fxaa;
    Shader bloomDown;
    Shader bloomUp;
    Shader tonemap;
    bool ready = false;
};

PostProcessor::PostProcessor() : impl_(new Impl()) {}
PostProcessor::~PostProcessor() { Shutdown(); }

bool PostProcessor::Init(int width, int height) {
    width_ = MaxT(1, width);
    height_ = MaxT(1, height);
    if (!gl::glCreateShader) return false;
    SharedQuad().Create();

    RenderTargetDesc desc;
    desc.width = width_;
    desc.height = height_;
    desc.depth = true;
    desc.stencil = false;
    desc.colorFormat = PixelFormat::RGBA16F;
    desc.samples = 1;
    desc.filter = TextureFilter::Linear;
    desc.name = "post-scene";
    if (!scene_.Create(desc)) return false;

    impl_->ready = impl_->blit.Build(builtin::kPostVert, builtin::kBlitFrag, "blit") &&
                   impl_->fxaa.Build(builtin::kPostVert, builtin::kFxaaFrag, "fxaa") &&
                   impl_->bloomDown.Build(builtin::kPostVert, builtin::kBloomDownFrag, "bloom-down") &&
                   impl_->bloomUp.Build(builtin::kPostVert, builtin::kBloomUpFrag, "bloom-up") &&
                   impl_->tonemap.Build(builtin::kPostVert, builtin::kTonemapFrag, "tonemap");
    valid_ = impl_->ready;
    return valid_;
}

void PostProcessor::Shutdown() {
    scene_.Destroy();
    bloomChain_.clear();
    valid_ = false;
    if (impl_) impl_->ready = false;
}

void PostProcessor::Resize(int width, int height) {
    width = MaxT(1, width);
    height = MaxT(1, height);
    if (width == width_ && height == height_) return;
    width_ = width;
    height_ = height;
    scene_.Resize(width_, height_);
    float w = static_cast<float>(width_), h = static_cast<float>(height_);
    for (auto& rt : bloomChain_) {
        w = MaxT(1.0f, w * 0.5f);
        h = MaxT(1.0f, h * 0.5f);
        rt.Resize(static_cast<int>(w), static_cast<int>(h));
    }
}

void PostProcessor::Apply(const Texture& sceneTex, const PostProcessSettings& s, int fbWidth,
                           int fbHeight, unsigned int targetFbo) {
    if (!valid_) {
        if (!Init(fbWidth, fbHeight)) return;
    }
    FullscreenQuad& quad = SharedQuad();
    gl::glDisable(gl::GL_DEPTH_TEST);
    gl::glDepthMask(0);
    gl::glDisable(gl::GL_CULL_FACE);

    const Texture* bloomResult = nullptr;

    if (s.enabled && s.bloom) {
        if (static_cast<int>(bloomChain_.size()) != s.bloomMips) {
            bloomChain_.clear();
            float w = static_cast<float>(scene_.Width()), h = static_cast<float>(scene_.Height());
            for (int i = 0; i < Clamp(s.bloomMips, 1, 8); ++i) {
                RenderTargetDesc d;
                w = MaxT(1.0f, w * 0.5f);
                h = MaxT(1.0f, h * 0.5f);
                d.width = static_cast<int>(w);
                d.height = static_cast<int>(h);
                d.depth = false;
                d.colorFormat = PixelFormat::RGBA16F;
                d.filter = TextureFilter::Linear;
                d.name = "bloom-" + std::to_string(i);
                RenderTarget rt;
                rt.Create(d);
                bloomChain_.push_back(std::move(rt));
            }
        }
        gl::glBindFramebuffer(gl::GL_FRAMEBUFFER, targetFbo);
        // Цепочка даунсемплинга
        impl_->bloomDown.Bind();
        const Texture* src = &sceneTex;
        for (usize i = 0; i < bloomChain_.size(); ++i) {
            RenderTarget& rt = bloomChain_[i];
            gl::glBindFramebuffer(gl::GL_FRAMEBUFFER, rt.Fbo());
            gl::glViewport(0, 0, rt.Width(), rt.Height());
            impl_->bloomDown.SetTexture("uTexture", *src, 0);
            impl_->bloomDown.Set("uTexelSize", Vec2{1.0f / MaxT(1, src->Width()),
                                                    1.0f / MaxT(1, src->Height())});
            impl_->bloomDown.Set("uThreshold", i == 0 ? s.bloomThreshold : 0.0f);
            quad.Draw();
            src = &rt.ColorTexture();
        }
        // Апсемплинг (аддитивный)
        gl::glEnable(gl::GL_BLEND);
        gl::glBlendFunc(gl::GL_ONE, gl::GL_ONE);
        impl_->bloomUp.Bind();
        for (int i = static_cast<int>(bloomChain_.size()) - 1; i > 0; --i) {
            RenderTarget& dst = bloomChain_[static_cast<usize>(i - 1)];
            const Texture& up = bloomChain_[static_cast<usize>(i)].ColorTexture();
            gl::glBindFramebuffer(gl::GL_FRAMEBUFFER, dst.Fbo());
            gl::glViewport(0, 0, dst.Width(), dst.Height());
            impl_->bloomUp.SetTexture("uTexture", up, 0);
            impl_->bloomUp.Set("uTexelSize", Vec2{1.0f / MaxT(1, up.Width()), 1.0f / MaxT(1, up.Height())});
            impl_->bloomUp.Set("uIntensity", s.bloomIntensity);
            quad.Draw();
        }
        gl::glDisable(gl::GL_BLEND);
        bloomResult = &bloomChain_[0].ColorTexture();
    }

    gl::glBindFramebuffer(gl::GL_FRAMEBUFFER, targetFbo);
    gl::glViewport(0, 0, fbWidth, fbHeight);
    gl::glDisable(gl::GL_BLEND);
    gl::glClearColor(0, 0, 0, 1);
    gl::glClear(gl::GL_COLOR_BUFFER_BIT);

    impl_->tonemap.Bind();
    Mat4 identity;
    impl_->tonemap.Set("uTransform", identity);
    impl_->tonemap.SetTexture("uTexture", sceneTex, 0);
    if (bloomResult) impl_->tonemap.SetTexture("uBloom", *bloomResult, 1);
    else impl_->tonemap.SetTexture("uBloom", sceneTex, 1);
    impl_->tonemap.Set("uUseBloom", bloomResult ? 1 : 0);
    impl_->tonemap.Set("uExposure", s.exposure);
    impl_->tonemap.Set("uMode", s.tonemap ? s.tonemapMode : 3);
    impl_->tonemap.Set("uGamma", s.gamma);
    impl_->tonemap.Set("uVignette", s.vignette ? s.vignetteIntensity : 0.0f);
    impl_->tonemap.Set("uGrain", s.filmGrain ? s.grainAmount : 0.0f);
    impl_->tonemap.Set("uTime", static_cast<f32>(NowSeconds()));
    impl_->tonemap.Set("uAberration", s.chromaticAberration ? s.aberrationAmount : 0.0f);
    quad.Draw();

    gl::glDepthMask(1);
    gl::glEnable(gl::GL_DEPTH_TEST);
}

}  // namespace crossrender
