#include "crossrender/Engine.h"

#include "crossrender/Resource.h"

#include "crossrender/ui/Ui.h"
#include "crossrender/gfx/GL.h"
#include "crossrender/core/Log.h"
#include "crossrender/core/File.h"
#include "crossrender/text/Font.h"
#include "crossrender/platform/Window.h"
#include "crossrender/platform/Platform.h"

#include <algorithm>

#if defined(ENG_PLATFORM_WASM)
#include <emscripten.h>
#endif

namespace crossrender {

Engine::Engine() = default;
Engine::~Engine() { Shutdown(); }

bool Engine::Init(const EngineConfig& config) {
    if (initialised_) return true;
    config_ = config;
    LogSetLevel(LogLevel::Debug);

    if (!config_.assetsPath.empty()) SetAssetRoot(config_.assetsPath);
    ENG_LOGI("engine", "CrossRender starting on %s (%s)", PlatformName().c_str(),
             PlatformArch().c_str());
    ENG_LOGI("engine", "asset root: %s", GetAssetRoot().c_str());
    ENG_LOGI("engine", "user root: %s", GetUserRoot().c_str());

    if (!PlatformInit()) {
        ENG_LOGE("engine", "platform init failed");
        return false;
    }

    void* (*glProc)(const char*) = nullptr;

    if (config_.headless) {
        if (!CreateHeadlessGLContext()) {
            ENG_LOGE("engine", "headless GL context unavailable");
            return false;
        }
        glProc = HeadlessGLGetProcAddress;
    } else {
        window_.reset(new Window());
        if (!window_->Create(config_.window)) {
            ENG_LOGE("engine", "window creation failed");
            return false;
        }
        glProc = window_->GLGetProcAddress();

        // Сообщаем текущей сцене об изменении размера окна или DPI. Вьюпорт
        // пересчитывается каждый кадр, так что отрисовка адаптируется сама,
        // но сцену, разложившую свои панели один раз, нужно попросить переделать это.
        window_->callbacks.onResize = [this](int w, int h) {
            UpdateViewport();
            scenes_.Resize(static_cast<int>(viewport_.w), static_cast<int>(viewport_.h));
            (void)w;
            (void)h;
        };
        window_->callbacks.onDpiChanged = [this](f32) { UpdateViewport(); };
    }

    if (!gl::LoadFunctions(glProc)) {
        ENG_LOGE("engine", "failed to load OpenGL entry points");
        return false;
    }
    gl::GpuInfo gpu = gl::QueryGpuInfo();
    ENG_LOGI("engine", "GPU: %s | %s", gpu.renderer.c_str(), gpu.version.c_str());
    ENG_LOGI("engine", "GLSL: %s", gpu.glslVersion.c_str());

    if (!r2d_.Init()) ENG_LOGW("engine", "Renderer2D unavailable");
    if (config_.enable3D) {
        if (!r3d_.Init()) ENG_LOGW("engine", "Renderer3D unavailable");
    }
    if (config_.enableUI) {
        ui_.Init(&r2d_);
        ui_.SetTheme(UiTheme::Dark());
    }

    resources_.reset(new ResourceCache());
    resources_->SetAssetRoot(GetAssetRoot());

    defaultFont_ = FontManager::Get().DefaultFont();
    defaultSdfFont_ = FontManager::Get().DefaultSdfFont();
    if (ui_.Theme().font == nullptr) ui_.Theme().font = defaultFont_;

    if (config_.enableAudio) {
        if (!Audio::Get().Init()) {
            ENG_LOGW("engine", "audio unavailable, running silently");
        }
    }

    if (config_.enablePostProcessing) {
        post_.reset(new PostProcessor());
    }

    // Retro-дисплей (pixel art / ASCII) и стекуемая цепочка фильтров.
    retro_.reset(new RetroDisplay());
    if (!retro_->Init()) {
        ENG_LOGW("engine", "retro display unavailable");
    }
    if (config_.enableFilters) {
        filters_.reset(new FilterChain());
        if (!filters_->Init()) {
            ENG_LOGW("engine", "filter chain unavailable");
            filters_.reset();
        } else if (config_.filtersDefaultEnabled) {
            FilterChain preset = FilterChain::MakePreset("Clean");
            for (const FilterInstance& f : preset.Filters()) filters_->Add(f);
        }
    }

    UpdateViewport();

    ctx_.engine = this;
    ctx_.scenes = &scenes_;
    ctx_.r2d = &r2d_;
    ctx_.r3d = &r3d_;
    ctx_.ui = &ui_;
    ctx_.viewport = viewport_;
    ctx_.dpiScale = dpiScale_;
    scenes_.SetContext(&ctx_);
    scenes_.SetRenderer2D(&r2d_);

    clock_.SetFixedStep(config_.fixedTimeStep);
    initialised_ = true;
    return true;
}

void Engine::Shutdown() {
    if (!initialised_) return;
    scenes_.Reset();
    if (config_.enableAudio) Audio::Get().Shutdown();
    ui_.Shutdown();
    r3d_.Shutdown();
    r2d_.Shutdown();
    filters_.reset();
    scratch_.reset();
    if (retro_) {
        retro_->Shutdown();
        retro_.reset();
    }
    post_.reset();
    sceneTarget_.reset();
    headlessTarget_.reset();
    if (resources_) {
        resources_->Shutdown();
        resources_.reset();
    }
    if (window_) {
        window_->Destroy();
        window_.reset();
    }
    if (config_.headless) DestroyHeadlessGLContext();
    gl::UnloadFunctions();
    PlatformShutdown();
    initialised_ = false;
}

void Engine::UpdateViewport() {
    int fbW = 0, fbH = 0;
    if (window_) {
        fbW = window_->FramebufferWidth();
        fbH = window_->FramebufferHeight();
        dpiScale_ = window_->DpiScale();
    } else {
        fbW = config_.headlessTarget.width > 0 ? config_.headlessTarget.width : 1280;
        fbH = config_.headlessTarget.height > 0 ? config_.headlessTarget.height : 720;
        dpiScale_ = 1.0f;
    }
    if (fbW <= 0) fbW = 1;
    if (fbH <= 0) fbH = 1;
    SafeArea safe = SafeArea::Query();
    viewport_ = safe.Apply(Rect{0, 0, static_cast<f32>(fbW) / dpiScale_,
                                static_cast<f32>(fbH) / dpiScale_});
    // В retro-режиме весь кадр (включая UI) раскладывается по низкоразрешённому
    // виртуальному экрану — в этом смысл режимов pixel-art/ASCII.
    if (config_.retro.mode != RetroMode::Off && retro_ && retro_->Valid()) {
        retro_->Resize(fbW, fbH, config_.retro);
        const int vw = retro_->VirtualWidth();
        const int vh = retro_->VirtualHeight();
        if (vw > 0 && vh > 0) viewport_ = safe.Apply(Rect{0, 0, static_cast<f32>(vw),
                                                          static_cast<f32>(vh)});
        ctx_.dpiScale = 1.0f;
    } else {
        ctx_.dpiScale = dpiScale_;
    }
    ctx_.viewport = viewport_;
}

Window& Engine::GetWindow() {
    if (window_) return *window_;
    // Headless: выдаём отсоединённое окно, чтобы вызывающие не разыменовали null.
    static Window dummy;
    return dummy;
}

const Window& Engine::GetWindow() const {
    return const_cast<Engine*>(this)->GetWindow();
}

bool Engine::FiltersActive() const {
    return filters_ && filters_->Valid() && filters_->Count() > 0;
}

RenderTarget* Engine::EnsureHeadlessTarget(int w, int h) {
    if (!config_.headless) return nullptr;
    if (!headlessTarget_) headlessTarget_.reset(new RenderTarget());
    if (headlessTarget_->Width() != w || headlessTarget_->Height() != h) {
        RenderTargetDesc d;
        d.width = w;
        d.height = h;
        d.depth = true;
        d.stencil = false;
        d.colorFormat = PixelFormat::RGBA8;
        d.depthFormat = PixelFormat::Depth24Stencil8;
        d.filter = TextureFilter::Linear;
        d.samples = MaxT(1, config_.headlessTarget.samples);
        d.name = "engine-headless";
        headlessTarget_->Create(d);
    }
    return headlessTarget_.get();
}

void Engine::BeginFrame() {
    UpdateViewport();
    clock_.Tick();
    f32 dt = clock_.Delta();
    if (dt > config_.maxDeltaTime) dt = config_.maxDeltaTime;
    ctx_.dt = dt;

    if (window_) {
        window_->GetInput().BeginFrame();
        window_->PollEvents();
    }
}

void Engine::EndFrame() {
    if (window_) {
        window_->GetInput().EndFrame();
        window_->SwapBuffers();
    }
}

void Engine::Step(f32 dtOverride) {
    if (!initialised_) return;

    Input& frameInput = window_ ? window_->GetInput() : headlessInput_;
    frameInput.BeginFrame();
    if (window_) {
        window_->PollEvents();
        if (window_->ShouldClose()) quit_ = true;
    }

    UpdateViewport();
    clock_.Tick();
    f32 dt = dtOverride >= 0 ? dtOverride : clock_.Delta();
    if (dt > config_.maxDeltaTime) dt = config_.maxDeltaTime;
    f32 scaled = paused_ ? 0.0f : dt;
    ctx_.dt = scaled;

    // На паузе активная сцена заморожена (без Update), но всё ещё рендерится,
    // поэтому меню паузы и оверлеи продолжают работать.
    if (!paused_) scenes_.Update(scaled);
    if (onUpdate) onUpdate(*this, scaled);

    int fbW = window_ ? window_->FramebufferWidth() : MaxT(1, config_.headlessTarget.width);
    int fbH = window_ ? window_->FramebufferHeight() : MaxT(1, config_.headlessTarget.height);
    if (fbW <= 0) fbW = 1;
    if (fbH <= 0) fbH = 1;
    // Чтобы offscreen-области Bind/Unbind восстанавливали правильный вьюпорт
    // для фреймбуфера по умолчанию (сцены могут рендерить в свои цели).
    RenderTarget::SetDefaultViewport(fbW, fbH);

    Scene* scene = scenes_.Current();
    const bool wants3D = scene && scene->Wants3D();
    const bool usePost = config_.enablePostProcessing && post_ && post_->Valid() && wants3D &&
                         scene->WantsPostProcessing() && post_->SceneTarget().Valid();

    // ---- выбор места рендеринга кадра ----------------------------------
    // headless  -> offscreen-цель, чтобы пиксели можно было прочитать
    // retro     -> низкоразрешённая виртуальная цель (весь кадр, включая UI)
    // filters   -> составная цель (фреймбуфер по умолчанию нельзя семплировать)
    RenderTarget* headless = EnsureHeadlessTarget(fbW, fbH);
    const bool retroOn = config_.retro.mode != RetroMode::Off && retro_ && retro_->Valid();
    const bool filtersOn = FiltersActive();

    int frameW = fbW, frameH = fbH;
    f32 frameDpi = dpiScale_;
    RenderTarget* frameTarget = nullptr;
    bool boundFrameTarget = false;

    if (retroOn) {
        retro_->Resize(fbW, fbH, config_.retro);
        frameTarget = retro_->VirtualTarget();
        frameW = MaxT(1, retro_->VirtualWidth());
        frameH = MaxT(1, retro_->VirtualHeight());
        frameDpi = 1.0f;
        // Сначала привязываем *финальную* цель: retro-дисплей привязывает свою
        // виртуальную цель поверх неё и в конце кадра отвязывается обратно к ней
        // перед проходом resolve. Именно привязка здесь направляет headless-
        // (offscreen) retro-рендеринг в читаемую цель.
        if (headless) headless->Bind();
        if (frameTarget) {
            retro_->BeginFrame(r2d_, config_.retro, frameDpi);
            frameTarget->Clear(scene ? scene->ClearColor() : Color::FromARGB(0xFF0E1016), true, false);
            boundFrameTarget = true;
        }
    } else if (filtersOn) {
        if (!scratch_) scratch_.reset(new RenderTarget());
        if (scratch_->Width() != fbW || scratch_->Height() != fbH) {
            RenderTargetDesc d;
            d.width = fbW;
            d.height = fbH;
            d.depth = true;
            d.stencil = false;
            d.colorFormat = PixelFormat::RGBA8;
            d.samples = 1;
            d.filter = TextureFilter::Linear;
            d.wrap = TextureWrap::ClampToEdge;
            d.name = "engine-composite";
            scratch_->Create(d);
        }
        if (scratch_->Valid()) {
            frameTarget = scratch_.get();
            scratch_->Bind();
            scratch_->Clear(scene ? scene->ClearColor() : Color::FromARGB(0xFF0E1016), true, false);
            boundFrameTarget = true;
        }
    } else if (headless) {
        frameTarget = headless;
        headless->Bind();
        headless->Clear(scene ? scene->ClearColor() : Color::FromARGB(0xFF0E1016), true, false);
        boundFrameTarget = true;
    }

    // ---- проход 3D ------------------------------------------------------
    if (wants3D) scenes_.Prepare3D();
    if (wants3D) {
        if (usePost) {
            RenderTarget& hdr = post_->SceneTarget();
            if (hdr.Width() != frameW || hdr.Height() != frameH) post_->Resize(frameW, frameH);
            hdr.Bind();
            hdr.Clear(scene->ClearColor(), true, false);
            r3d_.BeginFrame(r3d_.GetCamera(), frameW, frameH, &hdr, false, false);
            scenes_.Render3D();
            r3d_.EndFrame();
            hdr.Unbind();
            // Разрешаем HDR-цепочку прямо в целевой кадр.
            const unsigned int dstFbo = frameTarget ? frameTarget->Fbo() : 0;
            post_->Apply(hdr.ColorTexture(), postSettings_, frameW, frameH, dstFbo);
            gl::glBindFramebuffer(gl::GL_FRAMEBUFFER, dstFbo);
        } else {
            r3d_.BeginFrame(r3d_.GetCamera(), frameW, frameH, frameTarget,
                            frameTarget == nullptr, true);
            scenes_.Render3D();
            r3d_.EndFrame();
            // Renderer3D::BeginFrame привязывает переданную цель и оставляет её
            // привязанной — вызывающий должен разрулить это; балансируем здесь,
            // иначе стек целей рендеринга растёт каждый кадр, и собственный
            // Unbind() retro-дисплея восстановил бы не тот фреймбуфер.
            if (frameTarget) frameTarget->Unbind();
        }
    }

    // ---- проход 2D / UI -------------------------------------------------
    r2d_.BeginFrame(frameW, frameH, frameDpi);
    if (usePost) {
        // Пост-цепочка уже записала собранное изображение; блитить нечего.
    } else if (!wants3D && !frameTarget) {
        Color clear = scene ? scene->ClearColor() : Color::FromARGB(0xFF0E1016);
        gl::glClearColor(clear.r, clear.g, clear.b, clear.a);
        gl::glClear(gl::GL_COLOR_BUFFER_BIT | gl::GL_DEPTH_BUFFER_BIT);
    }

    if (config_.enableUI) ui_.BeginFrame(r2d_, frameInput, viewport_, scaled);
    scenes_.Render2D();
    if (onOverlay) onOverlay(*this);
    if (config_.enableUI) {
        scenes_.RenderTransition(r2d_, viewport_);
        ui_.EndFrame();
        ui_.RenderOverlays();
        if (showDebugOverlay_) {
            char buf[512];
            std::snprintf(buf, sizeof(buf),
                          "FPS %.1f    frame %.2f ms\nscene %s   stack %d\ndraw calls %d   2D verts %d\n"
                          "3D triangles %d   lights %d\nGPU %s",
                          stats_.fps, stats_.frameMs, scenes_.CurrentName().c_str(),
                          scenes_.StackDepth(), stats_.drawCalls, stats_.vertices2D,
                          stats_.triangles, r3d_.LightCount(), gl::QueryGpuInfo().renderer.c_str());
            ui_.DrawDebugOverlay(buf);
        }
    }
    r2d_.EndFrame();

    // ---- фильтры + retro resolve ----------------------------------------
    const unsigned int finalFbo = headless ? headless->Fbo() : 0;
    if (filtersOn && frameTarget) {
        if (retroOn) {
            // Фильтруем виртуальное изображение и возвращаем его на место, чтобы
            // retro resolve применил проход CRT/ASCII/scanline поверх отфильтрованных пикселей.
            if (scratch_->Width() != frameW || scratch_->Height() != frameH) {
                RenderTargetDesc d;
                d.width = frameW;
                d.height = frameH;
                d.depth = false;
                d.colorFormat = PixelFormat::RGBA8;
                d.name = "engine-filter-scratch";
                scratch_->Create(d);
            }
            filters_->Apply(frameTarget->ColorTexture(), scratch_->Fbo(), frameW, frameH);
            gl::glBindFramebuffer(gl::GL_READ_FRAMEBUFFER, scratch_->Fbo());
            gl::glBindFramebuffer(gl::GL_DRAW_FRAMEBUFFER, frameTarget->Fbo());
            gl::glBlitFramebuffer(0, 0, frameW, frameH, 0, 0, frameW, frameH,
                                  gl::GL_COLOR_BUFFER_BIT, gl::GL_NEAREST);
            gl::glBindFramebuffer(gl::GL_FRAMEBUFFER, frameTarget->Fbo());
        } else {
            // Целевой кадр здесь всё ещё привязан; читаем из его текстуры и
            // пишем прямо в финальный фреймбуфер.
            filters_->Apply(frameTarget->ColorTexture(), finalFbo, frameW, frameH);
        }
    }

    if (retroOn && frameTarget) {
        // RetroDisplay::EndFrame разрешает виртуальное изображение в то, что
        // привязано, и сам отвязывает виртуальную цель, поэтому оставляем её
        // привязанной (или явно перевязываем после проходов фильтров выше).
        gl::glBindFramebuffer(gl::GL_FRAMEBUFFER, frameTarget->Fbo());
        retro_->EndFrame(r2d_, config_.retro, fbW, fbH);
    } else if (boundFrameTarget && frameTarget) {
        frameTarget->Unbind();
    }

    // ---- учёт статистики ------------------------------------------------
    const Renderer2D::Stats& r2dStats = r2d_.GetStats();
    stats_.drawCalls = r2dStats.drawCalls + r3d_.GetStats().drawCalls;
    stats_.vertices2D = r2dStats.vertices;
    stats_.triangles = r3d_.GetStats().triangles;
    stats_.frame = clock_.Frame();
    stats_.fps = clock_.FPS();
    stats_.frameMs = dt * 1000.0f;
    stats_.sceneStack = scenes_.StackDepth();
    stats_.sceneName = scenes_.CurrentName();
    stats_.activeVoices = config_.enableAudio ? Audio::Get().ActiveVoices() : 0;

    if (config_.enableAudio) Audio::Get().Update(scaled);

    frameInput.EndFrame();
    if (window_) window_->SwapBuffers();
}

int Engine::Run() {
    if (!initialised_) {
        ENG_LOGE("engine", "Run() called before a successful Init()");
        return 1;
    }
#if defined(ENG_PLATFORM_WASM)
    // Главный поток принадлежит браузеру: блокирующий while-цикл здесь навсегда
    // заморозил бы вкладку. Отдаём управление requestAnimationFrame; Step()
    // выполняется раз в кадр анимации. simulate_infinite_loop=1 разматывает стек
    // этого вызова, поэтому код после Run() (Shutdown в RunExample) не выполняется,
    // и цикл сам останавливает движок при выходе.
    emscripten_set_main_loop_arg(
        [](void* self) {
            auto& engine = *static_cast<Engine*>(self);
            engine.Step();
            if (engine.ShouldQuit()) {
                emscripten_cancel_main_loop();
                engine.Shutdown();
            }
        },
        this, 0, 1);
    return 0;
#else
    while (!quit_) {
        Step();
        if (config_.targetFps > 0) {
            f32 target = 1.0f / static_cast<f32>(config_.targetFps);
            f32 frameTime = clock_.UnscaledDelta();
            if (frameTime < target) SleepMs(static_cast<u32>((target - frameTime) * 1000.0f));
        }
    }
    return 0;
#endif
}

std::string Engine::Screenshot(const std::string& filename) {
    int fbW = window_ ? window_->FramebufferWidth() : config_.headlessTarget.width;
    int fbH = window_ ? window_->FramebufferHeight() : config_.headlessTarget.height;
    if (fbW <= 0 || fbH <= 0) return {};
    std::vector<u8> pixels;
    if (headlessTarget_ && headlessTarget_->Valid()) {
        if (!headlessTarget_->ReadPixels(&pixels)) return {};
    } else {
        pixels.resize(static_cast<usize>(fbW) * fbH * 4);
        gl::glPixelStorei(gl::GL_PACK_ALIGNMENT, 1);
        gl::glReadPixels(0, 0, fbW, fbH, gl::GL_RGBA, gl::GL_UNSIGNED_BYTE, pixels.data());
    }
    // Начало координат OpenGL — снизу слева; для PNG переворачиваем.
    std::vector<u8> flipped(pixels.size());
    for (int y = 0; y < fbH; ++y) {
        std::memcpy(&flipped[static_cast<usize>(y) * fbW * 4],
                    &pixels[static_cast<usize>(fbH - 1 - y) * fbW * 4],
                    static_cast<usize>(fbW) * 4);
    }
    std::string name = filename;
    if (name.empty()) {
        name = "screenshot_" + std::to_string(clock_.Frame()) + ".png";
    }
    std::string dir = PathJoin(GetUserRoot(), "screenshots");
    // stb_image_write не создаёт каталоги: делаем это через файловую систему.
    FS().WriteFile(PathJoin(dir, ".keep"), "", 0);
    std::string path = PathJoin(dir, name);
    if (!Texture::EncodePng(path, fbW, fbH, 4, flipped.data())) {
        ENG_LOGE("engine", "failed to write screenshot %s", path.c_str());
        return {};
    }
    ENG_LOGI("engine", "screenshot written to %s", path.c_str());
    return path;
}

Color Engine::ReadPixel(int x, int y) {
    if (headlessTarget_ && headlessTarget_->Valid()) {
        std::vector<u8> pixels;
        if (!headlessTarget_->ReadPixels(&pixels)) return Color::Black;
        int w = headlessTarget_->Width(), h = headlessTarget_->Height();
        // ReadPixels возвращает строки снизу вверх; вызывающие передают начало сверху слева.
        int yy = h - 1 - y;
        if (x < 0 || yy < 0 || x >= w || yy >= h) return Color::Black;
        usize i = (static_cast<usize>(yy) * w + x) * 4;
        if (i + 3 >= pixels.size()) return Color::Black;
        return Color::FromBytes(pixels[i], pixels[i + 1], pixels[i + 2], pixels[i + 3]);
    }
    if (!gl::glReadPixels) return Color::Black;
    u8 px[4] = {0, 0, 0, 0};
    gl::glReadPixels(x, y, 1, 1, gl::GL_RGBA, gl::GL_UNSIGNED_BYTE, px);
    return Color::FromBytes(px[0], px[1], px[2], px[3]);
}

int RunExample(const EngineConfig& config, const std::function<void(Engine&)>& setup) {
    Engine engine;
    EngineConfig cfg = config;
    if (!engine.Init(cfg)) {
        ENG_LOGE("engine", "engine initialisation failed");
        return 1;
    }
    if (setup) setup(engine);
    // Колбэк setup регистрирует сцены; открываем стартовую сцену сейчас.
    if (!cfg.startScene.empty()) engine.Scenes().SetScene(cfg.startScene);
    int rc = engine.Run();
    engine.Shutdown();
    return rc;
}

}  // namespace crossrender
