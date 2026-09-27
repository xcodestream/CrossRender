# crossrender/net/Net.h — сокеты, адреса и веб-сокет-клиент

Сетевой слой движка: IPv4-адреса, TCP- и UDP-сокеты (и клиент, и сервер),
клиент WebSocket, счётчики трафика и одноразовый `HttpGet` — без исключений,
с ошибками через `LastError()`.

## Заголовок

```cpp
#include "crossrender/net/Net.h"
```

## Обзор

`Net.h` — один заголовок на весь сетевой слой. Здесь нет интерфейсов-абстракций
и нет зависимостей от оконного или графического слоя: это обёртки над
платформенным вводом-выводом (BSD sockets в macOS/Linux/iOS/Android, Winsock в
Windows) плюс собственный клиент WebSocket по стандарту RFC 6455 (протокол
веб-сокетов), написанный прямо в `Net.cpp`: рукопожатие, кадрирование, SHA-1 и
base64 там реализованы вручную, без внешних библиотек.

Реальный охват по платформам:

| Что | macOS, Linux, Windows, iOS, Android | WASM |
|---|---|---|
| TCP: `TcpSocket` | настоящие сокеты: `Listen`/`Connect`/`Accept`/`Send`/`Receive` и кадрированные `SendMessage`/`ReceiveMessage` | заглушки, вердикт в `LastError()` |
| UDP: `UdpSocket` | настоящие сокеты: `Bind`/`Connect`/`SendTo`/`Send`/`ReceiveFrom`/`SetBroadcast` | заглушки, вердикт в `LastError()` |
| WebSocket | настоящий клиент RFC 6455: рукопожатие и кадры вручную поверх обычного TCP | браузерный `WebSocket` через JS-прослойку `engine/src/platform/wasm/WebSocketGlue.cpp` |
| `HttpGet` | HTTP/1.1 GET вручную поверх TCP, только `http://` | синхронный `XMLHttpRequest` в `engine/src/platform/wasm/HttpGlue.cpp` |
| `NetResolveHost` | `getaddrinfo` | пустая строка — DNS без JS-прослойки нет |
| `NetFindFreePort` | UDP-bind на `127.0.0.1:0` | `0` |
| `NetInit` / `NetShutdown` | `WSAStartup` / `WSACleanup` в Windows, иначе только запись в лог | то же, без Winsock |

Чтение с сокета не блокирует поток: `TcpSocket::Receive`, `TcpSocket::Accept` и
`UdpSocket::ReceiveFrom` возвращают `false`/`nullptr`, когда данных нет.
Отправка, наоборот, ограниченно ждёт готовности сокета к записи (до 5 секунд),
а подключение (`TcpSocket::Connect`) синхронно ждёт завершения `connect`.

```cpp
// Минимальный цикл сервера и клиента по TCP — так это выглядит на настоящих платформах.
crossrender::NetInit();

crossrender::TcpSocket server;
const crossrender::u16 port = crossrender::NetFindFreePort();
if (!server.Listen(port)) {
    ENG_LOGE("net", "сервер не запустился: %s", server.LastError().c_str());
} else {
    crossrender::TcpSocket client;
    if (client.Connect("127.0.0.1", port, 2.0f)) {
        // На стороне сервера соединение появится только после Accept().
        std::unique_ptr<crossrender::TcpSocket> peer = server.Accept();
        ENG_LOGI("net", "соединение: %s", peer ? "принято" : "ещё не готово");
    }
}
crossrender::NetShutdown();
```

**Чего здесь нет.** Это важно знать до того, как вы начнёте на это
рассчитывать:

* **TLS/SSL нет нигде.** В движке нет TLS-бэкенда: `WebSocket::Connect("wss://...")`
  сразу завершается ошибкой с текстом про отсутствующий TLS-бэкенд, а
  `HttpGet("https://...")` — такой же ошибкой. Значение
  `NetProtocol::WebSocketSecure` объявлено в перечислении, но не реализовано
  ничем.
* **`crossrender::FetchUrl` из `File.h` — не часть этого заголовка** и безусловная
  заглушка: она возвращает `false` на **всех** платформах, включая WASM.
  Сетевую загрузку ассетов через неё построить нельзя (подробности —
  `docs/core/File.md`).
* **Нет оценки RTT и скорости.** Поля `NetStats::rttMs`,
  `NetStats::packetLoss`, `NetStats::sendRateBytesPerSec` и
  `NetStats::receiveRateBytesPerSec` реализация никогда не заполняет — там
  всегда `0`. Счётчики байт и пакетов, наоборот, настоящие.
* **Состояния `SocketState::Connecting` и `SocketState::Closing` не
  присваиваются никогда**: `TcpSocket::Connect` синхронен и заканчивается либо
  `Connected`, либо `Error`, а `TcpSocket::Close` даёт `Closed`.
* **`NetProtocol` не принимает ни одна функция** заголовка — это подсказка о
  транспорте, зарезервированная для вызывающего кода и будущих версий.
* **`NetPacket::channel` и `NetPacket::reliable` никто не заполняет** —
  надёжного канала поверх UDP в движке нет, поля остаются `0` и `false`.
* **DNS нет на WASM**: `NetResolveHost` возвращает пустую строку, а
  `NetAddress::Parse` разбирает только литеральный IPv4.
* **Только IPv4**: адрес хранится как `u32`, сокеты создаются с семейством
  `AF_INET`; IPv6, многоадресной рассылки и шифрования нет.
* **Нет потоков и очередей ввода-вывода**: `WebSocket` требует ручного вызова
  `Poll()` раз в кадр, сокеты не имеют внутренней синхронизации — один сокет
  не следует дёргать из двух потоков одновременно.

```cpp
// Защищённые схемы отклоняются мгновенно — это не «недоработка настройки», а факт.
std::vector<crossrender::u8> body;
std::string error;
if (!crossrender::HttpGet("https://example.com/level.json", &body, &error)) {
    ENG_LOGW("net", "HTTPS недоступен: %s", error.c_str());
}

crossrender::WebSocket secure;
if (!secure.Connect("wss://example.com/chat")) {
    ENG_LOGW("net", "WSS недоступен: %s", secure.LastError().c_str());
}
```

Типичный порядок работы:

1. На старте — `crossrender::NetInit()` (в Windows поднимает Winsock; в остальных
   системах просто пишет в лог и возвращает `true`).
2. Адрес сервера получают через `NetAddress::Parse` / `NetResolveHost`, а для
   сервера — через `TcpSocket::Listen(port)`; `NetFindFreePort()` даёт
   подходящий эфемерный порт (это подсказка, а не резервирование порта).
3. Обмен идёт через `TcpSocket` (поток) или `UdpSocket` (датаграммы);
   для текстовых и бинарных сообщений применяют `SendMessage`/`ReceiveMessage`
   с 4-байтовым префиксом длины.
4. WebSocket-клиент после `Connect` требует вызова `Poll()` раз в кадр — он
   доводит рукопожатие и разбирает входящие кадры, вызывая колбэки из
   `WsCallbacks`.
5. На выходе — `crossrender::NetShutdown()`.

## Члены класса

### `struct NetAddress`

IPv4-адрес и порт в одном значении: `ip` хранится в порядке байтов хоста
(`0x7F000001` — это `127.0.0.1`), `port` — обычное число. Структура
`trivially copyable`, её передают по значению и хранят в контейнерах.

```cpp
// Адрес по умолчанию — 0.0.0.0:0, то есть «любой интерфейс, любой порт».
crossrender::NetAddress empty;
ENG_LOGI("net", "по умолчанию: %s (any=%d)", empty.ToString().c_str(),
         empty.IsAny() ? 1 : 0);

crossrender::NetAddress server = crossrender::NetAddress::Parse("127.0.0.1", 27015);
ENG_LOGI("net", "сервер: %s", server.ToString().c_str());
```

### `u32 ip`

Адрес в порядке байтов хоста. `0` означает `0.0.0.0` — «любой интерфейс»;
именно это значение получается при неудачном разборе имени хоста, поэтому
`ip == 0` нельзя трактовать как валидный адрес назначения.

```cpp
crossrender::NetAddress a = crossrender::NetAddress::Parse("10.0.0.7", 9000);
if (a.ip == 0x0A000007u) {
    ENG_LOGI("net", "адрес разобран в двоичном виде: 0x%08X", a.ip);
} else if (a.IsAny()) {
    ENG_LOGW("net", "хост не разрешился — ip остался нулевым");
}
```

### `u16 port`

Порт в порядке байтов хоста. Для клиента `0` в этом поле бесполезен (подключение
к нулевому порту не сработает), а для `TcpSocket::Listen` нулевой порт означает
«дай любой свободный» — реальный номер потом сообщает
`TcpSocket::LocalAddress()`.

```cpp
crossrender::NetAddress local;
local.port = crossrender::NetFindFreePort();
ENG_LOGI("net", "пробуем занять порт %u", static_cast<unsigned>(local.port));
```

### `std::string ToString() const`

Печатает адрес в виде `"a.b.c.d:порт"`. Удобно для логов; для `ip == 0`
получается `"0.0.0.0:порт"`.

* **Возвращает:** строку вида `"127.0.0.1:8080"`.

```cpp
crossrender::NetAddress peer = crossrender::NetAddress::Parse("192.168.1.20", 7777);
ENG_LOGI("net", "подключаемся к %s", peer.ToString().c_str());   // "192.168.1.20:7777"
```

