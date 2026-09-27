//
// CrossRender
// Автор: xcodestream <xcodestream@gmail.com>
// Лицензия: MIT
// Дата: 15.05.2026
// Описание: импорт 3D-моделей (OBJ/MTL, glTF 2.0, PLY, STL) и контейнер геометрии с материалами.
//
#pragma once

#include "crossrender/gfx/Mesh.h"
#include "crossrender/core/Base.h"
#include "crossrender/core/Math.h"

#include <memory>
#include <string>
#include <vector>

namespace crossrender {

enum class ModelFormat : u8 { Unknown, Obj, Gltf, Glb, Ply, Stl, Vox, FbxLite };

struct ModelNode {
    std::string name;
    int parent = -1;
    std::vector<int> children;
    Vec3 translation{0, 0, 0};
    Vec3 rotationEuler{0, 0, 0};
    Quat rotation{0, 0, 0, 1};
    Vec3 scale{1, 1, 1};
    Mat4 localMatrix = Mat4::Identity();
    int mesh = -1;        // индекс в Model::meshes
    int material = -1;    // индекс в Model::materials
    bool hasMatrix = false;
};

// Загруженная модель: меши, материалы и иерархия узлов.
class Model {
public:
    Model() = default;
    ~Model() = default;
    Model(Model&&) noexcept = default;
    Model& operator=(Model&&) noexcept = default;
    Model(const Model&) = delete;
    Model& operator=(const Model&) = delete;

    // Автоматически определяет формат по расширению / магическим байтам.
    bool Load(const std::string& path);
    bool LoadFromMemory(const void* data, usize size, ModelFormat format, const std::string& name = "");
    void Destroy();

    // ---- отдельные импортёры (публичные, чтобы тесты вызывали их напрямую) ----
    static bool ImportObj(const char* text, usize size, MeshData* outMesh,
                          std::vector<Material>* outMaterials, std::string* error);
    static bool ImportGltf(const void* data, usize size, bool binary, std::vector<MeshData>* outMeshes,
                           std::vector<Material>* outMaterials, std::vector<ModelNode>* outNodes,
                           std::vector<Texture>* outTextures, std::string* error);
    static bool ImportPly(const void* data, usize size, MeshData* outMesh, std::string* error);
    static bool ImportStl(const void* data, usize size, MeshData* outMesh, std::string* error);

    [[nodiscard]] bool Valid() const { return !meshes_.empty(); }
    [[nodiscard]] ModelFormat Format() const { return format_; }
    [[nodiscard]] const std::string& Name() const { return name_; }
    [[nodiscard]] int MeshCount() const { return static_cast<int>(meshes_.size()); }
    [[nodiscard]] int MaterialCount() const { return static_cast<int>(materials_.size()); }
    [[nodiscard]] int NodeCount() const { return static_cast<int>(nodes_.size()); }
    [[nodiscard]] const MeshData& MeshAt(int i) const { return meshes_[static_cast<usize>(i)]; }
    [[nodiscard]] const Material& MaterialAt(int i) const { return materials_[static_cast<usize>(i)]; }
    [[nodiscard]] const ModelNode& NodeAt(int i) const { return nodes_[static_cast<usize>(i)]; }
    [[nodiscard]] std::vector<ModelNode>& Nodes() { return nodes_; }
    [[nodiscard]] std::vector<Material>& Materials() { return materials_; }
    [[nodiscard]] std::vector<MeshData>& Meshes() { return meshes_; }
    [[nodiscard]] std::vector<Texture>& Textures() { return textures_; }
    [[nodiscard]] const Bounds& Bounds() const { return bounds_; }

    // Загружает все меши на GPU (идемпотентно).
    void UploadToGpu();
    [[nodiscard]] bool Uploaded() const { return uploaded_; }
    [[nodiscard]] const Mesh& GpuMesh(int i) const { return gpuMeshes_[static_cast<usize>(i)]; }

    // Применяет общую трансформацию модели к каждой вершине (запекает масштаб/up-axis).
    void BakeTransform(const Mat4& m);
    // Пересчитывает сглаженные нормали для каждого меша, где их нет.
    void EnsureNormals();

    // Процедурно генерирует встроенные low-poly модели.
    static Model MakeLowPolyTree(u64 seed = 1);
    static Model MakeLowPolyRock(u64 seed = 2);
    static Model MakeLowPolyCrystal(u64 seed = 3);
    static Model MakeLowPolyCharacter();

private:
    ModelFormat format_ = ModelFormat::Unknown;
    std::string name_;
    std::vector<MeshData> meshes_;
    std::vector<Material> materials_;
    std::vector<ModelNode> nodes_;
    std::vector<Texture> textures_;
    std::vector<Mesh> gpuMeshes_;
    struct Bounds bounds_;
    bool uploaded_ = false;
};

}  // namespace crossrender
