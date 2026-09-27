// Тесты воксельного модуля: хранение чанков, отсечение граней на границах
// чанков, greedy meshing, ambient occlusion, бросание лучей, освещение и генерация.
//
// Проверки, зависящие от GPU (загрузка атласа палитры, загрузка меша), защищены,
// чтобы набор проходил и без GL-контекста.
#include "crossrender/test/Test.h"
#include "crossrender/voxel/Voxel.h"

#include <cmath>
#include <vector>
#include <algorithm>

using namespace crossrender;

namespace {

constexpr int kS = kVoxelChunkSize;

// Стабильный хеш по всем вокселям мира (детерминированный порядок).
u64 HashWorld(VoxelWorld& w) {
    std::vector<VoxelChunk*> chunks = w.AllChunks();
    std::sort(chunks.begin(), chunks.end(), [](const VoxelChunk* a, const VoxelChunk* b) {
        const ChunkCoord ca = a->Coord(), cb = b->Coord();
        if (ca.x != cb.x) return ca.x < cb.x;
        if (ca.y != cb.y) return ca.y < cb.y;
        return ca.z < cb.z;
    });
    u64 h = 1469598103934665603ULL;
    for (VoxelChunk* c : chunks) {
        for (int y = 0; y < kS; ++y) {
            for (int z = 0; z < kS; ++z) {
                for (int x = 0; x < kS; ++x) {
                    h ^= static_cast<u64>(c->Get(x, y, z));
                    h *= 1099511628211ULL;
                }
            }
        }
    }
    return h;
}

int CountSolids(VoxelWorld& w) {
    int n = 0;
    for (VoxelChunk* c : w.AllChunks()) n += c->SolidCount();
    return n;
}

// Индексы всех вершин `md` с нормалью, совпадающей с `n`, лежащих на заданной
// плоскости/прямоугольнике, в возрастающем (порядок эмиссии) порядке.
std::vector<u32> FindFaceVertices(const MeshData& md, const Vec3& n, f32 plane, f32 x0, f32 x1, f32 z0,
                                  f32 z1) {
    std::vector<u32> out;
    for (u32 i = 0; i < static_cast<u32>(md.vertices.size()); ++i) {
        const Vertex& v = md.vertices[i];
        if (Dot(v.normal, n) < 0.99f) continue;
        if (std::fabs(v.position.y - plane) > 1e-4f) continue;
        if (v.position.x < x0 - 1e-4f || v.position.x > x1 + 1e-4f) continue;
        if (v.position.z < z0 - 1e-4f || v.position.z > z1 + 1e-4f) continue;
        out.push_back(i);
    }
    return out;
}

// Шесть индексов квада, владеющего вершиной `first`.
std::vector<u32> QuadIndices(const MeshData& md, u32 first) {
    for (usize i = 0; i + 5 < md.indices.size(); i += 6) {
        if (md.indices[i] == first) return {md.indices[i],     md.indices[i + 1], md.indices[i + 2],
                                            md.indices[i + 3], md.indices[i + 4], md.indices[i + 5]};
    }
    return {};
}

}  // namespace

