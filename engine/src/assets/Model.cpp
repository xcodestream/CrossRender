// Импортёры моделей: Wavefront OBJ/MTL, glTF 2.0 (.gltf/.glb), PLY (ascii+binary),
// STL (ascii+binary) и MagicaVoxel .vox, плюс контейнер модели времени выполнения
// и набор процедурных low-poly объектов.
//
// Все парсеры защитные: каждый диапазон buffer/view/accessor проверяется до
// чтения, исключения не бросаются, а сбои сообщаются через выходной параметр
// `error`.
#include <vector>  // Math.h использует std::vector раньше, чем подключает его сам.

#include <array>
#include <cmath>
#include <cctype>
#include <string>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <algorithm>
#include <unordered_map>

#include "crossrender/core/Log.h"
#include "crossrender/core/File.h"
#include "crossrender/core/Json.h"
#include "crossrender/assets/Model.h"

namespace crossrender {
namespace {

// ---------------------------------------------------------------------------
// Текстовые хелперы
// ---------------------------------------------------------------------------
inline bool IsSpace(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
}

// Токенизатор по пробельным символам над диапазоном байтов.
struct Tokenizer {
    const char* p = nullptr;
    const char* end = nullptr;
    Tokenizer() = default;
    Tokenizer(const char* begin, usize size) : p(begin), end(begin ? begin + size : nullptr) {}
    bool Next(std::string* out) {
        if (!p || !end) return false;
        while (p < end && IsSpace(*p)) ++p;
        if (p >= end) return false;
        const char* s = p;
        while (p < end && !IsSpace(*p)) ++p;
        out->assign(s, static_cast<usize>(p - s));
        return true;
    }
};

bool ParseF32(const std::string& s, f32* out) {
    if (s.empty()) return false;
    char* endp = nullptr;
    const double v = std::strtod(s.c_str(), &endp);
    if (endp == s.c_str() || (endp && *endp != '\0') || !std::isfinite(v)) return false;
    if (out) *out = static_cast<f32>(v);
    return true;
}

bool ParseI64(const std::string& s, i64* out) {
    if (s.empty()) return false;
    char* endp = nullptr;
    const long long v = std::strtoll(s.c_str(), &endp, 10);
    if (endp == s.c_str() || (endp && *endp != '\0')) return false;
    if (out) *out = static_cast<i64>(v);
    return true;
}

// ---------------------------------------------------------------------------
// Base64 / percent-декодирование
// ---------------------------------------------------------------------------
int Base64Value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

bool Base64Decode(const char* data, usize size, std::vector<u8>* out) {
    if (!out) return false;
    out->clear();
    if (!data) return size == 0;
    out->reserve(size / 4 * 3 + 3);
    u32 acc = 0;
    int bits = 0;
    for (usize i = 0; i < size; ++i) {
        const char c = data[i];
        if (c == '=') break;
        if (IsSpace(c)) continue;
        const int v = Base64Value(c);
        if (v < 0) return false;
        acc = (acc << 6) | static_cast<u32>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out->push_back(static_cast<u8>((acc >> bits) & 0xFFu));
        }
    }
    return true;
}

std::string PercentDecode(const std::string& s) {
    auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::string out;
    out.reserve(s.size());
    for (usize i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            const int hi = hex(s[i + 1]), lo = hex(s[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>(hi * 16 + lo));
                i += 2;
                continue;
            }
        }
        out.push_back(s[i]);
    }
    return out;
}

// ---------------------------------------------------------------------------
// Бинарные читатели (байты собираются, никогда не переинтерпретируются хостом)
// ---------------------------------------------------------------------------
inline u16 ReadU16LE(const u8* p) { return static_cast<u16>(p[0] | (p[1] << 8)); }
inline u16 ReadU16BE(const u8* p) { return static_cast<u16>((p[0] << 8) | p[1]); }
inline u32 ReadU32LE(const u8* p) {
    return static_cast<u32>(p[0]) | (static_cast<u32>(p[1]) << 8) | (static_cast<u32>(p[2]) << 16) |
           (static_cast<u32>(p[3]) << 24);
}
inline u32 ReadU32BE(const u8* p) {
    return (static_cast<u32>(p[0]) << 24) | (static_cast<u32>(p[1]) << 16) | (static_cast<u32>(p[2]) << 8) |
           static_cast<u32>(p[3]);
}
inline u64 ReadU64LE(const u8* p) {
    u64 v = 0;
    for (int i = 7; i >= 0; --i) v = (v << 8) | p[i];
    return v;
}
inline u64 ReadU64BE(const u8* p) {
    u64 v = 0;
    for (int i = 0; i < 8; ++i) v = (v << 8) | p[i];
    return v;
}
inline f32 ReadF32LE(const u8* p) {
    const u32 bits = ReadU32LE(p);
    f32 v = 0;
    std::memcpy(&v, &bits, sizeof(v));
    return v;
}
inline f32 ReadF32BE(const u8* p) {
    const u32 bits = ReadU32BE(p);
    f32 v = 0;
    std::memcpy(&v, &bits, sizeof(v));
    return v;
}

// ---------------------------------------------------------------------------
// Определение формата
// ---------------------------------------------------------------------------
ModelFormat FormatFromExtension(const std::string& path) {
    const std::string ext = PathExt(path);  // в нижнем регистре, с точкой
    if (ext == ".obj") return ModelFormat::Obj;
    if (ext == ".gltf") return ModelFormat::Gltf;
    if (ext == ".glb") return ModelFormat::Glb;
    if (ext == ".ply") return ModelFormat::Ply;
    if (ext == ".stl") return ModelFormat::Stl;
    if (ext == ".vox") return ModelFormat::Vox;
    if (ext == ".fbx") return ModelFormat::FbxLite;
    return ModelFormat::Unknown;
}

bool LooksLikeBinaryStl(const u8* d, usize n) {
    if (!d || n < 84) return false;
    const u32 count = ReadU32LE(d + 80);
    if (count == 0 || count > 100000000u) return false;
    return static_cast<usize>(84) + static_cast<usize>(count) * 50u == n;
}

ModelFormat FormatFromMagic(const u8* d, usize n) {
    if (!d || n < 3) return ModelFormat::Unknown;
    if (n >= 4 && std::memcmp(d, "glTF", 4) == 0) return ModelFormat::Glb;
    if (n >= 4 && std::memcmp(d, "VOX ", 4) == 0) return ModelFormat::Vox;
    if (std::memcmp(d, "ply", 3) == 0 && (n == 3 || IsSpace(static_cast<char>(d[3])))) return ModelFormat::Ply;
    if (LooksLikeBinaryStl(d, n)) return ModelFormat::Stl;

    usize i = 0;
    if (n >= 3 && d[0] == 0xEF && d[1] == 0xBB && d[2] == 0xBF) i = 3;  // UTF-8 BOM
    while (i < n && IsSpace(static_cast<char>(d[i]))) ++i;
    if (i < n && d[i] == '{') return ModelFormat::Gltf;
    if (n >= i + 5 && std::memcmp(d + i, "solid", 5) == 0) return ModelFormat::Stl;

    // OBJ: первая содержательная строка должна начинаться с известного ключевого слова Wavefront.
    static const char* kObjKeywords[] = {"v",  "vt", "vn", "vp", "f",      "l",
                                         "p",  "o",  "g",  "s",  "usemtl", "mtllib"};
    const usize limit = (i + 4096 < n) ? i + 4096 : n;
    usize k = i;
    while (k < limit) {
        while (k < limit && (d[k] == ' ' || d[k] == '\t' || d[k] == '\r')) ++k;
        if (k >= limit) break;
        if (d[k] == '\n') {
            ++k;
            continue;
        }
        if (d[k] == '#') {
            while (k < limit && d[k] != '\n') ++k;
            continue;
        }
        const usize start = k;
        while (k < limit && !IsSpace(static_cast<char>(d[k]))) ++k;
        const std::string tok(reinterpret_cast<const char*>(d) + start, k - start);
        for (const char* kw : kObjKeywords)
            if (tok == kw) return ModelFormat::Obj;
        return ModelFormat::Unknown;
    }
    return ModelFormat::Unknown;
}

ModelFormat DetectFormat(const std::string& path, const u8* d, usize n) {
    const ModelFormat ext = FormatFromExtension(path);
    const ModelFormat magic = FormatFromMagic(d, n);
    // Текстовый и бинарный glTF различимы только по содержимому.
    if (ext == ModelFormat::Gltf && magic == ModelFormat::Glb) return ModelFormat::Glb;
    if (ext == ModelFormat::Glb && magic == ModelFormat::Gltf) return ModelFormat::Gltf;
    if (ext != ModelFormat::Unknown) return ext;
    return magic;
}

// ---------------------------------------------------------------------------
// Хелперы мешей
// ---------------------------------------------------------------------------
void RepairBounds(MeshData* mesh) {
    if (!mesh) return;
    mesh->ComputeBounds();
    if (mesh->bounds.Valid() || mesh->vertices.empty()) return;
    Bounds b;
    for (const Vertex& v : mesh->vertices) b.Expand(v.position);
    mesh->bounds = b;
}

bool HasZeroNormal(const MeshData& mesh) {
    for (const Vertex& v : mesh.vertices)
        if (LengthSq(v.normal) < 1e-12f) return true;
    return false;
}

// Заполняет оставшиеся нулевые нормали (страховочная сетка вокруг MeshData::ComputeNormals).
void RepairZeroNormals(MeshData* mesh, bool smooth) {
    if (!mesh || mesh->vertices.empty() || mesh->indices.size() < 3) return;
    if (!HasZeroNormal(*mesh)) return;
    const usize vcount = mesh->vertices.size();
    std::vector<Vec3> acc(smooth ? vcount : 0);
    for (usize i = 0; i + 2 < mesh->indices.size(); i += 3) {
        const u32 a = mesh->indices[i], b = mesh->indices[i + 1], c = mesh->indices[i + 2];
        if (a >= vcount || b >= vcount || c >= vcount) continue;
        const Vec3 fn = Cross(mesh->vertices[b].position - mesh->vertices[a].position,
                              mesh->vertices[c].position - mesh->vertices[a].position);
        if (smooth) {
            acc[a] += fn;
            acc[b] += fn;
            acc[c] += fn;
        } else {
            const Vec3 nn = Normalize(fn);
            if (LengthSq(nn) > 0.25f) {
                if (LengthSq(mesh->vertices[a].normal) < 1e-12f) mesh->vertices[a].normal = nn;
                if (LengthSq(mesh->vertices[b].normal) < 1e-12f) mesh->vertices[b].normal = nn;
                if (LengthSq(mesh->vertices[c].normal) < 1e-12f) mesh->vertices[c].normal = nn;
            }
        }
    }
    if (!smooth) return;
    for (usize i = 0; i < vcount; ++i) {
        if (LengthSq(mesh->vertices[i].normal) >= 1e-12f) continue;
        const Vec3 n = Normalize(acc[i]);
        mesh->vertices[i].normal = LengthSq(n) > 0.25f ? n : Vec3{0, 1, 0};
    }
}

// Гладкие нормали через MeshData, с локальным откатом, если метод-член — no-op.
void EnsureSmoothNormals(MeshData* mesh) {
    if (!mesh || mesh->vertices.empty()) return;
    mesh->ComputeNormals(true);
    if (mesh->indices.size() < 3) return;
    bool untouched = true;
    for (const Vertex& v : mesh->vertices) {
        if (v.normal.x != 0.0f || v.normal.y != 1.0f || v.normal.z != 0.0f) {
            untouched = false;
            break;
        }
    }
    if (!untouched) return;
    for (Vertex& v : mesh->vertices) v.normal = Vec3{0, 0, 0};
    RepairZeroNormals(mesh, true);
}

// Габариты модели с учётом иерархии узлов (совпадает с тем, как рендереры обходят узлы).
Bounds ComputeNodeAwareBounds(const std::vector<MeshData>& meshes, const std::vector<ModelNode>& nodes) {
    Bounds b;
    if (nodes.empty()) {
        for (const MeshData& m : meshes) b.Expand(m.bounds);
        return b;
    }
    std::vector<int> chain;
    for (usize i = 0; i < nodes.size(); ++i) {
        const ModelNode& node = nodes[i];
        if (node.mesh < 0 || static_cast<usize>(node.mesh) >= meshes.size()) continue;
        const Bounds& mb = meshes[static_cast<usize>(node.mesh)].bounds;
        if (!mb.Valid()) continue;
        chain.clear();
        int cur = static_cast<int>(i);
        int guard = 0;
        while (cur >= 0 && static_cast<usize>(cur) < nodes.size() && guard++ < 512) {
            chain.push_back(cur);
            cur = nodes[static_cast<usize>(cur)].parent;
        }
        Mat4 world = Mat4::Identity();
        for (usize k = chain.size(); k-- > 0;) world = world * nodes[static_cast<usize>(chain[k])].localMatrix;
        for (int c = 0; c < 8; ++c) {
            const Vec3 p{(c & 1) ? mb.max.x : mb.min.x, (c & 2) ? mb.max.y : mb.min.y,
                         (c & 4) ? mb.max.z : mb.min.z};
            b.Expand(world.TransformPoint(p));
        }
    }
    if (!b.Valid()) {
        for (const MeshData& m : meshes) b.Expand(m.bounds);
    }
    return b;
}

// ---------------------------------------------------------------------------
// Wavefront OBJ / MTL
// ---------------------------------------------------------------------------
struct ObjKey {
    i64 v = 0, vt = -1, vn = -1, sk = 0;
    bool operator==(const ObjKey& o) const { return v == o.v && vt == o.vt && vn == o.vn && sk == o.sk; }
};

struct ObjKeyHash {
    usize operator()(const ObjKey& k) const {
        u64 h = 1469598103934665603ULL;
        auto mix = [&h](u64 x) {
            h ^= x;
            h *= 1099511628211ULL;
        };
        mix(static_cast<u64>(k.v));
        mix(static_cast<u64>(k.vt));
        mix(static_cast<u64>(k.vn));
        mix(static_cast<u64>(k.sk));
        return static_cast<usize>(h);
    }
};

struct MtlInfo {
    std::string name;
    Color diffuse{0.8f, 0.8f, 0.8f, 1.0f};
    bool hasDiffuse = false;
    Color specular{0, 0, 0, 1};
    bool hasSpecular = false;
    Color emissive{0, 0, 0, 1};
    bool hasEmissive = false;
    f32 ns = 0;
    bool hasNs = false;
    f32 alpha = 1;
    bool hasAlpha = false;
    std::string mapKd, mapKe, mapBump;
};

std::vector<MtlInfo> ParseMtl(const char* text, usize size) {
    std::vector<MtlInfo> out;
    if (!text || size == 0) return out;
    usize pos = 0;
    while (pos < size) {
        usize end = pos;
        while (end < size && text[end] != '\n') ++end;
        usize len = end - pos;
        if (len > 0 && text[pos + len - 1] == '\r') --len;
        std::string line(text + pos, len);
        pos = end + 1;
        const usize hash = line.find('#');
        if (hash != std::string::npos) line.resize(hash);

        Tokenizer tk(line.data(), line.size());
        std::string kw;
        if (!tk.Next(&kw)) continue;
        if (kw == "newmtl") {
            MtlInfo m;
            tk.Next(&m.name);
            out.push_back(std::move(m));
            continue;
        }
        if (out.empty()) continue;
        MtlInfo& m = out.back();
        if (kw == "Kd" || kw == "Ks" || kw == "Ke") {
            f32 c[3] = {0, 0, 0};
            int n = 0;
            std::string t;
            while (n < 3 && tk.Next(&t)) {
                if (!ParseF32(t, &c[n])) break;
                ++n;
            }
            if (n < 3) continue;
            if (kw == "Kd") {
                m.diffuse = Color(c[0], c[1], c[2], m.diffuse.a);
                m.hasDiffuse = true;
            } else if (kw == "Ks") {
                m.specular = Color(c[0], c[1], c[2], 1.0f);
                m.hasSpecular = true;
            } else {
                m.emissive = Color(c[0], c[1], c[2], 1.0f);
                m.hasEmissive = true;
            }
        } else if (kw == "Ns" || kw == "d" || kw == "Tr") {
            std::string t;
            f32 v = 0;
            if (!tk.Next(&t) || !ParseF32(t, &v)) continue;
            if (kw == "Ns") {
                m.ns = v;
                m.hasNs = true;
            } else {
                m.alpha = (kw == "Tr") ? (1.0f - v) : v;
                m.hasAlpha = true;
            }
        } else if (kw == "map_Kd" || kw == "map_Ke" || kw == "map_Bump" || kw == "map_bump" || kw == "bump" ||
                   kw == "norm") {
            // Опции вроде "-o 1 1" предшествуют имени файла; последний токен — путь.
            std::string t, last;
            while (tk.Next(&t)) last = t;
            if (last.empty()) continue;
            if (kw == "map_Kd") m.mapKd = last;
            else if (kw == "map_Ke") m.mapKe = last;
            else m.mapBump = last;
        }
    }
    return out;
}

