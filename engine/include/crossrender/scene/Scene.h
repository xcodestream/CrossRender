//
// CrossRender
// Автор: xcodestream <xcodestream@gmail.com>
// Лицензия: MIT
// Дата: 15.05.2026
// Описание: система сцен: экраны приложения, переходы и стек сцен.
//
#pragma once

#include "crossrender/ui/Ui.h"
#include "crossrender/core/Base.h"
#include "crossrender/core/Math.h"
#include "crossrender/gfx/Renderer2D.h"
#include "crossrender/gfx/Renderer3D.h"

#include <memory>
#include <string>
#include <vector>
#include <functional>
#include <unordered_map>

namespace crossrender {

class Engine;
class SceneManager;

// ---------------------------------------------------------------------------
// Сцена
// ---------------------------------------------------------------------------
struct SceneContext {
    Engine* engine = nullptr;
    SceneManager* scenes = nullptr;
    Renderer2D* r2d = nullptr;
    Renderer3D* r3d = nullptr;
    UiContext* ui = nullptr;
    Rect viewport;      // логический прямоугольник вьюпорта (уже с поправкой на safe-area)
    f32 dpiScale = 1;
    f32 dt = 0;
};

class Scene {
public:
    Scene() = default;
    virtual ~Scene() = default;
    Scene(const Scene&) = delete;
    Scene& operator=(const Scene&) = delete;

    [[nodiscard]] virtual const char* Name() const = 0;
    // Вызывается, когда сцена становится активной (после старта перехода).
    virtual void OnEnter(SceneContext& ctx) { (void)ctx; }
    // Вызывается, когда сцену вот-вот заменят (старт исходящего перехода).
    virtual void OnExit(SceneContext& ctx) { (void)ctx; }
    // Покадровое обновление. `dt` — масштабированное время кадра.
    virtual void Update(SceneContext& ctx, f32 dt) { (void)ctx; (void)dt; }
    // Вызывается раз в кадр ДО начала 3D-прохода, чтобы сцена задала
    // камеру / свет / окружение рендерера именно для этого кадра.
    virtual void Prepare3D(SceneContext& ctx) { (void)ctx; }
    // Рисует 3D-мир (вызывается только при Wants3D()).
    virtual void Render3D(SceneContext& ctx) { (void)ctx; }
    // Рисует 2D/UI-содержимое (вызывается всегда; также служит оверлеем 3D).
    virtual void Render2D(SceneContext& ctx) { (void)ctx; }
    // Вызывается после вывода кадра на экран (для отложенной работы).
    virtual void PostFrame(SceneContext& ctx, f32 dt) { (void)ctx; (void)dt; }
    virtual void OnResize(SceneContext& ctx, int width, int height) { (void)ctx; (void)width; (void)height; }
    virtual bool OnBackPressed(SceneContext& ctx) { (void)ctx; return false; }
    // Пауза/возобновление, когда поверх кладут другую сцену.
    virtual void OnPause(SceneContext& ctx) { (void)ctx; }
    virtual void OnResume(SceneContext& ctx) { (void)ctx; }

    [[nodiscard]] virtual bool Wants3D() const { return false; }
    // 3D рисуется во внеэкранную HDR-цель и компонуется под 2D-проходом.
    [[nodiscard]] virtual bool WantsPostProcessing() const { return false; }
    [[nodiscard]] virtual bool WantsUiCapture() const { return false; }
    // Цвет фона сцены (используется, когда Wants3D() возвращает false).
    [[nodiscard]] virtual Color ClearColor() const { return Color::FromARGB(0xFF0E1016); }
    // Позволяет сцене распоряжаться всем кадром (пропускает стандартную очистку).
    [[nodiscard]] virtual bool CustomClear() const { return false; }
    // Необязательное человекочитаемое описание в списке сцен примера.
    [[nodiscard]] virtual const char* Description() const { return ""; }
};

// ---------------------------------------------------------------------------
// Переход между сценами
// ---------------------------------------------------------------------------
enum class TransitionType : u8 { None, Fade, SlideLeft, SlideRight, SlideUp, SlideDown, CircleWipe, CrossDissolve };

struct TransitionDesc {
    // По умолчанию — мгновенное переключение, чтобы программные GoTo/Push/Pop
    // были синхронными; для анимированного перехода передайте явный тип.
    TransitionType type = TransitionType::None;
    f32 duration = 0.35f;
    Color color = Color{0, 0, 0, 1};
    // Рисует обе сцены одновременно (тяжелее) вместо полного перекрытия.
    bool crossFade = false;
};

// ---------------------------------------------------------------------------
// SceneManager
// ---------------------------------------------------------------------------
class SceneManager {
public:
    SceneManager();
    ~SceneManager();

    void SetContext(SceneContext* ctx) { ctx_ = ctx; }
    // Сбрасывает все сцены и фабрики (используется при завершении работы движка).
    void Reset();
    void SetRenderer2D(Renderer2D* r) { r2d_ = r; }

    // Регистрирует фабрику, чтобы сцены можно было создавать по имени.
    void Register(const std::string& name, std::function<std::unique_ptr<Scene>()> factory);

    // Немедленное переключение (без перехода).
    bool SetScene(const std::string& name);
    bool SetScene(std::unique_ptr<Scene> scene);
    // Переключение с переходом.
    bool GoTo(const std::string& name, const TransitionDesc& t = {});
    // Кладёт сцену поверх (предыдущая приостанавливается, но хранится для Back).
    bool Push(const std::string& name, const TransitionDesc& t = {});
    void Push(std::unique_ptr<Scene> scene);
    bool Pop(const TransitionDesc& t = {});
    // Снимает стек до конкретной зарегистрированной сцены.
    bool PopTo(const std::string& name);

    [[nodiscard]] Scene* Current() const { return current_.get(); }
    [[nodiscard]] Scene* Previous() const { return previous_.get(); }
    [[nodiscard]] const std::string& CurrentName() const { return currentName_; }
    [[nodiscard]] int StackDepth() const { return static_cast<int>(stack_.size()); }
    [[nodiscard]] bool Transitioning() const { return transition_.active; }
    [[nodiscard]] f32 TransitionProgress() const;
    // Истинно, когда кадр занят затуханием перехода (пропустить ввод сцены).
    [[nodiscard]] bool InputBlocked() const;

    // Покадровый драйвер.
    void Update(f32 dt);
    void Prepare3D();
    void Render3D();
    void Render2D();
    // Вызывается Engine после прохода сцены: рисует оверлеи переходов.
    void RenderTransition(Renderer2D& r2d, const Rect& screen);
    void Resize(int w, int h);
    // Обрабатывает системную кнопку «назад» (Android). Возвращает true, если обработана.
    bool Back();

    [[nodiscard]] bool Exists(const std::string& name) const;
    [[nodiscard]] std::vector<std::string> RegisteredNames() const;

private:
    struct TransitionState {
        bool active = false;
        TransitionDesc desc;
        f32 time = 0;
        std::function<void()> pendingSwitch;
        bool pushed = false;
    };
    void BeginTransition(const TransitionDesc& t, std::function<void()> switchFn);
    void ApplyPending();

    SceneContext* ctx_ = nullptr;
    Renderer2D* r2d_ = nullptr;
    std::unique_ptr<Scene> current_, previous_, pending_;
    std::vector<std::unique_ptr<Scene>> stack_;
    std::unordered_map<std::string, std::function<std::unique_ptr<Scene>()>> factories_;
    std::string currentName_;
    TransitionState transition_;
};

}  // namespace crossrender
