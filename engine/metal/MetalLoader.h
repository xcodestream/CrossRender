//
// CrossRender
// Автор: xcodestream <xcodestream@gmail.com>
// Лицензия: MIT
// Дата: 15.05.2026
// Описание: слой совместимости с Metal: перенаправление загрузчика GL на реализации поверх Metal (CR_METAL=ON).
//
#pragma once

namespace crossrender::mtlgl {

// Возвращает указатель на функцию для точки входа GL (никогда не null, пока
// слинкован модуль контекста).
void* ResolveGLProc(const char* name);

// true, когда у точки входа есть реальная реализация поверх Metal.
bool IsImplemented(const char* name);

}  // namespace crossrender::mtlgl
