// Тесты системы частиц (симуляция на CPU, кривые, формы, суб-эмиттеры).
// Все тесты симуляции не требуют GL; единственный smoke-тест рендера защищён
// ENG_REQUIRE_GL и пропускается, если контекст недоступен.

#include "crossrender/fx/Particles.h"

#include "crossrender/test/Test.h"
#include "crossrender/gfx/Renderer3D.h"

#include <cmath>
#include <string>
#include <vector>
#include <cstring>

using namespace crossrender;

namespace {

int CountAlive(const Particle* p, int capacity) {
    if (p == nullptr) return 0;
    int n = 0;
    for (int i = 0; i < capacity; ++i) {
        if (p[i].alive) ++n;
    }
    return n;
}

f32 MeanY(const Particle* p, int capacity) {
    if (p == nullptr) return 0.0f;
    f64 sum = 0.0;
    int n = 0;
    for (int i = 0; i < capacity; ++i) {
        if (!p[i].alive) continue;
        sum += static_cast<f64>(p[i].position.y);
        ++n;
    }
    return n > 0 ? static_cast<f32>(sum / n) : 0.0f;
}

bool AllFinite(const Particle* p, int capacity) {
    if (p == nullptr) return false;
    for (int i = 0; i < capacity; ++i) {
        if (!p[i].alive) continue;
        const Particle& q = p[i];
        if (!std::isfinite(q.position.x) || !std::isfinite(q.position.y) ||
            !std::isfinite(q.position.z))
            return false;
        if (!std::isfinite(q.velocity.x) || !std::isfinite(q.velocity.y) ||
            !std::isfinite(q.velocity.z))
            return false;
        if (!std::isfinite(q.size) || !std::isfinite(q.rotation) || !std::isfinite(q.age))
            return false;
    }
    return true;
}

// Детерминированный эмиттер, используемый тестами сил / форм.
ParticleEmitterDesc MakeEmitter(int maxParticles = 1000) {
    ParticleEmitterDesc d;
    d.name = "Test";
    d.rate = 100.0f;
    d.burst = 0;
    d.duration = 0.0f;
    d.looping = true;
    d.maxParticles = maxParticles;
    d.lifetime = {10.0f, 10.0f};
    d.startSpeed = {0.0f, 0.0f};
    d.startSize = {0.1f, 0.1f};
    d.startRotation = {0.0f, 0.0f};
    d.startAngularVelocity = {0.0f, 0.0f};
    d.shape = EmitterShape::Point;
    d.gravity = {0.0f, 0.0f, 0.0f};
    return d;
}

}  // namespace

