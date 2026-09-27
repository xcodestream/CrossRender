# crossrender/gfx/Shader.h — шейдерная программа, униформы и встроенные программы

Обёртка над программой OpenGL: компиляция вершинного и фрагментного шейдера
из строк или файлов, кэш расположения униформ и готовые исходники, общие для
нескольких рендереров.

## Заголовок

```cpp
#include "crossrender/gfx/Shader.h"
```

## Обзор

`Shader` владеет именем программы OpenGL (`glCreateProgram`). Объект нельзя
копировать, но можно перемещать; деструктор вызывает `Destroy()`.

Порядок работы:

1. При необходимости добавить макросы препроцессора: `AddDefine("MAX_LIGHTS 8")`
   (до сборки).
2. Собрать программу: `Build` (две строки), `BuildFile` (два файла) или
   `Load` (пара `<base>.vert` / `<base>.frag`).
3. Проверить результат и, если нужно, прочитать диагностику из `Log()`.
4. Сделать текущей: `Bind()`, затем выставить униформы (`Set`, `SetTexture`,
   `SetIntArray`).
5. Освободить: `Destroy()` или деструктор.

Поверх имён вершинных атрибутов движок использует фиксированные location:
`0` — позиция, `1` — нормаль, `2` — UV, `3` — касательная, `4` — цвет,
`5` — дополнительные UV (см. `docs/gfx/Mesh.md`).

#### Как `Build` собирает исходник

К тексту шейдера применяется препроцессинг (`BuildSource`):

1. Если исходник **не начинается** с `#version` (пробелы и переводы строк в
   начале игнорируются), впереди подставляется `builtin::Preamble()` —
   преамбула (заголовок) с версией GLSL и `precision`-квалификаторами.
2. Затем идут строки `#define`, накопленные в `defines_` (то есть
   добавленные через `AddDefine`).
3. Только после этого — сам исходник.

Поэтому `AddDefine("MAX_LIGHTS 8")` избавляет от необходимости вписывать
`#define` в каждый шейдер, а преамбулу можно переопределить, начав исходник
с собственного `#version`.

#### Честные ограничения

* Без контекста OpenGL `Build` пишет `ENG_LOGE("shader", "%s: no GL context")`
  и возвращает `false`. Ошибки компиляции и линковки также попадают в
  `Log()` и в `ENG_LOGE`.
* `Set` не сообщает об ошибке, если униформа не найдена (или выброшена
  оптимизатором): вызов молча ничего не делает. Проверить имя заранее можно
  через `UniformLocation` (результат `-1` означает «нет такой униформы»).
  Кэш запоминает и `-1`, так что повторные вызовы дороже не становятся.
* `SetTexture` всегда привязывает цель `GL_TEXTURE_2D`: cubemap и 3D-текстуры
  придётся привязывать вручную через `crossrender/gfx/GL.h`. Функция не проверяет
  `tex.Valid()` — привязка пустой текстуры (`Id() == 0`) отвязывает юнит.
  Активный текстурный юнит после вызова не восстанавливается.
* Слот текстуры не выбирается автоматически, несмотря на комментарий в
  заголовке: `slot` задаёт вызывающий. В движке материалы занимают юниты
  `0..4` (`uBaseColorTex`, `uNormalTex`, `uMetallicRoughnessTex`,
  `uEmissiveTex`, `uOcclusionTex`), тени — начиная с юнита `5`.
* Перегрузки `Set` для `std::vector` при пустом векторе выходят сразу, не
  обнуляя униформу: массив останется в прежнем состоянии.
* `Shader::Unbind()` — статический и вызывает `glUseProgram(0)` без проверки
  контекста; в headless-сборке (когда указатели OpenGL пусты) его вызывать
  нельзя. `Bind()` безопасен: без собранной программы он ничего не делает.
* `Build(nullptr, ...)` недопустим: нулевые строки приведут к
  неопределённому поведению, а не к понятной ошибке.

## Члены класса

### `struct UniformValue`

