# crossrender/gfx/RenderTarget.h — фреймбуферы, чтение пикселей и постобработка

Оффскрин-рендеринг: фреймбуфер (framebuffer — буфер кадра) с цветом,
глубиной и MSAA (multisample anti-aliasing — сглаживание несколькими
выборками), RAII-обёртка для его привязки, настройки постобработки и цепочка
эффектов bloom + tonemap + FXAA.

## Заголовок

```cpp
#include "crossrender/gfx/RenderTarget.h"
```

## Обзор

Заголовок содержит четыре сущности:

1. **`RenderTargetDesc`** — описание того, что создать: размер, число
   выборок MSAA, наличие глубины и трафарета, формат цвета, количество
   цветовых вложений (MRT, multiple render targets — несколько целей
   отрисовки) и параметры сэмплера.
2. **`RenderTarget`** — сам фреймбуфер. Держит FBO, цветовые текстуры, при
   необходимости отдельный FBO для resolve (разрешения MSAA), depth-текстуру
   или depth-рендербуфер. `Bind`/`Unbind` работают как стек, поэтому
   оффскрин-проходы можно вкладывать друг в друга.
3. **`ScopedRenderTarget`** — RAII-обёртка: привязывает цель в конструкторе и
   снимает в деструкторе.
4. **`PostProcessor`** и **`PostProcessSettings`** — цепочка постобработки:
   сцена рендерится в HDR-таргет `SceneTarget()`, затем `Apply` применяет
   bloom и тонмаппинг в целевой фреймбуфер.

Типичный порядок работы:

1. Заполнить `RenderTargetDesc` и вызвать `Create`.
2. Каждый кадр: `Bind()` → `Clear(...)` → нарисовать сцену → `Unbind()`
   (внутри `Unbind` MSAA разрешается в цветовую текстуру).
3. Прочитать результат (`ColorTexture`, `ReadPixels`) или передать текстуру в
   постобработку.
4. Для постобработки: `PostProcessor::Init(w, h)`, рендер сцены в
   `SceneTarget()`, затем `Apply(SceneTarget().ColorTexture(), settings, w, h, 0)`.

#### Стек привязок `Bind` / `Unbind`

`Bind` кладёт на внутренний стек текущий FBO и размер вьюпорта, затем
привязывает свой FBO и ставит вьюпорт по размеру цели. `Unbind` вызывает
`Resolve()`, снимает запись со стека и восстанавливает FBO и вьюпорт. Для
самого внешнего уровня (`restore.fbo == 0`) вьюпорт берётся не из стека, а из
значения, записанного движком через `SetDefaultViewport` — так размер окна
остаётся верным после его изменения во время оффскрин-прохода.

#### Честные ограничения

* `RenderTargetDesc::colorAsTexture` **нигде не читается**: цель всегда
  создаёт сэмплируемые цветовые текстуры, отказаться от них нельзя.
* Поле `desc.depthFormat` используется только в пути без MSAA. В MSAA-пути
  глубина всегда рендербуфер: `DEPTH24_STENCIL8` при `stencil = true` и
  `DEPTH_COMPONENT24` иначе, поэтому `HasDepthTexture()` для MSAA-цели
  всегда `false`.
* В MSAA-пути формат цветового рендербуфера различает только `RGBA16F` и
  «всё остальное» (`GL_RGBA8`), тогда как resolve-текстуры создаются с
  запрошенным `colorFormat`.
* `Unbind` для MSAA-цели вызывает `Resolve()`, а тот **очищает стек
  привязок** и сбрасывает текущий FBO в `0`. Поэтому вложенные
  `Bind`/`Unbind` надёжно работают только для целей без MSAA: после первого
  `Unbind` MSAA-цели управление всегда возвращается в дефолтный фреймбуфер.
* `SetViewport` — это «сырой» `glViewport`: он не обновляет вьюпорт,
  сохранённый в стеке, поэтому после `Unbind` восстанавливается размер цели,
  а не то, что вы поставили вручную.
* `Clear` временно включает тест глубины и запись глубины, если требуется
  очистка буфера глубины, а затем возвращает прежние значения.
* `ReadPixels` пишет RGBA8 и просто вызывает `glReadPixels(0, 0, ...)`, а
  OpenGL отдаёт строки начиная с **нижней**. Если нужна картинка с первой
  строкой сверху, переворачивайте буфер сами (комментарий в заголовке
  обещает левый верхний угол, реализация этого не делает).