// ---------------------------------------------------------------------------
// Кривые
// ---------------------------------------------------------------------------
ENG_TEST(Particles, CurveLinearHoldAndClamping) {
    const Curve linear = Curve::FromPoints({{0.0f, 0.0f}, {1.0f, 10.0f}});
    ENG_CHECK_NEAR(linear.Evaluate(0.5f), 5.0f, 0.05f);
    ENG_CHECK_NEAR(linear.Evaluate(0.25f), 2.5f, 0.05f);
    ENG_CHECK_NEAR(linear.Evaluate(0.0f), 0.0f, 1e-5f);
    ENG_CHECK_NEAR(linear.Evaluate(1.0f), 10.0f, 1e-5f);
    // Ограничение (clamp) вне диапазона ключей.
    ENG_CHECK_NEAR(linear.Evaluate(-3.0f), 0.0f, 1e-5f);
    ENG_CHECK_NEAR(linear.Evaluate(4.0f), 10.0f, 1e-5f);
    ENG_CHECK_NEAR(linear.minValue, 0.0f, 1e-6f);
    ENG_CHECK_NEAR(linear.maxValue, 10.0f, 1e-6f);

    // Ключи hold сохраняют значение левого ключа на всём сегменте.
    Curve hold;
    hold.AddKey(0.0f, 1.0f, 2);
    hold.AddKey(0.5f, 7.0f, 2);
    hold.AddKey(1.0f, 3.0f, 2);
    ENG_CHECK_NEAR(hold.Evaluate(0.1f), 1.0f, 1e-5f);
    ENG_CHECK_NEAR(hold.Evaluate(0.49f), 1.0f, 1e-5f);
    ENG_CHECK_NEAR(hold.Evaluate(0.5f), 7.0f, 1e-5f);
    ENG_CHECK_NEAR(hold.Evaluate(0.99f), 7.0f, 1e-5f);
    ENG_CHECK_NEAR(hold.Evaluate(1.0f), 3.0f, 1e-5f);

    // Константа.
    const Curve c = Curve::Constant(3.5f);
    ENG_CHECK_NEAR(c.Evaluate(0.4f), 3.5f, 1e-5f);
    ENG_CHECK_NEAR(c.minValue, 3.5f, 1e-6f);
    ENG_CHECK_NEAR(c.maxValue, 3.5f, 1e-6f);

    // Неотсортированный AddKey сохраняет сортировку кривой и корректное вычисление.
    Curve mixed;
    mixed.AddKey(1.0f, 4.0f);
    mixed.AddKey(0.0f, 0.0f);
    mixed.AddKey(0.5f, 1.0f);
    ENG_CHECK_NEAR(mixed.Evaluate(0.5f), 1.0f, 1e-5f);
    ENG_CHECK_NEAR(mixed.Evaluate(0.75f), 2.5f, 0.1f);

    // RandomValue сэмплирует внутри диапазона значений кривой.
    Random rng(1234u);
    const Curve ramp = Curve::FromPoints({{0.0f, 2.0f}, {1.0f, 5.0f}});
    for (int i = 0; i < 64; ++i) {
        const f32 v = ramp.RandomValue(rng);
        ENG_CHECK(v >= 2.0f - 1e-5f && v <= 5.0f + 1e-5f);
    }
    const Curve flat = Curve::Constant(7.0f);
    ENG_CHECK_NEAR(flat.RandomValue(rng), 7.0f, 1e-5f);
}

ENG_TEST(Particles, CurveFromPointsPassesThroughPoints) {
    const std::vector<Vec2> pts{{0.0f, 0.2f}, {0.25f, 1.0f}, {0.6f, 0.15f}, {1.0f, 0.8f}};
    const Curve smooth = Curve::FromPoints(pts, 0.7f);
    for (const Vec2& p : pts) {
        ENG_CHECK_NEAR(smooth.Evaluate(p.x), p.y, 1e-3f);
    }
    const Curve sharp = Curve::FromPoints(pts, 0.0f);
    for (const Vec2& p : pts) {
        ENG_CHECK_NEAR(sharp.Evaluate(p.x), p.y, 1e-3f);
    }
    // Сегмент со сглаживанием Безье всё равно интерполируется монотонно между ключами.
    const f32 mid = smooth.Evaluate(0.42f);
    ENG_CHECK(mid >= 0.14f && mid <= 1.01f);
}

ENG_TEST(Particles, ColorCurveGradientAndConstant) {
    const ColorCurve g = ColorCurve::Gradient(
        {{0.0f, Color(0.0f, 0.0f, 0.0f, 1.0f)}, {1.0f, Color(1.0f, 1.0f, 1.0f, 1.0f)}});
    const Color mid = g.Evaluate(0.5f);
    ENG_CHECK_NEAR(mid.r, 0.5f, 1e-4f);
    ENG_CHECK_NEAR(mid.g, 0.5f, 1e-4f);
    ENG_CHECK_NEAR(mid.b, 0.5f, 1e-4f);
    ENG_CHECK_NEAR(mid.a, 1.0f, 1e-4f);

    // Ограничение на обоих концах, а неотсортированные входные ключи сортируются.
    const ColorCurve unsorted = ColorCurve::Gradient(
        {{1.0f, Color(0.0f, 0.0f, 0.0f, 1.0f)}, {0.0f, Color(1.0f, 1.0f, 1.0f, 1.0f)}});
    ENG_CHECK_NEAR(unsorted.Evaluate(0.5f).r, 0.5f, 1e-4f);
    ENG_CHECK_NEAR(unsorted.Evaluate(-1.0f).r, 1.0f, 1e-4f);
    ENG_CHECK_NEAR(unsorted.Evaluate(2.0f).r, 0.0f, 1e-4f);

    const ColorCurve c = ColorCurve::Constant(Color(0.2f, 0.4f, 0.6f, 0.8f));
    ENG_CHECK_NEAR(c.Evaluate(0.7f).g, 0.4f, 1e-4f);
    ENG_CHECK_NEAR(c.Evaluate(0.0f).a, 0.8f, 1e-4f);
}