### `static NetAddress Parse(const std::string& host, u16 port)`

Собирает адрес из имени хоста и порта. Пустая строка и `"*"` дают `0.0.0.0`
(«любой интерфейс»). Литеральный IPv4 разбирается без сети; всё остальное
уходит в `NetResolveHost`.

* **Возвращает:** заполненный адрес; при неудаче `ip == 0`, а `port` остаётся
  тем, что передали.
* **Ограничение:** на WASM резолвинга нет — не литеральное имя даёт `ip == 0`.
* **Ограничение:** IPv6 не поддерживается, `"::1"` не разберётся.

```cpp
crossrender::NetAddress any  = crossrender::NetAddress::Parse("0.0.0.0", 8080);     // слушать всё
crossrender::NetAddress host = crossrender::NetAddress::Parse("localhost", 8080);   // через DNS
if (host.IsAny()) {
    ENG_LOGW("net", "имя не разрешилось — адрес остался нулевым");
} else {
    ENG_LOGI("net", "localhost -> %s", host.ToString().c_str());
}
```

### `static NetAddress Local(u16 port = 0)`

Адрес локальной петли (`127.0.0.1`) с указанным портом. Именно так удобно
задавать адрес сервера в тестах и локальных демонах.

* **Возвращает:** адрес `127.0.0.1:<port>`; при `port == 0` — `127.0.0.1:0`.
* **Контекст:** метод не открывает сокет и не проверяет, свободен ли порт.

```cpp
const crossrender::NetAddress loopback = crossrender::NetAddress::Local(27015);
ENG_LOGI("net", "локальный сервер: %s", loopback.ToString().c_str());
```

### `bool IsAny() const`

Проверяет, что `ip == 0`. Так помечается адрес «любой интерфейс» (`0.0.0.0`)
и результат неудачного `Parse`.

* **Возвращает:** `true` при `ip == 0`.
* **Ограничение:** метод не отличает намеренный `0.0.0.0` от неразрешённого
  имени — оба случая дают `true`.

```cpp
if (crossrender::NetAddress::Parse("no-such-host.invalid", 80).IsAny()) {
    ENG_LOGW("net", "хост не разрешился, подключаться некуда");
}
```

### `bool operator==(const NetAddress& o) const`

Сравнивает два адреса — и `ip`, и `port` должны совпасть. Удобно для проверок
в тестах и для поиска пира в списке.

* **Возвращает:** `true`, если оба поля равны.

```cpp
const crossrender::NetAddress a = crossrender::NetAddress::Local(5000);
const crossrender::NetAddress b = crossrender::NetAddress::Parse("127.0.0.1", 5000);
if (a == b) {
    ENG_LOGI("net", "адреса совпадают: %s", a.ToString().c_str());
}
```

### `enum class NetProtocol : u8`

Подсказка о транспорте. Перечислены **все** значения — других в заголовке нет.
Ни одна функция `Net.h` это перечисление не принимает: тип зарезервирован для
вызывающего кода и будущих версий, сам по себе он ничего не переключает.

| Значение | Смысл |
|---|---|
| `NetProtocol::Tcp` | потоковый транспорт; реализован классом `TcpSocket` |
| `NetProtocol::Udp` | датаграммы; реализован классом `UdpSocket` |
| `NetProtocol::WebSocket` | RFC 6455 поверх TCP; реализован классом `WebSocket` |
| `NetProtocol::WebSocketSecure` | `wss://`; **не реализован** — TLS в движке нет |

```cpp
// Значение можно хранить в своих настройках, но движок его не читает.
crossrender::NetProtocol transport = crossrender::NetProtocol::Udp;
switch (transport) {
    case crossrender::NetProtocol::Tcp:           ENG_LOGI("net", "берём TcpSocket"); break;
    case crossrender::NetProtocol::Udp:           ENG_LOGI("net", "берём UdpSocket"); break;
    case crossrender::NetProtocol::WebSocket:     ENG_LOGI("net", "берём WebSocket"); break;
    case crossrender::NetProtocol::WebSocketSecure:
        ENG_LOGW("net", "wss:// не поддерживается: TLS-бэкенда нет");
        break;
}
```

### `enum class SocketState : u8`

Состояние TCP-сокета. Таблица полная — перечислены **все** значения
перечисления.

| Значение | Смысл |
|---|---|
| `SocketState::Closed` | сокет закрыт или ещё не открывался |
| `SocketState::Connecting` | зарезервировано; реализация **никогда** его не выставляет |
| `SocketState::Connected` | соединение установлено (`Connect` или `Accept`) |
| `SocketState::Listening` | сокет слушает порт (`Listen`) |
| `SocketState::Error` | последняя операция провалилась; подробности в `LastError()` |
| `SocketState::Closing` | зарезервировано; реализация **никогда** его не выставляет |

```cpp
crossrender::TcpSocket server;
if (server.Listen(0) && server.State() == crossrender::SocketState::Listening) {
    ENG_LOGI("net", "слушаем %s", server.LocalAddress().ToString().c_str());
} else {
    ENG_LOGE("net", "состояние %d, ошибка: %s", static_cast<int>(server.State()),
             server.LastError().c_str());
}
```

### `struct NetStats`

Снимок счётчиков сокета. Поля заполняются «как есть», без блокировок: это
обычная структура внутри сокета, а `Stats()` возвращает на неё константную
ссылку.

**Честно о содержимом:** реализация заполняет только `bytesSent`,
`bytesReceived`, `packetsSent`, `packetsReceived`, `packetsLost` и
`connectTimeMs`. Поля `rttMs`, `packetLoss`, `sendRateBytesPerSec` и
`receiveRateBytesPerSec` не заполняются никогда — в них всегда `0`, потому что
в движке нет оценщика RTT и скорости.

```cpp
crossrender::TcpSocket sock;
// ... после обмена данными ...
const crossrender::NetStats& s = sock.Stats();
ENG_LOGI("net", "отправлено %llu Б, получено %llu Б, соединение заняло %llu мс",
         static_cast<unsigned long long>(s.bytesSent),
         static_cast<unsigned long long>(s.bytesReceived),
         static_cast<unsigned long long>(s.connectTimeMs));
if (s.rttMs == 0.0f) {
    ENG_LOGD("net", "rttMs всегда 0: RTT реализацией не измеряется");
}
```

### `u64 bytesSent`

Сколько байт сокет отправил за время жизни. `TcpSocket::SendMessage` учитывает
и 4-байтовый префикс длины, и сам payload; `WebSocket` учитывает и байты
рукопожатия.

```cpp
crossrender::TcpSocket client;
if (client.Connect("127.0.0.1", 9000, 1.0f)) {
    const std::vector<crossrender::u8> payload = {'p', 'i', 'n', 'g'};
    client.SendMessage(payload);
    // 4 байта префикса + 4 байта полезной нагрузки
    ENG_LOGI("net", "отправлено %llu байт",
             static_cast<unsigned long long>(client.Stats().bytesSent));
}
```

### `u64 bytesReceived`

Сколько байт сокет принял. У `TcpSocket` и `UdpSocket` это ровно объём
полученных данных, у `WebSocket` — байты, прочитанные из сокета, плюс ещё раз
длина собранного сообщения (так устроен учёт в `HandleFrame`), поэтому на
нативных сборках значение завышено относительно полезной нагрузки.

```cpp
const crossrender::u64 received = udp.Stats().bytesReceived;
ENG_LOGI("net", "принято %llu байт", static_cast<unsigned long long>(received));
```

### `u64 packetsSent`

Счётчик отправок. У TCP это не «пакеты», а число успешных вызовов записи:
один `Send` (и, значит, один `SendMessage`) увеличивает счётчик на единицу,
даже если данные ушли несколькими TCP-сегментами. У UDP — число датаграмм.

```cpp
const std::vector<crossrender::u8> ping = {1};
tcp.SendMessage(ping);
tcp.SendMessage(ping);
ENG_LOGI("net", "вызовов записи: %llu",
         static_cast<unsigned long long>(tcp.Stats().packetsSent));   // 2
```

### `u64 packetsReceived`

Счётчик успешных чтений: каждый `TcpSocket::Receive`/`UdpSocket::ReceiveFrom`,
вернувший данные, увеличивает его на единицу. Значение зависит от того, как
ядро разбило поток на сегменты, поэтому у TCP оно непостоянно.

```cpp
if (udp.ReceiveFrom(&packet)) {
    ENG_LOGI("net", "получено датаграмм: %llu",
             static_cast<unsigned long long>(udp.Stats().packetsReceived));
}
```

### `u64 packetsLost`

Единственная «потеря», которую считает реализация: неудачная отправка
датаграммы из-за переполнения буфера сокета (`EWOULDBLOCK`). В
`UdpSocket::SendTo` и `UdpSocket::Send` такой случай увеличивает счётчик на
единицу и возвращает `false`; в остальных местах поле не меняется.

```cpp
if (!udp.SendTo(target, data.data(), data.size())) {
    // Либо ошибка с текстом в LastError(), либо (при EWOULDBLOCK) просто потеря.
    ENG_LOGW("net", "датаграмма не ушла, потерь: %llu",
             static_cast<unsigned long long>(udp.Stats().packetsLost));
}
```

### `f32 rttMs`

Поле есть, но **всегда `0`**: время кругового обмена (round-trip time)
реализация не измеряет, и ни один метод его не записывает.