* `PostProcessSettings::fxaa` не используется в `Apply`: шейдер FXAA
  собирается в `Init`, но отдельного прохода для него в цепочке нет.
* `PostProcessSettings::enabled` выключает **только bloom**: тонмаппинг,
  гамма, виньетка, зерно и аберрация управляются своими флагами и
  выполняются всегда.
* `PostProcessor` некопируемый и неперемещаемый (внутри `std::unique_ptr` и
  объявленный деструктор), поэтому храните его по месту или по указателю.
* Без контекста OpenGL `RenderTarget::Create` пишет
  `ENG_LOGW("rt", "no GL context; render target '%s' not created")` и
  возвращает `false`; `PostProcessor::Init` в этом случае возвращает `false`,
  а `Apply` становится no-op.

## Члены класса

### `struct RenderTargetDesc`

Описание создаваемого фреймбуфера. Значения по умолчанию рассчитаны на
обычную цветную цель: без MSAA, с глубиной, без трафарета, один цветовой
аттачмент, формат `RGBA8`.

```cpp
crossrender::RenderTargetDesc desc;
desc.width = 1280;
desc.height = 720;
desc.name = "main-scene";
```

### `int RenderTargetDesc::width`

Ширина цели в пикселях. `Create` отвергает неположительное значение.

```cpp
desc.width = 1920;
```

### `int RenderTargetDesc::height`

Высота цели в пикселях.

```cpp
desc.height = 1080;
```

### `int RenderTargetDesc::samples`

Число выборок MSAA (`1` — без сглаживания). Если драйвер поддерживает
меньше, значение уменьшается до `GL_MAX_SAMPLES` с предупреждением
`ENG_LOGW("rt", ...)`, а фактическое значение видно в `Desc().samples`.

```cpp
crossrender::RenderTargetDesc msaa;
msaa.width = msaa.height = 1024;
msaa.samples = 4;
```

### `bool RenderTargetDesc::depth`

Создавать ли буфер глубины. Без него тест глубины не работает, и объекты
рисуются в порядке вызовов.

```cpp
desc.depth = true;
```

### `bool RenderTargetDesc::stencil`

Добавлять ли трафарет (stencil — буфер маски). При `true` вложение глубины
становится `DEPTH_STENCIL_ATTACHMENT`.

```cpp
desc.stencil = true;
```

### `bool RenderTargetDesc::colorAsTexture`

Объявлено для «разрешить resolve в сэмплируемую текстуру», но в
`RenderTarget::Create` **не используется** — цветовая текстура создаётся
всегда.

```cpp
desc.colorAsTexture = true;    // поле ни на что не влияет
```

### `PixelFormat RenderTargetDesc::colorFormat`

Формат цветовых вложений. Для HDR-сцены используйте
`PixelFormat::RGBA16F`, для обычной — `RGBA8`. В MSAA-пути различаются
только `RGBA16F` и «всё остальное».

```cpp
desc.colorFormat = crossrender::PixelFormat::RGBA16F;    // HDR-сцена
```

### `PixelFormat RenderTargetDesc::depthFormat`

Формат глубины для пути без MSAA. По умолчанию
`PixelFormat::Depth24Stencil8`. В MSAA-пути поле игнорируется.

```cpp
desc.depthFormat = crossrender::PixelFormat::Depth32F;
```

### `int RenderTargetDesc::colorAttachments`

Число цветовых вложений (MRT). Зажимается в диапазон `1..4`; фактическое
значение доступно через `ColorAttachmentCount()`.

```cpp
crossrender::RenderTargetDesc gbuffer;
gbuffer.width = gbuffer.height = 1280;
gbuffer.colorAttachments = 3;      // albedo, normal, ORM
```

### `TextureFilter RenderTargetDesc::filter`

Фильтрация цветовых текстур цели. На depth-текстуру не влияет — та всегда
`Nearest`.

```cpp
desc.filter = crossrender::TextureFilter::Linear;
```

### `TextureWrap RenderTargetDesc::wrap`

Адресация цветовых текстур цели. Для экранных эффектов правильнее
`ClampToEdge`, чтобы фильтрация не «заворачивала» края.

```cpp
desc.wrap = crossrender::TextureWrap::ClampToEdge;
```

### `bool RenderTargetDesc::generateMipmaps`

