#include "crossrender/core/File.h"

#if defined(ENG_PLATFORM_MACOS) || defined(ENG_PLATFORM_IOS)
#  include <mach-o/dyld.h>
#elif defined(ENG_PLATFORM_LINUX) || defined(ENG_PLATFORM_ANDROID)
#  include <unistd.h>
#endif

#include "crossrender/core/Log.h"
#include "crossrender/core/Time.h"

#include <cctype>
#include <cstdio>
#include <fstream>
#include <filesystem>

#if defined(ENG_PLATFORM_WINDOWS)
#  include <direct.h>
#  include <windows.h>
#else
#  include <dirent.h>
#  include <sys/stat.h>
#  include <unistd.h>
#endif

namespace crossrender {
namespace {

FileSystem* g_fs = nullptr;
std::string g_assetRoot;
std::string g_userRoot;

// Файловая система с корнем в каталоге ассетов плюс записываемый пользовательский каталог.
// Порядок разрешения путей:
//   1. абсолютные пути используются как есть
//   2. "user/..." -> <userRoot>/...
//   3. <assetRoot>/<path>
//   4. <path> относительно рабочего каталога
class NativeFileSystem : public FileSystem {
public:
    bool ReadFile(const std::string& path, ByteBuffer* out) override {
        std::string resolved = ResolvePath(path);
        if (resolved.empty()) return false;
        std::ifstream f(resolved, std::ios::binary);
        if (!f.is_open()) return false;
        f.seekg(0, std::ios::end);
        std::streamoff size = f.tellg();
        if (size < 0) return false;
        f.seekg(0, std::ios::beg);
        out->resize(static_cast<usize>(size));
        if (size > 0) f.read(reinterpret_cast<char*>(out->data()), size);
        return f.good() || f.eof();
    }

    bool WriteFile(const std::string& path, const void* data, usize size) override {
        std::string resolved = ResolveWritePath(path);
        if (resolved.empty()) return false;
        std::error_code ec;
        std::filesystem::path p(resolved);
        if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path(), ec);
        std::ofstream f(resolved, std::ios::binary | std::ios::trunc);
        if (!f.is_open()) {
            ENG_LOGE("fs", "cannot write %s", resolved.c_str());
            return false;
        }
        if (size > 0) f.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
        return f.good();
    }

    bool Exists(const std::string& path) override {
        std::string resolved = ResolvePath(path);
        if (resolved.empty()) return false;
        std::error_code ec;
        return std::filesystem::exists(resolved, ec);
    }

    std::vector<std::string> ListDir(const std::string& path) override {
        std::vector<std::string> out;
        std::string resolved = ResolvePath(path);
        if (resolved.empty()) return out;
        std::error_code ec;
        for (auto it = std::filesystem::directory_iterator(resolved, ec);
             !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
            out.push_back(it->path().filename().string());
        }
        return out;
    }

    std::string ResolvePath(const std::string& path) override {
        using namespace std::filesystem;
        if (path.empty()) return {};
        if (path[0] == '/') return path;
#if defined(ENG_PLATFORM_WINDOWS)
        if (path.size() > 2 && path[1] == ':') return path;
#endif
        if (path.rfind("user/", 0) == 0) {
            std::string rest = path.substr(5);
            return PathJoin(GetUserRoot(), rest);
        }
        std::error_code ec;
        if (!g_assetRoot.empty()) {
            std::string p = PathJoin(g_assetRoot, path);
            if (exists(p, ec)) return p;
            // Позволяем "assets/foo" разрешаться в "<root>/foo".
            if (path.rfind("assets/", 0) == 0) {
                std::string p2 = PathJoin(g_assetRoot, path.substr(7));
                if (exists(p2, ec)) return p2;
            }
        }
        if (exists(path, ec)) return path;
        return {};
    }

    std::string ResolveWritePath(const std::string& path) {
        if (path.empty()) return {};
        if (path[0] == '/') return path;
        if (path.rfind("user/", 0) == 0) return PathJoin(GetUserRoot(), path.substr(5));
        if (path.rfind("assets/", 0) == 0) {
            std::string rest = path.substr(7);
            return g_assetRoot.empty() ? PathJoin(GetUserRoot(), rest)
                                       : PathJoin(g_assetRoot, rest);
        }
        return PathJoin(GetUserRoot(), path);
    }