Универсальный контейнер значения униформы: одно поле под каждый
поддерживаемый тип плюс `type`, который говорит, какое поле читать. В
текущем рендерере униформы выставляются типизированными перегрузками `Set`,
а `UniformValue` — общий формат для отражения (reflection) и для кода,
который хранит значения униформ в контейнере и применяет их пачкой.

```cpp
crossrender::UniformValue uv;
uv.type = crossrender::UniformValue::Type::Vec3;
uv.v3 = crossrender::Vec3{0.2f, 0.6f, 1.0f};
ENG_LOGI("demo", "тип значения: %d", static_cast<int>(uv.type));
```

### `enum class UniformValue::Type`

Тег, показывающий, какое из полей `UniformValue` действительно заполнено.

| Значение | Поле |
|---|---|
| `UniformValue::Type::Int` | `i` |
| `UniformValue::Type::Float` | `f` |
| `UniformValue::Type::Vec2` | `v2` |
| `UniformValue::Type::Vec3` | `v3` |
| `UniformValue::Type::Vec4` | `v4` |
| `UniformValue::Type::Mat3` | `m3` |
| `UniformValue::Type::Mat4` | `m4` |
| `UniformValue::Type::Texture` | `tex` и `slot` |

```cpp
crossrender::UniformValue uv;
uv.type = crossrender::UniformValue::Type::Texture;
uv.tex = &albedo;
uv.slot = 0;
```

### `Type UniformValue::type`

Какое поле контейнера считается активным. По умолчанию `Type::Float`
(при этом поле `f` равно нулю).

```cpp
crossrender::UniformValue exposure;
exposure.type = crossrender::UniformValue::Type::Float;
exposure.f = 1.2f;
```

### `i32 UniformValue::i`

Целое значение (`Type::Int`): индекс, флаг, номер режима.

```cpp
crossrender::UniformValue mode;
mode.type = crossrender::UniformValue::Type::Int;
mode.i = 3;                    // например, «тонмаппинг выключен»
```

### `f32 UniformValue::f`

Вещественное значение (`Type::Float`).

```cpp
crossrender::UniformValue intensity;
intensity.f = 0.6f;            // bloomIntensity
```

### `Vec2 UniformValue::v2`

Двумерный вектор (`Type::Vec2`): размер текселя, скролл UV.

```cpp
crossrender::UniformValue texel;
texel.type = crossrender::UniformValue::Type::Vec2;
texel.v2 = crossrender::Vec2{1.0f / 1280.0f, 1.0f / 720.0f};
```

### `Vec3 UniformValue::v3`

Трёхмерный вектор (`Type::Vec3`): направление света, цвет без альфы,
коэффициент масштаба.

```cpp
crossrender::UniformValue dir;
dir.type = crossrender::UniformValue::Type::Vec3;
dir.v3 = crossrender::Normalize(crossrender::Vec3{-0.4f, -1.0f, -0.3f});
```

### `Vec4 UniformValue::v4`

Четырёхмерный вектор (`Type::Vec4`): цвет с альфой, плоскость отсечения.

```cpp
crossrender::UniformValue tint;
tint.type = crossrender::UniformValue::Type::Vec4;
tint.v4 = crossrender::Color::Red.ToVec4();
```

### `Mat4 UniformValue::m4`

Матрица 4x4 (`Type::Mat4`): трансформация, view-projection.

```cpp
crossrender::UniformValue xform;
xform.type = crossrender::UniformValue::Type::Mat4;
xform.m4 = camera.ViewProj(16.0f / 9.0f);
```

### `f32 UniformValue::m3[9]`

Матрица 3x3, записанная как девять чисел по столбцам (как того ждёт
`glUniformMatrix3fv` с `transpose = 0`).

```cpp
crossrender::UniformValue normalMatrix;
normalMatrix.type = crossrender::UniformValue::Type::Mat3;
for (int i = 0; i < 9; ++i) normalMatrix.m3[i] = (i % 4 == 0) ? 1.0f : 0.0f;  // единичная
```

