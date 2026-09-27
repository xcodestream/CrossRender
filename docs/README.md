# Документация CrossRender

Полное описание публичного API движка на русском языке. Каждый файл
документирует ровно один заголовок из `engine/include` и содержит пример кода
для **каждого** публичного члена класса.

## Как читать

* Файлы повторяют дерево заголовков: `engine/include/crossrender/gfx/Texture.h`
  описывается в `docs/gfx/Texture.md`.
* В каждом файле есть разделы `## Обзор` (как устроено и когда использовать),
  `## Члены класса` (по разделу на каждый публичный член, с примером),
  `## Пример целиком` и `## См. также`.
* Формат и правила описаны в [STYLE.md](STYLE.md).
* Проверить, что документация не разошлась с кодом, можно так:

```bash
python3 tools/doccheck.py -v
```

Скрипт проверяет, что текст на русском, что у каждого подраздела есть пример,
и что все упомянутые идентификаторы действительно есть в заголовках.

## Быстрый старт

Если вы впервые в этом движке, читайте в таком порядке:

1. `core/Base.md` — типы, платформы, базовые макросы.
2. `core/Log.md` — как движок сообщает о проблемах.
3. `core/File.md` — как читать и писать файлы, где лежат ассеты.
4. `Engine.md` — как устроен главный цикл и конфигурация.
5. `scene/Scene.md` — как написать свою сцену.
6. `gfx/Renderer2D.md` — как рисовать интерфейс и 2D-графику.
7. `gfx/Renderer3D.md` — как рисовать 3D.

## Ядро

| Файл | Заголовок | Что описывает |
|---|---|---|
| [core/Base.md](core/Base.md) | `crossrender/core/Base.h` | платформы, типы `u8`/`f32`/`usize`, константы, `ScopeExit` |
| [core/Log.md](core/Log.md) | `crossrender/core/Log.h` | уровни логирования, приёмники, история, `ENG_ASSERT` |
| [core/File.md](core/File.md) | `crossrender/core/File.h` | файловая система, корни `assets`/`user`, пути |
| [core/Json.md](core/Json.md) | `crossrender/core/Json.h` | разбор и запись JSON |
| [core/Math.md](core/Math.md) | `crossrender/core/Math.h` | векторы, матрицы, кватернионы, цвета, easing, `Random` |
| [core/Time.md](core/Time.md) | `crossrender/core/Time.h` | время, `Clock`, дельта кадра, сон |

## Графика

| Файл | Заголовок | Что описывает |
|---|---|---|
| [gfx/GL.md](gfx/GL.md) | `crossrender/gfx/GL.h` | собственный загрузчик OpenGL/GLES |
| [gfx/Texture.md](gfx/Texture.md) | `crossrender/gfx/Texture.h` | текстуры: создание, загрузка, обновление, чтение |
| [gfx/Shader.md](gfx/Shader.md) | `crossrender/gfx/Shader.h` | сборка шейдеров и установка uniform-ов |
| [gfx/Mesh.md](gfx/Mesh.md) | `crossrender/gfx/Mesh.h` | `MeshData`, примитивы, `Material`, `Camera`, `Light`, `Environment` |
| [gfx/RenderTarget.md](gfx/RenderTarget.h) | `crossrender/gfx/RenderTarget.h` | рендер-таргеты и постобработка |
| [gfx/Renderer2D.md](gfx/Renderer2D.md) | `crossrender/gfx/Renderer2D.h` | 2D-рендерер: пути, заливки, текст, отсечение |
| [gfx/Renderer3D.md](gfx/Renderer3D.md) | `crossrender/gfx/Renderer3D.h` | 3D-рендерер: проходы, тени, освещение, лучи |
| [gfx/Retro.md](gfx/Retro.md) | `crossrender/gfx/Retro.h` | пиксельный и ASCII-режимы, палитры, дизеринг, CRT |
| [gfx/SpriteAtlas.md](gfx/SpriteAtlas.md) | `crossrender/gfx/SpriteAtlas.h` | спрайтовые атласы, форматы описаний, упаковка |
| [gfx/FilterChain.md](gfx/FilterChain.h) | `crossrender/gfx/FilterChain.h` | цепочка пост-фильтров |
| [gfx/SlugText.md](gfx/SlugText.h) | `crossrender/gfx/SlugText.h` | векторный текст по контурам глифов |

## Текст, интерфейс, анимация

| Файл | Заголовок | Что описывает |
|---|---|---|
| [text/Font.md](text/Font.md) | `crossrender/text/Font.h` | TTF/OTF: растеризация, SDF, контуры, метрики |
| [ui/Ui.md](ui/Ui.md) | `crossrender/ui/Ui.h` | виджеты: кнопки, списки, поля, панели, тема |
| [anim/Anim.md](anim/Anim.md) | `crossrender/anim/Anim.h` | скелет, клипы, треки, смешивание, пружины |
| [anim/Lottie.md](anim/Lottie.md) | `crossrender/anim/Lottie.h` | проигрывание Lottie-анимаций |
| [fx/Particles.md](fx/Particles.h) | `crossrender/fx/Particles.h` | системы частиц |

## Звук

| Файл | Заголовок | Что описывает |
|---|---|---|
| [audio/Audio.md](audio/Audio.md) | `crossrender/audio/Audio.h` | микшер, клипы, шины, 3D-звук |
| [audio/Chiptune.md](audio/Chiptune.md) | `crossrender/audio/Chiptune.h` | 8- и 16-битная музыка, трекер, синтез |

## Ассеты, воксели, сеть, платформа

| Файл | Заголовок | Что описывает |
|---|---|---|
| [assets/Model.md](assets/Model.md) | `crossrender/assets/Model.h` | загрузка моделей OBJ/PLY/STL |
| [voxel/Voxel.md](voxel/Voxel.md) | `crossrender/voxel/Voxel.h` | воксельный мир, чанки, мешинг, лучи |
| [net/Net.md](net/Net.md) | `crossrender/net/Net.h` | TCP, UDP, WebSocket, HTTP |
| [platform/Platform.md](platform/Platform.h) | `crossrender/platform/Platform.h` | инициализация платформы, хуки приложения |
| [platform/Window.md](platform/Window.h) | `crossrender/platform/Window.h` | окно, ввод, курсор, DPI |

## Движок и тесты

| Файл | Заголовок | Что описывает |
|---|---|---|
| [Engine.md](Engine.md) | `crossrender/Engine.h` | конфигурация, главный цикл, сцены, скриншоты |
| [Resource.md](Resource.md) | `crossrender/Resource.h` | кэш ресурсов |
| [scene/Scene.md](scene/Scene.h) | `crossrender/scene/Scene.h` | базовый класс сцены, менеджер сцен, переходы |
| [test/Test.md](test/Test.md) | `crossrender/test/Test.h` | регистрация и запуск тестов |

## Примеры

Полный пример использования — приложение `examples/sources`: 46 сцен, каждая
демонстрирует свою часть движка. Список сцен:

```bash
./build/host/bin/gameengine_example --list
```

Запуск конкретной сцены:

```bash
./build/host/bin/gameengine_example --scene earth
```

## Сборка

```bash
./build.sh                 # сборка под текущую платформу
./build.sh run             # собрать и запустить пример
./build.sh test            # собрать и прогнать тесты
./build.sh assets          # перегенерировать ассеты
```
