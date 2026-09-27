// JavaScript-мост WebSocket для сетевого бэкенда WASM.
//
// engine/src/net/Net.cpp реализует браузерный WebSocket-транспорт, вызывая
// пять точек входа eng_js_ws_* ниже; браузерные колбэки возвращаются в wasm
// через экспорты eng_ws_on_*, определённые в Net.cpp (там они помечены
// EMSCRIPTEN_KEEPALIVE, что даёт JS-стороне прямой враппер `_eng_ws_on_*` —
// гораздо дешевле, чем Module.ccall).
//
// Коды состояния, возвращаемые eng_js_ws_state(), совпадают с crossrender::WsState:
//   0 Closed, 1 Connecting, 2 Open, 3 Closing, 4 Error
// NOTE: engine/src/net/Net.cpp сейчас проверяет `eng_js_ws_state(id) == 1` как
// «открыто» (см. WebSocket::Poll), что при этом отображении значит «подключается».
// Авторитетный путь — события (eng_ws_on_open), поэтому запасной путь опроса
// просто никогда не выбирается; см. отчёт по платформе.
#include "crossrender/platform/Platform.h"

#if defined(ENG_PLATFORM_WASM) && defined(__EMSCRIPTEN__)

#include <emscripten/emscripten.h>

// Реализовано в engine/src/net/Net.cpp — здесь только объявление, без определения.
extern "C" {
void eng_ws_on_open(int id);
void eng_ws_on_message(int id, const void* data, int len, int isBinary);
void eng_ws_on_close(int id, int code, const char* reason);
void eng_ws_on_error(int id, const char* msg);
}

// ---------------------------------------------------------------------------
// Все пять функций ниже — EM_JS: C-сигнатура генерируется, а тело является
// JavaScript. Блоки кода EM_ASM/EM_JS не должны содержать запятую на верхнем
// уровне (C-препроцессор делит аргументы макроса по запятым), поэтому объектные
// литералы собираются по полям, а каждый аргумент вызова остаётся в скобках.
// ---------------------------------------------------------------------------

// Создаёт сокет и подключает четыре DOM-колбэка. Возвращает id моста, либо
// -1, если браузер отказывается создать WebSocket (неверный URL).
EM_JS(int, eng_js_ws_create, (const char* url, const char* protocols), {
    if (typeof WebSocket === 'undefined') {
        console.error('[crossrender.ws] WebSocket is not available in this environment');
        return -1;
    }
    if (!Module.__engWs) {
        Module.__engWs = ({});
        Module.__engWsNext = 1;
    }
    var id = Module.__engWsNext++;
    var entry = ({});
    entry.ws = null;
    entry.state = 1;  // connecting
    Module.__engWs[id] = entry;

    // C-строка -> JS-строка без опоры на хелперы рантайма (HEAPU8 доступен
    // всегда, TextDecoder стандартен в браузерах и node).
    var readString = function (ptr) {
        if (!ptr) { return String(); }
        var len = 0;
        while (HEAPU8[ptr + len]) { len++; }
        return new TextDecoder('utf-8').decode(HEAPU8.subarray(ptr, ptr + len));
    };
    // доступ к аллокатору (Module._malloc/_free — документированный путь; голые
    // имена _malloc/_free покрывают сборки, экспортирующие их лишь в скоуп модуля)
    var alloc = Module._malloc || (typeof _malloc !== 'undefined' ? _malloc : null);
    var freeFn = Module._free || (typeof _free !== 'undefined' ? _free : null);
    if (!alloc) { console.error('[crossrender.ws] _malloc is not exported to JS'); return -1; }

    // JS-строка -> malloc-буфер UTF-8 с NUL на конце (освобождает вызывающий).
    var writeString = function (text) {
        var bytes = new TextEncoder().encode(text);
        var ptr = alloc(bytes.length + 1);
        HEAPU8.set(bytes, ptr);
        HEAPU8[ptr + bytes.length] = 0;
        return ptr;
    };
    var release = function (ptr) { if (freeFn && ptr) { freeFn(ptr); } };
    // Диапазон байтов -> malloc-копия (движок освобождает её сразу после вызова).
    var writeBytes = function (bytes) {
        var ptr = alloc(bytes.length + 1);
        HEAPU8.set(bytes, ptr);
        HEAPU8[ptr + bytes.length] = 0;
        return ptr;
    };

    var proto = readString(protocols);
    try {
        entry.ws = proto.length ? new WebSocket(readString(url), proto) : new WebSocket(readString(url));
    } catch (err) {
        entry.state = 4;
        delete Module.__engWs[id];
        console.error('[crossrender.ws] failed to open ' + readString(url) + ': ' + err);
        return -1;
    }
    entry.ws.binaryType = 'arraybuffer';

    entry.ws.onopen = function () {
        entry.state = 2;  // open
        _eng_ws_on_open(id);
    };

    entry.ws.onmessage = function (ev) {
        var data = ev.data;
        if (typeof data === 'string') {
            // Текстовые кадры передаются движку как байты UTF-8.
            var utf8 = new TextEncoder().encode(data);
            var textPtr = writeString(data);
            _eng_ws_on_message(id, textPtr, utf8.length, 0);
            release(textPtr);
            return;
        }
        if (data instanceof ArrayBuffer) {
            var binPtr = writeBytes(new Uint8Array(data));
            _eng_ws_on_message(id, binPtr, data.byteLength, 1);
            release(binPtr);
            return;
        }
        // Blob (binaryType не сработал): сообщаем пустое бинарное сообщение,
        // а не выбрасываем уведомление.
        _eng_ws_on_message(id, 0, 0, 1);
    };

    entry.ws.onclose = function (ev) {
        entry.state = 0;  // closed
        var reason = (ev && ev.reason) ? ev.reason : String();
        var reasonPtr = writeString(reason);
        _eng_ws_on_close(id, (ev && ev.code) ? ev.code : 1005, reasonPtr);
        release(reasonPtr);
        delete Module.__engWs[id];
    };

    entry.ws.onerror = function () {
        entry.state = 4;  // error
        var msgPtr = writeString('websocket error');
        _eng_ws_on_error(id, msgPtr);
        release(msgPtr);
    };

    return id;
});