// ---------------------------------------------------------------------------
// Эмиссия / время жизни
// ---------------------------------------------------------------------------
ENG_TEST(Particles, EmissionRateIsRespected) {
    ParticleEmitterDesc d = MakeEmitter(1000);
    d.rate = 100.0f;
    d.lifetime = {10.0f, 10.0f};
    d.burst = 0;

    ParticleSystem ps;
    ps.Init({d}, 42u);
    ENG_CHECK_EQ(ps.AliveCount(), 0);
    ENG_CHECK_EQ(ps.Capacity(), 1000);

    int peak = 0;
    for (int i = 0; i < 10; ++i) {
        ps.Simulate(0.1f);  // всего 1.0 с
        peak = peak > ps.AliveCount() ? peak : ps.AliveCount();
    }
    ENG_CHECK_NEAR(ps.AliveCount(), 100, 2);
    ENG_CHECK(peak <= 1000);
}

ENG_TEST(Particles, ParticlesDieAfterLifetime) {
    ParticleEmitterDesc d = MakeEmitter(500);
    d.rate = 200.0f;
    d.duration = 0.2f;
    d.looping = false;
    d.lifetime = {0.3f, 0.3f};

    ParticleSystem ps;
    ps.Init({d}, 7u);
    ps.Simulate(0.25f);
    ENG_CHECK(ps.AliveCount() > 0);
    ENG_CHECK(ps.IsPlaying());

    ps.Simulate(1.0f);
    ENG_CHECK_EQ(ps.AliveCount(), 0);
    ENG_CHECK(!ps.IsPlaying());
}

ENG_TEST(Particles, GravityPullsParticlesDown) {
    ParticleEmitterDesc d = MakeEmitter(200);
    d.rate = 0.0f;
    d.burst = 10;
    d.lifetime = {10.0f, 10.0f};
    d.startSpeed = {0.5f, 0.5f};  // точечная эмиссия без конуса вдоль +Y
    d.gravity = {0.0f, -9.81f, 0.0f};

    ParticleSystem ps;
    ps.Init({d}, 3u);
    ps.Simulate(0.1f);
    const f32 y0 = MeanY(ps.Particles(0), 200);
    ps.Simulate(0.4f);
    const f32 y1 = MeanY(ps.Particles(0), 200);
    ENG_CHECK(y1 < y0);
    ENG_CHECK(y1 < -0.2f);
}

ENG_TEST(Particles, GroundCollisionStopsParticles) {
    ParticleEmitterDesc d = MakeEmitter(64);
    d.rate = 0.0f;
    d.burst = 5;
    d.lifetime = {10.0f, 10.0f};
    d.shape = EmitterShape::Point;
    d.gravity = {0.0f, -9.81f, 0.0f};
    d.collideGround = true;
    d.groundY = 0.0f;
    d.bounce = 0.0f;
    d.friction = 0.7f;

    ParticleSystem ps;
    ps.Init({d}, 11u);
    ps.SetTransform(Mat4::Translate({0.0f, 2.0f, 0.0f}));
    ps.Simulate(2.0f);

    const Particle* p = ps.Particles(0);
    const int alive = CountAlive(p, 64);
    ENG_CHECK_EQ(alive, 5);
    for (int i = 0; i < 64; ++i) {
        if (!p[i].alive) continue;
        ENG_CHECK(p[i].position.y >= d.groundY - 1e-4f);
        ENG_CHECK(std::fabs(p[i].velocity.y) <= 1e-3f);  // bounce 0 => частицы успокоились
    }
    // Эмиттер с отскоком вместо этого сохраняет вертикальное движение.
    ps.Emitter(0).bounce = 0.6f;
    ps.Restart();
    ps.Simulate(0.7f);
    bool anyBounce = false;
    for (int i = 0; i < 64; ++i) {
        if (p[i].alive && p[i].velocity.y > 0.05f) anyBounce = true;
    }
    ENG_CHECK(anyBounce);
}