```cpp
// Так выглядит честная проверка: значение не несёт информации.
if (socket.Stats().rttMs > 0.0f) {
    ENG_LOGI("net", "RTT %.1f мс", socket.Stats().rttMs);
} else {
    ENG_LOGD("net", "RTT неизвестен — поле rttMs не заполняется");
}
```

### `f32 packetLoss`

Доля потерь. Тоже **всегда `0`**: оценщика потерь нет, а `packetsLost` — это
просто счётчик неудачных отправок, из которого доля не вычисляется.

```cpp
const float loss = tcp.Stats().packetLoss;   // всегда 0.0f
ENG_LOGD("net", "packetLoss=%.3f (не заполняется реализацией)", loss);
```

### `f32 sendRateBytesPerSec`

Скорость отправки. **Всегда `0`** — окна измерения и таймера скорости в
реализации нет.

```cpp
const float rate = ws.Stats().sendRateBytesPerSec;   // всегда 0.0f
ENG_LOGD("net", "скорость отправки не измеряется: %.1f", rate);
```

### `f32 receiveRateBytesPerSec`

Скорость приёма. **Всегда `0`** по той же причине, что и
`sendRateBytesPerSec`.

```cpp
const float rate = udp.Stats().receiveRateBytesPerSec;   // всегда 0.0f
ENG_LOGD("net", "скорость приёма не измеряется: %.1f", rate);
```

### `u64 connectTimeMs`

Сколько миллисекунд занял успешный `TcpSocket::Connect`. Заполняется только там
(и только на настоящих платформах): у сокета, полученного через `Accept`, у
`UdpSocket` и у `WebSocket` поле остаётся `0`.

```cpp
crossrender::TcpSocket client;
if (client.Connect("127.0.0.1", 27015, 2.0f)) {
    ENG_LOGI("net", "установление соединения заняло %llu мс",
             static_cast<unsigned long long>(client.Stats().connectTimeMs));
}
// У сокета из Accept(), у UdpSocket и у WebSocket это поле всегда 0.
```

### `struct NetPacket`

Принятая датаграмма вместе с метаданными: полезная нагрузка, адрес отправителя и
время приёма. Именно это заполняет `UdpSocket::ReceiveFrom`; для TCP такого типа
нет — там поток и кадрированные сообщения.

```cpp
crossrender::NetPacket packet;
if (udp.ReceiveFrom(&packet)) {
    ENG_LOGI("net", "датаграмма %d байт от %s", static_cast<int>(packet.data.size()),
             packet.sender.ToString().c_str());
}
```

### `std::vector<u8> data`

Полезная нагрузка датаграммы ровно в том виде, в каком она пришла. Максимальный
размер ограничен буфером приёма — 65536 байт; всё, что больше, ядро обрежет или
не отправит.

```cpp
crossrender::NetPacket packet;
if (!udp.ReceiveFrom(&packet)) {
    // Очередь пуста или сокет закрыт — поле data не трогается.
} else if (packet.data.size() > 1200) {
    ENG_LOGW("net", "датаграмма %d байт может не пройти через чужой MTU",
             static_cast<int>(packet.data.size()));
}
```

### `NetAddress sender`

Адрес того, кто прислал датаграмму. Порт в нём — настоящий порт отправителя,
поэтому на него можно ответить через `UdpSocket::SendTo`.

```cpp
crossrender::NetPacket packet;
if (udp.ReceiveFrom(&packet)) {
    // Отвечаем тому же пиру.
    const std::string pong = "pong";
    udp.SendTo(packet.sender, pong.data(), pong.size());
}
```

### `f64 receiveTime`

Момент приёма в секундах по монотонным часам процесса (`steady_clock`), а не
календарное время. Годится для разниц и таймаутов, но не для вывода даты.
Заполняется только `UdpSocket::ReceiveFrom`.

```cpp
crossrender::NetPacket packet;
if (udp.ReceiveFrom(&packet) && packet.receiveTime > 0.0) {
    ENG_LOGD("net", "метка приёма: %.3f с (монотонное время)", packet.receiveTime);
}
```

### `int channel`

Канал надёжности — задумывался как подсказка «ненадёжный / надёжный
упорядоченный». **Реализация его не заполняет:** `ReceiveFrom` всегда
обнуляет поле, так что там всегда `0`.

```cpp
crossrender::NetPacket packet;
if (udp.ReceiveFrom(&packet)) {
    ENG_LOGD("net", "канал %d (всегда 0 — поля не заполняются)", packet.channel);
}
```

### `bool reliable`

Флаг надёжной доставки. **Всегда `false`:** слоя переотправки и подтверждений
поверх UDP в движке нет, и `ReceiveFrom` явно сбрасывает поле в `false`.

```cpp
crossrender::NetPacket packet;
if (udp.ReceiveFrom(&packet) && !packet.reliable) {
    // Так и есть всегда: за надёжность эта реализация не отвечает.
    ENG_LOGD("net", "ненадёжная датаграмма, как и любая другая");
}
```

### `class TcpSocket`

TCP-сокет: и клиент, и сервер в одном классе. Объект владеет дескриптором и
закрывает его в деструкторе; копировать нельзя, перемещать можно. Состояние
видно через `State()`, текст последней ошибки — через `LastError()`.

Чтение не блокирует: `Receive` и `Accept` возвращают `false`/`nullptr`, если
данных или соединений нет. Отправка ограниченно ждёт готовности к записи (до
5 секунд), а `Connect` синхронен: он возвращает либо готовое соединение, либо
`false` — состояния `Connecting` не бывает.

На WASM ни один метод, работающий с сетью, не выполняет операцию: они пишут в
`LastError()` текст `"... : native sockets are unsupported on the WASM build"` и
возвращают `false`/`nullptr` (`NetFindFreePort()` там даёт `0`). У `Send`,
`Receive`, `ReceiveMessage` и `Accept` до этой ветки успевает сработать проверка
дескриптора, поэтому на практике вы увидите `"... is not open"` — сокет на WASM
не открывается никогда.

```cpp
// Сервер: слушаем порт и принимаем одно соединение.
crossrender::TcpSocket server;
if (!server.Listen(27015)) {
    ENG_LOGE("net", "порт занят: %s", server.LastError().c_str());
} else {
    std::unique_ptr<crossrender::TcpSocket> peer = server.Accept();
    ENG_LOGI("net", "на прослушивании %d, соединений принято: %d",
             server.State() == crossrender::SocketState::Listening ? 1 : 0, peer ? 1 : 0);
}
```

### `TcpSocket()`

Создаёт объект без сокета: дескриптора ещё нет, `Valid()` возвращает `false`,
`State()` — `Closed`. Внутренняя структура создаётся сразу, поэтому пустой
объект безопасен и его можно перемещать.

```cpp
crossrender::TcpSocket socket;                       // просто значение на стеке
ENG_LOGI("net", "готов к работе: valid=%d, state=%d", socket.Valid() ? 1 : 0,
         static_cast<int>(socket.State()));
```

### `~TcpSocket()`

Деструктор закрывает дескриптор (вызывает `Close()`), если тот был открыт.
Утечек дескрипторов при выходе из области видимости не остаётся.

```cpp
void ServeOnce() {
    crossrender::TcpSocket server;
    server.Listen(0);
    // ... работа ...
}   // здесь дескриптор закроется сам
```

### `TcpSocket(TcpSocket&&) noexcept` / `TcpSocket& operator=(TcpSocket&&) noexcept`

Перемещение переносит дескриптор, счётчики и текст ошибки; источник остаётся
работоспособным пустым объектом (`State() == Closed`, счётчики обнулены), а
присваивание предварительно закрывает то, чем владел приёмник. Поведение
конструктора и оператора одинаковое, поэтому они описаны одним разделом.

* **Контекст:** копирование запрещено, так что сокет можно хранить только в
  `std::unique_ptr`/`std::vector` с перемещением.

```cpp
std::vector<crossrender::TcpSocket> clients;
std::unique_ptr<crossrender::TcpSocket> incoming = server.Accept();
if (incoming) {
    crossrender::TcpSocket owned = std::move(*incoming);   // дескриптор переехал из unique_ptr
    clients.push_back(std::move(owned));           // ... и дальше в вектор
}
```

### `bool Listen(u16 port, int backlog = 8)`

Открывает слушающий сокет на всех интерфейсах с `SO_REUSEADDR`. Порт `0`
означает «любой свободный» — реальный номер потом сообщает `LocalAddress()`.
Перед открытием старый сокет закрывается.

* **Возвращает:** `true` при успехе; иначе `false`, состояние `Error`, причина в
  `LastError()` (например, `bind(...): Address already in use`).
* **Параметры:** `backlog` — размер очереди непринятых соединений; значения
  `<= 0` заменяются на 8.
* **Контекст:** сразу после успеха `State() == Listening`, `Valid() == true`.

```cpp
crossrender::TcpSocket server;
if (!server.Listen(0, 16)) {
    ENG_LOGE("net", "не удалось открыть порт: %s", server.LastError().c_str());
    return;
}
// Порт 0 попросил у ОС свободный номер — узнаём, какой выдали.
const crossrender::u16 port = server.LocalAddress().port;
ENG_LOGI("net", "слушаем порт %u", static_cast<unsigned>(port));
```

### `bool Connect(const std::string& host, u16 port, f32 timeoutSeconds = 5.0f)`

