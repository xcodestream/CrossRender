// Реализация сети: работа с адресами, TCP (клиент + сервер), UDP и нативный
// WebSocket-клиент (RFC 6455) с бэкендом WASM/браузер.
//
// Замечания по дизайну
//  * Каждый сокет неблокирующий; ни один вызов не блокируется бесконечно. Отправка
//    использует ограниченный цикл poll, подключение — ограниченный цикл poll,
//    приём никогда не блокируется.
//  * Исключений нигде нет; сбои проявляются как `false` + LastError().
//  * В сборке WASM нет BSD-сокетов: TcpSocket/UdpSocket сообщают понятную
//    ошибку, WebSocket делегирует JS WebSocket API через `eng_js_*`.

#include "crossrender/net/Net.h"

#include "crossrender/core/Log.h"

#include "net/NetInternal.h"

#include <map>
#include <chrono>
#include <cstdio>
#include <random>
#include <string>
#include <vector>
#include <cstring>
#include <algorithm>

// ---------------------------------------------------------------------------
// Платформенный сокетный слой
// ---------------------------------------------------------------------------
#if defined(ENG_PLATFORM_WINDOWS)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
using EngSocket = SOCKET;
using EngSsize = int;
const EngSocket kEngInvalidSocket = INVALID_SOCKET;
#  define ENG_SOCK_ERR() WSAGetLastError()
#elif defined(ENG_PLATFORM_WASM)
using EngSocket = int;
using EngSsize = long;
const EngSocket kEngInvalidSocket = -1;
#  define ENG_SOCK_ERR() 0
#else
#  include <arpa/inet.h>
#  include <errno.h>
#  include <fcntl.h>
#  include <netdb.h>
#  include <netinet/in.h>
#  include <netinet/tcp.h>
#  include <poll.h>
#  include <sys/socket.h>
#  include <sys/types.h>
#  include <unistd.h>
using EngSocket = int;
using EngSsize = ssize_t;
const EngSocket kEngInvalidSocket = -1;
#  define ENG_SOCK_ERR() errno
#endif

#if defined(ENG_PLATFORM_WASM)
#  if defined(__EMSCRIPTEN__)
#    include <emscripten/emscripten.h>
#    define ENG_EXPORT_JS extern "C" EMSCRIPTEN_KEEPALIVE
#  else
#    define ENG_EXPORT_JS extern "C"
#  endif
// Предоставляется JS-мостом платформенного слоя WASM.
extern "C" {
int eng_js_ws_create(const char* url, const char* protocols);
void eng_js_ws_send_text(int id, const char* utf8);
void eng_js_ws_send_binary(int id, const void* data, int len);
void eng_js_ws_close(int id, int code, const char* reason);
int eng_js_ws_state(int id);
int eng_js_http_get(const char* url, void** outData, int* outLen);
void eng_js_free(void* p);
}
#else
#  define ENG_EXPORT_JS extern "C"
#endif

namespace crossrender {
namespace {

// ---------------------------------------------------------------------------
// Малые переносимые помощники
// ---------------------------------------------------------------------------
[[maybe_unused]] f64 NowSeconds() {
    using namespace std::chrono;
    return duration<f64>(steady_clock::now().time_since_epoch()).count();
}

std::string ToLowerAscii(const std::string& s) {
    std::string out = s;
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

std::string TrimAscii(const std::string& s) {
    usize b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n')) --e;
    return s.substr(b, e - b);
}

bool ParseIPv4Literal(const std::string& s, u32* out) {
    u32 parts[4] = {0, 0, 0, 0};
    int idx = 0;
    u32 cur = 0;
    bool any = false;
    for (usize i = 0; i <= s.size(); ++i) {
        const char c = (i < s.size()) ? s[i] : '.';
        if (c >= '0' && c <= '9') {
            cur = cur * 10u + static_cast<u32>(c - '0');
            if (cur > 255u) return false;
            any = true;
        } else if (c == '.') {
            if (!any || idx >= 4) return false;
            parts[idx++] = cur;
            cur = 0;
            any = false;
        } else {
            return false;
        }
    }
    if (idx != 4) return false;
    *out = (parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3];
    return true;
}

std::string Ipv4ToString(u32 ip) {
    return std::to_string((ip >> 24) & 0xFFu) + "." + std::to_string((ip >> 16) & 0xFFu) + "." +
           std::to_string((ip >> 8) & 0xFFu) + "." + std::to_string(ip & 0xFFu);
}

// --- Разбор HTTP URL (http:// только в нативных сборках) --------------------
[[maybe_unused]] bool ParseHttpUrl(const std::string& url, std::string* scheme, std::string* host, u16* port,
                  std::string* path) {
    const usize sep = url.find("://");
    if (sep == std::string::npos) return false;
    *scheme = ToLowerAscii(url.substr(0, sep));
    std::string rest = url.substr(sep + 3);
    const usize slash = rest.find('/');
    std::string hostPort = (slash == std::string::npos) ? rest : rest.substr(0, slash);
    *path = (slash == std::string::npos) ? std::string("/") : rest.substr(slash);
    if (hostPort.empty()) return false;
    *port = (*scheme == "https") ? static_cast<u16>(443) : static_cast<u16>(80);
    const usize colon = hostPort.rfind(':');
    if (colon != std::string::npos) {
        *host = hostPort.substr(0, colon);
        const int p = std::atoi(hostPort.substr(colon + 1).c_str());
        if (p > 0 && p < 65536) *port = static_cast<u16>(p);
    } else {
        *host = hostPort;
    }
    return !host->empty();
}

#if !defined(ENG_PLATFORM_WASM)
// ---------------------------------------------------------------------------
// Помощники BSD/Winsock (только нативные сборки)
// ---------------------------------------------------------------------------
std::string SocketErrorString(int e) {
#if defined(ENG_PLATFORM_WINDOWS)
    return "winsock error " + std::to_string(e);
#else
    const char* s = std::strerror(e);
    return std::string(s ? s : "unknown error") + " (" + std::to_string(e) + ")";
#endif
}

bool IsWouldBlock(int e) {
#if defined(ENG_PLATFORM_WINDOWS)
    return e == WSAEWOULDBLOCK;
#else
    return e == EAGAIN || e == EWOULDBLOCK;
#endif
}

bool IsInProgress(int e) {
#if defined(ENG_PLATFORM_WINDOWS)
    return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS;
#else
    return e == EINPROGRESS || e == EALREADY;
#endif
}

int CloseSocketHandle(EngSocket s) {
    if (s == kEngInvalidSocket) return 0;
#if defined(ENG_PLATFORM_WINDOWS)
    return ::closesocket(s);
#else
    return ::close(s);
#endif
}

int SendFlags() {
#if defined(MSG_NOSIGNAL)
    return MSG_NOSIGNAL;
#else
    return 0;
#endif
}

void ApplyNoSigPipe(EngSocket s) {
#if defined(SO_NOSIGPIPE)
    int one = 1;
    ::setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, reinterpret_cast<const char*>(&one), sizeof(one));
#else
    (void)s;
#endif
}

bool SetNonBlockingFd(EngSocket s, bool on) {
#if defined(ENG_PLATFORM_WINDOWS)
    u_long mode = on ? 1u : 0u;
    return ::ioctlsocket(s, FIONBIO, &mode) == 0;
#else
    int flags = ::fcntl(s, F_GETFL, 0);
    if (flags < 0) return false;
    if (on) {
        flags |= O_NONBLOCK;
    } else {
        flags &= ~O_NONBLOCK;
    }
    return ::fcntl(s, F_SETFL, flags) == 0;
#endif
}

bool SetBoolOpt(EngSocket s, int level, int opt, bool on) {
    int one = on ? 1 : 0;
    return ::setsockopt(s, level, opt, reinterpret_cast<const char*>(&one), sizeof(one)) == 0;
}

int WaitFd(EngSocket s, short events, int timeoutMs) {
    if (s == kEngInvalidSocket) return -1;
    if (timeoutMs < 0) timeoutMs = 0;
#if defined(ENG_PLATFORM_WINDOWS)
    WSAPOLLFD p;
    p.fd = s;
    p.events = events;
    p.revents = 0;
    const int r = ::WSAPoll(&p, 1, timeoutMs);
#else
    pollfd p;
    p.fd = s;
    p.events = events;
    p.revents = 0;
    const int r = ::poll(&p, 1, timeoutMs);
#endif
    if (r <= 0) return r;
    return 1;
}

sockaddr_in ToSockaddr(const NetAddress& a) {
    sockaddr_in s{};
    s.sin_family = AF_INET;
    s.sin_port = htons(a.port);
    s.sin_addr.s_addr = htonl(a.ip);
    return s;
}

NetAddress FromSockaddr(const sockaddr_in& s) {
    NetAddress a;
    a.ip = ntohl(s.sin_addr.s_addr);
    a.port = ntohs(s.sin_port);
    return a;
}

bool SendAllFd(EngSocket s, const void* data, usize size, int timeoutMs, std::string* err) {
    const u8* p = static_cast<const u8*>(data);
    usize sent = 0;
    const f64 deadline = NowSeconds() + static_cast<f64>(timeoutMs) / 1000.0;
    while (sent < size) {
        const usize chunk = std::min<usize>(size - sent, 1u << 20);
#if defined(ENG_PLATFORM_WINDOWS)
        const int n = ::send(s, reinterpret_cast<const char*>(p + sent), static_cast<int>(chunk),
                             SendFlags());
#else
        const EngSsize n = ::send(s, p + sent, chunk, SendFlags());
#endif
        if (n > 0) {
            sent += static_cast<usize>(n);
            continue;
        }
        if (n == 0) {
            if (err) *err = "send() wrote 0 bytes";
            return false;
        }
        const int e = ENG_SOCK_ERR();
        if (!IsWouldBlock(e)) {
            if (err) *err = "send(): " + SocketErrorString(e);
            return false;
        }
        const f64 remaining = deadline - NowSeconds();
        if (remaining <= 0.0) {
            if (err) *err = "send() timed out";
            return false;
        }
        const int pr = WaitFd(s, POLLOUT, static_cast<int>(remaining * 1000.0) + 1);
        if (pr < 0) {
            if (err) *err = "send(): poll failed";
            return false;
        }
    }
    return true;
}

EngSocket ConnectTcpFd(const std::string& host, u16 port, f32 timeoutSeconds, std::string* err) {
    NetAddress addr = NetAddress::Parse(host, port);
    if (addr.ip == 0) {
        if (err) *err = "could not resolve host '" + host + "'";
        return kEngInvalidSocket;
    }
    EngSocket s = ::socket(AF_INET, SOCK_STREAM, 0);
    if (s == kEngInvalidSocket) {
        if (err) *err = "socket(): " + SocketErrorString(ENG_SOCK_ERR());
        return kEngInvalidSocket;
    }
    ApplyNoSigPipe(s);
    SetNonBlockingFd(s, true);
    const sockaddr_in a = ToSockaddr(addr);
    const int rc = ::connect(s, reinterpret_cast<const sockaddr*>(&a), sizeof(a));
    if (rc != 0) {
        const int e = ENG_SOCK_ERR();
        if (!IsInProgress(e)) {
            if (err) *err = "connect(): " + SocketErrorString(e);
            CloseSocketHandle(s);
            return kEngInvalidSocket;
        }
        f32 timeout = timeoutSeconds;
        if (timeout <= 0.0f) timeout = 5.0f;
        const int pr = WaitFd(s, POLLOUT, static_cast<int>(timeout * 1000.0f));
        if (pr == 0) {
            if (err) *err = "connect() timed out after " + std::to_string(timeout) + "s";
            CloseSocketHandle(s);
            return kEngInvalidSocket;
        }
        if (pr < 0) {
            if (err) *err = "connect(): poll failed";
            CloseSocketHandle(s);
            return kEngInvalidSocket;
        }
        int soErr = 0;
        socklen_t len = sizeof(soErr);
        if (::getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&soErr), &len) != 0) {
            if (err) *err = "connect(): getsockopt failed";
            CloseSocketHandle(s);
            return kEngInvalidSocket;
        }
        if (soErr != 0) {
            if (err) *err = "connect(): " + SocketErrorString(soErr);
            CloseSocketHandle(s);
            return kEngInvalidSocket;
        }
    }
    return s;
}

