// Пример crossrender: три цветных круга на орбите вокруг центра экрана, каждый
// пульсирует с мягким ореолом, плюс стилизованный заголовок и подпись. Escape выходит.
#include <cmath>
#include <cstdio>
#include <algorithm>

#include "crossrender/Engine.h"

namespace {

using namespace crossrender;

class CirclesScene : public Scene {
public:
    [[nodiscard]] const char* Name() const override { return "circles"; }
    [[nodiscard]] const char* Description() const override {
        return "Three coloured circles orbiting and pulsing";
    }
    [[nodiscard]] Color ClearColor() const override { return Color::FromARGB(0xFF2E3136); }

    void OnEnter(SceneContext& ctx) override {
        // Ubuntu покрывает кириллицу; иначе откатываемся на процедурный шрифт.
        font_ = FontManager::Get().Load("fonts/ubuntu.ttf");
        if (!font_) font_ = ctx.engine->DefaultFont();
    }

    void Update(SceneContext& ctx, f32 dt) override {
        time_ += dt;
        if (ctx.engine->GetInput().KeyPressed(Key::Escape)) ctx.engine->Quit();
    }

    void Render2D(SceneContext& ctx) override {
        Renderer2D& r = *ctx.r2d;
        const Rect vp = ctx.viewport;
        const f32 cx = vp.x + vp.w * 0.5f;
        const f32 cy = vp.y + vp.h * 0.5f;
        const f32 orbit = std::min(vp.w, vp.h) * 0.25f;

        struct Orb {
            Color color;
            f32 speed;    // радиан в секунду
            f32 phase;    // начальный угол
            f32 radius;   // радиус круга
        };
        constexpr Orb kOrbs[3] = {
            {Color{0.94f, 0.35f, 0.30f, 1.0f}, 1.10f, 0.0f, 64.0f},   // красный
            {Color{0.30f, 0.84f, 0.45f, 1.0f}, -0.80f, 2.09f, 52.0f}, // зелёный
            {Color{0.35f, 0.55f, 0.95f, 1.0f}, 0.60f, 4.19f, 44.0f},  // синий
        };

        for (const Orb& orb : kOrbs) {
            const f32 a = time_ * orb.speed + orb.phase;
            const f32 pulse = 1.0f + 0.18f * std::sin(time_ * 2.4f + orb.phase);
            const f32 px = cx + std::cos(a) * orbit;
            const f32 py = cy + std::sin(a) * orbit;
            const f32 pr = orb.radius * pulse;

            // Мягкий ореол позади круга.
            r.FillCircle(px, py, pr * 1.7f, Color{orb.color.r, orb.color.g, orb.color.b, 0.15f});
            r.FillCircle(px, py, pr, orb.color);
        }

        // Едва заметный маркер центра, чтобы орбита читалась.
        r.FillCircle(cx, cy, 6.0f, Color{1.0f, 1.0f, 1.0f, 0.25f});

        // Заголовок и подпись.
        if (font_) {
            const char* title = "crossrender";
            const f32 titleSize = 96.0f;
            TextMetrics m = MeasureText(*font_, title, titleSize);

            TextStyle titleStyle;
            titleStyle.gradient = true;
            titleStyle.innerColor = Color{1.0f, 0.85f, 0.45f, 1.0f};
            titleStyle.outerColor = Color{0.95f, 0.45f, 0.35f, 1.0f};
            titleStyle.shadowColor = Color{0, 0, 0, 0.55f};
            titleStyle.shadowOffset = {3.0f, 4.0f};
            r.DrawTextStyled(*font_, title, Vec2{cx - m.width * 0.5f, vp.y + 40.0f}, titleSize,
                             titleStyle);

            char caption[96];
            std::snprintf(caption, sizeof(caption), "3 круга · %.1f с · Esc — выход", time_);
            TextStyle captionStyle;
            captionStyle.color = Color{0.85f, 0.87f, 0.9f, 0.75f};
            captionStyle.shadowColor = Color{0, 0, 0, 0.5f};
            captionStyle.shadowOffset = {1.0f, 2.0f};
            TextMetrics cm = MeasureText(*font_, caption, 22.0f);
            r.DrawTextStyled(*font_, caption, Vec2{cx - cm.width * 0.5f, vp.y + vp.h - 46.0f}, 22.0f,
                             captionStyle);
        }
    }

private:
    f32 time_ = 0;
    Font* font_ = nullptr;
};

}  // namespace

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    EngineConfig cfg;
    cfg.window.title = "CrossRender - Circles Example";
    cfg.window.width = 1280;
    cfg.window.height = 800;
    cfg.window.vsync = true;
    cfg.window.msaaSamples = 4;
    cfg.enable3D = false;
    cfg.enableAudio = false;
    cfg.enableUI = false;
    cfg.startScene = "circles";

    return RunExample(cfg, [](Engine& engine) {
        engine.Scenes().Register("circles", [] { return std::make_unique<CirclesScene>(); });
    });
}