// Отправляет текстовый кадр UTF-8.
EM_JS(void, eng_js_ws_send_text, (int id, const char* utf8), {
    var entry = Module.__engWs ? Module.__engWs[id] : null;
    if (!entry || !entry.ws || entry.ws.readyState !== 1) { return; }
    var ptr = utf8;
    var len = 0;
    while (HEAPU8[ptr + len]) { len++; }
    var text = new TextDecoder('utf-8').decode(HEAPU8.subarray(ptr, ptr + len));
    try {
        entry.ws.send(text);
    } catch (err) {
        entry.state = 4;
        console.error('[crossrender.ws] send failed: ' + err);
    }
});

// Отправляет бинарный кадр (куча wasm копируется в отдельный ArrayBuffer,
// потому что движок переиспользует буфер отправки сразу после возврата из вызова).
EM_JS(void, eng_js_ws_send_binary, (int id, const void* data, int len), {
    var entry = Module.__engWs ? Module.__engWs[id] : null;
    if (!entry || !entry.ws || entry.ws.readyState !== 1 || len <= 0) { return; }
    var copy = new Uint8Array(len);
    copy.set(HEAPU8.subarray(data, data + len));
    try {
        entry.ws.send(copy.buffer);
    } catch (err) {
        entry.state = 4;
        console.error('[crossrender.ws] binary send failed: ' + err);
    }
});

// Запускает процедуру закрытия.
EM_JS(void, eng_js_ws_close, (int id, int code, const char* reason), {
    var entry = Module.__engWs ? Module.__engWs[id] : null;
    if (!entry || !entry.ws) { return; }
    entry.state = 3;  // closing
    var ptr = reason;
    var len = 0;
    if (ptr) { while (HEAPU8[ptr + len]) { len++; } }
    var text = len ? new TextDecoder('utf-8').decode(HEAPU8.subarray(ptr, ptr + len)) : String();
    // Браузер отвергает причины длиннее 123 байт.
    if (text.length > 120) { text = text.substring(0, 120); }
    try {
        if (code) {
            entry.ws.close(code, text);
        } else {
            entry.ws.close();
        }
    } catch (err) {
        try { entry.ws.close(); } catch (err2) { }
    }
});

// Возвращает текущее состояние в нумерации crossrender::WsState.
EM_JS(int, eng_js_ws_state, (int id), {
    var entry = Module.__engWs ? Module.__engWs[id] : null;
    if (!entry) { return 0; }         // Closed (unknown id)
    if (entry.state === 4) { return 4; }  // Error
    if (!entry.ws) { return 0; }
    switch (entry.ws.readyState) {
        case 0: return 1;  // CONNECTING -> Connecting
        case 1: return 2;  // OPEN       -> Open
        case 2: return 3;  // CLOSING    -> Closing
        case 3: return 0;  // CLOSED     -> Closed
        default: return 0;
    }
});

#else

// В сборках без Emscripten эта единица трансляции компилируется в пустоту
// (браузерного WebSocket-транспорта там нет; Net.cpp использует BSD-сокеты).
namespace crossrender {}

#endif  // ENG_PLATFORM_WASM && __EMSCRIPTEN__