bool GetSockAddr(EngSocket s, bool peer, NetAddress* out) {
    if (!out) return false;
    sockaddr_in a{};
    socklen_t len = sizeof(a);
    const int rc = peer ? ::getpeername(s, reinterpret_cast<sockaddr*>(&a), &len)
                        : ::getsockname(s, reinterpret_cast<sockaddr*>(&a), &len);
    if (rc != 0) return false;
    *out = FromSockaddr(a);
    return true;
}
#endif  // !ENG_PLATFORM_WASM

[[maybe_unused]] void Unsupported(std::string* err, const char* what) {
    const std::string msg =
        std::string(what) + ": native sockets are unsupported on the WASM build";
    if (err) *err = msg;
    ENG_LOGW("net", "%s", msg.c_str());
}

}  // namespace

// ===========================================================================
// net_internal: SHA-1 / Base64 / WebSocket framing
// ===========================================================================
namespace net_internal {

namespace {

inline u32 Rotl32(u32 v, u32 b) { return (v << b) | (v >> (32u - b)); }

void Sha1Block(u32 h[5], const u8* p) {
    u32 w[80];
    for (int i = 0; i < 16; ++i) {
        w[i] = (static_cast<u32>(p[i * 4]) << 24) | (static_cast<u32>(p[i * 4 + 1]) << 16) |
               (static_cast<u32>(p[i * 4 + 2]) << 8) | static_cast<u32>(p[i * 4 + 3]);
    }
    for (int i = 16; i < 80; ++i) {
        w[i] = Rotl32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }
    u32 a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; ++i) {
        u32 f = 0, k = 0;
        if (i < 20) {
            f = (b & c) | ((~b) & d);
            k = 0x5A827999u;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1u;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDCu;
        } else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6u;
        }
        const u32 tmp = Rotl32(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = Rotl32(b, 30);
        b = a;
        a = tmp;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
}

const char kB64Alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

const i8* B64DecodeTable() {
    static i8 table[256];
    static const bool once = []() {
        std::memset(table, -1, sizeof(table));
        for (int i = 0; i < 64; ++i) {
            table[static_cast<u8>(kB64Alphabet[i])] = static_cast<i8>(i);
        }
        return true;
    }();
    (void)once;
    return table;
}

}  // namespace

void Sha1(const void* data, usize size, u8 out[20]) {
    u32 h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
    const u8* p = static_cast<const u8*>(data);
    const usize fullBlocks = size / 64;
    for (usize i = 0; i < fullBlocks; ++i) {
        Sha1Block(h, p + i * 64);
    }
    u8 tail[128];
    std::memset(tail, 0, sizeof(tail));
    const usize rem = size - fullBlocks * 64;
    if (rem > 0 && p != nullptr) {
        std::memcpy(tail, p + fullBlocks * 64, rem);
    }
    tail[rem] = 0x80;
    const usize padded = (rem < 56) ? 64 : 128;
    const u64 bits = static_cast<u64>(size) * 8u;
    for (int i = 0; i < 8; ++i) {
        tail[padded - 1 - static_cast<usize>(i)] = static_cast<u8>(bits >> (8 * i));
    }
    Sha1Block(h, tail);
    if (padded == 128) {
        Sha1Block(h, tail + 64);
    }
    for (int i = 0; i < 5; ++i) {
        out[i * 4 + 0] = static_cast<u8>(h[i] >> 24);
        out[i * 4 + 1] = static_cast<u8>(h[i] >> 16);
        out[i * 4 + 2] = static_cast<u8>(h[i] >> 8);
        out[i * 4 + 3] = static_cast<u8>(h[i]);
    }
}

std::string Sha1Hex(const void* data, usize size) {
    u8 digest[20];
    Sha1(data, size, digest);
    static const char* kHex = "0123456789abcdef";
    std::string out;
    out.reserve(40);
    for (int i = 0; i < 20; ++i) {
        out.push_back(kHex[(digest[i] >> 4) & 0x0F]);
        out.push_back(kHex[digest[i] & 0x0F]);
    }
    return out;
}

std::string Base64Encode(const void* data, usize size) {
    const u8* p = static_cast<const u8*>(data);
    std::string out;
    out.reserve(((size + 2) / 3) * 4);
    usize i = 0;
    for (; i + 3 <= size; i += 3) {
        const u32 v = (static_cast<u32>(p[i]) << 16) | (static_cast<u32>(p[i + 1]) << 8) |
                      static_cast<u32>(p[i + 2]);
        out.push_back(kB64Alphabet[(v >> 18) & 0x3F]);
        out.push_back(kB64Alphabet[(v >> 12) & 0x3F]);
        out.push_back(kB64Alphabet[(v >> 6) & 0x3F]);
        out.push_back(kB64Alphabet[v & 0x3F]);
    }
    const usize rem = size - i;
    if (rem == 1) {
        const u32 v = static_cast<u32>(p[i]) << 16;
        out.push_back(kB64Alphabet[(v >> 18) & 0x3F]);
        out.push_back(kB64Alphabet[(v >> 12) & 0x3F]);
        out.push_back('=');
        out.push_back('=');
    } else if (rem == 2) {
        const u32 v = (static_cast<u32>(p[i]) << 16) | (static_cast<u32>(p[i + 1]) << 8);
        out.push_back(kB64Alphabet[(v >> 18) & 0x3F]);
        out.push_back(kB64Alphabet[(v >> 12) & 0x3F]);
        out.push_back(kB64Alphabet[(v >> 6) & 0x3F]);
        out.push_back('=');
    }
    return out;
}

bool Base64Decode(const std::string& text, std::vector<u8>* out) {
    if (out) out->clear();
    const i8* table = B64DecodeTable();
    u32 acc = 0;
    int bits = 0;
    for (const char c : text) {
        if (c == '=') break;
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
        const i8 v = table[static_cast<u8>(c)];
        if (v < 0) return false;
        acc = ((acc << 6) | static_cast<u32>(v)) & 0xFFFFFFu;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (out) out->push_back(static_cast<u8>((acc >> bits) & 0xFFu));
        }
    }
    return true;
}

std::string WebSocketAcceptKey(const std::string& clientKey) {
    const std::string material = clientKey + kWebSocketGuid;
    u8 digest[20];
    Sha1(material.data(), material.size(), digest);
    return Base64Encode(digest, sizeof(digest));
}

std::string GenerateWebSocketKey() {
    static std::mt19937 rng(static_cast<u32>(
        std::chrono::steady_clock::now().time_since_epoch().count() ^ 0x9E3779B9u));
    u8 key[16];
    for (u8& b : key) {
        b = static_cast<u8>(rng() & 0xFFu);
    }
    return Base64Encode(key, sizeof(key));
}

const char* const kWsTestClientKey = "dGhlIHNhbXBsZSBub25jZQ==";
const char* const kWsTestAccept = "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=";

std::vector<u8> WsBuildFrame(u8 opcode, const void* payload, usize size, bool fin, bool mask,
                             u32 maskKey) {
    const u8* p = static_cast<const u8*>(payload);
    std::vector<u8> out;
    out.reserve(size + 14);
    out.push_back(static_cast<u8>((fin ? 0x80u : 0x00u) | (opcode & 0x0Fu)));
    const u8 maskBit = mask ? 0x80u : 0x00u;
    if (size < 126) {
        out.push_back(static_cast<u8>(maskBit | static_cast<u8>(size)));
    } else if (size <= 0xFFFF) {
        out.push_back(static_cast<u8>(maskBit | 126u));
        out.push_back(static_cast<u8>((size >> 8) & 0xFFu));
        out.push_back(static_cast<u8>(size & 0xFFu));
    } else {
        out.push_back(static_cast<u8>(maskBit | 127u));
        for (int i = 7; i >= 0; --i) {
            out.push_back(static_cast<u8>((static_cast<u64>(size) >> (8 * i)) & 0xFFu));
        }
    }
    u8 maskBytes[4] = {0, 0, 0, 0};
    if (mask) {
        maskBytes[0] = static_cast<u8>((maskKey >> 24) & 0xFFu);
        maskBytes[1] = static_cast<u8>((maskKey >> 16) & 0xFFu);
        maskBytes[2] = static_cast<u8>((maskKey >> 8) & 0xFFu);
        maskBytes[3] = static_cast<u8>(maskKey & 0xFFu);
        for (int i = 0; i < 4; ++i) out.push_back(maskBytes[i]);
    }
    const usize start = out.size();
    out.resize(start + size);
    for (usize i = 0; i < size; ++i) {
        const u8 v = (p != nullptr) ? p[i] : 0u;
        out[start + i] = mask ? static_cast<u8>(v ^ maskBytes[i % 4]) : v;
    }
    return out;
}