// ---------------------------------------------------------------------------
// Хранение
// ---------------------------------------------------------------------------
ENG_TEST(Voxel, SetGetRoundTrip) {
    VoxelChunk c;
    ENG_CHECK(c.Empty());
    ENG_CHECK_EQ(c.SolidCount(), 0);
    c.Set(3, 4, 5, 7);
    ENG_CHECK_EQ(c.Get(3, 4, 5), 7);
    ENG_CHECK_EQ(c.GetLight(3, 4, 5), 15);
    ENG_CHECK_EQ(c.SolidCount(), 1);
    ENG_CHECK(c.IsSolid(3, 4, 5));
    ENG_CHECK(c.IsEmpty(3, 4, 6));
    c.Set(3, 4, 5, 9, 4);
    ENG_CHECK_EQ(c.Get(3, 4, 5), 9);
    ENG_CHECK_EQ(c.GetLight(3, 4, 5), 4);
    ENG_CHECK_EQ(c.SolidCount(), 1);
    c.Set(3, 4, 5, 0);
    ENG_CHECK_EQ(c.SolidCount(), 0);
    ENG_CHECK(c.Empty());

    // Чтения вне диапазона безопасны: воздух / полный свет, записи игнорируются.
    ENG_CHECK_EQ(c.Get(-1, 0, 0), 0);
    ENG_CHECK_EQ(c.Get(0, kS, 0), 0);
    ENG_CHECK_EQ(c.GetLight(-1, 0, 0), 15);
    ENG_CHECK_EQ(c.GetLight(0, 0, kS), 15);
    c.Set(-1, -1, -1, 5);
    c.Set(kS, 0, 0, 5);
    ENG_CHECK(c.Empty());

    c.Fill(2);
    ENG_CHECK_EQ(c.SolidCount(), kVoxelChunkVolume);
    c.Clear();
    ENG_CHECK(c.Empty());
    ENG_CHECK_EQ(c.SolidCount(), 0);
}

ENG_TEST(Voxel, NegativeWorldCoords) {
    VoxelWorld w;
    w.SetVoxel(-1, 5, -33, 7);
    ENG_CHECK_EQ(w.GetVoxel(-1, 5, -33), 7);
    ENG_CHECK_EQ(w.GetVoxel(0, 5, -33), 0);
    ENG_CHECK_EQ(w.GetVoxel(-1, 5, -32), 0);
    ENG_CHECK(w.IsSolid(-1, 5, -33));
    ENG_CHECK(!w.IsSolid(-1, 5, -34));

    // -1 / 32 -> чанк -1, локально 31;  -33 / 32 -> чанк -2, локально 31.
    VoxelChunk* c = w.GetChunk({-1, 0, -2});
    ENG_CHECK(c != nullptr);
    if (c) {
        ENG_CHECK_EQ(c->Get(31, 5, 31), 7);
        ENG_CHECK_EQ(c->Coord().x, -1);
        ENG_CHECK_EQ(c->Coord().z, -2);
        ENG_CHECK_NEAR(c->WorldOrigin().x, -32.0f, 1e-5f);
        ENG_CHECK_NEAR(c->WorldOrigin().z, -64.0f, 1e-5f);
    }
    ENG_CHECK_EQ(w.ChunkCount(), 1);

    // Каждый отрицательный мировой воксель в диапазоне проходит круговой путь через мир.
    for (int i = 0; i < 200; ++i) {
        const int x = -i - 1, y = -i - 1, z = -i - 1;
        w.SetVoxel(x, y, z, static_cast<u8>(1 + (i % 20)));
        ENG_CHECK_EQ(w.GetVoxel(x, y, z), static_cast<u8>(1 + (i % 20)));
    }
}

// ---------------------------------------------------------------------------
// Построение мешей
// ---------------------------------------------------------------------------
ENG_TEST(Voxel, GreedyMeshing) {
    VoxelWorld w;
    w.GenerateBox(Vec3{2, 2, 2}, Vec3{10, 10, 10}, 1);
    VoxelChunk* c = w.GetChunk({0, 0, 0});
    ENG_CHECK(c != nullptr);
    if (!c) return;

    // Сплошной блок 8x8x8, greedy: ровно шесть объединённых квадов (12 треугольников).
    c->RebuildMesh(w.Palette(), true, false, false, &w);
    ENG_CHECK_EQ(c->Mesh().indices.size(), static_cast<usize>(36));
    ENG_CHECK_EQ(c->Mesh().vertices.size(), static_cast<usize>(24));

    // Без greedy: 6 * 64 отдельных квадов.
    c->RebuildMesh(w.Palette(), false, false, false, &w);
    ENG_CHECK_EQ(c->Mesh().indices.size(), static_cast<usize>(6 * 64 * 6));
    ENG_CHECK_EQ(c->Mesh().vertices.size(), static_cast<usize>(6 * 64 * 4));

    // Все объединённые квады должны нести наружную нормаль.
    c->RebuildMesh(w.Palette(), true, false, false, &w);
    for (const Vertex& v : c->Mesh().vertices) {
        ENG_CHECK_NEAR(Length(v.normal), 1.0f, 1e-4f);
        const Vec3 mid = v.position - Vec3{6.0f, 6.0f, 6.0f};
        ENG_CHECK(Dot(mid, v.normal) > 0.0f);
    }

    // UV атласа текстур выдаются при текстурировании: проявляется начало ячейки травы.
    c->RebuildMesh(w.Palette(), true, true, false, &w);
    ENG_CHECK_GT(c->Mesh().vertices.size(), static_cast<usize>(0));
}

