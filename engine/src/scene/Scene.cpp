#include "crossrender/scene/Scene.h"

#include "crossrender/Engine.h"
#include "crossrender/core/Log.h"
#include "crossrender/platform/Window.h"

#include <algorithm>

namespace crossrender {

SceneManager::SceneManager() = default;
SceneManager::~SceneManager() = default;

void SceneManager::Reset() {
    current_.reset();
    previous_.reset();
    stack_.clear();
    factories_.clear();
    currentName_.clear();
    transition_ = TransitionState{};
}

void SceneManager::Register(const std::string& name,
                            std::function<std::unique_ptr<Scene>()> factory) {
    factories_[name] = std::move(factory);
}

bool SceneManager::Exists(const std::string& name) const {
    return factories_.find(name) != factories_.end();
}

std::vector<std::string> SceneManager::RegisteredNames() const {
    std::vector<std::string> names;
    names.reserve(factories_.size());
    for (const auto& kv : factories_) names.push_back(kv.first);
    std::sort(names.begin(), names.end());
    return names;
}

bool SceneManager::SetScene(const std::string& name) {
    auto it = factories_.find(name);
    if (it == factories_.end() || !it->second) {
        ENG_LOGE("scene", "unknown scene '%s'", name.c_str());
        return false;
    }
    auto scene = it->second();
    if (!scene) return false;
    return SetScene(std::move(scene));
}

bool SceneManager::SetScene(std::unique_ptr<Scene> scene) {
    if (!scene) return false;
    if (current_ && ctx_) current_->OnExit(*ctx_);
    previous_ = std::move(current_);
    current_ = std::move(scene);
    currentName_ = current_->Name();
    if (ctx_) {
        current_->OnEnter(*ctx_);
        current_->OnResize(*ctx_, static_cast<int>(ctx_->viewport.w), static_cast<int>(ctx_->viewport.h));
    }
    ENG_LOGI("scene", "entered '%s'", currentName_.c_str());
    return true;
}

bool SceneManager::GoTo(const std::string& name, const TransitionDesc& t) {
    if (!Exists(name)) {
        ENG_LOGE("scene", "GoTo: unknown scene '%s'", name.c_str());
        return false;
    }
    if (t.type == TransitionType::None || t.duration <= 0) return SetScene(name);
    BeginTransition(t, [this, name]() { SetScene(name); });
    return true;
}

bool SceneManager::Push(const std::string& name, const TransitionDesc& t) {
    if (!Exists(name)) return false;
    auto factory = factories_[name];
    if (t.type == TransitionType::None || t.duration <= 0) {
        Push(factory());
        return true;
    }
    BeginTransition(t, [this, factory]() { Push(factory()); });
    return true;
}

void SceneManager::Push(std::unique_ptr<Scene> scene) {
    if (!scene) return;
    if (current_) {
        if (ctx_) current_->OnPause(*ctx_);
        stack_.push_back(std::move(current_));
    }
    current_ = std::move(scene);
    currentName_ = current_->Name();
    if (ctx_) {
        current_->OnEnter(*ctx_);
        current_->OnResize(*ctx_, static_cast<int>(ctx_->viewport.w), static_cast<int>(ctx_->viewport.h));
    }
}

bool SceneManager::Pop(const TransitionDesc& t) {
    if (stack_.empty()) return false;
    if (t.type == TransitionType::None || t.duration <= 0) {
        ApplyPending();
        auto prev = std::move(stack_.back());
        stack_.pop_back();
        if (current_ && ctx_) current_->OnExit(*ctx_);
        previous_ = std::move(current_);
        current_ = std::move(prev);
        currentName_ = current_->Name();
        if (ctx_) current_->OnResume(*ctx_);
        return true;
    }
    BeginTransition(t, [this]() {
        if (stack_.empty()) return;
        auto prev = std::move(stack_.back());
        stack_.pop_back();
        if (current_ && ctx_) current_->OnExit(*ctx_);
        previous_ = std::move(current_);
        current_ = std::move(prev);
        currentName_ = current_->Name();
        if (ctx_) current_->OnResume(*ctx_);
    });
    return true;
}

bool SceneManager::PopTo(const std::string& name) {
    while (current_ && current_->Name() != name && !stack_.empty()) {
        if (current_ && ctx_) current_->OnExit(*ctx_);
        current_ = std::move(stack_.back());
        stack_.pop_back();
    }
    if (current_) currentName_ = current_->Name();
    if (ctx_ && current_) current_->OnResume(*ctx_);
    return current_ && current_->Name() == name;
}

void SceneManager::BeginTransition(const TransitionDesc& t, std::function<void()> switchFn) {
    ApplyPending();
    if (transition_.active) {
        // Немедленно завершаем текущий переход, затем запускаем новый.
        transition_.active = false;
        if (transition_.pendingSwitch) transition_.pendingSwitch();
        transition_.pendingSwitch = nullptr;
    }
    transition_.active = true;
    transition_.desc = t;
    transition_.time = 0;
    transition_.pendingSwitch = std::move(switchFn);
    transition_.pushed = false;
}

void SceneManager::ApplyPending() {
    if (transition_.pendingSwitch) {
        auto fn = std::move(transition_.pendingSwitch);
        transition_.pendingSwitch = nullptr;
        fn();
    }
}

f32 SceneManager::TransitionProgress() const {
    if (!transition_.active) return 1.0f;
    f32 d = transition_.desc.duration > 0 ? transition_.desc.duration : 0.001f;
    return Clamp(transition_.time / d, 0.0f, 1.0f);
}

bool SceneManager::InputBlocked() const {
    if (!transition_.active) return false;
    // Блокируем ввод, пока экран закрыт, т.е. в первую половину затухания.
    return TransitionProgress() < 0.5f;
}

void SceneManager::Update(f32 dt) {
    if (!current_) return;

    if (transition_.active) {
        transition_.time += dt;
        f32 d = transition_.desc.duration > 0 ? transition_.desc.duration : 0.001f;
        f32 half = d * 0.5f;
        if (!transition_.pushed && transition_.time >= half) {
            // Точка невозврата: меняем сцены, пока экран полностью закрыт.
            if (transition_.pendingSwitch) {
                auto fn = std::move(transition_.pendingSwitch);
                transition_.pendingSwitch = nullptr;
                fn();
            }
            transition_.pushed = true;
        }
        if (transition_.time >= d) {
            transition_.active = false;
            transition_.pushed = false;
        }
    }

    if (current_ && ctx_) current_->Update(*ctx_, dt);
}

void SceneManager::Prepare3D() {
    if (!current_ || !ctx_) return;
    if (current_->Wants3D()) current_->Prepare3D(*ctx_);
}

void SceneManager::Render3D() {
    if (!current_ || !ctx_) return;
    if (current_->Wants3D()) current_->Render3D(*ctx_);
}

void SceneManager::Render2D() {
    if (!current_ || !ctx_) return;
    current_->Render2D(*ctx_);
}

void SceneManager::RenderTransition(Renderer2D& r2d, const Rect& screen) {
    if (!transition_.active) return;
    f32 p = TransitionProgress();
    const TransitionDesc& t = transition_.desc;
    r2d.Save();
    switch (t.type) {
        case TransitionType::Fade: {
            // Закрытие в первой половине, открытие во второй.
            f32 alpha = p < 0.5f ? p * 2.0f : (1.0f - p) * 2.0f;
            r2d.FillRect(screen.x, screen.y, screen.w, screen.h, t.color.WithAlpha(alpha));
            break;
        }
        case TransitionType::CrossDissolve: {
            f32 alpha = p < 0.5f ? p * 2.0f : (1.0f - p) * 2.0f;
            r2d.FillRect(screen.x, screen.y, screen.w, screen.h, t.color.WithAlpha(alpha * 0.85f));
            break;
        }
        case TransitionType::SlideLeft:
        case TransitionType::SlideRight:
        case TransitionType::SlideUp:
        case TransitionType::SlideDown: {
            f32 cover = p < 0.5f ? p * 2.0f : (1.0f - p) * 2.0f;
            // При p = 0.5 «шторка» полностью на экране.
            f32 w = screen.w, h = screen.h;
            f32 x = screen.x, y = screen.y;
            f32 e = cover;
            switch (t.type) {
                case TransitionType::SlideLeft: x = screen.x + w * (1.0f - e); break;
                case TransitionType::SlideRight: x = screen.x - w * (1.0f - e); break;
                case TransitionType::SlideUp: y = screen.y + h * (1.0f - e); break;
                default: y = screen.y - h * (1.0f - e); break;
            }
            r2d.FillRect(x, y, w, h, t.color);
            break;
        }
        case TransitionType::CircleWipe: {
            f32 radius = p < 0.5f ? p * 2.0f : (1.0f - p) * 2.0f;
            Vec2 c = screen.Center();
            f32 maxR = Length(Vec2{screen.w, screen.h}) * 0.5f;
            r2d.Circle(c.x, c.y, maxR * radius * 1.2f);
            r2d.FillColor(t.color);
            r2d.Fill();
            // Углы вне круга заполняем тем же цветом с полной альфой, только
            // когда круг почти полностью раскрыт.
            if (radius > 0.98f) r2d.FillRect(screen.x, screen.y, screen.w, screen.h, t.color);
            break;
        }
        default: break;
    }
    r2d.Restore();
}

void SceneManager::Resize(int w, int h) {
    if (current_ && ctx_) current_->OnResize(*ctx_, w, h);
}

bool SceneManager::Back() {
    if (!current_ || !ctx_) return false;
    if (current_->OnBackPressed(*ctx_)) return true;
    if (!stack_.empty()) return Pop();
    return false;
}

}  // namespace crossrender