### `const Texture* UniformValue::tex`

Указатель на текстуру для `Type::Texture`. Указатель **не владеющий**:
текстура должна жить дольше значения.

```cpp
crossrender::UniformValue slot0;
slot0.type = crossrender::UniformValue::Type::Texture;
slot0.tex = &albedo;
slot0.slot = 0;
```

### `i32 UniformValue::slot`

Текстурный юнит для `Type::Texture`. Должен совпадать с тем, что передан в
`Set` для сэмплера (`glUniform1i`).

```cpp
uv.slot = 2;                   // третий юнит, если 0..1 заняты
```

### `Shader()`

Создаёт пустой объект: программы нет, `Valid() == false`. Обращений к
OpenGL нет, поэтому конструктор безопасен до создания контекста.

```cpp
crossrender::Shader placeholder;
ENG_ASSERT(!placeholder.Valid());
```

### `~Shader()`

Вызывает `Destroy()` — удаляет программу OpenGL, если она была собрана.

```cpp
{
    crossrender::Shader local;
    local.Build(crossrender::builtin::kPostVert, crossrender::builtin::kBlitFrag, "local-blit");
}   // программа освобождена автоматически
```

### `Shader(Shader&& o) noexcept`

Перемещающий конструктор: переносит имя программы, имя, лог, defines и кэш
униформ, оставляя `o` пустым.

```cpp
std::vector<crossrender::Shader> passes;
passes.push_back(crossrender::Shader{});     // перемещение
```

### `Shader& operator=(Shader&& o) noexcept`

Перемещающее присваивание: освобождает текущую программу и забирает ресурсы
`o`. Самоприсваивание безопасно.

```cpp
crossrender::Shader a, b;
b = std::move(a);                    // b владеет программой, a пуста
```

### `Shader(const Shader&) = delete`, `Shader& operator=(const Shader&) = delete`

Копирование запрещено: программа OpenGL — уникальный ресурс. Передавайте
`Shader&` или `const Shader&`.

```cpp
void DrawPass(crossrender::Shader& shader, const crossrender::Mesh& mesh) {
    shader.Bind();
    mesh.Draw();
}
```

### `bool Build(const char* vertexSrc, const char* fragmentSrc, const std::string& name = "shader")`

Компилирует и линкует программу. К обоим исходникам применяется преамбула и
накопленные defines. Имя используется только в диагностике. Перед сборкой
предыдущая программа освобождается, лог очищается.

* **Возвращает:** `true`, если программа собралась и слинковалась.
* **Контекст:** требует контекста OpenGL; иначе `false` и запись в лог.
* **Диагностика:** текст ошибок компиляции/линковки доступен в `Log()`.

```cpp
crossrender::Shader shader;
shader.AddDefine("MAX_LIGHTS 8");
const char* vs = R"(layout(location = 0) in vec3 aPos;
void main() { gl_Position = vec4(aPos, 1.0); })";
const char* fs = R"(out vec4 FragColor;
void main() { FragColor = vec4(1.0, 0.5, 0.2, 1.0); })";
if (!shader.Build(vs, fs, "flat-orange")) {
    ENG_LOGE("demo", "шейдер не собрался:\n%s", shader.Log().c_str());
}
```

### `bool BuildFile(const std::string& vertPath, const std::string& fragPath)`

Читает два файла через `ReadTextFile` и вызывает `Build`, используя имя файла
вершинного шейдера как имя программы. Если хотя бы один файл не читается —
пишет `ENG_LOGE("shader", "cannot read %s / %s")` и возвращает `false`.

```cpp
crossrender::Shader water;
if (!water.BuildFile("shaders/water.vert", "shaders/water.frag")) {
    ENG_LOGW("demo", "шейдер воды недоступен, вода будет плоской");
}
```

### `bool Load(const std::string& basePath)`

Удобная обёртка над `BuildFile`: подставляет расширения сам, то есть
`Load("shaders/water")` читает `shaders/water.vert` и `shaders/water.frag`.
Имя программы берётся из имени `.vert`-файла.