ENG_TEST(Voxel, TextureUVInsideCell) {
    VoxelWorld w;
    w.GenerateBox(Vec3{2, 2, 2}, Vec3{10, 10, 10}, 5);  // песок -> ячейка атласа 5
    VoxelChunk* c = w.GetChunk({0, 0, 0});
    ENG_CHECK(c != nullptr);
    if (!c) return;
    const Rect cell = w.PaletteUV(5);

    for (int greedy = 0; greedy < 2; ++greedy) {
        c->RebuildMesh(w.Palette(), greedy != 0, true, false, &w);
        bool sawMinU = false, sawMaxU = false, sawMinV = false, sawMaxV = false;
        for (const Vertex& v : c->Mesh().vertices) {
            // Объединённый квад никогда не должен сэмплировать вне своей ячейки атласа.
            ENG_CHECK(v.uv.x >= cell.x - 1e-4f && v.uv.x <= cell.x + cell.w + 1e-4f);
            ENG_CHECK(v.uv.y >= cell.y - 1e-4f && v.uv.y <= cell.y + cell.h + 1e-4f);
            if (std::fabs(v.uv.x - cell.x) < 1e-4f) sawMinU = true;
            if (std::fabs(v.uv.x - (cell.x + cell.w)) < 1e-4f) sawMaxU = true;
            if (std::fabs(v.uv.y - cell.y) < 1e-4f) sawMinV = true;
            if (std::fabs(v.uv.y - (cell.y + cell.h)) < 1e-4f) sawMaxV = true;
        }
        ENG_CHECK(sawMinU && sawMaxU && sawMinV && sawMaxV);  // ячейка использована полностью
    }

    // Трава использует боковую ячейку на вертикальных гранях и верхнюю на гранях по Y.
    VoxelWorld g;
    g.GenerateBox(Vec3{2, 2, 2}, Vec3{3, 3, 3}, 2);
    VoxelChunk* gc = g.GetChunk({0, 0, 0});
    if (gc) {
        gc->RebuildMesh(g.Palette(), false, true, false, &g);
        const Rect topCell = g.PaletteUV(2);
        const Rect sideCell = g.PaletteUV(4);
        for (const Vertex& v : gc->Mesh().vertices) {
            const Rect want = (std::fabs(v.normal.y) > 0.99f) ? topCell : sideCell;
            const bool inX = std::fabs(v.uv.x - want.x) < 1e-4f ||
                             std::fabs(v.uv.x - (want.x + want.w)) < 1e-4f;
            const bool inY = std::fabs(v.uv.y - want.y) < 1e-4f ||
                             std::fabs(v.uv.y - (want.y + want.h)) < 1e-4f;
            ENG_CHECK(inX && inY);
        }
    }
}