// ---------------------------------------------------------------------------
// Детерминизм / устойчивость
// ---------------------------------------------------------------------------
ENG_TEST(Particles, DeterminismWithSameSeed) {
    auto run = [](std::vector<f32>* out) {
        ParticleEmitterDesc d = MakeEmitter(400);
        d.rate = 137.0f;
        d.lifetime = {0.8f, 2.0f};
        d.shape = EmitterShape::Sphere;
        d.shapeRadius = {0.1f, 0.6f};
        d.gravity = {0.0f, -4.0f, 0.0f};
        d.wind = {0.5f, 0.0f, 0.2f};
        d.drag = 0.3f;
        d.turbulenceStrength = 0.7f;
        d.startSpeed = {1.0f, 3.0f};
        d.startSize = {0.05f, 0.2f};
        d.attractors.push_back({{0.0f, 1.0f, 0.0f}, 2.0f, 4.0f});
        d.vortexAxis = {0.0f, 1.0f, 0.0f};
        d.vortexStrength = 1.3f;
        d.collideGround = true;
        d.groundY = -1.0f;
        d.bounce = 0.3f;

        ParticleSystem ps;
        ps.Init({d}, 999u);
        for (int i = 0; i < 100; ++i) ps.Simulate(1.0f / 60.0f);

        const Particle* p = ps.Particles(0);
        out->clear();
        out->reserve(400 * 7);
        for (int i = 0; i < 400; ++i) {
            out->push_back(p[i].position.x);
            out->push_back(p[i].position.y);
            out->push_back(p[i].position.z);
            out->push_back(p[i].velocity.x);
            out->push_back(p[i].velocity.y);
            out->push_back(p[i].velocity.z);
            out->push_back(p[i].alive ? 1.0f : 0.0f);
        }
    };

    std::vector<f32> a, b;
    run(&a);
    run(&b);
    ENG_CHECK_EQ(a.size(), b.size());
    ENG_CHECK(a.size() == b.size());
    if (a.size() == b.size() && !a.empty()) {
        ENG_CHECK(std::memcmp(a.data(), b.data(), a.size() * sizeof(f32)) == 0);
    }
}

ENG_TEST(Particles, HugeDeltaTimeIsSafe) {
    ParticleEmitterDesc d = MakeEmitter(300);
    d.rate = 1000.0f;
    d.lifetime = {0.5f, 1.5f};
    d.shape = EmitterShape::Sphere;
    d.shapeRadius = {0.1f, 0.5f};
    d.startSpeed = {2.0f, 8.0f};
    d.gravity = {0.0f, -9.81f, 0.0f};
    d.drag = 0.5f;
    d.turbulenceStrength = 1.0f;
    d.collideGround = true;
    d.groundY = 0.0f;
    d.bounce = 0.4f;

    ParticleSystem ps;
    ps.Init({d}, 77u);
    ps.Simulate(2.0f);

    const Particle* p = ps.Particles(0);
    ENG_CHECK(ps.AliveCount() <= 300);
    ENG_CHECK(AllFinite(p, 300));
    ENG_CHECK(ps.Bounds().Valid());
    if (ps.Bounds().Valid()) {
        ENG_CHECK(ps.Bounds().min.y >= d.groundY - 100.0f);
    }
    // NaN / отрицательный dt игнорируются, а не портят состояние.
    ps.Simulate(std::nanf(""));
    ps.Simulate(-1.0f);
    ENG_CHECK(AllFinite(p, 300));
}