Генерировать ли мипмапы для цветовых текстур цели. Нужно, если текстура
будет сэмплироваться с уменьшением (например, грубый префильтр).

```cpp
desc.generateMipmaps = false;
```

### `std::string RenderTargetDesc::name`

Имя цели для логов. Попадает в сообщения `ENG_LOGW("rt", "no GL context;
render target '%s' not created")` и `ENG_LOGE("rt", "framebuffer '%s'
incomplete ...")`.

```cpp
desc.name = "post-scene";
```

### `class RenderTarget`

Фреймбуфер с цветовыми вложениями, глубиной и необязательным MSAA. Копировать
нельзя, перемещать можно; деструктор вызывает `Destroy()`. Привязка
управляется парой `Bind`/`Unbind` (со стеком) или `ScopedRenderTarget`.

```cpp
crossrender::RenderTarget rt;
if (!rt.Create(desc)) ENG_LOGW("demo", "оффскрин недоступен");
```

### `RenderTarget()`

Создаёт пустой объект: ни FBO, ни текстур, `Valid() == false`. Обращений к
OpenGL нет.

```cpp
crossrender::RenderTarget placeholder;
ENG_ASSERT(!placeholder.Valid());
```

### `~RenderTarget()`

Вызывает `Destroy()`: освобождает FBO, resolve-FBO, рендербуферы и текстуры.

```cpp
{
    crossrender::RenderTarget local;
    local.Create(desc);
}   // всё освобождено
```

### `RenderTarget(RenderTarget&&) noexcept`

Перемещающий конструктор: переносит FBO, resolve-FBO, рендербуферы, текстуры
и описание, оставляя источник пустым. Именно так цели складываются в
`std::vector` (например, в bloom-цепочке `PostProcessor`).

```cpp
std::vector<crossrender::RenderTarget> chain;
chain.push_back(crossrender::RenderTarget{});
```

### `RenderTarget& operator=(RenderTarget&&) noexcept`

Перемещающее присваивание: освобождает текущие ресурсы и забирает чужие.
Самоприсваивание безопасно.

```cpp
crossrender::RenderTarget a, b;
b = std::move(a);
```

### `RenderTarget(const RenderTarget&) = delete`, `RenderTarget& operator=(const RenderTarget&) = delete`

Копирование запрещено: FBO и его вложения — уникальные ресурсы GPU.

```cpp
void DrawInto(crossrender::RenderTarget& rt, const crossrender::Mesh& mesh) {
    rt.Bind();          // по ссылке
    mesh.Draw();
    rt.Unbind();
}
```

### `bool Create(const RenderTargetDesc& desc)`

Создаёт фреймбуфер по описанию. Число цветовых вложений зажимается в `1..4`;
при MSAA проверяется `GL_MAX_SAMPLES`, и при нехватке выборок значение
уменьшается с предупреждением. В пути без MSAA глубина сначала пробуется как
сэмплируемая текстура (`depthFormat`, `Nearest`), и только при неудаче
создаётся рендербуфер. В конце проверяется `glCheckFramebufferStatus`: при
неполном фреймбуфере пишется `ENG_LOGE("rt", "framebuffer '%s' incomplete
(0x%04X)")`, ресурсы уничтожаются и возвращается `false`. Ранее созданное
содержимое объекта освобождается.

* **Возвращает:** `true`, если фреймбуфер собран и `Valid()`.
* **Контекст:** без OpenGL пишет `ENG_LOGW("rt", "no GL context; ...")` и
  возвращает `false`.

```cpp
crossrender::RenderTargetDesc desc;
desc.width = 1024;
desc.height = 1024;
desc.samples = 4;                       // MSAA 4x
desc.colorFormat = crossrender::PixelFormat::RGBA16F;
desc.name = "hdr-scene";
crossrender::RenderTarget scene;
if (!scene.Create(desc)) {
    ENG_LOGE("demo", "не удалось создать оффскрин-таргет");
}
```

### `bool Resize(int width, int height)`

Пересоздаёт цель под новый размер, сохраняя остальные поля описания. Если
размер не изменился и FBO существует — возвращает `true` без работы. Иначе
вызывает `Create` (то есть все текстуры пересоздаются).

```cpp
if (!rt.Resize(fbWidth, fbHeight)) {
    ENG_LOGW("demo", "ресайз цели не удался");
}
```

### `void Destroy()`