int WsParseFrame(const u8* data, usize size, usize* consumed, WsFrame* out) {
    if (consumed) *consumed = 0;
    if (data == nullptr || size < 2) return 0;

    const u8 b0 = data[0];
    const u8 b1 = data[1];
    const bool fin = (b0 & 0x80u) != 0;
    if ((b0 & 0x70u) != 0) return -1;  // биты RSV должны быть сброшены (расширений нет)
    const u8 opcode = static_cast<u8>(b0 & 0x0Fu);
    if (opcode != kWsContinuation && opcode != kWsText && opcode != kWsBinary &&
        opcode != kWsClose && opcode != kWsPing && opcode != kWsPong) {
        return -1;
    }

    const bool masked = (b1 & 0x80u) != 0;
    u64 len = b1 & 0x7Fu;
    usize pos = 2;
    if (len == 126) {
        if (size < pos + 2) return 0;
        len = (static_cast<u64>(data[pos]) << 8) | static_cast<u64>(data[pos + 1]);
        pos += 2;
    } else if (len == 127) {
        if (size < pos + 8) return 0;
        len = 0;
        for (int i = 0; i < 8; ++i) {
            len = (len << 8) | static_cast<u64>(data[pos + static_cast<usize>(i)]);
        }
        pos += 8;
        if (len > (1ull << 31)) return -1;  // абсурдные кадры отклоняем
    }

    const bool control = (opcode & 0x08u) != 0;
    if (control && (!fin || len > 125)) return -1;

    u8 maskBytes[4] = {0, 0, 0, 0};
    if (masked) {
        if (size < pos + 4) return 0;
        for (usize i = 0; i < 4; ++i) maskBytes[i] = data[pos + i];
        pos += 4;
    }
    if (len > static_cast<u64>(size - pos)) return 0;

    if (out) {
        out->fin = fin;
        out->opcode = opcode;
        out->masked = masked;
        out->payload.resize(static_cast<usize>(len));
        for (usize i = 0; i < static_cast<usize>(len); ++i) {
            const u8 v = data[pos + i];
            out->payload[i] = masked ? static_cast<u8>(v ^ maskBytes[i % 4]) : v;
        }
    }
    if (consumed) *consumed = pos + static_cast<usize>(len);
    return 1;
}

}  // namespace net_internal

// ===========================================================================
// NetAddress
// ===========================================================================
std::string NetAddress::ToString() const {
    return Ipv4ToString(ip) + ":" + std::to_string(port);
}

NetAddress NetAddress::Parse(const std::string& host, u16 port) {
    NetAddress a;
    a.port = port;
    if (host.empty() || host == "*") {
        a.ip = 0;
        return a;
    }
    u32 ip = 0;
    if (ParseIPv4Literal(host, &ip)) {
        a.ip = ip;
        return a;
    }
    const std::string resolved = NetResolveHost(host);
    if (!resolved.empty() && ParseIPv4Literal(resolved, &ip)) {
        a.ip = ip;
    }
    return a;
}

NetAddress NetAddress::Local(u16 port) {
    NetAddress a;
    a.ip = 0x7F000001u;  // 127.0.0.1 — локальный хост
    a.port = port;
    return a;
}

// ===========================================================================
// TcpSocket
// ===========================================================================
struct TcpSocket::Impl {
    EngSocket fd = kEngInvalidSocket;
    SocketState state = SocketState::Closed;
    NetAddress remote;
    NetAddress local;
    bool listening = false;
    std::vector<u8> msgBuf;   // накопление для ReceiveMessage()
    std::vector<u8> scratch;  // промежуточный буфер Receive()
};

TcpSocket::TcpSocket() : impl_(std::make_unique<Impl>()) {}

TcpSocket::~TcpSocket() { Close(); }

TcpSocket::TcpSocket(TcpSocket&& o) noexcept
    : impl_(std::move(o.impl_)), stats_(o.stats_), error_(std::move(o.error_)) {
    if (!impl_) impl_ = std::make_unique<Impl>();
    o.stats_ = NetStats{};
    o.error_.clear();
}

TcpSocket& TcpSocket::operator=(TcpSocket&& o) noexcept {
    if (this != &o) {
        Close();
        impl_ = std::move(o.impl_);
        stats_ = o.stats_;
        error_ = std::move(o.error_);
        if (!impl_) impl_ = std::make_unique<Impl>();
        o.stats_ = NetStats{};
        o.error_.clear();
    }
    return *this;
}

bool TcpSocket::Listen(u16 port, int backlog) {
    error_.clear();
#if defined(ENG_PLATFORM_WASM)
    (void)port;
    (void)backlog;
    Unsupported(&error_, "TcpSocket::Listen");
    if (impl_) impl_->state = SocketState::Error;
    return false;
#else
    Close();
    if (!impl_) impl_ = std::make_unique<Impl>();
    EngSocket s = ::socket(AF_INET, SOCK_STREAM, 0);
    if (s == kEngInvalidSocket) {
        error_ = "socket(): " + SocketErrorString(ENG_SOCK_ERR());
        ENG_LOGE("net", "TcpSocket::Listen %s", error_.c_str());
        impl_->state = SocketState::Error;
        return false;
    }
    ApplyNoSigPipe(s);
    SetBoolOpt(s, SOL_SOCKET, SO_REUSEADDR, true);
    sockaddr_in bindAddr{};
    bindAddr.sin_family = AF_INET;
    bindAddr.sin_addr.s_addr = htonl(INADDR_ANY);  // принимаем на всех интерфейсах
    bindAddr.sin_port = htons(port);
    if (::bind(s, reinterpret_cast<const sockaddr*>(&bindAddr), sizeof(bindAddr)) != 0) {
        error_ = "bind(" + std::to_string(port) + "): " + SocketErrorString(ENG_SOCK_ERR());
        ENG_LOGE("net", "TcpSocket::Listen %s", error_.c_str());
        CloseSocketHandle(s);
        impl_->state = SocketState::Error;
        return false;
    }
    if (::listen(s, backlog > 0 ? backlog : 8) != 0) {
        error_ = "listen(): " + SocketErrorString(ENG_SOCK_ERR());
        ENG_LOGE("net", "TcpSocket::Listen %s", error_.c_str());
        CloseSocketHandle(s);
        impl_->state = SocketState::Error;
        return false;
    }
    SetNonBlockingFd(s, true);
    impl_->fd = s;
    impl_->listening = true;
    impl_->state = SocketState::Listening;
    GetSockAddr(s, false, &impl_->local);  // отражает реальный порт, когда port == 0
    ENG_LOGI("net", "TCP listening on %s", impl_->local.ToString().c_str());
    return true;
#endif
}

bool TcpSocket::Connect(const std::string& host, u16 port, f32 timeoutSeconds) {
    error_.clear();
#if defined(ENG_PLATFORM_WASM)
    (void)host;
    (void)port;
    (void)timeoutSeconds;
    Unsupported(&error_, "TcpSocket::Connect");
    if (impl_) impl_->state = SocketState::Error;
    return false;
#else
    Close();
    if (!impl_) impl_ = std::make_unique<Impl>();
    const f64 t0 = NowSeconds();
    std::string err;
    EngSocket s = ConnectTcpFd(host, port, timeoutSeconds, &err);
    if (s == kEngInvalidSocket) {
        error_ = err;
        ENG_LOGW("net", "TcpSocket::Connect %s:%u failed: %s", host.c_str(),
                 static_cast<unsigned>(port), error_.c_str());
        impl_->state = SocketState::Error;
        return false;
    }
    impl_->fd = s;
    impl_->listening = false;
    impl_->state = SocketState::Connected;
    SetBoolOpt(s, IPPROTO_TCP, TCP_NODELAY, true);
    GetSockAddr(s, true, &impl_->remote);
    GetSockAddr(s, false, &impl_->local);
    impl_->remote.port = impl_->remote.port ? impl_->remote.port : port;
    stats_.connectTimeMs = static_cast<u64>((NowSeconds() - t0) * 1000.0);
    ENG_LOGI("net", "TCP connected %s -> %s", impl_->local.ToString().c_str(),
             impl_->remote.ToString().c_str());
    return true;
#endif
}

std::unique_ptr<TcpSocket> TcpSocket::Accept() {
#if defined(ENG_PLATFORM_WASM)
    error_ = "TcpSocket::Accept: native sockets are unsupported on the WASM build";
    return nullptr;
#else
    if (!impl_ || impl_->fd == kEngInvalidSocket || !impl_->listening) return nullptr;
    if (WaitFd(impl_->fd, POLLIN, 0) <= 0) return nullptr;
    sockaddr_in a{};
    socklen_t len = sizeof(a);
    EngSocket c = ::accept(impl_->fd, reinterpret_cast<sockaddr*>(&a), &len);
    if (c == kEngInvalidSocket) {
        const int e = ENG_SOCK_ERR();
        if (!IsWouldBlock(e)) {
            error_ = "accept(): " + SocketErrorString(e);
            ENG_LOGW("net", "TcpSocket::Accept %s", error_.c_str());
        }
        return nullptr;
    }
    ApplyNoSigPipe(c);
    SetNonBlockingFd(c, true);
    SetBoolOpt(c, IPPROTO_TCP, TCP_NODELAY, true);
    auto out = std::unique_ptr<TcpSocket>(new TcpSocket());
    out->impl_->fd = c;
    out->impl_->listening = false;
    out->impl_->state = SocketState::Connected;
    out->impl_->remote = FromSockaddr(a);
    GetSockAddr(c, false, &out->impl_->local);
    ENG_LOGI("net", "TCP accepted %s", out->impl_->remote.ToString().c_str());
    return out;
#endif
}

