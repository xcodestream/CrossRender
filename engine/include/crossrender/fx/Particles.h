//
// CrossRender
// Автор: xcodestream <xcodestream@gmail.com>
// Лицензия: MIT
// Дата: 15.05.2026
// Описание: система 3D-частиц: CPU-симуляция, эмиттеры и кривые поведения по времени жизни.
//
#pragma once

#include "crossrender/gfx/Mesh.h"
#include "crossrender/core/Base.h"
#include "crossrender/core/Math.h"

#include <memory>
#include <string>
#include <vector>
#include <functional>

namespace crossrender {

class Renderer3D;
class Camera;

// Скалярная/цветовая кривая, выборка по времени жизни частицы (t в [0,1]).
struct Curve {
    struct Key {
        f32 t = 0;
        f32 v = 0;
        // Безье-ручки для плавной интерполяции (easing в стиле Lottie).
        f32 outX = 0.33f, outY = 0.33f, inX = 0.67f, inY = 0.67f;
        int easing = 0;  // 0 — линейная, 1 — безье, 2 — удержание
    };
    std::vector<Key> keys;
    f32 minValue = 0, maxValue = 1;

    static Curve Constant(f32 v);
    // Кривые-рампы: ключи — пары (t, значение); сглаживание в [0,1] (0 = линейно).
    static Curve FromPoints(const std::vector<Vec2>& points, f32 smoothness = 0.0f);
    [[nodiscard]] f32 Evaluate(f32 t) const;
    void AddKey(f32 t, f32 v, int easing = 0);
    // Случайное значение в диапазоне значений кривой (разброс применяется позже).
    [[nodiscard]] f32 RandomValue(class Random& rng) const;
};

struct ColorCurve {
    std::vector<std::pair<f32, Color>> keys;
    static ColorCurve Constant(const Color& c);
    static ColorCurve Gradient(const std::vector<std::pair<f32, Color>>& keys);
    [[nodiscard]] Color Evaluate(f32 t) const;
};

// Вспомогательный случайный диапазон, используемый по всему описанию эмиттера.
struct RangeF {
    f32 min = 0, max = 0;
    RangeF() = default;
    RangeF(f32 v) : min(v), max(v) {}
    RangeF(f32 a, f32 b) : min(a), max(b) {}
    [[nodiscard]] f32 Sample(class Random& rng) const { return rng.Range(min, max); }
};

struct RangeV3 {
    Vec3 min{0, 0, 0}, max{0, 0, 0};
    RangeV3() = default;
    RangeV3(const Vec3& v) : min(v), max(v) {}
    RangeV3(const Vec3& a, const Vec3& b) : min(a), max(b) {}
    [[nodiscard]] Vec3 Sample(class Random& rng) const {
        return {rng.Range(min.x, max.x), rng.Range(min.y, max.y), rng.Range(min.z, max.z)};
    }
};

enum class EmitterShape : u8 { Point, Sphere, Hemisphere, Box, Cone, Circle, Edge, Mesh };
enum class SimulationSpace : u8 { World, Local };
enum class ParticleRenderMode : u8 { Billboard, StretchedBillboard, HorizontalBillboard, VerticalBillboard, Mesh, Trail };
enum class ParticleBlendMode : u8 { Alpha, Additive, Multiply, Premultiplied, Screen };

struct ParticleEmitterDesc {
    std::string name = "Particles";
    // Эмиссия
    f32 rate = 100.0f;             // частиц в секунду
    int burst = 0;                 // частиц, выбрасываемых на старте
    f32 duration = 0.0f;           // 0 = бесконечно
    bool looping = true;
    int maxParticles = 2000;
    // Время жизни / скорость
    RangeF lifetime{1.0f, 2.0f};
    RangeF startSpeed{1.0f, 2.0f};
    RangeF startSize{0.05f, 0.15f};
    RangeF startRotation{0.0f, kTau};
    RangeF startAngularVelocity{-2.0f, 2.0f};
    ColorCurve startColor = ColorCurve::Constant(Color::White);
    Curve sizeOverLifetime = Curve::Constant(1.0f);
    Curve alphaOverLifetime = Curve::FromPoints({{0, 1}, {1, 0}});
    Curve speedOverLifetime = Curve::Constant(1.0f);
    Curve rotationOverLifetime = Curve::Constant(0.0f);
    ColorCurve colorOverLifetime = ColorCurve::Constant(Color::White);
    // Форма
    EmitterShape shape = EmitterShape::Sphere;
    RangeF shapeRadius{0.1f, 0.5f};
    Vec3 shapeBox{1, 1, 1};
    f32 coneAngle = 25.0f * kDeg2Rad;
    // Силы
    Vec3 gravity{0, -9.81f, 0};
    Vec3 wind{0, 0, 0};
    f32 drag = 0.0f;
    f32 turbulenceStrength = 0.0f;
    f32 turbulenceFrequency = 1.0f;
    // Аттракторы (позиция, сила, радиус), вычисляемые на каждом шаге.
    struct Attractor {
        Vec3 position;
        f32 strength = 0;
        f32 radius = 5.0f;
    };
    std::vector<Attractor> attractors;
    // Вихрь
    Vec3 vortexAxis{0, 1, 0};
    f32 vortexStrength = 0.0f;
    // Столкновения
    bool collideGround = false;
    f32 groundY = 0.0f;
    f32 bounce = 0.4f;
    f32 friction = 0.7f;
    // Шум / случайность
    RangeF sizeVariance{1, 1};
    // Рендеринг
    ParticleRenderMode renderMode = ParticleRenderMode::Billboard;
    ParticleBlendMode blendMode = ParticleBlendMode::Additive;
    const Texture* texture = nullptr;
    const Mesh* mesh = nullptr;
    bool softParticles = false;
    f32 softFadeDistance = 0.5f;
    bool lit = false;
    bool castShadows = false;
    bool alignToVelocity = false;
    f32 stretchScale = 0.1f;
    bool sortByDepth = true;
    bool receiveFog = true;
    SimulationSpace space = SimulationSpace::World;
    // Суб-эмиттеры: порождаются при рождении/смерти частицы.
    struct SubEmitter {
        std::string emitterName;
        int trigger = 0;  // 0 — рождение, 1 — смерть, 2 — столкновение
        int count = 4;
    };
    std::vector<SubEmitter> subEmitters;
};

struct Particle {
    Vec3 position;
    Vec3 velocity;
    Vec3 spawnPosition;
    Color color;
    f32 size = 1, rotation = 0, angularVelocity = 0;
    f32 age = 0, lifetime = 1;
    f32 seed = 0;
    bool alive = true;
    [[nodiscard]] f32 NormalizedAge() const { return lifetime > 0 ? age / lifetime : 1.0f; }
};

class ParticleSystem {
public:
    ParticleSystem();
    ~ParticleSystem();
    ParticleSystem(const ParticleSystem&) = delete;
    ParticleSystem& operator=(const ParticleSystem&) = delete;