Подключается к серверу. Имя хоста разрешается через `NetAddress::Parse` (то
есть работает и литеральный IPv4, и DNS на настоящих платформах). Вызов
синхронный: внутри ограниченный цикл ожидания готовности к записи, поэтому
`Connecting` в состоянии не появится — сразу `Connected` или `Error`.
При успехе включается `TCP_NODELAY` и заполняется `NetStats::connectTimeMs`.

* **Возвращает:** `true`, если соединение установлено.
* **Параметры:** `timeoutSeconds` — предел ожидания; значения `<= 0`
  заменяются на 5 секунд.
* **Ограничение:** блокирует поток до `timeoutSeconds`; для отзывчивого
  интерфейса подключайтесь вне кадра отрисовки.

```cpp
crossrender::TcpSocket client;
if (!client.Connect("127.0.0.1", 27015, 2.0f)) {
    ENG_LOGW("net", "сервер недоступен: %s", client.LastError().c_str());
} else {
    ENG_LOGI("net", "подключились к %s", client.RemoteAddress().ToString().c_str());
}
```

### `std::unique_ptr<TcpSocket> Accept()`

Забирает одно готовое входящее соединение у слушающего сокета. Вызов
неблокирующий: если очередь пуста, возвращается `nullptr` — это нормальная
ситуация, а не ошибка.

* **Возвращает:** новый сокет в состоянии `Connected` (им владеет вызывающий)
  либо `nullptr`, если соединений нет или сокет не слушает.
* **Контекст:** у принятого сокета тоже включён `TCP_NODELAY`; его
  `RemoteAddress()` — адрес клиента.
* **Замечание:** в `tests/test_net.cpp` готовность соединения ждут циклом
  `WaitUntil(..., 2.0)` с паузами по 2 мс; на загруженной машине такой тест
  может не успеть и «мигнуть» — это чувствительность теста, а не сети.

```cpp
// Вызывайте Accept() в цикле кадра, пока он возвращает сокеты.
while (std::unique_ptr<crossrender::TcpSocket> peer = server.Accept()) {
    ENG_LOGI("net", "подключился %s", peer->RemoteAddress().ToString().c_str());
    clients.push_back(std::move(peer));
}
```

### `bool Send(const void* data, usize size)`

Пишет в сокет `size` байт из `data`. Отправка не «выстреливает и забывает»:
если буфер сокета заполнен, вызов ждёт готовности к записи до 5 секунд
(ограниченный цикл `poll`), поэтому вопреки комментарию в заголовке метод
не является строго неблокирующим.

* **Возвращает:** `true`, если отправлены все байты; при этом `bytesSent`
  увеличивается на `size`, а `packetsSent` — на 1.
* **Возвращает:** `false` при закрытом сокете, `data == nullptr` при `size > 0`
  или при ошибке/таймауте; `size == 0` даёт `true`, ничего не отправляя.
* **Контекст:** `error_` не очищается перед вызовом — старый текст может
  остаться, если операция просто не удалась.

```cpp
const std::string line = "ping\n";
if (!client.Send(line.data(), line.size())) {
    ENG_LOGE("net", "не отправилось: %s", client.LastError().c_str());
}
```

### `bool Receive(std::vector<u8>* out, usize maxBytes = 64 * 1024)`

Читает то, что уже пришло, и ничего не ждёт: при пустом буфере сокета
возвращается `false` (при этом `LastError()` остаётся прежним, а не получает
новый текст). Буфер `out` предварительно очищается, поэтому при `false` он
пуст.

* **Возвращает:** `true` и данные в `out`, если что-то прочитано; при этом
  `bytesReceived` растёт на число байт, а `packetsReceived` — на 1.
* **Возвращает:** `false`, если данных нет, сокет закрыт или `maxBytes == 0`.
* **Контекст:** если peer закрыл соединение (`recv` вернул 0), состояние
  становится `Closed`, а в `LastError()` попадает `"peer closed the connection"`.
* **Ограничение:** при настоящей ошибке чтения состояние становится `Error`.

```cpp
// Цикл кадра: вычитываем всё, что успело прийти, и не ждём.
std::vector<crossrender::u8> chunk;
while (client.Receive(&chunk)) {
    ProcessBytes(chunk.data(), chunk.size());
}
```

### `bool SendMessage(const std::vector<u8>& payload)`

Отправляет сообщение с 4-байтовым префиксом длины в порядке little-endian
(младший байт первым), за которым идёт сама полезная нагрузка. Это кадрирование
общее для TCP и WebSocket-сообщений движка: принимающая сторона читает
`ReceiveMessage`.

* **Возвращает:** `true`, если ушли и префикс, и нагрузка.
* **Контекст:** пустая нагрузка отправляет только 4 байта заголовка.
* **Ограничение:** длина берётся по модулю 2^32 — сообщение больше 4 ГиБ
  отправится с неверным префиксом.

```cpp
const std::vector<crossrender::u8> payload = {1, 2, 3, 4, 5};
if (!client.SendMessage(payload)) {
    ENG_LOGE("net", "сообщение не ушло: %s", client.LastError().c_str());
}
```

### `bool ReceiveMessage(std::vector<u8>* payload)`

Собирает одно кадрированное сообщение из потока. Метод вычитывает всё, что
готово (`Receive`), копит байты во внутреннем буфере и отдаёт сообщение только
целиком; `payload` очищается в начале.

* **Возвращает:** `true`, когда накопился префикс и полная нагрузка.
* **Возвращает:** `false`, если сообщение ещё не собрано (это нормальное
  состояние ожидания), сокет закрыт или префикс объявил больше 64 МиБ.
* **Ограничение:** потолок одного сообщения — 64 МиБ; при его превышении в
  `LastError()` пишется причина и сокет **закрывается**.
* **Замечание:** тесты ждут сообщение циклом `WaitUntil(..., 2.0)` с паузами
  2 мс — на загруженной машине это может не успеть сработать.

```cpp
std::vector<crossrender::u8> message;
// Вызывайте в цикле кадра, пока метод возвращает true.
while (client.ReceiveMessage(&message)) {
    HandleMessage(message);
}
```

### `void Close()`

Закрывает дескриптор, очищает внутренний буфер сообщений и переводит состояние
в `Closed`. Повторный вызов безопасен. Состояние `Error` не перезаписывается —
диагностика последней неудачи сохраняется.

* **Контекст:** после `Close()` `Valid()` возвращает `false`, а повторное
  `Send`/`Receive` дают ошибку `"... is not open"`.

```cpp
client.Close();
ENG_LOGI("net", "сокет закрыт, valid=%d", client.Valid() ? 1 : 0);
```

### `void SetNoDelay(bool enable)`

Включает или выключает `TCP_NODELAY` — запрет на буферизацию мелких отправок
(алгоритм Нейгла). Для игровых сообщений обычно включают: задержка важнее
экономии трафика. `Connect` и `Accept` включают его сами.

* **Контекст:** метод ничего не возвращает; при неудаче текст попадает в
  `LastError()`, а состояние не меняется.
* **Ограничение:** на неподключённом сокете в `LastError()` окажется
  `"SetNoDelay: socket is not open"`.

```cpp
client.SetNoDelay(true);
if (!client.LastError().empty()) {
    ENG_LOGW("net", "TCP_NODELAY не применился: %s", client.LastError().c_str());
}
```

### `void SetNonBlocking(bool enable)`

Управляет неблокирующим режимом дескриптора. Все сокеты и так создаются
неблокирующими, поэтому метод нужен редко — например, чтобы временно вернуть
блокирующее чтение для простого синхронного кода.

* **Контекст:** ошибка не бросается, а попадает в `LastError()`
  (`"fcntl(O_NONBLOCK): ..."` или эквивалент Winsock).

```cpp
client.SetNonBlocking(true);
const bool ok = client.LastError().empty();
ENG_LOGI("net", "неблокирующий режим: %s", ok ? "включён" : client.LastError().c_str());
```

### `void SetKeepAlive(bool enable)`

Включает или выключает `SO_KEEPALIVE` — периодические проверки живости
соединения на уровне ядра. Полезно для долгих соединений, которые могут
«повиснуть» без трафика.

* **Контекст:** параметры интервалов ядра метод не настраивает, только включает
  механизм; ошибка попадает в `LastError()`.

```cpp
// Долгий матч: пусть ядро само заметит мёртвое соединение.
client.SetKeepAlive(true);
ENG_LOGI("net", "keep-alive: %s", client.LastError().empty() ? "включён" : "ошибка");
```

### `bool Valid() const`

Проверяет, что сокет открыт и не находится в состоянии `Closed`/`Error`. То
есть `true` и для `Listening`, и для `Connected`.

* **Возвращает:** `true`, если дескриптор существует и состояние рабочее.

```cpp
if (!server.Valid()) {
    ENG_LOGW("net", "сокет непригоден: %s", server.LastError().c_str());
}
```

### `SocketState State() const`

Текущее состояние сокета (см. таблицу `SocketState`). У перемещённого объекта
возвращает `Closed`.

```cpp
switch (client.State()) {
    case crossrender::SocketState::Connected: ENG_LOGI("net", "канал открыт"); break;
    case crossrender::SocketState::Error:     ENG_LOGE("net", "%s", client.LastError().c_str()); break;
    case crossrender::SocketState::Closed:    ENG_LOGI("net", "канал закрыт"); break;
    default:                          ENG_LOGD("net", "переходное состояние"); break;
}
```

### `NetAddress RemoteAddress() const`

Адрес второй стороны соединения. У слушающего сокета и у пустого объекта там
`0.0.0.0:0`.