Освобождает текстуры, depth-рендербуфер, MSAA-рендербуферы, resolve-FBO и
FBO; повторный вызов безопасен. Без загруженной библиотеки OpenGL просто
обнуляет имена.

```cpp
rt.Destroy();
ENG_ASSERT(!rt.Valid());
```

### `void Bind()`

Делает цель текущей для рисования: кладёт на стек предыдущие FBO и размер
вьюпорта, привязывает свой FBO и ставит вьюпорт `0, 0, Width(), Height()`.
Парный вызов — `Unbind()`.

```cpp
rt.Bind();
rt.Clear(crossrender::Color::FromRGB(0x101018));
```

### `void Unbind()`

Завершает проход: разрешает MSAA (`Resolve`), снимает запись со стека и
восстанавливает предыдущий FBO и вьюпорт. Для самого внешнего уровня
использует вьюпорт, записанный через `SetDefaultViewport`. Для MSAA-цели
стек при этом очищается (см. ограничения).

```cpp
rt.Bind();
DrawScene();
rt.Unbind();          // MSAA разрешён в ColorTexture()
```

### `static void SetDefaultViewport(int width, int height)`

Запоминает размер вьюпорта дефолтного фреймбуфера. Движок вызывает это
каждый кадр (`Engine.cpp`), чтобы `Unbind` возвращал корректный вьюпорт
после оффскрин-прохода, даже если окно изменило размер.

```cpp
crossrender::RenderTarget::SetDefaultViewport(fbWidth, fbHeight);
```

### `void Resolve()`

Разрешает MSAA: блитит (`glBlitFramebuffer`) все цветовые вложения из
мультисэмплового FBO в resolve-FBO, куда привязаны обычные текстуры. Для
цели без MSAA — no-op. Вызывается автоматически из `Unbind`, но можно
вызвать и вручную, если нужно сэмплировать результат, не снимая привязку.

```cpp
rt.Resolve();                                  // MSAA -> обычная текстура
shader.SetTexture("uScene", rt.ColorTexture(), 0);
```

### `void Clear(const Color& c, bool depth = true, bool stencil = false)`

Очищает цель: цвет — заданным цветом, глубину и трафарет — по флагам. Чтобы
очистка глубины сработала, функция временно включает `GL_DEPTH_TEST` и
запись глубины, а затем возвращает прежнее состояние. Цель должна быть
привязана.

```cpp
rt.Bind();
rt.Clear(crossrender::Color{0.05f, 0.05f, 0.08f, 1.0f}, /*depth=*/true, /*stencil=*/false);
```

### `void SetViewport(int x, int y, int w, int h)`

Ставит вьюпорт вручную — например, чтобы нарисовать сцену только в части
цели (split-screen). Это «сырой» `glViewport`; стек привязок он не
обновляет.

```cpp
rt.Bind();
rt.SetViewport(0, 0, rt.Width() / 2, rt.Height());   // левая половина
```

### `bool Valid() const`

`true`, если FBO создан. Проверяйте перед использованием, особенно в
headless-режиме и после `Resize`.

```cpp
if (!rt.Valid()) ENG_LOGW("demo", "оффскрин недоступен, рисую прямо в окно");
```

### `unsigned int Fbo() const`

Имя FBO OpenGL. Нужно для ручных вызовов (`glBindFramebuffer`,
`glBlitFramebuffer`) — например, при разрешении MSAA вручную или для
отладочной визуализации.

```cpp
crossrender::gl::glBindFramebuffer(crossrender::gl::GL_FRAMEBUFFER, rt.Fbo());
crossrender::gl::glViewport(0, 0, rt.Width(), rt.Height());
```

### `int Width() const`

Ширина цели в пикселях (из описания).

```cpp
ENG_LOGI("demo", "цель %dx%d", rt.Width(), rt.Height());
```

### `int Height() const`

Высота цели в пикселях.

```cpp
const float texelY = 1.0f / static_cast<float>(rt.Height());
```

### `f32 Aspect() const`

Соотношение сторон `Width() / Height()`; при нулевой высоте возвращает `1`.
Удобно для камеры, рисующей прямо в цель.

```cpp
camera.fovY = crossrender::Radians(60.0f);
shader.Set("uProj", camera.Proj(rt.Aspect()));
```

### `const RenderTargetDesc& Desc() const`

