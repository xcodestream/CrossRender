//
// CrossRender
// Автор: xcodestream <xcodestream@gmail.com>
// Лицензия: MIT
// Дата: 15.05.2026
// Описание: сетевой слой: IPv4-адреса, TCP- и UDP-сокеты и веб-сокет-клиент.
//
#pragma once

#include "crossrender/core/Base.h"

#include <memory>
#include <string>
#include <vector>
#include <functional>

namespace crossrender {

// ---------------------------------------------------------------------------
// Адрес
// ---------------------------------------------------------------------------
struct NetAddress {
    u32 ip = 0;        // IPv4 в порядке байтов хоста
    u16 port = 0;
    [[nodiscard]] std::string ToString() const;
    static NetAddress Parse(const std::string& host, u16 port);
    static NetAddress Local(u16 port = 0);
    [[nodiscard]] bool IsAny() const { return ip == 0; }
    bool operator==(const NetAddress& o) const { return ip == o.ip && port == o.port; }
};

enum class NetProtocol : u8 { Tcp, Udp, WebSocket, WebSocketSecure };

enum class SocketState : u8 { Closed, Connecting, Connected, Listening, Error, Closing };

struct NetStats {
    u64 bytesSent = 0;
    u64 bytesReceived = 0;
    u64 packetsSent = 0;
    u64 packetsReceived = 0;
    u64 packetsLost = 0;
    f32 rttMs = 0;
    f32 packetLoss = 0;
    f32 sendRateBytesPerSec = 0;
    f32 receiveRateBytesPerSec = 0;
    u64 connectTimeMs = 0;
};

// Один пакет данных: датаграмма или фрагмент потока.
struct NetPacket {
    std::vector<u8> data;
    NetAddress sender;
    f64 receiveTime = 0;
    int channel = 0;   // подсказка канала надёжности (ненадёжный / надёжный с сохранением порядка)
    bool reliable = false;
};

// ---------------------------------------------------------------------------
// TCP-сокет (клиент + сервер)
// ---------------------------------------------------------------------------
class TcpSocket {
public:
    TcpSocket();
    ~TcpSocket();
    TcpSocket(TcpSocket&&) noexcept;
    TcpSocket& operator=(TcpSocket&&) noexcept;
    TcpSocket(const TcpSocket&) = delete;
    TcpSocket& operator=(const TcpSocket&) = delete;

    bool Listen(u16 port, int backlog = 8);
    bool Connect(const std::string& host, u16 port, f32 timeoutSeconds = 5.0f);
    // Принимает ожидающее соединение (неблокирующе). Возвращает nullptr, если его нет.
    std::unique_ptr<TcpSocket> Accept();
    // Неблокирующая: возвращает false, если данных нет.
    bool Send(const void* data, usize size);
    bool Receive(std::vector<u8>* out, usize maxBytes = 64 * 1024);
    // Помощники с фреймингом (4-байтовый префикс длины в little-endian).
    bool SendMessage(const std::vector<u8>& payload);
    bool ReceiveMessage(std::vector<u8>* payload);
    void Close();
    void SetNoDelay(bool enable);
    void SetNonBlocking(bool enable);
    void SetKeepAlive(bool enable);

    [[nodiscard]] bool Valid() const;
    [[nodiscard]] SocketState State() const;
    [[nodiscard]] NetAddress RemoteAddress() const;
    [[nodiscard]] NetAddress LocalAddress() const;
    [[nodiscard]] const NetStats& Stats() const { return stats_; }
    [[nodiscard]] const std::string& LastError() const { return error_; }

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
    NetStats stats_{};
    std::string error_;
};

// ---------------------------------------------------------------------------
// UDP-сокет
// ---------------------------------------------------------------------------
class UdpSocket {
public:
    UdpSocket();
    ~UdpSocket();
    UdpSocket(UdpSocket&&) noexcept;
    UdpSocket& operator=(UdpSocket&&) noexcept;
    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;

    bool Bind(u16 port);
    bool Connect(const std::string& host, u16 port);  // задаёт узел по умолчанию
    bool SendTo(const NetAddress& addr, const void* data, usize size);
    bool Send(const void* data, usize size);
    // Неблокирующий приём; возвращает false, если очередь пуста.
    bool ReceiveFrom(NetPacket* packet);
    void Close();
    void SetBroadcast(bool enable);
    void SetNonBlocking(bool enable);

    [[nodiscard]] bool Valid() const;
    [[nodiscard]] NetAddress LocalAddress() const;
    [[nodiscard]] const NetStats& Stats() const { return stats_; }
    [[nodiscard]] const std::string& LastError() const { return error_; }

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
    NetStats stats_{};
    std::string error_;
};

// ---------------------------------------------------------------------------
// WebSocket-клиент (RFC 6455). Нативно: собственные handshake и фрейминг.
// WASM: делегирует браузерному WebSocket через JS-связку.
// ---------------------------------------------------------------------------
enum class WsState : u8 { Closed, Connecting, Open, Closing, Error };

struct WsCallbacks {
    std::function<void()> onOpen;
    std::function<void(const std::string&)> onText;
    std::function<void(const std::vector<u8>&)> onBinary;
    std::function<void(u16, const std::string&)> onClose;
    std::function<void(const std::string&)> onError;
};

class WebSocket {
public:
    WebSocket();
    ~WebSocket();
    WebSocket(const WebSocket&) = delete;
    WebSocket& operator=(const WebSocket&) = delete;

    bool Connect(const std::string& url, const WsCallbacks& callbacks = {},
                 const std::vector<std::string>& protocols = {});
    // Обрабатывает handshake и входящие фреймы. Вызывать раз за кадр.
    void Poll();
    bool SendText(const std::string& text);
    bool SendBinary(const void* data, usize size);
    bool SendPing();
    void Close(u16 code = 1000, const std::string& reason = "");

    [[nodiscard]] WsState State() const { return state_; }
    [[nodiscard]] bool IsOpen() const { return state_ == WsState::Open; }
    [[nodiscard]] const std::string& Url() const { return url_; }
    [[nodiscard]] const NetStats& Stats() const { return stats_; }
    [[nodiscard]] const std::string& LastError() const { return error_; }
    // Согласованный субпротокол (пусто, если его нет).
    [[nodiscard]] const std::string& Protocol() const { return protocol_; }
    // Для тестов: подать сырые байты, как если бы они пришли из сокета.
    void InjectForTest(const void* data, usize size);

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
    WsState state_ = WsState::Closed;
    std::string url_, protocol_, error_;
    NetStats stats_{};
};

// ---------------------------------------------------------------------------
// Помощники высокого уровня
// ---------------------------------------------------------------------------
// Одноразовый HTTP(S) GET на уровне платформы (WinHTTP/NSURLSession/
// libcurl/JS fetch). Возвращает false при неудаче.
bool HttpGet(const std::string& url, std::vector<u8>* out, std::string* error = nullptr);

// Инициализация сетевой подсистемы (WSAStartup на Windows; в остальном no-op).
bool NetInit();
void NetShutdown();
// Разрешает имя хоста в строку с IPv4-адресом.
std::string NetResolveHost(const std::string& host);
// Свободный эфемерный порт (используется тестами).
u16 NetFindFreePort();

}  // namespace crossrender