```cpp
crossrender::Shader sky;
sky.Load("shaders/sky");        // shaders/sky.vert + shaders/sky.frag
```

### `void Destroy()`

Удаляет программу OpenGL и очищает кэш расположения униформ; повторный вызов
безопасен. Накопленные defines при этом сохраняются.

```cpp
shader.Destroy();
ENG_ASSERT(!shader.Valid());
```

### `void Bind() const`

Делает программу текущей (`glUseProgram`). Если программа не собрана, вызов
ничего не делает — это безопасно.

```cpp
shader.Bind();
shader.Set("uExposure", 1.2f);
```

### `static void Unbind()`

Снимает текущую программу (`glUseProgram(0)`). Статический метод, потому что
действует глобально.

```cpp
shader.Bind();
mesh.Draw();
crossrender::Shader::Unbind();          // дальше рисуем без программы
```

### `void Set(const char* name, i32 v)`

Выставляет целочисленную униформу (`glUniform1i`): номера текстурных юнитов,
флаги, режимы.

```cpp
shader.Set("uMode", 2);          // например, «Blend»-режим альфы
```

### `void Set(const char* name, f32 v)`

Выставляет вещественную униформу (`glUniform1f`): экспозицию, шероховатость,
порог.

```cpp
shader.Set("uRoughness", material.roughness);
shader.Set("uExposure", settings.exposure);
```

### `void Set(const char* name, const Vec2& v)`

Выставляет `vec2` (`glUniform2f`): размер текселя, скролл UV, масштаб.

```cpp
shader.Set("uTexelSize", crossrender::Vec2{1.0f / scene.Width(), 1.0f / scene.Height()});
```

### `void Set(const char* name, const Vec3& v)`

Выставляет `vec3` (`glUniform3f`): направления, цвета без альфы, параметры
материала.

```cpp
shader.Set("uLightDir", light.direction);
shader.Set("uEmissive", material.emissive.rgb() * material.emissiveStrength);
```

### `void Set(const char* name, const Vec4& v)`

Выставляет `vec4` (`glUniform4f`). Работает и с `Vec4`, и с `Color`.

```cpp
shader.Set("uClipPlane", crossrender::Vec4{0.0f, 1.0f, 0.0f, -0.5f});
```

### `void Set(const char* name, const Color& v)`

Выставляет цвет как `vec4` (`glUniform4f`): компоненты `r, g, b, a`
передаются по порядку. Именно так `Renderer3D` передаёт `uBaseColor` и
`uEmissive`.

```cpp
shader.Set("uBaseColor", material.baseColor);
shader.Set("uFogColor", environment.fogColor);
```

### `void Set(const char* name, const Mat4& v)`

Выставляет матрицу 4x4 без транспонирования (`glUniformMatrix4fv`), беря
данные из `Mat4::data()`. Матрицы движка хранятся по столбцам, поэтому
транспонирование не нужно.

```cpp
shader.Set("uViewProj", camera.ViewProj(1280.0f / 720.0f));
shader.Set("uModel", crossrender::Mat4::TRS(position, rotationEuler, scale));
```

### `void Set(const char* name, const float* m3)`

Выставляет матрицу 3x3 из девяти чисел (`glUniformMatrix3fv`). Указатель
должен ссылаться минимум на девять значений; проверки длины нет — для
матрицы нормалей в движке используется этот путь.

```cpp
const float normalMatrix[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
shader.Set("uNormalMatrix", normalMatrix);
```

### `void Set(const char* name, const std::vector<Vec3>& v)`

Выставляет массив `vec3` одним вызовом (`glUniform3fv`). Длина берётся из
вектора. При пустом векторе — ранний выход, униформа не обнуляется.

```cpp
std::vector<crossrender::Vec3> positions = {light0.position, light1.position};
shader.Set("uLightPositions", positions);
```

### `void Set(const char* name, const std::vector<Vec4>& v)`