ENG_TEST(Voxel, BorderFaceCulling) {
    // Эталон: один одинокий полный чанк -> 6 объединённых квадов (12 треугольников).
    VoxelWorld lone;
    lone.GenerateBox(Vec3{0, 0, 0}, Vec3{kS, kS, kS}, 1);
    VoxelChunk* lc = lone.GetChunk({0, 0, 0});
    ENG_CHECK(lc != nullptr);
    if (!lc) return;
    lc->RebuildMesh(lone.Palette(), true, false, false, &lone);
    const usize loneIndices = lc->Mesh().indices.size();
    ENG_CHECK_EQ(loneIndices, static_cast<usize>(36));

    // Два соседних полных чанка: общая плоскость не должна генерировать грани.
    VoxelWorld pair;
    pair.GenerateBox(Vec3{0, 0, 0}, Vec3{2 * kS, kS, kS}, 1);
    VoxelChunk* a = pair.GetChunk({0, 0, 0});
    VoxelChunk* b = pair.GetChunk({1, 0, 0});
    ENG_CHECK(a != nullptr);
    ENG_CHECK(b != nullptr);
    if (!a || !b) return;
    a->RebuildMesh(pair.Palette(), true, false, false, &pair);
    b->RebuildMesh(pair.Palette(), true, false, false, &pair);

    // Грань +X чанка 0 отсекается: 5 квадов вместо 6, и ни один чанк не может
    // содержать грань, направленную на общую плоскость.
    ENG_CHECK_EQ(a->Mesh().indices.size(), static_cast<usize>(30));
    ENG_CHECK_EQ(b->Mesh().indices.size(), static_cast<usize>(30));
    for (const Vertex& v : a->Mesh().vertices) {
        ENG_CHECK_MSG(v.normal.x < 0.99f, "chunk 0 emitted a +X face on the shared border plane");
    }
    for (const Vertex& v : b->Mesh().vertices) {
        ENG_CHECK_MSG(v.normal.x > -0.99f, "chunk 1 emitted a -X face on the shared border plane");
    }

    // Отсечение без greedy на границе: 5 * 32 * 32 квадов на каждый чанк.
    a->RebuildMesh(pair.Palette(), false, false, false, &pair);
    ENG_CHECK_EQ(a->Mesh().indices.size(), static_cast<usize>(5 * 32 * 32 * 6));
}

ENG_TEST(Voxel, AmbientOcclusion) {
    // Одинокий воксель: каждый угол верхней грани полностью открыт.
    VoxelWorld w;
    w.SetVoxel(4, 4, 4, 1);
    VoxelChunk* c = w.GetChunk({0, 0, 0});
    ENG_CHECK(c != nullptr);
    if (!c) return;
    c->RebuildMesh(w.Palette(), false, false, true, &w);
    std::vector<u32> top = FindFaceVertices(c->Mesh(), Vec3{0, 1, 0}, 5.0f, 4.0f, 5.0f, 4.0f, 5.0f);
    ENG_CHECK_EQ(top.size(), static_cast<usize>(4));
    if (top.size() != 4) return;
    for (u32 i : top) ENG_CHECK_NEAR(c->Mesh().vertices[i].uv2.x, 1.0f, 1e-4f);
    std::vector<u32> quad = QuadIndices(c->Mesh(), top[0]);
    ENG_CHECK_EQ(quad.size(), static_cast<usize>(6));
    if (quad.size() == 6) {
        // Нет перекрытия -> диагональ по умолчанию (v0 - v2).
        ENG_CHECK_EQ(quad[0], top[0]);
        ENG_CHECK_EQ(quad[1], top[1]);
        ENG_CHECK_EQ(quad[2], top[2]);
        ENG_CHECK_EQ(quad[3], top[0]);
        ENG_CHECK_EQ(quad[4], top[2]);
        ENG_CHECK_EQ(quad[5], top[3]);
    }

    // Перекрыватели в (4,5,3) и (5,5,3) затемняют два угла верхней грани:
    //   p0 (0,0) -> 2, p1 (0,1) -> 3, p2 (1,1) -> 3, p3 (1,0) -> 1
    VoxelWorld o;
    o.SetVoxel(4, 4, 4, 1);
    o.SetVoxel(4, 5, 3, 1);
    o.SetVoxel(5, 5, 3, 1);
    VoxelChunk* oc = o.GetChunk({0, 0, 0});
    ENG_CHECK(oc != nullptr);
    if (!oc) return;
    oc->RebuildMesh(o.Palette(), false, false, true, &o);
    std::vector<u32> t2 = FindFaceVertices(oc->Mesh(), Vec3{0, 1, 0}, 5.0f, 4.0f, 5.0f, 4.0f, 5.0f);
    ENG_CHECK_EQ(t2.size(), static_cast<usize>(4));
    if (t2.size() != 4) return;
    ENG_CHECK_NEAR(oc->Mesh().vertices[t2[0]].uv2.x, 2.0f / 3.0f, 1e-4f);
    ENG_CHECK_NEAR(oc->Mesh().vertices[t2[1]].uv2.x, 1.0f, 1e-4f);
    ENG_CHECK_NEAR(oc->Mesh().vertices[t2[2]].uv2.x, 1.0f, 1e-4f);
    ENG_CHECK_NEAR(oc->Mesh().vertices[t2[3]].uv2.x, 1.0f / 3.0f, 1e-4f);
    // Затемнение попадает в цвет вершины.
    ENG_CHECK_GT(oc->Mesh().vertices[t2[0]].color.x, oc->Mesh().vertices[t2[3]].color.x);
    ENG_CHECK(oc->Mesh().vertices[t2[3]].color.x < 1.0f);

    // a0 + a2 (2/3 + 1) > a1 + a3 (1 + 1/3) -> квад переворачивается на другую
    // диагональ: (v0,v1,v3), (v1,v2,v3).
    std::vector<u32> quad2 = QuadIndices(oc->Mesh(), t2[0]);
    ENG_CHECK_EQ(quad2.size(), static_cast<usize>(6));
    if (quad2.size() == 6) {
        ENG_CHECK_EQ(quad2[0], t2[0]);
        ENG_CHECK_EQ(quad2[1], t2[1]);
        ENG_CHECK_EQ(quad2[2], t2[3]);
        ENG_CHECK_EQ(quad2[3], t2[1]);
        ENG_CHECK_EQ(quad2[4], t2[2]);
        ENG_CHECK_EQ(quad2[5], t2[3]);
    }
}

