#include "crossrender/gfx/Mesh.h"

#include "crossrender/gfx/GL.h"
#include "crossrender/core/Log.h"

#include <map>
#include <unordered_map>

namespace crossrender {
namespace {

// Интерливинг-раскладка (чересстрочная) под структуру Vertex движка.
constexpr u32 kAttrPosition = 0;
constexpr u32 kAttrNormal = 1;
constexpr u32 kAttrUV = 2;
constexpr u32 kAttrTangent = 3;
constexpr u32 kAttrColor = 4;
constexpr u32 kAttrUV2 = 5;

}  // namespace

// ---------------------------------------------------------------------------
// MeshData
// ---------------------------------------------------------------------------
void MeshData::ComputeBounds() {
    bounds = Bounds{};
    bool any = false;
    for (const Vertex& v : vertices) {
        bounds.Expand(v.position);
        any = true;
    }
    if (!any) bounds = Bounds{};
}

void MeshData::ComputeNormals(bool smooth) {
    if (vertices.empty()) return;
    for (Vertex& v : vertices) v.normal = Vec3{0, 0, 0};
    const usize triCount = indices.size() / 3;
    for (usize t = 0; t < triCount; ++t) {
        u32 i0 = indices[t * 3 + 0], i1 = indices[t * 3 + 1], i2 = indices[t * 3 + 2];
        if (i0 >= vertices.size() || i1 >= vertices.size() || i2 >= vertices.size()) continue;
        Vec3 a = vertices[i0].position, b = vertices[i1].position, c = vertices[i2].position;
        Vec3 n = Cross(b - a, c - a);
        if (!smooth) {
            vertices[i0].normal = n;
            vertices[i1].normal = n;
            vertices[i2].normal = n;
        } else {
            vertices[i0].normal += n;
            vertices[i1].normal += n;
            vertices[i2].normal += n;
        }
    }
    for (Vertex& v : vertices) {
        f32 len = Length(v.normal);
        v.normal = len > kEpsilon ? v.normal / len : Vec3{0, 1, 0};
    }
}

void MeshData::ComputeTangents() {
    for (Vertex& v : vertices) v.tangent = Vec4{0, 0, 0, 1};
    std::vector<Vec3> tan(vertices.size()), bitan(vertices.size());
    const usize triCount = indices.size() / 3;
    for (usize t = 0; t < triCount; ++t) {
        u32 i0 = indices[t * 3 + 0], i1 = indices[t * 3 + 1], i2 = indices[t * 3 + 2];
        if (i0 >= vertices.size() || i1 >= vertices.size() || i2 >= vertices.size()) continue;
        const Vertex &v0 = vertices[i0], &v1 = vertices[i1], &v2 = vertices[i2];
        Vec3 e1 = v1.position - v0.position;
        Vec3 e2 = v2.position - v0.position;
        Vec2 d1 = v1.uv - v0.uv;
        Vec2 d2 = v2.uv - v0.uv;
        f32 det = d1.x * d2.y - d2.x * d1.y;
        if (std::fabs(det) < 1e-8f) continue;
        f32 r = 1.0f / det;
        Vec3 T = (e1 * d2.y - e2 * d1.y) * r;
        Vec3 B = (e2 * d1.x - e1 * d2.x) * r;
        tan[i0] += T; tan[i1] += T; tan[i2] += T;
        bitan[i0] += B; bitan[i1] += B; bitan[i2] += B;
    }
    for (usize i = 0; i < vertices.size(); ++i) {
        Vec3 n = vertices[i].normal;
        Vec3 t = tan[i];
        if (LengthSq(t) < 1e-10f) {
            vertices[i].tangent = Vec4{1, 0, 0, 1};
            continue;
        }
        t = Normalize(t - n * Dot(n, t));
        f32 w = (Dot(Cross(n, t), bitan[i]) < 0.0f) ? -1.0f : 1.0f;
        vertices[i].tangent = Vec4{t, w};
    }
}

namespace {

void PushQuad(MeshData& m, const Vec3& a, const Vec3& b, const Vec3& c, const Vec3& d,
              const Vec3& normal, Vec2 uvScale) {
    u32 base = static_cast<u32>(m.vertices.size());
    auto push = [&](const Vec3& p, Vec2 uv) {
        Vertex v;
        v.position = p;
        v.normal = normal;
        v.uv = uv;
        v.color = Color::White.ToVec4();
        m.vertices.push_back(v);
    };
    push(a, {0, 0});
    push(b, {uvScale.x, 0});
    push(c, {uvScale.x, uvScale.y});
    push(d, {0, uvScale.y});
    m.indices.insert(m.indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
}

}  // namespace

MeshData MeshData::Quad(f32 w, f32 h) {
    MeshData m;
    m.name = "quad";
    f32 hw = w * 0.5f, hh = h * 0.5f;
    PushQuad(m, {-hw, -hh, 0}, {hw, -hh, 0}, {hw, hh, 0}, {-hw, hh, 0}, {0, 0, 1}, {1, 1});
    m.ComputeBounds();
    return m;
}

MeshData MeshData::Plane(f32 width, f32 depth, int subdiv, Vec2 uvScale) {
    MeshData m;
    m.name = "plane";
    subdiv = subdiv < 1 ? 1 : subdiv;
    f32 hw = width * 0.5f, hd = depth * 0.5f;
    for (int z = 0; z <= subdiv; ++z) {
        for (int x = 0; x <= subdiv; ++x) {
            f32 u = static_cast<f32>(x) / subdiv;
            f32 v = static_cast<f32>(z) / subdiv;
            Vertex vert;
            vert.position = {-hw + u * width, 0.0f, -hd + v * depth};
            vert.normal = {0, 1, 0};
            vert.uv = {u * uvScale.x, v * uvScale.y};
            m.vertices.push_back(vert);
        }
    }
    int row = subdiv + 1;
    for (int z = 0; z < subdiv; ++z) {
        for (int x = 0; x < subdiv; ++x) {
            u32 i0 = static_cast<u32>(z * row + x);
            u32 i1 = i0 + 1;
            u32 i2 = i0 + static_cast<u32>(row);
            u32 i3 = i2 + 1;
            // +u вдоль +x, +v вдоль +z, нормаль +y: такой порядок смотрит вверх.
            m.indices.insert(m.indices.end(), {i0, i2, i1, i1, i2, i3});
        }
    }
    m.ComputeBounds();
    return m;
}

MeshData MeshData::Cube(f32 size, bool smoothNormals) {
    MeshData m;
    m.name = "cube";
    f32 h = size * 0.5f;
    // +X, -X, +Y, -Y, +Z, -Z
    PushQuad(m, {h, -h, h}, {h, -h, -h}, {h, h, -h}, {h, h, h}, {1, 0, 0}, {1, 1});
    PushQuad(m, {-h, -h, -h}, {-h, -h, h}, {-h, h, h}, {-h, h, -h}, {-1, 0, 0}, {1, 1});
    PushQuad(m, {-h, h, h}, {h, h, h}, {h, h, -h}, {-h, h, -h}, {0, 1, 0}, {1, 1});
    PushQuad(m, {-h, -h, -h}, {h, -h, -h}, {h, -h, h}, {-h, -h, h}, {0, -1, 0}, {1, 1});
    PushQuad(m, {-h, -h, h}, {h, -h, h}, {h, h, h}, {-h, h, h}, {0, 0, 1}, {1, 1});
    PushQuad(m, {h, -h, -h}, {-h, -h, -h}, {-h, h, -h}, {h, h, -h}, {0, 0, -1}, {1, 1});
    if (smoothNormals) {
        for (Vertex& v : m.vertices) v.normal = Normalize(v.position);
    }
    m.ComputeBounds();
    return m;
}

MeshData MeshData::Sphere(f32 radius, int segments, int rings) {
    MeshData m;
    m.name = "sphere";
    segments = segments < 3 ? 3 : segments;
    rings = rings < 2 ? 2 : rings;
    for (int y = 0; y <= rings; ++y) {
        f32 v = static_cast<f32>(y) / rings;
        f32 phi = v * kPi;
        for (int x = 0; x <= segments; ++x) {
            f32 u = static_cast<f32>(x) / segments;
            f32 theta = u * kTau;
            Vec3 n{std::sin(phi) * std::cos(theta), std::cos(phi), std::sin(phi) * std::sin(theta)};
            Vertex vert;
            vert.position = n * radius;
            vert.normal = n;
            vert.uv = {u, 1.0f - v};
            vert.tangent = Vec4{Normalize(Vec3{-n.z, 0, n.x}), 1};
            m.vertices.push_back(vert);
        }
    }
    int row = segments + 1;
    for (int y = 0; y < rings; ++y) {
        for (int x = 0; x < segments; ++x) {
            u32 i0 = static_cast<u32>(y * row + x);
            u32 i1 = i0 + 1;
            u32 i2 = i0 + static_cast<u32>(row);
            u32 i3 = i2 + 1;
            m.indices.insert(m.indices.end(), {i0, i1, i2, i1, i3, i2});
        }
    }
    m.ComputeBounds();
    return m;
}

MeshData MeshData::IcoSphere(f32 radius, int subdivisions) {
    MeshData m;
    m.name = "icosphere";
    const f32 t = (1.0f + std::sqrt(5.0f)) * 0.5f;
    std::vector<Vec3> verts = {{-1, t, 0}, {1, t, 0}, {-1, -t, 0}, {1, -t, 0},
                               {0, -1, t}, {0, 1, t}, {0, -1, -t}, {0, 1, -t},
                               {t, 0, -1}, {t, 0, 1}, {-t, 0, -1}, {-t, 0, 1}};
    for (Vec3& v : verts) v = Normalize(v);
    std::vector<u32> idx = {0, 11, 5, 0, 5, 1, 0, 1, 7,  0, 7, 10, 0, 10, 11,
                            1, 5, 9,  5, 11, 4, 11, 10, 2, 10, 7, 6, 7, 1, 8,
                            3, 9, 4,  3, 4, 2, 3, 2, 6,  3, 6, 8, 3, 8, 9,
                            4, 9, 5,  2, 4, 11, 6, 2, 10, 8, 6, 7, 9, 8, 1};
    for (int s = 0; s < subdivisions; ++s) {
        std::unordered_map<u64, u32> cache;
        std::vector<u32> newIdx;
        auto midpoint = [&](u32 a, u32 b) -> u32 {
            u64 key = a < b ? (static_cast<u64>(a) << 32 | b) : (static_cast<u64>(b) << 32 | a);
            auto it = cache.find(key);
            if (it != cache.end()) return it->second;
            Vec3 mid = Normalize((verts[a] + verts[b]) * 0.5f);
            u32 id = static_cast<u32>(verts.size());
            verts.push_back(mid);
            cache[key] = id;
            return id;
        };
        for (usize i = 0; i + 2 < idx.size(); i += 3) {
            u32 a = idx[i], b = idx[i + 1], c = idx[i + 2];
            u32 ab = midpoint(a, b), bc = midpoint(b, c), ca = midpoint(c, a);
            newIdx.insert(newIdx.end(), {a, ab, ca, b, bc, ab, c, ca, bc, ab, bc, ca});
        }
        idx.swap(newIdx);
    }
    for (const Vec3& p : verts) {
        Vertex v;
        v.position = p * radius;
        v.normal = p;
        v.uv = {std::atan2(p.z, p.x) / kTau + 0.5f, std::asin(Clamp(p.y, -1.0f, 1.0f)) / kPi + 0.5f};
        m.vertices.push_back(v);
    }
    m.indices = idx;
    m.ComputeTangents();
    m.ComputeBounds();
    return m;
}

MeshData MeshData::Cylinder(f32 radius, f32 height, int segments, bool capped) {
    MeshData m;
    m.name = "cylinder";
    segments = segments < 3 ? 3 : segments;
    f32 hh = height * 0.5f;
    for (int i = 0; i <= segments; ++i) {
        f32 u = static_cast<f32>(i) / segments;
        f32 a = u * kTau;
        Vec3 n{std::cos(a), 0, std::sin(a)};
        Vertex v;
        v.position = {n.x * radius, -hh, n.z * radius};
        v.normal = n;
        v.uv = {u, 1};
        m.vertices.push_back(v);
        v.position = {n.x * radius, hh, n.z * radius};
        v.uv = {u, 0};
        m.vertices.push_back(v);
    }
    for (int i = 0; i < segments; ++i) {
        u32 i0 = static_cast<u32>(i * 2);
        m.indices.insert(m.indices.end(), {i0, i0 + 1, i0 + 2, i0 + 1, i0 + 3, i0 + 2});
    }
    if (capped) {
        for (int side = 0; side < 2; ++side) {
            f32 y = side == 0 ? hh : -hh;
            Vec3 n{0, side == 0 ? 1.0f : -1.0f, 0};
            u32 center = static_cast<u32>(m.vertices.size());
            Vertex c;
            c.position = {0, y, 0};
            c.normal = n;
            c.uv = {0.5f, 0.5f};
            m.vertices.push_back(c);
            for (int i = 0; i <= segments; ++i) {
                f32 a = static_cast<f32>(i) / segments * kTau;
                Vertex v;
                v.position = {std::cos(a) * radius, y, std::sin(a) * radius};
                v.normal = n;
                v.uv = {std::cos(a) * 0.5f + 0.5f, std::sin(a) * 0.5f + 0.5f};
                m.vertices.push_back(v);
            }
            for (int i = 0; i < segments; ++i) {
                u32 a = center + 1 + static_cast<u32>(i);
                u32 b = a + 1;
                if (side == 0)
                    m.indices.insert(m.indices.end(), {center, b, a});
                else
                    m.indices.insert(m.indices.end(), {center, a, b});
            }
        }
    }
    m.ComputeBounds();
    return m;
}

MeshData MeshData::Cone(f32 radius, f32 height, int segments) {
    MeshData m;
    m.name = "cone";
    segments = segments < 3 ? 3 : segments;
    f32 hh = height * 0.5f;
    for (int i = 0; i < segments; ++i) {
        f32 a0 = static_cast<f32>(i) / segments * kTau;
        f32 a1 = static_cast<f32>(i + 1) / segments * kTau;
        Vec3 p0{std::cos(a0) * radius, -hh, std::sin(a0) * radius};
        Vec3 p1{std::cos(a1) * radius, -hh, std::sin(a1) * radius};
        Vec3 tip{0, hh, 0};
        // Боковая нормаль должна смотреть от оси и вверх по склону. При
        // порядке вершин p0, p1 по ободу основания cross(tip - p0,
        // p1 - p0) даёт внешнюю нормаль; обратный порядок указывает внутрь конуса.
        Vec3 n = Normalize(Cross(tip - p0, p1 - p0));
        u32 base = static_cast<u32>(m.vertices.size());
        Vertex v;
        v.position = p0; v.normal = n; v.uv = {0, 1}; m.vertices.push_back(v);
        v.position = p1; v.normal = n; v.uv = {1, 1}; m.vertices.push_back(v);
        v.position = tip; v.normal = n; v.uv = {0.5f, 0}; m.vertices.push_back(v);
        m.indices.insert(m.indices.end(), {base, base + 2, base + 1});
        // Нижняя крышка
        u32 capBase = static_cast<u32>(m.vertices.size());
        v.position = {0, -hh, 0}; v.normal = {0, -1, 0}; v.uv = {0.5f, 0.5f};
        m.vertices.push_back(v);
        v.position = p1; v.normal = {0, -1, 0}; v.uv = {0.5f + std::cos(a1) * 0.5f, 0.5f + std::sin(a1) * 0.5f};
        m.vertices.push_back(v);
        v.position = p0; v.normal = {0, -1, 0}; v.uv = {0.5f + std::cos(a0) * 0.5f, 0.5f + std::sin(a0) * 0.5f};
        m.vertices.push_back(v);
        // Крышка смотрит вниз, поэтому её лицевые треугольники должны
        // обходиться в другом направлении, чем боковые выше.
        m.indices.insert(m.indices.end(), {capBase, capBase + 2, capBase + 1});
    }
    m.ComputeBounds();
    return m;
}

MeshData MeshData::Torus(f32 major, f32 minor, int majorSeg, int minorSeg) {
    MeshData m;
    m.name = "torus";
    for (int i = 0; i <= majorSeg; ++i) {
        f32 u = static_cast<f32>(i) / majorSeg;
        f32 a = u * kTau;
        Vec3 center{std::cos(a) * major, 0, std::sin(a) * major};
        Vec3 outward{std::cos(a), 0, std::sin(a)};
        for (int j = 0; j <= minorSeg; ++j) {
            f32 v = static_cast<f32>(j) / minorSeg;
            f32 b = v * kTau;
            Vec3 n = outward * std::cos(b) + Vec3{0, 1, 0} * std::sin(b);
            Vertex vert;
            vert.position = center + n * minor;
            vert.normal = n;
            vert.uv = {u, v};
            m.vertices.push_back(vert);
        }
    }
    int row = minorSeg + 1;
    for (int i = 0; i < majorSeg; ++i) {
        for (int j = 0; j < minorSeg; ++j) {
            u32 i0 = static_cast<u32>(i * row + j);
            u32 i1 = i0 + 1;
            u32 i2 = i0 + static_cast<u32>(row);
            u32 i3 = i2 + 1;
            m.indices.insert(m.indices.end(), {i0, i1, i2, i1, i3, i2});
        }
    }
    m.ComputeBounds();
    return m;
}

MeshData MeshData::Capsule(f32 radius, f32 height, int segments, int rings) {
    MeshData m;
    m.name = "capsule";
    f32 cylinderHalf = MaxT(0.0f, height * 0.5f - radius);
    int halfRings = MaxT(2, rings / 2);
    // Верхняя полусфера + цилиндр + нижняя полусфера.
    std::vector<f32> ys;
    std::vector<f32> radial;
    for (int i = 0; i <= halfRings; ++i) {
        f32 t = static_cast<f32>(i) / halfRings;
        f32 phi = t * kPi * 0.5f;
        ys.push_back(cylinderHalf + std::cos(phi) * radius);
        radial.push_back(std::sin(phi) * radius);
    }
    std::vector<f32> allY = ys;
    std::vector<f32> allR = radial;
    for (int i = halfRings; i >= 0; --i) {
        allY.push_back(-ys[static_cast<usize>(i)]);
        allR.push_back(radial[static_cast<usize>(i)]);
    }
    int rows = static_cast<int>(allY.size());
    for (int r = 0; r < rows; ++r) {
        for (int s = 0; s <= segments; ++s) {
            f32 u = static_cast<f32>(s) / segments;
            f32 a = u * kTau;
            Vec3 n{std::cos(a) * (allR[static_cast<usize>(r)] / MaxT(radius, kEpsilon)), 0,
                   std::sin(a) * (allR[static_cast<usize>(r)] / MaxT(radius, kEpsilon))};
            f32 dy = allY[static_cast<usize>(r)];
            f32 ny = dy > cylinderHalf ? (dy - cylinderHalf) / MaxT(radius, kEpsilon)
                                       : (dy < -cylinderHalf ? (dy + cylinderHalf) / MaxT(radius, kEpsilon) : 0.0f);
            Vertex v;
            v.position = {std::cos(a) * allR[static_cast<usize>(r)], dy,
                          std::sin(a) * allR[static_cast<usize>(r)]};
            v.normal = Normalize(Vec3{n.x, ny, n.z});
            v.uv = {u, static_cast<f32>(r) / (rows - 1)};
            m.vertices.push_back(v);
        }
    }
    int row = segments + 1;
    for (int r = 0; r < rows - 1; ++r) {
        for (int s = 0; s < segments; ++s) {
            u32 i0 = static_cast<u32>(r * row + s);
            u32 i1 = i0 + 1;
            u32 i2 = i0 + static_cast<u32>(row);
            u32 i3 = i2 + 1;
            m.indices.insert(m.indices.end(), {i0, i1, i2, i1, i3, i2});
        }
    }
    m.ComputeBounds();
    return m;
}

MeshData MeshData::Merge(const std::vector<MeshData>& parts) {
    MeshData out;
    out.name = "merged";
    for (const MeshData& p : parts) {
        u32 base = static_cast<u32>(out.vertices.size());
        out.vertices.insert(out.vertices.end(), p.vertices.begin(), p.vertices.end());
        for (u32 i : p.indices) out.indices.push_back(base + i);
    }
    out.ComputeBounds();
    return out;
}

// ---------------------------------------------------------------------------
// Mesh (GPU)
// ---------------------------------------------------------------------------
Mesh::~Mesh() { Destroy(); }

Mesh::Mesh(Mesh&& o) noexcept { *this = std::move(o); }

Mesh& Mesh::operator=(Mesh&& o) noexcept {
    if (this == &o) return *this;
    Destroy();
    vao_ = o.vao_;
    vbo_ = o.vbo_;
    ebo_ = o.ebo_;
    indexCount_ = o.indexCount_;
    vertexCount_ = o.vertexCount_;
    index32_ = o.index32_;
    bounds_ = o.bounds_;
    subMeshes_ = std::move(o.subMeshes_);
    name_ = std::move(o.name_);
    o.vao_ = o.vbo_ = o.ebo_ = 0;
    o.indexCount_ = o.vertexCount_ = 0;
    return *this;
}

bool Mesh::Create(const MeshData& data) {
    vertexCount_ = static_cast<u32>(data.vertices.size());
    indexCount_ = static_cast<u32>(data.indices.size());
    bounds_ = data.bounds;
    subMeshes_ = data.subMeshes;
    if (data.name.size()) name_ = data.name;
    if (data.bounds.Valid() == false && vertexCount_ > 0) {
        crossrender::Bounds b;
        for (const Vertex& v : data.vertices) b.Expand(v.position);
        bounds_ = b;
    }
    if (!gl::glGenVertexArrays || vertexCount_ == 0 || indexCount_ == 0) {
        if (vertexCount_ == 0) ENG_LOGW("mesh", "mesh '%s' has no vertices", name_.c_str());
        return false;
    }
    if (vao_ == 0) {
        gl::glGenVertexArrays(1, &vao_);
        gl::glGenBuffers(1, &vbo_);
        gl::glGenBuffers(1, &ebo_);
    }
    gl::glBindVertexArray(vao_);

    gl::glBindBuffer(gl::GL_ARRAY_BUFFER, vbo_);
    gl::glBufferData(gl::GL_ARRAY_BUFFER,
                     static_cast<gl::GLsizeiptr>(data.vertices.size() * sizeof(Vertex)),
                     data.vertices.data(), gl::GL_STATIC_DRAW);

    gl::glBindBuffer(gl::GL_ELEMENT_ARRAY_BUFFER, ebo_);
    gl::glBufferData(gl::GL_ELEMENT_ARRAY_BUFFER,
                     static_cast<gl::GLsizeiptr>(data.indices.size() * sizeof(u32)),
                     data.indices.data(), gl::GL_STATIC_DRAW);

    const gl::GLsizei stride = sizeof(Vertex);
    auto attrib = [&](u32 location, int comps, u32 offset, bool integer = false) {
        gl::glEnableVertexAttribArray(location);
        if (integer)
            gl::glVertexAttribIPointer(location, comps, gl::GL_UNSIGNED_INT, stride,
                                       reinterpret_cast<const void*>(static_cast<usize>(offset)));
        else
            gl::glVertexAttribPointer(location, comps, gl::GL_FLOAT, gl::GL_FALSE, stride,
                                      reinterpret_cast<const void*>(static_cast<usize>(offset)));
    };
    attrib(kAttrPosition, 3, offsetof(Vertex, position));
    attrib(kAttrNormal, 3, offsetof(Vertex, normal));
    attrib(kAttrUV, 2, offsetof(Vertex, uv));
    attrib(kAttrTangent, 4, offsetof(Vertex, tangent));
    attrib(kAttrColor, 4, offsetof(Vertex, color));
    attrib(kAttrUV2, 2, offsetof(Vertex, uv2));
    gl::glBindVertexArray(0);

    if (subMeshes_.empty() && indexCount_ > 0) {
        MeshData::SubMesh sm;
        sm.indexOffset = 0;
        sm.indexCount = indexCount_;
        subMeshes_.push_back(sm);
    }
    return true;
}

bool Mesh::Create(const void* vertices, usize vertexBytes, const std::vector<u32>& indices,
                  const std::vector<u32>& layout) {
    (void)vertices;
    (void)vertexBytes;
    (void)layout;
    vertexCount_ = static_cast<u32>(vertexBytes / sizeof(Vertex));
    indexCount_ = static_cast<u32>(indices.size());
    if (!gl::glGenVertexArrays) return false;
    gl::glGenVertexArrays(1, &vao_);
    gl::glGenBuffers(1, &vbo_);
    gl::glGenBuffers(1, &ebo_);
    gl::glBindVertexArray(vao_);
    gl::glBindBuffer(gl::GL_ARRAY_BUFFER, vbo_);
    gl::glBufferData(gl::GL_ARRAY_BUFFER, static_cast<gl::GLsizeiptr>(vertexBytes), vertices,
                     gl::GL_STATIC_DRAW);
    gl::glBindBuffer(gl::GL_ELEMENT_ARRAY_BUFFER, ebo_);
    gl::glBufferData(gl::GL_ELEMENT_ARRAY_BUFFER,
                     static_cast<gl::GLsizeiptr>(indices.size() * sizeof(u32)), indices.data(),
                     gl::GL_STATIC_DRAW);
    const gl::GLsizei stride = sizeof(Vertex);
    auto attrib = [&](u32 location, int comps, u32 offset) {
        gl::glEnableVertexAttribArray(location);
        gl::glVertexAttribPointer(location, comps, gl::GL_FLOAT, gl::GL_FALSE, stride,
                                  reinterpret_cast<const void*>(static_cast<usize>(offset)));
    };
    attrib(kAttrPosition, 3, offsetof(Vertex, position));
    attrib(kAttrNormal, 3, offsetof(Vertex, normal));
    attrib(kAttrUV, 2, offsetof(Vertex, uv));
    attrib(kAttrTangent, 4, offsetof(Vertex, tangent));
    attrib(kAttrColor, 4, offsetof(Vertex, color));
    attrib(kAttrUV2, 2, offsetof(Vertex, uv2));
    gl::glBindVertexArray(0);
    return true;
}

void Mesh::Destroy() {
    if (ebo_ && gl::glDeleteBuffers) gl::glDeleteBuffers(1, &ebo_);
    if (vbo_ && gl::glDeleteBuffers) gl::glDeleteBuffers(1, &vbo_);
    if (vao_ && gl::glDeleteVertexArrays) gl::glDeleteVertexArrays(1, &vao_);
    vao_ = vbo_ = ebo_ = 0;
    indexCount_ = vertexCount_ = 0;
    subMeshes_.clear();
}

void Mesh::Bind() const { gl::glBindVertexArray(vao_); }

void Mesh::Draw() const {
    if (!vao_ || indexCount_ == 0) return;
    gl::glBindVertexArray(vao_);
    gl::glDrawElements(gl::GL_TRIANGLES, static_cast<gl::GLsizei>(indexCount_), gl::GL_UNSIGNED_INT,
                       nullptr);
}

void Mesh::DrawSub(u32 indexOffset, u32 indexCount) const {
    if (!vao_ || indexCount == 0) return;
    gl::glBindVertexArray(vao_);
    gl::glDrawElements(gl::GL_TRIANGLES, static_cast<gl::GLsizei>(indexCount), gl::GL_UNSIGNED_INT,
                       reinterpret_cast<const void*>(static_cast<usize>(indexOffset) * sizeof(u32)));
}

void Mesh::DrawInstanced(int count) const {
    if (!vao_ || indexCount_ == 0 || count <= 0) return;
    gl::glBindVertexArray(vao_);
    gl::glDrawElementsInstanced(gl::GL_TRIANGLES, static_cast<gl::GLsizei>(indexCount_),
                                gl::GL_UNSIGNED_INT, nullptr, count);
}

void Mesh::DrawSubInstanced(u32 indexOffset, u32 indexCount, int count) const {
    if (!vao_ || indexCount == 0 || count <= 0) return;
    gl::glBindVertexArray(vao_);
    gl::glDrawElementsInstanced(gl::GL_TRIANGLES, static_cast<gl::GLsizei>(indexCount),
                                gl::GL_UNSIGNED_INT,
                                reinterpret_cast<const void*>(static_cast<usize>(indexOffset) * sizeof(u32)),
                                count);
}

Mesh* Mesh::GetPrimitive(const std::string& key) {
    static std::map<std::string, Mesh*> cache;
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;
    MeshData data;
    if (key == "cube")
        data = MeshData::Cube(1.0f);
    else if (key == "sphere")
        data = MeshData::Sphere(0.5f, 24, 16);
    else if (key == "quad")
        data = MeshData::Quad(1.0f, 1.0f);
    else if (key == "plane")
        data = MeshData::Plane(1.0f, 1.0f, 1);
    else if (key == "cylinder")
        data = MeshData::Cylinder(0.5f, 1.0f, 24);
    else if (key == "cone")
        data = MeshData::Cone(0.5f, 1.0f, 24);
    else if (key == "torus")
        data = MeshData::Torus(0.5f, 0.15f, 32, 16);
    else if (key == "capsule")
        data = MeshData::Capsule(0.35f, 1.2f, 20, 12);
    else
        data = MeshData::Cube(1.0f);
    Mesh* mesh = new Mesh();
    mesh->SetName(key);
    mesh->Create(data);
    cache[key] = mesh;
    return mesh;
}

// ---------------------------------------------------------------------------
// Пресеты материалов
// ---------------------------------------------------------------------------
Material Material::Default() {
    Material m;
    m.name = "Default";
    return m;
}

Material Material::Unlit(const Color& c) {
    Material m;
    m.name = "Unlit";
    m.unlit = true;
    m.baseColor = c;
    m.roughness = 1.0f;
    return m;
}

Material Material::Checker() {
    Material m;
    m.name = "Checker";
    m.baseColor = Color::White;
    m.roughness = 0.55f;
    return m;
}

Material Material::Metal(const Color& c, f32 roughness) {
    Material m;
    m.name = "Metal";
    m.baseColor = c;
    m.metallic = 1.0f;
    m.roughness = roughness;
    return m;
}

Material Material::Emissive(const Color& c, f32 strength) {
    Material m;
    m.name = "Emissive";
    m.emissive = c;
    m.emissiveStrength = strength;
    m.baseColor = Color{0.02f, 0.02f, 0.02f, 1.0f};
    m.roughness = 0.4f;
    return m;
}

// ---------------------------------------------------------------------------
// Пресеты источников света
// ---------------------------------------------------------------------------
Light Light::Directional(const Vec3& dir, const Color& c, f32 intensity, bool shadows) {
    Light l;
    l.type = LightType::Directional;
    l.direction = Normalize(dir);
    l.color = c;
    l.intensity = intensity;
    l.castShadows = shadows;
    return l;
}

Light Light::Point(const Vec3& pos, const Color& c, f32 intensity, f32 range, bool shadows) {
    Light l;
    l.type = LightType::Point;
    l.position = pos;
    l.color = c;
    l.intensity = intensity;
    l.range = range;
    l.castShadows = shadows;
    return l;
}

Light Light::Spot(const Vec3& pos, const Vec3& dir, const Color& c, f32 intensity, f32 range,
                  f32 inner, f32 outer, bool shadows) {
    Light l;
    l.type = LightType::Spot;
    l.position = pos;
    l.direction = Normalize(dir);
    l.color = c;
    l.intensity = intensity;
    l.range = range;
    l.innerCone = inner;
    l.outerCone = outer;
    l.castShadows = shadows;
    return l;
}

}  // namespace crossrender
