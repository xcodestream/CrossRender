// Тесты импортёров моделей (OBJ/MTL, glTF/GLB, PLY, STL, VOX) и процедурных
// low-poly-пресетов.
//
// Тестовые ассеты генерируются в памяти и записываются в папку пользовательских
// данных (или /tmp), чтобы импортёры работали через реальные файловые пути.
#include <vector>  // Math.h требует, чтобы std::vector был доступен.

#include <array>
#include <cmath>
#include <cstdio>
#include <string>
#include <cstring>

#include "crossrender/core/File.h"
#include "crossrender/test/Test.h"
#include "crossrender/assets/Model.h"

#if !defined(ENG_PLATFORM_WINDOWS)
#  include <sys/stat.h>
#endif

namespace {

// ---------------------------------------------------------------------------
// Помощники для создания тестовых файлов
// ---------------------------------------------------------------------------
std::string SampleDir() {
    static std::string cached;
    if (!cached.empty()) return cached;
    std::string dir = crossrender::GetUserRoot();
    if (!dir.empty()) {
        dir = crossrender::PathJoin(dir, "model_tests");
#if defined(ENG_PLATFORM_WINDOWS)
        ::_mkdir(dir.c_str());
#else
        ::mkdir(dir.c_str(), 0755);
#endif
        // Откат к /tmp, если папка пользовательских данных недоступна для записи (например, в sandbox-запусках тестов).
        if (crossrender::WriteTextFile(crossrender::PathJoin(dir, "write_probe.tmp"), "ok")) {
            cached = dir;
            return cached;
        }
    }
    dir = "/tmp/eng_model_tests";
#if defined(ENG_PLATFORM_WINDOWS)
    ::_mkdir(dir.c_str());
#else
    ::mkdir(dir.c_str(), 0755);
#endif
    cached = dir;
    return cached;
}

std::string WriteSample(const std::string& name, const std::string& text) {
    const std::string path = crossrender::PathJoin(SampleDir(), name);
    ENG_CHECK_MSG(crossrender::WriteTextFile(path, text), name);
    return path;
}

std::string WriteSampleBin(const std::string& name, const void* data, crossrender::usize size) {
    const std::string path = crossrender::PathJoin(SampleDir(), name);
    ENG_CHECK_MSG(crossrender::WriteBinaryFile(path, data, size), name);
    return path;
}

std::string Base64Encode(const std::vector<crossrender::u8>& in) {
    static const char kTable[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((in.size() + 2) / 3 * 4);
    crossrender::usize i = 0;
    while (i + 2 < in.size()) {
        const crossrender::u32 v = (static_cast<crossrender::u32>(in[i]) << 16) | (static_cast<crossrender::u32>(in[i + 1]) << 8) |
                           static_cast<crossrender::u32>(in[i + 2]);
        out.push_back(kTable[(v >> 18) & 63]);
        out.push_back(kTable[(v >> 12) & 63]);
        out.push_back(kTable[(v >> 6) & 63]);
        out.push_back(kTable[v & 63]);
        i += 3;
    }
    if (i + 1 == in.size()) {
        const crossrender::u32 v = static_cast<crossrender::u32>(in[i]) << 16;
        out.push_back(kTable[(v >> 18) & 63]);
        out.push_back(kTable[(v >> 12) & 63]);
        out.push_back('=');
        out.push_back('=');
    } else if (i + 2 == in.size()) {
        const crossrender::u32 v = (static_cast<crossrender::u32>(in[i]) << 16) | (static_cast<crossrender::u32>(in[i + 1]) << 8);
        out.push_back(kTable[(v >> 18) & 63]);
        out.push_back(kTable[(v >> 12) & 63]);
        out.push_back(kTable[(v >> 6) & 63]);
        out.push_back('=');
    }
    return out;
}

void PutU16LE(std::vector<crossrender::u8>& v, crossrender::u16 x) {
    v.push_back(static_cast<crossrender::u8>(x & 0xFF));
    v.push_back(static_cast<crossrender::u8>((x >> 8) & 0xFF));
}
void PutU32LE(std::vector<crossrender::u8>& v, crossrender::u32 x) {
    v.push_back(static_cast<crossrender::u8>(x & 0xFF));
    v.push_back(static_cast<crossrender::u8>((x >> 8) & 0xFF));
    v.push_back(static_cast<crossrender::u8>((x >> 16) & 0xFF));
    v.push_back(static_cast<crossrender::u8>((x >> 24) & 0xFF));
}
void PutF32LE(std::vector<crossrender::u8>& v, crossrender::f32 f) {
    crossrender::u32 bits = 0;
    std::memcpy(&bits, &f, 4);
    PutU32LE(v, bits);
}
void PutI32LE(std::vector<crossrender::u8>& v, crossrender::i32 x) { PutU32LE(v, static_cast<crossrender::u32>(x)); }

// ---------------------------------------------------------------------------
// Конструирование glTF-образца
// ---------------------------------------------------------------------------
// 3 позиции float32 + 3 индекса uint16, упакованные в один бинарный буфер.
std::vector<crossrender::u8> TriangleBuffer() {
    std::vector<crossrender::u8> bin;
    PutF32LE(bin, 0.0f);
    PutF32LE(bin, 0.0f);
    PutF32LE(bin, 0.0f);
    PutF32LE(bin, 1.0f);
    PutF32LE(bin, 0.0f);
    PutF32LE(bin, 0.0f);
    PutF32LE(bin, 0.0f);
    PutF32LE(bin, 1.0f);
    PutF32LE(bin, 0.0f);
    PutU16LE(bin, 0);
    PutU16LE(bin, 1);
    PutU16LE(bin, 2);
    return bin;
}

// bufferUri: "data:application/octet-stream;base64,<...>" для .gltf, пусто для GLB.
std::string TriangleGltfJson(const std::string& bufferUri) {
    const bool embedded = !bufferUri.empty();
    std::string json = "{";
    json += "\"asset\":{\"version\":\"2.0\",\"generator\":\"crossrender-test\"},";
    json += "\"scene\":0,";
    json += "\"scenes\":[{\"nodes\":[0]}],";
    json += "\"nodes\":[{\"name\":\"tri\",\"mesh\":0,\"translation\":[1.0,2.0,3.0]}],";
    json += "\"meshes\":[{\"name\":\"triangle\",\"primitives\":[{\"attributes\":{\"POSITION\":0},\"indices\":1,"
            "\"material\":0}]}],";
    json +=
        "\"materials\":[{\"name\":\"red\",\"pbrMetallicRoughness\":{\"baseColorFactor\":[1.0,0.0,0.0,1.0],"
        "\"metallicFactor\":0.0,\"roughnessFactor\":0.5}}],";
    json += "\"buffers\":[{\"byteLength\":42";
    if (embedded) json += ",\"uri\":\"" + bufferUri + "\"";
    json += "}],";
    json += "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36},";
    json += "{\"buffer\":0,\"byteOffset\":36,\"byteLength\":6}],";
    json += "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},";
    json += "{\"bufferView\":1,\"componentType\":5123,\"count\":3,\"type\":\"SCALAR\"}]";
    json += "}";
    return json;
}

std::vector<crossrender::u8> BuildGlb(const std::string& json, const std::vector<crossrender::u8>& bin) {
    std::vector<crossrender::u8> jsonChunk(json.begin(), json.end());
    while (jsonChunk.size() % 4 != 0) jsonChunk.push_back(' ');
    std::vector<crossrender::u8> binChunk = bin;
    while (binChunk.size() % 4 != 0) binChunk.push_back(0);

    const crossrender::u32 total = 12 + 8 + static_cast<crossrender::u32>(jsonChunk.size()) + 8 +
                           static_cast<crossrender::u32>(binChunk.size());
    std::vector<crossrender::u8> out;
    out.push_back('g');
    out.push_back('l');
    out.push_back('T');
    out.push_back('F');
    PutU32LE(out, 2);
    PutU32LE(out, total);
    PutU32LE(out, static_cast<crossrender::u32>(jsonChunk.size()));
    PutU32LE(out, 0x4E4F534Au);  // "JSON"
    out.insert(out.end(), jsonChunk.begin(), jsonChunk.end());
    PutU32LE(out, static_cast<crossrender::u32>(binChunk.size()));
    PutU32LE(out, 0x004E4942u);  // "BIN\0"
    out.insert(out.end(), binChunk.begin(), binChunk.end());
    return out;
}

// ---------------------------------------------------------------------------
// Конструирование образцов PLY / STL / VOX
// ---------------------------------------------------------------------------
const char* kPlyAsciiHeader =
    "ply\n"
    "format ascii 1.0\n"
    "comment tetrahedron\n"
    "element vertex 4\n"
    "property float x\n"
    "property float y\n"
    "property float z\n"
    "property float nx\n"
    "property float ny\n"
    "property float nz\n"
    "property uchar red\n"
    "property uchar green\n"
    "property uchar blue\n"
    "element face 4\n"
    "property list uchar int vertex_indices\n"
    "end_header\n";

std::string PlyAsciiSample() {
    return std::string(kPlyAsciiHeader) +
           "0 0 0 0 0 -1 255 0 0\n"
           "1 0 0 0 1 0 0 255 0\n"
           "0 1 0 1 0 0 0 0 255\n"
           "0 0 1 0 1 1 255 255 255\n"
           "3 0 2 1\n"
           "3 0 1 3\n"
           "3 0 3 2\n"
           "3 1 2 3\n";
}

std::vector<crossrender::u8> PlyBinarySample(bool bigEndian) {
    std::vector<crossrender::u8> out;
    static const crossrender::f32 kData[4][6] = {
        {0, 0, 0, 0, 0, -1}, {1, 0, 0, 0, 1, 0}, {0, 1, 0, 1, 0, 0}, {0, 0, 1, 0, 1, 1}};
    auto putF32 = [&](crossrender::f32 f) {
        if (bigEndian) {
            crossrender::u32 bits = 0;
            std::memcpy(&bits, &f, 4);
            out.push_back(static_cast<crossrender::u8>((bits >> 24) & 0xFF));
            out.push_back(static_cast<crossrender::u8>((bits >> 16) & 0xFF));
            out.push_back(static_cast<crossrender::u8>((bits >> 8) & 0xFF));
            out.push_back(static_cast<crossrender::u8>(bits & 0xFF));
        } else {
            PutF32LE(out, f);
        }
    };
    auto putI32 = [&](crossrender::i32 v) {
        if (bigEndian) {
            out.push_back(static_cast<crossrender::u8>((v >> 24) & 0xFF));
            out.push_back(static_cast<crossrender::u8>((v >> 16) & 0xFF));
            out.push_back(static_cast<crossrender::u8>((v >> 8) & 0xFF));
            out.push_back(static_cast<crossrender::u8>(v & 0xFF));
        } else {
            PutI32LE(out, v);
        }
    };
    for (int i = 0; i < 4; ++i)
        for (int k = 0; k < 6; ++k) putF32(kData[i][k]);
    static const crossrender::i32 kFaces[4][3] = {{0, 2, 1}, {0, 1, 3}, {0, 3, 2}, {1, 2, 3}};
    for (int i = 0; i < 4; ++i) {
        out.push_back(3);  // количество элементов списка (uchar)
        for (int k = 0; k < 3; ++k) putI32(kFaces[i][k]);
    }
    return out;
}

std::vector<crossrender::u8> StlBinarySample() {
    std::vector<crossrender::u8> out(80, 0);
    std::memcpy(out.data(), "crossrender test triangle", 17);
    PutU32LE(out, 1);  // количество треугольников
    PutF32LE(out, 0.0f);
    PutF32LE(out, 0.0f);
    PutF32LE(out, 1.0f);  // нормаль грани
    PutF32LE(out, 0.0f);
    PutF32LE(out, 0.0f);
    PutF32LE(out, 0.0f);
    PutF32LE(out, 1.0f);
    PutF32LE(out, 0.0f);
    PutF32LE(out, 0.0f);
    PutF32LE(out, 0.0f);
    PutF32LE(out, 1.0f);
    PutF32LE(out, 0.0f);
    PutU16LE(out, 0);  // счётчик байтов атрибута
    return out;
}

// Минимальный файл MagicaVoxel с двумя соседними вокселями на одной записи палитры 1.
std::vector<crossrender::u8> MakeVoxChunk(const char* id, const std::vector<crossrender::u8>& content) {
    std::vector<crossrender::u8> out;
    out.insert(out.end(), id, id + 4);
    PutU32LE(out, static_cast<crossrender::u32>(content.size()));
    PutU32LE(out, 0);
    out.insert(out.end(), content.begin(), content.end());
    return out;
}

std::vector<crossrender::u8> MakeVoxFile(const std::vector<std::vector<crossrender::u8>>& chunks, crossrender::i32 version = 150) {
    std::vector<crossrender::u8> main;
    main.insert(main.end(), {'M', 'A', 'I', 'N'});
    PutU32LE(main, 0);
    crossrender::u32 children = 0;
    for (const auto& c : chunks) children += static_cast<crossrender::u32>(c.size());
    PutU32LE(main, children);
    for (const auto& c : chunks) main.insert(main.end(), c.begin(), c.end());

    std::vector<crossrender::u8> out;
    out.insert(out.end(), {'V', 'O', 'X', ' '});
    PutU32LE(out, static_cast<crossrender::u32>(version));
    out.insert(out.end(), main.begin(), main.end());
    return out;
}

std::vector<crossrender::u8> MakeSizeChunk(crossrender::i32 x, crossrender::i32 y, crossrender::i32 z) {
    std::vector<crossrender::u8> size;
    PutI32LE(size, x);
    PutI32LE(size, y);
    PutI32LE(size, z);
    return MakeVoxChunk("SIZE", size);
}

std::vector<crossrender::u8> MakeXyziChunk(const std::vector<std::array<crossrender::u8, 4>>& voxels) {
    std::vector<crossrender::u8> xyzi;
    PutU32LE(xyzi, static_cast<crossrender::u32>(voxels.size()));
    for (const auto& v : voxels) {
        xyzi.push_back(v[0]);
        xyzi.push_back(v[1]);
        xyzi.push_back(v[2]);
        xyzi.push_back(v[3]);
    }
    return MakeVoxChunk("XYZI", xyzi);
}

std::vector<crossrender::u8> MakeRgbaChunk() {
    std::vector<crossrender::u8> rgba(256 * 4, 0);
    rgba[0] = 255;
    rgba[1] = 40;
    rgba[2] = 30;
    rgba[3] = 255;  // запись палитры 1 (индекс XYZI 1 отображается на запись 0)
    return MakeVoxChunk("RGBA", rgba);
}

// Словарное кодирование, используемое чанками графа сцены: счётчик, затем пары (длина, байты).
void PutVoxDict(std::vector<crossrender::u8>& out, const std::vector<std::pair<std::string, std::string>>& entries) {
    PutI32LE(out, static_cast<crossrender::i32>(entries.size()));
    for (const auto& kv : entries) {
        PutI32LE(out, static_cast<crossrender::i32>(kv.first.size()));
        out.insert(out.end(), kv.first.begin(), kv.first.end());
        PutI32LE(out, static_cast<crossrender::i32>(kv.second.size()));
        out.insert(out.end(), kv.second.begin(), kv.second.end());
    }
}

std::vector<crossrender::u8> VoxSample() {
    return MakeVoxFile({MakeSizeChunk(2, 1, 1), MakeXyziChunk({{{0, 0, 0, 1}, {1, 0, 0, 1}}}), MakeRgbaChunk()});
}


const char* kObjSample =
    "# two-triangle quad plus a negative-index triangle\n"
    "mtllib quad.mtl\n"
    "v 0 0 0\n"
    "v 1 0 0\n"
    "v 1 0 1\n"
    "v 0 0 1\n"
    "vt 0 0\n"
    "vt 1 0\n"
    "vt 1 1\n"
    "vt 0 1\n"
    "vn 0 1 0\n"
    "usemtl red\n"
    "g quad\n"
    "f 1/1/1 2/2/1 3/3/1 4/4/1\n"
    "usemtl blue\n"
    "o tri\n"
    "f -4/1/1 -3/2/1 -2/3/1\n";

const char* kMtlSample =
    "newmtl red\n"
    "Kd 1.0 0.2 0.1\n"
    "Ks 0.2 0.2 0.2\n"
    "Ns 32\n"
    "d 1.0\n"
    "newmtl blue\n"
    "Kd 0.1 0.2 1.0\n";

void CheckTriangleGltf(crossrender::Model& m) {
    ENG_CHECK_EQ(m.MeshCount(), 1);
    ENG_CHECK_EQ(m.MaterialCount(), 1);
    ENG_CHECK_EQ(m.NodeCount(), 1);
    const crossrender::MeshData& md = m.MeshAt(0);
    ENG_CHECK_EQ(md.vertices.size(), 3u);
    ENG_CHECK_EQ(md.indices.size(), 3u);
    ENG_CHECK_EQ(md.subMeshes.size(), 1u);
    ENG_CHECK_EQ(md.subMeshes[0].materialIndex, 0);
    ENG_CHECK_STR_EQ(m.MaterialAt(0).name, "red");
    ENG_CHECK_NEAR(m.MaterialAt(0).baseColor.r, 1.0f, 1e-5f);
    ENG_CHECK_NEAR(m.MaterialAt(0).baseColor.g, 0.0f, 1e-5f);
    ENG_CHECK_NEAR(m.MaterialAt(0).metallic, 0.0f, 1e-5f);
    ENG_CHECK_NEAR(m.MaterialAt(0).roughness, 0.5f, 1e-5f);
    // Вершины остаются в локальном пространстве.
    ENG_CHECK_NEAR(md.vertices[1].position.x, 1.0f, 1e-5f);
    ENG_CHECK_NEAR(md.vertices[1].position.y, 0.0f, 1e-5f);
    // ... а трансформацию несёт узел.
    ENG_CHECK_EQ(m.NodeAt(0).mesh, 0);
    ENG_CHECK_NEAR(m.NodeAt(0).translation.x, 1.0f, 1e-5f);
    ENG_CHECK_NEAR(m.NodeAt(0).translation.y, 2.0f, 1e-5f);
    ENG_CHECK_NEAR(m.NodeAt(0).translation.z, 3.0f, 1e-5f);
    // Границы вычисляются по трансформированной геометрии узла.
    ENG_CHECK(m.Bounds().Valid());
    ENG_CHECK_NEAR(m.Bounds().min.x, 1.0f, 1e-4f);
    ENG_CHECK_NEAR(m.Bounds().min.y, 2.0f, 1e-4f);
    ENG_CHECK_NEAR(m.Bounds().min.z, 3.0f, 1e-4f);
    ENG_CHECK_NEAR(m.Bounds().max.x, 2.0f, 1e-4f);
    ENG_CHECK_NEAR(m.Bounds().max.y, 3.0f, 1e-4f);
    ENG_CHECK_NEAR(m.Bounds().max.z, 3.0f, 1e-4f);
}

std::vector<crossrender::u8> GarbageBytes() {
    std::vector<crossrender::u8> v;
    for (int i = 0; i < 32; ++i) v.push_back(static_cast<crossrender::u8>(i * 7 + 1));
    return v;
}

}  // namespace

// ---------------------------------------------------------------------------
// OBJ / MTL
// ---------------------------------------------------------------------------
ENG_TEST(Model, ObjQuadAndMaterials) {
    const std::string dir = SampleDir();
    ENG_CHECK(crossrender::WriteTextFile(crossrender::PathJoin(dir, "quad.mtl"), kMtlSample));
    const std::string path = WriteSample("quad.obj", kObjSample);

    crossrender::Model m;
    ENG_CHECK(m.Load(path));
    ENG_CHECK(m.Format() == crossrender::ModelFormat::Obj);
    ENG_CHECK(m.Valid());
    ENG_CHECK_EQ(m.MeshCount(), 1);
    ENG_CHECK_EQ(m.MaterialCount(), 2);
    ENG_CHECK(m.NodeCount() >= 1);

    const crossrender::MeshData& md = m.MeshAt(0);
    ENG_CHECK_EQ(md.vertices.size(), 4u);   // сварены по (v, vt, vn)
    ENG_CHECK_EQ(md.indices.size(), 9u);    // четырёхугольник -> 2 треугольника + 1 треугольник с отрицательными индексами
    ENG_CHECK_EQ(md.subMeshes.size(), 2u);  // группы usemtl
    ENG_CHECK_EQ(md.subMeshes[0].indexCount, 6u);
    ENG_CHECK_EQ(md.subMeshes[1].indexCount, 3u);
    ENG_CHECK_EQ(md.subMeshes[0].materialIndex, 0);
    ENG_CHECK_EQ(md.subMeshes[1].materialIndex, 1);

    ENG_CHECK_STR_EQ(m.MaterialAt(0).name, "red");
    ENG_CHECK_STR_EQ(m.MaterialAt(1).name, "blue");
    ENG_CHECK_NEAR(m.MaterialAt(0).baseColor.r, 1.0f, 1e-4f);
    ENG_CHECK_NEAR(m.MaterialAt(0).baseColor.g, 0.2f, 1e-4f);
    ENG_CHECK_NEAR(m.MaterialAt(0).baseColor.b, 0.1f, 1e-4f);
    ENG_CHECK_NEAR(m.MaterialAt(0).baseColor.a, 1.0f, 1e-4f);
    ENG_CHECK_NEAR(m.MaterialAt(1).baseColor.r, 0.1f, 1e-4f);
    ENG_CHECK_NEAR(m.MaterialAt(1).baseColor.b, 1.0f, 1e-4f);
    ENG_CHECK_NEAR(m.MaterialAt(0).roughness, 0.821f, 5e-3f);  // из Ns = 32

    // Заданная нормаль "vn 0 1 0" должна сохраниться.
    ENG_CHECK_NEAR(md.vertices[0].normal.y, 1.0f, 1e-5f);
    // Границы плоского четырёхугольника в плоскости XZ.
    ENG_CHECK(m.Bounds().Valid());
    ENG_CHECK_NEAR(m.Bounds().min.x, 0.0f, 1e-5f);
    ENG_CHECK_NEAR(m.Bounds().min.y, 0.0f, 1e-5f);
    ENG_CHECK_NEAR(m.Bounds().min.z, 0.0f, 1e-5f);
    ENG_CHECK_NEAR(m.Bounds().max.x, 1.0f, 1e-5f);
    ENG_CHECK_NEAR(m.Bounds().max.y, 0.0f, 1e-5f);
    ENG_CHECK_NEAR(m.Bounds().max.z, 1.0f, 1e-5f);

    // Базовое имя сохраняется.
    ENG_CHECK_STR_EQ(m.Name(), "quad.obj");
}

ENG_TEST(Model, ObjDirectAndInvalidInput) {
    // Публичная точка входа импортёра.
    crossrender::MeshData mesh;
    std::vector<crossrender::Material> mats;
    std::string err;
    const std::string obj(kObjSample);
    ENG_CHECK(crossrender::Model::ImportObj(obj.data(), obj.size(), &mesh, &mats, &err));
    ENG_CHECK_EQ(mesh.vertices.size(), 4u);
    ENG_CHECK_EQ(mesh.indices.size(), 9u);
    ENG_CHECK_EQ(mats.size(), 2u);

    // Отсутствующие нормали должны быть сгенерированы (плоские при "s off").
    const char* noNormals =
        "v 0 0 0\nv 1 0 0\nv 0 1 0\n"
        "s off\nf 1 2 3\n"
        "s 1\nf 1 2 3\n";
    crossrender::MeshData mesh2;
    std::vector<crossrender::Material> mats2;
    err.clear();
    ENG_CHECK(crossrender::Model::ImportObj(noNormals, std::strlen(noNormals), &mesh2, &mats2, &err));
    ENG_CHECK_EQ(mesh2.indices.size(), 6u);
    for (const crossrender::Vertex& v : mesh2.vertices) ENG_CHECK_NEAR(crossrender::Length(v.normal), 1.0f, 1e-4f);

    // Строки "v" могут содержать необязательную компоненту w (игнорируется).
    const char* withW = "v 0 0 0 1.0\nv 1 0 0 1.0\nv 0 1 0 1.0\nf 1 2 3\n";
    crossrender::MeshData mesh3;
    std::vector<crossrender::Material> mats3;
    err.clear();
    ENG_CHECK(crossrender::Model::ImportObj(withW, std::strlen(withW), &mesh3, &mats3, &err));
    ENG_CHECK_EQ(mesh3.vertices.size(), 3u);
    ENG_CHECK_EQ(mesh3.indices.size(), 3u);

    // Структурно некорректный ввод приводит к ошибке с сообщением.
    const char* badIndex = "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 9\n";
    err.clear();
    ENG_CHECK(!crossrender::Model::ImportObj(badIndex, std::strlen(badIndex), &mesh2, &mats2, &err));
    ENG_CHECK(!err.empty());

    const char* noVertices = "f 1 2 3\n";
    err.clear();
    ENG_CHECK(!crossrender::Model::ImportObj(noVertices, std::strlen(noVertices), &mesh2, &mats2, &err));

    crossrender::Model m;
    ENG_CHECK(!m.LoadFromMemory(nullptr, 0, crossrender::ModelFormat::Obj, "empty"));
}

// ---------------------------------------------------------------------------
// glTF / GLB
// ---------------------------------------------------------------------------
ENG_TEST(Model, GltfTextWithEmbeddedBuffer) {
    const std::vector<crossrender::u8> bin = TriangleBuffer();
    const std::string uri = "data:application/octet-stream;base64," + Base64Encode(bin);
    const std::string json = TriangleGltfJson(uri);

    crossrender::Model m;
    ENG_CHECK(m.LoadFromMemory(json.data(), json.size(), crossrender::ModelFormat::Gltf, "tri"));
    ENG_CHECK(m.Format() == crossrender::ModelFormat::Gltf);
    CheckTriangleGltf(m);

    // Тот же контент, записанный на диск как файл .gltf.
    const std::string path = WriteSample("triangle.gltf", json);
    crossrender::Model file;
    ENG_CHECK(file.Load(path));
    ENG_CHECK(file.Format() == crossrender::ModelFormat::Gltf);
    CheckTriangleGltf(file);
}

ENG_TEST(Model, GlbWithBinChunk) {
    const std::vector<crossrender::u8> bin = TriangleBuffer();
    const std::string json = TriangleGltfJson("");
    const std::vector<crossrender::u8> glb = BuildGlb(json, bin);

    const std::string path = WriteSampleBin("triangle.glb", glb.data(), glb.size());
    crossrender::Model m;
    ENG_CHECK(m.Load(path));
    ENG_CHECK(m.Format() == crossrender::ModelFormat::Glb);
    CheckTriangleGltf(m);

    // Загрузка тех же байтов из памяти (формат указан как Gltf) тоже должна работать.
    crossrender::Model mem;
    ENG_CHECK(mem.LoadFromMemory(glb.data(), glb.size(), crossrender::ModelFormat::Gltf, "glb-mem"));
    ENG_CHECK(mem.Format() == crossrender::ModelFormat::Glb);
    CheckTriangleGltf(mem);

    // Определение формата по магическим байтам без полезного расширения.
    const std::string magicPath = WriteSampleBin("triangle_magic.dat", glb.data(), glb.size());
    crossrender::Model magic;
    ENG_CHECK(magic.Load(magicPath));
    ENG_CHECK(magic.Format() == crossrender::ModelFormat::Glb);
}

ENG_TEST(Model, GltfRejectsGarbage) {
    const char* junk = "{\"asset\":{\"version\":\"2.0\"},\"meshes\":";
    crossrender::Model m;
    ENG_CHECK(!m.LoadFromMemory(junk, std::strlen(junk), crossrender::ModelFormat::Gltf, "junk"));
    ENG_CHECK(!m.Valid());
}

// ---------------------------------------------------------------------------
// PLY
// ---------------------------------------------------------------------------
ENG_TEST(Model, PlyAsciiTetrahedron) {
    const std::string path = WriteSample("tetra_ascii.ply", PlyAsciiSample());
    crossrender::Model m;
    ENG_CHECK(m.Load(path));
    ENG_CHECK(m.Format() == crossrender::ModelFormat::Ply);
    ENG_CHECK_EQ(m.MeshCount(), 1);
    const crossrender::MeshData& md = m.MeshAt(0);
    ENG_CHECK_EQ(md.vertices.size(), 4u);
    ENG_CHECK_EQ(md.indices.size(), 12u);  // 4 треугольника
    ENG_CHECK_NEAR(m.Bounds().min.x, 0.0f, 1e-5f);
    ENG_CHECK_NEAR(m.Bounds().min.y, 0.0f, 1e-5f);
    ENG_CHECK_NEAR(m.Bounds().min.z, 0.0f, 1e-5f);
    ENG_CHECK_NEAR(m.Bounds().max.x, 1.0f, 1e-5f);
    ENG_CHECK_NEAR(m.Bounds().max.y, 1.0f, 1e-5f);
    ENG_CHECK_NEAR(m.Bounds().max.z, 1.0f, 1e-5f);
    // Файл содержит нормали.
    ENG_CHECK_NEAR(crossrender::Length(md.vertices[0].normal), 1.0f, 1e-4f);
}

ENG_TEST(Model, PlyBinaryLittleEndian) {
    const std::string header =
        "ply\n"
        "format binary_little_endian 1.0\n"
        "element vertex 4\n"
        "property float x\nproperty float y\nproperty float z\n"
        "property float nx\nproperty float ny\nproperty float nz\n"
        "element face 4\n"
        "property list uchar int vertex_indices\n"
        "end_header\n";
    std::vector<crossrender::u8> bytes(header.begin(), header.end());
    const std::vector<crossrender::u8> payload = PlyBinarySample(false);
    bytes.insert(bytes.end(), payload.begin(), payload.end());
    const std::string path = WriteSampleBin("tetra_le.ply", bytes.data(), bytes.size());

    crossrender::Model m;
    ENG_CHECK(m.Load(path));
    ENG_CHECK(m.Format() == crossrender::ModelFormat::Ply);
    ENG_CHECK_EQ(m.MeshAt(0).vertices.size(), 4u);
    ENG_CHECK_EQ(m.MeshAt(0).indices.size(), 12u);
    ENG_CHECK_NEAR(m.MeshAt(0).vertices[3].position.z, 1.0f, 1e-5f);
    ENG_CHECK_NEAR(m.Bounds().max.z, 1.0f, 1e-5f);
}

ENG_TEST(Model, PlyBinaryBigEndian) {
    const std::string header =
        "ply\n"
        "format binary_big_endian 1.0\n"
        "element vertex 4\n"
        "property float x\nproperty float y\nproperty float z\n"
        "property float nx\nproperty float ny\nproperty float nz\n"
        "element face 4\n"
        "property list uchar int vertex_indices\n"
        "end_header\n";
    std::vector<crossrender::u8> bytes(header.begin(), header.end());
    const std::vector<crossrender::u8> payload = PlyBinarySample(true);
    bytes.insert(bytes.end(), payload.begin(), payload.end());
    const std::string path = WriteSampleBin("tetra_be.ply", bytes.data(), bytes.size());

    crossrender::Model m;
    ENG_CHECK(m.Load(path));
    ENG_CHECK_EQ(m.MeshAt(0).vertices.size(), 4u);
    ENG_CHECK_EQ(m.MeshAt(0).indices.size(), 12u);
    ENG_CHECK_NEAR(m.MeshAt(0).vertices[2].position.y, 1.0f, 1e-5f);
    // В файле нет нормалей -> они генерируются.
    ENG_CHECK_NEAR(crossrender::Length(m.MeshAt(0).vertices[0].normal), 1.0f, 1e-3f);
}

// ---------------------------------------------------------------------------
// STL
// ---------------------------------------------------------------------------
ENG_TEST(Model, StlBinaryTriangle) {
    const std::vector<crossrender::u8> bytes = StlBinarySample();
    const std::string path = WriteSampleBin("tri.stl", bytes.data(), bytes.size());
    crossrender::Model m;
    ENG_CHECK(m.Load(path));
    ENG_CHECK(m.Format() == crossrender::ModelFormat::Stl);
    ENG_CHECK_EQ(m.MeshCount(), 1);
    const crossrender::MeshData& md = m.MeshAt(0);
    ENG_CHECK_EQ(md.vertices.size(), 3u);
    ENG_CHECK_EQ(md.indices.size(), 3u);
    ENG_CHECK_NEAR(md.vertices[0].normal.z, 1.0f, 1e-5f);
    ENG_CHECK_NEAR(md.vertices[1].position.x, 1.0f, 1e-5f);
    ENG_CHECK_NEAR(md.vertices[2].position.y, 1.0f, 1e-5f);
    ENG_CHECK_NEAR(m.Bounds().max.x, 1.0f, 1e-5f);
    ENG_CHECK_NEAR(m.Bounds().max.y, 1.0f, 1e-5f);
}

ENG_TEST(Model, StlAsciiTriangle) {
    const char* ascii =
        "solid crossrender\n"
        "  facet normal 0 0 1\n"
        "    outer loop\n"
        "      vertex 0 0 0\n"
        "      vertex 1 0 0\n"
        "      vertex 0 1 0\n"
        "    endloop\n"
        "  endfacet\n"
        "endsolid crossrender\n";
    crossrender::MeshData md;
    std::string err;
    ENG_CHECK(crossrender::Model::ImportStl(ascii, std::strlen(ascii), &md, &err));
    ENG_CHECK_EQ(md.vertices.size(), 3u);
    ENG_CHECK_EQ(md.indices.size(), 3u);
    ENG_CHECK_NEAR(md.vertices[0].normal.z, 1.0f, 1e-5f);
    ENG_CHECK_NEAR(md.bounds.max.y, 1.0f, 1e-5f);
}

// ---------------------------------------------------------------------------
// MagicaVoxel
// ---------------------------------------------------------------------------
ENG_TEST(Model, VoxGreedyMeshing) {
    const std::vector<crossrender::u8> bytes = VoxSample();
    const std::string path = WriteSampleBin("two_voxels.vox", bytes.data(), bytes.size());
    crossrender::Model m;
    ENG_CHECK(m.Load(path));
    ENG_CHECK(m.Format() == crossrender::ModelFormat::Vox);
    ENG_CHECK_EQ(m.MeshCount(), 1);
    const crossrender::MeshData& md = m.MeshAt(0);
    ENG_CHECK(!md.vertices.empty());
    ENG_CHECK(!md.indices.empty());

    // Два соседних вокселя: общая грань отсекается, каждая пара копланарных
    // граней объединяется, что даёт шесть квадов = 12 треугольников (36 индексов)
    // вместо 10 необъединённых граней = 20 треугольников (60 индексов).
    ENG_CHECK_MSG(md.indices.size() < 60u, "greedy meshing must merge coplanar faces");
    ENG_CHECK_EQ(md.indices.size(), 36u);
    ENG_CHECK_EQ(md.vertices.size(), 24u);  // 6 квадов x 4 вершины

    ENG_CHECK_NEAR(m.Bounds().min.x, 0.0f, 1e-5f);
    ENG_CHECK_NEAR(m.Bounds().min.y, 0.0f, 1e-5f);
    ENG_CHECK_NEAR(m.Bounds().min.z, 0.0f, 1e-5f);
    ENG_CHECK_NEAR(m.Bounds().max.x, 2.0f, 1e-5f);
    ENG_CHECK_NEAR(m.Bounds().max.y, 1.0f, 1e-5f);
    ENG_CHECK_NEAR(m.Bounds().max.z, 1.0f, 1e-5f);

    // Цвета палитры на вершину + путь к текстуре палитры.
    ENG_CHECK_EQ(m.MaterialCount(), 1);
    ENG_CHECK(m.MaterialAt(0).vertexColors);
    ENG_CHECK(m.MaterialAt(0).baseColorTex != nullptr);
    ENG_CHECK_EQ(m.Textures().size(), 1u);
    ENG_CHECK(m.MaterialAt(0).baseColorTex == &m.Textures()[0]);
    ENG_CHECK_NEAR(md.vertices[0].color.x, 1.0f, 2e-2f);   // запись палитры (255, 40, 30)
    ENG_CHECK_NEAR(md.vertices[0].uv2.x, 1.0f, 1e-5f);    // индекс палитры, упакованный в uv2
    // Нормали граней, выровненные по осям.
    ENG_CHECK_NEAR(crossrender::Length(md.vertices[0].normal), 1.0f, 1e-5f);
}

// ---------------------------------------------------------------------------
// Определение формата
// ---------------------------------------------------------------------------
ENG_TEST(Model, FormatDetectionByExtensionAndMagic) {
    const std::vector<crossrender::u8> glb = BuildGlb(TriangleGltfJson(""), TriangleBuffer());
    const std::string gltfJson = TriangleGltfJson("data:application/octet-stream;base64," +
                                                  Base64Encode(TriangleBuffer()));
    const std::vector<crossrender::u8> stl = StlBinarySample();
    const std::vector<crossrender::u8> vox = VoxSample();
    const std::string obj = "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n";
    const std::string ply = PlyAsciiSample();

    struct Case {
        const char* name;
        const void* data;
        crossrender::usize size;
        crossrender::ModelFormat expected;
    };
    const Case cases[] = {
        {"detect.obj", obj.data(), obj.size(), crossrender::ModelFormat::Obj},
        {"detect.gltf", gltfJson.data(), gltfJson.size(), crossrender::ModelFormat::Gltf},
        {"detect.glb", glb.data(), glb.size(), crossrender::ModelFormat::Glb},
        {"detect.ply", ply.data(), ply.size(), crossrender::ModelFormat::Ply},
        {"detect.stl", stl.data(), stl.size(), crossrender::ModelFormat::Stl},
        {"detect.vox", vox.data(), vox.size(), crossrender::ModelFormat::Vox},
        // Неизвестное расширение -> решают магические байты.
        {"magic.obj.dat", obj.data(), obj.size(), crossrender::ModelFormat::Obj},
        {"magic.gltf.dat", gltfJson.data(), gltfJson.size(), crossrender::ModelFormat::Gltf},
        {"magic.glb.dat", glb.data(), glb.size(), crossrender::ModelFormat::Glb},
        {"magic.ply.dat", ply.data(), ply.size(), crossrender::ModelFormat::Ply},
        {"magic.stl.dat", stl.data(), stl.size(), crossrender::ModelFormat::Stl},
        {"magic.vox.dat", vox.data(), vox.size(), crossrender::ModelFormat::Vox},
    };
    for (const Case& c : cases) {
        const std::string path = WriteSampleBin(c.name, c.data, c.size);
        crossrender::Model m;
        const bool ok = m.Load(path);
        ENG_CHECK_MSG(ok, c.name);
        ENG_CHECK_MSG(m.Format() == c.expected, c.name);
    }

    // Неизвестные данные остаются неизвестными.
    const std::vector<crossrender::u8> junk = GarbageBytes();
    const std::string junkPath = WriteSampleBin("garbage.dat", junk.data(), junk.size());
    crossrender::Model m;
    ENG_CHECK(!m.Load(junkPath));
    ENG_CHECK(m.Format() == crossrender::ModelFormat::Unknown);

    // Отсутствующий файл.
    crossrender::Model missing;
    ENG_CHECK(!missing.Load(crossrender::PathJoin(SampleDir(), "definitely_missing.obj")));
}

// ---------------------------------------------------------------------------
// Процедурные пресеты
// ---------------------------------------------------------------------------
ENG_TEST(Model, ProceduralLowPolyPresets) {
    crossrender::Model tree = crossrender::Model::MakeLowPolyTree(7);
    crossrender::Model rock = crossrender::Model::MakeLowPolyRock(11);
    crossrender::Model crystal = crossrender::Model::MakeLowPolyCrystal(13);
    crossrender::Model character = crossrender::Model::MakeLowPolyCharacter();

    crossrender::Model* models[4] = {&tree, &rock, &crystal, &character};
    const char* names[4] = {"tree", "rock", "crystal", "character"};
    for (int i = 0; i < 4; ++i) {
        crossrender::Model& m = *models[i];
        ENG_CHECK_MSG(m.Valid(), names[i]);
        ENG_CHECK_MSG(m.MeshCount() >= 1, names[i]);
        ENG_CHECK_MSG(m.MaterialCount() >= 1, names[i]);
        ENG_CHECK_MSG(m.NodeCount() >= 1, names[i]);
        ENG_CHECK_MSG(m.Bounds().Valid(), names[i]);
        const crossrender::MeshData& md = m.MeshAt(0);
        ENG_CHECK_MSG(md.vertices.size() > 24u, names[i]);
        ENG_CHECK_MSG(md.indices.size() >= 36u, names[i]);
        ENG_CHECK_MSG(md.indices.size() < 6000u, names[i]);  // лимит low-poly
        ENG_CHECK_MSG(!md.subMeshes.empty(), names[i]);
        int badNormals = 0;
        for (const crossrender::Vertex& v : md.vertices)
            if (crossrender::LengthSq(v.normal) < 0.25f) ++badNormals;
        ENG_CHECK_MSG(badNormals == 0, names[i]);
        for (const crossrender::MeshData::SubMesh& sm : md.subMeshes) {
            ENG_CHECK_MSG(sm.indexOffset + sm.indexCount <= md.indices.size(), names[i]);
            ENG_CHECK_MSG(sm.materialIndex >= 0 && sm.materialIndex < m.MaterialCount(), names[i]);
        }
    }

    // Детерминированность при заданном seed.
    crossrender::Model tree2 = crossrender::Model::MakeLowPolyTree(7);
    ENG_CHECK_NEAR(tree2.Bounds().max.y, tree.Bounds().max.y, 1e-6f);
    ENG_CHECK_EQ(tree2.MeshAt(0).vertices.size(), tree.MeshAt(0).vertices.size());
    crossrender::Model tree3 = crossrender::Model::MakeLowPolyTree(8);
    ENG_CHECK(tree3.Valid());
    ENG_CHECK(tree3.Bounds().Valid());

    // Несколько материалов с различными цветами.
    ENG_CHECK(tree.MaterialCount() >= 2);
    ENG_CHECK(crystal.MaterialAt(0).emissive.r + crystal.MaterialAt(0).emissive.g +
                  crystal.MaterialAt(0).emissive.b >
              0.0f);
}

ENG_TEST(Model, BakeTransformAndEnsureNormals) {
    crossrender::Model m = crossrender::Model::MakeLowPolyTree(3);
    const crossrender::Vec3 before = m.Bounds().max;
    m.BakeTransform(crossrender::Mat4::Scale(crossrender::Vec3{2, 2, 2}));
    ENG_CHECK(m.Bounds().Valid());
    ENG_CHECK_NEAR(m.Bounds().max.x, before.x * 2.0f, 1e-4f);
    ENG_CHECK_NEAR(m.Bounds().max.y, before.y * 2.0f, 1e-4f);
    ENG_CHECK_NEAR(m.Bounds().max.z, before.z * 2.0f, 1e-4f);

    // Меш без нормалей получает сглаженные.
    crossrender::MeshData md;
    crossrender::Vertex a, b, c;
    a.position = {0, 0, 0};
    b.position = {1, 0, 0};
    c.position = {0, 1, 0};
    a.normal = b.normal = c.normal = crossrender::Vec3{0, 0, 0};
    md.vertices = {a, b, c};
    md.indices = {0, 1, 2};
    crossrender::Model m2;
    m2.Meshes().push_back(md);
    m2.EnsureNormals();
    for (const crossrender::Vertex& v : m2.MeshAt(0).vertices) ENG_CHECK_NEAR(crossrender::Length(v.normal), 1.0f, 1e-4f);
}

// ---------------------------------------------------------------------------
// Граничные случаи glTF: чередующиеся bufferViews с шагом (stride), нормализованные
// целочисленные атрибуты, triangle strip, узлы-матрицы и sparse-аксессоры.
// ---------------------------------------------------------------------------
ENG_TEST(Model, GltfStridedNormalizedAndMatrixNode) {
    // 4 вершины, stride 32: position(12) + normal(12) + normalized ubyte4 цвет(4) + паддинг(4).
    std::vector<crossrender::u8> bin;
    static const crossrender::f32 kPos[4][3] = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}};
    for (int i = 0; i < 4; ++i) {
        PutF32LE(bin, kPos[i][0]);
        PutF32LE(bin, kPos[i][1]);
        PutF32LE(bin, kPos[i][2]);
        PutF32LE(bin, 0.0f);
        PutF32LE(bin, 0.0f);
        PutF32LE(bin, 1.0f);
        bin.push_back(255);
        bin.push_back(128);
        bin.push_back(0);
        bin.push_back(255);
        PutU32LE(bin, 0);  // паддинг
    }
    for (crossrender::u16 i : {0, 1, 2, 3}) PutU16LE(bin, i);

    const std::string uri = "data:application/octet-stream;base64," + Base64Encode(bin);
    std::string json = "{";
    json += "\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],";
    json += "\"nodes\":[{\"name\":\"strip\",\"mesh\":0,\"matrix\":[1,0,0,0,0,1,0,0,0,0,1,0,5,0,0,1]}],";
    json += "\"meshes\":[{\"name\":\"strip\",\"primitives\":[{\"attributes\":{\"POSITION\":0,\"NORMAL\":1,"
            "\"COLOR_0\":2},\"indices\":3,\"material\":0,\"mode\":5}]}],";
    json += "\"materials\":[{\"name\":\"tinted\",\"pbrMetallicRoughness\":{\"baseColorFactor\":[0.2,0.4,0.6,"
            "1.0]}}],";
    json += "\"buffers\":[{\"byteLength\":136,\"uri\":\"" + uri + "\"}],";
    json += "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":128,\"byteStride\":32},";
    json += "{\"buffer\":0,\"byteOffset\":128,\"byteLength\":8}],";
    json += "\"accessors\":[{\"bufferView\":0,\"byteOffset\":0,\"componentType\":5126,\"count\":4,\"type\":\"VEC3\"},";
    json += "{\"bufferView\":0,\"byteOffset\":12,\"componentType\":5126,\"count\":4,\"type\":\"VEC3\"},";
    json += "{\"bufferView\":0,\"byteOffset\":24,\"componentType\":5121,\"count\":4,\"type\":\"VEC4\","
            "\"normalized\":true},";
    json += "{\"bufferView\":1,\"componentType\":5123,\"count\":4,\"type\":\"SCALAR\"},";
    // Неиспользуемый sparse-аксессор: должен попасть в лог и быть проигнорирован, но не прочитан.
    json += "{\"bufferView\":0,\"componentType\":5126,\"count\":1,\"type\":\"VEC3\",\"sparse\":{\"count\":1,"
            "\"indices\":{\"bufferView\":1,\"byteOffset\":0,\"componentType\":5123},"
            "\"values\":{\"bufferView\":1,\"byteOffset\":0,\"componentType\":5126}}}]";
    json += "}";

    crossrender::Model m;
    ENG_CHECK(m.LoadFromMemory(json.data(), json.size(), crossrender::ModelFormat::Gltf, "strip"));
    ENG_CHECK_EQ(m.MeshCount(), 1);
    const crossrender::MeshData& md = m.MeshAt(0);
    ENG_CHECK_EQ(md.vertices.size(), 4u);
    ENG_CHECK_EQ(md.indices.size(), 6u);  // mode 5: 4 вершины strip -> 2 треугольника
    // Чередующийся (interleaved) атрибут NORMAL.
    ENG_CHECK_NEAR(md.vertices[2].normal.z, 1.0f, 1e-5f);
    // Нормализованный беззнаковый байтовый COLOR_0.
    ENG_CHECK_NEAR(md.vertices[0].color.x, 1.0f, 2e-3f);
    ENG_CHECK_NEAR(md.vertices[0].color.y, 128.0f / 255.0f, 2e-3f);
    ENG_CHECK_NEAR(md.vertices[0].color.z, 0.0f, 2e-3f);
    ENG_CHECK_NEAR(md.vertices[0].color.w, 1.0f, 2e-3f);
    ENG_CHECK(m.MaterialAt(0).vertexColors);
    // Узел, построенный из матрицы в column-major порядке.
    ENG_CHECK(m.NodeAt(0).hasMatrix);
    ENG_CHECK_NEAR(m.NodeAt(0).translation.x, 5.0f, 1e-5f);
    ENG_CHECK(m.Bounds().Valid());
    ENG_CHECK_NEAR(m.Bounds().min.x, 5.0f, 1e-4f);
    ENG_CHECK_NEAR(m.Bounds().max.x, 6.0f, 1e-4f);
    ENG_CHECK_NEAR(m.Bounds().max.y, 1.0f, 1e-4f);
}