ENG_TEST(Particles, CapacityBudgetIsCapped) {
    std::vector<ParticleEmitterDesc> descs(2);
    descs[0].name = "A";
    descs[0].maxParticles = 150000;
    descs[1].name = "B";
    descs[1].maxParticles = 150000;
    ParticleSystem ps;
    ps.Init(descs, 5u);
    ENG_CHECK(ps.Capacity() > 0);
    ENG_CHECK(ps.Capacity() <= 200000);
    ENG_CHECK(ps.EmitterCount() == 2);
}

// ---------------------------------------------------------------------------
// Сэмплирование форм
// ---------------------------------------------------------------------------
ENG_TEST(Particles, ShapeSampling) {
    // Сфера: каждое появление лежит внутри shapeRadius.max.
    {
        ParticleEmitterDesc d = MakeEmitter(256);
        d.rate = 0.0f;
        d.burst = 200;
        d.lifetime = {5.0f, 5.0f};
        d.shape = EmitterShape::Sphere;
        d.shapeRadius = {0.3f, 0.8f};

        ParticleSystem ps;
        ps.Init({d}, 5u);
        ps.Simulate(1.0f / 60.0f);
        const Particle* p = ps.Particles(0);
        ENG_CHECK_EQ(CountAlive(p, 256), 200);
        for (int i = 0; i < 256; ++i) {
            if (!p[i].alive) continue;
            // Объёмное сэмплирование: каждое появление лежит внутри сэмплированного радиуса.
            ENG_CHECK(Length(p[i].position) <= 0.8f + 1e-3f);
        }
    }
    // Бокс: внутри бокса (полуразмеры).
    {
        ParticleEmitterDesc d = MakeEmitter(256);
        d.rate = 0.0f;
        d.burst = 200;
        d.lifetime = {5.0f, 5.0f};
        d.shape = EmitterShape::Box;
        d.shapeBox = {2.0f, 4.0f, 6.0f};

        ParticleSystem ps;
        ps.Init({d}, 6u);
        ps.Simulate(1.0f / 60.0f);
        const Particle* p = ps.Particles(0);
        ENG_CHECK_EQ(CountAlive(p, 256), 200);
        for (int i = 0; i < 256; ++i) {
            if (!p[i].alive) continue;
            ENG_CHECK(std::fabs(p[i].position.x) <= 1.0f + 1e-3f);
            ENG_CHECK(std::fabs(p[i].position.y) <= 2.0f + 1e-3f);
            ENG_CHECK(std::fabs(p[i].position.z) <= 3.0f + 1e-3f);
        }
    }
    // Конус (вокруг +Y): позиция остаётся внутри половинного угла и радиуса.
    {
        ParticleEmitterDesc d = MakeEmitter(256);
        d.rate = 0.0f;
        d.burst = 200;
        d.lifetime = {5.0f, 5.0f};
        d.shape = EmitterShape::Cone;
        d.coneAngle = 20.0f * kDeg2Rad;
        d.shapeRadius = {0.4f, 0.4f};

        ParticleSystem ps;
        ps.Init({d}, 8u);
        ps.Simulate(1.0f / 60.0f);
        const Particle* p = ps.Particles(0);
        ENG_CHECK_EQ(CountAlive(p, 256), 200);
        for (int i = 0; i < 256; ++i) {
            if (!p[i].alive) continue;
            const Vec3 v = p[i].position;
            const f32 len = Length(v);
            ENG_CHECK(len <= 0.4f + 1e-3f);
            ENG_CHECK(len > 0.0f);
            const f32 angle = std::acos(Clamp(v.y / MaxT(len, 1e-6f), -1.0f, 1.0f));
            ENG_CHECK(angle <= 20.0f * kDeg2Rad + 1e-3f);
        }
    }
}

