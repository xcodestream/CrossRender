// =============================================================================
// Voxel.cpp - чанковое воксельное хранилище, greedy-мешинг, ambient occlusion,
//             блочное освещение, генерация мира, палитра-атлас и путь
//             рендеринга raymarching по 3D-текстуре.
//
// Порядок хранения внутри чанка (задокументированный контракт):
//     index = x + z * 32 + y * 32 * 32        (x самый быстрый, затем z, затем y)
//     x, y, z в [0, 32).  Всё снаружи — "air" / "lit" для чтения.
//
// Мировые координаты знаковые и преобразуются с floor-делением, поэтому
// отрицательные координаты корректно попадают в отрицательные чанки (C-усечение не используется).
//
// Мешинг никогда не выделяет память на грань: черновая маска живёт на стеке,
// а выходные векторы сохраняют ёмкость между пересборками.
// =============================================================================
#include "crossrender/voxel/Voxel.h"

#include "crossrender/gfx/GL.h"
#include "crossrender/core/Log.h"
#include "crossrender/gfx/Shader.h"
#include "crossrender/gfx/Renderer3D.h"

#include <cmath>
#include <string>
#include <vector>
#include <cstring>
#include <algorithm>
#include <unordered_map>

namespace crossrender {
namespace {

constexpr int kS = kVoxelChunkSize;
constexpr int kS2 = kS * kS;
constexpr int kVol = kVoxelChunkVolume;

// ---------------------------------------------------------------------------
// Раскладка палитры, используемая встроенными генераторами.
//
// NOTE: замороженные аргументы по умолчанию `GenerateFlat` — `top = 2, fill = 3`,
// что осмысленно, только когда id 2 — поверхность травы, а id 3 — заполнение
// землёй, поэтому здесь используется такая раскладка (0 воздух, 1 камень,
// 2 трава, 3 земля, 4 бок травы, далее остальные нужные материалы).
// ---------------------------------------------------------------------------
enum PaletteId : u8 {
    kAir = 0,
    kStone = 1,
    kGrass = 2,
    kDirt = 3,
    kGrassSide = 4,
    kSand = 5,
    kWood = 6,
    kLeaves = 7,
    kWater = 8,
    kGlass = 9,
    kBrick = 10,
    kSnow = 11,
    kIce = 12,
    kLava = 13,
    kCobble = 14,
    kPlanks = 15,
    kGravel = 16,
    kObsidian = 17,
    kGlowstone = 18,
    kMetal = 19,
    kGold = 20,
    kRedWool = 21,
    kBlueWool = 22,
    kPaletteCount = 23,
};

// ---------------------------------------------------------------------------
// Малые помощники
// ---------------------------------------------------------------------------
inline int FloorDiv(int a, int b) {
    int q = a / b;
    if ((a % b) != 0 && ((a < 0) != (b < 0))) --q;
    return q;
}
inline int FloorMod(int a, int b) {
    const int r = a % b;
    return (r != 0 && r < 0) ? r + b : r;
}
inline bool InChunk(int v) { return v >= 0 && v < kS; }
inline int VoxelIndex(int x, int y, int z) { return x + z * kS + y * kS2; }
inline int FloorToInt(f32 v) { return static_cast<int>(std::floor(v)); }

inline bool IsOpaqueId(const VoxelPalette& p, u8 id) {
    return id != 0 && p.Get(id).a >= 0.99f;
}

// ---------------------------------------------------------------------------
// Детерминированный целочисленный хеш-шум (без rand() и глобального состояния).
// ---------------------------------------------------------------------------
inline u32 HashU32(u32 x) {
    x ^= x >> 16;
    x *= 0x7FEB352Du;
    x ^= x >> 15;
    x *= 0x846CA68Bu;
    x ^= x >> 16;
    return x;
}
inline u32 Hash3(i32 x, i32 y, i32 z, u32 seed) {
    u32 h = seed * 0x9E3779B9u;
    h ^= static_cast<u32>(x) * 0x8DA6B343u;
    h ^= static_cast<u32>(y) * 0xD8163841u;
    h ^= static_cast<u32>(z) * 0xCB1AB31Fu;
    return HashU32(h);
}
inline f32 HashFloat(i32 x, i32 y, i32 z, u32 seed) {
    return static_cast<f32>(Hash3(x, y, z, seed) >> 8) * (1.0f / 16777216.0f);
}
inline f32 ValueNoise2(f32 x, f32 y, u32 seed) {
    const i32 xi = FloorToInt(x), yi = FloorToInt(y);
    const f32 fx = x - static_cast<f32>(xi), fy = y - static_cast<f32>(yi);
    const f32 ux = fx * fx * (3.0f - 2.0f * fx);
    const f32 uy = fy * fy * (3.0f - 2.0f * fy);
    const f32 a = HashFloat(xi, yi, 0, seed);
    const f32 b = HashFloat(xi + 1, yi, 0, seed);
    const f32 c = HashFloat(xi, yi + 1, 0, seed);
    const f32 d = HashFloat(xi + 1, yi + 1, 0, seed);
    return Lerp(Lerp(a, b, ux), Lerp(c, d, ux), uy);
}
inline f32 Fbm2(f32 x, f32 y, u64 seed, int octaves) {
    f32 sum = 0.0f, amp = 1.0f, norm = 0.0f;
    for (int i = 0; i < octaves; ++i) {
        sum += ValueNoise2(x, y, static_cast<u32>(seed) + static_cast<u32>(i) * 7919u) * amp;
        norm += amp;
        x *= 2.03f;
        y *= 1.97f;
        amp *= 0.5f;
    }
    return norm > 0.0f ? sum / norm : 0.0f;
}
inline f32 ValueNoise3(f32 x, f32 y, f32 z, u32 seed) {
    const i32 xi = FloorToInt(x), yi = FloorToInt(y), zi = FloorToInt(z);
    const f32 fx = x - static_cast<f32>(xi), fy = y - static_cast<f32>(yi), fz = z - static_cast<f32>(zi);
    const f32 ux = fx * fx * (3.0f - 2.0f * fx);
    const f32 uy = fy * fy * (3.0f - 2.0f * fy);
    const f32 uz = fz * fz * (3.0f - 2.0f * fz);
    const f32 c000 = HashFloat(xi, yi, zi, seed);
    const f32 c100 = HashFloat(xi + 1, yi, zi, seed);
    const f32 c010 = HashFloat(xi, yi + 1, zi, seed);
    const f32 c110 = HashFloat(xi + 1, yi + 1, zi, seed);
    const f32 c001 = HashFloat(xi, yi, zi + 1, seed);
    const f32 c101 = HashFloat(xi + 1, yi, zi + 1, seed);
    const f32 c011 = HashFloat(xi, yi + 1, zi + 1, seed);
    const f32 c111 = HashFloat(xi + 1, yi + 1, zi + 1, seed);
    const f32 x00 = Lerp(c000, c100, ux), x10 = Lerp(c010, c110, ux);
    const f32 x01 = Lerp(c001, c101, ux), x11 = Lerp(c011, c111, ux);
    return Lerp(Lerp(x00, x10, uy), Lerp(x01, x11, uy), uz);
}
inline f32 Fbm3(f32 x, f32 y, f32 z, u32 seed, int octaves) {
    f32 sum = 0.0f, amp = 1.0f, norm = 0.0f;
    for (int i = 0; i < octaves; ++i) {
        sum += ValueNoise3(x, y, z, seed + static_cast<u32>(i) * 6151u) * amp;
        norm += amp;
        x *= 2.01f;
        y *= 2.05f;
        z *= 1.99f;
        amp *= 0.5f;
    }
    return norm > 0.0f ? sum / norm : 0.0f;
}

// ---------------------------------------------------------------------------
// Таблицы граней.  Грани упорядочены +X, -X, +Y, -Y, +Z, -Z (face = axis*2 +
// (sign > 0 ? 0 : 1)).  Для каждой грани `u` и `v` — две оси в её плоскости,
// а `kFaceCorner[f][k]` даёт угол k как (смещение по u, смещение по v).
//
// Квады ниже выпускаются против часовой стрелки при взгляде снаружи, поэтому
// отсечение задних граней по умолчанию в движке их сохраняет.
// ---------------------------------------------------------------------------
constexpr int kFaceAxis[6] = {0, 0, 1, 1, 2, 2};
constexpr int kFaceSign[6] = {1, -1, 1, -1, 1, -1};
constexpr int kFaceUAxis[6] = {2, 2, 0, 0, 1, 1};
constexpr int kFaceVAxis[6] = {1, 1, 2, 2, 0, 0};
constexpr int kFaceCorner[6][4][2] = {
    {{1, 0}, {0, 0}, {0, 1}, {1, 1}},  // +X
    {{0, 0}, {1, 0}, {1, 1}, {0, 1}},  // -X
    {{0, 0}, {0, 1}, {1, 1}, {1, 0}},  // +Y
    {{0, 1}, {0, 0}, {1, 0}, {1, 1}},  // -Y
    {{0, 0}, {0, 1}, {1, 1}, {1, 0}},  // +Z
    {{0, 0}, {1, 0}, {1, 1}, {0, 1}},  // -Z
};

inline Vec3 FaceNormal(int f) {
    const int a = kFaceAxis[f];
    const f32 s = static_cast<f32>(kFaceSign[f]);
    return a == 0 ? Vec3{s, 0, 0} : (a == 1 ? Vec3{0, s, 0} : Vec3{0, 0, s});
}
// Классический воксельный AO: 3 = полностью открыто, 0 = полностью закрыто.
inline int VertexAO(int side1, int side2, int corner) {
    if (side1 && side2) return 0;
    return 3 - (side1 + side2 + corner);
}

// Ячейка текстуры для грани: трава показывает боковую текстуру на боках и
// верхнюю — сверху/снизу.
inline u8 FaceTextureId(u8 id, int axis) {
    if (id == kGrass) return axis == 1 ? kGrass : kGrassSide;
    return id;
}

// Упаковывает знаковые мировые воксельные координаты в 21 бит на ось (хранение очереди).
// Значения хранятся в дополнительном коде и расширяются знаком на выходе.
inline u64 PackVoxel(int x, int y, int z) {
    const u64 ox = static_cast<u64>(static_cast<u32>(x)) & 0x1FFFFFULL;
    const u64 oy = static_cast<u64>(static_cast<u32>(y)) & 0x1FFFFFULL;
    const u64 oz = static_cast<u64>(static_cast<u32>(z)) & 0x1FFFFFULL;
    return ox | (oy << 21) | (oz << 42);
}
inline int SignExtend21(u64 v) {
    const u32 s = static_cast<u32>(v) & 0x1FFFFFu;
    return static_cast<int>((s ^ 0x100000u) - 0x100000u);
}
inline void UnpackVoxel(u64 p, int* x, int* y, int* z) {
    *x = SignExtend21(p);
    *y = SignExtend21(p >> 21);
    *z = SignExtend21(p >> 42);
}

// ---------------------------------------------------------------------------
// Помощник заполнения чанков: кэширует чанк последнего записанного вокселя.
// ---------------------------------------------------------------------------
struct ChunkWriter {
    VoxelWorld* world = nullptr;
    VoxelChunk* chunk = nullptr;
    ChunkCoord coord;