Выставляет массив `vec4` (`glUniform4fv`) — например, цвета источников света
или плоскости теней.

```cpp
std::vector<crossrender::Vec4> colors = {light0.color.ToVec4(), light1.color.ToVec4()};
shader.Set("uLightColors", colors);
```

### `void Set(const char* name, const std::vector<Mat4>& v)`

Выставляет массив матриц 4x4 (`glUniformMatrix4fv` с `count = v.size()`) —
так передаются матрицы каскадов теней и костей скелета.

```cpp
std::vector<crossrender::Mat4> cascades;
cascades.push_back(camera.ViewProj(16.0f / 9.0f));
cascades.push_back(crossrender::Mat4::Identity());
shader.Set("uShadowMatrices", cascades);   // пустой вектор ничего не изменит
```

### `void SetTexture(const char* name, const Texture& tex, int slot = 0)`

Привязывает текстуру к юниту `slot` и записывает этот номер в сэмплерную
униформу `name`. Именно так текстуры попадают в шейдер — у `Texture` нет
собственного `Bind`. Если униформы нет, вызов ничего не делает (текстура
тоже не привязывается). Цель всегда `GL_TEXTURE_2D`.

```cpp
shader.SetTexture("uBaseColorTex", albedo, 0);
shader.SetTexture("uNormalTex", normals, 1);
```

### `void SetIntArray(const char* name, const i32* values, int count)`

Выставляет массив целых (`glUniform1iv`) — удобно для «каких юнитов» или
палитровых индексов. Длина задаётся явно и не проверяется.

```cpp
const crossrender::i32 units[3] = {0, 1, 2};
shader.SetIntArray("uTextureUnits", units, 3);
```

### `void AddDefine(const std::string& define)`

Добавляет строку `#define` в список, который подставляется в оба шейдера при
следующем `Build`. Строка должна быть в формате «имя» или «имя значение».
Вызывайте до сборки; после `Build` изменение списка не влияет на уже
собранную программу.

```cpp
const char* vsSource = "void main() { gl_Position = vec4(0.0); }";
const char* fsSource = "out vec4 c; void main() { c = vec4(1.0); }";
shader.AddDefine("USE_FOG 1");
shader.AddDefine("MAX_LIGHTS 8");
shader.Build(vsSource, fsSource, "forward");
```

### `void ClearDefines()`

Очищает список defines. Полезно, если одна и та же переменная `Shader`
пересобирается под разные варианты.

```cpp
const char* vsSource = "void main() { gl_Position = vec4(0.0); }";
const char* fsSource = "out vec4 c; void main() { c = vec4(1.0); }";
shader.ClearDefines();
shader.AddDefine("SHADOWS 0");
shader.Build(vsSource, fsSource, "forward-noshadow");
```

### `bool Valid() const`

`true`, если программа успешно создана (`program_ != 0`). Так проверяют
результат `Build` перед использованием.

```cpp
if (shader.Valid()) {
    shader.Bind();
} else {
    ENG_LOGW("demo", "программа не собрана, пропускаю проход");
}
```

### `unsigned int Id() const`

Имя программы OpenGL. Нужно только для ручных вызовов вроде
`glGetUniformLocation` через `crossrender/gfx/GL.h`.

```cpp
ENG_LOGD("demo", "program id = %u", shader.Id());
```

### `const std::string& Name() const`

Имя программы, заданное при сборке (для `BuildFile` — имя файла вершинного
шейдера). Используется в диагностике.

```cpp
ENG_LOGE("demo", "шейдер '%s' не готов", shader.Name().c_str());
```

### `const std::string& Log() const`

Накопленный текст ошибок компиляции и линковки. Заполняется при `Build`,
очищается в его начале. Пустая строка означает успешную сборку.

```cpp
if (!shader.Build(crossrender::builtin::kForwardVert, crossrender::builtin::kForwardFrag, "terrain")) {
    ENG_LOGE("demo", "лог сборки:\n%s", shader.Log().c_str());
}
```