Фактическое описание цели. Отличается от исходного, если `samples` были
уменьшены драйвером или `colorAttachments` зажаты в `1..4`.

```cpp
if (rt.Desc().samples != requested.samples) {
    ENG_LOGW("demo", "MSAA понижен до %d", rt.Desc().samples);
}
```

### `Texture& ColorTexture()`

Цветовая текстура с индексом 0 — основной результат прохода. Если текстур
нет (цель не создана), возвращает статическую пустую текстуру, поэтому
проверяйте `Valid()`.

```cpp
shader.SetTexture("uScene", rt.ColorTexture(), 0);
```

### `Texture& TextureAt(int index)`

Цветовая текстура по индексу для MRT. При выходе индекса за диапазон
возвращает статическую пустую текстуру (безопасно, но и без данных).

```cpp
if (gbuffer.ColorAttachmentCount() >= 3) {
    shader.SetTexture("uNormal", gbuffer.TextureAt(1), 1);
    shader.SetTexture("uOrm", gbuffer.TextureAt(2), 2);
}
```

### `Texture& DepthTexture()`

Сэмплируемая текстура глубины. Валидна только если `HasDepthTexture()`
истинно; для MSAA-цели и для цели без глубины это пустая текстура.

```cpp
if (rt.HasDepthTexture()) {
    depthShader.SetTexture("uDepth", rt.DepthTexture(), 0);
}
```

### `int ColorAttachmentCount() const`

Число цветовых вложений (после зажима в `1..4`).

```cpp
for (int i = 0; i < rt.ColorAttachmentCount(); ++i) {
    ENG_LOGI("demo", "цветовое вложение %d: %dx%d", i, rt.TextureAt(i).Width(), rt.TextureAt(i).Height());
}
```

### `bool HasDepthTexture() const`

`true`, если глубина хранится в сэмплируемой текстуре. Для MSAA-цели всегда
`false` (глубина там рендербуфер), как и при `desc.depth = false`.

```cpp
if (!rt.HasDepthTexture()) ENG_LOGD("demo", "глубину нельзя сэмплировать");
```

### `bool ReadPixels(std::vector<u8>* outRGBA)`

Читает цвет в буфер `RGBA8` размером `Width() * Height() * 4` (буфер
перевыделяется). Для MSAA-цели читает из resolve-FBO, иначе из основного;
после чтения восстанавливает ранее привязанный FBO. Возвращает `false`, если
указатель пуст, размеры некорректны или нет `glReadPixels`.

* **Порядок строк:** OpenGL отдаёт строки начиная с нижней; чтобы получить
  изображение «сверху вниз», переворачивайте строки самостоятельно.

```cpp
std::vector<crossrender::u8> pixels;
if (rt.ReadPixels(&pixels)) {
    crossrender::Texture::EncodePng("out/frame.png", rt.Width(), rt.Height(), 4, pixels.data());
}
```

### `class ScopedRenderTarget`

RAII-обёртка привязки: конструктор вызывает `Bind()`, деструктор —
`Unbind()`. Копировать нельзя. Цель должна жить дольше объекта-обёртки.
Рекомендуемый способ оффскрин-прохода: выход из области видимости
гарантирует снятие привязки даже при раннем `return`.

```cpp
{
    crossrender::ScopedRenderTarget scope(rt);        // Bind
    rt.Clear(crossrender::Color::Black);
    DrawScene();
}                                             // Unbind
```

### `ScopedRenderTarget(RenderTarget& rt)`

Привязывает цель (вызывает `rt.Bind()`). Ссылку не хранит — только
указатель, поэтому цель обязана пережить обёртку.

```cpp
crossrender::RenderTarget shadowMap;
crossrender::ScopedRenderTarget pass(shadowMap);
```

### `~ScopedRenderTarget()`

Вызывает `rt_->Unbind()`, возвращая предыдущий фреймбуфер и вьюпорт.

```cpp
void RenderShadow(crossrender::RenderTarget& map) {
    crossrender::ScopedRenderTarget guard(map);
    // ... рисование ...
}   // привязка снята автоматически
```

### `struct PostProcessSettings`

Настройки цепочки постобработки. Значения по умолчанию включают bloom и
тонмаппинг (ACES, экспозиция 1, гамма 2.2) и FXAA; виньетка, зерно и
хроматическая аберрация выключены.