    explicit ChunkWriter(VoxelWorld* w) : world(w) {}

    void Set(int x, int y, int z, u8 id) {
        const ChunkCoord c{FloorDiv(x, kS), FloorDiv(y, kS), FloorDiv(z, kS)};
        if (chunk == nullptr || !(c == coord)) {
            chunk = world->GetOrCreateChunk(c);
            coord = c;
        }
        if (chunk) chunk->Set(FloorMod(x, kS), FloorMod(y, kS), FloorMod(z, kS), id, 15);
    }
};

// ---------------------------------------------------------------------------
// Ресурсы raymarch (живут всё время процесса: без разбора GL на выходе).
// Замороженный API VoxelWorld не имеет члена для шейдера/меша, поэтому режим
// raymarch владеет единственной лениво собранной программой + мешем единичного бокса здесь.
// ---------------------------------------------------------------------------
struct RaymarchResources {
    Shader shader;
    Mesh box;
    bool built = false;
    bool ok = false;
};

RaymarchResources& Raymarch() {
    static RaymarchResources* res = new RaymarchResources();
    return *res;
}

// GLSL-преамбула, повторяющая crossrender::builtin::Preamble() (Shader.cpp может ещё
// не существовать, поэтому воксельный модуль несёт собственную копию).
const char* VoxelPreamble() {
#if ENG_GLES
    return "#version 300 es\n"
           "precision highp float;\n"
           "precision highp int;\n"
           "precision highp sampler3D;\n";
#else
    return "#version 330 core\n";
#endif
}

const char* kVoxelRaymarchVert = R"GLSL(
layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec3 aNormal;
uniform mat4 uViewProj;
uniform vec3 uBoxMin;
uniform vec3 uBoxSize;
out vec3 vWorldPos;
void main() {
    vec3 world = uBoxMin + aPosition * uBoxSize;
    vWorldPos = world;
    gl_Position = uViewProj * vec4(world, 1.0);
}
)GLSL";

const char* kVoxelRaymarchFrag = R"GLSL(
precision highp float;
precision highp sampler3D;
in vec3 vWorldPos;
out vec4 fragColor;
uniform sampler3D uVolume;
uniform vec3 uBoxMin;
uniform vec3 uBoxSize;
uniform vec3 uCameraPos;
uniform vec3 uLightDir;
uniform vec3 uAmbient;
uniform float uVoxelSize;
uniform float uStepScale;
uniform float uOpacity;
uniform int uMaxSteps;

float occ(vec3 uvw) { return texture(uVolume, clamp(uvw, vec3(0.0), vec3(1.0))).a; }

void main() {
    vec3 lo = uBoxMin;
    vec3 hi = uBoxMin + uBoxSize;
    vec3 ro = uCameraPos;
    vec3 rd = normalize(vWorldPos - ro);
    vec3 inv = vec3(1.0) / rd;
    vec3 t0 = (lo - ro) * inv;
    vec3 t1 = (hi - ro) * inv;
    vec3 tsmall = min(t0, t1);
    vec3 tbig = max(t0, t1);
    float tNear = max(max(tsmall.x, tsmall.y), tsmall.z);
    float tFar = min(min(tbig.x, tbig.y), tbig.z);
    tNear = max(tNear, 0.0);
    if (tFar <= tNear) discard;

    float stepLen = max(uVoxelSize * uStepScale, 0.05);
    vec4 acc = vec4(0.0);
    float t = tNear + stepLen * 0.5;
    for (int i = 0; i < 512; ++i) {
        if (i >= uMaxSteps || t > tFar || acc.a > 0.985) break;
        vec3 uvw = (ro + rd * t - lo) / uBoxSize;
        vec4 s = texture(uVolume, clamp(uvw, vec3(0.0), vec3(1.0)));
        if (s.a > 0.004) {
            vec3 texel = 1.0 / vec3(textureSize(uVolume, 0));
            vec3 grad = vec3(
                occ(uvw + vec3(texel.x, 0.0, 0.0)) - occ(uvw - vec3(texel.x, 0.0, 0.0)),
                occ(uvw + vec3(0.0, texel.y, 0.0)) - occ(uvw - vec3(0.0, texel.y, 0.0)),
                occ(uvw + vec3(0.0, 0.0, texel.z)) - occ(uvw - vec3(0.0, 0.0, texel.z)));
            vec3 nrm = normalize(grad + vec3(0.0, 0.0001, 0.0));
            float ndl = max(dot(nrm, uLightDir), 0.0);
            vec3 shade = uAmbient + vec3(0.85) * ndl;
            float a = s.a * uOpacity * (1.0 - acc.a);
            acc.rgb += s.rgb * shade * a;
            acc.a += a;
        }
        t += stepLen;
    }
    if (acc.a < 0.004) discard;
    fragColor = vec4(acc.rgb, acc.a);
}
)GLSL";

MeshData MakeUnitBoxMesh() {
    MeshData md;
    md.name = "voxel_raymarch_box";
    Bounds b;
    b.Expand(Vec3{0, 0, 0});
    b.Expand(Vec3{1, 1, 1});
    md.bounds = b;
    for (int f = 0; f < 6; ++f) {
        const int axis = kFaceAxis[f], ua = kFaceUAxis[f], va = kFaceVAxis[f];
        const Vec3 n = FaceNormal(f);
        const u32 base = static_cast<u32>(md.vertices.size());
        for (int k = 0; k < 4; ++k) {
            const int du = kFaceCorner[f][k][0], dv = kFaceCorner[f][k][1];
            int p[3] = {0, 0, 0};
            if (kFaceSign[f] > 0) p[axis] = 1;
            p[ua] += du;
            p[va] += dv;
            Vertex v;
            v.position = {static_cast<f32>(p[0]), static_cast<f32>(p[1]), static_cast<f32>(p[2])};
            v.normal = n;
            v.uv = {static_cast<f32>(du), static_cast<f32>(dv)};
            v.uv2 = {1.0f, 1.0f};
            v.color = Vec4{1, 1, 1, 1};
            md.vertices.push_back(v);
        }
        md.indices.push_back(base + 0);
        md.indices.push_back(base + 1);
        md.indices.push_back(base + 2);
        md.indices.push_back(base + 0);
        md.indices.push_back(base + 2);
        md.indices.push_back(base + 3);
    }
    return md;
}

void BuildRaymarchResources() {
    RaymarchResources& res = Raymarch();
    if (res.built) return;
    res.built = true;
    const std::string vs = std::string(VoxelPreamble()) + kVoxelRaymarchVert;
    const std::string fs = std::string(VoxelPreamble()) + kVoxelRaymarchFrag;
    if (!res.shader.Build(vs.c_str(), fs.c_str(), "voxel_raymarch")) {
        ENG_LOGW("voxel", "raymarch shader build failed: %s", res.shader.Log().c_str());
        res.ok = false;
        return;
    }
    const MeshData box = MakeUnitBoxMesh();
    res.ok = res.box.Create(box) && res.box.Valid();
    if (!res.ok) ENG_LOGW("voxel", "raymarch box mesh upload failed");
}

}  // namespace

// ===========================================================================
// VoxelPalette
// ===========================================================================
VoxelPalette::VoxelPalette() {
    colors.assign(kPaletteCount, Color{1, 1, 1, 1});
    colors[kAir] = Color{0, 0, 0, 0};
}