### `int UniformLocation(const char* name)`

Возвращает расположение униформы, при первом обращении запрашивая его у
драйвера и кэшируя результат. `-1` означает «такой униформы нет» (её не
объявили или выбросил оптимизатор). Кэшируется и `-1`, поэтому повторные
вызовы дешёвые. При отсутствии программы возвращает `-1`, ничего не кэшируя.

```cpp
if (shader.UniformLocation("uShadowMatrices") >= 0) {
    shader.Set("uShadowMatrices", cascades);
}
```

### `const char* builtin::Preamble()`

Преамбула GLSL для текущей платформы. На настольных платформах это
`#version 330 core` и `#define ENG_GLES 0`; на GLES (Android, iOS, WASM) —
`#version 300 es`, квалификаторы `precision highp float/int` и
`#define ENG_GLES 1`. `Build` подставляет её автоматически, если исходник не
начинается с `#version`. Полезно знать при написании шейдеров: код должен
компилироваться в обоих вариантах.

```cpp
const char* pre = crossrender::builtin::Preamble();
ENG_LOGI("demo", "преамбула GLSL:\n%s", pre);
```

### `const char* builtin::kBlitVert`

Вершинный шейдер полноэкранного блита: рисует один треугольник (полоса из
четырёх вершин) и передаёт UV. Используется постобработкой.

```cpp
crossrender::Shader blit;
blit.Build(crossrender::builtin::kBlitVert, crossrender::builtin::kBlitFrag, "blit");
```

### `const char* builtin::kBlitFrag`

Фрагментный шейдер блита: сэмплирует текстуру и умножает её на оттенок
(`uTint`).

```cpp
crossrender::Shader tinted;
tinted.Build(crossrender::builtin::kBlitVert, crossrender::builtin::kBlitFrag, "tinted-blit");
tinted.SetTexture("uTexture", sceneTex, 0);
tinted.Set("uTint", crossrender::Color::White);
```

### `const char* builtin::kSpriteVert`

Вершинный шейдер неосвещённой геометрии с цветом вершины: его используют
2D-батч и отладочные линии.

```cpp
crossrender::Shader lines;
lines.Build(crossrender::builtin::kSpriteVert, crossrender::builtin::kSpriteFrag, "debug-lines");
```

### `const char* builtin::kSpriteFrag`

Фрагментный шейдер спрайта: текстура, умноженная на цвет вершины.

```cpp
crossrender::Shader sprites;
sprites.Build(crossrender::builtin::kSpriteVert, crossrender::builtin::kSpriteFrag, "sprites");
sprites.Set("uProjection", crossrender::Mat4::Ortho2D(1280.0f, 720.0f));
```

### `const char* builtin::kForwardVert`

Вершинный шейдер прямого (forward) прохода 3D: PBR-lite, до 8 источников
света и тени.

```cpp
crossrender::Shader forward;
forward.Build(crossrender::builtin::kForwardVert, crossrender::builtin::kForwardFrag, "forward");
```

### `const char* builtin::kForwardFrag`

Фрагментный шейдер прямого прохода: металличность-шероховатость,
`AlphaMode`, туман, IBL-приближение.

```cpp
crossrender::Shader forward;
forward.Build(crossrender::builtin::kForwardVert, crossrender::builtin::kForwardFrag, "forward-pbr");
forward.Set("uMetallic", 0.0f);
```

### `const char* builtin::kShadowVert`

Вершинный шейдер теневого прохода — пишет только глубину.

```cpp
crossrender::Shader shadow;
shadow.Build(crossrender::builtin::kShadowVert, crossrender::builtin::kShadowFrag, "shadow");
```

### `const char* builtin::kShadowFrag`

Фрагментный шейдер теневого прохода: пустой (глубина пишется без цвета).

```cpp
crossrender::Shader shadowDepth;
shadowDepth.Build(crossrender::builtin::kShadowVert, crossrender::builtin::kShadowFrag, "shadow-depth");
```