```cpp
crossrender::PostProcessSettings post;
post.bloom = true;
post.bloomIntensity = 0.8f;
post.tonemapMode = 0;        // ACES
```

### `bool PostProcessSettings::enabled`

Общий выключатель — но фактически управляет **только bloom**: без него
тонмаппинг и остальные эффекты всё равно применяются по своим флагам.

```cpp
settings.enabled = false;    // bloom выключен, тонмаппинг остался
```

### `bool PostProcessSettings::bloom`

Включает bloom-цепочку (свечение ярких участков).

```cpp
settings.bloom = true;
```

### `f32 PostProcessSettings::bloomThreshold`

Порог яркости, выше которого пиксели попадают в bloom. Больше значение —
светятся только самые яркие участки.

```cpp
settings.bloomThreshold = 1.2f;
```

### `f32 PostProcessSettings::bloomIntensity`

Интенсивность добавления bloom при обратном проходе по мипмапам.

```cpp
settings.bloomIntensity = 0.6f;
```

### `int PostProcessSettings::bloomMips`

Число уровней bloom-цепочки. Зажимается в `1..8`; при изменении цепочка
пересоздаётся целиком.

```cpp
settings.bloomMips = 5;
```

### `bool PostProcessSettings::tonemap`

Включает тонмаппинг (перевод HDR в LDR). При `false` шейдер получает режим
`3` — «без тонмаппинга».

```cpp
settings.tonemap = true;
```

### `f32 PostProcessSettings::exposure`

Экспозиция до тонмаппинга: `1.0` — без изменений, больше — светлее.

```cpp
settings.exposure = 1.4f;
```

### `int PostProcessSettings::tonemapMode`

Оператор тонмаппинга: `0` — ACES, `1` — Reinhard, `2` — Uncharted2,
`3` — без тонмаппинга. Значения вне диапазона шейдер трактует как
«без тонмаппинга».

```cpp
settings.tonemapMode = 1;    // Reinhard
```

### `bool PostProcessSettings::fxaa`

Флаг FXAA. Шейдер FXAA собирается в `Init`, но `Apply` его не запускает —
этот флаг сейчас ни на что не влияет.

```cpp
settings.fxaa = true;        // пока не используется в Apply
```

### `bool PostProcessSettings::vignette`

Включает виньетку (затемнение по краям кадра). Реализована внутри шейдера
тонмаппинга: при `false` передаётся нулевая интенсивность.

```cpp
settings.vignette = true;
settings.vignetteIntensity = 0.4f;
```

### `f32 PostProcessSettings::vignetteIntensity`

Сила виньетки; применяется, только если `vignette == true`.

```cpp
settings.vignetteIntensity = 0.3f;
```

### `f32 PostProcessSettings::gamma`

Показатель гамма-коррекции, по умолчанию `2.2`.

```cpp
settings.gamma = 2.2f;
```

### `bool PostProcessSettings::filmGrain`

Включает зерно плёнки; при `false` в шейдер уходит нулевая амплитуда.

```cpp
settings.filmGrain = true;
settings.grainAmount = 0.05f;
```

### `f32 PostProcessSettings::grainAmount`

Амплитуда зерна (шум анимируется по времени `uTime`).

```cpp
settings.grainAmount = 0.04f;
```

### `bool PostProcessSettings::chromaticAberration`

Включает хроматическую аберрацию (цветной ореол по краям).

```cpp
settings.chromaticAberration = true;
settings.aberrationAmount = 0.003f;
```

### `f32 PostProcessSettings::aberrationAmount`

Сила смещения каналов для аберрации.

```cpp
settings.aberrationAmount = 0.002f;
```

### `class PostProcessor`

Готовая цепочка постобработки: держит HDR-таргет сцены и bloom-цепочку и
собирает пять встроенных шейдеров (`blit`, `fxaa`, `bloom-down`, `bloom-up`,
`tonemap`). Сцена рисуется в `SceneTarget()`, результат отдаётся в `Apply`.
Некопируемый и неперемещаемый.

```cpp
crossrender::PostProcessor post;
if (!post.Init(1280, 720)) ENG_LOGW("demo", "постобработка недоступна");
```

### `PostProcessor()`

Создаёт объект и внутреннюю реализацию (`Impl` со шейдерами), но ничего не
создаёт на GPU. Для работы нужен `Init`.

```cpp
crossrender::PostProcessor post;      // ещё не готова, Valid() == false
```