ENG_TEST(Model, GltfExternalBufferUri) {
    const std::vector<crossrender::u8> bin = TriangleBuffer();
    ENG_CHECK(crossrender::WriteBinaryFile(crossrender::PathJoin(SampleDir(), "triangle_ext.bin"), bin.data(), bin.size()));
    const std::string json = TriangleGltfJson("triangle_ext.bin");
    const std::string path = WriteSample("triangle_ext.gltf", json);

    crossrender::Model m;
    ENG_CHECK(m.Load(path));  // URI буфера разрешается относительно .gltf
    CheckTriangleGltf(m);
}

// ---------------------------------------------------------------------------
// Триангуляция веером для полигонов PLY
// ---------------------------------------------------------------------------
ENG_TEST(Model, PlyPolygonFanTriangulation) {
    const char* ply =
        "ply\n"
        "format ascii 1.0\n"
        "element vertex 4\n"
        "property float x\nproperty float y\nproperty float z\n"
        "element face 1\n"
        "property list uchar int vertex_indices\n"
        "end_header\n"
        "0 0 0\n1 0 0\n1 0 1\n0 0 1\n"
        "4 0 1 2 3\n";
    crossrender::MeshData md;
    std::string err;
    ENG_CHECK(crossrender::Model::ImportPly(ply, std::strlen(ply), &md, &err));
    ENG_CHECK_EQ(md.vertices.size(), 4u);
    ENG_CHECK_EQ(md.indices.size(), 6u);  // квад -> 2 треугольника
    ENG_CHECK_NEAR(md.bounds.max.z, 1.0f, 1e-5f);
    // Сгенерированные нормали (в файле их нет).
    for (const crossrender::Vertex& v : md.vertices) ENG_CHECK_NEAR(crossrender::Length(v.normal), 1.0f, 1e-3f);
}