// ---------------------------------------------------------------------------
// Суб-эмиттеры, явные всплески (burst), очистка
// ---------------------------------------------------------------------------
ENG_TEST(Particles, ExplosionDeathSubEmitterFeedsSmoke) {
    const std::vector<ParticleEmitterDesc> descs = ParticleSystem::PresetExplosion();
    ENG_CHECK(descs.size() >= 2);
    if (descs.size() < 2) return;

    const std::string smokeName = descs[1].name;
    ENG_CHECK(descs[0].subEmitters.size() == 1);
    ENG_CHECK_STR_EQ(descs[0].subEmitters[0].emitterName, smokeName);

    ParticleSystem ps;
    ps.Init(descs, 2024u);

    int smokeIdx = -1;
    for (int i = 0; i < ps.EmitterCount(); ++i) {
        if (ps.Emitter(i).name == smokeName) smokeIdx = i;
    }
    ENG_CHECK(smokeIdx >= 0);
    if (smokeIdx < 0) return;
    ENG_CHECK(ps.Particles(smokeIdx) != nullptr);

    for (int i = 0; i < 20; ++i) ps.Simulate(0.1f);  // 2 с: burst давно закончился

    const int smokeCapacity = ps.Emitter(smokeIdx).maxParticles;
    const int smokeAlive = CountAlive(ps.Particles(smokeIdx), smokeCapacity);
    ENG_CHECK_MSG(smokeAlive > 0, "death sub-emitter produced no smoke particles");

    // У эмиттера дыма rate 0, поэтому каждая частица дыма пришла из цепочки
    // суб-эмиттеров.
    ENG_CHECK_NEAR(ps.Emitter(smokeIdx).rate, 0.0f, 1e-6f);
}

ENG_TEST(Particles, EmitBurstAndClear) {
    ParticleEmitterDesc d = MakeEmitter(100);
    d.rate = 0.0f;
    d.burst = 0;

    ParticleSystem ps;
    ps.Init({d}, 1u);
    ENG_CHECK_EQ(ps.AliveCount(), 0);
    ENG_CHECK_EQ(ps.Capacity(), 100);

    ps.EmitBurst(0, 37);
    ENG_CHECK_EQ(ps.AliveCount(), 37);
    ps.EmitBurst(0, 10);
    ENG_CHECK_EQ(ps.AliveCount(), 47);
    // Переполнение пула ограничивается, но не превышается.
    ps.EmitBurst(0, 1000);
    ENG_CHECK_EQ(ps.AliveCount(), 100);

    ps.Clear();
    ENG_CHECK_EQ(ps.AliveCount(), 0);
    ENG_CHECK_EQ(ps.Capacity(), 100);
    const Particle* p = ps.Particles(0);
    ENG_CHECK(p != nullptr);
    if (p != nullptr) ENG_CHECK_EQ(CountAlive(p, 100), 0);

    // Некорректные индексы игнорируются.
    ps.EmitBurst(-1, 5);
    ps.EmitBurst(7, 5);
    ENG_CHECK_EQ(ps.AliveCount(), 0);
    ENG_CHECK(ps.Particles(3) == nullptr);
}

