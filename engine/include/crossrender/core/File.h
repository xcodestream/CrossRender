//
// CrossRender
// Автор: xcodestream <xcodestream@gmail.com>
// Лицензия: MIT
// Дата: 15.05.2026
// Описание: виртуальная файловая система: пути, корни ресурсов, чтение и запись файлов.
//
#pragma once

#include "crossrender/core/Base.h"

#include <string>
#include <vector>

namespace crossrender {

using ByteBuffer = std::vector<u8>;

// Абстракция файловой системы. Единственный глобальный экземпляр привязывается
// платформенным слоем при старте (см. FileSystemBind). Пути используют '/' на всех платформах.
class FileSystem {
public:
    virtual ~FileSystem() = default;

    // имена корней: "assets" (только чтение, поставляется с приложением), "user" (на запись).
    virtual bool ReadFile(const std::string& path, ByteBuffer* out) = 0;
    virtual bool WriteFile(const std::string& path, const void* data, usize size) = 0;
    virtual bool Exists(const std::string& path) = 0;
    virtual std::vector<std::string> ListDir(const std::string& path) = 0;
    virtual std::string ResolvePath(const std::string& path) = 0;
    virtual i64 FileTime(const std::string& path) { (void)path; return 0; }
};

// Устанавливает файловую систему для всего процесса. Вызывается платформенным слоем.
void FileSystemBind(FileSystem* fs);
FileSystem& FS();

// Удобные помощники (при ошибке возвращают пустой результат).
std::string ReadTextFile(const std::string& path);
ByteBuffer ReadBinaryFile(const std::string& path);
bool WriteTextFile(const std::string& path, const std::string& text);
bool WriteBinaryFile(const std::string& path, const void* data, usize size);
bool FileExists(const std::string& path);
bool DirectoryExists(const std::string& path);
// Создаёт `path` и все недостающие родительские каталоги (как `mkdir -p`).
// Возвращает true, если после вызова каталог существует.
bool CreateDirectories(const std::string& path);

// Помощники для работы с путями.
std::string PathJoin(const std::string& a, const std::string& b);
std::string PathDir(const std::string& p);
std::string PathBase(const std::string& p);
std::string PathExt(const std::string& p);  // в нижнем регистре, с точкой
std::string PathNormalize(const std::string& p);

// Переопределение корня ассетов (командная строка / определение бандла).
void SetAssetRoot(const std::string& root);
const std::string& GetAssetRoot();
void SetUserRoot(const std::string& root);
const std::string& GetUserRoot();

// Загрузка по сети (только WASM; на нативе возвращает false). Используется для стриминга ассетов.
bool FetchUrl(const std::string& url, ByteBuffer* out);

}  // namespace crossrender