// ---------------------------------------------------------------------------
// Граф сцены VOX: несколько моделей, объединяемых с их сдвигами nTRN
// ---------------------------------------------------------------------------
ENG_TEST(Model, VoxSceneGraphTranslation) {
    std::vector<std::vector<crossrender::u8>> chunks;
    chunks.push_back(MakeSizeChunk(1, 1, 1));
    chunks.push_back(MakeXyziChunk({{{0, 0, 0, 1}}}));  // модель 0
    chunks.push_back(MakeSizeChunk(1, 1, 1));
    chunks.push_back(MakeXyziChunk({{{0, 0, 0, 1}}}));  // модель 1
    chunks.push_back(MakeRgbaChunk());

    // nTRN(узел 0) сдвигает своего потомка (узел 1) на (4, 0, 0).
    std::vector<crossrender::u8> trn;
    PutI32LE(trn, 0);
    PutVoxDict(trn, {{"_name", "shifted"}});
    PutI32LE(trn, 1);
    PutI32LE(trn, -1);
    PutI32LE(trn, -1);
    PutI32LE(trn, 1);
    PutVoxDict(trn, {{"_t", "4 0 0"}});
    chunks.push_back(MakeVoxChunk("nTRN", trn));

    // nSHP(узел 1) -> модель 1, nSHP(узел 2) -> модель 0 (без сдвига).
    std::vector<crossrender::u8> shp1;
    PutI32LE(shp1, 1);
    PutVoxDict(shp1, {});
    PutI32LE(shp1, 1);
    PutI32LE(shp1, 1);
    PutVoxDict(shp1, {});
    chunks.push_back(MakeVoxChunk("nSHP", shp1));

    std::vector<crossrender::u8> shp0;
    PutI32LE(shp0, 2);
    PutVoxDict(shp0, {});
    PutI32LE(shp0, 1);
    PutI32LE(shp0, 0);
    PutVoxDict(shp0, {});
    chunks.push_back(MakeVoxChunk("nSHP", shp0));

    // nGRP для нас информационный, его нужно просто пройти.
    std::vector<crossrender::u8> grp;
    PutI32LE(grp, 3);
    PutVoxDict(grp, {});
    PutI32LE(grp, 2);
    PutI32LE(grp, 1);
    PutI32LE(grp, 2);
    chunks.push_back(MakeVoxChunk("nGRP", grp));

    const std::vector<crossrender::u8> bytes = MakeVoxFile(chunks);
    const std::string path = WriteSampleBin("translated.vox", bytes.data(), bytes.size());
    crossrender::Model m;
    ENG_CHECK(m.Load(path));
    ENG_CHECK(m.Format() == crossrender::ModelFormat::Vox);
    ENG_CHECK_EQ(m.MeshCount(), 1);
    // Два куба 1x1x1, один из них в x = 4.
    ENG_CHECK_NEAR(m.Bounds().min.x, 0.0f, 1e-5f);
    ENG_CHECK_NEAR(m.Bounds().max.x, 5.0f, 1e-5f);
    ENG_CHECK_NEAR(m.Bounds().max.y, 1.0f, 1e-5f);
    ENG_CHECK_NEAR(m.Bounds().max.z, 1.0f, 1e-5f);
    ENG_CHECK_EQ(m.MeshAt(0).indices.size(), 72u);  // 2 куба x 6 квадов x 6 индексов
}