### `const char* builtin::kVoxelVert`

Вершинный шейдер воксельного чанка (жадная меш-генерация и реймарш).

```cpp
crossrender::Shader voxel;
voxel.Build(crossrender::builtin::kVoxelVert, crossrender::builtin::kVoxelFrag, "voxel");
```

### `const char* builtin::kVoxelFrag`

Фрагментный шейдер вокселей: палитра и ambient occlusion, упакованные в
`uv2`.

```cpp
crossrender::Texture palette3D;
palette3D.Create3D(64, 64, 64, crossrender::PixelFormat::R8);
crossrender::Shader voxel;
voxel.Build(crossrender::builtin::kVoxelVert, crossrender::builtin::kVoxelFrag, "voxel");
voxel.SetTexture("uPalette", palette3D, 0);   // 3D-текстура палитры
```

### `const char* builtin::kParticleVert`

Вершинный шейдер частиц-билбордов (инстансинг).

```cpp
crossrender::Shader particles;
particles.Build(crossrender::builtin::kParticleVert, crossrender::builtin::kParticleFrag, "particles");
```

### `const char* builtin::kParticleFrag`

Фрагментный шейдер частиц: мягкая альфа, tinting, поддержка разных режимов
смешивания.

```cpp
crossrender::Shader particles;
particles.Build(crossrender::builtin::kParticleVert, crossrender::builtin::kParticleFrag, "particles");
particles.Set("uSoftness", 0.5f);
```

### `const char* builtin::kSdfTextVert`

Вершинный шейдер SDF-текста (шрифты на подписанных расстояниях —
Signed Distance Field).

```cpp
crossrender::Shader text;
text.Build(crossrender::builtin::kSdfTextVert, crossrender::builtin::kSdfTextFrag, "sdf-text");
```

### `const char* builtin::kSdfTextFrag`

Фрагментный шейдер SDF-текста: порог и сглаживание по spread из
`Texture::SdfSpread()`.

```cpp
text.SetTexture("uAtlas", glyphAtlas, 0);
text.Set("uSdfSpread", glyphAtlas.SdfSpread());
```

### `const char* builtin::kPostVert`

Вершинный шейдер постобработки: общий полноэкранный треугольник для всех
фильтров.

```cpp
crossrender::Shader post;
post.Build(crossrender::builtin::kPostVert, crossrender::builtin::kTonemapFrag, "tonemap");
```

### `const char* builtin::kFxaaFrag`

Фрагментный шейдер FXAA — сглаживание краёв после тонмаппинга.

```cpp
crossrender::Shader fxaa;
fxaa.Build(crossrender::builtin::kPostVert, crossrender::builtin::kFxaaFrag, "fxaa");
fxaa.SetTexture("uTexture", ldrColor, 0);
```

### `const char* builtin::kBloomDownFrag`

Фрагментный шейдер понижения разрешения с порогом яркости — первый шаг
bloom-цепочки.

```cpp
crossrender::Shader bloomDown;
bloomDown.Build(crossrender::builtin::kPostVert, crossrender::builtin::kBloomDownFrag, "bloom-down");
bloomDown.Set("uThreshold", 1.0f);
```

### `const char* builtin::kBloomUpFrag`

Фрагментный шейдер повышения разрешения bloom-цепочки; складывается
аддитивно (`GL_ONE, GL_ONE`).

```cpp
crossrender::Shader bloomUp;
bloomUp.Build(crossrender::builtin::kPostVert, crossrender::builtin::kBloomUpFrag, "bloom-up");
bloomUp.Set("uIntensity", 0.6f);
```

### `const char* builtin::kTonemapFrag`

Фрагментный шейдер тонмаппинга: ACES, Reinhard, Uncharted2 или «без
тонмаппинга», плюс гамма, виньетка, зерно и хроматическая аберрация.

```cpp
crossrender::Shader tonemap;
tonemap.Build(crossrender::builtin::kPostVert, crossrender::builtin::kTonemapFrag, "tonemap");
tonemap.Set("uMode", 0);        // ACES
tonemap.Set("uGamma", 2.2f);
```