* **Возвращает:** адрес пира или нулевой адрес, если его нет.
* **Контекст:** заполняется в `Connect` (для клиента) и в `Accept` (для
  принятого сокета).

```cpp
ENG_LOGI("net", "клиент пришёл с %s", peer->RemoteAddress().ToString().c_str());
```

### `NetAddress LocalAddress() const`

Адрес и порт этого конца соединения. У слушающего сокета это единственный
способ узнать, какой порт выдала ОС при `Listen(0)`.

* **Возвращает:** локальный адрес или нулевой адрес, если сокет не открыт.

```cpp
crossrender::TcpSocket server;
server.Listen(0);
// Порт, который на самом деле заняли, виден только здесь.
const crossrender::u16 actual = server.LocalAddress().port;
ENG_LOGI("net", "сервер доступен на %s", server.LocalAddress().ToString().c_str());
```

### `const NetStats& Stats() const`

Счётчики сокета по ссылке — читайте их в любой момент, копия не нужна. Значения
меняются по мере операций; см. `struct NetStats` о том, какие поля вообще
заполняются.

* **Возвращает:** константную ссылку на живую структуру внутри сокета.

```cpp
const crossrender::NetStats& s = client.Stats();
ENG_LOGI("net", "итог: %llu байт отправлено, %llu принято",
         static_cast<unsigned long long>(s.bytesSent),
         static_cast<unsigned long long>(s.bytesReceived));
```

### `const std::string& LastError() const`

Текст последней ошибки. Строка непустая после неудачи; успешные операции её
не всегда очищают, поэтому ориентируйтесь на код возврата, а текст читайте как
пояснение.

* **Возвращает:** константную ссылку на строку ошибки (пустую, если ошибок не
  было).

```cpp
if (!client.Connect("127.0.0.1", 27015, 1.0f)) {
    ENG_LOGE("net", "подключение не удалось: %s", client.LastError().c_str());
}
```

### `class UdpSocket`

UDP-сокет: датаграммы без соединения и без гарантий доставки. После `Bind`
сокет принимает датаграммы на любом интерфейсе, `Connect` назначает пира по
умолчанию (после этого работает `Send`), а `SendTo` шлёт по произвольному
адресу.

Копировать нельзя, перемещать можно. Чтение неблокирующее: `ReceiveFrom`
возвращает `false`, когда очередь пуста. На WASM все операции с сетью —
заглушки с текстом про неподдерживаемые нативные сокеты в `LastError()`.

```cpp
// Приём датаграмм: Bind + цикл кадра.
crossrender::UdpSocket udp;
if (!udp.Bind(27016)) {
    ENG_LOGE("net", "UDP-порт не занят: %s", udp.LastError().c_str());
    return;
}
crossrender::NetPacket packet;
while (udp.ReceiveFrom(&packet)) {
    ENG_LOGI("net", "%d байт от %s", static_cast<int>(packet.data.size()),
             packet.sender.ToString().c_str());
}
```

### `UdpSocket()`

Создаёт объект без сокета: дескриптора нет, `Valid()` возвращает `false`.
Сокет появляется только после `Bind` или `Connect`.

```cpp
crossrender::UdpSocket udp;
ENG_LOGI("net", "до Bind: valid=%d", udp.Valid() ? 1 : 0);
```

### `~UdpSocket()`

Деструктор закрывает дескриптор, если он был открыт (`Close()`), — ручное
закрытие не обязательно, но полезно, чтобы освободить порт раньше.

```cpp
void ProbePort() {
    crossrender::UdpSocket udp;
    udp.Bind(0);   // ... используем ...
}   // дескриптор закрыт, порт освобождён
```

### `UdpSocket(UdpSocket&&) noexcept` / `UdpSocket& operator=(UdpSocket&&) noexcept`

Перемещение переносит дескриптор, пира по умолчанию, счётчики и текст ошибки;
источник становится пустым, но пригодным к использованию, а приёмник перед
присваиванием закрывает свой прежний сокет. Оба метода ведут себя одинаково,
поэтому описаны вместе.

```cpp
std::vector<crossrender::UdpSocket> peers;
crossrender::UdpSocket tmp;
tmp.Bind(0);
peers.push_back(std::move(tmp));   // сокет переехал в вектор
ENG_LOGI("net", "сокетов в векторе: %d", static_cast<int>(peers.size()));
```

### `bool Bind(u16 port)`

Открывает датаграммный сокет и привязывает его ко всем интерфейсам
(`0.0.0.0`) с `SO_REUSEADDR`. Порт `0` — «любой свободный», реальный номер
сообщает `LocalAddress()`. Прежний сокет перед этим закрывается.

* **Возвращает:** `true` при успехе; иначе `false` и текст в `LastError()`
  (например, `bind(27016): Address already in use`).
* **Контекст:** после успеха `Valid() == true`, `Stats()` начинают считать.

```cpp
crossrender::UdpSocket udp;
if (!udp.Bind(0)) {
    ENG_LOGE("net", "UDP не поднялся: %s", udp.LastError().c_str());
} else {
    ENG_LOGI("net", "слушаем UDP-порт %u", static_cast<unsigned>(udp.LocalAddress().port));
}
```

### `bool Connect(const std::string& host, u16 port)`

Назначает пира по умолчанию: после этого `Send` отправляет датаграммы именно
ему, а ядро отфильтровывает датаграммы от других адресов. Если сокет ещё не
открыт, сначала выполняется `Bind(0)`.

* **Возвращает:** `true`, если адрес разрешился и `connect` прошёл.
* **Ограничение:** это не «соединение» в смысле TCP: никаких рукопожатий не
  происходит, ошибка доставки позже не сообщается.
* **Ограничение:** при неразрешённом имени в `LastError()` окажется
  `"could not resolve host '...'"`.

```cpp
crossrender::UdpSocket client;
if (!client.Connect("127.0.0.1", 27016)) {
    ENG_LOGW("net", "пир не назначен: %s", client.LastError().c_str());
} else if (client.Send("ping", 4)) {
    ENG_LOGI("net", "датаграмма ушла");
}
```

### `bool SendTo(const NetAddress& addr, const void* data, usize size)`

Отправляет одну датаграмму по указанному адресу. Сокет при этом не обязан быть
«подключённым».

* **Возвращает:** `true`, если датаграмма принята ядром; при этом `bytesSent`
  растёт на `size`, а `packetsSent` — на 1.
* **Возвращает:** `false` при закрытом сокете, `data == nullptr` при
  `size > 0` или при ошибке; отправка нулевой длины формально успешна.
* **Контекст:** если буфер сокета переполнен (`EWOULDBLOCK`), метод возвращает
  `false`, увеличивает `packetsLost` и **не** пишет текст ошибки — это потеря,
  а не сбой. Далее в `LastError()` появится настоящая причина, если она была.

```cpp
const crossrender::NetAddress target = crossrender::NetAddress::Parse("192.168.1.255", 27016);
udp.SetBroadcast(true);
const std::string hello = "hello";
if (!udp.SendTo(target, hello.data(), hello.size())) {
    ENG_LOGW("net", "широковещательная датаграмма не ушла");
}
```

### `bool Send(const void* data, usize size)`

Отправляет датаграмму пиру, назначенному через `Connect`.

* **Возвращает:** `false` с текстом
  `"UdpSocket::Send: no default peer (call Connect first)"`, если `Connect` не
  вызывался; иначе ведёт себя как `SendTo` (счётчики, `packetsLost` при
  `EWOULDBLOCK`).
* **Контекст:** внутри используется `send()`, а не `sendto()`: на подключённом
  датаграммном сокете `sendto` с адресом вернул бы `EISCONN`.

```cpp
crossrender::UdpSocket client;
client.Connect("127.0.0.1", 27016);
if (!client.Send("ping", 4)) {
    ENG_LOGE("net", "не отправилось: %s", client.LastError().c_str());
}
```

### `bool ReceiveFrom(NetPacket* packet)`

Принимает одну датаграмму, если она уже в очереди, и заполняет `packet`. Вызов
не блокирующий: пустая очередь — это `false`, а не ожидание.

* **Возвращает:** `true` и заполненный `packet` (`data`, `sender`,
  `receiveTime`); при этом `bytesReceived` растёт на число байт, а
  `packetsReceived` — на 1.
* **Возвращает:** `false` при пустой очереди, закрытом сокете или ошибке; при
  этом `packet` обнуляется в начале вызова.
* **Ограничение:** буфер приёма — 65536 байт; более крупная датаграмма будет
  обрезана. Поля `channel` и `reliable` всегда остаются `0` и `false`.

```cpp
crossrender::NetPacket packet;
while (udp.ReceiveFrom(&packet)) {
    // Обрабатываем, пока в очереди есть датаграммы.
    OnDatagram(packet.data, packet.sender);
}
```

### `void Close()`

Закрывает дескриптор и сбрасывает пира по умолчанию. Повторный вызов
безопасен; после закрытия `Valid()` возвращает `false`.

```cpp
udp.Close();
ENG_LOGI("net", "UDP-сокет закрыт: valid=%d", udp.Valid() ? 1 : 0);
```

### `void SetBroadcast(bool enable)`

Включает или выключает `SO_BROADCAST` — разрешение слать датаграммы на
широковещательный адрес подсети (например, `192.168.1.255`).

* **Контекст:** без этого флага `SendTo` на широковещательный адрес завершается
  ошибкой; сам метод ошибку не бросает, а пишет её в `LastError()`.

