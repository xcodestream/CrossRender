//
// CrossRender
// Автор: xcodestream <xcodestream@gmail.com>
// Лицензия: MIT
// Дата: 15.05.2026
// Описание: воксельный мир: чанки 32x32x32, палитра блоков и генерация мешей.
//
#pragma once

#include "crossrender/gfx/Mesh.h"
#include "crossrender/core/Base.h"
#include "crossrender/core/Math.h"
#include "crossrender/gfx/Texture.h"

#include <memory>
#include <vector>
#include <unordered_map>

namespace crossrender {

class Renderer3D;

constexpr int kVoxelChunkSize = 32;
constexpr int kVoxelChunkVolume = kVoxelChunkSize * kVoxelChunkSize * kVoxelChunkSize;

// Один воксель: индекс палитры 0 означает пустоту.
struct Voxel {
    u8 id = 0;         // индекс палитры 1..255
    u8 light = 0;      // запечённый уровень света 0..15 (небо + блоки)
    u8 ao = 255;       // перекрытие окружения (ambient occlusion) 0..255
    u8 flags = 0;
};

struct VoxelPalette {
    std::vector<Color> colors;
    VoxelPalette();
    [[nodiscard]] Color Get(u8 id) const {
        return id < colors.size() ? colors[id] : Color::Magenta;
    }
    void Set(u8 id, const Color& c) {
        if (id >= colors.size()) colors.resize(id + 1, Color::White);
        colors[id] = c;
    }
    // Палитра по умолчанию: камень, трава, земля, песок, дерево, листья, вода, стекло, кирпич, снег...
    static VoxelPalette Default();
    static VoxelPalette Ice();
};

struct ChunkCoord {
    int x = 0, y = 0, z = 0;
    bool operator==(const ChunkCoord& o) const { return x == o.x && y == o.y && z == o.z; }
};

struct ChunkCoordHash {
    usize operator()(const ChunkCoord& c) const {
        usize h = static_cast<usize>(c.x) * 73856093u;
        h ^= static_cast<usize>(c.y) * 19349663u;
        h ^= static_cast<usize>(c.z) * 83492791u;
        return h;
    }
};

// ---------------------------------------------------------------------------
// Chunk: плотное хранение вокселей + сгенерированный меш (кэшируется до изменения)
// ---------------------------------------------------------------------------
class VoxelChunk {
public:
    VoxelChunk() { voxels_.resize(kVoxelChunkVolume); }

    void Set(int x, int y, int z, u8 id, u8 light = 15);
    [[nodiscard]] u8 Get(int x, int y, int z) const;
    [[nodiscard]] u8 GetLight(int x, int y, int z) const;
    [[nodiscard]] bool IsEmpty(int x, int y, int z) const { return Get(x, y, z) == 0; }
    [[nodiscard]] bool IsSolid(int x, int y, int z) const { return Get(x, y, z) != 0; }
    void Fill(u8 id);
    void Clear();

    [[nodiscard]] ChunkCoord Coord() const { return coord_; }
    void SetCoord(const ChunkCoord& c) { coord_ = c; }
    [[nodiscard]] Vec3 WorldOrigin() const {
        return {static_cast<f32>(coord_.x * kVoxelChunkSize), static_cast<f32>(coord_.y * kVoxelChunkSize),
                static_cast<f32>(coord_.z * kVoxelChunkSize)};
    }
    [[nodiscard]] bool Dirty() const { return dirty_; }
    void MarkDirty() { dirty_ = true; }
    [[nodiscard]] bool Empty() const { return nonEmptyCount_ == 0; }
    [[nodiscard]] int SolidCount() const { return nonEmptyCount_; }

    // Перестраивает меш для рендера. `world` нужен для выборки соседних чанков,
    // чтобы грани на границах чанков корректно отсекались.
    class VoxelWorld* world = nullptr;
    void RebuildMesh(const VoxelPalette& palette, bool greedy, bool textured, bool ao,
                     const class VoxelWorld* owner);

    [[nodiscard]] const MeshData& Mesh() const { return meshData_; }
    [[nodiscard]] const crossrender::Mesh& GpuMesh() const { return gpuMesh_; }
    [[nodiscard]] bool MeshValid() const { return meshValid_; }
    void UploadMesh();

private:
    std::vector<u8> voxels_;
    std::vector<u8> lights_;
    ChunkCoord coord_;
    bool dirty_ = true, meshValid_ = false;
    int nonEmptyCount_ = 0;
    MeshData meshData_;
    crossrender::Mesh gpuMesh_;
};

// ---------------------------------------------------------------------------
// World: разреженная карта чанков + утилиты генерации
// ---------------------------------------------------------------------------
struct VoxelMeshingOptions {
    bool greedy = true;        // объединяет копланарные грани
    bool textured = true;      // выдаёт UV для использования текстурного атласа
    bool ambientOcclusion = true;
    bool bakeLight = true;
    bool cullInterior = true;
};

struct VoxelRayHit {
    bool hit = false;
    int x = 0, y = 0, z = 0;        // поражённый воксель
    int nx = 0, ny = 0, nz = 0;     // нормаль грани
    f32 distance = 0;
    u8 id = 0;
};

class VoxelWorld {
public:
    VoxelWorld();