VoxelPalette VoxelPalette::Default() {
    VoxelPalette p;
    p.colors.assign(kPaletteCount, Color{1, 1, 1, 1});
    auto set = [&p](u8 id, u32 hex, f32 a = 1.0f) { p.colors[id] = Color::FromRGB(hex).WithAlpha(a); };
    set(kAir, 0x000000, 0.0f);
    set(kStone, 0x8A8A90);
    set(kGrass, 0x5FA845);
    set(kDirt, 0x7A5636);
    set(kGrassSide, 0x6E9445);
    set(kSand, 0xD9CB8C);
    set(kWood, 0x6B4F2A);
    set(kLeaves, 0x3F7A2E);
    set(kWater, 0x3A6FD8, 0.72f);
    set(kGlass, 0xCFEAF5, 0.30f);
    set(kBrick, 0xA64B3A);
    set(kSnow, 0xF2F6FA);
    set(kIce, 0xA8D8F0, 0.78f);
    set(kLava, 0xE2581F);
    set(kCobble, 0x7A7A7E);
    set(kPlanks, 0xB08A50);
    set(kGravel, 0x8C8480);
    set(kObsidian, 0x241C33);
    set(kGlowstone, 0xF7D97A);
    set(kMetal, 0xB9C2CC);
    set(kGold, 0xE8C14A);
    set(kRedWool, 0xB8433C);
    set(kBlueWool, 0x3F5FB0);
    return p;
}

VoxelPalette VoxelPalette::Ice() {
    VoxelPalette p = Default();
    p.colors[kStone] = Color::FromRGB(0x9FB4C4);
    p.colors[kGrass] = Color::FromRGB(0xDCEFF7);
    p.colors[kDirt] = Color::FromRGB(0x8FA6B4);
    p.colors[kGrassSide] = Color::FromRGB(0xC4DDEA);
    p.colors[kSand] = Color::FromRGB(0xC9DCE8);
    p.colors[kWater] = Color::FromRGB(0x2E6FA8).WithAlpha(0.82f);
    p.colors[kLeaves] = Color::FromRGB(0x8FC8C0);
    p.colors[kWood] = Color::FromRGB(0x6E6259);
    p.colors[kIce] = Color::FromRGB(0xBFE6F5).WithAlpha(0.85f);
    return p;
}

// ===========================================================================
// VoxelChunk
// ===========================================================================
void VoxelChunk::Set(int x, int y, int z, u8 id, u8 light) {
    if (!InChunk(x) || !InChunk(y) || !InChunk(z)) return;
    const int i = VoxelIndex(x, y, z);
    const u8 old = voxels_[i];
    if (lights_.size() != static_cast<usize>(kVol)) lights_.assign(kVol, 15);
    const u8 oldLight = lights_[i];
    if (old == id && oldLight == light) return;
    if (old == 0 && id != 0) {
        ++nonEmptyCount_;
    } else if (old != 0 && id == 0) {
        --nonEmptyCount_;
    }
    voxels_[i] = id;
    lights_[i] = light;
    dirty_ = true;
}

u8 VoxelChunk::Get(int x, int y, int z) const {
    if (!InChunk(x) || !InChunk(y) || !InChunk(z)) return 0;
    return voxels_[VoxelIndex(x, y, z)];
}

u8 VoxelChunk::GetLight(int x, int y, int z) const {
    if (!InChunk(x) || !InChunk(y) || !InChunk(z)) return 15;
    if (lights_.size() != static_cast<usize>(kVol)) return 15;
    return lights_[VoxelIndex(x, y, z)];
}

void VoxelChunk::Fill(u8 id) {
    std::fill(voxels_.begin(), voxels_.end(), id);
    lights_.assign(kVol, 15);
    nonEmptyCount_ = (id == 0) ? 0 : kVol;
    dirty_ = true;
}

void VoxelChunk::Clear() { Fill(0); }

// ---------------------------------------------------------------------------
// Мешинг
// ---------------------------------------------------------------------------
void VoxelChunk::RebuildMesh(const VoxelPalette& palette, bool greedy, bool textured, bool ao,
                             const VoxelWorld* owner) {
    std::vector<Vertex>& verts = meshData_.vertices;
    std::vector<u32>& idx = meshData_.indices;
    verts.clear();
    idx.clear();
    meshValid_ = false;
    dirty_ = false;
    if (meshData_.name.empty()) meshData_.name = "voxel_chunk";

    if (nonEmptyCount_ == 0) {
        meshData_.bounds = Bounds{};
        meshData_.subMeshes.clear();
        if (gpuMesh_.Valid()) gpuMesh_.Destroy();
        return;
    }
    if (verts.capacity() == 0) verts.reserve(4096);
    if (idx.capacity() == 0) idx.reserve(8192);
    if (meshData_.subMeshes.empty()) {
        MeshData::SubMesh sm;
        sm.indexOffset = 0;
        sm.indexCount = 0;
        sm.name = meshData_.name;
        meshData_.subMeshes.push_back(sm);
    }

    const bool bakeLight = (owner != nullptr) ? owner->Options().bakeLight : true;
    const bool cullInterior = (owner != nullptr) ? owner->Options().cullInterior : true;

    // Выборка (возможно, вне чанка) вокселя с откатом к соседнему чанку через
    // владельца, чтобы внутренние стены на границах чанков отсекались.
    auto sampleVoxel = [&](int lx, int ly, int lz) -> u8 {
        if (InChunk(lx) && InChunk(ly) && InChunk(lz)) return voxels_[VoxelIndex(lx, ly, lz)];
        if (owner == nullptr) return 0;
        return owner->GetVoxel(coord_.x * kS + lx, coord_.y * kS + ly, coord_.z * kS + lz);
    };
    // Грань выживает, когда сосед — воздух, либо другой полупрозрачный материал
    // (вода рядом со стеклом); непрозрачные и same-id соседи её скрывают.
    // `cullInterior = false` отключает это отсечение.
    auto faceVisible = [&](u8 self, u8 nb) -> bool {
        if (nb == 0) return true;
        if (!cullInterior) return true;
        if (nb == self) return false;
        return !IsOpaqueId(palette, nb);
    };
    auto opaqueAt = [&](int lx, int ly, int lz) -> int {
        return IsOpaqueId(palette, sampleVoxel(lx, ly, lz)) ? 1 : 0;
    };

    auto emitFace = [&](int f, int x, int y, int z, int w, int h, u8 id) {
        const int axis = kFaceAxis[f], ua = kFaceUAxis[f], va = kFaceVAxis[f];
        const Vec3 n = FaceNormal(f);
        const u8 texId = FaceTextureId(id, axis);
        const f32 cellS = 1.0f / 16.0f;
        const f32 cellX = static_cast<f32>(texId % 16) * cellS;
        const f32 cellY = static_cast<f32>(texId / 16) * cellS;
        const Color base = textured ? Color{1, 1, 1, 1} : palette.Get(id);

        u8 light = 15;
        if (lights_.size() == static_cast<usize>(kVol)) light = lights_[VoxelIndex(x, y, z)];
        const f32 lightShade =
            bakeLight ? (0.15f + 0.85f * (static_cast<f32>(light) * (1.0f / 15.0f))) : 1.0f;

        const u32 first = static_cast<u32>(verts.size());
        for (int k = 0; k < 4; ++k) {
            const int du = kFaceCorner[f][k][0], dv = kFaceCorner[f][k][1];
            int p[3] = {x, y, z};
            if (kFaceSign[f] > 0) p[axis] += 1;
            p[ua] += du ? w : 0;
            p[va] += dv ? h : 0;

            f32 shade = lightShade;
            f32 aoValue = 1.0f;
            if (ao) {
                const int su = du ? 1 : -1;
                const int sv = dv ? 1 : -1;
                int b[3] = {x, y, z};
                b[ua] += du ? (w - 1) : 0;
                b[va] += dv ? (h - 1) : 0;
                int o1[3] = {0, 0, 0};
                int o2[3] = {0, 0, 0};
                int o3[3] = {0, 0, 0};
                o1[axis] = o2[axis] = o3[axis] = kFaceSign[f];
                o1[ua] += su;
                o2[va] += sv;
                o3[ua] += su;
                o3[va] += sv;
                const int s1 = opaqueAt(b[0] + o1[0], b[1] + o1[1], b[2] + o1[2]);
                const int s2 = opaqueAt(b[0] + o2[0], b[1] + o2[1], b[2] + o2[2]);
                const int s3 = opaqueAt(b[0] + o3[0], b[1] + o3[1], b[2] + o3[2]);
                const int vao = VertexAO(s1, s2, s3);
                aoValue = static_cast<f32>(vao) * (1.0f / 3.0f);
                shade *= 0.45f + 0.55f * aoValue;
            }

            Vertex v;
            v.position = {static_cast<f32>(p[0]), static_cast<f32>(p[1]), static_cast<f32>(p[2])};
            v.normal = n;
            // Обе протяжённости в плоскости грани свёрнуты в UV атласа: смещение
            // угла нормируется объединённым отрезком (w по u, h по v), чтобы
            // ячейка покрывала весь квад. Палитра — атлас, поэтому буквальный
            // повтор на воксель переходил бы в соседние ячейки
            // (чужие материалы) — перенос требует fract() во фрагментном
            // шейдере, чего forward-шейдер движка не делает.
            const f32 lu = static_cast<f32>(du ? w : 0) / static_cast<f32>(w);
            const f32 lv = static_cast<f32>(dv ? h : 0) / static_cast<f32>(h);
            v.uv = textured ? Vec2{cellX + lu * cellS, cellY + lv * cellS} : Vec2{lu, lv};
            v.uv2 = {aoValue, bakeLight ? (static_cast<f32>(light) * (1.0f / 15.0f)) : 1.0f};
            v.color = Vec4{base.r * shade, base.g * shade, base.b * shade, base.a};
            v.tangent = Vec4{1, 0, 0, 1};
            verts.push_back(v);
        }

        // Разрезаем по диагонали с меньшей суммарной окклюзией: это убирает
        // артефакт анизотропии интерполяции AO.
        bool flip = false;
        if (ao) {
            const f32 a0 = verts[first + 0].uv2.x;
            const f32 a1 = verts[first + 1].uv2.x;
            const f32 a2 = verts[first + 2].uv2.x;
            const f32 a3 = verts[first + 3].uv2.x;
            flip = (a0 + a2) > (a1 + a3);
        }
        if (flip) {
            idx.push_back(first + 0);
            idx.push_back(first + 1);
            idx.push_back(first + 3);
            idx.push_back(first + 1);
            idx.push_back(first + 2);
            idx.push_back(first + 3);
        } else {
            idx.push_back(first + 0);
            idx.push_back(first + 1);
            idx.push_back(first + 2);
            idx.push_back(first + 0);
            idx.push_back(first + 2);
            idx.push_back(first + 3);
        }
    };

    int mask[kS * kS];
    for (int axis = 0; axis < 3; ++axis) {
        const int ua = kFaceUAxis[axis * 2];
        const int va = kFaceVAxis[axis * 2];
        for (int pass = 0; pass < 2; ++pass) {
            const int sign = (pass == 0) ? 1 : -1;
            const int f = axis * 2 + (pass == 0 ? 0 : 1);
            for (int d = 0; d < kS; ++d) {
                std::memset(mask, 0, sizeof(mask));
                for (int v = 0; v < kS; ++v) {
                    for (int u = 0; u < kS; ++u) {
                        int c[3];
                        c[axis] = d;
                        c[ua] = u;
                        c[va] = v;
                        const u8 id = voxels_[VoxelIndex(c[0], c[1], c[2])];
                        if (id == 0) continue;
                        int nb[3] = {c[0], c[1], c[2]};
                        nb[axis] += sign;
                        if (faceVisible(id, sampleVoxel(nb[0], nb[1], nb[2]))) mask[v * kS + u] = id;
                    }
                }
                if (!greedy) {
                    for (int v = 0; v < kS; ++v) {
                        for (int u = 0; u < kS; ++u) {
                            const u8 id = static_cast<u8>(mask[v * kS + u]);
                            if (id == 0) continue;
                            int c[3];
                            c[axis] = d;
                            c[ua] = u;
                            c[va] = v;
                            emitFace(f, c[0], c[1], c[2], 1, 1, id);
                        }
                    }
                    continue;
                }
                for (int v = 0; v < kS; ++v) {
                    for (int u = 0; u < kS;) {
                        const u8 id = static_cast<u8>(mask[v * kS + u]);
                        if (id == 0) {
                            ++u;
                            continue;
                        }
                        int w = 1;
                        while (u + w < kS && mask[v * kS + u + w] == id) ++w;
                        int h = 1;
                        while (v + h < kS) {
                            bool ok = true;
                            for (int k = 0; k < w; ++k) {
                                if (mask[(v + h) * kS + u + k] != id) {
                                    ok = false;
                                    break;
                                }
                            }
                            if (!ok) break;
                            ++h;
                        }
                        for (int vv = 0; vv < h; ++vv) {
                            for (int uu = 0; uu < w; ++uu) mask[(v + vv) * kS + u + uu] = 0;
                        }
                        int c[3];
                        c[axis] = d;
                        c[ua] = u;
                        c[va] = v;
                        emitFace(f, c[0], c[1], c[2], w, h, id);
                        u += w;
                    }
                }
            }
        }
    }

    Bounds b;
    for (const Vertex& v : verts) b.Expand(v.position);
    meshData_.bounds = verts.empty() ? Bounds{} : b;
    if (!meshData_.subMeshes.empty()) meshData_.subMeshes[0].indexCount = static_cast<u32>(idx.size());
}