bool TcpSocket::Send(const void* data, usize size) {
    if (!impl_ || impl_->fd == kEngInvalidSocket) {
        error_ = "TcpSocket::Send: socket is not open";
        return false;
    }
    if (size == 0) return true;
    if (data == nullptr) {
        error_ = "TcpSocket::Send: null buffer";
        return false;
    }
#if defined(ENG_PLATFORM_WASM)
    Unsupported(&error_, "TcpSocket::Send");
    return false;
#else
    std::string err;
    if (!SendAllFd(impl_->fd, data, size, 5000, &err)) {
        error_ = err;
        ENG_LOGW("net", "TcpSocket::Send %s", error_.c_str());
        return false;
    }
    stats_.bytesSent += size;
    stats_.packetsSent += 1;
    return true;
#endif
}

bool TcpSocket::Receive(std::vector<u8>* out, usize maxBytes) {
    if (out) out->clear();
    if (!impl_ || impl_->fd == kEngInvalidSocket) {
        error_ = "TcpSocket::Receive: socket is not open";
        return false;
    }
    if (maxBytes == 0) return false;
#if defined(ENG_PLATFORM_WASM)
    Unsupported(&error_, "TcpSocket::Receive");
    return false;
#else
    if (impl_->scratch.size() < maxBytes) impl_->scratch.resize(maxBytes);
    const EngSsize n = ::recv(impl_->fd, impl_->scratch.data(), static_cast<int>(maxBytes), 0);
    if (n > 0) {
        if (out) out->assign(impl_->scratch.begin(), impl_->scratch.begin() + n);
        stats_.bytesReceived += static_cast<u64>(n);
        stats_.packetsReceived += 1;
        return true;
    }
    if (n == 0) {
        impl_->state = SocketState::Closed;
        impl_->listening = false;
        error_ = "peer closed the connection";
        ENG_LOGI("net", "TCP peer closed the connection");
        return false;
    }
    const int e = ENG_SOCK_ERR();
    if (IsWouldBlock(e)) return false;
    error_ = "recv(): " + SocketErrorString(e);
    ENG_LOGW("net", "TcpSocket::Receive %s", error_.c_str());
    impl_->state = SocketState::Error;
    return false;
#endif
}

bool TcpSocket::SendMessage(const std::vector<u8>& payload) {
    u8 header[4];
    const u32 len = static_cast<u32>(payload.size() & 0xFFFFFFFFu);
    header[0] = static_cast<u8>(len & 0xFFu);
    header[1] = static_cast<u8>((len >> 8) & 0xFFu);
    header[2] = static_cast<u8>((len >> 16) & 0xFFu);
    header[3] = static_cast<u8>((len >> 24) & 0xFFu);
    if (!Send(header, sizeof(header))) return false;
    if (payload.empty()) return true;
    return Send(payload.data(), payload.size());
}

bool TcpSocket::ReceiveMessage(std::vector<u8>* payload) {
    if (payload) payload->clear();
    if (!impl_ || impl_->fd == kEngInvalidSocket) return false;
    constexpr u32 kMaxMessageBytes = 64u * 1024u * 1024u;

    // Выкачиваем всё ожидающее (никогда не блокирует: Receive возвращает false на EAGAIN).
    for (;;) {
        std::vector<u8> chunk;
        if (!Receive(&chunk, 64 * 1024)) break;
        impl_->msgBuf.insert(impl_->msgBuf.end(), chunk.begin(), chunk.end());
        if (chunk.size() < 64 * 1024) break;
    }
    if (impl_->msgBuf.size() < 4) return false;
    const u32 len = static_cast<u32>(impl_->msgBuf[0]) | (static_cast<u32>(impl_->msgBuf[1]) << 8) |
                    (static_cast<u32>(impl_->msgBuf[2]) << 16) |
                    (static_cast<u32>(impl_->msgBuf[3]) << 24);
    if (len > kMaxMessageBytes) {
        error_ = "ReceiveMessage: frame length " + std::to_string(len) + " exceeds the limit";
        ENG_LOGE("net", "%s", error_.c_str());
        Close();
        return false;
    }
    if (impl_->msgBuf.size() < 4 + static_cast<usize>(len)) return false;
    if (payload) {
        payload->assign(impl_->msgBuf.begin() + 4, impl_->msgBuf.begin() + 4 + len);
    }
    impl_->msgBuf.erase(impl_->msgBuf.begin(), impl_->msgBuf.begin() + 4 + len);
    return true;
}

void TcpSocket::Close() {
    if (!impl_) return;
    if (impl_->fd != kEngInvalidSocket) {
#if !defined(ENG_PLATFORM_WASM)
        CloseSocketHandle(impl_->fd);
#endif
        impl_->fd = kEngInvalidSocket;
    }
    impl_->listening = false;
    impl_->msgBuf.clear();
    if (impl_->state != SocketState::Error) impl_->state = SocketState::Closed;
}

void TcpSocket::SetNoDelay(bool enable) {
    error_.clear();
#if defined(ENG_PLATFORM_WASM)
    Unsupported(&error_, "TcpSocket::SetNoDelay");
    (void)enable;
#else
    if (!impl_ || impl_->fd == kEngInvalidSocket) {
        error_ = "SetNoDelay: socket is not open";
        return;
    }
    if (!SetBoolOpt(impl_->fd, IPPROTO_TCP, TCP_NODELAY, enable)) {
        error_ = "setsockopt(TCP_NODELAY): " + SocketErrorString(ENG_SOCK_ERR());
    }
#endif
}

void TcpSocket::SetNonBlocking(bool enable) {
    error_.clear();
#if defined(ENG_PLATFORM_WASM)
    Unsupported(&error_, "TcpSocket::SetNonBlocking");
    (void)enable;
#else
    if (!impl_ || impl_->fd == kEngInvalidSocket) {
        error_ = "SetNonBlocking: socket is not open";
        return;
    }
    if (!SetNonBlockingFd(impl_->fd, enable)) {
        error_ = "fcntl(O_NONBLOCK): " + SocketErrorString(ENG_SOCK_ERR());
    }
#endif
}

void TcpSocket::SetKeepAlive(bool enable) {
    error_.clear();
#if defined(ENG_PLATFORM_WASM)
    Unsupported(&error_, "TcpSocket::SetKeepAlive");
    (void)enable;
#else
    if (!impl_ || impl_->fd == kEngInvalidSocket) {
        error_ = "SetKeepAlive: socket is not open";
        return;
    }
    if (!SetBoolOpt(impl_->fd, SOL_SOCKET, SO_KEEPALIVE, enable)) {
        error_ = "setsockopt(SO_KEEPALIVE): " + SocketErrorString(ENG_SOCK_ERR());
    }
#endif
}

bool TcpSocket::Valid() const {
    if (!impl_ || impl_->fd == kEngInvalidSocket) return false;
    return impl_->state != SocketState::Closed && impl_->state != SocketState::Error;
}

SocketState TcpSocket::State() const { return impl_ ? impl_->state : SocketState::Closed; }

NetAddress TcpSocket::RemoteAddress() const { return impl_ ? impl_->remote : NetAddress{}; }

NetAddress TcpSocket::LocalAddress() const { return impl_ ? impl_->local : NetAddress{}; }

// ===========================================================================
// UdpSocket
// ===========================================================================
struct UdpSocket::Impl {
    EngSocket fd = kEngInvalidSocket;
    NetAddress local;
    NetAddress remote;
    bool hasRemote = false;
    std::vector<u8> scratch;
};

UdpSocket::UdpSocket() : impl_(std::make_unique<Impl>()) {}

UdpSocket::~UdpSocket() { Close(); }

UdpSocket::UdpSocket(UdpSocket&& o) noexcept
    : impl_(std::move(o.impl_)), stats_(o.stats_), error_(std::move(o.error_)) {
    if (!impl_) impl_ = std::make_unique<Impl>();
    o.stats_ = NetStats{};
    o.error_.clear();
}

UdpSocket& UdpSocket::operator=(UdpSocket&& o) noexcept {
    if (this != &o) {
        Close();
        impl_ = std::move(o.impl_);
        stats_ = o.stats_;
        error_ = std::move(o.error_);
        if (!impl_) impl_ = std::make_unique<Impl>();
        o.stats_ = NetStats{};
        o.error_.clear();
    }
    return *this;
}

bool UdpSocket::Bind(u16 port) {
    error_.clear();
#if defined(ENG_PLATFORM_WASM)
    (void)port;
    Unsupported(&error_, "UdpSocket::Bind");
    return false;
#else
    Close();
    if (!impl_) impl_ = std::make_unique<Impl>();
    EngSocket s = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (s == kEngInvalidSocket) {
        error_ = "socket(): " + SocketErrorString(ENG_SOCK_ERR());
        ENG_LOGE("net", "UdpSocket::Bind %s", error_.c_str());
        return false;
    }
    ApplyNoSigPipe(s);
    SetBoolOpt(s, SOL_SOCKET, SO_REUSEADDR, true);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);  // принимаем на всех интерфейсах
    a.sin_port = htons(port);
    if (::bind(s, reinterpret_cast<const sockaddr*>(&a), sizeof(a)) != 0) {
        error_ = "bind(" + std::to_string(port) + "): " + SocketErrorString(ENG_SOCK_ERR());
        ENG_LOGE("net", "UdpSocket::Bind %s", error_.c_str());
        CloseSocketHandle(s);
        return false;
    }
    SetNonBlockingFd(s, true);
    impl_->fd = s;
    GetSockAddr(s, false, &impl_->local);
    ENG_LOGI("net", "UDP bound to %s", impl_->local.ToString().c_str());
    return true;
#endif
}

