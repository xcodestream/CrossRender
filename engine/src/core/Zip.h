//
// CrossRender
// Автор: xcodestream <xcodestream@gmail.com>
// Лицензия: MIT
// Дата: 15.05.2026
// Описание: внутренний читатель ZIP-архивов и декодер DEFLATE (RFC 1950/1951).
//
#pragma once

#include "crossrender/core/File.h"

#include <string>
#include <vector>

namespace crossrender {

// Декодирует сырой поток DEFLATE (RFC 1951) целиком. expectedSize, если задан,
// используется только как подсказка для резервирования буфера и как предохранитель
// от зацикленных повреждённых данных (вывод крупнее expectedSize считается ошибкой).
// Возвращает false на усечённом или повреждённом вводе; буфер при этом может
// содержать частичный результат и не должен использоваться.
bool Inflate(const u8* data, usize size, usize expectedSize, ByteBuffer* out);

// Возвращает имена всех файловых записей архива (записи-каталоги с завершающим
// '/' отбрасываются). Возвращает false, если структура архива не распознана.
bool ZipListEntries(const ByteBuffer& zip, std::vector<std::string>* out);

// Извлекает запись архива по имени (разделитель путей в архиве — '/'). Методы
// хранения (0) и deflate (8) поддерживаются; шифрованные и ZIP64-записи — нет.
bool ZipReadFile(const ByteBuffer& zip, const std::string& name, ByteBuffer* out);

}  // namespace crossrender