    VoxelChunk* GetChunk(const ChunkCoord& c);
    VoxelChunk* GetOrCreateChunk(const ChunkCoord& c);
    void RemoveChunk(const ChunkCoord& c);
    void Clear();
    [[nodiscard]] int ChunkCount() const { return static_cast<int>(chunks_.size()); }
    [[nodiscard]] std::vector<VoxelChunk*> AllChunks();

    // Доступ к вокселям в мировых координатах (создаёт чанки при записи, если `create`).
    void SetVoxel(int x, int y, int z, u8 id, bool createChunk = true);
    [[nodiscard]] u8 GetVoxel(int x, int y, int z) const;
    [[nodiscard]] bool IsSolid(int x, int y, int z) const;

    [[nodiscard]] VoxelPalette& Palette() { return palette_; }
    void SetPalette(const VoxelPalette& p) { palette_ = p; }
    [[nodiscard]] VoxelMeshingOptions& Meshing() { return meshing_; }
    void SetMeshing(const VoxelMeshingOptions& m) { meshing_ = m; }

    // Перестраивает меши всех изменённых чанков и загружает их на GPU.
    void UpdateMeshes();
    void MarkAllDirty();
    // Рисует каждый чанк (с текстурированием атласом палитры, когда `textured`).
    void Render(Renderer3D& r, const Material& mat);
    [[nodiscard]] const Bounds& Bounds() const { return bounds_; }
    void RecomputeBounds();

    // ---- генерация -----------------------------------------------------
    // Детерминированный рельеф: карта высот fBm + пещеры + деревья + жилы руды.
    void GenerateTerrain(int chunksX, int chunksZ, u64 seed = 1337, int baseHeight = 12,
                         int amplitude = 10);
    void GenerateFlat(int chunksX, int chunksZ, int height = 4, u8 top = 2, u8 fill = 3);
    void GenerateSphere(const Vec3& center, f32 radius, u8 id);
    void GenerateBox(const Vec3& lo, const Vec3& hi, u8 id);
    // Структурированный, богатый деталями демо-остров (используется воксельной сценой).
    void GenerateIsland(int radiusChunks, u64 seed = 7);

    // ---- выборка -------------------------------------------------------
    [[nodiscard]] VoxelRayHit Raycast(const Vec3& origin, const Vec3& dir, f32 maxDistance = 100.0f) const;
    // Заливает свет от неба вниз; вызывайте после генерации.
    void ComputeLighting(int sunHeight = 64);
    // Применяет простое перекрытие окружения к мешам чанков (используется при построении мешей).
    [[nodiscard]] f32 SampleAO(int x, int y, int z, int nx, int ny, int nz) const;

    // Строит текстурный атлас из палитры (каждый id = одна ячейка).
    void BuildPaletteAtlas(int cellSize = 16);
    [[nodiscard]] const Texture& PaletteAtlas() const { return paletteAtlas_; }
    // UV-прямоугольник элемента палитры `id` в атласе.
    [[nodiscard]] Rect PaletteUV(u8 id) const;

    // ---- режим реймарча по 3D-текстуре (без построения мешей) --------------------------
    // Загружает область как 3D-текстуру для реймарча во фрагментном шейдере.
    bool BuildVolumeTexture(const Vec3& min, const Vec3& size, u8 fillOutside = 0);
    [[nodiscard]] const Texture& VolumeTexture() const { return volumeTexture_; }
    void RenderRaymarched(Renderer3D& r, const Vec3& boxMin, const Vec3& boxSize, const Camera& cam);

    const VoxelMeshingOptions& Options() const { return meshing_; }

private:
    std::unordered_map<ChunkCoord, std::unique_ptr<VoxelChunk>, ChunkCoordHash> chunks_;
    VoxelPalette palette_;
    VoxelMeshingOptions meshing_;
    struct Bounds bounds_;
    Texture paletteAtlas_;
    Texture volumeTexture_;
    Vec3 volumeMin_, volumeSize_;
};

}  // namespace crossrender