void VoxelChunk::UploadMesh() {
    if (meshData_.indices.empty()) {
        if (gpuMesh_.Valid()) gpuMesh_.Destroy();
        meshValid_ = false;
        return;
    }
    meshValid_ = gpuMesh_.Create(meshData_);
}

// ===========================================================================
// VoxelWorld
// ===========================================================================
VoxelWorld::VoxelWorld() { palette_ = VoxelPalette::Default(); }

VoxelChunk* VoxelWorld::GetChunk(const ChunkCoord& c) {
    auto it = chunks_.find(c);
    return it == chunks_.end() ? nullptr : it->second.get();
}

VoxelChunk* VoxelWorld::GetOrCreateChunk(const ChunkCoord& c) {
    auto it = chunks_.find(c);
    if (it != chunks_.end()) return it->second.get();
    std::unique_ptr<VoxelChunk> ptr(new VoxelChunk());
    VoxelChunk* raw = ptr.get();
    raw->SetCoord(c);
    raw->world = this;
    chunks_.emplace(c, std::move(ptr));
    return raw;
}

void VoxelWorld::RemoveChunk(const ChunkCoord& c) { chunks_.erase(c); }

void VoxelWorld::Clear() {
    chunks_.clear();
    bounds_ = crossrender::Bounds{};
    if (volumeTexture_.Valid()) volumeTexture_.Destroy();
}

std::vector<VoxelChunk*> VoxelWorld::AllChunks() {
    std::vector<VoxelChunk*> out;
    out.reserve(chunks_.size());
    for (auto& kv : chunks_) {
        if (kv.second) out.push_back(kv.second.get());
    }
    return out;
}

void VoxelWorld::SetVoxel(int x, int y, int z, u8 id, bool createChunk) {
    const ChunkCoord c{FloorDiv(x, kS), FloorDiv(y, kS), FloorDiv(z, kS)};
    VoxelChunk* ch = GetChunk(c);
    if (ch == nullptr) {
        if (!createChunk) return;
        ch = GetOrCreateChunk(c);
    }
    const int lx = FloorMod(x, kS), ly = FloorMod(y, kS), lz = FloorMod(z, kS);
    ch->Set(lx, ly, lz, id);
    // Граничный воксель меняет отсечение граней соседнего чанка.
    if (lx == 0) {
        if (VoxelChunk* n = GetChunk({c.x - 1, c.y, c.z})) n->MarkDirty();
    }
    if (lx == kS - 1) {
        if (VoxelChunk* n = GetChunk({c.x + 1, c.y, c.z})) n->MarkDirty();
    }
    if (ly == 0) {
        if (VoxelChunk* n = GetChunk({c.x, c.y - 1, c.z})) n->MarkDirty();
    }
    if (ly == kS - 1) {
        if (VoxelChunk* n = GetChunk({c.x, c.y + 1, c.z})) n->MarkDirty();
    }
    if (lz == 0) {
        if (VoxelChunk* n = GetChunk({c.x, c.y, c.z - 1})) n->MarkDirty();
    }
    if (lz == kS - 1) {
        if (VoxelChunk* n = GetChunk({c.x, c.y, c.z + 1})) n->MarkDirty();
    }
}

u8 VoxelWorld::GetVoxel(int x, int y, int z) const {
    const ChunkCoord c{FloorDiv(x, kS), FloorDiv(y, kS), FloorDiv(z, kS)};
    auto it = chunks_.find(c);
    if (it == chunks_.end() || !it->second) return 0;
    return it->second->Get(FloorMod(x, kS), FloorMod(y, kS), FloorMod(z, kS));
}

bool VoxelWorld::IsSolid(int x, int y, int z) const { return GetVoxel(x, y, z) != 0; }

void VoxelWorld::UpdateMeshes() {
    std::vector<VoxelChunk*> dirty;
    dirty.reserve(chunks_.size());
    for (auto& kv : chunks_) {
        if (kv.second && kv.second->Dirty()) dirty.push_back(kv.second.get());
    }
    for (VoxelChunk* c : dirty) {
        c->RebuildMesh(palette_, meshing_.greedy, meshing_.textured, meshing_.ambientOcclusion, this);
        c->UploadMesh();
    }
}

void VoxelWorld::MarkAllDirty() {
    for (auto& kv : chunks_) {
        if (kv.second) kv.second->MarkDirty();
    }
}

void VoxelWorld::Render(Renderer3D& r, const Material& mat) {
    Material m = mat;  // по значению: указатель атласа не должен утечь наружу
    if (meshing_.textured) {
        if (!paletteAtlas_.Valid()) BuildPaletteAtlas(16);
        if (paletteAtlas_.Valid()) m.baseColorTex = &paletteAtlas_;
    } else {
        m.vertexColors = true;
    }
    for (auto& kv : chunks_) {
        VoxelChunk* c = kv.second.get();
        if (c == nullptr || c->Empty() || !c->GpuMesh().Valid()) continue;
        r.Draw(c->GpuMesh(), m, Mat4::Translate(c->WorldOrigin()));
    }
}

