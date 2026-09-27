// Тесты crossrender/net (TCP, UDP, кадрирование WebSocket, SHA-1/base64, HttpGet).
//
// Здесь всё использует только loopback; внешний сетевой доступ не требуется.

#include "crossrender/net/Net.h"

#include "crossrender/core/Log.h"
#include "crossrender/test/Test.h"

#include "net/NetInternal.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <algorithm>
#include <functional>

using namespace crossrender;

namespace {

f64 NowSeconds() {
    using namespace std::chrono;
    return duration<f64>(steady_clock::now().time_since_epoch()).count();
}

void SleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

// Опрашивает `fn`, пока она не вернёт true или не истечёт `seconds` (ограничено, не зависает).
bool WaitUntil(const std::function<bool()>& fn, double seconds) {
    const double deadline = NowSeconds() + seconds;
    for (;;) {
        if (fn()) return true;
        if (NowSeconds() >= deadline) return fn();
        SleepMs(2);
    }
}

std::string ToLowerCopy(const std::string& s) {
    std::string out = s;
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

std::string TrimCopy(const std::string& s) {
    usize b = 0;
    usize e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n')) --e;
    return s.substr(b, e - b);
}

[[maybe_unused]] std::string HeaderValue(const std::string& header, const std::string& lowerName) {
    const std::string lower = ToLowerCopy(header);
    const usize p = lower.find(lowerName + ":");
    if (p == std::string::npos) return std::string();
    const usize start = p + lowerName.size() + 1;
    const usize end = header.find("\r\n", start);
    return TrimCopy(header.substr(start, end == std::string::npos ? std::string::npos : end - start));
}

// Ответ рукопожатия, который (синтетический) сервер отправляет для kWsTestClientKey.
std::string TestHandshakeResponse() {
    const std::string accept = net_internal::WebSocketAcceptKey(net_internal::kWsTestClientKey);
    return "HTTP/1.1 101 Switching Protocols\r\n"
           "Upgrade: websocket\r\n"
           "Connection: Upgrade\r\n"
           "Sec-WebSocket-Accept: " + accept + "\r\n\r\n";
}

std::string Str(const std::vector<u8>& v) { return std::string(v.begin(), v.end()); }

// Присоединяет рабочий поток, даже если проверка бросает исключение, чтобы
// неудачный ассерт не приводил к std::terminate через деструктор joinable std::thread.
struct ThreadJoiner {
    std::thread& thread;
    explicit ThreadJoiner(std::thread& t) : thread(t) {}
    ~ThreadJoiner() {
        if (thread.joinable()) thread.join();
    }
    ThreadJoiner(const ThreadJoiner&) = delete;
    ThreadJoiner& operator=(const ThreadJoiner&) = delete;
};

// Набор колбэков WebSocket устанавливается только через Connect(), поэтому
// синтетические (InjectForTest) тесты устанавливают его, пытаясь подключиться к
// закрытому loopback-порту. На порту 1 ничего не слушает, поэтому это быстро завершается неудачей.
bool InstallCallbacksViaRefusedConnect(WebSocket* ws, const WsCallbacks& cb) {
    const bool connected = ws->Connect("ws://127.0.0.1:1/", cb);
    return !connected;
}

}  // namespace

// ===========================================================================
// SHA-1 / base64 / ключ рукопожатия
// ===========================================================================
ENG_TEST(Net, Sha1Base64) {
    using namespace net_internal;

    // Известные тестовые векторы SHA-1.
    ENG_CHECK_STR_EQ(Sha1Hex("abc", 3), "a9993e364706816aba3e25717850c26c9cd0d89d");
    ENG_CHECK_STR_EQ(Sha1Hex("", 0), "da39a3ee5e6b4b0d3255bfef95601890afd80709");
    ENG_CHECK_STR_EQ(Sha1Hex("The quick brown fox jumps over the lazy dog", 43),
                     "2fd4e1c67a2d28fced849ee1bb76e7391b93eb12");

    // Многоблочный ввод не должен приводить к падению / должен давать 20 байтов дайджеста.
    const std::string longInput(500, 'x');
    ENG_CHECK_EQ(Sha1Hex(longInput.data(), longInput.size()).size(), static_cast<usize>(40));

    // Векторы base64 (все три случая паддинга).
    ENG_CHECK_STR_EQ(Base64Encode("abc", 3), "YWJj");
    ENG_CHECK_STR_EQ(Base64Encode("a", 1), "YQ==");
    ENG_CHECK_STR_EQ(Base64Encode("ab", 2), "YWI=");
    ENG_CHECK_STR_EQ(Base64Encode("the sample nonce", 16), "dGhlIHNhbXBsZSBub25jZQ==");

    std::vector<u8> decoded;
    ENG_CHECK(Base64Decode("dGhlIHNhbXBsZSBub25jZQ==", &decoded));
    ENG_CHECK_STR_EQ(Str(decoded), "the sample nonce");
    ENG_CHECK(!Base64Decode("!!!not base64!!!", &decoded));

    // RFC 6455, раздел 1.3: accept-токен для примера ключа.
    ENG_CHECK_STR_EQ(kWsTestClientKey, "dGhlIHNhbXBsZSBub25jZQ==");
    ENG_CHECK_STR_EQ(kWsTestAccept, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
    ENG_CHECK_STR_EQ(WebSocketAcceptKey("dGhlIHNhbXBsZSBub25jZQ=="),
                     "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");

    // Свежесгенерированный ключ — это 24 символа base64 (16 случайных байтов).
    const std::string k1 = GenerateWebSocketKey();
    const std::string k2 = GenerateWebSocketKey();
    ENG_CHECK_EQ(k1.size(), static_cast<usize>(24));
    ENG_CHECK(k1 != k2);
    std::vector<u8> rawKey;
    ENG_CHECK(Base64Decode(k1, &rawKey));
    ENG_CHECK_EQ(rawKey.size(), static_cast<usize>(16));
}

// ===========================================================================
// Адреса / инициализация / resolve
// ===========================================================================
ENG_TEST(Net, InitAndAddress) {
    ENG_CHECK(NetInit());

    const NetAddress a = NetAddress::Parse("127.0.0.1", 8080);
    ENG_CHECK_EQ(a.port, static_cast<u16>(8080));
    ENG_CHECK_EQ(a.ip, 0x7F000001u);
    ENG_CHECK(!a.IsAny());
    ENG_CHECK_STR_EQ(a.ToString(), "127.0.0.1:8080");
    ENG_CHECK(a == NetAddress::Parse("127.0.0.1", 8080));
    ENG_CHECK(!(a == NetAddress::Parse("127.0.0.1", 8081)));

    const NetAddress any = NetAddress::Parse("0.0.0.0", 9000);
    ENG_CHECK(any.IsAny());
    ENG_CHECK_STR_EQ(any.ToString(), "0.0.0.0:9000");

    const NetAddress local = NetAddress::Local(5000);
    ENG_CHECK_EQ(local.ip, 0x7F000001u);
    ENG_CHECK_EQ(local.port, static_cast<u16>(5000));

    // Разбор имени хоста идёт через getaddrinfo.
    ENG_CHECK_STR_EQ(NetAddress::Parse("localhost", 70).ToString(), "127.0.0.1:70");

    NetShutdown();
    ENG_CHECK(NetInit());  // оставляем подсистему пригодной для остальных тестов
}

ENG_TEST(Net, ResolveHost) {
    ENG_CHECK(NetInit());
    ENG_CHECK_STR_EQ(NetResolveHost("127.0.0.1"), "127.0.0.1");
    ENG_CHECK_STR_EQ(NetResolveHost("localhost"), "127.0.0.1");
    ENG_CHECK(NetFindFreePort() != 0);
}

// ===========================================================================
// TCP
// ===========================================================================
ENG_TEST(Net, TcpLoopback) {
    ENG_CHECK(NetInit());
    const u16 port = NetFindFreePort();
    ENG_CHECK(port != 0);

    TcpSocket server;
    ENG_CHECK_MSG(server.Listen(port), server.LastError());
    ENG_CHECK(server.Valid());
    ENG_CHECK(server.State() == SocketState::Listening);
    ENG_CHECK_EQ(server.LocalAddress().port, port);

    TcpSocket client;
    ENG_CHECK_MSG(client.Connect("127.0.0.1", port, 2.0f), client.LastError());
    ENG_CHECK(client.Valid());
    ENG_CHECK(client.State() == SocketState::Connected);
    ENG_CHECK_EQ(client.RemoteAddress().port, port);
    ENG_CHECK(client.Stats().connectTimeMs < 2000);

    std::unique_ptr<TcpSocket> peer;
    ENG_CHECK(WaitUntil([&]() { peer = server.Accept(); return peer != nullptr; }, 2.0));
    ENG_CHECK(peer->State() == SocketState::Connected);
    ENG_CHECK_EQ(peer->RemoteAddress().port, client.LocalAddress().port);

    // кадрированное сообщение client -> server
    const std::vector<u8> payload = {1, 2, 3, 4, 5, 0, 255};
    ENG_CHECK_MSG(client.SendMessage(payload), client.LastError());
    std::vector<u8> got;
    ENG_CHECK(WaitUntil([&]() { return peer->ReceiveMessage(&got); }, 2.0));
    ENG_CHECK(got == payload);

    // кадрированное сообщение server -> client
    const std::vector<u8> reply = {'p', 'o', 'n', 'g'};
    ENG_CHECK_MSG(peer->SendMessage(reply), peer->LastError());
    std::vector<u8> got2;
    ENG_CHECK(WaitUntil([&]() { return client.ReceiveMessage(&got2); }, 2.0));
    ENG_CHECK(got2 == reply);

    // Неблокирующий Receive() на пустой очереди сразу возвращает false.
    std::vector<u8> none;
    const f64 t0 = NowSeconds();
    ENG_CHECK(!peer->Receive(&none));
    ENG_CHECK(NowSeconds() - t0 < 0.5);

    // Учёт статистики: 4-байтовый префикс длины + полезная нагрузка в каждом направлении.
    ENG_CHECK(client.Stats().bytesSent > 0);
    ENG_CHECK(client.Stats().bytesReceived > 0);
    ENG_CHECK(peer->Stats().bytesSent > 0);
    ENG_CHECK(peer->Stats().bytesReceived > 0);
    ENG_CHECK(client.Stats().packetsSent > 0);
    ENG_CHECK_EQ(client.Stats().bytesSent, static_cast<u64>(4 + payload.size()));
    ENG_CHECK_EQ(peer->Stats().bytesReceived, static_cast<u64>(4 + payload.size()));

    client.SetNoDelay(true);
    client.SetKeepAlive(true);
    client.SetNonBlocking(true);
    ENG_CHECK(client.Valid());

    client.Close();
    ENG_CHECK(!client.Valid());
    ENG_CHECK(client.State() == SocketState::Closed);
    peer->Close();
    server.Close();
    ENG_CHECK(!server.Valid());
}

ENG_TEST(Net, TcpListenEphemeral) {
    ENG_CHECK(NetInit());
    TcpSocket server;
    // Порт 0 просит ОС выделить эфемерный порт; LocalAddress() должна его сообщить.
    ENG_CHECK_MSG(server.Listen(0), server.LastError());
    ENG_CHECK(server.Valid());
    const u16 port = server.LocalAddress().port;
    ENG_CHECK(port != 0);

    TcpSocket client;
    ENG_CHECK_MSG(client.Connect("127.0.0.1", port, 2.0f), client.LastError());
    std::unique_ptr<TcpSocket> peer;
    ENG_CHECK(WaitUntil([&]() { peer = server.Accept(); return peer != nullptr; }, 2.0));
    ENG_CHECK(peer->Valid());
    peer->Close();
    client.Close();
    server.Close();
}

ENG_TEST(Net, TcpConnectRefused) {
    ENG_CHECK(NetInit());
    // На этом свободном порту (полученном через UDP) ничего не слушает.
    const u16 port = NetFindFreePort();
    ENG_CHECK(port != 0);

    TcpSocket c;
    const f64 t0 = NowSeconds();
    const bool ok = c.Connect("127.0.0.1", port, 1.0f);
    const f64 elapsed = NowSeconds() - t0;

    ENG_CHECK(!ok);
    ENG_CHECK(elapsed < 3.0);  // быстрый отказ, без зависания
    ENG_CHECK(!c.Valid());
    ENG_CHECK(c.State() == SocketState::Error);
    ENG_CHECK(!c.LastError().empty());

    // Дальнейшее использование должно корректно завершаться неудачей, а не падать.
    ENG_CHECK(!c.SendMessage({1, 2, 3}));
    std::vector<u8> out;
    ENG_CHECK(!c.Receive(&out));
    ENG_CHECK(!c.ReceiveMessage(&out));
    ENG_CHECK(c.Accept() == nullptr);
}

// ===========================================================================
// UDP
// ===========================================================================
ENG_TEST(Net, UdpLoopback) {
    ENG_CHECK(NetInit());
    UdpSocket a;
    UdpSocket b;
    ENG_CHECK_MSG(a.Bind(0), a.LastError());
    ENG_CHECK_MSG(b.Bind(0), b.LastError());
    ENG_CHECK(a.Valid());
    ENG_CHECK(b.Valid());
    ENG_CHECK(a.LocalAddress().port != 0);
    ENG_CHECK(b.LocalAddress().port != 0);
    ENG_CHECK(a.LocalAddress().port != b.LocalAddress().port);

    const NetAddress target = NetAddress::Parse("127.0.0.1", b.LocalAddress().port);
    const std::vector<u8> payload = {0xDE, 0xAD, 0xBE, 0xEF, 1, 2, 3};
    ENG_CHECK_MSG(a.SendTo(target, payload.data(), payload.size()), a.LastError());

    NetPacket pkt;
    ENG_CHECK(WaitUntil([&]() { return b.ReceiveFrom(&pkt); }, 2.0));
    ENG_CHECK(pkt.data == payload);
    ENG_CHECK(pkt.sender.ip == NetAddress::Parse("127.0.0.1", 0).ip);
    ENG_CHECK_EQ(pkt.sender.port, a.LocalAddress().port);
    ENG_CHECK(pkt.receiveTime > 0.0);
    ENG_CHECK_EQ(pkt.channel, 0);
    ENG_CHECK(!pkt.reliable);

    ENG_CHECK_EQ(a.Stats().bytesSent, static_cast<u64>(payload.size()));
    ENG_CHECK_EQ(a.Stats().packetsSent, static_cast<u64>(1));
    ENG_CHECK_EQ(b.Stats().bytesReceived, static_cast<u64>(payload.size()));
    ENG_CHECK_EQ(b.Stats().packetsReceived, static_cast<u64>(1));

    // Пустая очередь -> сразу false, без блокировки.
    NetPacket empty;
    const f64 t0 = NowSeconds();
    ENG_CHECK(!b.ReceiveFrom(&empty));
    ENG_CHECK(NowSeconds() - t0 < 0.5);

    // Send() требует предварительный Connect().
    ENG_CHECK(!a.Send(payload.data(), payload.size()));
    ENG_CHECK(!a.LastError().empty());

    // Connect() задаёт пира по умолчанию, после чего Send() работает.
    UdpSocket c;
    ENG_CHECK_MSG(c.Connect("127.0.0.1", b.LocalAddress().port), c.LastError());
    const std::vector<u8> payload2 = {'h', 'i'};
    ENG_CHECK_MSG(c.Send(payload2.data(), payload2.size()), c.LastError());
    NetPacket pkt2;
    ENG_CHECK(WaitUntil([&]() { return b.ReceiveFrom(&pkt2); }, 2.0));
    ENG_CHECK(pkt2.data == payload2);

    // Граничные случаи не приводят к падению.
    ENG_CHECK(!a.SendTo(target, nullptr, 4));
    ENG_CHECK(a.SendTo(target, payload.data(), 0));

    c.Close();
    ENG_CHECK(!c.Valid());
    a.Close();
    b.Close();
}

// ===========================================================================
// WebSocket: чистый кодек кадров
// ===========================================================================
ENG_TEST(Net, WsFrameCodec) {
    using namespace net_internal;
    usize consumed = 0;
    WsFrame frame;

    // Приём-передача (round-trip) небольшого маскированного кадра.
    const std::string small = "The quick brown fox";
    const std::vector<u8> f =
        WsBuildFrame(kWsBinary, small.data(), small.size(), true, true, 0x12345678u);
    ENG_CHECK_EQ(WsParseFrame(f.data(), f.size(), &consumed, &frame), 1);
    ENG_CHECK_EQ(consumed, f.size());
    ENG_CHECK_EQ(static_cast<int>(frame.opcode), static_cast<int>(kWsBinary));
    ENG_CHECK(frame.fin);
    ENG_CHECK(frame.masked);
    ENG_CHECK_STR_EQ(Str(frame.payload), small);

    // Немаскированный текстовый кадр (server -> client).
    const std::vector<u8> f2 = WsBuildFrame(kWsText, small.data(), small.size(), true, false, 0);
    ENG_CHECK_EQ(WsParseFrame(f2.data(), f2.size(), &consumed, &frame), 1);
    ENG_CHECK(!frame.masked);
    ENG_CHECK_STR_EQ(Str(frame.payload), small);

    // 16-битная длина полезной нагрузки (при маскировании: +4 байта маски).
    const std::vector<u8> medium(300, 0xAB);
    const std::vector<u8> f3 =
        WsBuildFrame(kWsBinary, medium.data(), medium.size(), true, true, 0xAABBCCDDu);
    ENG_CHECK_EQ(f3.size(), static_cast<usize>(2 + 2 + 4 + 300));
    ENG_CHECK_EQ(WsParseFrame(f3.data(), f3.size(), &consumed, &frame), 1);
    ENG_CHECK(frame.payload == medium);

    // 64-битная длина полезной нагрузки.
    const std::vector<u8> large(70000, 0x5A);
    const std::vector<u8> f4 = WsBuildFrame(kWsBinary, large.data(), large.size(), true, false, 0);
    ENG_CHECK_EQ(f4.size(), static_cast<usize>(2 + 8 + 70000));
    ENG_CHECK_EQ(WsParseFrame(f4.data(), f4.size(), &consumed, &frame), 1);
    ENG_CHECK(frame.payload == large);

    // Частичные кадры сообщают "нужно больше данных".
    ENG_CHECK_EQ(WsParseFrame(f.data(), 1, &consumed, &frame), 0);
    ENG_CHECK_EQ(WsParseFrame(f3.data(), f3.size() - 1, &consumed, &frame), 0);
    ENG_CHECK_EQ(WsParseFrame(nullptr, 0, &consumed, &frame), 0);
    ENG_CHECK_EQ(consumed, static_cast<usize>(0));

    // Ошибки протокола: неверный opcode, биты RSV, слишком большой управляющий кадр.
    const u8 badOpcode[2] = {0x8F, 0x00};
    ENG_CHECK_EQ(WsParseFrame(badOpcode, sizeof(badOpcode), &consumed, &frame), -1);
    const u8 badRsv[2] = {0xC1, 0x00};
    ENG_CHECK_EQ(WsParseFrame(badRsv, sizeof(badRsv), &consumed, &frame), -1);
    const u8 badControl[4] = {0x89, 126, 0x00, 0x7E};  // ping, заявляющий 126 байтов полезной нагрузки
    ENG_CHECK_EQ(WsParseFrame(badControl, sizeof(badControl), &consumed, &frame), -1);
}

// ===========================================================================
// WebSocket: рукопожатие + кадрирование через InjectForTest
// ===========================================================================
ENG_TEST(Net, WebSocketInjected) {
    WebSocket ws;
    int opened = 0;
    int errors = 0;
    std::string text;
    std::vector<u8> binary;
    WsCallbacks cb;
    cb.onOpen = [&]() { ++opened; };
    cb.onText = [&](const std::string& s) { text = s; };
    cb.onBinary = [&](const std::vector<u8>& b) { binary = b; };
    cb.onError = [&](const std::string&) { ++errors; };

    ENG_CHECK(ws.State() == WsState::Closed);
    ENG_CHECK(!ws.IsOpen());

    // Устанавливаем колбэки (возможно только через Connect) с помощью отклонённого
    // loopback-подключения, затем гоняем протокол синтетически.
    ENG_CHECK(InstallCallbacksViaRefusedConnect(&ws, cb));
    ENG_CHECK_EQ(errors, 1);

    const std::string response = TestHandshakeResponse();
    ws.InjectForTest(response.data(), response.size());
    ENG_CHECK(ws.State() == WsState::Connecting);
    ws.Poll();

    // Инжектированный ответ 101 должен открыть сокет и вызвать onOpen.
    ENG_CHECK(ws.IsOpen());
    ENG_CHECK_EQ(opened, 1);
    ENG_CHECK_STR_EQ(ws.Protocol(), "");
    ENG_CHECK(ws.LastError().empty());

    // Сырой немаскированный текстовый кадр сервера -> onText с той же строкой.
    const std::string hello = "hello";
    const std::vector<u8> f1 =
        net_internal::WsBuildFrame(net_internal::kWsText, hello.data(), hello.size(), true, false, 0);
    ws.InjectForTest(f1.data(), f1.size());
    ws.Poll();
    ENG_CHECK_STR_EQ(text, "hello");

    // Маскированный клиентский кадр от нашего конструктора проходит круговой путь через наш парсер.
    const std::string masked = "masked-payload";
    const std::vector<u8> f2 = net_internal::WsBuildFrame(net_internal::kWsText, masked.data(),
                                                          masked.size(), true, true, 0xDEADBEEFu);
    ws.InjectForTest(f2.data(), f2.size());
    ws.Poll();
    ENG_CHECK_STR_EQ(text, "masked-payload");

    // Фрагментированное текстовое сообщение: text + continuation.
    text.clear();
    const std::string part1 = "frag";
    const std::string part2 = "mented";
    const std::vector<u8> f3 = net_internal::WsBuildFrame(net_internal::kWsText, part1.data(),
                                                          part1.size(), false, false, 0);
    const std::vector<u8> f4 = net_internal::WsBuildFrame(net_internal::kWsContinuation,
                                                          part2.data(), part2.size(), true, false, 0);
    ws.InjectForTest(f3.data(), f3.size());
    ws.Poll();
    ENG_CHECK(text.empty());  // не завершено, пока не придёт последний фрагмент
    ws.InjectForTest(f4.data(), f4.size());
    ws.Poll();
    ENG_CHECK_STR_EQ(text, "fragmented");

    // Бинарный кадр с 16-битной длиной.
    const std::vector<u8> big(400, 0x11);
    const std::vector<u8> f5 =
        net_internal::WsBuildFrame(net_internal::kWsBinary, big.data(), big.size(), true, false, 0);
    ws.InjectForTest(f5.data(), f5.size());
    ws.Poll();
    ENG_CHECK(binary == big);

    // Кадр ping не должен ломать поток (pong — no-op без сокета).
    const std::vector<u8> ping = net_internal::WsBuildFrame(net_internal::kWsPing, "p", 1, true, false, 0);
    ws.InjectForTest(ping.data(), ping.size());
    ws.Poll();
    ENG_CHECK(ws.IsOpen());

    // Оба кадра одной инжекцией: два кадра в одном буфере.
    text.clear();
    std::vector<u8> two = f1;
    two.insert(two.end(), f1.begin(), f1.end());
    ws.InjectForTest(two.data(), two.size());
    ws.Poll();
    ENG_CHECK_STR_EQ(text, "hello");
    ENG_CHECK(ws.IsOpen());

    // Некорректный кадр -> состояние Error.
    const std::vector<u8> bad = net_internal::WsBuildFrame(
        net_internal::kWsText, hello.data(), hello.size(), true, false, 0);
    std::vector<u8> badOpcode = bad;
    badOpcode[0] = static_cast<u8>(0x80 | 0x0F);  // недопустимый opcode 0xF
    ws.InjectForTest(badOpcode.data(), badOpcode.size());
    ws.Poll();
    ENG_CHECK(ws.State() == WsState::Error);
    ENG_CHECK_EQ(errors, 2);

    // Close() из Error всё равно переводит в Closed и идемпотентен.
    ws.Close();
    ENG_CHECK(ws.State() == WsState::Closed);
    ENG_CHECK(!ws.SendText("after close"));
    ENG_CHECK(!ws.SendBinary(big.data(), big.size()));
    ENG_CHECK(!ws.SendPing());
    ws.Poll();  // безопасно в закрытом состоянии
}

ENG_TEST(Net, WebSocketSendBeforeOpen) {
    WebSocket ws;
    ENG_CHECK(ws.State() == WsState::Closed);
    ENG_CHECK(!ws.IsOpen());
    ENG_CHECK(ws.Url().empty());

    // Отправка до открытия сокета должна корректно завершаться неудачей (без падения и блокировки).
    ENG_CHECK(!ws.SendText("nope"));
    ENG_CHECK(!ws.LastError().empty());
    const std::vector<u8> data = {1, 2, 3};
    ENG_CHECK(!ws.SendBinary(data.data(), data.size()));
    ENG_CHECK(!ws.SendPing());
    ENG_CHECK(ws.Stats().bytesSent == 0);

    // Закрытие никогда не подключавшегося сокета безвредно и завершается состоянием Closed.
    ws.Close();
    ENG_CHECK(ws.State() == WsState::Closed);
    ws.Poll();

    // Сокет, всё ещё проходящий рукопожатие (полного ответа 101 ещё нет), тоже
    // не открыт: SendText должна давать сбой, пока Poll() не завершит рукопожатие.
    WebSocket pending;
    const std::string partial = "HTTP/1.1 101 Switching Protocols\r\n";
    pending.InjectForTest(partial.data(), partial.size());
    ENG_CHECK(pending.State() == WsState::Connecting);
    ENG_CHECK(!pending.IsOpen());
    ENG_CHECK(!pending.SendText("too early"));
    ENG_CHECK(!pending.SendBinary(data.data(), data.size()));
    ENG_CHECK(!pending.SendPing());
    pending.Poll();  // по-прежнему нет "\r\n\r\n" -> остаётся Connecting
    ENG_CHECK(pending.State() == WsState::Connecting);
    ENG_CHECK(!pending.SendText("still too early"));
    pending.Close();
    ENG_CHECK(pending.State() == WsState::Closed);

    // Не-websocket URL отклоняется с понятной ошибкой вместо падения.
    WebSocket bad;
    int errorCount = 0;
    WsCallbacks cb;
    cb.onError = [&](const std::string&) { ++errorCount; };
    ENG_CHECK(!bad.Connect("http://127.0.0.1:1/", cb));
    ENG_CHECK(bad.State() == WsState::Error);
    ENG_CHECK(!bad.LastError().empty());
    ENG_CHECK_EQ(errorCount, 1);
    ENG_CHECK(!bad.Connect("not-a-websocket-url", cb));
    ENG_CHECK_EQ(errorCount, 2);
#if !defined(ENG_PLATFORM_WASM)
    // TLS не поддерживается нативным клиентом: громкая ошибка, без зависания.
    WebSocket tls;
    ENG_CHECK(!tls.Connect("wss://127.0.0.1:1/", cb));
    ENG_CHECK(tls.State() == WsState::Error);
    ENG_CHECK(tls.LastError().find("wss") != std::string::npos);
#endif
}

ENG_TEST(Net, WebSocketCloseFrame) {
    WebSocket ws;
    int closed = 0;
    u16 code = 0;
    std::string reason;
    WsCallbacks cb;
    cb.onClose = [&](u16 c, const std::string& r) {
        ++closed;
        code = c;
        reason = r;
    };
    ENG_CHECK(InstallCallbacksViaRefusedConnect(&ws, cb));

    const std::string response = TestHandshakeResponse();
    ws.InjectForTest(response.data(), response.size());
    ws.Poll();
    ENG_CHECK(ws.IsOpen());

    // Кадр закрытия сервера: код 1001 + "bye".
    const u8 payload[5] = {0x03, 0xE9, 'b', 'y', 'e'};
    const std::vector<u8> closeFrame =
        net_internal::WsBuildFrame(net_internal::kWsClose, payload, sizeof(payload), true, false, 0);
    ws.InjectForTest(closeFrame.data(), closeFrame.size());
    ws.Poll();

    ENG_CHECK(ws.State() == WsState::Closed);
    ENG_CHECK_EQ(closed, 1);
    ENG_CHECK_EQ(static_cast<int>(code), 1001);
    ENG_CHECK_STR_EQ(reason, "bye");
    ENG_CHECK(!ws.SendText("x"));

    // Poll() после закрытия пиром не должен вызывать колбэк дважды.
    ws.Poll();
    ENG_CHECK_EQ(closed, 1);

    // Повторное закрытие идемпотентно.
    ws.Close();
    ENG_CHECK(ws.State() == WsState::Closed);
    ENG_CHECK_EQ(closed, 1);
}

// ===========================================================================
// WebSocket: настоящее рукопожатие + кадрирование с loopback-сервером
// ===========================================================================
ENG_TEST(Net, WebSocketLoopbackServer) {
    ENG_CHECK(NetInit());
#if !defined(ENG_PLATFORM_WASM)
    const u16 port = NetFindFreePort();
    ENG_CHECK(port != 0);

    std::atomic<bool> ready{false};
    std::atomic<bool> gotClientText{false};
    std::string clientText;
    std::string serverError;

    std::thread server([&]() {
        TcpSocket srv;
        if (!srv.Listen(port)) {
            serverError = srv.LastError();
            ready = true;
            return;
        }
        ready = true;
        const f64 deadline = NowSeconds() + 5.0;

        std::unique_ptr<TcpSocket> conn;
        while (NowSeconds() < deadline) {
            conn = srv.Accept();
            if (conn) break;
            SleepMs(2);
        }
        if (!conn) return;

        // Читаем HTTP upgrade-запрос.
        std::vector<u8> request;
        static const char kHeaderEnd[] = "\r\n\r\n";
        while (NowSeconds() < deadline) {
            std::vector<u8> chunk;
            if (conn->Receive(&chunk)) request.insert(request.end(), chunk.begin(), chunk.end());
            if (std::search(request.begin(), request.end(), kHeaderEnd, kHeaderEnd + 4) !=
                request.end()) {
                break;
            }
            SleepMs(2);
        }
        const std::string key = HeaderValue(Str(request), "sec-websocket-key");
        if (key.empty()) {
            serverError = "server: no Sec-WebSocket-Key in request";
            return;
        }
        const std::string response = "HTTP/1.1 101 Switching Protocols\r\n"
                                     "Upgrade: websocket\r\n"
                                     "Connection: Upgrade\r\n"
                                     "Sec-WebSocket-Accept: " +
                                     net_internal::WebSocketAcceptKey(key) + "\r\n\r\n";
        conn->Send(response.data(), response.size());

        // Отправляем немаскированный текстовый кадр server -> client.
        const std::string greeting = "from-server";
        const std::vector<u8> frame = net_internal::WsBuildFrame(
            net_internal::kWsText, greeting.data(), greeting.size(), true, false, 0);
        conn->Send(frame.data(), frame.size());

        // Читаем (маскированный) клиентский кадр и снимаем маску нашим парсером.
        std::vector<u8> incoming;
        const f64 readDeadline = NowSeconds() + 3.0;
        while (NowSeconds() < readDeadline) {
            std::vector<u8> chunk;
            if (conn->Receive(&chunk)) incoming.insert(incoming.end(), chunk.begin(), chunk.end());
            usize consumed = 0;
            net_internal::WsFrame parsed;
            const int r =
                net_internal::WsParseFrame(incoming.data(), incoming.size(), &consumed, &parsed);
            if (r == 1) {
                if (parsed.opcode == net_internal::kWsText) {
                    clientText = Str(parsed.payload);
                    gotClientText = true;
                }
                break;
            }
            SleepMs(2);
        }

        // Рукопожатие закрытия: 1000 "bye".
        const u8 closePayload[5] = {0x03, 0xE8, 'b', 'y', 'e'};
        const std::vector<u8> closeFrame = net_internal::WsBuildFrame(
            net_internal::kWsClose, closePayload, sizeof(closePayload), true, false, 0);
        conn->Send(closeFrame.data(), closeFrame.size());
    });
    ThreadJoiner serverJoiner(server);

    while (!ready) SleepMs(1);

    WebSocket ws;
    int opened = 0;
    int closed = 0;
    u16 closeCode = 0;
    std::string text;
    std::string lastError;
    WsCallbacks cb;
    cb.onOpen = [&]() { ++opened; };
    cb.onText = [&](const std::string& s) { text = s; };
    cb.onClose = [&](u16 c, const std::string&) {
        ++closed;
        closeCode = c;
    };
    cb.onError = [&](const std::string& m) { lastError = m; };

    const bool requested =
        ws.Connect("ws://127.0.0.1:" + std::to_string(port) + "/chat", cb, {"chat"});
    ENG_CHECK_MSG(requested, ws.LastError());
    ENG_CHECK(WaitUntil(
        [&]() {
            ws.Poll();
            return ws.IsOpen();
        },
        3.0));
    ENG_CHECK_MSG(ws.IsOpen(), lastError);
    ENG_CHECK_EQ(opened, 1);
    ENG_CHECK(ws.Stats().bytesSent > 0);

    // Текст server -> client.
    ENG_CHECK(WaitUntil(
        [&]() {
            ws.Poll();
            return !text.empty();
        },
        3.0));
    ENG_CHECK_STR_EQ(text, "from-server");

    // Текст client -> server (маскированный в сети).
    ENG_CHECK(ws.SendText("from-client"));
    ENG_CHECK(WaitUntil([&]() { return gotClientText.load(); }, 3.0));

    // Кадр закрытия сервера -> onClose + Closed.
    ENG_CHECK(WaitUntil(
        [&]() {
            ws.Poll();
            return closed > 0;
        },
        3.0));
    ENG_CHECK(ws.State() == WsState::Closed);
    ENG_CHECK_EQ(closed, 1);
    ENG_CHECK_EQ(static_cast<int>(closeCode), 1000);
    ENG_CHECK(ws.Stats().bytesReceived > 0);

    server.join();
    ENG_CHECK_MSG(serverError.empty(), serverError);
    ENG_CHECK_STR_EQ(clientText, "from-client");
#endif
}

// ===========================================================================
// HttpGet
// ===========================================================================
ENG_TEST(Net, HttpGetRejectsTls) {
    ENG_CHECK(NetInit());
#if !defined(ENG_PLATFORM_WASM)
    std::vector<u8> out;
    std::string error;
    // https:// должен давать сбой без обращения к сети (в нативной версии нет TLS).
    ENG_CHECK(!HttpGet("https://example.com/", &out, &error));
    ENG_CHECK(error.find("https") != std::string::npos);
    ENG_CHECK(out.empty());

    error.clear();
    ENG_CHECK(!HttpGet("ftp://example.com/", &out, &error));
    ENG_CHECK(!error.empty());
    ENG_CHECK(!HttpGet("not a url", &out, &error));
    ENG_CHECK(!error.empty());
#endif
}

ENG_TEST(Net, HttpGetLoopback) {
    ENG_CHECK(NetInit());
#if !defined(ENG_PLATFORM_WASM)
    const u16 port = NetFindFreePort();
    ENG_CHECK(port != 0);

    std::atomic<bool> serverReady{false};
    std::string serverError;
    std::thread server([&]() {
        TcpSocket srv;
        const bool listening = srv.Listen(port);
        if (!listening) serverError = srv.LastError();
        serverReady = true;
        if (!listening) return;

        std::unique_ptr<TcpSocket> conn;
        const f64 deadline = NowSeconds() + 5.0;
        while (NowSeconds() < deadline) {
            conn = srv.Accept();
            if (conn) break;
            SleepMs(2);
        }
        if (!conn) return;

        std::vector<u8> request;
        static const char kHeaderEnd[] = "\r\n\r\n";
        while (NowSeconds() < deadline) {
            std::vector<u8> chunk;
            if (conn->Receive(&chunk)) request.insert(request.end(), chunk.begin(), chunk.end());
            if (std::search(request.begin(), request.end(), kHeaderEnd, kHeaderEnd + 4) !=
                request.end()) {
                break;
            }
            SleepMs(2);
        }
        const std::string body = "hello world";
        const std::string response = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: " +
                                     std::to_string(body.size()) +
                                     "\r\nConnection: close\r\n\r\n" + body;
        conn->Send(response.data(), response.size());
    });
    ThreadJoiner serverJoiner(server);

    while (!serverReady) SleepMs(1);

    std::vector<u8> body;
    std::string error;
    const bool ok = HttpGet("http://127.0.0.1:" + std::to_string(port) + "/hello", &body, &error);
    server.join();

    ENG_CHECK_MSG(serverError.empty(), serverError);
    ENG_CHECK_MSG(ok, error);
    ENG_CHECK_STR_EQ(Str(body), "hello world");
#endif
}

ENG_TEST(Net, HttpGetRedirect) {
    ENG_CHECK(NetInit());
#if !defined(ENG_PLATFORM_WASM)
    const u16 port = NetFindFreePort();
    ENG_CHECK(port != 0);

    std::atomic<bool> serverReady{false};
    std::string serverError;
    std::thread server([&]() {
        TcpSocket srv;
        const bool listening = srv.Listen(port);
        if (!listening) serverError = srv.LastError();
        serverReady = true;
        if (!listening) return;

        // Обслуживаем два запроса: 302 для /start, затем 200 для /final.
        for (int i = 0; i < 2; ++i) {
            const f64 deadline = NowSeconds() + 5.0;
            std::unique_ptr<TcpSocket> conn;
            while (NowSeconds() < deadline) {
                conn = srv.Accept();
                if (conn) break;
                SleepMs(2);
            }
            if (!conn) return;

            std::vector<u8> request;
            static const char kHeaderEnd[] = "\r\n\r\n";
            while (NowSeconds() < deadline) {
                std::vector<u8> chunk;
                if (conn->Receive(&chunk)) {
                    request.insert(request.end(), chunk.begin(), chunk.end());
                }
                if (std::search(request.begin(), request.end(), kHeaderEnd, kHeaderEnd + 4) !=
                    request.end()) {
                    break;
                }
                SleepMs(2);
            }
            if (i == 0) {
                const std::string redirect =
                    "HTTP/1.1 302 Found\r\nLocation: /final\r\nContent-Length: 0\r\n"
                    "Connection: close\r\n\r\n";
                conn->Send(redirect.data(), redirect.size());
            } else {
                const std::string body = "final";
                const std::string response =
                    "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(body.size()) +
                    "\r\nConnection: close\r\n\r\n" + body;
                conn->Send(response.data(), response.size());
            }
        }
    });
    ThreadJoiner serverJoiner(server);

    while (!serverReady) SleepMs(1);

    std::vector<u8> body;
    std::string error;
    const bool ok = HttpGet("http://127.0.0.1:" + std::to_string(port) + "/start", &body, &error);
    server.join();

    ENG_CHECK_MSG(serverError.empty(), serverError);
    ENG_CHECK_MSG(ok, error);
    ENG_CHECK_STR_EQ(Str(body), "final");
#endif
}