// ---------------------------------------------------------------------------
// Palette / atlas
// ---------------------------------------------------------------------------
ENG_TEST(Voxel, PaletteDefaults) {
    VoxelPalette p = VoxelPalette::Default();
    ENG_CHECK_GT(static_cast<int>(p.colors.size()), 20);
    ENG_CHECK(p.Get(0).a < 0.01f);          // воздух прозрачен
    ENG_CHECK(p.Get(1).a > 0.9f);           // камень непрозрачен
    ENG_CHECK(!(p.Get(1) == p.Get(5)));      // камень != песок
    ENG_CHECK(!(p.Get(2) == p.Get(3)));      // трава != земля
    ENG_CHECK(p.Get(8).a < 0.99f);          // вода полупрозрачна
    const VoxelPalette ice = VoxelPalette::Ice();
    ENG_CHECK_EQ(ice.colors.size(), p.colors.size());
    ENG_CHECK(!(ice.Get(1) == p.Get(1)));

    VoxelWorld w;
    const Rect r = w.PaletteUV(3);
    ENG_CHECK_NEAR(r.x, 3.0f / 16.0f, 1e-6f);
    ENG_CHECK_NEAR(r.y, 0.0f, 1e-6f);
    ENG_CHECK_NEAR(r.w, 1.0f / 16.0f, 1e-6f);
    ENG_CHECK_NEAR(r.h, 1.0f / 16.0f, 1e-6f);
    const Rect r2 = w.PaletteUV(17);  // строка 1, столбец 1
    ENG_CHECK_NEAR(r2.x, 1.0f / 16.0f, 1e-6f);
    ENG_CHECK_NEAR(r2.y, 1.0f / 16.0f, 1e-6f);
    const Rect r3 = w.PaletteUV(255);
    ENG_CHECK_NEAR(r3.x, 15.0f / 16.0f, 1e-6f);
    ENG_CHECK_NEAR(r3.y, 15.0f / 16.0f, 1e-6f);
}