### `~PostProcessor()`

Вызывает `Shutdown()`: уничтожает таргет сцены и bloom-цепочку.

```cpp
{
    crossrender::PostProcessor local;
    local.Init(640, 360);
}   // ресурсы освобождены
```

### `bool PostProcessor::Init(int width, int height)`

Инициализирует цепочку: создаёт HDR-таргет сцены (`RGBA16F`, с глубиной,
`TextureFilter::Linear`, имя `"post-scene"`) и компилирует встроенные шейдеры
из `builtin::kPostVert` + `kBlitFrag` / `kFxaaFrag` / `kBloomDownFrag` /
`kBloomUpFrag` / `kTonemapFrag`. Размеры зажимаются минимум в `1`.

* **Возвращает:** `true`, если таргет и все шейдеры готовы.
* **Контекст:** без OpenGL возвращает `false` (проверка `gl::glCreateShader`).

```cpp
crossrender::PostProcessor post;
if (!post.Init(fbWidth, fbHeight)) {
    ENG_LOGE("demo", "постобработка не инициализировалась: нет GPU?");
}
```

### `void PostProcessor::Shutdown()`

Уничтожает таргет сцены и очищает bloom-цепочку, переводит объект в
невалидное состояние. Повторный вызов безопасен; после `Shutdown` `Apply`
ничего не делает.

```cpp
post.Shutdown();
ENG_ASSERT(!post.Valid());
```

### `void PostProcessor::Resize(int width, int height)`

Меняет размер таргета сцены и пропорционально уменьшает каждый уровень
bloom-цепочки (каждый следующий — вдвое меньше предыдущего, минимум `1`).
Если размер не изменился — no-op. Шейдеры не пересобираются.

```cpp
void OnWindowResize(crossrender::PostProcessor& post, int w, int h) {
    post.Resize(w, h);
}
```

### `void PostProcessor::Apply(const Texture& scene, const PostProcessSettings& settings, int fbWidth, int fbHeight, unsigned int targetFbo = 0)`

Применяет цепочку: при включённом bloom строит downsample-цепочку с порогом
на первом уровне и складывает её обратно аддитивно (`GL_ONE, GL_ONE`), затем
рисует тонмаппинг (плюс гамма, виньетка, зерно, аберрация) в `targetFbo`
(`0` — дефолтный фреймбуфер) с вьюпортом `fbWidth x fbHeight`. Перед началом
выключает тест глубины, запись глубины и отсечение граней; по завершении
возвращает тест глубины и запись глубины.

* **`scene`** должна быть HDR (`RGBA16F`) — тонмаппинг рассчитывает на
  значения больше `1`.
* **Если объект невалиден** (не было `Init`), вызов сразу возвращается.
* **Bloom-цепочка** пересоздаётся при изменении числа мипмапов; размеры
  уровней берутся от размера `SceneTarget()`.

```cpp
// Сцена уже отрисована в HDR-таргет постобработки.
crossrender::PostProcessSettings post;
post.exposure = 1.15f;
post.bloom = true;
processor.Apply(processor.SceneTarget().ColorTexture(), post, fbWidth, fbHeight, /*targetFbo=*/0);
```

### `RenderTarget& PostProcessor::SceneTarget()`

HDR-таргет, в который нужно рисовать сцену до `Apply`. Именно его цветовая
текстура обычно передаётся первым аргументом `Apply`. Валиден после
успешного `Init`.

```cpp
crossrender::RenderTarget& sceneTarget = processor.SceneTarget();
sceneTarget.Bind();
sceneTarget.Clear(crossrender::Color::Black);
DrawScene();
sceneTarget.Unbind();
```

### `bool PostProcessor::Valid() const`

`true`, если `Init` прошёл и `Shutdown` ещё не вызывался. Отражает успех
создания таргета и всех пяти шейдеров.

```cpp
if (processor.Valid()) {
    processor.Apply(processor.SceneTarget().ColorTexture(), settings, fbW, fbH);
} else {
    DrawSceneDirectly();          // запасной путь без постобработки
}
```

## Пример целиком