bool UdpSocket::Connect(const std::string& host, u16 port) {
    error_.clear();
#if defined(ENG_PLATFORM_WASM)
    (void)host;
    (void)port;
    Unsupported(&error_, "UdpSocket::Connect");
    return false;
#else
    if (!impl_ || impl_->fd == kEngInvalidSocket) {
        if (!Bind(0)) return false;
    }
    const NetAddress addr = NetAddress::Parse(host, port);
    if (addr.ip == 0) {
        error_ = "could not resolve host '" + host + "'";
        return false;
    }
    const sockaddr_in a = ToSockaddr(addr);
    if (::connect(impl_->fd, reinterpret_cast<const sockaddr*>(&a), sizeof(a)) != 0) {
        error_ = "connect(): " + SocketErrorString(ENG_SOCK_ERR());
        ENG_LOGW("net", "UdpSocket::Connect %s", error_.c_str());
        return false;
    }
    impl_->remote = addr;
    impl_->hasRemote = true;
    return true;
#endif
}

bool UdpSocket::SendTo(const NetAddress& addr, const void* data, usize size) {
    if (!impl_ || impl_->fd == kEngInvalidSocket) {
        error_ = "UdpSocket::SendTo: socket is not open";
        return false;
    }
    if (data == nullptr && size > 0) {
        error_ = "UdpSocket::SendTo: null buffer";
        return false;
    }
#if defined(ENG_PLATFORM_WASM)
    (void)addr;
    (void)data;
    (void)size;
    Unsupported(&error_, "UdpSocket::SendTo");
    return false;
#else
    const sockaddr_in a = ToSockaddr(addr);
    const EngSsize n = ::sendto(impl_->fd, data, size, SendFlags(),
                               reinterpret_cast<const sockaddr*>(&a), sizeof(a));
    if (n < 0 || static_cast<usize>(n) != size) {
        const int e = ENG_SOCK_ERR();
        if (IsWouldBlock(e)) {
            stats_.packetsLost += 1;
            return false;
        }
        error_ = "sendto(): " + SocketErrorString(e);
        ENG_LOGW("net", "UdpSocket::SendTo %s", error_.c_str());
        return false;
    }
    stats_.bytesSent += size;
    stats_.packetsSent += 1;
    return true;
#endif
}

bool UdpSocket::Send(const void* data, usize size) {
    if (!impl_ || impl_->fd == kEngInvalidSocket) {
        error_ = "UdpSocket::Send: socket is not open";
        return false;
    }
    if (!impl_->hasRemote) {
        error_ = "UdpSocket::Send: no default peer (call Connect first)";
        return false;
    }
    if (data == nullptr && size > 0) {
        error_ = "UdpSocket::Send: null buffer";
        return false;
    }
#if defined(ENG_PLATFORM_WASM)
    Unsupported(&error_, "UdpSocket::Send");
    return false;
#else
    // Сокет подключён: используем send() (sendto() с адресом вернёт
    // EISCONN на подключённом датаграммном сокете).
    const EngSsize n = ::send(impl_->fd, data, size, SendFlags());
    if (n < 0 || static_cast<usize>(n) != size) {
        const int e = ENG_SOCK_ERR();
        if (IsWouldBlock(e)) {
            stats_.packetsLost += 1;
            return false;
        }
        error_ = "send(): " + SocketErrorString(e);
        ENG_LOGW("net", "UdpSocket::Send %s", error_.c_str());
        return false;
    }
    stats_.bytesSent += size;
    stats_.packetsSent += 1;
    return true;
#endif
}

bool UdpSocket::ReceiveFrom(NetPacket* packet) {
    if (packet) {
        packet->data.clear();
        packet->sender = NetAddress{};
        packet->receiveTime = 0;
        packet->channel = 0;
        packet->reliable = false;
    }
    if (!impl_ || impl_->fd == kEngInvalidSocket) {
        error_ = "UdpSocket::ReceiveFrom: socket is not open";
        return false;
    }
#if defined(ENG_PLATFORM_WASM)
    Unsupported(&error_, "UdpSocket::ReceiveFrom");
    return false;
#else
    constexpr usize kMaxDatagram = 65536;
    if (impl_->scratch.size() < kMaxDatagram) impl_->scratch.resize(kMaxDatagram);
    sockaddr_in from{};
    socklen_t len = sizeof(from);
    const EngSsize n = ::recvfrom(impl_->fd, impl_->scratch.data(), static_cast<int>(impl_->scratch.size()),
                                 0, reinterpret_cast<sockaddr*>(&from), &len);
    if (n < 0) {
        const int e = ENG_SOCK_ERR();
        if (IsWouldBlock(e)) return false;
        error_ = "recvfrom(): " + SocketErrorString(e);
        ENG_LOGW("net", "UdpSocket::ReceiveFrom %s", error_.c_str());
        return false;
    }
    if (packet) {
        packet->data.assign(impl_->scratch.begin(), impl_->scratch.begin() + n);
        packet->sender = FromSockaddr(from);
        packet->receiveTime = NowSeconds();
    }
    stats_.bytesReceived += static_cast<u64>(n);
    stats_.packetsReceived += 1;
    return true;
#endif
}

void UdpSocket::Close() {
    if (!impl_) return;
    if (impl_->fd != kEngInvalidSocket) {
#if !defined(ENG_PLATFORM_WASM)
        CloseSocketHandle(impl_->fd);
#endif
        impl_->fd = kEngInvalidSocket;
    }
    impl_->hasRemote = false;
    impl_->remote = NetAddress{};
}

void UdpSocket::SetBroadcast(bool enable) {
    error_.clear();
#if defined(ENG_PLATFORM_WASM)
    (void)enable;
    Unsupported(&error_, "UdpSocket::SetBroadcast");
#else
    if (!impl_ || impl_->fd == kEngInvalidSocket) {
        error_ = "SetBroadcast: socket is not open";
        return;
    }
    if (!SetBoolOpt(impl_->fd, SOL_SOCKET, SO_BROADCAST, enable)) {
        error_ = "setsockopt(SO_BROADCAST): " + SocketErrorString(ENG_SOCK_ERR());
    }
#endif
}

void UdpSocket::SetNonBlocking(bool enable) {
    error_.clear();
#if defined(ENG_PLATFORM_WASM)
    (void)enable;
    Unsupported(&error_, "UdpSocket::SetNonBlocking");
#else
    if (!impl_ || impl_->fd == kEngInvalidSocket) {
        error_ = "SetNonBlocking: socket is not open";
        return;
    }
    if (!SetNonBlockingFd(impl_->fd, enable)) {
        error_ = "fcntl(O_NONBLOCK): " + SocketErrorString(ENG_SOCK_ERR());
    }
#endif
}

bool UdpSocket::Valid() const { return impl_ && impl_->fd != kEngInvalidSocket; }

NetAddress UdpSocket::LocalAddress() const { return impl_ ? impl_->local : NetAddress{}; }

// ===========================================================================
// WebSocket - очередь событий WASM JS (определена на всех платформах; используется только на WASM)
// ===========================================================================
namespace {

struct WasmWsEvent {
    enum class Kind { Open, Message, Close, Error };
    Kind kind = Kind::Open;
    std::vector<u8> data;
    bool binary = false;
    int code = 0;
    std::string text;
};

std::map<int, std::vector<WasmWsEvent>>& WsEventQueues() {
    static std::map<int, std::vector<WasmWsEvent>> queues;
    return queues;
}

void WsPushEvent(int id, WasmWsEvent ev) { WsEventQueues()[id].push_back(std::move(ev)); }

}  // namespace

ENG_EXPORT_JS void eng_ws_on_open(int id) {
    WasmWsEvent ev;
    ev.kind = WasmWsEvent::Kind::Open;
    WsPushEvent(id, std::move(ev));
}

ENG_EXPORT_JS void eng_ws_on_message(int id, const void* data, int len, int isBinary) {
    WasmWsEvent ev;
    ev.kind = WasmWsEvent::Kind::Message;
    ev.binary = isBinary != 0;
    if (data != nullptr && len > 0) {
        const u8* p = static_cast<const u8*>(data);
        ev.data.assign(p, p + len);
    }
    WsPushEvent(id, std::move(ev));
}

ENG_EXPORT_JS void eng_ws_on_close(int id, int code, const char* reason) {
    WasmWsEvent ev;
    ev.kind = WasmWsEvent::Kind::Close;
    ev.code = code;
    if (reason != nullptr) ev.text = reason;
    WsPushEvent(id, std::move(ev));
}

ENG_EXPORT_JS void eng_ws_on_error(int id, const char* msg) {
    WasmWsEvent ev;
    ev.kind = WasmWsEvent::Kind::Error;
    if (msg != nullptr) ev.text = msg;
    WsPushEvent(id, std::move(ev));
}

// ===========================================================================
// WebSocket
// ===========================================================================
struct WebSocket::Impl {
    WebSocket* self = nullptr;
    WsCallbacks cb;

    EngSocket fd = kEngInvalidSocket;  // нативный транспорт
    int jsId = -1;                     // транспорт WASM

    bool handshakeSent = false;
    bool handshakeDone = false;
    bool synthetic = false;  // InjectForTest() провёл рукопожатие (без сокета)
    std::string clientKey;

    std::vector<u8> raw;        // полученные, но ещё не разобранные на кадры байты
    std::vector<u8> fragData;   // накопление фрагментированного сообщения
    u8 fragOpcode = 0;
    bool closeSent = false;
    bool closeNotified = false;
    bool peerEof = false;  // пир закрыл TCP-поток (кадры могут ещё быть в буфере)
    std::vector<u8> scratch;

    void NotifyClose(u16 code, const std::string& reason) {
        if (closeNotified) return;
        closeNotified = true;
        if (cb.onClose) cb.onClose(code, reason);
    }

    void Fail(const std::string& msg) {
        self->error_ = msg;
        self->state_ = WsState::Error;
        ENG_LOGE("net", "WebSocket: %s", msg.c_str());
        if (cb.onError) cb.onError(msg);
        ShutdownSocket();
    }

    void ShutdownSocket() {
#if defined(ENG_PLATFORM_WASM)
        if (jsId >= 0) {
            eng_js_ws_close(jsId, 1000, "");
            WsEventQueues().erase(jsId);
            jsId = -1;
        }
#else
        if (fd != kEngInvalidSocket) {
            CloseSocketHandle(fd);
            fd = kEngInvalidSocket;
        }
#endif
    }

