//
// CrossRender
// Автор: xcodestream <xcodestream@gmail.com>
// Лицензия: MIT
// Дата: 15.05.2026
// Описание: внутренние сетевые помощники: SHA-1/base64 и кодек кадров RFC 6455 для тестов.
//
#pragma once

#include "crossrender/core/Base.h"

#include <string>
#include <vector>

namespace crossrender {
namespace net_internal {

// ---------------------------------------------------------------------------
// SHA-1 (FIPS 180-1). Записывает 20 «сырых» байтов дайджеста.
// ---------------------------------------------------------------------------
void Sha1(const void* data, usize size, u8 out[20]);
std::string Sha1Hex(const void* data, usize size);

// ---------------------------------------------------------------------------
// Base64 (RFC 4648, стандартный алфавит, выравнивание символом '=').
// ---------------------------------------------------------------------------
std::string Base64Encode(const void* data, usize size);
// Возвращает false при недопустимых символах. Пробельные символы игнорируются.
bool Base64Decode(const std::string& text, std::vector<u8>* out);

// ---------------------------------------------------------------------------
// RFC 6455 handshake
// ---------------------------------------------------------------------------
constexpr const char* kWebSocketGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

// base64(SHA1(clientKey + GUID)) — ожидаемое значение Sec-WebSocket-Accept.
std::string WebSocketAcceptKey(const std::string& clientKey);
// 16 случайных байтов в кодировке base64 (свежий Sec-WebSocket-Key).
std::string GenerateWebSocketKey();

// (Синтетические) рукопожатия, запускаемые через InjectForTest(), используют
// примерный ключ из RFC 6455, чтобы тесты могли предсказать ожидаемый accept-
// токен. Это также векторы, которые требуются задачей:
//   kWsTestClientKey == "dGhlIHNhbXBsZSBub25jZQ=="
//   kWsTestAccept    == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo="
extern const char* const kWsTestClientKey;
extern const char* const kWsTestAccept;

// ---------------------------------------------------------------------------
// WebSocket framing (RFC 6455 section 5)
// ---------------------------------------------------------------------------
enum WsOpcode : u8 {
    kWsContinuation = 0x0,
    kWsText = 0x1,
    kWsBinary = 0x2,
    kWsClose = 0x8,
    kWsPing = 0x9,
    kWsPong = 0xA,
};

// Собирает полный кадр. `maskKey` записывается big-endian и используется для
// маскирования полезной нагрузки при `mask` == true (клиенты маскируют; серверы — нет).
std::vector<u8> WsBuildFrame(u8 opcode, const void* payload, usize size, bool fin,
                             bool mask, u32 maskKey);

struct WsFrame {
    u8 opcode = 0;
    bool fin = true;
    bool masked = false;
    std::vector<u8> payload;
};

// Разбирает один кадр из `data`. Замаскированные кадры размаскируются на месте
// (снисходительно: парсер принимает оба направления, чтобы тесты могли
// прогонять клиентские кадры в обе стороны).
//   возвращает  1 -> кадр полный, использовано `*consumed` байтов
//               0 -> нужно больше байтов
//              -1 -> ошибка протокола (плохой opcode / биты RSV / кадр слишком велик)
int WsParseFrame(const u8* data, usize size, usize* consumed, WsFrame* out);

}  // namespace net_internal
}  // namespace crossrender