```cpp
#include "crossrender/gfx/RenderTarget.h"

#include "crossrender/core/Log.h"
#include "crossrender/gfx/Mesh.h"
#include "crossrender/gfx/Shader.h"
#include "crossrender/gfx/Texture.h"

#include <vector>

// Кадр с оффскрин-рендерингом и постобработкой:
// цвет + глубина -> чтение пикселей -> bloom + тонмаппинг в окно.
void RenderFrame(crossrender::PostProcessor& post, crossrender::Mesh& scene, crossrender::Shader& pbr,
                 crossrender::PostProcessSettings& settings, int fbWidth, int fbHeight) {
    // 0. Движок каждый кадр сообщает размер дефолтного вьюпорта.
    crossrender::RenderTarget::SetDefaultViewport(fbWidth, fbHeight);

    // 1. Цель с цветом и глубиной; MSAA включён, чтобы проверить resolve.
    crossrender::RenderTargetDesc desc;
    desc.width = fbWidth;
    desc.height = fbHeight;
    desc.samples = 4;
    desc.depth = true;
    desc.colorFormat = crossrender::PixelFormat::RGBA16F;   // HDR для тонмаппинга
    desc.depthFormat = crossrender::PixelFormat::Depth24Stencil8;
    desc.filter = crossrender::TextureFilter::Linear;
    desc.wrap = crossrender::TextureWrap::ClampToEdge;
    desc.name = "frame";

    crossrender::RenderTarget target;
    if (!target.Create(desc)) {
        ENG_LOGW("demo", "оффскрин-таргет не создан (%s)", target.Desc().name.c_str());
    }

    // 2. Рисуем сцену в цель: Bind -> Clear -> Draw -> Unbind.
    if (target.Valid()) {
        target.Bind();
        target.Clear(crossrender::Color{0.05f, 0.07f, 0.12f, 1.0f}, /*depth=*/true);
        pbr.Bind();
        pbr.Set("uProj", crossrender::Mat4::Perspective(crossrender::Radians(60.0f), target.Aspect(), 0.1f, 300.0f));
        scene.Draw();
        target.Unbind();                 // здесь MSAA разрешается в ColorTexture()
    }

    // 3. Чтение пикселей обратно (например, для скриншота).
    std::vector<crossrender::u8> pixels;
    if (target.Valid() && target.ReadPixels(&pixels)) {
        crossrender::Texture::EncodePng("out/frame.png", target.Width(), target.Height(), 4, pixels.data());
        ENG_LOGI("demo", "кадр сохранён: %d пикселей", static_cast<int>(pixels.size() / 4));
    }

    // 4. Постобработка: сцена -> HDR-таргет -> bloom+tonemap -> окно.
    if (!post.Valid()) {
        ENG_LOGW("demo", "постобработка недоступна");
        return;
    }
    post.Resize(fbWidth, fbHeight);
    crossrender::RenderTarget& sceneTarget = post.SceneTarget();
    {
        crossrender::ScopedRenderTarget scope(sceneTarget);   // Bind + Unbind по RAII
        sceneTarget.Clear(crossrender::Color::Black);
        pbr.Bind();
        scene.Draw();
    }
    settings.enabled = true;
    settings.bloom = true;
    settings.bloomThreshold = 1.0f;
    settings.bloomMips = 4;
    settings.tonemap = true;
    settings.tonemapMode = 0;            // ACES
    settings.gamma = 2.2f;
    post.Apply(sceneTarget.ColorTexture(), settings, fbWidth, fbHeight, /*targetFbo=*/0);

    // 5. Цель с MRT: три цветовых вложения доступны по индексу.
    if (target.ColorAttachmentCount() > 1) {
        ENG_LOGD("demo", "вложение 1: %dx%d", target.TextureAt(1).Width(), target.TextureAt(1).Height());
    }
    if (target.HasDepthTexture()) {
        ENG_LOGD("demo", "глубину можно сэмплировать");
    }
}
```

## См. также

* `docs/gfx/Texture.md` — цветовые и глубинные текстуры цели, `EncodePng`
  для скриншотов.
* `docs/gfx/Shader.md` — встроенные шейдеры постобработки
  (`kPostVert`, `kFxaaFrag`, `kBloomDownFrag`, `kBloomUpFrag`,
  `kTonemapFrag`) и `Shader::SetTexture`.
* `docs/gfx/Mesh.md` — камера и материалы, которыми рисуется сцена в цель.
* `docs/core/Base.md` — `Clamp` / `MaxT`, используемые при зажиме размеров.
* `docs/core/Log.md` — сообщения `"rt"` об отсутствии контекста и неполном
  фреймбуфере.