void VoxelWorld::RecomputeBounds() {
    crossrender::Bounds b;
    for (auto& kv : chunks_) {
        VoxelChunk* c = kv.second.get();
        if (c == nullptr || c->Empty()) continue;
        const Vec3 o = c->WorldOrigin();
        b.Expand(o);
        b.Expand(o + Vec3{static_cast<f32>(kS), static_cast<f32>(kS), static_cast<f32>(kS)});
    }
    bounds_ = b;
}

// ---------------------------------------------------------------------------
// Выборка / запросы
// ---------------------------------------------------------------------------
VoxelRayHit VoxelWorld::Raycast(const Vec3& origin, const Vec3& dir, f32 maxDistance) const {
    VoxelRayHit hit;
    const Vec3 d = Normalize(dir);
    if (LengthSq(d) < 0.5f || maxDistance <= 0.0f) return hit;

    int x = FloorToInt(origin.x), y = FloorToInt(origin.y), z = FloorToInt(origin.z);
    const int stepX = d.x > 0 ? 1 : (d.x < 0 ? -1 : 0);
    const int stepY = d.y > 0 ? 1 : (d.y < 0 ? -1 : 0);
    const int stepZ = d.z > 0 ? 1 : (d.z < 0 ? -1 : 0);
    const f32 kInf = 1e30f;
    const f32 tDeltaX = stepX != 0 ? std::fabs(1.0f / d.x) : kInf;
    const f32 tDeltaY = stepY != 0 ? std::fabs(1.0f / d.y) : kInf;
    const f32 tDeltaZ = stepZ != 0 ? std::fabs(1.0f / d.z) : kInf;
    f32 tMaxX = stepX != 0 ? ((stepX > 0 ? (static_cast<f32>(x + 1) - origin.x)
                                         : (origin.x - static_cast<f32>(x))) *
                              tDeltaX)
                           : kInf;
    f32 tMaxY = stepY != 0 ? ((stepY > 0 ? (static_cast<f32>(y + 1) - origin.y)
                                         : (origin.y - static_cast<f32>(y))) *
                              tDeltaY)
                           : kInf;
    f32 tMaxZ = stepZ != 0 ? ((stepZ > 0 ? (static_cast<f32>(z + 1) - origin.z)
                                         : (origin.z - static_cast<f32>(z))) *
                              tDeltaZ)
                           : kInf;

    f32 t = 0.0f;
    int nx = 0, ny = 0, nz = 0;
    for (int i = 0; i < 100000; ++i) {  // ограничено: tDelta >= 1 для единичных направлений
        const u8 id = GetVoxel(x, y, z);
        if (id != 0) {
            hit.hit = true;
            hit.x = x;
            hit.y = y;
            hit.z = z;
            hit.nx = nx;
            hit.ny = ny;
            hit.nz = nz;
            hit.distance = t;
            hit.id = id;
            return hit;
        }
        if (tMaxX < tMaxY && tMaxX < tMaxZ) {
            x += stepX;
            t = tMaxX;
            tMaxX += tDeltaX;
            nx = -stepX;
            ny = 0;
            nz = 0;
        } else if (tMaxY < tMaxZ) {
            y += stepY;
            t = tMaxY;
            tMaxY += tDeltaY;
            nx = 0;
            ny = -stepY;
            nz = 0;
        } else {
            z += stepZ;
            t = tMaxZ;
            tMaxZ += tDeltaZ;
            nx = 0;
            ny = 0;
            nz = -stepZ;
        }
        if (t > maxDistance) break;
    }
    return VoxelRayHit{};
}