// ---------------------------------------------------------------------------
// Загрузка на GPU / уничтожение (контекст не нужен: Create должен корректно обработать неудачу)
// ---------------------------------------------------------------------------
ENG_TEST(Model, UploadToGpuAndDestroy) {
    crossrender::Model m = crossrender::Model::MakeLowPolyRock(5);
    ENG_CHECK(m.Valid());
    m.UploadToGpu();
    if (crossrender::test::EnsureGLContext()) {
        ENG_CHECK(m.Uploaded());
    } else {
        ENG_CHECK(!m.Uploaded());  // headless: загрузка не удаётся, но не должна приводить к падению
    }
    m.Destroy();
    ENG_CHECK(!m.Valid());
    ENG_CHECK_EQ(m.MeshCount(), 0);
    ENG_CHECK_EQ(m.MaterialCount(), 0);
    ENG_CHECK_EQ(m.NodeCount(), 0);
    ENG_CHECK(!m.Uploaded());
    ENG_CHECK(m.Format() == crossrender::ModelFormat::Unknown);
    ENG_CHECK(m.Name().empty());
}

// ---------------------------------------------------------------------------
// Текстуры материалов OBJ (Map_Kd)
// ---------------------------------------------------------------------------
ENG_TEST(Model, ObjMissingTextureIsNonFatal) {
    const std::string dir = SampleDir();
    ENG_CHECK(crossrender::WriteTextFile(crossrender::PathJoin(dir, "missing_tex.mtl"),
                                 "newmtl t\nKd 0.5 0.25 0.75\nmap_Kd no_such_file.png\n"));
    const std::string path = WriteSample(
        "missing_tex.obj", "mtllib missing_tex.mtl\nv 0 0 0\nv 1 0 0\nv 0 1 0\nusemtl t\nf 1 2 3\n");

    crossrender::Model m;
    ENG_CHECK(m.Load(path));
    ENG_CHECK_EQ(m.MaterialCount(), 1);
    ENG_CHECK_STR_EQ(m.MaterialAt(0).name, "t");
    ENG_CHECK_NEAR(m.MaterialAt(0).baseColor.r, 0.5f, 1e-4f);
    ENG_CHECK_NEAR(m.MaterialAt(0).baseColor.g, 0.25f, 1e-4f);
    ENG_CHECK_NEAR(m.MaterialAt(0).baseColor.b, 0.75f, 1e-4f);
    ENG_CHECK(m.MaterialAt(0).baseColorTex == nullptr);
    ENG_CHECK_EQ(m.Textures().size(), 0u);
}