```cpp
udp.SetBroadcast(true);
if (!udp.LastError().empty()) {
    ENG_LOGW("net", "широковещание недоступно: %s", udp.LastError().c_str());
}
```

### `void SetNonBlocking(bool enable)`

Управляет неблокирующим режимом дескриптора. Сокеты и так создаются
неблокирующими, поэтому метод нужен только для нестандартных сценариев.

* **Контекст:** при неудаче (`"SetNonBlocking: socket is not open"` или текст
  системной ошибки) причина попадает в `LastError()`.

```cpp
udp.SetNonBlocking(true);
ENG_LOGI("net", "неблокирующий UDP: %s", udp.LastError().empty() ? "да" : "нет");
```

### `bool Valid() const`

Проверяет, что дескриптор открыт. В отличие от `TcpSocket::Valid`, состояний
нет — есть только «открыт/закрыт».

```cpp
if (!udp.Valid()) {
    ENG_LOGW("net", "UDP-сокет не открыт: %s", udp.LastError().c_str());
}
```

### `NetAddress LocalAddress() const`

Локальный адрес и порт сокета. До `Bind`/`Connect` возвращает `0.0.0.0:0`.

```cpp
ENG_LOGI("net", "наши датаграммы ждут на %s", udp.LocalAddress().ToString().c_str());
```

### `const NetStats& Stats() const`

Живые счётчики датаграмм: `bytesSent`, `packetsSent`, `bytesReceived`,
`packetsReceived` и `packetsLost` заполняются, поля RTT и скоростей — никогда
(см. `struct NetStats`).

```cpp
const crossrender::NetStats& s = udp.Stats();
ENG_LOGI("net", "ушло %llu датаграмм, потеряно %llu",
         static_cast<unsigned long long>(s.packetsSent),
         static_cast<unsigned long long>(s.packetsLost));
```

### `const std::string& LastError() const`

Текст последней ошибки. Обратите внимание: неудачная отправка из-за переполнения
буфера (`EWOULDBLOCK`) ошибкой не считается и текста не оставляет — только
увеличивает `packetsLost`.

```cpp
if (!udp.SendTo(target, data.data(), data.size())) {
    if (!udp.LastError().empty()) {
        ENG_LOGE("net", "UDP: %s", udp.LastError().c_str());
    } else {
        ENG_LOGD("net", "потеря из-за переполнения буфера отправки");
    }
}
```

### `enum class WsState : u8`

Состояние клиента WebSocket. Таблица полная — перечислены **все** значения
перечисления.

| Значение | Смысл |
|---|---|
| `WsState::Closed` | соединение закрыто или ещё не открывалось |
| `WsState::Connecting` | рукопожатие отправлено/начато, ответа ещё нет |
| `WsState::Open` | рукопожатие принято, кадры можно слать и принимать |
| `WsState::Closing` | кадр закрытия отправлен; состояние кратковременное |
| `WsState::Error` | протокольная ошибка, отказ рукопожатия или обрыв |

```cpp
crossrender::WebSocket ws;
ws.Connect("ws://127.0.0.1:9001/chat");
ENG_LOGI("net", "до первого Poll(): %d (Connecting)",
         static_cast<int>(ws.State()) == static_cast<int>(crossrender::WsState::Connecting) ? 1 : 0);
```

### `struct WsCallbacks`

Набор обработчиков событий WebSocket. Все поля — `std::function`, любое можно
оставить пустым; колбэки вызываются **изнутри** `Connect` (ошибка) и `Poll`
(открытие, сообщения, закрытие), то есть в том же потоке, который их вызывает.

```cpp
crossrender::WsCallbacks callbacks;
callbacks.onOpen   = [] { ENG_LOGI("net", "соединение открыто"); };
callbacks.onError  = [](const std::string& e) { ENG_LOGE("net", "ошибка: %s", e.c_str()); };
// Остальные обработчики не заданы — события просто не будут замечены.
crossrender::WebSocket ws;
ws.Connect("ws://127.0.0.1:9001/chat", callbacks);
```

### `std::function<void()> onOpen`

Вызывается один раз, когда рукопожатие принято и состояние стало `Open`.
Никаких аргументов нет — URL и согласованный подпротокол доступны через
`WebSocket::Url()` и `WebSocket::Protocol()`.

```cpp
crossrender::WebSocket ws;
crossrender::WsCallbacks cb;
cb.onOpen = [&ws] {
    ENG_LOGI("net", "открыт %s, протокол '%s'", ws.Url().c_str(), ws.Protocol().c_str());
};
```

### `std::function<void(const std::string&)> onText`

Текстовый кадр (opcode `0x1`), собранный из фрагментов целиком. Данные приходят
как UTF-8 в `std::string`.

```cpp
crossrender::WsCallbacks cb;
cb.onText = [](const std::string& json) {
    ENG_LOGI("net", "пришёл текст: %s", json.c_str());
};
```

### `std::function<void(const std::vector<u8>&)> onBinary`

Бинарный кадр (opcode `0x2`) целиком, включая склеенные фрагменты
(continuation-кадры). Строка от бинарных данных отличается только opcode.

```cpp
crossrender::WsCallbacks cb;
cb.onBinary = [](const std::vector<crossrender::u8>& payload) {
    ENG_LOGI("net", "пришло %d байт", static_cast<int>(payload.size()));
};
```

### `std::function<void(u16, const std::string&)> onClose`

Закрытие соединения: первым аргументом код (по RFC 6455; `1000` — нормальное
закрытие, `1005` — «код не получен», `1006` — обрыв без кадра закрытия),
вторым — причина. Вызывается не более одного раза за соединение.

```cpp
crossrender::WsCallbacks cb;
cb.onClose = [](crossrender::u16 code, const std::string& reason) {
    ENG_LOGW("net", "закрыто: код %u, причина '%s'", static_cast<unsigned>(code), reason.c_str());
};
```

### `std::function<void(const std::string&)> onError`

Ошибка: отказ рукопожатия, некорректный кадр, обрыв чтения. Сразу после
колбэка состояние — `WsState::Error`, а тот же текст лежит в
`WebSocket::LastError()`.

```cpp
crossrender::WsCallbacks cb;
cb.onError = [](const std::string& message) {
    ENG_LOGE("net", "веб-сокет сломался: %s", message.c_str());
};
```

### `class WebSocket`

Клиент WebSocket по RFC 6455. Класс **не копируется и не перемещается** —
создавайте его на месте или во владеющем `std::unique_ptr`. Один экземпляр
обслуживает одно соединение.

Реализация настоящая на всех платформах, но разными путями. На нативных
сборках это рукопожатие и кадрирование, написанные вручную поверх обычного
TCP-сокета (SHA-1 и base64 тоже реализованы в `Net.cpp`). На WASM класс
делегирует браузерному `WebSocket` через JS-прослойку: события открытия,
сообщений, закрытия и ошибок приходят в очередь и разбираются в `Poll()`.

Ключевое отличие от `TcpSocket`: рукопожатие завершается не в `Connect`, а в
`Poll()`, поэтому **`Poll()` нужно вызывать раз в кадр**, иначе колбэки не
сработают. TLS нет: `wss://` отклоняется сразу.

```cpp
crossrender::WebSocket ws;
crossrender::WsCallbacks cb;
cb.onText = [](const std::string& text) { ENG_LOGI("net", "сервер: %s", text.c_str()); };
if (!ws.Connect("ws://127.0.0.1:9001/lobby", cb)) {
    ENG_LOGW("net", "веб-сокет не поднялся: %s", ws.LastError().c_str());
}
// Дальше — каждый кадр:
if (ws.IsOpen()) {
    ws.SendText("ping");
    ws.Poll();
}
```

### `WebSocket()`

Создаёт клиент в состоянии `Closed` без соединения. Колбэки задаются только
через `Connect`, поэтому до него события некуда доставлять; пустой объект
безопасно удалять и закрывать.

```cpp
crossrender::WebSocket ws;
ENG_LOGI("net", "url пуст: %d, открыт: %d", ws.Url().empty() ? 1 : 0, ws.IsOpen() ? 1 : 0);
```

### `~WebSocket()`

Деструктор закрывает транспорт: на нативных сборках — дескриптор TCP, на
WASM — браузерный сокет через JS-прослойку. Кадр закрытия при этом не
отправляется, `onClose` не вызывается — для вежливого прощания нужен явный
`Close()`.

```cpp
void ConnectTemporarily() {
    crossrender::WebSocket ws;
    ws.Connect("ws://127.0.0.1:9001/probe");
    ws.Poll();
}   // транспорт закроется автоматически, без кадра закрытия
```

### `bool Connect(const std::string& url, const WsCallbacks& callbacks = {}, const std::vector<std::string>& protocols = {})`

Начинает соединение: запоминает URL и колбэки, сбрасывает счётчики и
внутренние буферы. На нативных сборках сразу открывается TCP, отправляется
HTTP-запрос на переключение протокола (`Sec-WebSocket-Key`,
`Sec-WebSocket-Version: 13` и, если заданы, `Sec-WebSocket-Protocol`), и
возвращается `true` **до** получения ответа — состояние `Connecting`.
На WASM вызывается `eng_js_ws_create`, и состояние тоже сразу `Connecting`.

* **Возвращает:** `false` при мгновенной неудаче: `wss://` (нет TLS-бэкенда),
  URL не с `ws://`, пустой хост, ошибка TCP-подключения, отказ браузера создать
  сокет. В этом случае состояние `Error`, `LastError()` заполнен, а `onError`
  уже вызван.