    bool SendRaw(const void* data, usize size) {
#if defined(ENG_PLATFORM_WASM)
        (void)data;
        (void)size;
        return false;
#else
        if (fd == kEngInvalidSocket) return false;
        std::string err;
        if (!SendAllFd(fd, data, size, 5000, &err)) {
            self->error_ = err;
            ENG_LOGW("net", "WebSocket send failed: %s", err.c_str());
            return false;
        }
        self->stats_.bytesSent += size;
        self->stats_.packetsSent += 1;
        return true;
#endif
    }

    void SendFrame(u8 opcode, const void* data, usize size) {
#if defined(ENG_PLATFORM_WASM)
        if (jsId < 0) return;
        if (opcode == net_internal::kWsText) {
            std::string text(static_cast<const char*>(data), size);
            eng_js_ws_send_text(jsId, text.c_str());
            self->stats_.bytesSent += size;
        } else if (opcode == net_internal::kWsBinary) {
            eng_js_ws_send_binary(jsId, data, static_cast<int>(size));
            self->stats_.bytesSent += size;
        } else if (opcode == net_internal::kWsClose) {
            eng_js_ws_close(jsId, 1000, "");
        }
        // Браузерный WebSocket API не может отправлять кадры ping/pong.
#else
        if (fd == kEngInvalidSocket) return;
        static std::mt19937 rng(static_cast<u32>(
            std::chrono::steady_clock::now().time_since_epoch().count() ^ 0x85EBCA6Bu));
        const u32 maskKey = rng();
        const std::vector<u8> frame =
            net_internal::WsBuildFrame(opcode, data, size, true, true, maskKey);
        SendRaw(frame.data(), frame.size());
#endif
    }

    void ReadSocket() {
#if !defined(ENG_PLATFORM_WASM)
        if (fd == kEngInvalidSocket) return;
        if (scratch.size() < 16384) scratch.resize(16384);
        for (;;) {
            const EngSsize n = ::recv(fd, scratch.data(), static_cast<int>(scratch.size()), 0);
            if (n > 0) {
                raw.insert(raw.end(), scratch.begin(), scratch.begin() + n);
                self->stats_.bytesReceived += static_cast<u64>(n);
                self->stats_.packetsReceived += 1;
                continue;
            }
            if (n == 0) {
                // Пока не сообщаем о закрытии: кадр close может уже лежать
                // в `raw` и должен быть обработан первым (иначе код/причина
                // статуса будут потеряны в пользу 1006).
                ENG_LOGI("net", "WebSocket: peer closed the connection");
                peerEof = true;
                ShutdownSocket();
                return;
            }
            const int e = ENG_SOCK_ERR();
            if (IsWouldBlock(e)) return;
            if (self->state_ == WsState::Closed) return;
            Fail("recv(): " + SocketErrorString(e));
            return;
        }
#endif
    }

    // Возвращает true, когда рукопожатие обработано (открыто или провалено).
    bool TryHandshake() {
        static const char kHeaderEnd[] = "\r\n\r\n";
        const char* begin = reinterpret_cast<const char*>(raw.data());
        const char* end = begin + raw.size();
        const char* found = std::search(begin, end, kHeaderEnd, kHeaderEnd + 4);
        if (found == end) {
            if (raw.size() > 64 * 1024) Fail("handshake response too large");
            return false;
        }
        const usize headerLen = static_cast<usize>(found - begin) + 4;
        const std::string header(begin, headerLen);
        raw.erase(raw.begin(), raw.begin() + static_cast<std::ptrdiff_t>(headerLen));

        // Строка статуса.
        const usize sp = header.find(' ');
        int status = 0;
        if (sp != std::string::npos) {
            usize i = sp + 1;
            while (i < header.size() && header[i] >= '0' && header[i] <= '9') {
                status = status * 10 + (header[i] - '0');
                ++i;
            }
        }
        // Заголовки.
        std::map<std::string, std::string> headers;
        usize lineStart = header.find("\r\n");
        if (lineStart == std::string::npos) lineStart = header.size();
        lineStart += 2;
        while (lineStart < header.size()) {
            const usize lineEnd = header.find("\r\n", lineStart);
            if (lineEnd == std::string::npos || lineEnd == lineStart) break;
            const std::string line = header.substr(lineStart, lineEnd - lineStart);
            const usize colon = line.find(':');
            if (colon != std::string::npos) {
                headers[ToLowerAscii(TrimAscii(line.substr(0, colon)))] = TrimAscii(line.substr(colon + 1));
            }
            lineStart = lineEnd + 2;
        }

        const std::string expect = net_internal::WebSocketAcceptKey(clientKey);
        const auto it = headers.find("sec-websocket-accept");
        const std::string got = (it != headers.end()) ? it->second : std::string();
        if (status != 101 || got != expect) {
            Fail("handshake rejected (status " + std::to_string(status) +
                 ", accept '" + got + "')");
            return true;
        }
        handshakeDone = true;
        self->error_.clear();
        const auto pit = headers.find("sec-websocket-protocol");
        if (pit != headers.end()) self->protocol_ = pit->second;
        self->state_ = WsState::Open;
        ENG_LOGI("net", "WebSocket open: %s", self->url_.c_str());
        if (cb.onOpen) cb.onOpen();
        return true;
    }

    void HandleFrame(const net_internal::WsFrame& f);

    void ProcessFrames() {
        for (;;) {
            usize consumed = 0;
            net_internal::WsFrame f;
            const int r = net_internal::WsParseFrame(raw.data(), raw.size(), &consumed, &f);
            if (r == 0) break;
            if (r < 0) {
                Fail("malformed frame");
                return;
            }
            raw.erase(raw.begin(), raw.begin() + static_cast<std::ptrdiff_t>(consumed));
            HandleFrame(f);
            if (self->state_ == WsState::Error || self->state_ == WsState::Closed) break;
        }
    }

#if defined(ENG_PLATFORM_WASM)
    void DrainWasmEvents() {
        if (jsId < 0) return;
        auto it = WsEventQueues().find(jsId);
        if (it == WsEventQueues().end()) return;
        std::vector<WasmWsEvent> events;
        events.swap(it->second);
        for (const WasmWsEvent& ev : events) {
            switch (ev.kind) {
                case WasmWsEvent::Kind::Open:
                    if (self->state_ == WsState::Connecting) {
                        self->state_ = WsState::Open;
                        if (cb.onOpen) cb.onOpen();
                    }
                    break;
                case WasmWsEvent::Kind::Message:
                    self->stats_.bytesReceived += ev.data.size();
                    if (ev.binary) {
                        if (cb.onBinary) cb.onBinary(ev.data);
                    } else {
                        if (cb.onText) {
                            cb.onText(std::string(reinterpret_cast<const char*>(ev.data.data()),
                                                  ev.data.size()));
                        }
                    }
                    break;
                case WasmWsEvent::Kind::Close: {
                    const u16 code = static_cast<u16>(ev.code > 0 ? ev.code : 1005);
                    self->state_ = WsState::Closed;
                    NotifyClose(code, ev.text);
                    ShutdownSocket();
                    break;
                }
                case WasmWsEvent::Kind::Error:
                    Fail(ev.text.empty() ? std::string("websocket error") : ev.text);
                    break;
            }
        }
    }
#endif
};

void WebSocket::Impl::HandleFrame(const net_internal::WsFrame& f) {
    using namespace net_internal;
    switch (f.opcode) {
        case kWsPing:
            SendFrame(kWsPong, f.payload.data(), f.payload.size());
            break;
        case kWsPong:
            break;
        case kWsClose: {
            u16 code = 1005;  // "no status received"
            std::string reason;
            if (f.payload.size() >= 2) {
                code = static_cast<u16>((static_cast<u16>(f.payload[0]) << 8) | f.payload[1]);
                if (f.payload.size() > 2) {
                    reason.assign(reinterpret_cast<const char*>(f.payload.data()) + 2,
                                  f.payload.size() - 2);
                }
            }
            if (!closeSent) {
                SendFrame(kWsClose, f.payload.data(), f.payload.size());
                closeSent = true;
            }
            self->state_ = WsState::Closing;
            NotifyClose(code, reason);
            ShutdownSocket();
            self->state_ = WsState::Closed;
            break;
        }
        case kWsText:
        case kWsBinary:
        case kWsContinuation:
        default: {
            if (f.opcode != kWsContinuation) {
                fragOpcode = f.opcode;
                fragData = f.payload;
            } else {
                fragData.insert(fragData.end(), f.payload.begin(), f.payload.end());
            }
            if (!f.fin) break;
            const u8 op = (fragOpcode != 0) ? fragOpcode : static_cast<u8>(kWsBinary);
            std::vector<u8> payload;
            payload.swap(fragData);
            fragOpcode = 0;
            self->stats_.bytesReceived += payload.size();
            if (op == kWsText) {
                if (cb.onText) {
                    cb.onText(std::string(reinterpret_cast<const char*>(payload.data()), payload.size()));
                }
            } else {
                if (cb.onBinary) cb.onBinary(payload);
            }
            break;
        }
    }
}

WebSocket::WebSocket() : impl_(std::make_unique<Impl>()) {
    impl_->self = this;
    if (impl_->raw.capacity() == 0) impl_->raw.reserve(4096);
}

WebSocket::~WebSocket() {
    if (impl_) impl_->ShutdownSocket();
}