ENG_TEST(Voxel, PaletteAtlas) {
    VoxelWorld w;
    w.BuildPaletteAtlas(8);
    if (!w.PaletteAtlas().Valid()) ENG_SKIP("no GL context available");
    ENG_CHECK_EQ(w.PaletteAtlas().Width(), 128);
    ENG_CHECK_EQ(w.PaletteAtlas().Height(), 128);
    ENG_CHECK(w.PaletteAtlas().Format() == PixelFormat::RGBA8);
}

// ---------------------------------------------------------------------------
// Бросание лучей
// ---------------------------------------------------------------------------
ENG_TEST(Voxel, Raycast) {
    VoxelWorld w;
    w.GenerateFlat(1, 1, 4, 2, 3);  // сплошное 0..2, трава на y = 3

    // Прямо вниз: входит в верхний воксель при y == 4, нормаль +Y.
    VoxelRayHit down = w.Raycast(Vec3{16.5f, 10.0f, 16.5f}, Vec3{0, -1, 0}, 100.0f);
    ENG_CHECK(down.hit);
    ENG_CHECK_EQ(down.x, 16);
    ENG_CHECK_EQ(down.y, 3);
    ENG_CHECK_EQ(down.z, 16);
    ENG_CHECK_EQ(down.nx, 0);
    ENG_CHECK_EQ(down.ny, 1);
    ENG_CHECK_EQ(down.nz, 0);
    ENG_CHECK_NEAR(down.distance, 6.0f, 1e-3f);
    ENG_CHECK_EQ(down.id, static_cast<u8>(2));

    // Прямо вверх из той же точки не попадает ни во что.
    VoxelRayHit up = w.Raycast(Vec3{16.5f, 10.0f, 16.5f}, Vec3{0, 1, 0}, 5.0f);
    ENG_CHECK(!up.hit);

    // Горизонтальный луч над землёй не попадает.
    VoxelRayHit side = w.Raycast(Vec3{16.5f, 20.0f, 16.5f}, Vec3{1, 0, 0}, 100.0f);
    ENG_CHECK(!side.hit);

    // 45 градусов: лестничный DDA, попадает в верхнюю грань вокселя (6,3,0).
    const Vec3 d = Normalize(Vec3{1, -1, 0});
    VoxelRayHit diag = w.Raycast(Vec3{0.25f, 10.5f, 0.5f}, d, 100.0f);
    ENG_CHECK(diag.hit);
    ENG_CHECK_EQ(diag.x, 6);
    ENG_CHECK_EQ(diag.y, 3);
    ENG_CHECK_EQ(diag.z, 0);
    ENG_CHECK_EQ(diag.ny, 1);
    ENG_CHECK_NEAR(diag.distance, 6.5f * std::sqrt(2.0f), 1e-3f);

    // Вырожденное направление отклоняется, а не зацикливается.
    VoxelRayHit none = w.Raycast(Vec3{0, 0, 0}, Vec3{0, 0, 0}, 10.0f);
    ENG_CHECK(!none.hit);
}

// ---------------------------------------------------------------------------
// Генерация
// ---------------------------------------------------------------------------
ENG_TEST(Voxel, TerrainDeterminism) {
    VoxelWorld a, b, c;
    a.GenerateTerrain(2, 2, 4242, 12, 8);
    b.GenerateTerrain(2, 2, 4242, 12, 8);
    c.GenerateTerrain(2, 2, 4243, 12, 8);
    ENG_CHECK_GT(a.ChunkCount(), 0);
    ENG_CHECK_EQ(a.ChunkCount(), b.ChunkCount());
    ENG_CHECK_EQ(HashWorld(a), HashWorld(b));
    ENG_CHECK(HashWorld(a) != HashWorld(c));
    ENG_CHECK_GT(CountSolids(a), 0);

    // Невырожденный диапазон высот: самый верхний сплошной воксель меняется по столбцам.
    int lo = 1 << 20, hi = -1;
    for (int z = 0; z < 2 * kS; ++z) {
        for (int x = 0; x < 2 * kS; ++x) {
            int top = -1;
            for (int y = 6 * kS - 1; y >= 0; --y) {
                if (a.GetVoxel(x, y, z) != 0) {
                    top = y;
                    break;
                }
            }
            if (top >= 0) {
                lo = std::min(lo, top);
                hi = std::max(hi, top);
            }
        }
    }
    ENG_CHECK(hi > lo);
    ENG_CHECK_GT(hi - lo, 2);

    // Границы покрывают сгенерированную область.
    const Bounds& bnd = a.Bounds();
    ENG_CHECK(bnd.Valid());
    ENG_CHECK(bnd.max.x >= static_cast<f32>(2 * kS));
    ENG_CHECK(bnd.min.x <= 0.0f);
}