    // `emitters` симулируются вместе (суб-эмиттеры поддерживаются по имени).
    void Init(const std::vector<ParticleEmitterDesc>& emitters, u64 seed = 12345);
    void Shutdown();

    void SetTransform(const Mat4& t) { transform_ = t; }
    [[nodiscard]] const Mat4& Transform() const { return transform_; }
    void SetPlaying(bool playing) { playing_ = playing; }
    [[nodiscard]] bool IsPlaying() const { return playing_; }
    void Restart();
    void Simulate(f32 dt);
    void Render(Renderer3D& renderer, const Camera& camera);
    // Обновление + отрисовка одним вызовом.
    void Update(Renderer3D& renderer, const Camera& camera, f32 dt) {
        Simulate(dt);
        Render(renderer, camera);
    }

    [[nodiscard]] int AliveCount() const;
    [[nodiscard]] int Capacity() const;
    [[nodiscard]] int EmitterCount() const { return static_cast<int>(emitters_.size()); }
    [[nodiscard]] ParticleEmitterDesc& Emitter(int i) { return emitters_[static_cast<usize>(i)]; }
    [[nodiscard]] const ParticleEmitterDesc& Emitter(int i) const {
        return emitters_[static_cast<usize>(i)];
    }
    // Прямой доступ к частицам (тесты / игровые запросы).
    [[nodiscard]] const Particle* Particles(int emitterIndex) const;
    // Немедленно выбрасывает `count` частиц на указанном эмиттере.
    void EmitBurst(int emitterIndex, int count);
    // Освобождает все частицы.
    void Clear();
    [[nodiscard]] const Bounds& Bounds() const { return bounds_; }

    // Встроенные пресеты, используемые демо-сценой.
    static std::vector<ParticleEmitterDesc> PresetFire();
    static std::vector<ParticleEmitterDesc> PresetSmoke();
    static std::vector<ParticleEmitterDesc> PresetSparks();
    static std::vector<ParticleEmitterDesc> PresetMagic();
    static std::vector<ParticleEmitterDesc> PresetFountain();
    static std::vector<ParticleEmitterDesc> PresetExplosion();
    static std::vector<ParticleEmitterDesc> PresetSnow();
    static std::vector<ParticleEmitterDesc> PresetTrail();

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
    std::vector<ParticleEmitterDesc> emitters_;
    Mat4 transform_ = Mat4::Identity();
    crossrender::Bounds bounds_;
    bool playing_ = true;
};

}  // namespace crossrender