std::vector<std::string> ObjMtlLibraries(const char* text, usize size) {
    std::vector<std::string> libs;
    if (!text || size == 0) return libs;
    usize pos = 0;
    while (pos < size) {
        usize end = pos;
        while (end < size && text[end] != '\n') ++end;
        usize len = end - pos;
        if (len > 0 && text[pos + len - 1] == '\r') --len;
        const std::string line(text + pos, len);
        pos = end + 1;
        Tokenizer tk(line.data(), line.size());
        std::string kw, name;
        if (!tk.Next(&kw) || kw != "mtllib") continue;
        while (tk.Next(&name)) libs.push_back(name);
    }
    return libs;
}

void ApplyMtl(const std::vector<MtlInfo>& mtls, std::vector<Material>* mats, const std::string& dir,
              std::vector<Texture>* textures) {
    if (!mats || mtls.empty()) return;
    std::unordered_map<std::string, int> byName;
    for (usize i = 0; i < mtls.size(); ++i) byName[mtls[i].name] = static_cast<int>(i);

    struct TexReq {
        int material = 0;
        std::string path;
        int slot = 0;  // 0 базовый цвет, 1 emissive, 2 normal
    };
    std::vector<TexReq> reqs;

    for (usize mi = 0; mi < mats->size(); ++mi) {
        Material& mat = (*mats)[mi];
        auto it = byName.find(mat.name);
        if (it == byName.end()) continue;
        const MtlInfo& src = mtls[static_cast<usize>(it->second)];
        if (src.hasDiffuse) mat.baseColor = Color(src.diffuse.r, src.diffuse.g, src.diffuse.b, mat.baseColor.a);
        if (src.hasAlpha) mat.baseColor.a = src.alpha;
        if (src.hasNs) {
            const f32 ns = Clamp(src.ns, 0.0f, 1000.0f);
            mat.roughness = Clamp(1.0f - std::sqrt(ns / 1000.0f), 0.04f, 1.0f);
        }
        if (src.hasSpecular) {
            // Цвет блика аппроксимирует metalness для модели metallic-roughness.
            const f32 lum = 0.2126f * src.specular.r + 0.7152f * src.specular.g + 0.0722f * src.specular.b;
            mat.metallic = Clamp(0.5f * lum, 0.0f, 1.0f);
        }
        if (src.hasEmissive) mat.emissive = src.emissive;
        if (mat.baseColor.a < 0.999f) mat.alphaMode = AlphaMode::Blend;
        if (!src.mapKd.empty()) reqs.push_back({static_cast<int>(mi), src.mapKd, 0});
        if (!src.mapKe.empty()) reqs.push_back({static_cast<int>(mi), src.mapKe, 1});
        if (!src.mapBump.empty()) reqs.push_back({static_cast<int>(mi), src.mapBump, 2});
    }
    if (!textures || reqs.empty()) return;

    std::unordered_map<std::string, int> cache;
    for (const TexReq& req : reqs) {
        std::string path = req.path;
        if (path.empty()) continue;
        if (path[0] != '/') path = PathJoin(dir, path);
        auto ci = cache.find(path);
        if (ci == cache.end()) {
            Texture tex;
            if (!tex.LoadFromFile(path)) {
                ENG_LOGW("model", "OBJ: cannot load texture '%s' (no GL context?)", path.c_str());
                cache[path] = -1;
                continue;
            }
            tex.SetDebugName(PathBase(path));
            textures->push_back(std::move(tex));
            cache[path] = static_cast<int>(textures->size()) - 1;
        }
        const int ti = cache[path];
        if (ti < 0 || static_cast<usize>(ti) >= textures->size()) continue;
        const Texture* ptr = &(*textures)[static_cast<usize>(ti)];
        switch (req.slot) {
            case 0: (*mats)[static_cast<usize>(req.material)].baseColorTex = ptr; break;
            case 1: (*mats)[static_cast<usize>(req.material)].emissiveTex = ptr; break;
            default: (*mats)[static_cast<usize>(req.material)].normalTex = ptr; break;
        }
    }
}

bool ImportObjData(const char* text, usize size, MeshData* outMesh, std::vector<Material>* outMaterials,
                   std::string* error) {
    auto Fail = [&](const std::string& msg) {
        if (error) *error = msg;
        ENG_LOGW("model", "OBJ: %s", msg.c_str());
        return false;
    };
    if (!text || size == 0) return Fail("empty OBJ input");
    if (!outMesh) return Fail("no output mesh");

    outMesh->vertices.clear();
    outMesh->indices.clear();
    outMesh->subMeshes.clear();
    outMesh->bounds = Bounds{};
    outMesh->name.clear();
    if (outMaterials) outMaterials->clear();

    std::vector<Vec3> positions;
    std::vector<Vec2> uvs;
    std::vector<Vec3> normals;
    std::unordered_map<ObjKey, u32, ObjKeyHash> vertexMap;

    struct Group {
        std::string name;
        int material = -1;
        u32 first = 0;
    };
    Group group;
    bool groupOpen = false;
    int currentMaterial = -1;  // «липкий»: переживает строки "g"/"o"
    i64 smoothing = 1;
    i64 faceCounter = 0;
    std::string objectName;

    std::vector<Vec3> normalAccum;
    std::vector<char> needsNormal;

    auto FindOrAddMaterial = [&](const std::string& name) -> int {
        if (!outMaterials) return 0;
        for (usize i = 0; i < outMaterials->size(); ++i)
            if (outMaterials->at(i).name == name) return static_cast<int>(i);
        Material m;
        m.name = name;
        outMaterials->push_back(std::move(m));
        return static_cast<int>(outMaterials->size()) - 1;
    };

    auto CloseGroup = [&]() {
        if (!groupOpen) return;
        groupOpen = false;
        const u32 count = static_cast<u32>(outMesh->indices.size()) - group.first;
        if (count == 0) return;
        int mi = group.material;
        if (mi < 0) mi = FindOrAddMaterial("default");
        MeshData::SubMesh sm;
        sm.indexOffset = group.first;
        sm.indexCount = count;
        sm.materialIndex = mi;
        sm.name = group.name;
        outMesh->subMeshes.push_back(std::move(sm));
    };
    // Принимает имя по значению, чтобы вызывающие могли безопасно передать `group.name`.
    auto OpenGroup = [&](std::string name) {
        CloseGroup();
        group = Group{};
        group.name = std::move(name);
        group.material = currentMaterial;  // "usemtl" действует до следующей смены
        group.first = static_cast<u32>(outMesh->indices.size());
        groupOpen = true;
    };

    auto ResolveIndex = [](i64 raw, usize count, i64* out) -> bool {
        if (raw > 0) {
            if (static_cast<usize>(raw) > count) return false;
            *out = raw - 1;
            return true;
        }
        if (raw < 0) {
            if (static_cast<usize>(-raw) > count) return false;
            *out = static_cast<i64>(count) + raw;
            return true;
        }
        return false;
    };

    usize pos = 0;
    while (pos < size) {
        usize end = pos;
        while (end < size && text[end] != '\n') ++end;
        usize len = end - pos;
        if (len > 0 && text[pos + len - 1] == '\r') --len;
        std::string line(text + pos, len);
        pos = end + 1;
        const usize hash = line.find('#');
        if (hash != std::string::npos) line.resize(hash);

        Tokenizer tk(line.data(), line.size());
        std::string kw;
        if (!tk.Next(&kw)) continue;

        if (kw == "v") {
            f32 c[3] = {0, 0, 0};
            int n = 0;
            std::string t;
            while (n < 3 && tk.Next(&t)) {
                if (!ParseF32(t, &c[n])) return Fail("malformed 'v' line");
                ++n;
            }
            if (n < 3) return Fail("'v' line with fewer than 3 coordinates");
            positions.push_back(Vec3{c[0], c[1], c[2]});  // необязательная w игнорируется
        } else if (kw == "vt") {
            f32 u = 0, v = 0;
            std::string t;
            if (!tk.Next(&t) || !ParseF32(t, &u)) return Fail("malformed 'vt' line");
            if (tk.Next(&t) && !ParseF32(t, &v)) v = 0;
            uvs.push_back(Vec2{u, v});
        } else if (kw == "vn") {
            f32 c[3] = {0, 0, 0};
            int n = 0;
            std::string t;
            while (n < 3 && tk.Next(&t)) {
                if (!ParseF32(t, &c[n])) return Fail("malformed 'vn' line");
                ++n;
            }
            if (n < 3) return Fail("'vn' line with fewer than 3 coordinates");
            normals.push_back(Vec3{c[0], c[1], c[2]});
        } else if (kw == "vp") {
            continue;  // вершины параметрического пространства мешу не нужны
        } else if (kw == "g" || kw == "o") {
            std::string name, t;
            while (tk.Next(&t)) {
                if (!name.empty()) name.push_back(' ');
                name += t;
            }
            if (kw == "o" && !name.empty()) objectName = name;
            OpenGroup(name);
        } else if (kw == "usemtl") {
            std::string name, t;
            while (tk.Next(&t)) {
                if (!name.empty()) name.push_back(' ');
                name += t;
            }
            currentMaterial = FindOrAddMaterial(name);
            if (!groupOpen || outMesh->indices.size() > group.first) OpenGroup(group.name);
            group.material = currentMaterial;
        } else if (kw == "s") {
            std::string t;
            if (!tk.Next(&t)) {
                smoothing = 1;
            } else if (t == "off" || t == "0") {
                smoothing = 0;
            } else if (t == "on") {
                smoothing = 1;
            } else {
                i64 v = 1;
                smoothing = ParseI64(t, &v) ? v : 1;
            }
        } else if (kw == "f") {
            if (!groupOpen) OpenGroup(group.name);
            struct Ref {
                i64 v = 0, vt = -1, vn = -1;
            };
            std::vector<Ref> refs;
            std::string tok;
            while (tk.Next(&tok)) {
                // Разбираем "v", "v/vt", "v//vn" или "v/vt/vn".
                i64 fields[3] = {0, -1, -1};
                std::string part;
                int field = 0;
                for (usize ci = 0; ci <= tok.size() && field < 3; ++ci) {
                    if (ci == tok.size() || tok[ci] == '/') {
                        if (field == 0) {
                            if (part.empty() || !ParseI64(part, &fields[0]))
                                return Fail("malformed face vertex '" + tok + "'");
                        } else if (!part.empty()) {
                            if (!ParseI64(part, &fields[field])) return Fail("malformed face vertex '" + tok + "'");
                        }
                        part.clear();
                        ++field;
                    } else {
                        part.push_back(tok[ci]);
                    }
                }
                Ref r;
                if (!ResolveIndex(fields[0], positions.size(), &r.v))
                    return Fail("face position index out of range in '" + tok + "'");
                if (fields[1] != -1 && !ResolveIndex(fields[1], uvs.size(), &r.vt))
                    return Fail("face texture index out of range in '" + tok + "'");
                if (fields[2] != -1 && !ResolveIndex(fields[2], normals.size(), &r.vn))
                    return Fail("face normal index out of range in '" + tok + "'");
                refs.push_back(r);
            }
            if (refs.size() < 3) return Fail("face with fewer than 3 vertices");
            // Триангуляция веером (обрабатывает полигоны более чем с 3 вершинами).
            for (usize k = 1; k + 1 < refs.size(); ++k) {
                const Ref tri[3] = {refs[0], refs[k], refs[k + 1]};
                u32 idx[3] = {0, 0, 0};
                for (int e = 0; e < 3; ++e) {
                    const Ref& r = tri[e];
                    ObjKey key;
                    key.v = r.v;
                    key.vt = r.vt;
                    key.vn = r.vn;
                    // Вершины без явной нормали свариваются по группам сглаживания;
                    // smoothing "off" даёт каждой грани собственные вершины.
                    key.sk = (r.vn >= 0) ? 0 : (smoothing > 0 ? smoothing : -(1 + faceCounter));
                    auto it = vertexMap.find(key);
                    if (it != vertexMap.end()) {
                        idx[e] = it->second;
                        continue;
                    }
                    Vertex vtx;
                    vtx.position = positions[static_cast<usize>(r.v)];
                    if (r.vn >= 0) vtx.normal = Normalize(normals[static_cast<usize>(r.vn)]);
                    if (r.vt >= 0) vtx.uv = uvs[static_cast<usize>(r.vt)];
                    const u32 index = static_cast<u32>(outMesh->vertices.size());
                    if (index == 0xFFFFFFFFu) return Fail("too many vertices");
                    outMesh->vertices.push_back(vtx);
                    normalAccum.push_back(Vec3{0, 0, 0});
                    needsNormal.push_back(LengthSq(vtx.normal) < 1e-12f ? 1 : 0);
                    vertexMap.emplace(key, index);
                    idx[e] = index;
                }
                const Vec3& pa = outMesh->vertices[idx[0]].position;
                const Vec3& pb = outMesh->vertices[idx[1]].position;
                const Vec3& pc = outMesh->vertices[idx[2]].position;
                const Vec3 fn = Cross(pb - pa, pc - pa);
                for (int e = 0; e < 3; ++e)
                    if (needsNormal[idx[e]]) normalAccum[idx[e]] += fn;
                outMesh->indices.push_back(idx[0]);
                outMesh->indices.push_back(idx[1]);
                outMesh->indices.push_back(idx[2]);
            }
            ++faceCounter;
        }
        // "mtllib", "l", "p" и неизвестные ключевые слова здесь игнорируются (MTL обрабатывает Load).
    }

    if (positions.empty()) return Fail("OBJ contains no vertex positions");

    for (usize i = 0; i < outMesh->vertices.size(); ++i) {
        if (!needsNormal[i]) continue;
        const Vec3 n = Normalize(normalAccum[i]);
        outMesh->vertices[i].normal = LengthSq(n) > 0.25f ? n : Vec3{0, 1, 0};
    }
    CloseGroup();

    if (!objectName.empty()) outMesh->name = objectName;
    RepairBounds(outMesh);
    return true;
}

// ---------------------------------------------------------------------------
// glTF 2.0
// ---------------------------------------------------------------------------
std::string g_gltfBaseDir;  // задаётся на время импорта, чтобы разрешались внешние URI

struct GltfBufferView {
    int buffer = -1;
    usize byteOffset = 0;
    usize byteLength = 0;
    usize byteStride = 0;
};

struct GltfAccessor {
    bool valid = false;
    int bufferView = -1;
    usize byteOffset = 0;
    int componentType = 0;
    usize count = 0;
    int components = 0;
    bool normalized = false;
    bool sparse = false;
};

int ComponentByteSize(int ct) {
    switch (ct) {
        case 5120:  // BYTE
        case 5121:  // UNSIGNED_BYTE
            return 1;
        case 5122:  // SHORT
        case 5123:  // UNSIGNED_SHORT
            return 2;
        case 5125:  // UNSIGNED_INT
        case 5126:  // FLOAT
            return 4;
        default:
            return 0;
    }
}

int TypeComponents(const std::string& t) {
    if (t == "SCALAR") return 1;
    if (t == "VEC2") return 2;
    if (t == "VEC3") return 3;
    if (t == "VEC4") return 4;
    if (t == "MAT2") return 4;
    if (t == "MAT3") return 9;
    if (t == "MAT4") return 16;
    return 0;
}

f32 ReadAccessorComponent(const u8* p, int ct, bool normalized) {
    switch (ct) {
        case 5120: {
            i8 v = 0;
            std::memcpy(&v, p, 1);
            return normalized ? std::max(static_cast<f32>(v) / 127.0f, -1.0f) : static_cast<f32>(v);
        }
        case 5121:
            return normalized ? static_cast<f32>(*p) / 255.0f : static_cast<f32>(*p);
        case 5122: {
            i16 v = 0;
            std::memcpy(&v, p, 2);
            return normalized ? std::max(static_cast<f32>(v) / 32767.0f, -1.0f) : static_cast<f32>(v);
        }
        case 5123: {
            u16 v = 0;
            std::memcpy(&v, p, 2);
            return normalized ? static_cast<f32>(v) / 65535.0f : static_cast<f32>(v);
        }
        case 5125: {
            u32 v = 0;
            std::memcpy(&v, p, 4);
            return normalized ? static_cast<f32>(static_cast<f64>(v) / 4294967295.0) : static_cast<f32>(v);
        }
        case 5126: {
            f32 v = 0;
            std::memcpy(&v, p, 4);
            return v;
        }
        default:
            return 0.0f;
    }
}