ENG_TEST(Particles, PresetsAreDistinctAndUsable) {
    struct Preset {
        const char* name;
        std::vector<ParticleEmitterDesc> (*fn)();
    };
    const Preset presets[] = {
        {"Fire", &ParticleSystem::PresetFire},       {"Smoke", &ParticleSystem::PresetSmoke},
        {"Sparks", &ParticleSystem::PresetSparks},   {"Magic", &ParticleSystem::PresetMagic},
        {"Fountain", &ParticleSystem::PresetFountain},
        {"Explosion", &ParticleSystem::PresetExplosion},
        {"Snow", &ParticleSystem::PresetSnow},       {"Trail", &ParticleSystem::PresetTrail},
    };

    int blendModes = 0;
    int renderModes = 0;
    for (const Preset& preset : presets) {
        const std::vector<ParticleEmitterDesc> descs = preset.fn();
        ENG_CHECK(!descs.empty());
        ParticleSystem ps;
        ps.Init(descs, 1234u);
        ENG_CHECK(ps.Capacity() > 0);
        for (int i = 0; i < 30; ++i) ps.Simulate(1.0f / 30.0f);
        ENG_CHECK_MSG(ps.AliveCount() > 0, preset.name);
        ENG_CHECK(AllFinite(ps.Particles(0), ps.Emitter(0).maxParticles));
        blendModes |= 1 << static_cast<int>(descs[0].blendMode);
        renderModes |= 1 << static_cast<int>(descs[0].renderMode);
    }
    // Пресеты намеренно задействуют разные пути смешивания / рендера.
    ENG_CHECK(blendModes != 0);
    ENG_CHECK(renderModes != 0);
    ENG_CHECK(renderModes != (1 << static_cast<int>(ParticleRenderMode::Billboard)));
}

ENG_TEST(Particles, ShutdownAndReinitAreSafe) {
    ParticleSystem ps;
    ps.Init(ParticleSystem::PresetFire(), 1u);
    ps.Simulate(0.2f);
    ENG_CHECK(ps.AliveCount() > 0);

    ps.Shutdown();
    ENG_CHECK_EQ(ps.AliveCount(), 0);
    ENG_CHECK_EQ(ps.Capacity(), 0);
    ENG_CHECK_EQ(ps.EmitterCount(), 0);
    ENG_CHECK(ps.Particles(0) == nullptr);
    ENG_CHECK(!ps.IsPlaying());
    // Все точки входа должны оставаться безопасными после Shutdown.
    ps.Simulate(0.1f);
    ps.EmitBurst(0, 10);
    ps.Clear();
    ps.Restart();
    ENG_CHECK_EQ(ps.AliveCount(), 0);

    ps.Init(ParticleSystem::PresetSmoke(), 2u);
    ps.Simulate(0.3f);
    ENG_CHECK(ps.AliveCount() > 0);
    ENG_CHECK(ps.Capacity() > 0);
}

ENG_TEST(Particles, SwitchingRenderModeAfterInitIsSafe) {
    ParticleEmitterDesc d = MakeEmitter(64);
    d.rate = 60.0f;
    d.shape = EmitterShape::Sphere;
    d.shapeRadius = {0.1f, 0.4f};
    d.startSpeed = {1.0f, 2.0f};

    ParticleSystem ps;
    ps.Init({d}, 3u);
    for (int i = 0; i < 5; ++i) ps.Simulate(1.0f / 60.0f);

    ps.Emitter(0).renderMode = ParticleRenderMode::Trail;
    for (int i = 0; i < 10; ++i) ps.Simulate(1.0f / 60.0f);
    ENG_CHECK(ps.AliveCount() > 0);

    ps.Emitter(0).renderMode = ParticleRenderMode::Billboard;
    for (int i = 0; i < 10; ++i) ps.Simulate(1.0f / 60.0f);
    ENG_CHECK(AllFinite(ps.Particles(0), 64));
}

// ---------------------------------------------------------------------------
// GL smoke-тест (пропускается без контекста)
// ---------------------------------------------------------------------------
ENG_TEST(Particles, RenderSmokeTestWithGL) {
    ENG_REQUIRE_GL();

    Renderer3D renderer;
    if (!renderer.Init()) ENG_SKIP("Renderer3D::Init failed");

    Camera camera;
    camera.position = {0.0f, 1.5f, 5.0f};
    camera.target = {0.0f, 0.5f, 0.0f};
    renderer.SetCamera(camera);
    renderer.BeginFrame(camera, 64, 64);

    ParticleSystem ps;
    ps.Init(ParticleSystem::PresetFire(), 99u);
    ps.Simulate(0.15f);
    ENG_CHECK(ps.AliveCount() > 0);
    ps.Render(renderer, camera);  // не должно падать с шейдером или без

    renderer.EndFrame();
    renderer.Shutdown();
}