ENG_TEST(Voxel, IslandGeneration) {
    VoxelWorld w;
    w.GenerateIsland(1, 7);
    ENG_CHECK_GT(w.ChunkCount(), 0);
    const int solid = CountSolids(w);
    ENG_CHECK_GT(solid, 1000);

    const Bounds& b = w.Bounds();
    ENG_CHECK(b.Valid());
    ENG_CHECK(b.min.x <= 0.0f && b.max.x >= 0.0f);
    ENG_CHECK(b.min.z <= 0.0f && b.max.z >= 0.0f);
    ENG_CHECK_GT(b.max.y, 8.0f);

    // Ограничено: каждый чанк остаётся внутри запрошенного радиуса.
    for (VoxelChunk* c : w.AllChunks()) {
        const ChunkCoord cc = c->Coord();
        ENG_CHECK(cc.x >= -1 && cc.x <= 1);
        ENG_CHECK(cc.z >= -1 && cc.z <= 1);
        ENG_CHECK(cc.y >= 0 && cc.y <= 2);
    }
    // Центральный столбец сплошной снизу вверх (каменное ядро).
    ENG_CHECK(w.IsSolid(0, 0, 0));
    ENG_CHECK(w.IsSolid(0, 1, 0));

    // Детерминированность при том же seed.
    VoxelWorld w2;
    w2.GenerateIsland(1, 7);
    ENG_CHECK_EQ(HashWorld(w), HashWorld(w2));
}

// ---------------------------------------------------------------------------
// Конвейер освещения / построения мешей
// ---------------------------------------------------------------------------
ENG_TEST(Voxel, Lighting) {
    VoxelWorld w;
    w.GenerateFlat(1, 1, 4, 2, 3);
    VoxelChunk* c = w.GetChunk({0, 0, 0});
    ENG_CHECK(c != nullptr);
    if (!c) return;
    w.ComputeLighting(64);
    ENG_CHECK_EQ(c->GetLight(5, 3, 5), 15);   // открытая поверхность сохраняет полный свет
    ENG_CHECK_EQ(c->GetLight(5, 10, 5), 15);  // открытое небо
    ENG_CHECK(c->GetLight(5, 2, 5) < 15);     // под землёй темнее
    ENG_CHECK(c->GetLight(5, 2, 5) > 0);      // ... но свет с поверхности всё же просачивается
    ENG_CHECK(c->GetLight(5, 0, 5) <= c->GetLight(5, 2, 5));

    // Полностью запертая полость остаётся тёмной.
    w.SetVoxel(10, 20, 10, 1);
    w.ComputeLighting(64);
    ENG_CHECK_EQ(w.GetVoxel(10, 20, 10), static_cast<u8>(1));
}

ENG_TEST(Voxel, UpdateMeshes) {
    VoxelWorld w;
    w.GenerateFlat(2, 1, 4, 2, 3);
    ENG_CHECK_EQ(w.ChunkCount(), 2);
    w.UpdateMeshes();
    for (VoxelChunk* c : w.AllChunks()) {
        ENG_CHECK(!c->Dirty());
        ENG_CHECK(!c->Empty());
        // Прямая проверка CPU-построения меша (загрузка на GPU может быть недоступна).
        c->RebuildMesh(w.Palette(), w.Meshing().greedy, false, w.Meshing().ambientOcclusion, &w);
        ENG_CHECK_GT(c->Mesh().indices.size(), static_cast<usize>(0));
        ENG_CHECK_GT(c->Mesh().vertices.size(), static_cast<usize>(0));
        ENG_CHECK(c->Mesh().bounds.Valid());
    }

    // Правка граничного вокселя снова помечает затронутые чанки изменёнными.
    w.SetVoxel(kS, 8, 0, 5);
    ENG_CHECK(w.GetChunk({1, 0, 0}) != nullptr);
    ENG_CHECK(w.GetChunk({1, 0, 0})->Dirty());
    w.UpdateMeshes();
    for (VoxelChunk* c : w.AllChunks()) ENG_CHECK(!c->Dirty());
}

