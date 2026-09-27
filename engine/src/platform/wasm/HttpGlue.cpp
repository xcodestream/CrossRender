// Синхронный HTTP GET-мост для сетевого бэкенда WASM.
//
// engine/src/net/Net.cpp вызывает eng_js_http_get() из HttpGet(). В браузере нет
// синхронного fetch(): XMLHttpRequest с async = false — единственный способ
// заблокировать wasm-вызов до прихода ответа, и браузеры логируют
//
//   "Synchronous XMLHttpRequest on the main thread is deprecated because of its
//    detrimental effects to the end user's experience."
//
// при каждом вызове. Это по-прежнему работает в главном потоке (и в воркерах),
// что и делает блокирующий API загрузки ресурсов движка пригодным для Web.
// В будущем стоит перевести загрузку ресурсов на Asyncify или асинхронную очередь задач.
#include "crossrender/platform/Platform.h"

#if defined(ENG_PLATFORM_WASM) && defined(__EMSCRIPTEN__)

#include <emscripten/emscripten.h>

// При успехе возвращает 1 и записывает malloc-буфер + число байтов через
// выходные параметры (освобождать через eng_js_free), при неудаче — 0 (нули).
EM_JS(int, eng_js_http_get, (const char* url, void** outData, int* outLen), {
    // wasm32: указатели 32-битные, поэтому для выходных значений HEAPU32/HEAP32.
    if (outData) { HEAPU32[outData >> 2] = 0; }
    if (outLen) { HEAP32[outLen >> 2] = 0; }
    if (typeof XMLHttpRequest === 'undefined') {
        console.error('[crossrender.http] XMLHttpRequest is not available');
        return 0;
    }

    var ptr = url;
    var len = 0;
    while (HEAPU8[ptr + len]) { len++; }
    var target = new TextDecoder('utf-8').decode(HEAPU8.subarray(ptr, ptr + len));

    var xhr = new XMLHttpRequest();
    try {
        xhr.open('GET', target, false);  // false = synchronous (deprecated, see header)
    } catch (openErr) {
        console.error('[crossrender.http] open failed for ' + target + ': ' + openErr);
        return 0;
    }
    var usedArrayBuffer = false;
    try {
        xhr.responseType = 'arraybuffer';
        usedArrayBuffer = (xhr.responseType === 'arraybuffer');
    } catch (typeErr) {
        usedArrayBuffer = false;
    }
    if (!usedArrayBuffer) {
        // Старые движки отвергают responseType в синхронных запросах; откатываемся
        // к трюку с "бинарной строкой" и пользовательской кодировкой.
        try {
            xhr.overrideMimeType('text/plain; charset=x-user-defined');
        } catch (mimeErr) {
        }
    }
    try {
        xhr.send(null);
    } catch (sendErr) {
        console.error('[crossrender.http] send failed for ' + target + ': ' + sendErr);
        return 0;
    }
    // status 0 означает "file://" или непрозрачный ответ.
    if (xhr.status !== 0 && (xhr.status < 200 || xhr.status >= 300)) {
        console.error('[crossrender.http] HTTP ' + xhr.status + ' for ' + target);
        return 0;
    }

    var bytes = null;
    if (usedArrayBuffer && xhr.response instanceof ArrayBuffer) {
        bytes = new Uint8Array(xhr.response);
    } else if (typeof xhr.responseText === 'string') {
        var text = xhr.responseText;
        bytes = new Uint8Array(text.length);
        for (var i = 0; i < text.length; i++) {
            bytes[i] = text.charCodeAt(i) & 0xff;  // raw byte in the low 8 bits
        }
    } else if (xhr.response instanceof ArrayBuffer) {
        bytes = new Uint8Array(xhr.response);
    }
    if (!bytes) {
        console.error('[crossrender.http] no response body for ' + target);
        return 0;
    }

    var alloc = Module._malloc || (typeof _malloc !== 'undefined' ? _malloc : null);
    if (!alloc) {
        console.error('[crossrender.http] _malloc is not exported to JS');
        return 0;
    }
    var buffer = alloc(bytes.length + 1);
    if (!buffer) {
        console.error('[crossrender.http] out of memory for ' + target);
        return 0;
    }
    HEAPU8.set(bytes, buffer);
    HEAPU8[buffer + bytes.length] = 0;
    if (outData) { HEAPU32[outData >> 2] = buffer; }
    if (outLen) { HEAP32[outLen >> 2] = bytes.length; }
    return 1;
});

// Освобождает буфер, возвращённый eng_js_http_get().
EM_JS(void, eng_js_free, (void* p), {
    var freeFn = Module._free || (typeof _free !== 'undefined' ? _free : null);
    if (p && freeFn) { freeFn(p); }
});

#else

// В сборках без Emscripten эта единица трансляции компилируется в пустоту.
namespace crossrender {}

#endif  // ENG_PLATFORM_WASM && __EMSCRIPTEN__