bool WebSocket::Connect(const std::string& url, const WsCallbacks& callbacks,
                        const std::vector<std::string>& protocols) {
    if (!impl_) return false;
    error_.clear();
    stats_ = NetStats{};
    url_ = url;
    protocol_.clear();
    impl_->cb = callbacks;
    impl_->raw.clear();
    impl_->fragData.clear();
    impl_->fragOpcode = 0;
    impl_->closeSent = false;
    impl_->closeNotified = false;
    impl_->peerEof = false;
    impl_->handshakeDone = false;
    impl_->synthetic = false;

#if defined(ENG_PLATFORM_WASM)
    std::string csv;
    for (usize i = 0; i < protocols.size(); ++i) {
        if (i > 0) csv += ", ";
        csv += protocols[i];
    }
    const int id = eng_js_ws_create(url.c_str(), csv.empty() ? nullptr : csv.c_str());
    if (id < 0) {
        impl_->Fail("eng_js_ws_create() failed for '" + url + "'");
        return false;
    }
    impl_->jsId = id;
    state_ = WsState::Connecting;
    ENG_LOGI("net", "WebSocket connecting (browser): %s", url.c_str());
    return true;
#else
    const std::string lower = ToLowerAscii(url);
    if (lower.rfind("wss://", 0) == 0) {
        impl_->Fail(
            "wss:// is not supported by the native WebSocket client (no TLS backend): " + url);
        return false;
    }
    if (lower.rfind("ws://", 0) != 0) {
        impl_->Fail("invalid websocket url (expected ws:// or wss://): " + url);
        return false;
    }
    const std::string rest = url.substr(5);
    const usize slash = rest.find('/');
    const std::string hostPort = (slash == std::string::npos) ? rest : rest.substr(0, slash);
    std::string path = (slash == std::string::npos) ? std::string("/") : rest.substr(slash);
    if (path.empty()) path = "/";
    std::string host = hostPort;
    u16 port = 80;
    const usize colon = hostPort.rfind(':');
    if (colon != std::string::npos) {
        host = hostPort.substr(0, colon);
        const int p = std::atoi(hostPort.substr(colon + 1).c_str());
        if (p > 0 && p < 65536) port = static_cast<u16>(p);
    }
    if (host.empty()) {
        impl_->Fail("invalid websocket url (missing host): " + url);
        return false;
    }

    impl_->clientKey = net_internal::GenerateWebSocketKey();
    std::string err;
    impl_->fd = ConnectTcpFd(host, port, 5.0f, &err);
    if (impl_->fd == kEngInvalidSocket) {
        impl_->Fail("websocket connect to " + hostPort + " failed: " + err);
        return false;
    }
    SetNonBlockingFd(impl_->fd, true);

    std::string req = "GET " + path + " HTTP/1.1\r\n";
    req += "Host: " + hostPort + "\r\n";
    req += "Upgrade: websocket\r\n";
    req += "Connection: Upgrade\r\n";
    req += "Sec-WebSocket-Key: " + impl_->clientKey + "\r\n";
    req += "Sec-WebSocket-Version: 13\r\n";
    if (!protocols.empty()) {
        req += "Sec-WebSocket-Protocol: ";
        for (usize i = 0; i < protocols.size(); ++i) {
            if (i > 0) req += ", ";
            req += protocols[i];
        }
        req += "\r\n";
    }
    req += "\r\n";

    std::string sendErr;
    if (!SendAllFd(impl_->fd, req.data(), req.size(), 5000, &sendErr)) {
        impl_->Fail("failed to send websocket handshake: " + sendErr);
        return false;
    }
    stats_.bytesSent += req.size();
    stats_.packetsSent += 1;
    impl_->handshakeSent = true;
    state_ = WsState::Connecting;
    ENG_LOGI("net", "WebSocket connecting: %s", url.c_str());
    return true;
#endif
}

void WebSocket::Poll() {
    if (!impl_) return;
#if defined(ENG_PLATFORM_WASM)
    impl_->DrainWasmEvents();
    if (state_ == WsState::Connecting && impl_->jsId >= 0) {
        // Откат к состоянию JS на случай пропущенного события открытия.
        if (eng_js_ws_state(impl_->jsId) == 2) {
            state_ = WsState::Open;
            if (impl_->cb.onOpen) impl_->cb.onOpen();
        }
    }
    if (state_ == WsState::Connecting && impl_->synthetic) {
        impl_->TryHandshake();
    }
#else
    if (impl_->fd != kEngInvalidSocket && state_ != WsState::Closed &&
        state_ != WsState::Error) {
        impl_->ReadSocket();
    }
    if (state_ == WsState::Connecting) {
        if (!impl_->TryHandshake()) {
            if (impl_->peerEof) {
                impl_->Fail("connection closed during the websocket handshake");
            }
            return;
        }
    }
#endif
    if (state_ == WsState::Open) {
        impl_->ProcessFrames();
    }
    // Пир положил трубку без кадра close: сообщаем об аномальном закрытии только
    // после обработки всех буферизованных кадров.
    if (impl_->peerEof && state_ == WsState::Open) {
        state_ = WsState::Closed;
        impl_->NotifyClose(1006, "connection closed");
    }
}

bool WebSocket::SendText(const std::string& text) {
    if (!impl_) return false;
    if (state_ != WsState::Open) {
        error_ = "WebSocket::SendText: socket is not open";
        return false;
    }
    impl_->SendFrame(net_internal::kWsText, text.data(), text.size());
    return true;
}

bool WebSocket::SendBinary(const void* data, usize size) {
    if (!impl_) return false;
    if (state_ != WsState::Open) {
        error_ = "WebSocket::SendBinary: socket is not open";
        return false;
    }
    if (data == nullptr && size > 0) {
        error_ = "WebSocket::SendBinary: null buffer";
        return false;
    }
    impl_->SendFrame(net_internal::kWsBinary, data, size);
    return true;
}

bool WebSocket::SendPing() {
    if (!impl_) return false;
    if (state_ != WsState::Open) {
        error_ = "WebSocket::SendPing: socket is not open";
        return false;
    }
#if defined(ENG_PLATFORM_WASM)
    error_ = "WebSocket::SendPing: the browser WebSocket API cannot send ping frames";
    ENG_LOGW("net", "%s", error_.c_str());
    return false;
#else
    impl_->SendFrame(net_internal::kWsPing, nullptr, 0);
    return true;
#endif
}

void WebSocket::Close(u16 code, const std::string& reason) {
    if (!impl_) return;
    if (state_ == WsState::Open || state_ == WsState::Connecting) {
        std::vector<u8> payload;
        payload.push_back(static_cast<u8>((code >> 8) & 0xFFu));
        payload.push_back(static_cast<u8>(code & 0xFFu));
        payload.insert(payload.end(), reason.begin(), reason.end());
        impl_->SendFrame(net_internal::kWsClose, payload.data(), payload.size());
        impl_->closeSent = true;
    }
    state_ = WsState::Closing;
    impl_->ShutdownSocket();
    impl_->raw.clear();
    impl_->fragData.clear();
    impl_->fragOpcode = 0;
    state_ = WsState::Closed;
    impl_->NotifyClose(code, reason);
}

void WebSocket::InjectForTest(const void* data, usize size) {
    if (!impl_) return;
    if (data == nullptr || size == 0) return;
    if (state_ != WsState::Open && state_ != WsState::Connecting) {
        // Реального транспорта нет: проводим синтетическое рукопожатие с предсказуемым ключом.
        impl_->synthetic = true;
        impl_->clientKey = net_internal::kWsTestClientKey;
        impl_->handshakeDone = false;
        impl_->closeSent = false;
        impl_->closeNotified = false;
        impl_->peerEof = false;
        state_ = WsState::Connecting;
    }
    const u8* p = static_cast<const u8*>(data);
    impl_->raw.insert(impl_->raw.end(), p, p + size);
}

// ===========================================================================
// HttpGet
// ===========================================================================
#if !defined(ENG_PLATFORM_WASM)
namespace {

// Читает весь ответ (заголовки + тело), пока не встретит EOF, не выполнится
// Content-Length или не истечёт дедлайн. Никогда не блокируется бесконечно.
bool ReadHttpResponse(EngSocket fd, std::vector<u8>* raw, std::string* err) {
    static const char kHeaderEnd[] = "\r\n\r\n";
    const f64 deadline = NowSeconds() + 10.0;
    std::vector<u8> scratch(16384);
    bool headersSeen = false;
    i64 contentLength = -1;
    bool chunked = false;
    usize headerEnd = 0;

    for (;;) {
        if (headersSeen) {
            const usize bodyAvail = raw->size() - headerEnd;
            if (!chunked && contentLength >= 0 &&
                bodyAvail >= static_cast<usize>(contentLength)) {
                return true;
            }
            if (chunked) {
                // Завершающее "0\r\n\r\n" — конец chunked-тела.
                static const char kChunkEnd[] = "0\r\n\r\n";
                const char* b = reinterpret_cast<const char*>(raw->data()) + headerEnd;
                const char* e = reinterpret_cast<const char*>(raw->data()) + raw->size();
                if (std::search(b, e, kChunkEnd, kChunkEnd + 5) != e) return true;
            }
        }
        const f64 remaining = deadline - NowSeconds();
        if (remaining <= 0.0) {
            if (!raw->empty()) return true;  // возвращаем то, что есть
            if (err) *err = "http read timed out";
            return false;
        }
        const int pr = WaitFd(fd, POLLIN, static_cast<int>(remaining * 1000.0) + 1);
        if (pr < 0) {
            if (err) *err = "http read: poll failed";
            return false;
        }
        if (pr == 0) continue;
        const EngSsize n = ::recv(fd, scratch.data(), static_cast<int>(scratch.size()), 0);
        if (n == 0) return true;  // EOF: сервер закрыл соединение (Connection: close)
        if (n < 0) {
            const int e = ENG_SOCK_ERR();
            if (IsWouldBlock(e)) continue;
            if (err) *err = "http read: " + SocketErrorString(e);
            return false;
        }
        raw->insert(raw->end(), scratch.begin(), scratch.begin() + n);
        if (!headersSeen) {
            const char* b = reinterpret_cast<const char*>(raw->data());
            const char* e = b + raw->size();
            const char* found = std::search(b, e, kHeaderEnd, kHeaderEnd + 4);
            if (found != e) {
                headerEnd = static_cast<usize>(found - b) + 4;
                headersSeen = true;
                const std::string header(b, headerEnd);
                const std::string lower = ToLowerAscii(header);
                const usize cl = lower.find("content-length:");
                if (cl != std::string::npos) {
                    contentLength = std::atoll(header.c_str() + cl + 15);
                }
                if (lower.find("transfer-encoding:") != std::string::npos &&
                    lower.find("chunked") != std::string::npos) {
                    chunked = true;
                }
            } else if (raw->size() > 256 * 1024) {
                if (err) *err = "http response headers too large";
                return false;
            }
        }
    }
}

bool DecodeChunked(const std::vector<u8>& body, std::vector<u8>* out) {
    out->clear();
    usize i = 0;
    while (i < body.size()) {
        const usize lineEnd = [&]() {
            for (usize j = i; j + 1 < body.size(); ++j) {
                if (body[j] == '\r' && body[j + 1] == '\n') return j;
            }
            return body.size();
        }();
        if (lineEnd >= body.size()) return false;
        const std::string sizeLine(reinterpret_cast<const char*>(body.data()) + i, lineEnd - i);
        const u64 chunkSize =
            std::strtoull(sizeLine.c_str(), nullptr, 16);
        i = lineEnd + 2;
        if (chunkSize == 0) return true;
        if (i + chunkSize > body.size()) return false;
        out->insert(out->end(), body.begin() + static_cast<std::ptrdiff_t>(i),
                    body.begin() + static_cast<std::ptrdiff_t>(i + chunkSize));
        i += static_cast<usize>(chunkSize);
        if (i + 1 < body.size() && body[i] == '\r' && body[i + 1] == '\n') i += 2;
    }
    return true;
}

std::string ResolveRedirect(const std::string& location, const std::string& scheme,
                            const std::string& host, u16 port) {
    if (location.rfind("http://", 0) == 0 || location.rfind("https://", 0) == 0) {
        return location;
    }
    std::string base = scheme + "://" + host;
    if (!(scheme == "http" && port == 80) && !(scheme == "https" && port == 443)) {
        base += ":" + std::to_string(port);
    }
    if (!location.empty() && location[0] == '/') return base + location;
    return base + "/" + location;
}

}  // namespace
#endif  // !ENG_PLATFORM_WASM

