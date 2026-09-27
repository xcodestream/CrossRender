//
// CrossRender
// Автор: xcodestream <xcodestream@gmail.com>
// Лицензия: MIT
// Дата: 15.05.2026
// Описание: слой совместимости с Metal: устройство, поверхность и каркас кадра для реализации GL поверх Metal.
//
#pragma once

#include "crossrender/core/Base.h"

namespace crossrender::mtlgl {

// Создаёт контекст устройства/очереди. Возвращает false, если Metal недоступен.
bool CreateContext();
void DestroyContext();
[[nodiscard]] bool ContextActive();

// Подключает поверхность, в которую выводится фреймбуфер по умолчанию. `cametalLayer`
// — это CAMetalLayer*; слой конфигурируется (пиксельный формат, размер drawable).
void AttachLayer(void* cametalLayer, uint32_t width, uint32_t height);
// Обновляет размер drawable после изменения размера окна.
void SurfaceResized(uint32_t width, uint32_t height);
void SurfaceSize(uint32_t* width, uint32_t* height);

// Фиксирует текущий кадр: рендерит фреймбуфер по умолчанию в следующий
// drawable и показывает его. Вызывать один раз за кадр (замена SwapBuffers).
void Present();

// Вызываются реализациями GL, чтобы открыть/закрыть текущий render pass
// поверх активного фреймбуфера (drawable или текстура FBO).
void BeginOrRestartFrameIfDirty();
void EndFrameIfOpen();

// Диагностика.
[[nodiscard]] int UnimplementedCallCount();

}  // namespace crossrender::mtlgl
