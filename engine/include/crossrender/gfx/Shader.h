//
// CrossRender
// Автор: xcodestream <xcodestream@gmail.com>
// Лицензия: MIT
// Дата: 15.05.2026
// Описание: шейдерные программы OpenGL: компиляция, униформы и набор встроенных программ.
//
#pragma once

#include "crossrender/core/Base.h"
#include "crossrender/core/Math.h"

#include <string>
#include <vector>
#include <unordered_map>

namespace crossrender {

class Texture;

// Значение униформы для универсального сеттера на основе рефлексии.
struct UniformValue {
    enum class Type { Int, Float, Vec2, Vec3, Vec4, Mat3, Mat4, Texture } type = Type::Float;
    i32 i = 0;
    f32 f = 0;
    Vec2 v2;
    Vec3 v3;
    Vec4 v4;
    Mat4 m4;
    f32 m3[9]{};
    const Texture* tex = nullptr;
    i32 slot = 0;
};

class Shader {
public:
    Shader() = default;
    ~Shader();
    Shader(Shader&& o) noexcept;
    Shader& operator=(Shader&& o) noexcept;
    Shader(const Shader&) = delete;
    Shader& operator=(const Shader&) = delete;

    // Компилирует и линкует. `name` используется только в диагностике.
    bool Build(const char* vertexSrc, const char* fragmentSrc, const std::string& name = "shader");
    bool BuildFile(const std::string& vertPath, const std::string& fragPath);
    // Для удобства: загружает "<dir>/<base>.vert" + "<base>.frag".
    bool Load(const std::string& basePath);
    void Destroy();
    void Bind() const;
    static void Unbind();

    void Set(const char* name, i32 v);
    void Set(const char* name, f32 v);
    void Set(const char* name, const Vec2& v);
    void Set(const char* name, const Vec3& v);
    void Set(const char* name, const Vec4& v);
    void Set(const char* name, const Color& v);
    void Set(const char* name, const Mat4& v);
    void Set(const char* name, const float* m3);
    void Set(const char* name, const std::vector<Vec3>& v);
    void Set(const char* name, const std::vector<Vec4>& v);
    void Set(const char* name, const std::vector<Mat4>& v);
    // Привязывает `tex` к следующему свободному слоту и задаёт униформу сэмплера.
    void SetTexture(const char* name, const Texture& tex, int slot = 0);
    void SetIntArray(const char* name, const i32* values, int count);

    // Препроцессорные define, применяемые при компиляции (вызывать до Build).
    void AddDefine(const std::string& define) { defines_.push_back(define); }
    void ClearDefines() { defines_.clear(); }

    [[nodiscard]] bool Valid() const { return program_ != 0; }
    [[nodiscard]] unsigned int Id() const { return program_; }
    [[nodiscard]] const std::string& Name() const { return name_; }
    [[nodiscard]] const std::string& Log() const { return log_; }
    [[nodiscard]] int UniformLocation(const char* name);

private:
    unsigned int program_ = 0;
    std::string name_;
    std::string log_;
    std::vector<std::string> defines_;
    std::unordered_map<std::string, int> uniforms_;
};

// Встроенные исходники шейдеров, общие для нескольких рендереров.
namespace builtin {

// Полноэкранный блит одним треугольником с цветовым тонированием.
extern const char* kBlitVert;
extern const char* kBlitFrag;
// Неосвещённая/окрашенная по вершинам геометрия (2D-батч и отладочные линии).
extern const char* kSpriteVert;
extern const char* kSpriteFrag;
// 3D forward-шейдер PBR-lite: до 8 источников света + тени.
extern const char* kForwardVert;
extern const char* kForwardFrag;
// Проход только глубины для shadow map.
extern const char* kShadowVert;
extern const char* kShadowFrag;
// Шейдер воксельных чанков: raymarch / greedy-mesh.
extern const char* kVoxelVert;
extern const char* kVoxelFrag;
// Шейдер билбордов частиц (инстансинг).
extern const char* kParticleVert;
extern const char* kParticleFrag;
// Шейдер SDF-текста.
extern const char* kSdfTextVert;
extern const char* kSdfTextFrag;
// FXAA / композитинг.
extern const char* kPostVert;
extern const char* kFxaaFrag;
extern const char* kBloomDownFrag;
extern const char* kBloomUpFrag;
extern const char* kTonemapFrag;
// Градиент неба.
extern const char* kSkyVert;
extern const char* kSkyFrag;

// Преамбула версии GLSL для текущей платформы (добавляет precision-квалификаторы на GLES).
const char* Preamble();

}  // namespace builtin
}  // namespace crossrender