// Читает accessor как float; каждый диапазон проверяется заранее.
bool ReadAccessorFloats(const std::vector<std::vector<u8>>& buffers, const std::vector<GltfBufferView>& views,
                        const GltfAccessor& acc, std::vector<f32>* out, std::string* error) {
    if (out) out->clear();
    if (!acc.valid || acc.components <= 0 || acc.count == 0) {
        if (error) *error = "invalid accessor";
        return false;
    }
    if (acc.bufferView < 0 || static_cast<usize>(acc.bufferView) >= views.size()) {
        if (error) *error = "accessor without a valid bufferView";
        return false;
    }
    const GltfBufferView& view = views[static_cast<usize>(acc.bufferView)];
    if (view.buffer < 0 || static_cast<usize>(view.buffer) >= buffers.size()) {
        if (error) *error = "bufferView without a valid buffer";
        return false;
    }
    const std::vector<u8>& buf = buffers[static_cast<usize>(view.buffer)];
    const int cs = ComponentByteSize(acc.componentType);
    if (cs == 0) {
        if (error) *error = "unsupported component type";
        return false;
    }
    const usize elem = static_cast<usize>(cs) * static_cast<usize>(acc.components);
    const usize stride = view.byteStride ? view.byteStride : elem;
    if (stride < elem) {
        if (error) *error = "bufferView stride smaller than the element size";
        return false;
    }
    const usize start = view.byteOffset + acc.byteOffset;
    const usize span = (acc.count - 1) * stride + elem;
    if (acc.byteOffset + span > view.byteLength || start + span > buf.size()) {
        if (error) *error = "accessor reads outside its buffer";
        return false;
    }
    out->resize(acc.count * static_cast<usize>(acc.components));
    for (usize i = 0; i < acc.count; ++i) {
        const u8* base = buf.data() + start + i * stride;
        for (int c = 0; c < acc.components; ++c)
            (*out)[i * static_cast<usize>(acc.components) + static_cast<usize>(c)] =
                ReadAccessorComponent(base + static_cast<usize>(c) * static_cast<usize>(cs), acc.componentType,
                                      acc.normalized);
    }
    return true;
}

bool ReadAccessorIndices(const std::vector<std::vector<u8>>& buffers, const std::vector<GltfBufferView>& views,
                         const GltfAccessor& acc, std::vector<u32>* out, std::string* error) {
    if (out) out->clear();
    if (acc.componentType != 5120 && acc.componentType != 5121 && acc.componentType != 5122 &&
        acc.componentType != 5123 && acc.componentType != 5125) {
        if (error) *error = "index accessor with a non-integer component type";
        return false;
    }
    std::vector<f32> tmp;
    if (!ReadAccessorFloats(buffers, views, acc, &tmp, error)) return false;
    out->resize(tmp.size());
    for (usize i = 0; i < tmp.size(); ++i) {
        const f32 v = tmp[i];
        (*out)[i] = v <= 0.0f ? 0u : static_cast<u32>(v + 0.5f);
    }
    return true;
}

bool ParseGlbChunks(const u8* d, usize n, std::string* json, std::vector<u8>* bin, std::string* error) {
    if (n < 12) {
        if (error) *error = "GLB header truncated";
        return false;
    }
    if (std::memcmp(d, "glTF", 4) != 0) {
        if (error) *error = "bad GLB magic";
        return false;
    }
    if (ReadU32LE(d + 4) != 2) {
        if (error) *error = "unsupported GLB version";
        return false;
    }
    const u32 declared = ReadU32LE(d + 8);
    const usize limit = (declared > 0 && static_cast<usize>(declared) <= n) ? static_cast<usize>(declared) : n;
    usize off = 12;
    bool haveJson = false;
    while (off + 8 <= limit) {
        const u32 chunkLen = ReadU32LE(d + off);
        const u32 chunkType = ReadU32LE(d + off + 4);
        off += 8;
        if (static_cast<usize>(chunkLen) > limit - off) {
            if (error) *error = "truncated GLB chunk";
            return false;
        }
        if (chunkType == 0x4E4F534Au) {  // "JSON"
            json->assign(reinterpret_cast<const char*>(d) + off, chunkLen);
            haveJson = true;
        } else if (chunkType == 0x004E4942u) {  // "BIN\0"
            bin->assign(d + off, d + off + chunkLen);
        }
        off += chunkLen;
    }
    if (!haveJson) {
        if (error) *error = "GLB without a JSON chunk";
        return false;
    }
    return true;
}

// Гладкие нормали для примитива без атрибута NORMAL.
void ComputePrimitiveNormals(MeshData* mesh, usize vstart, usize vcount, usize istart, usize icount) {
    if (!mesh || vcount == 0 || icount < 3) return;
    std::vector<Vec3> acc(vcount);
    for (usize i = istart; i + 2 < istart + icount; i += 3) {
        const u32 a = mesh->indices[i], b = mesh->indices[i + 1], c = mesh->indices[i + 2];
        if (a < vstart || b < vstart || c < vstart) continue;
        const usize la = a - vstart, lb = b - vstart, lc = c - vstart;
        if (la >= vcount || lb >= vcount || lc >= vcount) continue;
        const Vec3 fn = Cross(mesh->vertices[b].position - mesh->vertices[a].position,
                              mesh->vertices[c].position - mesh->vertices[a].position);
        acc[la] += fn;
        acc[lb] += fn;
        acc[lc] += fn;
    }
    for (usize i = 0; i < vcount; ++i) {
        const Vec3 n = Normalize(acc[i]);
        mesh->vertices[vstart + i].normal = LengthSq(n) > 0.25f ? n : Vec3{0, 1, 0};
    }
}

// Касательные для примитива с нормалями и UV, но без атрибута TANGENT.
void ComputePrimitiveTangents(MeshData* mesh, usize vstart, usize vcount, usize istart, usize icount) {
    if (!mesh || vcount == 0 || icount < 3) return;
    std::vector<Vec3> tan(vcount), bitan(vcount);
    for (usize i = istart; i + 2 < istart + icount; i += 3) {
        const u32 a = mesh->indices[i], b = mesh->indices[i + 1], c = mesh->indices[i + 2];
        if (a < vstart || b < vstart || c < vstart) continue;
        const usize la = a - vstart, lb = b - vstart, lc = c - vstart;
        if (la >= vcount || lb >= vcount || lc >= vcount) continue;
        const Vertex& v0 = mesh->vertices[a];
        const Vertex& v1 = mesh->vertices[b];
        const Vertex& v2 = mesh->vertices[c];
        const Vec3 e1 = v1.position - v0.position;
        const Vec3 e2 = v2.position - v0.position;
        const Vec2 d1 = v1.uv - v0.uv;
        const Vec2 d2 = v2.uv - v0.uv;
        const f32 det = d1.x * d2.y - d2.x * d1.y;
        if (std::fabs(det) < 1e-8f) continue;
        const f32 r = 1.0f / det;
        const Vec3 t = (e1 * d2.y - e2 * d1.y) * r;
        const Vec3 btn = (e2 * d1.x - e1 * d2.x) * r;
        tan[la] += t;
        tan[lb] += t;
        tan[lc] += t;
        bitan[la] += btn;
        bitan[lb] += btn;
        bitan[lc] += btn;
    }
    for (usize i = 0; i < vcount; ++i) {
        Vertex& v = mesh->vertices[vstart + i];
        const Vec3 n = v.normal;
        Vec3 t = tan[i] - n * Dot(n, tan[i]);
        if (LengthSq(t) < 1e-12f) {
            v.tangent = Vec4{1, 0, 0, 1};
            continue;
        }
        t = Normalize(t);
        const f32 w = (Dot(Cross(n, t), bitan[i]) < 0.0f) ? -1.0f : 1.0f;
        v.tangent = Vec4{t, w};
    }
}