* **Параметры:** `protocols` — список подпротоколов; на нативных сборках
  согласованный подпротокол потом виден в `Protocol()`, на WASM это поле
  остаётся пустым.
* **Ограничение:** `Connect` не ждёт рукопожатия — его доводит `Poll()`.
* **Ограничение:** на WASM `onClose`/`onError`/`onText` доставляются только
  через `Poll()`; пропуск вызова означает потерянные события.

```cpp
crossrender::WebSocket ws;
crossrender::WsCallbacks cb;
cb.onError = [](const std::string& e) { ENG_LOGW("net", "не подключились: %s", e.c_str()); };
if (!ws.Connect("wss://secure.example.com/chat", cb)) {
    // Так и есть: защищённая схема не поддерживается, ошибка содержит "no TLS backend".
    ENG_LOGW("net", "%s", ws.LastError().c_str());
}
```

### `void Poll()`

Продвигает протокол. На нативных сборках читает сокет, доводит рукопожатие,
разбирает кадры (текст, бинарные данные, склейку фрагментов, ping → pong,
закрытие) и вызывает колбэки. На WASM выгружает очередь событий из JS-прослойки,
а если событие открытия потерялось — дополнительно спрашивает состояние у
браузера. Вызов безопасен в любом состоянии, в том числе после закрытия.

* **Контекст:** вызывайте раз в кадр, пока соединение нужно.
* **Контекст:** если peer закрыл TCP без кадра закрытия, состояние станет
  `Closed`, а `onClose` получит код `1006` и причину `"connection closed"`.
* **Ограничение:** некорректный кадр переводит соединение в `Error` и вызывает
  `onError`.

```cpp
// Игровой цикл: одна строка на кадр обслуживает весь веб-сокет.
void Update(crossrender::WebSocket& ws) {
    ws.Poll();
    if (ws.IsOpen()) {
        ws.SendText("tick");
    }
}
```

### `bool SendText(const std::string& text)`

Отправляет текстовый кадр (opcode `0x1`). Строка уходит как есть, поэтому
передавайте корректный UTF-8.

* **Возвращает:** `false` с текстом `"WebSocket::SendText: socket is not open"`,
  если состояние не `Open`.
* **Ограничение:** `true` означает «кадр передан транспорту», а не «запись
  удалась»: результат нижележащей отправки не проверяется. Если важна
  доставка, смотрите `LastError()` и `Stats()`.
* **Ограничение:** на WASM прослойка молча ничего не делает, если браузерный
  сокет не в состоянии `OPEN`, — метод всё равно вернёт `true`.

```cpp
if (ws.IsOpen() && !ws.SendText("{\"type\":\"hello\"}")) {
    ENG_LOGE("net", "текст не ушёл: %s", ws.LastError().c_str());
}
```

### `bool SendBinary(const void* data, usize size)`

Отправляет бинарный кадр (opcode `0x2`). Основной путь для игровых сообщений:
сериализованный пакет, снапшот, аудио-чанк.

* **Возвращает:** `false`, если соединение не открыто или `data == nullptr`
  при `size > 0`.
* **Ограничение:** как и у `SendText`, успех означает лишь передачу транспорту,
  а не подтверждённую запись; на WASM данные копируются в отдельный буфер,
  потому что движок переиспользует свой.

```cpp
const std::vector<crossrender::u8> snapshot = BuildSnapshot();
if (!ws.SendBinary(snapshot.data(), snapshot.size())) {
    ENG_LOGW("net", "снапшот не ушёл: %s", ws.LastError().c_str());
}
```

### `bool SendPing()`

Отправляет служебный ping-кадр. Ответный pong приходит в `Poll()` и наружу не
выносится — колбэка для него нет, так что метод годится только для поддержания
соединения.

* **Возвращает:** `false`, если соединение не открыто.
* **Ограничение:** **на WASM всегда `false`**: браузерный API не умеет
  отправлять ping, и в `LastError()` появляется
  `"... the browser WebSocket API cannot send ping frames"`. Переносимым
  кодом пинг не сделать.

```cpp
if (!ws.SendPing()) {
    // На Web это ожидаемо и не является ошибкой приложения.
    ENG_LOGD("net", "ping недоступен: %s", ws.LastError().c_str());
}
```

### `void Close(u16 code = 1000, const std::string& reason = "")`

Закрывает соединение: если оно было `Open`/`Connecting`, отправляется кадр
закрытия с кодом и причиной, затем транспорт освобождается, состояние проходит
через `Closing` и сразу становится `Closed`, и один раз вызывается `onClose`.
Повторный вызов безвреден и колбэк не дублирует.

* **Параметры:** по умолчанию `code = 1000` («нормальное закрытие») и пустая
  причина.
* **Ограничение:** метод не ждёт ответного кадра закрытия от сервера —
  состояние `Closed` наступает сразу.
* **Ограничение:** на WASM причина обрезается до 120 символов (браузер
  отвергает причины длиннее 123 байт).

```cpp
// Уходим по-хорошему: сервер увидит код 1000 и причину.
ws.Close(1000, "match over");
ENG_LOGI("net", "состояние после Close: %d (Closed)",
         static_cast<int>(ws.State()) == static_cast<int>(crossrender::WsState::Closed) ? 1 : 0);
```

### `WsState State() const`

Текущее состояние (см. таблицу `WsState`). После `Close` — `Closed`, после
ошибки — `Error`.

```cpp
if (ws.State() == crossrender::WsState::Error) {
    ENG_LOGE("net", "веб-сокет в ошибке: %s", ws.LastError().c_str());
}
```

### `bool IsOpen() const`

Короткая проверка «состояние `Open`». Именно ею стоит обрамлять отправку.

```cpp
if (ws.IsOpen()) {
    ws.SendText("ready");
} else {
    ENG_LOGD("net", "ещё не открыт, сообщение пропущено");
}
```

### `const std::string& Url() const`

URL, переданный в `Connect`. До подключения и после неудачного `Connect` строка
пустая или содержит последний запрошенный адрес.

```cpp
ENG_LOGI("net", "текущий веб-сокет: %s", ws.Url().empty() ? "(нет)" : ws.Url().c_str());
```

### `const NetStats& Stats() const`

Счётчики веб-сокета. `Connect` их обнуляет, дальше растут `bytesSent`
(включая байты рукопожатия) и `bytesReceived`. Осторожно: на нативных сборках
`bytesReceived` учитывает и сырые байты сокета, и ещё раз длину собранного
сообщения, поэтому значение завышено; поля RTT и скоростей всегда нулевые.

```cpp
const crossrender::NetStats& s = ws.Stats();
ENG_LOGI("net", "веб-сокет: %llu Б отправлено, %llu Б принято",
         static_cast<unsigned long long>(s.bytesSent),
         static_cast<unsigned long long>(s.bytesReceived));
```

### `const std::string& LastError() const`

Текст последней ошибки. Заполняется при отказе `Connect`, ошибке рукопожатия,
некорректном кадре и обрыве. Успешное рукопожатие очищает строку.

```cpp
ws.Poll();
if (!ws.LastError().empty()) {
    ENG_LOGW("net", "диагностика веб-сокета: %s", ws.LastError().c_str());
}
```

### `const std::string& Protocol() const`

Согласованный подпротокол из ответа сервера, если он был предложен в
`Connect(..., protocols)` и сервер его выбрал.

* **Возвращает:** имя подпротокола или пустую строку, если согласования не
  было.
* **Ограничение:** заполняется только на нативных сборках (разбор заголовка
  `Sec-WebSocket-Protocol`); на WASM всегда пусто.

```cpp
if (!ws.Protocol().empty()) {
    ENG_LOGI("net", "сервер выбрал подпротокол '%s'", ws.Protocol().c_str());
} else {
    ENG_LOGD("net", "подпротокол не согласован");
}
```

### `void InjectForTest(const void* data, usize size)`

**Тестовая защёлка — в игровом коде не использовать.** Метод подкладывает
сырые байты во входной буфер так, будто они пришли из сокета. Если транспорта
нет (состояние не `Open`/`Connecting`), он дополнительно переводит объект в
`Connecting` и включает синтетическое рукопожатие с эталонным ключом из
RFC 6455 — тогда ответ сервера можно записать в тесте строкой и проверить
разбор кадров без сети. Именно так устроены тесты `Net.WebSocketInjected` и
`Net.WebSocketCloseFrame`.

* **Контекст:** после инъекции протокол продвигает `Poll()`.
* **Ограничение:** на настоящем соединении байты просто подмешиваются в поток —
  это нарушит протокол.

```cpp
// Только в тесте: ответ 101 подкладывается строкой, без сети.
crossrender::WsCallbacks callbacks;
callbacks.onOpen = [] { ENG_LOGI("net", "открыт"); };
crossrender::WebSocket ws;
ws.Connect("ws://127.0.0.1:1/", callbacks);   // заведомо откажет — так ставятся колбэки
const std::string response =
    "HTTP/1.1 101 Switching Protocols\r\n"
    "Upgrade: websocket\r\n"
    "Connection: Upgrade\r\n"
    "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n\r\n";
ws.InjectForTest(response.data(), response.size());
ws.Poll();
ENG_CHECK(ws.IsOpen());
```

### `bool HttpGet(const std::string& url, std::vector<u8>* out, std::string* error = nullptr)`

