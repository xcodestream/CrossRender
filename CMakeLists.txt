cmake_minimum_required(VERSION 3.20)
project(CrossRender VERSION 1.0.0 LANGUAGES C CXX)

# Для платформенных бэкендов Apple (.mm-файлы) требуется Objective-C++.
if(APPLE)
    enable_language(OBJCXX)
endif()

# ---------------------------------------------------------------------------
# Глобальные настройки
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)

# Группировать таргеты по папкам в решениях/навигаторах IDE (Xcode, VS).
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_CXX_VISIBILITY_PRESET default)

option(CR_BUILD_EXAMPLE "Build the feature-showcase example" ON)
option(CR_BUILD_TESTS "Build the engine test suite" ON)
option(CR_HEADLESS_ONLY "Build without any windowing backend (CI)" OFF)
option(CR_TEST_INCLUDE_EXAMPLE "Link the example scenes into the test binary (integration test)" ON)
option(CR_ASAN "Enable AddressSanitizer + UBSan" OFF)
option(CR_WERROR "Treat warnings as errors" OFF)
option(CR_METAL "Route rendering through the Metal compatibility layer (Apple only)" OFF)
option(CR_ASSET_ZIP "Package examples/assets into assets.zip beside the app (all platforms except WASM; read back through core/File zip fallback)" OFF)
option(CR_WASM_GZIP "After the WASM build produce .gz sidecars for js/wasm/data/html" OFF)
option(CR_WASM_BROTLI "After the WASM build produce .br sidecars for js/wasm/data/html" OFF)

# Вне Apple слой Metal бессмыслен; отключаем вместо ошибки, чтобы флаг
# можно было безусловно передавать в скриптах.
if(CR_METAL AND NOT APPLE)
    message(WARNING "CR_METAL requires Apple platforms - ignoring")
    set(CR_METAL OFF)
endif()

# Инструменты для пост-сжатия WASM-вывода. Сайдкары раздаёт tools/serve_wasm.py,
# подставляя Content-Encoding по заголовку Accept-Encoding браузера.
if(CR_WASM_GZIP)
    find_program(CR_GZIP_EXE gzip)
    if(NOT CR_GZIP_EXE)
        message(FATAL_ERROR "CR_WASM_GZIP=ON requires the gzip executable on PATH")
    endif()
endif()
if(CR_WASM_BROTLI)
    find_program(CR_BROTLI_EXE brotli)
    if(NOT CR_BROTLI_EXE)
        # Резерв: CLI-утилиты нет, но есть python-модуль brotli (pip install brotli).
        execute_process(COMMAND python3 -c "import brotli"
                        RESULT_VARIABLE CR_BROTLI_PY_RESULT
                        OUTPUT_QUIET ERROR_QUIET)
        if(CR_BROTLI_PY_RESULT EQUAL 0)
            set(CR_BROTLI_PYTHON_EXE python3)
        else()
            message(FATAL_ERROR "CR_WASM_BROTLI=ON requires the brotli executable on PATH "
                                "or the python3 module 'brotli' (pip install brotli)")
        endif()
    endif()
endif()

# ---------------------------------------------------------------------------
# Определение платформы (зеркалит crossrender/core/Base.h)
# ---------------------------------------------------------------------------
if(EMSCRIPTEN)
    set(CR_PLATFORM "WASM")
    # WASM-сборка настроена на размер загрузки: -Oz вместо -O3 по умолчанию,
    # LTO по всему движку и без исключений C++ / RTTI (движок не использует
    # ни то, ни другое; try/catch в платформенных исходниках - это JavaScript
    # внутри EM_ASM-блоков). Линковочные -s* настройки размера - в ветке WASM ниже.
    set(CMAKE_C_FLAGS_RELEASE "-Oz")
    set(CMAKE_CXX_FLAGS_RELEASE "-Oz")
    set(CMAKE_EXE_LINKER_FLAGS_RELEASE "-Oz")
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-rtti -fno-exceptions")
elseif(ANDROID)
    set(CR_PLATFORM "ANDROID")
elseif(IOS)
    set(CR_PLATFORM "IOS")
elseif(APPLE)
    set(CR_PLATFORM "MACOS")
elseif(WIN32)
    set(CR_PLATFORM "WINDOWS")
elseif(UNIX)
    set(CR_PLATFORM "LINUX")
else()
    message(FATAL_ERROR "Unsupported platform for crossrender")