    i64 FileTime(const std::string& path) override {
        std::string resolved = ResolvePath(path);
        if (resolved.empty()) return 0;
        std::error_code ec;
        auto t = std::filesystem::last_write_time(resolved, ec);
        if (ec) return 0;
        return static_cast<i64>(t.time_since_epoch().count());
    }
};

NativeFileSystem g_nativeFs;

std::string ExecutablePathFallback() {
#if defined(ENG_PLATFORM_WINDOWS)
    char buf[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    return n > 0 ? std::string(buf, n) : std::string();
#elif defined(ENG_PLATFORM_MACOS) || defined(ENG_PLATFORM_IOS)
    // Здесь /proc/self/exe не существует, поэтому без этого корень ассетов
    // вычислялся от *текущего каталога*: приложение находило свои ассеты,
    // только если было запущено из корня репозитория, а во всех остальных
    // случаях молча откатывалось к процедурному шрифту без текстур вовсе.
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    if (size == 0) return std::string();
    std::vector<char> buf(size + 1, 0);
    if (_NSGetExecutablePath(buf.data(), &size) != 0) return std::string();
    return std::string(buf.data());
#elif defined(ENG_PLATFORM_LINUX) || defined(ENG_PLATFORM_ANDROID)
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return std::string();
    buf[n] = 0;
    return std::string(buf);
#else
    // WASM и всё остальное: пути к исполняемому файлу, от которого можно подниматься, нет.
    return std::string();
#endif
}

std::string DefaultAssetRoot() {
    using namespace std::filesystem;
    std::error_code ec;
    // Поднимаемся от исполняемого файла в поисках каталога "assets". Разрешение
    // от исполняемого файла, а не от рабочего каталога, позволяет запускать
    // бинарник откуда угодно - двойным щелчком, из IDE, из любой оболочки.
    std::string exeDir = PathDir(ExecutablePathFallback());
    if (!exeDir.empty()) {
        path p = weakly_canonical(path(exeDir), ec);
        if (ec) p = path(exeDir);
        for (int i = 0; i < 6 && !p.empty(); ++i) {
            std::error_code e2;
            if (exists(p / "assets", e2) && !e2) return (p / "assets").string();
            // Пример хранит свои сгенерированные ассеты внутри examples/.
            if (exists(p / "examples/assets", e2) && !e2) return (p / "examples/assets").string();
            const path parent = p.parent_path();
            if (parent == p) break;
            p = parent;
        }
    }
    // Откат к каталогу ассетов рабочего каталога.
    if (exists("assets", ec)) return absolute("assets", ec).string();
    if (exists("examples/assets", ec)) return absolute("examples/assets", ec).string();
    return "assets";
}

std::string DefaultUserRoot() {
    using namespace std::filesystem;
    std::error_code ec;
#if defined(ENG_PLATFORM_WINDOWS)
    const char* appdata = std::getenv("APPDATA");
    path base = appdata ? path(appdata) : temp_directory_path(ec);
#elif defined(ENG_PLATFORM_MACOS)
    const char* home = std::getenv("HOME");
    path base = home ? path(home) / "Library" / "Application Support" : temp_directory_path(ec);
#elif defined(ENG_PLATFORM_IOS) || defined(ENG_PLATFORM_ANDROID) || defined(ENG_PLATFORM_WASM)
    path base = "/tmp";
#else
    const char* home = std::getenv("HOME");
    path base = home ? path(home) / ".local" / "share" : temp_directory_path(ec);
#endif
    path dir = base / "CrossRender";
    create_directories(dir, ec);
    // Песочницы/CI могут отказать в записи по стандартному пути; откатываемся
    // к временному каталогу, чтобы у движка всегда был записываемый пользовательский корень.
    auto writable = [](const path& p) {
        std::error_code e;
        if (!std::filesystem::exists(p, e)) return false;
        path probe = p / ".ge-write-probe";
        std::ofstream f(probe, std::ios::binary | std::ios::trunc);
        if (!f.is_open()) return false;
        f << "1";
        f.close();
        std::filesystem::remove(probe, e);
        return true;
    };
    if (writable(dir)) return dir.string();
    std::error_code e2;
    path fallback = std::filesystem::temp_directory_path(e2) / "CrossRender";
    std::filesystem::create_directories(fallback, e2);
    ENG_LOGW("fs", "user root %s is not writable; using %s", dir.string().c_str(),
             fallback.string().c_str());
    return fallback.string();
}

}  // namespace

void FileSystemBind(FileSystem* fs) { g_fs = fs; }

FileSystem& FS() {
    if (!g_fs) g_fs = &g_nativeFs;
    return *g_fs;
}

void SetAssetRoot(const std::string& root) { g_assetRoot = PathNormalize(root); }
const std::string& GetAssetRoot() {
    if (g_assetRoot.empty()) g_assetRoot = DefaultAssetRoot();
    return g_assetRoot;
}
void SetUserRoot(const std::string& root) {
    g_userRoot = PathNormalize(root);
    std::error_code ec;
    std::filesystem::create_directories(g_userRoot, ec);
}
const std::string& GetUserRoot() {
    if (g_userRoot.empty()) g_userRoot = DefaultUserRoot();
    return g_userRoot;
}

std::string ReadTextFile(const std::string& path) {
    ByteBuffer data;
    if (!FS().ReadFile(path, &data)) return {};
    return std::string(reinterpret_cast<const char*>(data.data()), data.size());
}

ByteBuffer ReadBinaryFile(const std::string& path) {
    ByteBuffer data;
    FS().ReadFile(path, &data);
    return data;
}

bool WriteTextFile(const std::string& path, const std::string& text) {
    return FS().WriteFile(path, text.data(), text.size());
}

bool WriteBinaryFile(const std::string& path, const void* data, usize size) {
    return FS().WriteFile(path, data, size);
}

bool FileExists(const std::string& path) { return FS().Exists(path); }

bool FetchUrl(const std::string& url, ByteBuffer* out) {
    (void)url;
    (void)out;
    return false;
}

bool DirectoryExists(const std::string& path) {
    if (path.empty()) return false;
    std::error_code ec;
    std::string resolved = FS().ResolvePath(path);
    if (resolved.empty()) resolved = path;
    return std::filesystem::is_directory(resolved, ec);
}

bool CreateDirectories(const std::string& path) {
    if (path.empty()) return false;
    std::error_code ec;
    std::string resolved = FS().ResolvePath(path);
    if (resolved.empty()) resolved = path;
    if (std::filesystem::is_directory(resolved, ec)) return true;
    std::filesystem::create_directories(resolved, ec);
    return std::filesystem::is_directory(resolved, ec);
}

std::string PathJoin(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    if (b[0] == '/' || (b.size() > 1 && b[1] == ':')) return b;
    if (a.back() == '/' || a.back() == '\\') return a + b;
    return a + "/" + b;
}

std::string PathDir(const std::string& p) {
    usize slash = p.find_last_of("/\\");
    if (slash == std::string::npos) return ".";
    if (slash == 0) return "/";
    return p.substr(0, slash);
}

std::string PathBase(const std::string& p) {
    usize slash = p.find_last_of("/\\");
    return slash == std::string::npos ? p : p.substr(slash + 1);
}

std::string PathExt(const std::string& p) {
    std::string base = PathBase(p);
    usize dot = base.find_last_of('.');
    if (dot == std::string::npos) return {};
    std::string ext = base.substr(dot);
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext;
}

std::string PathNormalize(const std::string& p) {
    if (p.empty()) return p;
    std::error_code ec;
    std::filesystem::path path(p);
    auto norm = path.lexically_normal();
    std::string s = norm.string();
    while (s.size() > 1 && (s.back() == '/' || s.back() == '\\')) s.pop_back();
    return s;
}

}  // namespace crossrender