f32 VoxelWorld::SampleAO(int x, int y, int z, int nx, int ny, int nz) const {
    int axis = 0, sign = 1;
    if (ny != 0) {
        axis = 1;
        sign = ny > 0 ? 1 : -1;
    } else if (nz != 0) {
        axis = 2;
        sign = nz > 0 ? 1 : -1;
    } else {
        axis = 0;
        sign = nx >= 0 ? 1 : -1;
    }
    const int ua = (axis + 1) % 3, va = (axis + 2) % 3;
    static const int kCorners[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    int total = 0;
    for (int k = 0; k < 4; ++k) {
        const int su = kCorners[k][0] ? 1 : -1;
        const int sv = kCorners[k][1] ? 1 : -1;
        int o1[3] = {0, 0, 0}, o2[3] = {0, 0, 0}, o3[3] = {0, 0, 0};
        o1[axis] = o2[axis] = o3[axis] = sign;
        o1[ua] += su;
        o2[va] += sv;
        o3[ua] += su;
        o3[va] += sv;
        const int s1 = IsSolid(x + o1[0], y + o1[1], z + o1[2]) ? 1 : 0;
        const int s2 = IsSolid(x + o2[0], y + o2[1], z + o2[2]) ? 1 : 0;
        const int s3 = IsSolid(x + o3[0], y + o3[1], z + o3[2]) ? 1 : 0;
        total += VertexAO(s1, s2, s3);
    }
    return static_cast<f32>(total) * (1.0f / 12.0f);
}

// ---------------------------------------------------------------------------
// Освещение: вертикальный skylight + затухающий flood fill (label correcting BFS).
// ---------------------------------------------------------------------------
void VoxelWorld::ComputeLighting(int sunHeight) {
    (void)sunHeight;  // вертикальный проход уже стартует с верха мира
    if (chunks_.empty()) return;

    std::vector<VoxelChunk*> order = AllChunks();
    std::sort(order.begin(), order.end(), [](const VoxelChunk* a, const VoxelChunk* b) {
        if (a->Coord().y != b->Coord().y) return a->Coord().y > b->Coord().y;
        if (a->Coord().x != b->Coord().x) return a->Coord().x < b->Coord().x;
        return a->Coord().z < b->Coord().z;
    });

    // Проход 1 — skylight: обходим каждый столбец сверху вниз. Первый твёрдый
    // воксель сохраняет 15 (это открытая поверхность), всё ниже него стартует
    // тёмным и поднимается flood fill.
    std::unordered_map<ChunkCoord, std::vector<u8>, ChunkCoordHash> openCols;
    for (VoxelChunk* c : order) {
        const ChunkCoord cc = c->Coord();
        std::vector<u8> flags(static_cast<usize>(kS) * kS, 0);
        VoxelChunk* above = GetChunk({cc.x, cc.y + 1, cc.z});
        const std::vector<u8>* aboveFlags = nullptr;
        if (above != nullptr) {
            auto it = openCols.find({cc.x, cc.y + 1, cc.z});
            if (it != openCols.end()) aboveFlags = &it->second;
        }
        for (int lz = 0; lz < kS; ++lz) {
            for (int lx = 0; lx < kS; ++lx) {
                bool open =
                    (above == nullptr) ? true : (aboveFlags != nullptr && (*aboveFlags)[lz * kS + lx] != 0);
                bool allAir = open;
                for (int y = kS - 1; y >= 0; --y) {
                    const u8 id = c->Get(lx, y, lz);
                    const u8 l = open ? 15 : 0;
                    if (id != 0) {
                        open = false;
                        allAir = false;
                    }
                    c->Set(lx, y, lz, id, l);
                }
                flags[lz * kS + lx] = allAir ? 1 : 0;
            }
        }
        openCols.emplace(cc, std::move(flags));
    }

    auto lightAt = [&](int x, int y, int z) -> int {
        auto it = chunks_.find({FloorDiv(x, kS), FloorDiv(y, kS), FloorDiv(z, kS)});
        if (it == chunks_.end() || !it->second) return -1;
        return it->second->GetLight(FloorMod(x, kS), FloorMod(y, kS), FloorMod(z, kS));
    };

    // Семена: освещённые ячейки, касающиеся более тёмных (или края мира).
    static const int kOff[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    std::vector<u64> queue;
    queue.reserve(4096);
    for (auto& kv : chunks_) {
        VoxelChunk* c = kv.second.get();
        if (c == nullptr || c->Empty()) continue;
        const ChunkCoord cc = c->Coord();
        for (int ly = 0; ly < kS; ++ly) {
            for (int lz = 0; lz < kS; ++lz) {
                for (int lx = 0; lx < kS; ++lx) {
                    if (c->GetLight(lx, ly, lz) != 15) continue;
                    bool boundary = false;
                    for (int d = 0; d < 6 && !boundary; ++d) {
                        const int nlx = lx + kOff[d][0];
                        const int nly = ly + kOff[d][1];
                        const int nlz = lz + kOff[d][2];
                        int nl;
                        if (InChunk(nlx) && InChunk(nly) && InChunk(nlz)) {
                            nl = c->GetLight(nlx, nly, nlz);
                        } else {
                            nl = lightAt(cc.x * kS + nlx, cc.y * kS + nly, cc.z * kS + nlz);
                        }
                        if (nl != 15) boundary = true;
                    }
                    if (boundary) {
                        queue.push_back(PackVoxel(cc.x * kS + lx, cc.y * kS + ly, cc.z * kS + lz));
                    }
                }
            }
        }
    }

    // Проход 2 — flood fill с затуханием: прозрачные соседи стоят 1
    // (2 вниз), непрозрачные 4, поэтому свет просачивается в камень лишь
    // на несколько блоков, а пещеры у поверхности всё же получают немного света.
    usize head = 0;
    while (head < queue.size()) {
        int x, y, z;
        UnpackVoxel(queue[head++], &x, &y, &z);
        const int cur = lightAt(x, y, z);
        if (cur <= 1) continue;
        for (int d = 0; d < 6; ++d) {
            const int nx2 = x + kOff[d][0];
            const int ny2 = y + kOff[d][1];
            const int nz2 = z + kOff[d][2];
            auto it = chunks_.find({FloorDiv(nx2, kS), FloorDiv(ny2, kS), FloorDiv(nz2, kS)});
            if (it == chunks_.end() || !it->second) continue;  // при освещении чанки не создаём
            VoxelChunk* nc = it->second.get();
            const int lx = FloorMod(nx2, kS), ly = FloorMod(ny2, kS), lz = FloorMod(nz2, kS);
            const u8 nid = nc->Get(lx, ly, lz);
            int cost = 1;
            if (IsOpaqueId(palette_, nid)) {
                cost = 4;
            } else if (kOff[d][1] < 0) {
                cost = 2;
            }
            const int nl = cur - cost;
            if (nl > 0 && nc->GetLight(lx, ly, lz) < nl) {
                nc->Set(lx, ly, lz, nid, static_cast<u8>(nl));
                if (nl > 1) queue.push_back(PackVoxel(nx2, ny2, nz2));
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Палитра-атлас
// ---------------------------------------------------------------------------
void VoxelWorld::BuildPaletteAtlas(int cellSize) {
    if (cellSize < 4) cellSize = 4;
    if (cellSize > 128) cellSize = 128;
    constexpr int kCells = 16;
    const int size = kCells * cellSize;
    std::vector<u8> px(static_cast<usize>(size) * static_cast<usize>(size) * 4u, 0);

    for (int id = 0; id < kCells * kCells; ++id) {
        const u8 pid = static_cast<u8>(id);
        const int cx = (id % kCells) * cellSize;
        const int cy = (id / kCells) * cellSize;
        const Color base = palette_.Get(pid);
        for (int y = 0; y < cellSize; ++y) {
            for (int x = 0; x < cellSize; ++x) {
                const f32 n = HashFloat(x * 7 + id * 131, y * 13 + id * 57, id, 0x51ED2701u);
                const f32 coarse = HashFloat(x / 3 + id, y / 3, id * 3, 0x1B873593u);
                f32 shade = 0.90f + 0.20f * n;
                f32 alpha = base.a;
                Color c = base;
                switch (pid) {
                    case kAir:
                        c = Color{0, 0, 0, 0};
                        alpha = 0.0f;
                        break;
                    case kGrass:
                        shade *= 0.95f + 0.12f * n;
                        break;
                    case kGrassSide: {
                        const f32 blade = std::fabs(std::sin((x + 1) * 1.9f + n * 2.0f));
                        shade *= 0.72f + 0.45f * blade * (y > cellSize / 3 ? 1.0f : 0.55f);
                        break;
                    }
                    case kSand:
                        shade = 0.93f + 0.14f * n + 0.05f * std::sin(y * 2.1f);
                        break;
                    case kWood: {
                        const f32 dx = x - cellSize * 0.5f, dy = y - cellSize * 0.5f;
                        const f32 r = std::sqrt(dx * dx + dy * dy);
                        shade *= 0.82f + 0.22f * std::fabs(std::sin(r * 1.15f));
                        break;
                    }
                    case kLeaves:
                        shade *= (n < 0.16f) ? 0.55f : (0.95f + 0.18f * coarse);
                        break;
                    case kWater:
                        shade *= 0.90f + 0.16f * std::sin((x * 1.0f + y * 0.6f) * 0.85f + n);
                        alpha = 0.72f;
                        break;
                    case kGlass: {
                        const bool edge = x == 0 || y == 0 || x == cellSize - 1 || y == cellSize - 1;
                        const bool glint = ((x - y) % cellSize + cellSize) % cellSize == 3;
                        c = edge ? Color::FromRGB(0xEAF7FC) : Color::FromRGB(0xD6EEF7);
                        shade = edge ? 1.0f : (glint ? 1.15f : 0.95f);
                        alpha = edge ? 0.55f : 0.18f;
                        break;
                    }
                    case kBrick: {
                        const int band = std::max(2, cellSize / 4);
                        const int row = y / band;
                        const int off = (row % 2) * (cellSize / 4);
                        const bool mortar =
                            (y % band == 0) || (((x + off) % std::max(4, cellSize / 2)) == 0);
                        shade *= mortar ? 0.78f : (0.95f + 0.15f * n);
                        break;
                    }
                    case kSnow:
                        shade = 0.97f + 0.06f * n;
                        break;
                    case kIce:
                        shade *= 0.93f + 0.12f * std::fabs(std::sin(x * 1.4f + y * 0.9f));
                        alpha = 0.80f;
                        break;
                    case kLava:
                        shade = 0.70f + 0.65f * (0.5f * (n + coarse));
                        break;
                    case kCobble:
                        shade = 0.72f + 0.45f * coarse;
                        break;
                    case kPlanks:
                        shade *= 0.88f + 0.18f * n + ((y % std::max(3, cellSize / 3)) == 0 ? -0.15f : 0.0f);
                        break;
                    case kGravel:
                        shade = 0.78f + 0.40f * coarse + 0.08f * n;
                        break;
                    case kObsidian:
                        shade = 0.70f + 0.30f * coarse + 0.12f * n;
                        break;
                    case kGlowstone:
                        shade = 0.95f + 0.45f * n * n;
                        break;
                    case kMetal:
                        shade *= 0.94f + 0.08f * std::sin(y * 2.3f) + 0.06f * n;
                        break;
                    case kGold:
                        shade *= (n > 0.92f) ? 1.35f : (0.92f + 0.14f * n);
                        break;
                    case kRedWool:
                    case kBlueWool:
                        shade *= 0.93f + 0.14f * coarse;
                        break;
                    default:
                        shade *= 0.92f + 0.16f * coarse;
                        break;
                }
                c.r = Clamp(c.r * shade, 0.0f, 1.0f);
                c.g = Clamp(c.g * shade, 0.0f, 1.0f);
                c.b = Clamp(c.b * shade, 0.0f, 1.0f);
                c.a = Clamp(alpha, 0.0f, 1.0f);
                const usize o =
                    (static_cast<usize>(cy + y) * static_cast<usize>(size) + static_cast<usize>(cx + x)) * 4u;
                px[o + 0] = static_cast<u8>(Clamp(c.r * 255.0f + 0.5f, 0.0f, 255.0f));
                px[o + 1] = static_cast<u8>(Clamp(c.g * 255.0f + 0.5f, 0.0f, 255.0f));
                px[o + 2] = static_cast<u8>(Clamp(c.b * 255.0f + 0.5f, 0.0f, 255.0f));
                px[o + 3] = static_cast<u8>(Clamp(c.a * 255.0f + 0.5f, 0.0f, 255.0f));
            }
        }
    }

    if (paletteAtlas_.Valid()) paletteAtlas_.Destroy();
    paletteAtlas_.Create(size, size, PixelFormat::RGBA8, px.data(), TextureFilter::Nearest,
                         TextureWrap::ClampToEdge, false);
    paletteAtlas_.SetDebugName("voxel_palette_atlas");
}

Rect VoxelWorld::PaletteUV(u8 id) const {
    const f32 s = 1.0f / 16.0f;
    const int cell = id % 16;
    const int row = id / 16;
    return Rect{static_cast<f32>(cell) * s, static_cast<f32>(row) * s, s, s};
}

// ---------------------------------------------------------------------------
// Режим raymarch по 3D-текстуре
// ---------------------------------------------------------------------------
bool VoxelWorld::BuildVolumeTexture(const Vec3& min, const Vec3& size, u8 fillOutside) {
    volumeMin_ = min;
    volumeSize_ = size;
    constexpr int kMaxDim = 384;
    const int ix = Clamp(static_cast<int>(std::ceil(std::max(size.x, 1.0f))), 1, kMaxDim);
    const int iy = Clamp(static_cast<int>(std::ceil(std::max(size.y, 1.0f))), 1, kMaxDim);
    const int iz = Clamp(static_cast<int>(std::ceil(std::max(size.z, 1.0f))), 1, kMaxDim);
    const int ox = FloorToInt(min.x), oy = FloorToInt(min.y), oz = FloorToInt(min.z);

    std::vector<u8> px(static_cast<usize>(ix) * static_cast<usize>(iy) * static_cast<usize>(iz) * 4u, 0);
    const Color outside = palette_.Get(fillOutside);
    for (int z = 0; z < iz; ++z) {
        for (int y = 0; y < iy; ++y) {
            for (int x = 0; x < ix; ++x) {
                const int wx = ox + x, wy = oy + y, wz = oz + z;
                auto it = chunks_.find({FloorDiv(wx, kS), FloorDiv(wy, kS), FloorDiv(wz, kS)});
                u8 id = 0;
                u8 light = 15;
                if (it != chunks_.end() && it->second) {
                    const int lx = FloorMod(wx, kS), ly = FloorMod(wy, kS), lz = FloorMod(wz, kS);
                    id = it->second->Get(lx, ly, lz);
                    light = it->second->GetLight(lx, ly, lz);
                }
                const usize o = ((static_cast<usize>(z) * static_cast<usize>(iy) +
                                  static_cast<usize>(y)) *
                                     static_cast<usize>(ix) +
                                 static_cast<usize>(x)) *
                                4u;
                if (id == 0) {
                    if (fillOutside != 0) {
                        px[o + 0] = static_cast<u8>(Clamp(outside.r * 255.0f + 0.5f, 0.0f, 255.0f));
                        px[o + 1] = static_cast<u8>(Clamp(outside.g * 255.0f + 0.5f, 0.0f, 255.0f));
                        px[o + 2] = static_cast<u8>(Clamp(outside.b * 255.0f + 0.5f, 0.0f, 255.0f));
                        px[o + 3] = 255;
                    }
                    continue;
                }
                const Color c = palette_.Get(id);
                px[o + 0] = static_cast<u8>(Clamp(c.r * 255.0f + 0.5f, 0.0f, 255.0f));
                px[o + 1] = static_cast<u8>(Clamp(c.g * 255.0f + 0.5f, 0.0f, 255.0f));
                px[o + 2] = static_cast<u8>(Clamp(c.b * 255.0f + 0.5f, 0.0f, 255.0f));
                // A = занятость * свет, никогда не полностью чёрный, чтобы пещеры были видны.
                px[o + 3] =
                    static_cast<u8>(Clamp(64.0f + 191.0f * (static_cast<f32>(light) / 15.0f), 0.0f, 255.0f));
            }
        }
    }

    if (volumeTexture_.Valid()) volumeTexture_.Destroy();
    const bool ok = volumeTexture_.Create3D(ix, iy, iz, PixelFormat::RGBA8, px.data(),
                                            TextureFilter::Nearest, TextureWrap::ClampToEdge);
    volumeTexture_.SetDebugName("voxel_volume");
    return ok;
}

void VoxelWorld::RenderRaymarched(Renderer3D& r, const Vec3& boxMin, const Vec3& boxSize,
                                  const Camera& cam) {
    if (!volumeTexture_.Valid()) return;        // нет GL-контекста / ничего не упаковано
    if (gl::glDrawElements == nullptr) return;  // загрузчик GL не инициализирован
    BuildRaymarchResources();
    RaymarchResources& res = Raymarch();
    if (!res.ok || !res.box.Valid()) return;

    Mat4 vp = r.ViewProj();
    if (vp.m[0] == 1.0f && vp.m[5] == 1.0f && vp.m[10] == 1.0f && vp.m[15] == 1.0f && vp.m[12] == 0.0f &&
        vp.m[13] == 0.0f && vp.m[14] == 0.0f) {
        vp = cam.ViewProj(16.0f / 9.0f);  // кадр ещё не начат: откат
    }

    res.shader.Bind();
    res.shader.Set("uViewProj", vp);
    res.shader.Set("uBoxMin", boxMin);
    res.shader.Set("uBoxSize", boxSize);
    res.shader.Set("uCameraPos", cam.position);
    res.shader.Set("uLightDir", Normalize(Vec3{0.45f, 0.82f, 0.35f}));
    res.shader.Set("uAmbient", Vec3{0.30f, 0.33f, 0.40f});
    res.shader.Set("uVoxelSize", 1.0f);
    res.shader.Set("uStepScale", 0.8f);
    res.shader.Set("uOpacity", 0.9f);
    res.shader.Set("uMaxSteps", 256);
    res.shader.SetTexture("uVolume", volumeTexture_, 0);

    const bool hadCull = gl::glIsEnabled != nullptr && gl::glIsEnabled(gl::GL_CULL_FACE) != 0;
    gl::glDisable(gl::GL_CULL_FACE);  // камера может находиться внутри бокса
    gl::glEnable(gl::GL_BLEND);
    gl::glBlendFunc(gl::GL_SRC_ALPHA, gl::GL_ONE_MINUS_SRC_ALPHA);
    gl::glDepthMask(gl::GL_FALSE);
    res.box.Draw();
    gl::glDepthMask(gl::GL_TRUE);
    gl::glDisable(gl::GL_BLEND);
    if (hadCull) gl::glEnable(gl::GL_CULL_FACE);
    Shader::Unbind();
}

// ---------------------------------------------------------------------------
// Генерация
// ---------------------------------------------------------------------------
void VoxelWorld::GenerateFlat(int chunksX, int chunksZ, int height, u8 top, u8 fill) {
    if (chunksX < 1) chunksX = 1;
    if (chunksZ < 1) chunksZ = 1;
    height = Clamp(height, 1, kS * 4);
    ChunkWriter w(this);
    for (int cz = 0; cz < chunksZ; ++cz) {
        for (int cx = 0; cx < chunksX; ++cx) {
            for (int z = 0; z < kS; ++z) {
                for (int x = 0; x < kS; ++x) {
                    for (int y = 0; y < height; ++y) {
                        w.Set(cx * kS + x, y, cz * kS + z, (y == height - 1) ? top : fill);
                    }
                }
            }
        }
    }
    MarkAllDirty();
    RecomputeBounds();
}

void VoxelWorld::GenerateSphere(const Vec3& center, f32 radius, u8 id) {
    if (radius <= 0.0f || id == 0) return;
    const int x0 = FloorToInt(center.x - radius), x1 = static_cast<int>(std::ceil(center.x + radius));
    const int y0 = FloorToInt(center.y - radius), y1 = static_cast<int>(std::ceil(center.y + radius));
    const int z0 = FloorToInt(center.z - radius), z1 = static_cast<int>(std::ceil(center.z + radius));
    const f32 r2 = radius * radius;
    ChunkWriter w(this);
    for (int z = z0; z <= z1; ++z) {
        for (int y = y0; y <= y1; ++y) {
            for (int x = x0; x <= x1; ++x) {
                const Vec3 p{static_cast<f32>(x) + 0.5f - center.x, static_cast<f32>(y) + 0.5f - center.y,
                             static_cast<f32>(z) + 0.5f - center.z};
                if (LengthSq(p) <= r2) w.Set(x, y, z, id);
            }
        }
    }
    MarkAllDirty();
    RecomputeBounds();
}

void VoxelWorld::GenerateBox(const Vec3& lo, const Vec3& hi, u8 id) {
    if (id == 0) return;
    const Vec3 a = Min(lo, hi), b = Max(lo, hi);
    const int x0 = FloorToInt(a.x), x1 = static_cast<int>(std::ceil(b.x));
    const int y0 = FloorToInt(a.y), y1 = static_cast<int>(std::ceil(b.y));
    const int z0 = FloorToInt(a.z), z1 = static_cast<int>(std::ceil(b.z));
    ChunkWriter w(this);
    for (int z = z0; z < z1; ++z) {
        for (int y = y0; y < y1; ++y) {
            for (int x = x0; x < x1; ++x) w.Set(x, y, z, id);
        }
    }
    MarkAllDirty();
    RecomputeBounds();
}

void VoxelWorld::GenerateTerrain(int chunksX, int chunksZ, u64 seed, int baseHeight, int amplitude) {
    if (chunksX < 1) chunksX = 1;
    if (chunksZ < 1) chunksZ = 1;
    if (amplitude < 0) amplitude = 0;
    if (baseHeight < 3) baseHeight = 3;
    const int spanX = chunksX * kS, spanZ = chunksZ * kS;
    const int maxY = std::min(kS * 6, std::max(baseHeight + amplitude + 8, 16));
    const int sea = std::max(1, baseHeight - 2);

    ChunkWriter w(this);
    for (int z = 0; z < spanZ; ++z) {
        for (int x = 0; x < spanX; ++x) {
            const f32 n = Fbm2(static_cast<f32>(x), static_cast<f32>(z), seed, 5);
            int h = baseHeight +
                    static_cast<int>(std::lround((n * 2.0f - 1.0f) * static_cast<f32>(amplitude)));
            h = Clamp(h, 1, maxY - 2);
            const bool damp = (h <= sea + 1);
            for (int y = 0; y <= h; ++y) {
                // Пещеры: полости 3D-шума, вырезанные ниже поверхности.
                if (y > 1 && y < h - 1) {
                    const f32 cave = Fbm3(static_cast<f32>(x) * 0.09f, static_cast<f32>(y) * 0.13f,
                                          static_cast<f32>(z) * 0.09f,
                                          static_cast<u32>(seed) ^ 0x9E37u, 3);
                    if (cave > 0.62f) continue;
                }
                u8 id = kStone;
                if (y == h) {
                    id = damp ? kSand : kGrass;
                } else if (y > h - 4) {
                    id = damp ? kSand : kDirt;
                }
                w.Set(x, y, z, id);
            }
            for (int y = h + 1; y <= sea && y < maxY; ++y) w.Set(x, y, z, kWater);
        }
    }

    // Рудные жилы в камне.
    const int veinCount = std::max(3, (spanX * spanZ) / 900);
    Random rng(seed ^ 0xA5A5A5A5ULL);
    for (int i = 0; i < veinCount; ++i) {
        const int x = rng.RangeInt(1, spanX - 2);
        const int z = rng.RangeInt(1, spanZ - 2);
        const int y = rng.RangeInt(2, std::max(3, baseHeight - 3));
        const u8 ore = rng.Chance(0.35f) ? kGold : kMetal;
        const int count = rng.RangeInt(4, 9);
        for (int k = 0; k < count; ++k) {
            const int ox = x + rng.RangeInt(-2, 2), oy = y + rng.RangeInt(-1, 1),
                      oz = z + rng.RangeInt(-2, 2);
            if (oy < 1 || oy >= maxY) continue;
            if (GetVoxel(ox, oy, oz) == kStone) SetVoxel(ox, oy, oz, ore);
        }
    }

    // Деревья на плоских травяных столбцах вдали от границы.
    Random treeRng(seed ^ 0x51ED2701ULL);
    const int attempts = (spanX * spanZ) / 70;
    for (int i = 0; i < attempts; ++i) {
        const int x = treeRng.RangeInt(3, spanX - 4);
        const int z = treeRng.RangeInt(3, spanZ - 4);
        int h = maxY - 1;
        while (h > 0 && GetVoxel(x, h, z) == 0) --h;
        if (GetVoxel(x, h, z) != kGrass || h < sea + 2) continue;
        if (GetVoxel(x, h + 1, z) != 0 || GetVoxel(x + 1, h, z) == 0 || GetVoxel(x - 1, h, z) == 0) continue;
        const int trunk = treeRng.RangeInt(4, 6);
        for (int y = 1; y <= trunk && h + y < maxY; ++y) SetVoxel(x, h + y, z, kWood);
        const int top = std::min(h + trunk, maxY - 2);
        for (int dy = -2; dy <= 1; ++dy) {
            const int r = (dy >= 0) ? 1 : 2;
            for (int dz = -r; dz <= r; ++dz) {
                for (int dx = -r; dx <= r; ++dx) {
                    if (dx == 0 && dz == 0 && dy <= 0) continue;
                    if (std::abs(dx) == r && std::abs(dz) == r && r > 1) continue;
                    const int yy = top + dy;
                    if (yy < 1 || yy >= maxY) continue;
                    if (GetVoxel(x + dx, yy, z + dz) == 0) SetVoxel(x + dx, yy, z + dz, kLeaves);
                }
            }
        }
    }

    MarkAllDirty();
    RecomputeBounds();
    ENG_LOGI("voxel", "terrain: %d chunks (%dx%d), seed=%llu", ChunkCount(), chunksX, chunksZ,
             static_cast<unsigned long long>(seed));
}

void VoxelWorld::GenerateIsland(int radiusChunks, u64 seed) {
    if (radiusChunks < 1) radiusChunks = 1;
    if (radiusChunks > 12) radiusChunks = 12;
    const int R = radiusChunks * kS;
    const int dim = R * 2;
    const f32 sea = 8.0f;
    const f32 islandR = static_cast<f32>(R) * 0.86f;
    const f32 hillAmp = 21.0f;
    const int yMax = 44;

    std::vector<i16> heights(static_cast<usize>(dim) * static_cast<usize>(dim), 0);
    auto at = [&](int x, int z) -> i16& {
        return heights[(static_cast<usize>(z + R) * static_cast<usize>(dim)) +
                       static_cast<usize>(x + R)];
    };
    const f32 pondX = islandR * 0.58f, pondZ = islandR * 0.26f, pondR = islandR * 0.17f;

    for (int z = -R; z < R; ++z) {
        for (int x = -R; x < R; ++x) {
            const f32 fx = static_cast<f32>(x), fz = static_cast<f32>(z);
            const f32 d = std::sqrt(fx * fx + fz * fz);
            const f32 t = Clamp(d / islandR, 0.0f, 1.0f);
            f32 hill = hillAmp * std::pow(1.0f - SmoothStep(0.0f, 1.0f, t), 1.35f);
            if (d > islandR) {
                const f32 k =
                    Clamp((d - islandR) / std::max(1.0f, static_cast<f32>(R) - islandR), 0.0f, 1.0f);
                hill = -2.5f - 3.5f * k;  // шельф, уходящий в океан
            }
            const f32 n = (Fbm2(fx * 0.055f, fz * 0.055f, seed ^ 0x51EDu, 4) * 2.0f - 1.0f) * 1.8f;
            f32 hf = sea - 1.2f + hill + n;
            const f32 pdx = fx - pondX, pdz = fz - pondZ;
            const f32 pd = std::sqrt(pdx * pdx + pdz * pdz);
            if (pd < pondR) {
                const f32 k = 1.0f - pd / pondR;
                hf -= 6.0f * k * k * (3.0f - 2.0f * k);
            }
            at(x, z) = static_cast<i16>(Clamp(FloorToInt(hf), 0, yMax - 10));
        }
    }

    ChunkWriter w(this);
    for (int z = -R; z < R; ++z) {
        for (int x = -R; x < R; ++x) {
            const int h = at(x, z);
            const bool beach = h <= static_cast<int>(sea) + 1;
            for (int y = 0; y <= h; ++y) {
                u8 id;
                if (y == h) {
                    id = beach ? kSand : kGrass;
                } else if (y > h - 4) {
                    id = beach ? kSand : kDirt;
                } else {
                    id = kStone;
                }
                w.Set(x, y, z, id);
            }
            for (int y = h + 1; y <= static_cast<int>(sea); ++y) w.Set(x, y, z, kWater);
        }
    }

    Random rng(seed * 0x9E3779B97F4A7C15ULL + 17ULL);

    // Небольшие пещеры со светокамнем, чтобы интерьер был виден в демо.
    const int caveCount = std::max(2, static_cast<int>(static_cast<f32>(R) * 0.12f));
    for (int i = 0; i < caveCount; ++i) {
        const int cx = rng.RangeInt(-R / 2, R / 2);
        const int cz = rng.RangeInt(-R / 2, R / 2);
        const int cy = rng.RangeInt(6, 16);
        const int cr = rng.RangeInt(3, 5);
        for (int z = cz - cr; z <= cz + cr; ++z) {
            for (int y = cy - cr; y <= cy + cr; ++y) {
                for (int x = cx - cr; x <= cx + cr; ++x) {
                    if (x < -R || x >= R || z < -R || z >= R || y < 2 || y >= yMax) continue;
                    const int surf = at(Clamp(x, -R, R - 1), Clamp(z, -R, R - 1));
                    if (y > surf - 5) continue;  // поверхность никогда не пробиваем
                    const f32 dx = static_cast<f32>(x - cx), dy = static_cast<f32>(y - cy),
                              dz = static_cast<f32>(z - cz);
                    if (dx * dx + dy * dy + dz * dz > static_cast<f32>(cr * cr)) continue;
                    if (GetVoxel(x, y, z) == kStone) SetVoxel(x, y, z, kAir);
                }
            }
        }
        for (int k = 0; k < 3; ++k) {
            const int gx = cx + rng.RangeInt(-1, 1), gz = cz + rng.RangeInt(-1, 1);
            const int gy = cy - cr + 1;
            if (gy > 2 && GetVoxel(gx, gy, gz) == kAir) SetVoxel(gx, gy, gz, kGlowstone);
        }
    }

    // Рудные жилы в каменном ядре.
    const int veinCount = std::max(6, static_cast<int>(static_cast<f32>(R) * 0.35f));
    for (int i = 0; i < veinCount; ++i) {
        const int x = rng.RangeInt(-R + 2, R - 3);
        const int z = rng.RangeInt(-R + 2, R - 3);
        const int y = rng.RangeInt(2, 12);
        const u8 ore = rng.Chance(0.4f) ? kGold : kMetal;
        const int count = rng.RangeInt(4, 9);
        for (int k = 0; k < count; ++k) {
            const int ox = x + rng.RangeInt(-2, 2), oy = y + rng.RangeInt(-1, 1),
                      oz = z + rng.RangeInt(-2, 2);
            if (oy < 1 || oy >= yMax) continue;
            if (GetVoxel(ox, oy, oz) == kStone) SetVoxel(ox, oy, oz, ore);
        }
    }

    // Деревья на верхней части острова.
    const int treeAttempts = std::max(8, static_cast<int>(islandR * islandR * 0.012f));
    for (int i = 0; i < treeAttempts; ++i) {
        const int lim = std::max(2, static_cast<int>(islandR * 0.72f));
        const int x = rng.RangeInt(-lim, lim);
        const int z = rng.RangeInt(-lim, lim);
        const int h = at(x, z);
        if (h < static_cast<int>(sea) + 3) continue;
        bool flat = true;
        for (int dz = -1; dz <= 1 && flat; ++dz) {
            for (int dx = -1; dx <= 1; ++dx) {
                if (std::abs(at(x + dx, z + dz) - h) > 1) {
                    flat = false;
                    break;
                }
            }
        }
        if (!flat) continue;
        if (GetVoxel(x, h + 1, z) != kAir) continue;
        const int trunk = rng.RangeInt(4, 6);
        for (int y = 1; y <= trunk; ++y) {
            if (h + y < yMax) SetVoxel(x, h + y, z, kWood);
        }
        const int top = std::min(h + trunk, yMax - 3);
        for (int dy = -2; dy <= 1; ++dy) {
            const int r = (dy >= 0) ? 1 : 2;
            for (int dz = -r; dz <= r; ++dz) {
                for (int dx = -r; dx <= r; ++dx) {
                    if (dx == 0 && dz == 0 && dy <= 0) continue;
                    if (std::abs(dx) == r && std::abs(dz) == r && r > 1) continue;
                    const int yy = top + dy;
                    if (yy < 1 || yy >= yMax) continue;
                    if (GetVoxel(x + dx, yy, z + dz) == kAir) SetVoxel(x + dx, yy, z + dz, kLeaves);
                }
            }
        }
    }

    MarkAllDirty();
    RecomputeBounds();
    ENG_LOGI("voxel", "island: radius=%d chunks, %d chunks total", radiusChunks, ChunkCount());
}

}  // namespace crossrender