### `const char* builtin::kSkyVert`

Вершинный шейдер градиента неба.

```cpp
crossrender::Shader sky;
sky.Build(crossrender::builtin::kSkyVert, crossrender::builtin::kSkyFrag, "sky");
```

### `const char* builtin::kSkyFrag`

Фрагментный шейдер градиента неба; используется и для фона сцены.

```cpp
crossrender::Shader sky;
sky.Build(crossrender::builtin::kSkyVert, crossrender::builtin::kSkyFrag, "sky");
crossrender::Environment environment;
sky.Set("uSkyTop", environment.skyTop);
sky.Set("uSkyHorizon", environment.skyHorizon);
sky.Set("uSkyBottom", environment.skyBottom);
```

## Пример целиком

```cpp
#include "crossrender/gfx/Shader.h"

#include "crossrender/core/Log.h"
#include "crossrender/gfx/Texture.h"

#include <vector>

// Собирает программу для спрайтов из строк, добавляет define и выставляет
// униформы всех поддерживаемых типов, включая текстуру.
bool BuildSpriteProgram(crossrender::Shader& out, const crossrender::Texture& atlas) {
    static const char* kVert = R"(
layout(location = 0) in vec3 aPos;
layout(location = 2) in vec2 aUV;
uniform mat4 uViewProj;
uniform vec2 uUvOffset;
out vec2 vUV;
void main() {
    vUV = aUV + uUvOffset;
    gl_Position = uViewProj * vec4(aPos, 1.0);
})";
    static const char* kFrag = R"(
in vec2 vUV;
uniform sampler2D uAtlas;
uniform vec4 uTint;
uniform float uAlpha;
uniform int uMode;
out vec4 FragColor;
void main() {
    vec4 c = texture(uAtlas, vUV) * uTint;
    FragColor = vec4(c.rgb, c.a * uAlpha);
})";

    out.AddDefine("SPRITE_TINT 1");
    if (!out.Build(kVert, kFrag, "sprite")) {
        ENG_LOGE("demo", "сборка не удалась: %s", out.Log().c_str());
        return false;
    }

    // Все поддерживаемые типы униформ.
    out.Set("uMode", 1);                                     // i32
    out.Set("uAlpha", 0.8f);                                 // f32
    out.Set("uUvOffset", crossrender::Vec2{0.0f, 0.0f});             // Vec2
    out.Set("uTint", crossrender::Color::White);                     // Color -> vec4
    out.Set("uViewProj", crossrender::Mat4::Ortho2D(1280.0f, 720.0f));  // Mat4

    const float normalMatrix[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    out.Set("uNormalMatrix", normalMatrix);                  // float[9] -> mat3

    std::vector<crossrender::Vec3> offsets = {{0, 0, 0}, {1, 0, 0}};
    out.Set("uOffsets", offsets);                            // массив vec3

    // Текстура: SetTexture и привязывает юнит, и пишет номер в сэмплер.
    out.SetTexture("uAtlas", atlas, 0);

    // Преамбула подставляется автоматически, потому что исходники выше
    // не начинаются с #version.
    ENG_LOGD("demo", "преамбула: %s", crossrender::builtin::Preamble());
    return out.Valid();
}
```

## См. также

* `docs/core/Base.md` — типы, на которых построены `UniformValue` и векторы.
* `docs/core/Log.md` — куда попадают ошибки компиляции шейдеров.
* `docs/gfx/Texture.md` — `Texture::SetSdfParams`, `SdfSpread` и привязка
  текстур к юнитам.
* `docs/gfx/Mesh.md` — layout вершинных атрибутов (`location` 0..5) и
  слоты текстур `Material`.
* `docs/gfx/RenderTarget.md` — постобработка, использующая встроенные
  шейдеры `kPostVert` / `kFxaaFrag` / `kBloomDownFrag` / `kBloomUpFrag` /
  `kTonemapFrag`.