bool HttpGet(const std::string& url, std::vector<u8>* out, std::string* error) {
    if (out) out->clear();
#if defined(ENG_PLATFORM_WASM)
    void* data = nullptr;
    int len = 0;
    if (eng_js_http_get(url.c_str(), &data, &len) != 1) {
        const std::string msg = "HttpGet: fetch() failed for " + url;
        if (error) *error = msg;
        ENG_LOGE("net", "%s", msg.c_str());
        return false;
    }
    if (out && data != nullptr && len > 0) {
        const u8* p = static_cast<const u8*>(data);
        out->assign(p, p + len);
    }
    if (data != nullptr) eng_js_free(data);
    return true;
#else
    std::string current = url;
    for (int attempt = 0; attempt <= 3; ++attempt) {
        std::string scheme, host, path;
        u16 port = 0;
        if (!ParseHttpUrl(current, &scheme, &host, &port, &path)) {
            const std::string msg = "HttpGet: invalid url '" + current + "'";
            if (error) *error = msg;
            ENG_LOGE("net", "%s", msg.c_str());
            return false;
        }
        if (scheme == "https") {
            const std::string msg =
                "HttpGet: https:// is not supported by the native HTTP client (no TLS backend): " +
                current;
            if (error) *error = msg;
            ENG_LOGE("net", "%s", msg.c_str());
            return false;
        }
        if (scheme != "http") {
            const std::string msg = "HttpGet: unsupported scheme '" + scheme + "' in " + current;
            if (error) *error = msg;
            ENG_LOGE("net", "%s", msg.c_str());
            return false;
        }

        std::string err;
        EngSocket fd = ConnectTcpFd(host, port, 5.0f, &err);
        if (fd == kEngInvalidSocket) {
            const std::string msg = "HttpGet: connect to " + host + " failed: " + err;
            if (error) *error = msg;
            ENG_LOGE("net", "%s", msg.c_str());
            return false;
        }
        ENG_DEFER(CloseSocketHandle(fd));

        std::string req = "GET " + path + " HTTP/1.1\r\n";
        req += "Host: " + host;
        if (port != 80) req += ":" + std::to_string(port);
        req += "\r\n";
        req += "User-Agent: CrossRender/1.0\r\n";
        req += "Accept: */*\r\n";
        req += "Connection: close\r\n\r\n";
        if (!SendAllFd(fd, req.data(), req.size(), 5000, &err)) {
            const std::string msg = "HttpGet: send failed: " + err;
            if (error) *error = msg;
            ENG_LOGE("net", "%s", msg.c_str());
            return false;
        }

        std::vector<u8> raw;
        if (!ReadHttpResponse(fd, &raw, &err)) {
            const std::string msg = "HttpGet: " + err;
            if (error) *error = msg;
            ENG_LOGE("net", "%s", msg.c_str());
            return false;
        }

        static const char kHeaderEnd[] = "\r\n\r\n";
        const char* b = reinterpret_cast<const char*>(raw.data());
        const char* e = b + raw.size();
        const char* found = std::search(b, e, kHeaderEnd, kHeaderEnd + 4);
        if (found == e) {
            const std::string msg = "HttpGet: truncated response from " + host;
            if (error) *error = msg;
            ENG_LOGE("net", "%s", msg.c_str());
            return false;
        }
        const usize headerEnd = static_cast<usize>(found - b) + 4;
        const std::string header(b, headerEnd);
        const std::string lower = ToLowerAscii(header);
        const usize sp = header.find(' ');
        int status = 0;
        if (sp != std::string::npos) {
            usize i = sp + 1;
            while (i < header.size() && header[i] >= '0' && header[i] <= '9') {
                status = status * 10 + (header[i] - '0');
                ++i;
            }
        }

        if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
            const usize loc = lower.find("location:");
            if (loc == std::string::npos) {
                const std::string msg = "HttpGet: redirect without Location from " + host;
                if (error) *error = msg;
                ENG_LOGE("net", "%s", msg.c_str());
                return false;
            }
            const usize valStart = loc + 9;
            usize valEnd = header.find("\r\n", valStart);
            if (valEnd == std::string::npos) valEnd = header.size();
            const std::string location = TrimAscii(header.substr(valStart, valEnd - valStart));
            if (attempt == 3) {
                const std::string msg = "HttpGet: too many redirects for " + url;
                if (error) *error = msg;
                ENG_LOGE("net", "%s", msg.c_str());
                return false;
            }
            if (location.empty()) {
                const std::string msg = "HttpGet: empty redirect Location from " + host;
                if (error) *error = msg;
                ENG_LOGE("net", "%s", msg.c_str());
                return false;
            }
            const std::string next = ResolveRedirect(location, scheme, host, port);
            ENG_LOGI("net", "HttpGet: %d redirect -> %s", status, next.c_str());
            current = next;
            continue;
        }

        if (status != 200) {
            const std::string msg = "HttpGet: http status " + std::to_string(status) + " from " + host;
            if (error) *error = msg;
            ENG_LOGE("net", "%s", msg.c_str());
            return false;
        }

        std::vector<u8> body(raw.begin() + static_cast<std::ptrdiff_t>(headerEnd), raw.end());
        if (lower.find("transfer-encoding:") != std::string::npos &&
            lower.find("chunked") != std::string::npos) {
            std::vector<u8> decoded;
            if (!DecodeChunked(body, &decoded)) {
                const std::string msg = "HttpGet: malformed chunked body from " + host;
                if (error) *error = msg;
                ENG_LOGE("net", "%s", msg.c_str());
                return false;
            }
            body.swap(decoded);
        } else {
            const usize cl = lower.find("content-length:");
            if (cl != std::string::npos) {
                const u64 want = std::strtoull(header.c_str() + cl + 15, nullptr, 10);
                if (body.size() > want) body.resize(static_cast<usize>(want));
            }
        }
        if (out) *out = body;
        if (error) error->clear();
        ENG_LOGI("net", "HttpGet %s -> %u bytes", url.c_str(), static_cast<unsigned>(body.size()));
        return true;
    }
    const std::string msg = "HttpGet: too many redirects for " + url;
    if (error) *error = msg;
    return false;
#endif
}

// ===========================================================================
// Высокоуровневые помощники
// ===========================================================================
bool NetInit() {
#if defined(ENG_PLATFORM_WINDOWS) && !defined(ENG_PLATFORM_WASM)
    WSADATA data;
    const int rc = ::WSAStartup(MAKEWORD(2, 2), &data);
    if (rc != 0) {
        ENG_LOGE("net", "WSAStartup failed (code %d)", rc);
        return false;
    }
#endif
    ENG_LOGI("net", "network subsystem initialised");
    return true;
}

void NetShutdown() {
#if defined(ENG_PLATFORM_WINDOWS) && !defined(ENG_PLATFORM_WASM)
    ::WSACleanup();
#endif
    ENG_LOGI("net", "network subsystem shut down");
}

std::string NetResolveHost(const std::string& host) {
    u32 ip = 0;
    if (ParseIPv4Literal(host, &ip)) return Ipv4ToString(ip);
#if defined(ENG_PLATFORM_WASM)
    return std::string();  // без JS-моста DNS нет
#else
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    if (::getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || res == nullptr) {
        ENG_LOGW("net", "NetResolveHost: could not resolve '%s'", host.c_str());
        if (res != nullptr) ::freeaddrinfo(res);
        return std::string();
    }
    std::string out;
    for (addrinfo* it = res; it != nullptr; it = it->ai_next) {
        if (it->ai_family == AF_INET && it->ai_addr != nullptr) {
            const auto* sin = reinterpret_cast<const sockaddr_in*>(it->ai_addr);
            out = Ipv4ToString(ntohl(sin->sin_addr.s_addr));
            break;
        }
    }
    ::freeaddrinfo(res);
    return out;
#endif
}

u16 NetFindFreePort() {
#if defined(ENG_PLATFORM_WASM)
    return 0;
#else
    EngSocket s = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (s == kEngInvalidSocket) {
        ENG_LOGW("net", "NetFindFreePort: socket() failed");
        return 0;
    }
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    if (::bind(s, reinterpret_cast<const sockaddr*>(&a), sizeof(a)) != 0) {
        ENG_LOGW("net", "NetFindFreePort: bind() failed");
        CloseSocketHandle(s);
        return 0;
    }
    socklen_t len = sizeof(a);
    if (::getsockname(s, reinterpret_cast<sockaddr*>(&a), &len) != 0) {
        CloseSocketHandle(s);
        return 0;
    }
    const u16 port = ntohs(a.sin_port);
    CloseSocketHandle(s);
    return port;
#endif
}

}  // namespace crossrender