ENG_TEST(Model, ObjMapKdTextureDecode) {
    const std::string dir = SampleDir();
    std::vector<crossrender::u8> pixels(2 * 2 * 4, 255);
    pixels[0] = 255;
    pixels[1] = 0;
    pixels[2] = 0;  // один красный тексел делает это настоящим изображением
    if (!crossrender::Texture::EncodePng(crossrender::PathJoin(dir, "map_kd.png"), 2, 2, 4, pixels.data()))
        ENG_SKIP("PNG encoding unavailable");

    ENG_CHECK(crossrender::WriteTextFile(crossrender::PathJoin(dir, "map_kd.mtl"),
                                 "newmtl textured\nKd 1 1 1\nmap_Kd map_kd.png\n"));
    const std::string path = WriteSample("map_kd.obj",
                                         "mtllib map_kd.mtl\nv 0 0 0\nv 1 0 0\nv 0 1 0\n"
                                         "vt 0 0\nvt 1 0\nvt 0 1\nusemtl textured\nf 1/1 2/2 3/3\n");

    crossrender::Model m;
    ENG_CHECK(m.Load(path));  // должно завершиться успехом, даже если текстуру не удаётся загрузить
    ENG_CHECK_EQ(m.MaterialCount(), 1);

    // Декодирование текстуры требует GL-контекста; проверяем только при его наличии.
    ENG_REQUIRE_GL();
    ENG_CHECK_EQ(m.Textures().size(), 1u);
    ENG_CHECK(m.MaterialAt(0).baseColorTex == &m.Textures()[0]);
}