Одноразовый HTTP GET. На нативных сборках это небольшой HTTP/1.1-клиент,
написанный вручную поверх TCP: поддерживается только `http://`, перенаправления
`301`, `302`, `303`, `307`, `308` (не больше трёх), тело `chunked` и
`Content-Length`. Заголовок запроса содержит `User-Agent: CrossRender/1.0` и
`Connection: close`, на чтение отведено 10 секунд. Принимается только ответ
`200`. На WASM запрос выполняет синхронный `XMLHttpRequest` из
`HttpGlue.cpp` — работает, но браузер печатает в консоль предупреждение о
блокирующем XHR в главном потоке.

* **Возвращает:** `true`, если тело получено целиком; тело складывается в `out`
  (буфер очищается в начале вызова).
* **Параметры:** `error` необязателен; при успехе строка очищается, при неудаче
  получает текст причины. На WASM при сбое текст начинается с
  `"HttpGet: fetch() failed for ..."`, хотя внутри используется
  `XMLHttpRequest`.
* **Ограничение:** `https://` отклоняется сразу с упоминанием отсутствующего
  TLS-бэкенда; другие схемы — как неподдерживаемые.
* **Ограничение:** вызов синхронный и блокирует поток (на WASM — главный поток
  приложения) на всё время загрузки.

```cpp
std::vector<crossrender::u8> body;
std::string error;
if (!crossrender::HttpGet("http://127.0.0.1:8080/config.json", &body, &error)) {
    ENG_LOGE("net", "конфиг не загрузился: %s", error.c_str());
} else {
    ENG_LOGI("net", "получено %d байт", static_cast<int>(body.size()));
}
```

### `bool NetInit()`

Инициализирует сетевую подсистему. В Windows вызывает `WSAStartup(2, 2)` и при
отказе возвращает `false`; на остальных платформах ничего не делает — только
пишет в лог и возвращает `true`. Вызывайте один раз на старте приложения.

* **Возвращает:** `true`, если сеть готова к работе.

```cpp
if (!crossrender::NetInit()) {
    ENG_LOGE("net", "сетевой слой недоступен, мультиплеер выключен");
    return;
}
```

### `void NetShutdown()`

Освобождает сетевую подсистему: в Windows вызывает `WSACleanup()`, в остальных
системах только пишет в лог. После вызова не создавайте новые сокеты; парное
`NetInit()` снова разрешено.

```cpp
crossrender::NetInit();
// ... вся сетевая работа ...
crossrender::NetShutdown();
```

### `std::string NetResolveHost(const std::string& host)`

Превращает имя хоста в строку IPv4. Литеральный адрес возвращается как есть;
иначе используется `getaddrinfo` с семейством `AF_INET` и берётся первый
результат.

* **Возвращает:** строку вида `"93.184.216.34"` или **пустую строку** при
  неудаче (с записью в лог через `ENG_LOGW`).
* **Ограничение:** на WASM DNS нет: не литеральное имя сразу даёт пустую
  строку.
* **Ограничение:** только IPv4; имена, у которых есть лишь AAAA-записи, не
  разрешатся.

```cpp
const std::string ip = crossrender::NetResolveHost("localhost");
if (ip.empty()) {
    ENG_LOGW("net", "имя не разрешилось — работаем только на прямых адресах");
} else {
    ENG_LOGI("net", "localhost -> %s", ip.c_str());
}
```

### `u16 NetFindFreePort()`

Подбирает свободный эфемерный порт: временно открывает UDP-сокет на
`127.0.0.1:0`, узнаёт выданный номер и закрывает сокет.

* **Возвращает:** номер порта или `0` при неудаче (и на WASM — там всегда `0`).
* **Ограничение:** это подсказка, а не бронь: между вызовом и `Listen` порт
  может занять кто-то другой. При гонке используйте `Listen(0)` и
  `LocalAddress()`.
* **Контекст:** порт подбирается через UDP, поэтому он пригоден и для TCP в
  тестах, но гарантий занятости TCP-порта нет.

```cpp
const crossrender::u16 port = crossrender::NetFindFreePort();
if (port == 0) {
    ENG_LOGW("net", "свободный порт не найден — просим ОС напрямую в Listen(0)");
}
crossrender::TcpSocket server;
server.Listen(port);   // при гонке Listen вернёт false с текстом ошибки
```

## Пример целиком

```cpp
#include "crossrender/core/Log.h"
#include "crossrender/net/Net.h"

#include <memory>
#include <string>
#include <vector>

// Учебный обмен: TCP-сервер и клиент на локальной петле плюс клиент веб-сокета.
// Всё, что здесь показано, работает на настольных и мобильных платформах;
// на WASM TCP/UDP — заглушки, а веб-сокет настоящий.

bool StartServer(crossrender::TcpSocket* server, crossrender::u16* outPort) {
    if (!server->Listen(0)) {                     // 0 — пусть ОС выдаст порт
        ENG_LOGE("net", "сервер не слушает: %s", server->LastError().c_str());
        return false;
    }
    *outPort = server->LocalAddress().port;       // узнаём фактический порт
    ENG_LOGI("net", "сервер поднят на %s", server->LocalAddress().ToString().c_str());
    return true;
}

void ExchangeOnce(crossrender::TcpSocket* client, crossrender::TcpSocket* peer) {
    const std::vector<crossrender::u8> question = {'p', 'i', 'n', 'g'};
    if (!client->SendMessage(question)) {
        ENG_LOGE("net", "запрос не ушёл: %s", client->LastError().c_str());
        return;
    }

    std::vector<crossrender::u8> incoming;
    if (!peer->ReceiveMessage(&incoming)) {
        ENG_LOGD("net", "ответ ещё не пришёл — попробуем в следующем кадре");
        return;
    }
    ENG_LOGI("net", "сервер получил %d байт, отправлено клиентом %llu",
             static_cast<int>(incoming.size()),
             static_cast<unsigned long long>(client->Stats().bytesSent));

    const std::vector<crossrender::u8> reply = {'p', 'o', 'n', 'g'};
    peer->SendMessage(reply);
    std::vector<crossrender::u8> answer;
    if (client->ReceiveMessage(&answer)) {
        ENG_LOGI("net", "клиент получил ответ из %d байт", static_cast<int>(answer.size()));
    }
}

void DemoWebSocket() {
    crossrender::WebSocket ws;
    crossrender::WsCallbacks cb;
    cb.onOpen = [&ws] { ENG_LOGI("net", "веб-сокет открыт: %s", ws.Url().c_str()); };
    cb.onText = [](const std::string& text) { ENG_LOGI("net", "текст: %s", text.c_str()); };
    cb.onError = [](const std::string& e) { ENG_LOGW("net", "ошибка веб-сокета: %s", e.c_str()); };

    if (!ws.Connect("ws://127.0.0.1:9001/lobby", cb, {"game"})) {
        // Например, wss:// сюда не годится: TLS в движке нет.
        ENG_LOGW("net", "веб-сокет не подключился: %s", ws.LastError().c_str());
        return;
    }

    // Один Poll() на кадр доводит рукопожатие и разбирает входящие кадры.
    for (int frame = 0; frame < 120; ++frame) {
        ws.Poll();
        if (ws.IsOpen() && frame == 10) {
            ws.SendText("hello from the engine");
        }
    }
    ws.Close(1000, "demo finished");
    ENG_LOGI("net", "итог веб-сокета: отправлено %llu байт",
             static_cast<unsigned long long>(ws.Stats().bytesSent));
}

void RunDemo() {
    if (!crossrender::NetInit()) {                        // в Windows поднимает Winsock
        ENG_LOGE("net", "сеть недоступна");
        return;
    }

    crossrender::TcpSocket server;
    crossrender::u16 port = 0;
    if (!StartServer(&server, &port)) {
        crossrender::NetShutdown();
        return;
    }

    crossrender::TcpSocket client;
    if (client.Connect(crossrender::NetResolveHost("localhost"), port, 2.0f)) {
        // Ждём, пока сервер увидит входящее соединение.
        std::unique_ptr<crossrender::TcpSocket> peer;
        for (int attempt = 0; attempt < 100 && !peer; ++attempt) {
            peer = server.Accept();
        }
        if (peer) {
            ExchangeOnce(&client, peer.get());
        } else {
            ENG_LOGW("net", "клиент не был принят за отведённое время");
        }
    } else {
        ENG_LOGW("net", "подключение не удалось: %s", client.LastError().c_str());
    }

    DemoWebSocket();

    client.Close();
    server.Close();
    crossrender::NetShutdown();
}
```

## См. также

* `docs/core/File.md` — `FetchUrl` и виртуальная файловая система: помните, что
  `FetchUrl` объявлен в `File.h`, к `Net.h` не относится и является заглушкой,
  возвращающей `false` на всех платформах.
* `docs/core/Log.md` — макросы `ENG_LOGI` / `ENG_LOGW` / `ENG_LOGE`, которыми
  сопровождаются все сетевые операции.
* `docs/core/Base.md` — типы `crossrender::u8` / `crossrender::u16` / `crossrender::u32` / `crossrender::usize`
  и макросы платформы, по которым выбирается нативная или WASM-реализация.
* `docs/test/Test.md` — `ENG_TEST` и `ENG_CHECK`, на которых построены
  loopback-тесты сети в `tests/test_net.cpp`.
* `engine/src/platform/wasm/WebSocketGlue.cpp` и
  `engine/src/platform/wasm/HttpGlue.cpp` — JS-прослойки браузерного
  WebSocket и синхронного `XMLHttpRequest`.



