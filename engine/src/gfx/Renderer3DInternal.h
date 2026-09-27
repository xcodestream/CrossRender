//
// CrossRender
// Автор: xcodestream <xcodestream@gmail.com>
// Лицензия: MIT
// Дата: 15.05.2026
// Описание: внутренние вычисления Renderer3D: фрустум, разбиение каскадов теней, лучи.
//
#pragma once

#include "crossrender/gfx/Mesh.h"
#include "crossrender/core/Base.h"
#include "crossrender/core/Math.h"

namespace crossrender {
namespace r3d_internal {

// Прямой шейдер объявляет `#define MAX_LIGHTS 8` и массив каскадов из 3
// элементов; держите эти константы синхронно с `builtin::kForwardFrag`.
constexpr int kMaxLights = 8;
constexpr int kMaxCascades = 3;

// ---------------------------------------------------------------------------
// Отсечение по фрустуму
// ---------------------------------------------------------------------------
// Плоскость в виде `dot(normal, p) + d >= 0` для точек внутри фрустума.
struct Plane {
    Vec3 normal{0, 1, 0};
    f32 d = 0.0f;
};

// Извлекает 6 clip-плоскостей (левая, правая, нижняя, верхняя, ближняя,
// дальняя) из column-major view-projection матрицы методом Грибба/Хартманна
// (NDC z в [-1, 1], как в Mat4::Perspective/Ortho).
void ExtractFrustumPlanes(const Mat4& viewProj, Plane out[6]);

// Консервативный тест AABB/фрустум: false только когда бокс целиком
// снаружи хотя бы одной плоскости.
bool AabbInFrustum(const Plane planes[6], const Vec3& lo, const Vec3& hi);
bool AabbInFrustum(const Mat4& viewProj, const Vec3& lo, const Vec3& hi);

// Заключает преобразованный матрицей `m` axis-aligned бокс в мировую AABB
// (преобразует 8 углов; устойчиво для любой аффинной матрицы).
void TransformAabb(const Mat4& m, const Vec3& lo, const Vec3& hi, Vec3* outLo, Vec3* outHi);

// ---------------------------------------------------------------------------
// Каскады теней
// ---------------------------------------------------------------------------
// Практичная схема разбиения (Zhang et al.): смешивает логарифмическое и
// равномерное распределения с весом `lambda` (0 = равномерное, 1 = логарифмическое).
//
// Записывает ровно `count` дистанций в `out`:
//   * строго возрастающие, out[0] > nearZ
//   * out[count - 1] == farZ  (вызывающий передаёт эффективную дистанцию теней)
void ComputeCascadeSplits(f32 nearZ, f32 farZ, f32 lambda, int count, f32* out);

// ---------------------------------------------------------------------------
// Математика пикинга
// ---------------------------------------------------------------------------
// Slab-тест с локальной AABB. При промахе возвращает false; иначе `tMin` —
// дистанция входа (или дистанция выхода, когда начало луча внутри бокса;
// в этом случае возвращаемая дистанция >= 0).
bool RayAabbTest(const Vec3& origin, const Vec3& dir, const Vec3& lo, const Vec3& hi, f32* tMin);

// Мёллер–Трумбор, двусторонний (без отбраковки задних граней).
bool RayTriangle(const Vec3& origin, const Vec3& dir, const Vec3& v0, const Vec3& v1, const Vec3& v2,
                 f32* tOut, f32* uOut, f32* vOut);

// Ближайшее попадание в треугольник MeshData. Заполняет барицентрически
// интерполированные нормаль и UV. `triOut` получает индекс треугольника.
// Возвращает false, если в пределах `maxDistance` попаданий нет.
bool RayMeshTriangles(const Vec3& origin, const Vec3& dir, const MeshData& data, f32 maxDistance,
                      f32* tOut, int* triOut, Vec3* normalOut, Vec2* uvOut);

// ---------------------------------------------------------------------------
// Привязка мешей на CPU (используется для пикинга с точностью до треугольника)
// ---------------------------------------------------------------------------
// Публичный класс `Mesh` владеет только GL-хэндлами и габаритным боксом;
// данных вершин он не отдаёт. Кому нужен точный пикинг, регистрирует здесь
// исходный MeshData для меша (MeshData должен жить дольше привязки). Если
// данные не зарегистрированы, Renderer3D::Raycast откатывается к пересечению
// луча с AABB и `RayHit::triangleIndex == -1`.
void RegisterMeshData(const Mesh* mesh, const MeshData* data);
void UnregisterMeshData(const Mesh* mesh);
const MeshData* FindMeshData(const Mesh* mesh);

// ---------------------------------------------------------------------------
// Упаковка источников света
// ---------------------------------------------------------------------------
// Готовые к загрузке uniform-массивы (см. `builtin::kForwardFrag`).
struct PackedLights {
    int count = 0;
    i32 type[kMaxLights]{};
    Vec3 position[kMaxLights]{};
    Vec3 direction[kMaxLights]{};
    Vec3 color[kMaxLights]{};
    f32 intensity[kMaxLights]{};
    f32 range[kMaxLights]{};
    Vec2 cone[kMaxLights]{};  // (cos(inner), cos(outer))
    Vec2 area[kMaxLights]{};
};

// Преобразует половинные углы конуса прожектора (радианы) в пару косинусов,
// которую ждёт шейдер: x = cos(inner), y = cos(outer). Всегда x >= y.
Vec2 SpotConeCosines(f32 innerRad, f32 outerRad);

// Упаковывает первые `min(lightCount, maxLights)` источников. Возвращает число
// упакованных источников — то, чему нужно выставить `uLightCount`.
int PackLights(const Light* lights, int lightCount, int maxLights, PackedLights* out);

// Сопоставляет тип источника света целому, которое использует прямой шейдер.
i32 LightTypeToUniform(LightType type);

}  // namespace r3d_internal
}  // namespace crossrender