endif()
message(STATUS "CrossRender platform: ${CR_PLATFORM}")

set(CR_ENGINE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/engine)
set(CR_INCLUDE_DIR ${CR_ENGINE_DIR}/include)
set(CR_SRC_DIR ${CR_ENGINE_DIR}/src)
set(CR_THIRD_PARTY ${CR_ENGINE_DIR}/third_party)

# ---------------------------------------------------------------------------
# Исходники
# ---------------------------------------------------------------------------
file(GLOB_RECURSE CR_CORE_SOURCES CONFIGURE_DEPENDS
    ${CR_SRC_DIR}/core/*.cpp
    ${CR_SRC_DIR}/gfx/*.cpp
    ${CR_SRC_DIR}/text/*.cpp
    ${CR_SRC_DIR}/anim/*.cpp
    ${CR_SRC_DIR}/audio/*.cpp
    ${CR_SRC_DIR}/assets/*.cpp
    ${CR_SRC_DIR}/fx/*.cpp
    ${CR_SRC_DIR}/net/*.cpp
    ${CR_SRC_DIR}/scene/*.cpp
    ${CR_SRC_DIR}/ui/*.cpp
    ${CR_SRC_DIR}/voxel/*.cpp
    ${CR_SRC_DIR}/test/*.cpp
)

# Папка платформы: вклад вносит только текущая ОС, остальное игнорируется
# (так работает "весь платформенный код, сгруппированный по папкам с именем ОС").
# engine/src/platform/*.cpp содержит не зависящий от ОС общий код (Input, ...).
file(GLOB CR_PLATFORM_SHARED CONFIGURE_DEPENDS ${CR_SRC_DIR}/platform/*.cpp)
set(CR_PLATFORM_SOURCES)
if(CR_PLATFORM STREQUAL "MACOS")
    file(GLOB_RECURSE CR_PLATFORM_SOURCES CONFIGURE_DEPENDS ${CR_SRC_DIR}/platform/macos/*.mm ${CR_SRC_DIR}/platform/macos/*.cpp)
elseif(CR_PLATFORM STREQUAL "IOS")
    # У iOS своя реализация окна/EGL/звука; папка macOS НЕ компилируется
    # (зависит от AppKit).
    file(GLOB_RECURSE CR_PLATFORM_SOURCES CONFIGURE_DEPENDS ${CR_SRC_DIR}/platform/ios/*.mm ${CR_SRC_DIR}/platform/ios/*.cpp)
elseif(CR_PLATFORM STREQUAL "WINDOWS")
    file(GLOB_RECURSE CR_PLATFORM_SOURCES CONFIGURE_DEPENDS ${CR_SRC_DIR}/platform/windows/*.cpp)
elseif(CR_PLATFORM STREQUAL "LINUX")
    file(GLOB_RECURSE CR_PLATFORM_SOURCES CONFIGURE_DEPENDS ${CR_SRC_DIR}/platform/linux/*.cpp)
elseif(CR_PLATFORM STREQUAL "ANDROID")
    file(GLOB_RECURSE CR_PLATFORM_SOURCES CONFIGURE_DEPENDS ${CR_SRC_DIR}/platform/android/*.cpp)
elseif(CR_PLATFORM STREQUAL "WASM")
    file(GLOB_RECURSE CR_PLATFORM_SOURCES CONFIGURE_DEPENDS ${CR_SRC_DIR}/platform/wasm/*.cpp)
endif()

# Third party (однофайловые библиотеки в репозитории; реализации лежат в
# engine/third_party/stb_impl.c и stb_vorbis.c)
file(GLOB CR_THIRD_PARTY_SOURCES CONFIGURE_DEPENDS
    ${CR_THIRD_PARTY}/stb_impl.c
    ${CR_THIRD_PARTY}/minimp3_impl.c
    ${CR_THIRD_PARTY}/stb_vorbis.c
)

# Vendored однофайловые библиотеки чинить не наша забота. Глушим предупреждения,
# которые провоцируют их заведомо безопасные паттерны: укорачивание 64→32 в Apple Clang
# срабатывает десятки раз внутри minimp3_ex.h, а намеренная защита от переполнения
# указателя в stb_vorbis.c триггерит -Wtautological-compare на любом Clang, включая Emscripten.
set(CR_THIRD_PARTY_WARNING_FLAGS)
if(APPLE)
    list(APPEND CR_THIRD_PARTY_WARNING_FLAGS -Wno-shorten-64-to-32)
endif()
if(NOT MSVC)
    list(APPEND CR_THIRD_PARTY_WARNING_FLAGS -Wno-tautological-compare)
endif()
if(CR_THIRD_PARTY_WARNING_FLAGS)
    set_source_files_properties(${CR_THIRD_PARTY_SOURCES}
        PROPERTIES COMPILE_OPTIONS "${CR_THIRD_PARTY_WARNING_FLAGS}")
endif()

# Слой совместимости с Metal (engine/metal): чисто C++ ядро преобразований
# (таблицы форматов, GLSL->MSL) собирается на любой платформе и покрыто
# юнит-тестами везде; реализации GL поверх Metal - только Apple, включаются через CR_METAL.
file(GLOB CR_METAL_CORE_SOURCES
    ${CR_ENGINE_DIR}/metal/MetalTypes.cpp
    ${CR_ENGINE_DIR}/metal/ShaderTranslate.cpp)
set(CR_METAL_SOURCES ${CR_METAL_CORE_SOURCES})
if(CR_METAL AND APPLE)
    file(GLOB CR_METAL_BACKEND_SOURCES
        ${CR_ENGINE_DIR}/metal/MetalLoader.cpp
        ${CR_ENGINE_DIR}/metal/GLToMetal.mm)
    list(APPEND CR_METAL_SOURCES ${CR_METAL_BACKEND_SOURCES})
endif()

# Заголовки перечислены как исходники таргета, чтобы IDE-генераторы (Xcode, Visual
# Studio) показывали их в навигаторе рядом с использующим их кодом.
file(GLOB_RECURSE CR_ENGINE_HEADERS CONFIGURE_DEPENDS
    ${CR_INCLUDE_DIR}/crossrender/*.h
    ${CR_SRC_DIR}/*.h
    ${CR_ENGINE_DIR}/metal/*.h
)

set(CR_ALL_SOURCES
    ${CR_CORE_SOURCES}
    ${CR_PLATFORM_SHARED}
    ${CR_PLATFORM_SOURCES}
    ${CR_THIRD_PARTY_SOURCES}
    ${CR_ENGINE_HEADERS}
    ${CR_METAL_SOURCES}
)

add_library(crossrender STATIC ${CR_ALL_SOURCES})

# Зеркалим дерево на диске в навигаторах IDE вместо одного плоского списка.
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${CR_ALL_SOURCES})

target_include_directories(crossrender PUBLIC ${CR_INCLUDE_DIR} PRIVATE ${CR_THIRD_PARTY} ${CR_SRC_DIR} ${CR_ENGINE_DIR}/metal)
target_compile_definitions(crossrender PRIVATE ENG_BUILD_LIB=1)
set_target_properties(crossrender PROPERTIES FOLDER "Engine")

if(CR_METAL AND APPLE)
    target_compile_definitions(crossrender PUBLIC ENG_METAL=1)
    if(CR_PLATFORM STREQUAL "MACOS")
        find_library(METAL_FRAMEWORK Metal REQUIRED)
        find_library(QUARTZCORE_FRAMEWORK QuartzCore REQUIRED)
        target_link_libraries(crossrender PUBLIC ${METAL_FRAMEWORK} ${QUARTZCORE_FRAMEWORK})
    else()
        target_link_libraries(crossrender PUBLIC "-framework Metal" "-framework QuartzCore"
                                     "-framework Foundation")
    endif()
endif()

# ---------------------------------------------------------------------------
# Платформенно-зависимая линковка / флаги
# ---------------------------------------------------------------------------
if(CR_PLATFORM STREQUAL "MACOS")
    find_library(COCOA_LIBRARY Cocoa REQUIRED)
    find_library(OPENGL_LIBRARY OpenGL REQUIRED)
    find_library(IOKIT_LIBRARY IOKit REQUIRED)
    find_library(COREVIDEO_LIBRARY CoreVideo REQUIRED)
    find_library(AUDIOTOOLBOX_LIBRARY AudioToolbox REQUIRED)
    find_library(COREAUDIO_LIBRARY CoreAudio REQUIRED)
    find_library(COREFOUNDATION_LIBRARY CoreFoundation REQUIRED)
    find_library(COREGRAPHICS_LIBRARY CoreGraphics REQUIRED)
    find_library(METAL_LIBRARY Metal)
    target_link_libraries(crossrender PUBLIC
        ${COCOA_LIBRARY} ${OPENGL_LIBRARY} ${IOKIT_LIBRARY} ${COREVIDEO_LIBRARY}
        ${AUDIOTOOLBOX_LIBRARY} ${COREAUDIO_LIBRARY} ${COREFOUNDATION_LIBRARY}
        ${COREGRAPHICS_LIBRARY})
    if(METAL_LIBRARY)
        target_link_libraries(crossrender PUBLIC ${METAL_LIBRARY})
    endif()
    set_source_files_properties(${CR_PLATFORM_SOURCES} PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    target_compile_options(crossrender PRIVATE $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
elseif(CR_PLATFORM STREQUAL "IOS")
    set_source_files_properties(${CR_PLATFORM_SOURCES} PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    target_compile_options(crossrender PRIVATE $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
    target_link_libraries(crossrender PUBLIC
        "-framework Foundation" "-framework UIKit" "-framework OpenGLES"
        "-framework QuartzCore" "-framework CoreGraphics" "-framework AVFoundation"
        "-framework AudioToolbox" "-framework CoreAudio" "-framework CoreMotion")
elseif(CR_PLATFORM STREQUAL "WINDOWS")
    target_link_libraries(crossrender PUBLIC opengl32 winmm ws2_32 user32 gdi32 shell32 ole32)
elseif(CR_PLATFORM STREQUAL "LINUX")
    find_package(Threads REQUIRED)
    find_package(X11)
    find_package(OpenGL REQUIRED)
    target_link_libraries(crossrender PUBLIC ${OPENGL_OpenGL_LIBRARIES} Threads::Threads ${CMAKE_DL_LIBS})
    if(X11_FOUND)
        target_link_libraries(crossrender PUBLIC X11::X11)
    endif()
elseif(CR_PLATFORM STREQUAL "ANDROID")
    find_library(ANDROID_LIB android REQUIRED)
    find_library(LOG_LIB log REQUIRED)
    find_library(EGL_LIB EGL REQUIRED)
    find_library(GLES_LIB GLESv3 REQUIRED)
    find_library(OPENSL_LIB OpenSLES REQUIRED)
    find_library(AAUDIO_LIB aaudio)
    target_link_libraries(crossrender PUBLIC ${ANDROID_LIB} ${LOG_LIB} ${EGL_LIB} ${GLES_LIB} ${OPENSL_LIB})
    if(AAUDIO_LIB)
        target_link_libraries(crossrender PUBLIC ${AAUDIO_LIB})
    endif()
elseif(CR_PLATFORM STREQUAL "WASM")
    # Настройки -s* в Emscripten действуют только на этапе линковки; передача их
    # на этап компиляции вызывает предупреждение em++ "linker setting ignored".
    target_link_libraries(crossrender PUBLIC "-sUSE_WEBGL2=1" "-sFULL_ES3=1" "-sUSE_GLFW=0")
endif()

if(CR_ASAN)
    target_compile_options(crossrender PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(crossrender PRIVATE -fsanitize=address,undefined)
endif()

if(CR_WERROR)
    if(MSVC)
        target_compile_options(crossrender PRIVATE /W4 /WX)
    else()
        target_compile_options(crossrender PRIVATE -Wall -Wextra -Werror)
    endif()
endif()

# Предупреждения, которые в кодовой базе такого размера - чистый шум.
if(NOT MSVC)
    target_compile_options(crossrender PRIVATE -Wno-unused-parameter -Wno-missing-field-initializers)
endif()
# Apple объявила OpenGL-фреймворк устаревшим в 10.14; движок там намеренно
# нацелен на GL 3.3 core, поэтому шум от deprecation подавляется.
if(APPLE)
    target_compile_options(crossrender PRIVATE -Wno-deprecated-declarations)
endif()

# ---------------------------------------------------------------------------
# Пример приложения
# ---------------------------------------------------------------------------
if(CR_BUILD_EXAMPLE)
    file(GLOB_RECURSE CR_EXAMPLE_SOURCES CONFIGURE_DEPENDS
        ${CMAKE_CURRENT_SOURCE_DIR}/examples/sources/*.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/examples/sources/*.h)
    add_executable(crossrender_example ${CR_EXAMPLE_SOURCES})
    target_link_libraries(crossrender_example PRIVATE crossrender)
    set_target_properties(crossrender_example PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
        FOLDER "Examples")
    source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${CR_EXAMPLE_SOURCES})
    target_include_directories(crossrender_example PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/examples/sources)
    if(CR_PLATFORM STREQUAL "WASM")
        # Настройки линковки с прицелом на размер: клей только для браузера,
        # компактный аллокатор emmalloc, минифицированный Closure JS и агрессивная чистка wasm-переменных.
        set_target_properties(crossrender_example PROPERTIES
            SUFFIX ".html"
            LINK_FLAGS "--preload-file ${CMAKE_CURRENT_SOURCE_DIR}/examples/assets@/assets \
-sALLOW_MEMORY_GROWTH=1 -sEXIT_RUNTIME=1 \
-sENVIRONMENT=web -sMALLOC=emmalloc -sAGGRESSIVE_VARIABLE_ELIMINATION=1 \
--closure 1 --shell-file ${CMAKE_CURRENT_SOURCE_DIR}/cmake/shell.html")
        # Пост-сжатие вывода в сайдкары .gz/.br; сами файлы остаются несжатыми -
        # выбор кодировки происходит на стороне tools/serve_wasm.py.
        if(CR_WASM_GZIP OR CR_WASM_BROTLI)
            add_custom_command(TARGET crossrender_example POST_BUILD
                COMMAND ${CMAKE_COMMAND}
                    -DOUTDIR=$<TARGET_FILE_DIR:crossrender_example>
                    -DGZIP_EXE=${CR_GZIP_EXE}
                    -DBROTLI_EXE=${CR_BROTLI_EXE}
                    -DBROTLI_PYTHON=${CR_BROTLI_PYTHON_EXE}
                    -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/wasm_compress.cmake
                VERBATIM)
        endif()
    elseif(CR_ASSET_ZIP)
        # Упаковка ассетов игры в assets.zip рядом с приложением. Движок читает
        # архив через собственный DEFLATE-декодер (core/Zip) с прозрачным
        # откатом в core/File - отдельно распаковывать ничего не нужно.
        find_program(CR_PYTHON_EXE python3)
        if(NOT CR_PYTHON_EXE)
            message(FATAL_ERROR "CR_ASSET_ZIP=ON requires python3 on PATH")
        endif()
        add_custom_command(TARGET crossrender_example POST_BUILD
            COMMAND "${CR_PYTHON_EXE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tools/make_asset_zip.py"
                    "${CMAKE_CURRENT_SOURCE_DIR}/examples/assets"
                    "$<TARGET_FILE_DIR:crossrender_example>/assets.zip"
            VERBATIM)
    endif()
endif()

# ---------------------------------------------------------------------------
# Тесты
# ---------------------------------------------------------------------------
if(CR_BUILD_TESTS)
    enable_testing()
    file(GLOB_RECURSE CR_TEST_SOURCES CONFIGURE_DEPENDS
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/*.h)
    # Сценный фреймворк примера + сцены линкуются, чтобы tests/test_example.cpp
    # мог в headless-режиме запустить каждую зарегистрированную сцену (main.cpp исключён).
    file(GLOB CR_EXAMPLE_TEST_SOURCES CONFIGURE_DEPENDS
        ${CMAKE_CURRENT_SOURCE_DIR}/examples/sources/Mega.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/examples/sources/scenes/*.cpp)
    # Задайте CR_TEST_INCLUDE_EXAMPLE=OFF, чтобы собрать юнит-тесты без сцен
    # примера (удобно, пока сцена ещё пишется).
    if(CR_TEST_INCLUDE_EXAMPLE)
        list(APPEND CR_TEST_SOURCES ${CR_EXAMPLE_TEST_SOURCES})
    else()
        # test_example.cpp управляет реестром сцен примера, поэтому ему нужны
        # исходники примера; исключаем его, когда они не слинкованы.
        list(FILTER CR_TEST_SOURCES EXCLUDE REGEX "test_example\\.cpp$")
    endif()
    add_executable(crossrender_tests ${CR_TEST_SOURCES})
    target_link_libraries(crossrender_tests PRIVATE crossrender)
    target_include_directories(crossrender_tests PRIVATE ${CR_SRC_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/examples/sources ${CR_ENGINE_DIR}/metal)
    set_target_properties(crossrender_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
        FOLDER "Tests")
    source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${CR_TEST_SOURCES})
    add_test(NAME engine_tests COMMAND crossrender_tests)
endif()

message(STATUS "crossrender configured (example=${CR_BUILD_EXAMPLE} tests=${CR_BUILD_TESTS})")