bool ImportGltfInternal(const void* data, usize size, bool binary, std::vector<MeshData>* outMeshes,
                        std::vector<Material>* outMaterials, std::vector<ModelNode>* outNodes,
                        std::vector<Texture>* outTextures, std::string* error) {
    auto Fail = [&](const std::string& msg) {
        if (error) *error = msg;
        ENG_LOGE("model", "glTF: %s", msg.c_str());
        return false;
    };
    auto Warn = [&](const std::string& msg) { ENG_LOGW("model", "glTF: %s", msg.c_str()); };

    if (!data || size == 0) return Fail("empty input");
    if (!outMeshes || !outMaterials || !outNodes || !outTextures) return Fail("null output");

    std::string jsonText;
    std::vector<u8> binChunk;
    if (binary && size >= 4 && std::memcmp(data, "glTF", 4) == 0) {
        std::string err;
        if (!ParseGlbChunks(static_cast<const u8*>(data), size, &jsonText, &binChunk, &err)) return Fail(err);
    } else {
        const char* p = static_cast<const char*>(data);
        usize off = 0;
        if (size >= 3 && static_cast<u8>(p[0]) == 0xEF && static_cast<u8>(p[1]) == 0xBB &&
            static_cast<u8>(p[2]) == 0xBF)
            off = 3;
        jsonText.assign(p + off, size - off);
    }

    std::string jerr;
    JsonValue root = JsonValue::Parse(jsonText, &jerr);
    if (!root.IsObject()) return Fail("invalid glTF JSON" + (jerr.empty() ? std::string() : (": " + jerr)));

    outMeshes->clear();
    outMaterials->clear();
    outNodes->clear();
    outTextures->clear();

    // ---- buffers ---------------------------------------------------------
    std::vector<std::vector<u8>> buffers;
    if (const JsonValue* jb = root.Find("buffers"); jb && jb->IsArray()) {
        for (usize i = 0; i < jb->Size(); ++i) {
            const JsonValue& j = (*jb)[i];
            const std::string uri = j.GetString("uri");
            const i32 byteLength = j.GetInt("byteLength", 0);
            std::vector<u8> buf;
            if (uri.empty()) {
                if (!binChunk.empty()) buf = binChunk;
                else Warn("buffer " + std::to_string(i) + " has no uri and there is no BIN chunk");
            } else if (uri.rfind("data:", 0) == 0) {
                const usize comma = uri.find(',');
                if (comma == std::string::npos) {
                    Warn("malformed data URI in buffer " + std::to_string(i));
                } else if (uri.substr(5, comma - 5).find("base64") != std::string::npos) {
                    if (!Base64Decode(uri.data() + comma + 1, uri.size() - comma - 1, &buf))
                        Warn("invalid base64 buffer " + std::to_string(i));
                } else {
                    const std::string raw = PercentDecode(uri.substr(comma + 1));
                    buf.assign(raw.begin(), raw.end());
                }
            } else {
                std::string path = PercentDecode(uri);
                if (!g_gltfBaseDir.empty() && path[0] != '/') path = PathJoin(g_gltfBaseDir, path);
                buf = ReadBinaryFile(path);
                if (buf.empty()) Warn("external buffer not found: " + path);
            }
            if (byteLength > 0 && buf.size() > static_cast<usize>(byteLength))
                buf.resize(static_cast<usize>(byteLength));
            buffers.push_back(std::move(buf));
        }
    }

    // ---- bufferViews -----------------------------------------------------
    std::vector<GltfBufferView> views;
    if (const JsonValue* jv = root.Find("bufferViews"); jv && jv->IsArray()) {
        for (usize i = 0; i < jv->Size(); ++i) {
            const JsonValue& j = (*jv)[i];
            GltfBufferView bv;
            bv.buffer = j.GetInt("buffer", -1);
            bv.byteOffset = static_cast<usize>(std::max(0, j.GetInt("byteOffset", 0)));
            bv.byteLength = static_cast<usize>(std::max(0, j.GetInt("byteLength", 0)));
            bv.byteStride = static_cast<usize>(std::max(0, j.GetInt("byteStride", 0)));
            views.push_back(bv);
        }
    }

    // ---- accessors -------------------------------------------------------
    std::vector<GltfAccessor> accessors;
    if (const JsonValue* ja = root.Find("accessors"); ja && ja->IsArray()) {
        for (usize i = 0; i < ja->Size(); ++i) {
            const JsonValue& j = (*ja)[i];
            GltfAccessor a;
            a.bufferView = j.GetInt("bufferView", -1);
            a.byteOffset = static_cast<usize>(std::max(0, j.GetInt("byteOffset", 0)));
            a.componentType = j.GetInt("componentType", 0);
            a.count = static_cast<usize>(std::max(0, j.GetInt("count", 0)));
            a.components = TypeComponents(j.GetString("type"));
            a.normalized = j.GetBool("normalized", false);
            const JsonValue* sp = j.Find("sparse");
            a.sparse = sp && sp->IsObject();
            a.valid =
                a.components > 0 && a.count > 0 && a.count < 100000000u && ComponentByteSize(a.componentType) != 0;
            if (a.sparse) Warn("sparse accessor " + std::to_string(i) + " is ignored (base data is used)");
            accessors.push_back(a);
        }
    }

    // ---- materials -------------------------------------------------------
    struct MatTexRef {
        int baseColor = -1, normal = -1, metallicRoughness = -1, emissive = -1, occlusion = -1;
    };
    std::vector<MatTexRef> matRefs;
    if (const JsonValue* jm = root.Find("materials"); jm && jm->IsArray()) {
        for (usize i = 0; i < jm->Size(); ++i) {
            const JsonValue& j = (*jm)[i];
            Material m;
            m.name = j.GetString("name", "material" + std::to_string(i));
            m.baseColor = Color(1, 1, 1, 1);
            m.metallic = 1.0f;
            m.roughness = 1.0f;
            MatTexRef ref;
            if (const JsonValue* pbr = j.Find("pbrMetallicRoughness"); pbr && pbr->IsObject()) {
                if (const JsonValue* f = pbr->Find("baseColorFactor"); f && f->IsArray()) {
                    f32 c[4] = {1, 1, 1, 1};
                    for (usize k = 0; k < f->Size() && k < 4; ++k) c[k] = (*f)[k].AsFloat(1.0f);
                    m.baseColor = Color(c[0], c[1], c[2], c[3]);
                }
                m.metallic = pbr->GetFloat("metallicFactor", 1.0f);
                m.roughness = pbr->GetFloat("roughnessFactor", 1.0f);
                if (const JsonValue* t = pbr->Find("baseColorTexture"); t && t->IsObject())
                    ref.baseColor = t->GetInt("index", -1);
                if (const JsonValue* t = pbr->Find("metallicRoughnessTexture"); t && t->IsObject())
                    ref.metallicRoughness = t->GetInt("index", -1);
            }
            if (const JsonValue* e = j.Find("emissiveFactor"); e && e->IsArray()) {
                f32 c[3] = {0, 0, 0};
                for (usize k = 0; k < e->Size() && k < 3; ++k) c[k] = (*e)[k].AsFloat(0.0f);
                m.emissive = Color(c[0], c[1], c[2], 1.0f);
            }
            if (const JsonValue* t = j.Find("normalTexture"); t && t->IsObject()) {
                ref.normal = t->GetInt("index", -1);
                m.normalStrength = t->GetFloat("scale", 1.0f);
            }
            if (const JsonValue* t = j.Find("occlusionTexture"); t && t->IsObject()) {
                ref.occlusion = t->GetInt("index", -1);
                m.occlusionStrength = t->GetFloat("strength", 1.0f);
            }
            if (const JsonValue* t = j.Find("emissiveTexture"); t && t->IsObject())
                ref.emissive = t->GetInt("index", -1);
            const std::string alpha = j.GetString("alphaMode", "OPAQUE");
            if (alpha == "MASK") m.alphaMode = AlphaMode::Mask;
            else if (alpha == "BLEND") m.alphaMode = AlphaMode::Blend;
            m.alphaCutoff = j.GetFloat("alphaCutoff", 0.5f);
            if (j.GetBool("doubleSided", false)) {
                m.doubleSided = true;
                m.cullMode = CullMode::None;
            }
            outMaterials->push_back(m);
            matRefs.push_back(ref);
        }
    }

    // ---- images / samplers / textures ------------------------------------
    struct ImageSrc {
        std::string uri, mime;
        int bufferView = -1;
    };
    std::vector<ImageSrc> images;
    if (const JsonValue* ji = root.Find("images"); ji && ji->IsArray()) {
        for (usize i = 0; i < ji->Size(); ++i) {
            const JsonValue& j = (*ji)[i];
            ImageSrc s;
            s.uri = j.GetString("uri");
            s.mime = j.GetString("mimeType");
            s.bufferView = j.GetInt("bufferView", -1);
            images.push_back(s);
        }
    }
    struct SamplerInfo {
        int wrapS = 10497, wrapT = 10497, minFilter = 9987, magFilter = 9729;
    };
    std::vector<SamplerInfo> samplers;
    if (const JsonValue* js = root.Find("samplers"); js && js->IsArray()) {
        for (usize i = 0; i < js->Size(); ++i) {
            const JsonValue& j = (*js)[i];
            SamplerInfo s;
            s.wrapS = j.GetInt("wrapS", 10497);
            s.wrapT = j.GetInt("wrapT", 10497);
            s.minFilter = j.GetInt("minFilter", 9987);
            s.magFilter = j.GetInt("magFilter", 9729);
            samplers.push_back(s);
        }
    }
    auto wrapOf = [](int v) {
        switch (v) {
            case 33071: return TextureWrap::ClampToEdge;
            case 33648: return TextureWrap::MirroredRepeat;
            default: return TextureWrap::Repeat;
        }
    };
    auto filterOf = [](int v) {
        switch (v) {
            case 9728: return TextureFilter::Nearest;
            case 9729: return TextureFilter::Linear;
            case 9984:
            case 9986: return TextureFilter::NearestMipmapNearest;
            default: return TextureFilter::LinearMipmapLinear;
        }
    };

    std::vector<int> texSource, texSampler;
    if (const JsonValue* jt = root.Find("textures"); jt && jt->IsArray()) {
        for (usize i = 0; i < jt->Size(); ++i) {
            const JsonValue& j = (*jt)[i];
            texSource.push_back(j.GetInt("source", -1));
            texSampler.push_back(j.GetInt("sampler", -1));
        }
    }
    outTextures->resize(texSource.size());
    for (usize i = 0; i < texSource.size(); ++i) {
        Texture& tex = (*outTextures)[i];
        const int src = texSource[i];
        if (src < 0 || static_cast<usize>(src) >= images.size()) continue;
        const ImageSrc& im = images[static_cast<usize>(src)];
        std::vector<u8> bytes;
        bool loaded = false;
        if (!im.uri.empty()) {
            if (im.uri.rfind("data:", 0) == 0) {
                const usize comma = im.uri.find(',');
                if (comma != std::string::npos && im.uri.substr(5, comma - 5).find("base64") != std::string::npos) {
                    if (Base64Decode(im.uri.data() + comma + 1, im.uri.size() - comma - 1, &bytes) && !bytes.empty())
                        loaded = tex.LoadFromMemory(bytes.data(), bytes.size(), true);
                }
            } else {
                std::string path = PercentDecode(im.uri);
                if (!g_gltfBaseDir.empty() && path[0] != '/') path = PathJoin(g_gltfBaseDir, path);
                loaded = tex.LoadFromFile(path, true);
            }
            if (!loaded) Warn("image " + std::to_string(src) + " could not be decoded (no GL context?)");
        } else if (im.bufferView >= 0 && static_cast<usize>(im.bufferView) < views.size()) {
            const GltfBufferView& bv = views[static_cast<usize>(im.bufferView)];
            if (bv.buffer >= 0 && static_cast<usize>(bv.buffer) < buffers.size()) {
                const std::vector<u8>& buf = buffers[static_cast<usize>(bv.buffer)];
                if (bv.byteOffset + bv.byteLength <= buf.size()) {
                    bytes.assign(buf.begin() + static_cast<std::ptrdiff_t>(bv.byteOffset),
                                 buf.begin() + static_cast<std::ptrdiff_t>(bv.byteOffset + bv.byteLength));
                    if (!bytes.empty()) loaded = tex.LoadFromMemory(bytes.data(), bytes.size(), true);
                }
            }
            if (!loaded) Warn("image " + std::to_string(src) + " could not be decoded (no GL context?)");
        }
        const int si = (i < texSampler.size()) ? texSampler[i] : -1;
        if (si >= 0 && static_cast<usize>(si) < samplers.size() && loaded) {
            const SamplerInfo& s = samplers[static_cast<usize>(si)];
            tex.SetWrap(wrapOf(s.wrapS));
            tex.SetFilter(filterOf(s.minFilter));
        }
    }

    // Связываем текстуры материалов теперь, когда вектор текстур финален.
    auto texPtr = [&](int idx) -> const Texture* {
        if (idx < 0 || static_cast<usize>(idx) >= outTextures->size()) return nullptr;
        return &(*outTextures)[static_cast<usize>(idx)];
    };
    for (usize i = 0; i < outMaterials->size() && i < matRefs.size(); ++i) {
        Material& m = (*outMaterials)[i];
        m.baseColorTex = texPtr(matRefs[i].baseColor);
        m.normalTex = texPtr(matRefs[i].normal);
        m.metallicRoughnessTex = texPtr(matRefs[i].metallicRoughness);
        m.emissiveTex = texPtr(matRefs[i].emissive);
        m.occlusionTex = texPtr(matRefs[i].occlusion);
    }

    // ---- meshes ----------------------------------------------------------
    if (const JsonValue* jmesh = root.Find("meshes"); jmesh && jmesh->IsArray()) {
        for (usize mi = 0; mi < jmesh->Size(); ++mi) {
            const JsonValue& jm = (*jmesh)[mi];
            MeshData md;
            md.name = jm.GetString("name", "mesh" + std::to_string(mi));
            const JsonValue* prims = jm.Find("primitives");
            if (prims && prims->IsArray()) {
                for (usize pi = 0; pi < prims->Size(); ++pi) {
                    const JsonValue& jp = (*prims)[pi];
                    const int mode = jp.GetInt("mode", 4);
                    if (mode != 4 && mode != 5 && mode != 6) {
                        Warn("mesh " + std::to_string(mi) + " primitive " + std::to_string(pi) +
                             ": unsupported mode " + std::to_string(mode) + " (skipped)");
                        continue;
                    }
                    const JsonValue* attrs = jp.Find("attributes");
                    if (!attrs || !attrs->IsObject()) {
                        Warn("primitive without attributes (skipped)");
                        continue;
                    }
                    const int posAcc = attrs->GetInt("POSITION", -1);
                    if (posAcc < 0 || static_cast<usize>(posAcc) >= accessors.size()) {
                        Warn("primitive without POSITION (skipped)");
                        continue;
                    }
                    std::string aerr;
                    std::vector<f32> pos;
                    if (!ReadAccessorFloats(buffers, views, accessors[static_cast<usize>(posAcc)], &pos, &aerr) ||
                        accessors[static_cast<usize>(posAcc)].components != 3) {
                        Warn("POSITION accessor unreadable: " + aerr + " (primitive skipped)");
                        continue;
                    }
                    const usize vcount = pos.size() / 3;
                    if (vcount == 0) continue;
                    if (md.vertices.size() + vcount > 0x7FFFFFFFu) {
                        Warn("primitive too large (skipped)");
                        continue;
                    }

                    const int nrmAcc = attrs->GetInt("NORMAL", -1);
                    const int uvAcc = attrs->GetInt("TEXCOORD_0", -1);
                    const int tanAcc = attrs->GetInt("TANGENT", -1);
                    const int colAcc = attrs->GetInt("COLOR_0", -1);
                    std::vector<f32> nrm, uv, tan, col;
                    bool hasNormals = false, hasUV = false, hasTangent = false, hasColor = false;
                    if (nrmAcc >= 0 && static_cast<usize>(nrmAcc) < accessors.size() &&
                        accessors[static_cast<usize>(nrmAcc)].components == 3 &&
                        ReadAccessorFloats(buffers, views, accessors[static_cast<usize>(nrmAcc)], &nrm, &aerr) &&
                        nrm.size() == vcount * 3)
                        hasNormals = true;
                    if (uvAcc >= 0 && static_cast<usize>(uvAcc) < accessors.size() &&
                        accessors[static_cast<usize>(uvAcc)].components == 2 &&
                        ReadAccessorFloats(buffers, views, accessors[static_cast<usize>(uvAcc)], &uv, &aerr) &&
                        uv.size() == vcount * 2)
                        hasUV = true;
                    if (tanAcc >= 0 && static_cast<usize>(tanAcc) < accessors.size() &&
                        accessors[static_cast<usize>(tanAcc)].components == 4 &&
                        ReadAccessorFloats(buffers, views, accessors[static_cast<usize>(tanAcc)], &tan, &aerr) &&
                        tan.size() == vcount * 4)
                        hasTangent = true;
                    const int colComps = (colAcc >= 0 && static_cast<usize>(colAcc) < accessors.size())
                                             ? accessors[static_cast<usize>(colAcc)].components
                                             : 0;
                    if ((colComps == 3 || colComps == 4) &&
                        ReadAccessorFloats(buffers, views, accessors[static_cast<usize>(colAcc)], &col, &aerr) &&
                        col.size() == vcount * static_cast<usize>(colComps))
                        hasColor = true;

                    const u32 vbase = static_cast<u32>(md.vertices.size());
                    md.vertices.resize(static_cast<usize>(vbase) + vcount);
                    for (usize v = 0; v < vcount; ++v) {
                        Vertex& vx = md.vertices[static_cast<usize>(vbase) + v];
                        vx.position = Vec3{pos[v * 3], pos[v * 3 + 1], pos[v * 3 + 2]};
                        if (hasNormals) vx.normal = Normalize(Vec3{nrm[v * 3], nrm[v * 3 + 1], nrm[v * 3 + 2]});
                        if (hasUV) vx.uv = Vec2{uv[v * 2], uv[v * 2 + 1]};
                        if (hasTangent)
                            vx.tangent = Vec4{tan[v * 4], tan[v * 4 + 1], tan[v * 4 + 2], tan[v * 4 + 3]};
                        if (hasColor) {
                            vx.color = (colComps == 4) ? Vec4{col[v * 4], col[v * 4 + 1], col[v * 4 + 2],
                                                              col[v * 4 + 3]}
                                                       : Vec4{col[v * 3], col[v * 3 + 1], col[v * 3 + 2], 1.0f};
                        }
                    }

                    // Индексы (последовательные для неиндексированного примитива).
                    std::vector<u32> indices;
                    const int idxAcc = jp.GetInt("indices", -1);
                    if (idxAcc >= 0 && static_cast<usize>(idxAcc) < accessors.size()) {
                        if (!ReadAccessorIndices(buffers, views, accessors[static_cast<usize>(idxAcc)], &indices,
                                                 &aerr)) {
                            Warn("index accessor unreadable: " + aerr);
                            indices.clear();
                        }
                    }
                    if (indices.empty()) {
                        indices.resize(vcount);
                        for (usize v = 0; v < vcount; ++v) indices[v] = static_cast<u32>(v);
                    }

                    // Режимы 5/6 -> треугольники.
                    std::vector<u32> tri;
                    if (mode == 4) {
                        tri = indices;
                    } else if (mode == 5) {  // triangle strip
                        for (usize t = 0; t + 2 < indices.size(); ++t) {
                            if ((t & 1u) == 0u) {
                                tri.push_back(indices[t]);
                                tri.push_back(indices[t + 1]);
                                tri.push_back(indices[t + 2]);
                            } else {
                                tri.push_back(indices[t + 1]);
                                tri.push_back(indices[t]);
                                tri.push_back(indices[t + 2]);
                            }
                        }
                    } else {  // triangle fan
                        for (usize t = 1; t + 1 < indices.size(); ++t) {
                            tri.push_back(indices[0]);
                            tri.push_back(indices[t]);
                            tri.push_back(indices[t + 1]);
                        }
                    }

                    // Выбрасываем выходящие за диапазон треугольники вместо чтения за границами.
                    std::vector<u32> safe;
                    safe.reserve(tri.size());
                    usize dropped = 0;
                    for (usize t = 0; t + 2 < tri.size(); t += 3) {
                        if (tri[t] < vcount && tri[t + 1] < vcount && tri[t + 2] < vcount) {
                            safe.push_back(tri[t]);
                            safe.push_back(tri[t + 1]);
                            safe.push_back(tri[t + 2]);
                        } else {
                            ++dropped;
                        }
                    }
                    if (dropped > 0) Warn("dropped " + std::to_string(dropped) + " out-of-range triangles");
                    if (safe.empty()) {
                        md.vertices.resize(static_cast<usize>(vbase));
                        continue;
                    }

                    const u32 istart = static_cast<u32>(md.indices.size());
                    md.indices.insert(md.indices.end(), safe.begin(), safe.end());
                    if (!hasNormals)
                        ComputePrimitiveNormals(&md, vbase, vcount, istart, static_cast<usize>(safe.size()));
                    if (!hasTangent && hasUV)
                        ComputePrimitiveTangents(&md, vbase, vcount, istart, static_cast<usize>(safe.size()));

                    MeshData::SubMesh sm;
                    sm.indexOffset = istart;
                    sm.indexCount = static_cast<u32>(safe.size());
                    sm.materialIndex = jp.GetInt("material", -1);
                    sm.name = "primitive" + std::to_string(pi);
                    md.subMeshes.push_back(std::move(sm));

                    if (hasColor && sm.materialIndex >= 0 &&
                        static_cast<usize>(sm.materialIndex) < outMaterials->size())
                        (*outMaterials)[static_cast<usize>(sm.materialIndex)].vertexColors = true;
                }
            }
            RepairBounds(&md);
            outMeshes->push_back(std::move(md));  // держим индексы синхронно с node.mesh
        }
    }

    // ---- nodes -----------------------------------------------------------
    if (const JsonValue* jn = root.Find("nodes"); jn && jn->IsArray()) {
        outNodes->resize(jn->Size());
        for (usize i = 0; i < jn->Size(); ++i) {
            const JsonValue& j = (*jn)[i];
            ModelNode& node = (*outNodes)[i];
            node.name = j.GetString("name", "node" + std::to_string(i));
            const JsonValue* jmat = j.Find("matrix");
            if (jmat && jmat->IsArray() && jmat->Size() >= 16) {
                for (int k = 0; k < 16; ++k) {
                    const f32 def = (k == 0 || k == 5 || k == 10 || k == 15) ? 1.0f : 0.0f;
                    node.localMatrix.m[k] = (*jmat)[static_cast<usize>(k)].AsFloat(def);
                }
                node.hasMatrix = true;
                node.translation = Vec3{node.localMatrix.m[12], node.localMatrix.m[13], node.localMatrix.m[14]};
                node.rotation = Quat::FromMat4(node.localMatrix).Normalized();
                for (int c = 0; c < 3; ++c) {
                    const Vec3 axis{node.localMatrix.at(c, 0), node.localMatrix.at(c, 1), node.localMatrix.at(c, 2)};
                    node.scale[c] = Length(axis);
                }
            } else {
                if (const JsonValue* t = j.Find("translation"); t && t->IsArray() && t->Size() >= 3)
                    node.translation = Vec3{(*t)[0].AsFloat(0), (*t)[1].AsFloat(0), (*t)[2].AsFloat(0)};
                if (const JsonValue* r = j.Find("rotation"); r && r->IsArray() && r->Size() >= 4)
                    node.rotation = Quat{(*r)[0].AsFloat(0), (*r)[1].AsFloat(0), (*r)[2].AsFloat(0),
                                         (*r)[3].AsFloat(1)}
                                        .Normalized();
                if (const JsonValue* s = j.Find("scale"); s && s->IsArray() && s->Size() >= 3)
                    node.scale = Vec3{(*s)[0].AsFloat(1), (*s)[1].AsFloat(1), (*s)[2].AsFloat(1)};
                node.localMatrix =
                    Mat4::Translate(node.translation) * node.rotation.ToMat4() * Mat4::Scale(node.scale);
            }
            node.mesh = j.GetInt("mesh", -1);
            if (node.mesh < 0 || static_cast<usize>(node.mesh) >= outMeshes->size()) {
                node.mesh = -1;
            } else {
                const MeshData& src = (*outMeshes)[static_cast<usize>(node.mesh)];
                node.material = src.subMeshes.empty() ? 0 : src.subMeshes[0].materialIndex;
            }
            if (const JsonValue* jc = j.Find("children"); jc && jc->IsArray()) {
                for (usize k = 0; k < jc->Size(); ++k) {
                    const int child = (*jc)[k].AsInt(-1);
                    if (child >= 0 && static_cast<usize>(child) < outNodes->size()) node.children.push_back(child);
                }
            }
        }
        for (usize i = 0; i < outNodes->size(); ++i)
            for (int child : (*outNodes)[i].children)
                (*outNodes)[static_cast<usize>(child)].parent = static_cast<int>(i);
    }

    // Иерархия по умолчанию, когда в файле есть меши, но нет узлов.
    if (outNodes->empty()) {
        for (usize i = 0; i < outMeshes->size(); ++i) {
            ModelNode n;
            n.name = (*outMeshes)[i].name;
            n.mesh = static_cast<int>(i);
            n.material = (*outMeshes)[i].subMeshes.empty() ? 0 : (*outMeshes)[i].subMeshes[0].materialIndex;
            outNodes->push_back(std::move(n));
        }
    }

    // Каждому субмешу нужен индекс материала в допустимом диапазоне.
    int fallback = -1;
    for (MeshData& md : *outMeshes) {
        for (MeshData::SubMesh& sm : md.subMeshes) {
            if (sm.materialIndex < 0 || static_cast<usize>(sm.materialIndex) >= outMaterials->size()) {
                if (fallback < 0) {
                    Material d;
                    d.name = "default";
                    d.baseColor = Color(1, 1, 1, 1);
                    d.metallic = 1.0f;
                    d.roughness = 1.0f;
                    outMaterials->push_back(std::move(d));
                    fallback = static_cast<int>(outMaterials->size()) - 1;
                }
                sm.materialIndex = fallback;
            }
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// PLY (ascii + бинарный, little & big endian)
// ---------------------------------------------------------------------------
enum class PlyType { Invalid, I8, U8, I16, U16, I32, U32, F32, F64 };

int PlyTypeSize(PlyType t) {
    switch (t) {
        case PlyType::I8:
        case PlyType::U8:
            return 1;
        case PlyType::I16:
        case PlyType::U16:
            return 2;
        case PlyType::I32:
        case PlyType::U32:
        case PlyType::F32:
            return 4;
        case PlyType::F64:
            return 8;
        default:
            return 0;
    }
}

PlyType PlyParseType(const std::string& s) {
    if (s == "char" || s == "int8") return PlyType::I8;
    if (s == "uchar" || s == "uint8") return PlyType::U8;
    if (s == "short" || s == "int16") return PlyType::I16;
    if (s == "ushort" || s == "uint16") return PlyType::U16;
    if (s == "int" || s == "int32") return PlyType::I32;
    if (s == "uint" || s == "uint32") return PlyType::U32;
    if (s == "float" || s == "float32") return PlyType::F32;
    if (s == "double" || s == "float64") return PlyType::F64;
    return PlyType::Invalid;
}

f64 PlyReadScalar(const u8* p, PlyType t, bool bigEndian) {
    switch (t) {
        case PlyType::I8:
            return static_cast<f64>(static_cast<i8>(p[0]));
        case PlyType::U8:
            return static_cast<f64>(p[0]);
        case PlyType::I16: {
            const u16 raw = bigEndian ? ReadU16BE(p) : ReadU16LE(p);
            i16 v = 0;
            std::memcpy(&v, &raw, 2);
            return static_cast<f64>(v);
        }
        case PlyType::U16:
            return static_cast<f64>(bigEndian ? ReadU16BE(p) : ReadU16LE(p));
        case PlyType::I32: {
            const u32 raw = bigEndian ? ReadU32BE(p) : ReadU32LE(p);
            i32 v = 0;
            std::memcpy(&v, &raw, 4);
            return static_cast<f64>(v);
        }
        case PlyType::U32:
            return static_cast<f64>(bigEndian ? ReadU32BE(p) : ReadU32LE(p));
        case PlyType::F32:
            return static_cast<f64>(bigEndian ? ReadF32BE(p) : ReadF32LE(p));
        case PlyType::F64: {
            const u64 raw = bigEndian ? ReadU64BE(p) : ReadU64LE(p);
            f64 v = 0;
            std::memcpy(&v, &raw, 8);
            return v;
        }
        default:
            return 0;
    }
}

struct PlyProperty {
    std::string name;
    PlyType type = PlyType::Invalid;
    bool isList = false;
    PlyType countType = PlyType::Invalid;
    PlyType itemType = PlyType::Invalid;
};

struct PlyElement {
    std::string name;
    usize count = 0;
    std::vector<PlyProperty> props;
};

// Единый читатель секции данных PLY (ascii-токены или бинарные записи).
struct PlyReader {
    bool binary = false;
    bool bigEndian = false;
    const u8* p = nullptr;
    const u8* end = nullptr;
    Tokenizer tk;
    bool ok = true;

    bool Read(PlyType t, f64* out) {
        if (binary) {
            const int sz = PlyTypeSize(t);
            if (!ok || sz <= 0 || static_cast<usize>(end - p) < static_cast<usize>(sz)) {
                ok = false;
                return false;
            }
            *out = PlyReadScalar(p, t, bigEndian);
            p += sz;
            return true;
        }
        std::string s;
        if (!tk.Next(&s)) {
            ok = false;
            return false;
        }
        if (t == PlyType::F32 || t == PlyType::F64) {
            f32 v = 0;
            if (!ParseF32(s, &v)) {
                ok = false;
                return false;
            }
            *out = static_cast<f64>(v);
            return true;
        }
        i64 v = 0;
        if (!ParseI64(s, &v)) {
            ok = false;
            return false;
        }
        *out = static_cast<f64>(v);
        return true;
    }
};

f32 PlyColorComponent(f64 v, PlyType t) {
    switch (t) {
        case PlyType::U8: return static_cast<f32>(v / 255.0);
        case PlyType::U16: return static_cast<f32>(v / 65535.0);
        case PlyType::I8: return static_cast<f32>(v / 127.0);
        case PlyType::I16: return static_cast<f32>(v / 32767.0);
        default: return static_cast<f32>(v);
    }
}

bool ImportPlyData(const void* data, usize size, MeshData* outMesh, std::string* error) {
    auto Fail = [&](const std::string& msg) {
        if (error) *error = msg;
        ENG_LOGW("model", "PLY: %s", msg.c_str());
        return false;
    };
    if (!data || size < 4 || !outMesh) return Fail("empty PLY input");
    const u8* d = static_cast<const u8*>(data);
    if (std::memcmp(d, "ply", 3) != 0) return Fail("missing 'ply' magic");

    int format = -1;  // 0 ascii, 1 binary LE, 2 binary BE
    std::vector<PlyElement> elements;
    usize pos = 0;
    bool headerDone = false;
    while (pos < size && !headerDone) {
        usize end = pos;
        while (end < size && d[end] != '\n') ++end;
        usize len = end - pos;
        if (len > 0 && d[pos + len - 1] == '\r') --len;
        std::string line(reinterpret_cast<const char*>(d) + pos, len);
        pos = end + 1;
        Tokenizer tk(line.data(), line.size());
        std::string kw;
        if (!tk.Next(&kw)) continue;
        if (kw == "format") {
            std::string f;
            tk.Next(&f);
            if (f == "ascii") format = 0;
            else if (f == "binary_little_endian") format = 1;
            else if (f == "binary_big_endian") format = 2;
            else return Fail("unsupported PLY format '" + f + "'");
        } else if (kw == "element") {
            std::string name, countStr;
            tk.Next(&name);
            tk.Next(&countStr);
            i64 c = 0;
            if (!ParseI64(countStr, &c) || c < 0) return Fail("malformed element count");
            PlyElement el;
            el.name = name;
            el.count = static_cast<usize>(c);
            elements.push_back(std::move(el));
        } else if (kw == "property") {
            if (elements.empty()) continue;
            std::string t1;
            tk.Next(&t1);
            PlyProperty p;
            if (t1 == "list") {
                std::string ct, it, nm;
                tk.Next(&ct);
                tk.Next(&it);
                tk.Next(&nm);
                p.isList = true;
                p.countType = PlyParseType(ct);
                p.itemType = PlyParseType(it);
                p.name = nm;
                if (p.countType == PlyType::Invalid || p.itemType == PlyType::Invalid)
                    return Fail("invalid list property types");
            } else {
                std::string nm;
                tk.Next(&nm);
                p.type = PlyParseType(t1);
                p.name = nm;
                if (p.type == PlyType::Invalid) return Fail("invalid property type '" + t1 + "'");
            }
            elements.back().props.push_back(std::move(p));
        } else if (kw == "end_header") {
            headerDone = true;
        }
        // Строки "ply", "comment" и "obj_info" игнорируются.
    }
    if (!headerDone) return Fail("missing end_header");
    if (format < 0) return Fail("missing format line");

    const PlyElement* vertexEl = nullptr;
    for (const PlyElement& el : elements)
        if (el.name == "vertex") vertexEl = &el;
    if (!vertexEl) return Fail("no vertex element");

    PlyReader rd;
    rd.binary = (format != 0);
    rd.bigEndian = (format == 2);
    rd.p = d + pos;
    rd.end = d + size;
    rd.tk = Tokenizer(reinterpret_cast<const char*>(d) + pos, size - pos);

    std::vector<Vec3> positions, normals;
    std::vector<Vec2> uvs;
    std::vector<Color> colors;
    std::vector<u32> indices;
    bool hasNormals = false;

    for (const PlyElement& el : elements) {
        const bool isVertex = (el.name == "vertex");
        const bool isFace = (el.name == "face");
        // Интересующие индексы свойств (элемент vertex).
        int xi = -1, yi = -1, zi = -1, nxi = -1, nyi = -1, nzi = -1, si = -1, ti = -1;
        int ri = -1, gi = -1, bi = -1, ai = -1;
        int faceListProp = -1;
        if (isVertex) {
            for (usize pi = 0; pi < el.props.size(); ++pi) {
                const PlyProperty& pr = el.props[pi];
                std::string n = pr.name;
                for (char& c : n) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                if (n == "x") xi = static_cast<int>(pi);
                else if (n == "y") yi = static_cast<int>(pi);
                else if (n == "z") zi = static_cast<int>(pi);
                else if (n == "nx") nxi = static_cast<int>(pi);
                else if (n == "ny") nyi = static_cast<int>(pi);
                else if (n == "nz") nzi = static_cast<int>(pi);
                else if (n == "s" || n == "u" || n == "texture_u") si = static_cast<int>(pi);
                else if (n == "t" || n == "v" || n == "texture_v") ti = static_cast<int>(pi);
                else if (n == "red" || n == "r" || n == "diffuse_red") ri = static_cast<int>(pi);
                else if (n == "green" || n == "g" || n == "diffuse_green") gi = static_cast<int>(pi);
                else if (n == "blue" || n == "b" || n == "diffuse_blue") bi = static_cast<int>(pi);
                else if (n == "alpha" || n == "a") ai = static_cast<int>(pi);
            }
            if (xi < 0 || yi < 0 || zi < 0) return Fail("vertex element without x/y/z");
        } else if (isFace) {
            for (usize pi = 0; pi < el.props.size(); ++pi) {
                const PlyProperty& pr = el.props[pi];
                if (!pr.isList) continue;
                if (pr.name.find("vertex_ind") != std::string::npos ||
                    pr.name.find("vertex_index") != std::string::npos) {
                    faceListProp = static_cast<int>(pi);
                    break;
                }
                if (faceListProp < 0) faceListProp = static_cast<int>(pi);
            }
        }

        for (usize inst = 0; inst < el.count && rd.ok; ++inst) {
            f32 x = 0, y = 0, z = 0, nx = 0, ny = 0, nz = 0, u = 0, v = 0;
            Color col{1, 1, 1, 1};
            std::vector<u32> poly;
            for (usize pi = 0; pi < el.props.size(); ++pi) {
                const PlyProperty& pr = el.props[pi];
                if (pr.isList) {
                    f64 cntF = 0;
                    if (!rd.Read(pr.countType, &cntF)) break;
                    i64 cnt = static_cast<i64>(cntF);
                    if (cnt < 0) cnt = 0;
                    if (isFace && static_cast<int>(pi) == faceListProp) {
                        for (i64 k = 0; k < cnt; ++k) {
                            f64 iv = 0;
                            if (!rd.Read(pr.itemType, &iv)) break;
                            poly.push_back(iv < 0 ? 0u : static_cast<u32>(iv));
                        }
                    } else {
                        for (i64 k = 0; k < cnt; ++k) {
                            f64 dummy = 0;
                            if (!rd.Read(pr.itemType, &dummy)) break;
                        }
                    }
                    continue;
                }
                f64 val = 0;
                if (!rd.Read(pr.type, &val)) break;
                if (!isVertex) continue;
                const int idx = static_cast<int>(pi);
                if (idx == xi) x = static_cast<f32>(val);
                else if (idx == yi) y = static_cast<f32>(val);
                else if (idx == zi) z = static_cast<f32>(val);
                else if (idx == nxi) nx = static_cast<f32>(val);
                else if (idx == nyi) ny = static_cast<f32>(val);
                else if (idx == nzi) nz = static_cast<f32>(val);
                else if (idx == si) u = static_cast<f32>(val);
                else if (idx == ti) v = static_cast<f32>(val);
                else if (idx == ri) col.r = PlyColorComponent(val, pr.type);
                else if (idx == gi) col.g = PlyColorComponent(val, pr.type);
                else if (idx == bi) col.b = PlyColorComponent(val, pr.type);
                else if (idx == ai) col.a = PlyColorComponent(val, pr.type);
            }
            if (!rd.ok) break;
            if (isVertex) {
                positions.push_back(Vec3{x, y, z});
                if (nxi >= 0 && nyi >= 0 && nzi >= 0) {
                    normals.push_back(Vec3{nx, ny, nz});
                    hasNormals = true;
                } else {
                    normals.push_back(Vec3{0, 0, 0});
                }
                uvs.push_back((si >= 0 && ti >= 0) ? Vec2{u, v} : Vec2{0, 0});
                colors.push_back((ri >= 0 && gi >= 0 && bi >= 0) ? col : Color{1, 1, 1, 1});
            } else if (isFace && poly.size() >= 3) {
                // Триангуляция веером произвольных полигонов.
                for (usize k = 1; k + 1 < poly.size(); ++k) {
                    const u32 a = poly[0], b = poly[k], c = poly[k + 1];
                    if (a >= positions.size() || b >= positions.size() || c >= positions.size()) continue;
                    indices.push_back(a);
                    indices.push_back(b);
                    indices.push_back(c);
                }
            }
        }
        if (!rd.ok) {
            ENG_LOGW("model", "PLY: truncated data while reading element '%s'", el.name.c_str());
            break;
        }
    }

    if (positions.empty()) return Fail("PLY contains no vertices");

    outMesh->vertices.clear();
    outMesh->indices.clear();
    outMesh->subMeshes.clear();
    outMesh->name.clear();
    outMesh->vertices.resize(positions.size());
    for (usize i = 0; i < positions.size(); ++i) {
        Vertex& vx = outMesh->vertices[i];
        vx.position = positions[i];
        vx.uv = uvs[i];
        vx.color = colors[i].ToVec4();
        if (hasNormals) {
            const Vec3 n = Normalize(normals[i]);
            vx.normal = LengthSq(n) > 0.25f ? n : Vec3{0, 1, 0};
        }
    }
    outMesh->indices = indices;
    if (!hasNormals) EnsureSmoothNormals(outMesh);
    RepairBounds(outMesh);
    return true;
}

// ---------------------------------------------------------------------------
// STL (ascii + binary)
// ---------------------------------------------------------------------------
struct StlKey {
    i64 p[3] = {0, 0, 0};
    i64 n[3] = {0, 0, 0};
    bool operator==(const StlKey& o) const {
        return p[0] == o.p[0] && p[1] == o.p[1] && p[2] == o.p[2] && n[0] == o.n[0] && n[1] == o.n[1] &&
               n[2] == o.n[2];
    }
};

struct StlKeyHash {
    usize operator()(const StlKey& k) const {
        u64 h = 1469598103934665603ULL;
        auto mix = [&h](u64 x) {
            h ^= x;
            h *= 1099511628211ULL;
        };
        for (int i = 0; i < 3; ++i) {
            mix(static_cast<u64>(k.p[i]));
            mix(static_cast<u64>(k.n[i]));
        }
        return static_cast<usize>(h);
    }
};

inline i64 Quantize(f32 v, f32 scale) { return static_cast<i64>(std::lround(static_cast<double>(v) * scale)); }

bool ImportStlData(const void* data, usize size, MeshData* outMesh, std::string* error) {
    auto Fail = [&](const std::string& msg) {
        if (error) *error = msg;
        ENG_LOGW("model", "STL: %s", msg.c_str());
        return false;
    };
    if (!data || size < 15 || !outMesh) return Fail("empty STL input");
    const u8* d = static_cast<const u8*>(data);

    outMesh->vertices.clear();
    outMesh->indices.clear();
    outMesh->subMeshes.clear();
    outMesh->name.clear();
    outMesh->bounds = Bounds{};

    // Квантованная сварка: одинаковые позиции *и* одинаковые нормали делят
    // одну вершину, поэтому гранёный вид сохраняется.
    std::unordered_map<StlKey, u32, StlKeyHash> seen;
    auto AddVertex = [&](const Vec3& p, const Vec3& n) -> u32 {
        StlKey key;
        for (int i = 0; i < 3; ++i) {
            key.p[i] = Quantize(p[i], 100000.0f);
            key.n[i] = Quantize(n[i], 10000.0f);
        }
        auto it = seen.find(key);
        if (it != seen.end()) return it->second;
        Vertex vx;
        vx.position = p;
        vx.normal = n;
        const u32 idx = static_cast<u32>(outMesh->vertices.size());
        outMesh->vertices.push_back(vx);
        seen.emplace(key, idx);
        return idx;
    };
    auto AddTriangle = [&](const Vec3& a, const Vec3& b, const Vec3& c, const Vec3& facetNormal) {
        Vec3 n = Normalize(facetNormal);
        if (LengthSq(n) < 0.25f) n = Normalize(Cross(b - a, c - a));
        if (LengthSq(n) < 0.25f) n = Vec3{0, 1, 0};
        outMesh->indices.push_back(AddVertex(a, n));
        outMesh->indices.push_back(AddVertex(b, n));
        outMesh->indices.push_back(AddVertex(c, n));
    };

    if (LooksLikeBinaryStl(d, size)) {
        const u32 count = ReadU32LE(d + 80);
        if (static_cast<usize>(84) + static_cast<usize>(count) * 50u > size) return Fail("truncated binary STL");
        outMesh->vertices.reserve(static_cast<usize>(count) * 3);
        for (u32 i = 0; i < count; ++i) {
            const u8* t = d + 84 + static_cast<usize>(i) * 50u;
            const Vec3 n{ReadF32LE(t), ReadF32LE(t + 4), ReadF32LE(t + 8)};
            const Vec3 v0{ReadF32LE(t + 12), ReadF32LE(t + 16), ReadF32LE(t + 20)};
            const Vec3 v1{ReadF32LE(t + 24), ReadF32LE(t + 28), ReadF32LE(t + 32)};
            const Vec3 v2{ReadF32LE(t + 36), ReadF32LE(t + 40), ReadF32LE(t + 44)};
            AddTriangle(v0, v1, v2, n);
        }
    } else {
        Tokenizer tk(reinterpret_cast<const char*>(d), size);
        auto readVec3 = [&](Vec3* out) -> bool {
            std::string t;
            for (int i = 0; i < 3; ++i) {
                if (!tk.Next(&t)) return false;
                f32 v = 0;
                if (!ParseF32(t, &v)) return false;
                (*out)[i] = v;
            }
            return true;
        };
        Vec3 facetNormal{0, 0, 0};
        Vec3 tri[3];
        int have = 0;
        std::string tok, name;
        while (tk.Next(&tok)) {
            if (tok == "facet") {
                std::string kw;
                if (tk.Next(&kw) && kw == "normal") {
                    if (!readVec3(&facetNormal)) facetNormal = Vec3{0, 0, 0};
                }
            } else if (tok == "vertex") {
                if (!readVec3(&tri[have % 3])) break;
                ++have;
                if (have % 3 == 0) AddTriangle(tri[0], tri[1], tri[2], facetNormal);
            } else if (tok == "solid") {
                std::string t;
                if (name.empty() && have == 0 && tk.Next(&t)) name = t;
            }
        }
        if (name.rfind("endsolid", 0) == 0) name.clear();
        if (!name.empty()) outMesh->name = name;
    }

    if (outMesh->indices.empty()) return Fail("STL contains no triangles");
    RepairBounds(outMesh);
    return true;
}

// ---------------------------------------------------------------------------
// MagicaVoxel .vox (воксели с greedy-мешингом + текстура палитры)
// ---------------------------------------------------------------------------
struct VoxFace {
    u8 color = 0;
    i8 dir = 0;
};

// Классический greedy-мешер: сливает копланарные одноцветные грани в прямоугольники.
MeshData BuildGreedyVoxelMesh(const std::vector<u8>& vox, int sx, int sy, int sz, const Vec3& origin,
                              const std::array<u32, 256>& palette, int* outQuads) {
    MeshData mesh;
    if (sx <= 0 || sy <= 0 || sz <= 0) return mesh;
    const int dims[3] = {sx, sy, sz};
    auto at = [&](int x, int y, int z) -> u8 {
        if (x < 0 || y < 0 || z < 0 || x >= sx || y >= sy || z >= sz) return 0;
        return vox[((static_cast<usize>(z) * static_cast<usize>(sy)) + static_cast<usize>(y)) *
                       static_cast<usize>(sx) +
                   static_cast<usize>(x)];
    };
    auto colorOf = [&](u8 idx) -> Color {
        // Индексы палитры XYZI начинаются с 1: индекс 1 — запись RGBA 0.
        int i = static_cast<int>(idx) - 1;
        if (i < 0) i = 0;
        const u32 c = palette[static_cast<usize>(i)];
        return Color::FromBytes(static_cast<u8>(c & 0xFF), static_cast<u8>((c >> 8) & 0xFF),
                                static_cast<u8>((c >> 16) & 0xFF), static_cast<u8>((c >> 24) & 0xFF));
    };
    auto uvOf = [](u8 idx) -> Vec2 {
        int i = static_cast<int>(idx) - 1;
        if (i < 0) i = 0;
        return {((static_cast<f32>(i % 16)) + 0.5f) / 16.0f, ((static_cast<f32>(i / 16)) + 0.5f) / 16.0f};
    };

    int quads = 0;
    for (int d = 0; d < 3; ++d) {
        const int u = (d + 1) % 3, v = (d + 2) % 3;
        const int du = dims[u], dv = dims[v];
        std::vector<VoxFace> mask(static_cast<usize>(du) * static_cast<usize>(dv));
        int x[3] = {0, 0, 0};
        for (x[d] = -1; x[d] < dims[d]; ++x[d]) {
            usize n = 0;
            for (x[v] = 0; x[v] < dims[v]; ++x[v]) {
                for (x[u] = 0; x[u] < dims[u]; ++x[u], ++n) {
                    const u8 a = at(x[0], x[1], x[2]);
                    const u8 b = at(x[0] + (d == 0 ? 1 : 0), x[1] + (d == 1 ? 1 : 0), x[2] + (d == 2 ? 1 : 0));
                    VoxFace f;
                    if ((a != 0) != (b != 0)) f = (a != 0) ? VoxFace{a, -1} : VoxFace{b, 1};
                    mask[n] = f;
                }
            }
            n = 0;
            for (int j = 0; j < dv; ++j) {
                for (int i = 0; i < du;) {
                    const VoxFace f = mask[n];
                    if (f.color == 0) {
                        ++i;
                        ++n;
                        continue;
                    }
                    int w = 1;
                    while (i + w < du && mask[n + static_cast<usize>(w)].color == f.color &&
                           mask[n + static_cast<usize>(w)].dir == f.dir)
                        ++w;
                    int h = 1;
                    bool stop = false;
                    while (j + h < dv) {
                        for (int k = 0; k < w; ++k) {
                            const VoxFace& m2 =
                                mask[n + static_cast<usize>(k) + static_cast<usize>(h) * static_cast<usize>(du)];
                            if (m2.color != f.color || m2.dir != f.dir) {
                                stop = true;
                                break;
                            }
                        }
                        if (stop) break;
                        ++h;
                    }

                    Vec3 base{0, 0, 0};
                    base[d] = static_cast<f32>(x[d] + 1);
                    base[u] = static_cast<f32>(i);
                    base[v] = static_cast<f32>(j);
                    const Vec3 baseW = base + origin;
                    Vec3 ddu{0, 0, 0};
                    ddu[u] = static_cast<f32>(w);
                    Vec3 ddv{0, 0, 0};
                    ddv[v] = static_cast<f32>(h);
                    const Color col = colorOf(f.color);
                    const Vec2 uv = uvOf(f.color);
                    Vec3 nrm{0, 0, 0};
                    nrm[d] = (f.dir > 0) ? 1.0f : -1.0f;
                    Vec3 tang = Normalize(ddu);
                    if (f.dir < 0) tang = -tang;
                    const Vec3 p0 = baseW;
                    const Vec3 p1 = baseW + ddu;
                    const Vec3 p2 = baseW + ddu + ddv;
                    const Vec3 p3 = baseW + ddv;
                    const u32 b0 = static_cast<u32>(mesh.vertices.size());
                    auto push = [&](const Vec3& p) {
                        Vertex vx;
                        vx.position = p;
                        vx.normal = nrm;
                        vx.uv = uv;
                        vx.color = col.ToVec4();
                        vx.uv2 = Vec2{static_cast<f32>(f.color), 0.0f};
                        vx.tangent = Vec4{tang.x, tang.y, tang.z, 1.0f};
                        mesh.vertices.push_back(vx);
                    };
                    push(p0);
                    push(p1);
                    push(p2);
                    push(p3);
                    if (f.dir > 0) {
                        const u32 tri[6] = {b0, b0 + 1, b0 + 2, b0, b0 + 2, b0 + 3};
                        mesh.indices.insert(mesh.indices.end(), tri, tri + 6);
                    } else {
                        const u32 tri[6] = {b0, b0 + 2, b0 + 1, b0, b0 + 3, b0 + 2};
                        mesh.indices.insert(mesh.indices.end(), tri, tri + 6);
                    }
                    ++quads;

                    for (int l = 0; l < h; ++l)
                        for (int k = 0; k < w; ++k)
                            mask[n + static_cast<usize>(k) + static_cast<usize>(l) * static_cast<usize>(du)] =
                                VoxFace{};
                    i += w;
                    n += static_cast<usize>(w);
                }
            }
        }
    }
    if (outQuads) *outQuads = quads;
    return mesh;
}

void VoxDefaultPalette(std::array<u32, 256>& palette) {
    auto hsv = [](f32 h, f32 s, f32 v, u8* r, u8* g, u8* b) {
        h -= std::floor(h);
        const f32 f = h * 6.0f;
        const int i = static_cast<int>(f);
        const f32 fr = f - static_cast<f32>(i);
        const f32 p = v * (1.0f - s);
        const f32 q = v * (1.0f - fr * s);
        const f32 t = v * (1.0f - (1.0f - fr) * s);
        f32 rr = v, gg = t, bb = p;
        switch (i % 6) {
            case 1: rr = q; gg = v; bb = p; break;
            case 2: rr = p; gg = v; bb = t; break;
            case 3: rr = p; gg = q; bb = v; break;
            case 4: rr = t; gg = p; bb = v; break;
            case 5: rr = v; gg = p; bb = q; break;
            default: break;
        }
        *r = static_cast<u8>(Clamp(rr, 0.0f, 1.0f) * 255.0f + 0.5f);
        *g = static_cast<u8>(Clamp(gg, 0.0f, 1.0f) * 255.0f + 0.5f);
        *b = static_cast<u8>(Clamp(bb, 0.0f, 1.0f) * 255.0f + 0.5f);
    };
    for (int k = 0; k < 256; ++k) {
        const f32 hue = static_cast<f32>(k % 32) / 32.0f;
        const f32 sat = 0.35f + 0.55f * static_cast<f32>((k / 32) % 2);
        const f32 val = 0.30f + 0.70f * static_cast<f32>(k / 64) / 3.0f;
        u8 r = 0, g = 0, b = 0;
        hsv(hue, sat, val, &r, &g, &b);
        palette[static_cast<usize>(k)] = static_cast<u32>(r) | (static_cast<u32>(g) << 8) |
                                         (static_cast<u32>(b) << 16) | (0xFFu << 24);
    }
}

bool ImportVoxData(const void* data, usize size, std::vector<MeshData>* outMeshes,
                   std::vector<Material>* outMaterials, std::vector<Texture>* outTextures, std::string* error) {
    auto Fail = [&](const std::string& msg) {
        if (error) *error = msg;
        ENG_LOGW("model", "VOX: %s", msg.c_str());
        return false;
    };
    auto Warn = [&](const std::string& msg) { ENG_LOGW("model", "VOX: %s", msg.c_str()); };
    if (!data || size < 8 || !outMeshes || !outMaterials || !outTextures) return Fail("empty VOX input");
    const u8* d = static_cast<const u8*>(data);
    if (std::memcmp(d, "VOX ", 4) != 0) return Fail("missing 'VOX ' magic");

    struct VoxModelData {
        int sx = 1, sy = 1, sz = 1;
        std::vector<u8> vox;
    };
    std::array<u32, 256> palette{};
    bool havePalette = false;
    std::vector<VoxModelData> models;
    int curSx = 0, curSy = 0, curSz = 0;
    std::unordered_map<i32, Vec3> trnTranslation;
    std::unordered_map<i32, i32> trnParent;
    std::vector<std::pair<i32, int>> shapeInstances;

    usize pos = 8;
    while (pos + 12 <= size) {
        char id[5] = {0, 0, 0, 0, 0};
        std::memcpy(id, d + pos, 4);
        const u32 contentSize = ReadU32LE(d + pos + 4);
        const usize cs = pos + 12;
        if (cs + contentSize > size) {
            Warn("truncated chunk (stopping)");
            break;
        }
        const u8* c = d + cs;

        if (std::strcmp(id, "SIZE") == 0 && contentSize >= 12) {
            curSx = static_cast<i32>(ReadU32LE(c));
            curSy = static_cast<i32>(ReadU32LE(c + 4));
            curSz = static_cast<i32>(ReadU32LE(c + 8));
            if (curSx <= 0 || curSy <= 0 || curSz <= 0 || curSx > 4096 || curSy > 4096 || curSz > 4096)
                return Fail("invalid SIZE chunk");
        } else if (std::strcmp(id, "XYZI") == 0 && contentSize >= 4) {
            const u32 num = ReadU32LE(c);
            if (4u + static_cast<usize>(num) * 4u > contentSize) {
                Warn("XYZI chunk truncated");
            } else {
                VoxModelData m;
                m.sx = curSx > 0 ? curSx : 1;
                m.sy = curSy > 0 ? curSy : 1;
                m.sz = curSz > 0 ? curSz : 1;
                m.vox.assign(static_cast<usize>(m.sx) * static_cast<usize>(m.sy) * static_cast<usize>(m.sz), 0);
                for (u32 k = 0; k < num; ++k) {
                    const u8 vx = c[4 + static_cast<usize>(k) * 4 + 0];
                    const u8 vy = c[4 + static_cast<usize>(k) * 4 + 1];
                    const u8 vz = c[4 + static_cast<usize>(k) * 4 + 2];
                    const u8 ci = c[4 + static_cast<usize>(k) * 4 + 3];
                    if (vx >= m.sx || vy >= m.sy || vz >= m.sz) continue;
                    m.vox[(static_cast<usize>(vz) * static_cast<usize>(m.sy) + static_cast<usize>(vy)) *
                              static_cast<usize>(m.sx) +
                          static_cast<usize>(vx)] = ci;
                }
                models.push_back(std::move(m));
            }
        } else if (std::strcmp(id, "RGBA") == 0 && contentSize >= 1024) {
            for (int k = 0; k < 256; ++k) {
                const u8 r = c[k * 4 + 0], g = c[k * 4 + 1], b = c[k * 4 + 2], a = c[k * 4 + 3];
                palette[static_cast<usize>(k)] = static_cast<u32>(r) | (static_cast<u32>(g) << 8) |
                                                 (static_cast<u32>(b) << 16) | (static_cast<u32>(a) << 24);
            }
            havePalette = true;
        } else if (std::strcmp(id, "nTRN") == 0) {
            usize p = 0;
            auto readI32 = [&](i32* out) -> bool {
                if (p + 4 > contentSize) return false;
                *out = static_cast<i32>(ReadU32LE(c + p));
                p += 4;
                return true;
            };
            auto readDict = [&](std::string* translationOut) -> bool {
                i32 count = 0;
                if (!readI32(&count) || count < 0 || count > 65536) return false;
                for (i32 k = 0; k < count; ++k) {
                    i32 klen = 0;
                    if (!readI32(&klen) || klen < 0 || p + static_cast<usize>(klen) > contentSize) return false;
                    const std::string key(reinterpret_cast<const char*>(c + p), static_cast<usize>(klen));
                    p += static_cast<usize>(klen);
                    i32 vlen = 0;
                    if (!readI32(&vlen) || vlen < 0 || p + static_cast<usize>(vlen) > contentSize) return false;
                    const std::string val(reinterpret_cast<const char*>(c + p), static_cast<usize>(vlen));
                    p += static_cast<usize>(vlen);
                    if (translationOut && key == "_t") *translationOut = val;
                }
                return true;
            };
            i32 nodeId = 0, childId = 0, reserved = 0, layer = 0, numFrames = 0;
            if (readI32(&nodeId) && readDict(nullptr) && readI32(&childId) && readI32(&reserved) &&
                readI32(&layer) && readI32(&numFrames)) {
                bool haveT = false;
                Vec3 t{0, 0, 0};
                for (i32 f = 0; f < numFrames && f < 64; ++f) {
                    std::string tv;
                    if (!readDict(&tv)) break;
                    if (!tv.empty()) {
                        f32 xyz[3] = {0, 0, 0};
                        Tokenizer ttk(tv.data(), tv.size());
                        std::string tok;
                        int n = 0;
                        while (n < 3 && ttk.Next(&tok)) {
                            if (!ParseF32(tok, &xyz[n])) break;
                            ++n;
                        }
                        if (n == 3) {
                            t = Vec3{xyz[0], xyz[1], xyz[2]};
                            haveT = true;
                        }
                    }
                }
                if (haveT) {
                    trnTranslation[nodeId] = t;
                    trnParent[childId] = nodeId;
                }
            }
        } else if (std::strcmp(id, "nSHP") == 0) {
            usize p = 0;
            auto readI32 = [&](i32* out) -> bool {
                if (p + 4 > contentSize) return false;
                *out = static_cast<i32>(ReadU32LE(c + p));
                p += 4;
                return true;
            };
            auto skipDict = [&]() -> bool {
                i32 count = 0;
                if (!readI32(&count) || count < 0 || count > 65536) return false;
                for (i32 k = 0; k < count; ++k) {
                    for (int pass = 0; pass < 2; ++pass) {
                        i32 len = 0;
                        if (!readI32(&len) || len < 0 || p + static_cast<usize>(len) > contentSize) return false;
                        p += static_cast<usize>(len);
                    }
                }
                return true;
            };
            i32 nodeId = 0, numModels = 0;
            if (readI32(&nodeId) && skipDict() && readI32(&numModels) && numModels >= 0 && numModels < 65536) {
                for (i32 k = 0; k < numModels; ++k) {
                    i32 modelId = 0;
                    if (!readI32(&modelId) || !skipDict()) break;
                    shapeInstances.emplace_back(nodeId, modelId);
                }
            }
        }
        // Чанк nGRP и неизвестные чанки просто пропускаются.
        pos = cs + contentSize;  // вложенные чанки идут внутри содержимого родителя
    }

    if (models.empty()) return Fail("no XYZI voxel data");
    if (!havePalette) {
        VoxDefaultPalette(palette);
        Warn("no RGBA chunk: using a generated fallback palette");
    }

    // Узел-шейп наследует трансляцию фрейма 0 своего предка nTRN.
    auto translationOf = [&](i32 node) -> Vec3 {
        Vec3 t{0, 0, 0};
        i32 cur = node;
        int guard = 0;
        while (guard++ < 64) {
            auto it = trnParent.find(cur);
            if (it == trnParent.end()) break;
            auto tt = trnTranslation.find(it->second);
            if (tt != trnTranslation.end()) t += tt->second;
            cur = it->second;
        }
        return t;
    };

    struct Instance {
        int model = 0;
        Vec3 translation{0, 0, 0};
    };
    std::vector<Instance> instances;
    for (const auto& si : shapeInstances) {
        if (si.second < 0 || static_cast<usize>(si.second) >= models.size()) continue;
        instances.push_back({si.second, translationOf(si.first)});
    }
    if (instances.empty())
        for (usize i = 0; i < models.size(); ++i) instances.push_back({static_cast<int>(i), Vec3{0, 0, 0}});

    // Сливаем все экземпляры моделей в одну сетку (только трансляции, как
    // экспортирует MagicaVoxel для сцен с несколькими моделями).
    int gmin[3] = {0x7FFFFFFF, 0x7FFFFFFF, 0x7FFFFFFF};
    int gmax[3] = {-0x7FFFFFFF, -0x7FFFFFFF, -0x7FFFFFFF};
    std::vector<std::array<int, 3>> offsets(instances.size());
    for (usize i = 0; i < instances.size(); ++i) {
        const VoxModelData& m = models[static_cast<usize>(instances[i].model)];
        const std::array<int, 3> off = {static_cast<int>(std::lround(instances[i].translation.x)),
                                        static_cast<int>(std::lround(instances[i].translation.y)),
                                        static_cast<int>(std::lround(instances[i].translation.z))};
        offsets[i] = off;
        const int hi[3] = {off[0] + m.sx, off[1] + m.sy, off[2] + m.sz};
        for (int k = 0; k < 3; ++k) {
            gmin[k] = std::min(gmin[k], off[k]);
            gmax[k] = std::max(gmax[k], hi[k]);
        }
    }
    const int gs[3] = {gmax[0] - gmin[0], gmax[1] - gmin[1], gmax[2] - gmin[2]};
    const i64 cells = static_cast<i64>(gs[0]) * static_cast<i64>(gs[1]) * static_cast<i64>(gs[2]);
    if (cells <= 0 || cells > 64000000) return Fail("voxel grid too large");

    std::vector<u8> grid(static_cast<usize>(cells), 0);
    for (usize i = 0; i < instances.size(); ++i) {
        const VoxModelData& m = models[static_cast<usize>(instances[i].model)];
        for (int z = 0; z < m.sz; ++z) {
            for (int y = 0; y < m.sy; ++y) {
                for (int x = 0; x < m.sx; ++x) {
                    const u8 ci = m.vox[(static_cast<usize>(z) * static_cast<usize>(m.sy) +
                                         static_cast<usize>(y)) *
                                            static_cast<usize>(m.sx) +
                                        static_cast<usize>(x)];
                    if (ci == 0) continue;
                    const int gx = offsets[i][0] - gmin[0] + x;
                    const int gy = offsets[i][1] - gmin[1] + y;
                    const int gz = offsets[i][2] - gmin[2] + z;
                    if (gx < 0 || gy < 0 || gz < 0 || gx >= gs[0] || gy >= gs[1] || gz >= gs[2]) continue;
                    grid[(static_cast<usize>(gz) * static_cast<usize>(gs[1]) + static_cast<usize>(gy)) *
                             static_cast<usize>(gs[0]) +
                         static_cast<usize>(gx)] = ci;
                }
            }
        }
    }

    MeshData mesh = BuildGreedyVoxelMesh(grid, gs[0], gs[1], gs[2],
                                         Vec3{static_cast<f32>(gmin[0]), static_cast<f32>(gmin[1]),
                                              static_cast<f32>(gmin[2])},
                                         palette, nullptr);
    if (mesh.indices.empty()) return Fail("voxel model produced no geometry");
    mesh.name = "voxel";
    if (mesh.subMeshes.empty()) {
        MeshData::SubMesh sm;
        sm.indexOffset = 0;
        sm.indexCount = static_cast<u32>(mesh.indices.size());
        sm.materialIndex = 0;
        sm.name = "voxel";
        mesh.subMeshes.push_back(std::move(sm));
    }
    RepairBounds(&mesh);

    // Текстура палитры 16x16: тот же меш работает с текстурой (UV указывают на
    // тексел палитры) и без неё (цвета в вершинах, vertexColors = true).
    std::vector<u8> texels(16 * 16 * 4);
    for (int k = 0; k < 256; ++k) {
        const u32 c = palette[static_cast<usize>(k)];
        texels[static_cast<usize>(k) * 4 + 0] = static_cast<u8>(c & 0xFF);
        texels[static_cast<usize>(k) * 4 + 1] = static_cast<u8>((c >> 8) & 0xFF);
        texels[static_cast<usize>(k) * 4 + 2] = static_cast<u8>((c >> 16) & 0xFF);
        texels[static_cast<usize>(k) * 4 + 3] = static_cast<u8>((c >> 24) & 0xFF);
    }
    outTextures->clear();
    Texture paletteTex;
    paletteTex.Create(16, 16, PixelFormat::RGBA8, texels.data(), TextureFilter::Nearest,
                      TextureWrap::ClampToEdge, false);
    paletteTex.SetDebugName("vox_palette");
    outTextures->push_back(std::move(paletteTex));

    outMaterials->clear();
    Material mat;
    mat.name = "voxel";
    mat.baseColor = Color(1, 1, 1, 1);
    mat.metallic = 0.0f;
    mat.roughness = 0.9f;
    mat.vertexColors = true;
    mat.baseColorTex = &outTextures->back();  // стабильно: текстуры больше не добавляются
    outMaterials->push_back(std::move(mat));

    outMeshes->clear();
    outMeshes->push_back(std::move(mesh));
    ENG_LOGI("model", "VOX: %dx%dx%d grid, %u vertices, %u indices", gs[0], gs[1], gs[2],
             static_cast<unsigned>(outMeshes->back().vertices.size()),
             static_cast<unsigned>(outMeshes->back().indices.size()));
    return true;
}

// ---------------------------------------------------------------------------
// Процедурные low-poly хелперы
// ---------------------------------------------------------------------------
MeshData TransformPart(const MeshData& src, const Mat4& xf) {
    MeshData out = src;
    const Mat4 nrm = xf.NormalMatrix();
    for (Vertex& v : out.vertices) {
        v.position = xf.TransformPoint(v.position);
        v.normal = Normalize(nrm.TransformDir(v.normal));
        const Vec3 t = nrm.TransformDir(Vec3{v.tangent.x, v.tangent.y, v.tangent.z});
        const f32 len = Length(t);
        if (len > kEpsilon) v.tangent = Vec4{t / len, v.tangent.w};
    }
    out.bounds = Bounds{};
    for (const Vertex& v : out.vertices) out.bounds.Expand(v.position);
    return out;
}

// Расщепляет общие вершины, чтобы у каждого треугольника была своя плоская (гранёная) нормаль.
void Facet(MeshData* mesh) {
    if (!mesh || mesh->indices.size() < 3) return;
    std::vector<Vertex> verts;
    std::vector<u32> idx;
    verts.reserve(mesh->indices.size());
    idx.reserve(mesh->indices.size());
    for (usize i = 0; i + 2 < mesh->indices.size(); i += 3) {
        const u32 ia = mesh->indices[i], ib = mesh->indices[i + 1], ic = mesh->indices[i + 2];
        if (ia >= mesh->vertices.size() || ib >= mesh->vertices.size() || ic >= mesh->vertices.size()) continue;
        const Vertex& a = mesh->vertices[ia];
        const Vertex& b = mesh->vertices[ib];
        const Vertex& c = mesh->vertices[ic];
        Vec3 n = Normalize(Cross(b.position - a.position, c.position - a.position));
        if (LengthSq(n) < 0.25f) n = Vec3{0, 1, 0};
        for (const Vertex* src : {&a, &b, &c}) {
            Vertex v = *src;
            v.normal = n;
            idx.push_back(static_cast<u32>(verts.size()));
            verts.push_back(v);
        }
    }
    mesh->vertices = std::move(verts);
    mesh->indices = std::move(idx);
}

// Собирает одномешевую модель из трансформированных частей; возвращает габариты модели.
Bounds AssembleParts(Model* model, std::vector<MeshData>& parts, const std::vector<int>& partMats,
                     std::vector<Material> mats) {
    Bounds bounds;
    if (!model || parts.empty()) return bounds;

    MeshData merged = MeshData::Merge(parts);
    usize totalIndices = 0;
    for (const MeshData& p : parts) totalIndices += p.indices.size();
    Facet(&merged);  // low-poly вид: нормали по граням

    merged.subMeshes.clear();
    if (merged.indices.size() == totalIndices) {
        u32 off = 0;
        for (usize i = 0; i < parts.size(); ++i) {
            if (parts[i].indices.empty()) continue;
            MeshData::SubMesh sm;
            sm.indexOffset = off;
            sm.indexCount = static_cast<u32>(parts[i].indices.size());
            sm.materialIndex = (i < partMats.size()) ? partMats[i] : 0;
            sm.name = parts[i].name;
            off += sm.indexCount;
            merged.subMeshes.push_back(std::move(sm));
        }
    }
    if (merged.subMeshes.empty() && !merged.indices.empty()) {
        MeshData::SubMesh sm;
        sm.indexOffset = 0;
        sm.indexCount = static_cast<u32>(merged.indices.size());
        sm.materialIndex = 0;
        sm.name = merged.name;
        merged.subMeshes.push_back(std::move(sm));
    }

    bounds = Bounds{};
    for (const Vertex& v : merged.vertices) bounds.Expand(v.position);
    merged.bounds = bounds;
    merged.name = "lowpoly";

    model->Meshes().push_back(std::move(merged));
    model->Materials() = std::move(mats);
    ModelNode node;
    node.name = "root";
    node.mesh = 0;
    node.material = 0;
    model->Nodes().push_back(std::move(node));
    return bounds;
}

// ---------------------------------------------------------------------------
// Общий диспетчер импорта (трогает только публичные аксессоры Model).
// ---------------------------------------------------------------------------
bool ImportIntoModel(Model* model, const void* data, usize size, ModelFormat fmt, const std::string& name,
                     const std::string& baseDir, std::string* error) {
    if (!model) return false;
    auto& meshes = model->Meshes();
    auto& materials = model->Materials();
    auto& nodes = model->Nodes();
    auto& textures = model->Textures();
    std::string err;

    switch (fmt) {
        case ModelFormat::Obj: {
            const char* text = static_cast<const char*>(data);
            materials.clear();
            MeshData md;
            if (!ImportObjData(text, size, &md, &materials, &err)) {
                if (error) *error = err;
                return false;
            }
            md.name = name;
            meshes.push_back(std::move(md));

            std::vector<MtlInfo> mtls;
            for (const std::string& lib : ObjMtlLibraries(text, size)) {
                std::string path = lib;
                if (!baseDir.empty() && path[0] != '/') path = PathJoin(baseDir, path);
                const ByteBuffer bytes = ReadBinaryFile(path);
                if (bytes.empty()) {
                    ENG_LOGW("model", "OBJ: cannot read material library '%s'", path.c_str());
                    continue;
                }
                std::vector<MtlInfo> parsed =
                    ParseMtl(reinterpret_cast<const char*>(bytes.data()), bytes.size());
                mtls.insert(mtls.end(), std::make_move_iterator(parsed.begin()),
                            std::make_move_iterator(parsed.end()));
            }
            ApplyMtl(mtls, &materials, baseDir, &textures);
            return true;
        }
        case ModelFormat::Gltf:
        case ModelFormat::Glb: {
            const bool binary =
                (fmt == ModelFormat::Glb) || (size >= 4 && std::memcmp(data, "glTF", 4) == 0);
            const std::string previous = g_gltfBaseDir;
            g_gltfBaseDir = baseDir;
            const bool ok =
                ImportGltfInternal(data, size, binary, &meshes, &materials, &nodes, &textures, &err);
            g_gltfBaseDir = previous;
            if (!ok && error) *error = err;
            return ok;
        }
        case ModelFormat::Ply: {
            MeshData md;
            if (!ImportPlyData(data, size, &md, &err)) {
                if (error) *error = err;
                return false;
            }
            md.name = name;
            meshes.push_back(std::move(md));
            return true;
        }
        case ModelFormat::Stl: {
            MeshData md;
            if (!ImportStlData(data, size, &md, &err)) {
                if (error) *error = err;
                return false;
            }
            if (md.name.empty()) md.name = name;
            meshes.push_back(std::move(md));
            return true;
        }
        case ModelFormat::Vox:
            return ImportVoxData(data, size, &meshes, &materials, &textures, error);
        default:
            if (error) *error = "unsupported model format";
            return false;
    }
}

Bounds FinalizeImportedModel(Model* model) {
    auto& meshes = model->Meshes();
    auto& materials = model->Materials();
    auto& nodes = model->Nodes();

    for (MeshData& md : meshes) {
        if (md.subMeshes.empty() && !md.indices.empty()) {
            MeshData::SubMesh sm;
            sm.indexOffset = 0;
            sm.indexCount = static_cast<u32>(md.indices.size());
            sm.materialIndex = 0;
            sm.name = md.name;
            md.subMeshes.push_back(std::move(sm));
        }
        for (MeshData::SubMesh& sm : md.subMeshes)
            if (sm.materialIndex < 0) sm.materialIndex = 0;
    }
    if (materials.empty() && !meshes.empty()) {
        Material d;
        d.name = "default";
        materials.push_back(std::move(d));
    }
    for (MeshData& md : meshes)
        for (MeshData::SubMesh& sm : md.subMeshes)
            if (static_cast<usize>(sm.materialIndex) >= materials.size()) sm.materialIndex = 0;

    if (nodes.empty()) {
        for (usize i = 0; i < meshes.size(); ++i) {
            ModelNode n;
            n.name = meshes[i].name;
            n.mesh = static_cast<int>(i);
            n.material = meshes[i].subMeshes.empty() ? 0 : meshes[i].subMeshes[0].materialIndex;
            nodes.push_back(std::move(n));
        }
    }
    return ComputeNodeAwareBounds(meshes, nodes);
}

}  // namespace

// ---------------------------------------------------------------------------
// Model
// ---------------------------------------------------------------------------
bool Model::Load(const std::string& path) {
    Destroy();
    ByteBuffer data = ReadBinaryFile(path);
    if (data.empty()) {
        ENG_LOGE("model", "cannot read model file '%s'", path.c_str());
        return false;
    }
    const ModelFormat fmt = DetectFormat(path, data.data(), data.size());
    format_ = fmt;
    name_ = PathBase(path);
    if (fmt == ModelFormat::Unknown) {
        ENG_LOGE("model", "unknown model format: %s", path.c_str());
        return false;
    }
    if (fmt == ModelFormat::FbxLite) {
        ENG_LOGE("model", "FBX import is not supported: %s", path.c_str());
        return false;
    }
    std::string err;
    if (!ImportIntoModel(this, data.data(), data.size(), fmt, name_, PathDir(path), &err)) {
        ENG_LOGE("model", "failed to import '%s': %s", path.c_str(), err.c_str());
        return false;
    }
    bounds_ = FinalizeImportedModel(this);
    if (meshes_.empty()) {
        ENG_LOGE("model", "'%s' contains no geometry", path.c_str());
        return false;
    }
    ENG_LOGI("model", "loaded %s (%d mesh(es), %d material(s))", path.c_str(), MeshCount(), MaterialCount());
    return true;
}

bool Model::LoadFromMemory(const void* data, usize size, ModelFormat format, const std::string& name) {
    Destroy();
    if (!data || size == 0) {
        ENG_LOGE("model", "LoadFromMemory: empty input");
        return false;
    }
    ModelFormat fmt = format;
    const ModelFormat magic = FormatFromMagic(static_cast<const u8*>(data), size);
    if (fmt == ModelFormat::Unknown) {
        fmt = magic;
    } else if ((fmt == ModelFormat::Gltf || fmt == ModelFormat::Glb) &&
               (magic == ModelFormat::Gltf || magic == ModelFormat::Glb)) {
        fmt = magic;  // текстовый или бинарный glTF решается по содержимому
    }
    format_ = fmt;
    name_ = name.empty() ? std::string("memory") : name;
    if (fmt == ModelFormat::Unknown || fmt == ModelFormat::FbxLite) {
        ENG_LOGE("model", "LoadFromMemory: unsupported format for '%s'", name_.c_str());
        return false;
    }
    // Без пути нет базового каталога: внешние URI буферов/изображений glTF-импортёр
    // пропускает с предупреждением.
    std::string err;
    if (!ImportIntoModel(this, data, size, fmt, name_, std::string(), &err)) {
        ENG_LOGE("model", "failed to import '%s': %s", name_.c_str(), err.c_str());
        return false;
    }
    bounds_ = FinalizeImportedModel(this);
    if (meshes_.empty()) {
        ENG_LOGE("model", "'%s' contains no geometry", name_.c_str());
        return false;
    }
    return true;
}

void Model::Destroy() {
    meshes_.clear();
    materials_.clear();
    nodes_.clear();
    textures_.clear();
    gpuMeshes_.clear();
    bounds_ = crossrender::Bounds{};
    uploaded_ = false;
    format_ = ModelFormat::Unknown;
    name_.clear();
}

bool Model::ImportObj(const char* text, usize size, MeshData* outMesh, std::vector<Material>* outMaterials,
                      std::string* error) {
    if (!outMesh) {
        if (error) *error = "no output mesh";
        return false;
    }
    return ImportObjData(text, size, outMesh, outMaterials, error);
}

bool Model::ImportGltf(const void* data, usize size, bool binary, std::vector<MeshData>* outMeshes,
                       std::vector<Material>* outMaterials, std::vector<ModelNode>* outNodes,
                       std::vector<Texture>* outTextures, std::string* error) {
    return ImportGltfInternal(data, size, binary, outMeshes, outMaterials, outNodes, outTextures, error);
}

bool Model::ImportPly(const void* data, usize size, MeshData* outMesh, std::string* error) {
    return ImportPlyData(data, size, outMesh, error);
}

bool Model::ImportStl(const void* data, usize size, MeshData* outMesh, std::string* error) {
    return ImportStlData(data, size, outMesh, error);
}

void Model::UploadToGpu() {
    if (uploaded_) return;
    gpuMeshes_.clear();
    gpuMeshes_.resize(meshes_.size());
    bool ok = !meshes_.empty();
    for (usize i = 0; i < meshes_.size(); ++i) {
        gpuMeshes_[i].SetName(meshes_[i].name);
        if (!gpuMeshes_[i].Create(meshes_[i])) {
            ENG_LOGW("model", "GPU upload failed for mesh %u ('%s')", static_cast<unsigned>(i),
                     meshes_[i].name.c_str());
            ok = false;
        }
    }
    uploaded_ = ok;
}

void Model::BakeTransform(const Mat4& m) {
    const Mat4 nrm = m.NormalMatrix();
    for (MeshData& mesh : meshes_) {
        for (Vertex& v : mesh.vertices) {
            v.position = m.TransformPoint(v.position);
            v.normal = Normalize(nrm.TransformDir(v.normal));
            const Vec3 t = nrm.TransformDir(Vec3{v.tangent.x, v.tangent.y, v.tangent.z});
            const f32 len = Length(t);
            if (len > kEpsilon) v.tangent = Vec4{t / len, v.tangent.w};
        }
        RepairBounds(&mesh);
    }
    gpuMeshes_.clear();  // GPU-копии устаревают после запекания
    uploaded_ = false;
    bounds_ = ComputeNodeAwareBounds(meshes_, nodes_);
}

void Model::EnsureNormals() {
    for (MeshData& mesh : meshes_) {
        if (!HasZeroNormal(mesh)) continue;
        if (mesh.indices.size() >= 3) mesh.ComputeNormals(true);
        RepairZeroNormals(&mesh, true);
    }
}

// ---------------------------------------------------------------------------
// Процедурные low-poly объекты.
// Детерминированные (засеянный crossrender::Random), собираются из примитивов MeshData,
// которые считаются центрированными относительно своего локального начала.
// ---------------------------------------------------------------------------
Model Model::MakeLowPolyTree(u64 seed) {
    Random rng(seed);
    std::vector<MeshData> parts;
    std::vector<int> partMats;
    std::vector<Material> mats;

    Material bark;
    bark.name = "bark";
    bark.baseColor = Color(0.30f, 0.19f, 0.11f, 1.0f);
    bark.roughness = 0.95f;
    Material leafA;
    leafA.name = "leaf_dark";
    leafA.baseColor = Color(0.13f, 0.40f, 0.16f, 1.0f);
    leafA.roughness = 0.85f;
    Material leafB;
    leafB.name = "leaf_light";
    leafB.baseColor = Color(0.22f, 0.56f, 0.24f, 1.0f);
    leafB.roughness = 0.85f;
    mats.push_back(bark);
    mats.push_back(leafA);
    mats.push_back(leafB);

    const f32 trunkH = rng.Range(1.5f, 2.1f);
    const f32 trunkR = rng.Range(0.12f, 0.18f);
    MeshData trunk = MeshData::Cylinder(trunkR, trunkH, 8, true);
    trunk.name = "trunk";
    parts.push_back(TransformPart(trunk, Mat4::Translate(Vec3{0, trunkH * 0.5f, 0})));
    partMats.push_back(0);

    const int layers = 3;
    const f32 base = trunkH * 0.72f;
    for (int i = 0; i < layers; ++i) {
        const f32 shrink = 1.0f - 0.22f * static_cast<f32>(i);
        const f32 r = rng.Range(0.70f, 0.95f) * shrink;
        const f32 h = rng.Range(0.85f, 1.15f) * shrink;
        const bool rounded = (i == layers - 1);
        const f32 y = base + static_cast<f32>(i) * h * 0.50f + h * (rounded ? 0.85f : 0.5f);
        MeshData canopy = rounded ? MeshData::IcoSphere(r * 1.05f, 1) : MeshData::Cone(r, h, 8);
        canopy.name = rounded ? "top" : "foliage";
        const f32 yaw = rng.Range(0.0f, kTau);
        parts.push_back(TransformPart(canopy, Mat4::Translate(Vec3{0, y, 0}) * Mat4::RotateY(yaw)));
        partMats.push_back(1 + (i % 2));
    }

    Model model;
    model.bounds_ = AssembleParts(&model, parts, partMats, std::move(mats));
    model.format_ = ModelFormat::Unknown;
    model.name_ = "lowpoly_tree";
    return model;
}

Model Model::MakeLowPolyRock(u64 seed) {
    Random rng(seed);
    std::vector<MeshData> parts;
    std::vector<int> partMats;
    std::vector<Material> mats;

    Material stone;
    stone.name = "rock";
    stone.baseColor = Color(0.42f, 0.42f, 0.45f, 1.0f);
    stone.roughness = 0.95f;
    Material moss;
    moss.name = "moss";
    moss.baseColor = Color(0.28f, 0.40f, 0.22f, 1.0f);
    moss.roughness = 0.9f;
    mats.push_back(stone);
    mats.push_back(moss);

    const int lumps = 2 + rng.RangeInt(0, 1);
    for (int i = 0; i < lumps; ++i) {
        MeshData rock = MeshData::IcoSphere(rng.Range(0.55f, 0.85f), 1);
        rock.name = (i == 0) ? "rock" : "rock_small";
        Random jitter(rng.NextU64());
        for (Vertex& v : rock.vertices) {
            v.position = v.position * jitter.Range(0.82f, 1.18f);
            v.position.y *= jitter.Range(0.65f, 0.95f);
        }
        const Vec3 offset{rng.Range(-0.35f, 0.35f), rng.Range(-0.05f, 0.05f), rng.Range(-0.35f, 0.35f)};
        parts.push_back(TransformPart(rock, Mat4::Translate(offset) * Mat4::RotateY(rng.Range(0.0f, kTau))));
        partMats.push_back((i == 0 || rng.Chance(0.4f)) ? 0 : 1);
    }

    Model model;
    model.bounds_ = AssembleParts(&model, parts, partMats, std::move(mats));
    model.format_ = ModelFormat::Unknown;
    model.name_ = "lowpoly_rock";
    // Камни — напольные объекты: ставим их на y = 0.
    if (model.bounds_.Valid() && std::fabs(model.bounds_.min.y) > 1e-4f)
        model.BakeTransform(Mat4::Translate(Vec3{0, -model.bounds_.min.y, 0}));
    return model;
}

Model Model::MakeLowPolyCrystal(u64 seed) {
    Random rng(seed);
    std::vector<MeshData> parts;
    std::vector<int> partMats;
    std::vector<Material> mats;

    Material core;
    core.name = "crystal_core";
    core.baseColor = Color(0.35f, 0.85f, 0.95f, 1.0f);
    core.emissive = Color(0.10f, 0.45f, 0.55f, 1.0f);
    core.emissiveStrength = 1.4f;
    core.roughness = 0.25f;
    Material shard;
    shard.name = "crystal_shard";
    shard.baseColor = Color(0.55f, 0.70f, 0.98f, 1.0f);
    shard.emissive = Color(0.12f, 0.20f, 0.40f, 1.0f);
    shard.emissiveStrength = 1.1f;
    shard.roughness = 0.3f;
    mats.push_back(core);
    mats.push_back(shard);

    const int shards = 4 + rng.RangeInt(0, 2);
    for (int i = 0; i < shards; ++i) {
        const f32 r = rng.Range(0.14f, 0.26f);
        const f32 h = rng.Range(0.7f, 1.4f);  // полная высота бипирамиды
        const f32 x = rng.Range(-0.35f, 0.35f);
        const f32 z = rng.Range(-0.35f, 0.35f);
        const f32 tilt = rng.Range(-0.25f, 0.25f);
        const f32 yaw = rng.Range(0.0f, kTau);
        const f32 baseY = h * 0.5f;
        // Бипирамида = два конуса основание к основанию с общей базовой плоскостью на baseY.
        MeshData upper = MeshData::Cone(r, h * 0.5f, 6);
        upper.name = "crystal";
        MeshData lower = MeshData::Cone(r, h * 0.5f, 6);
        lower.name = "crystal";
        const Mat4 upperXf =
            Mat4::Translate(Vec3{x, baseY + h * 0.25f, z}) * Mat4::RotateY(yaw) * Mat4::RotateZ(tilt);
        const Mat4 lowerXf = Mat4::Translate(Vec3{x, baseY - h * 0.25f, z}) * Mat4::RotateY(yaw) *
                             Mat4::RotateZ(tilt) * Mat4::RotateX(kPi);
        parts.push_back(TransformPart(upper, upperXf));
        partMats.push_back(i == 0 ? 0 : 1);
        parts.push_back(TransformPart(lower, lowerXf));
        partMats.push_back(1);
    }

    Model model;
    model.bounds_ = AssembleParts(&model, parts, partMats, std::move(mats));
    model.format_ = ModelFormat::Unknown;
    model.name_ = "lowpoly_crystal";
    return model;
}

Model Model::MakeLowPolyCharacter() {
    Random rng(0xBEEF1234ULL);
    std::vector<MeshData> parts;
    std::vector<int> partMats;
    std::vector<Material> mats;

    Material skin;
    skin.name = "skin";
    skin.baseColor = Color(0.92f, 0.74f, 0.60f, 1.0f);
    skin.roughness = 0.75f;
    Material shirt;
    shirt.name = "shirt";
    shirt.baseColor = Color(0.80f, 0.24f, 0.22f, 1.0f);
    shirt.roughness = 0.8f;
    Material pants;
    pants.name = "pants";
    pants.baseColor = Color(0.18f, 0.22f, 0.42f, 1.0f);
    pants.roughness = 0.85f;
    Material hair;
    hair.name = "hair";
    hair.baseColor = Color(0.16f, 0.12f, 0.10f, 1.0f);
    hair.roughness = 0.9f;
    mats.push_back(skin);
    mats.push_back(shirt);
    mats.push_back(pants);
    mats.push_back(hair);

    // Ноги.
    for (int side = 0; side < 2; ++side) {
        const f32 x = (side == 0) ? -0.13f : 0.13f;
        MeshData leg = MeshData::Cylinder(0.085f, 0.62f, 6, true);
        leg.name = "leg";
        parts.push_back(TransformPart(leg, Mat4::Translate(Vec3{x, 0.31f, 0})));
        partMats.push_back(2);
    }
    // Торс.
    MeshData torso = MeshData::Cylinder(0.20f, 0.58f, 8, true);
    torso.name = "torso";
    parts.push_back(TransformPart(torso, Mat4::Translate(Vec3{0, 0.90f, 0})));
    partMats.push_back(1);
    // Руки.
    for (int side = 0; side < 2; ++side) {
        const f32 x = (side == 0) ? -0.27f : 0.27f;
        MeshData arm = MeshData::Cylinder(0.06f, 0.52f, 6, true);
        arm.name = "arm";
        const Mat4 xf = Mat4::Translate(Vec3{x, 0.92f, 0}) * Mat4::RotateZ(side == 0 ? 0.18f : -0.18f);
        parts.push_back(TransformPart(arm, xf));
        partMats.push_back(1);
    }
    // Голова + нос.
    MeshData head = MeshData::IcoSphere(0.175f, 1);
    head.name = "head";
    for (Vertex& v : head.vertices) v.position.y *= 1.05f;
    parts.push_back(TransformPart(head, Mat4::Translate(Vec3{0, 1.36f, 0})));
    partMats.push_back(0);
    MeshData nose = MeshData::Cone(0.035f, 0.09f, 4);
    nose.name = "nose";
    parts.push_back(TransformPart(nose, Mat4::Translate(Vec3{0, 1.34f, 0.17f}) * Mat4::RotateX(1.5708f)));
    partMats.push_back(0);
    // Волосы (шапочка, сплюснутая нижняя половина).
    MeshData cap = MeshData::IcoSphere(0.19f, 1);
    cap.name = "hair";
    for (Vertex& v : cap.vertices) {
        if (v.position.y < 0.0f) v.position.y *= 0.25f;
    }
    parts.push_back(
        TransformPart(cap, Mat4::Translate(Vec3{0, 1.40f, 0}) * Mat4::RotateZ(rng.Range(-0.1f, 0.1f))));
    partMats.push_back(3);

    Model model;
    model.bounds_ = AssembleParts(&model, parts, partMats, std::move(mats));
    model.format_ = ModelFormat::Unknown;
    model.name_ = "lowpoly_character";
    return model;
}

}  // namespace crossrender