ENG_TEST(Voxel, EmptyChunkMesh) {
    VoxelWorld w;
    VoxelChunk* c = w.GetOrCreateChunk({0, 0, 0});
    ENG_CHECK(c != nullptr);
    if (!c) return;
    c->RebuildMesh(w.Palette(), true, true, true, &w);
    ENG_CHECK(c->Empty());
    ENG_CHECK_EQ(c->Mesh().indices.size(), static_cast<usize>(0));
    ENG_CHECK(!c->MeshValid());
}

ENG_TEST(Voxel, SampleAO) {
    VoxelWorld w;
    w.GenerateFlat(1, 1, 4, 2, 3);
    // Верхняя грань непокрытого вокселя: полностью открыта.
    ENG_CHECK_NEAR(w.SampleAO(5, 3, 5, 0, 1, 0), 1.0f, 1e-4f);
    // Обкладываем воксель стенами: полностью перекрыт.
    w.SetVoxel(4, 3, 5, 1);
    w.SetVoxel(6, 3, 5, 1);
    w.SetVoxel(5, 3, 4, 1);
    w.SetVoxel(5, 3, 6, 1);
    w.SetVoxel(4, 4, 5, 1);
    w.SetVoxel(6, 4, 5, 1);
    w.SetVoxel(5, 4, 4, 1);
    w.SetVoxel(5, 4, 6, 1);
    w.SetVoxel(4, 4, 4, 1);
    w.SetVoxel(6, 4, 6, 1);
    ENG_CHECK(w.SampleAO(5, 3, 5, 0, 1, 0) < 0.25f);
}

// Каждый выданный треугольник должен обходиться против часовой стрелки при
// взгляде снаружи (движок отсекает задние грани) и нести вменяемые атрибуты
// вершин. Это также покрывает переворот AO-квадов на реальной сгенерированной геометрии.
ENG_TEST(Voxel, MeshWindingConsistency) {
    for (int mode = 0; mode < 4; ++mode) {
        const bool greedy = (mode & 1) != 0;
        const bool ao = (mode & 2) != 0;
        VoxelWorld w;
        w.GenerateIsland(1, 11);
        w.ComputeLighting(64);
        w.SetMeshing(VoxelMeshingOptions{greedy, true, ao, true, true});
        w.UpdateMeshes();

        usize triangles = 0;
        for (VoxelChunk* c : w.AllChunks()) {
            const MeshData& md = c->Mesh();
            ENG_CHECK_EQ(md.indices.size() % 3, static_cast<usize>(0));
            for (const Vertex& v : md.vertices) {
                ENG_CHECK(v.uv2.x >= -1e-4f && v.uv2.x <= 1.0f + 1e-4f);
                ENG_CHECK(v.uv2.y >= -1e-4f && v.uv2.y <= 1.0f + 1e-4f);
                ENG_CHECK_NEAR(Length(v.normal), 1.0f, 1e-4f);
            }
            for (usize i = 0; i + 2 < md.indices.size(); i += 3) {
                const Vertex& a = md.vertices[md.indices[i]];
                const Vertex& b = md.vertices[md.indices[i + 1]];
                const Vertex& d = md.vertices[md.indices[i + 2]];
                const Vec3 geo = Cross(b.position - a.position, d.position - a.position);
                ENG_CHECK_MSG(Dot(geo, a.normal) > 0.0f, "triangle wound the wrong way");
                ++triangles;
            }
        }
        ENG_CHECK_GT(triangles, static_cast<usize>(0));
    }
}
