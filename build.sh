#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# Скрипт сборки CrossRender - собирает движок, пример и тесты для всех
# поддерживаемых платформ из этого каталога.
#
#   ./build.sh                 # хост-платформа, Release, пример + тесты
#   ./build.sh run             # хост-платформа, сборка и запуск примера
#   ./build.sh test            # хост-платформа, сборка и запуск всех тестов
#   ./build.sh ios             # статическая библиотека iOS + Xcode-проект
#   ./build.sh wasm            # сборка Emscripten / WebGL2 (нужен emsdk)
#   ./build.sh android         # разделяемая библиотека Android (нужен NDK)
#   ./build.sh linux           # Linux (хост или кросс через CC/CXX)
#   ./build.sh windows         # Windows (кросс MinGW-w64 или хостовый MSVC)
#   ./build.sh all             # все платформы, доступные на этой машине
#
# Опции (в любом месте командной строки):
#   --debug          сборка Debug
#   --release        сборка Release (по умолчанию)
#   --no-example     пропустить пример-приложение
#   --no-tests       пропустить набор тестов
#   --asan           AddressSanitizer + UBSan
#   --metal          бэкенд слоя совместимости с Metal (сборки macOS/iOS)
#   --asset-zip      упаковать ассеты игры в assets.zip (нативные платформы)
#   --gzip           .gz-сайдкары для файлов WASM-сборки (wasm/js/data/html)
#   --brotli         .br-сайдкары для файлов WASM-сборки (wasm/js/data/html)
#   --jobs N         параллельные задачи сборки (по умолчанию: определённые ядра)
#   --clean          сначала удалить каталог сборки
# ---------------------------------------------------------------------------
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT"

TARGET="host"
BUILD_TYPE="Release"
BUILD_EXAMPLE="ON"
BUILD_TESTS="ON"
ASAN="OFF"
METAL="OFF"
ASSET_ZIP="OFF"
WASM_GZIP="OFF"
WASM_BROTLI="OFF"
JOBS=""
CLEAN="0"
RUN_AFTER="0"

detect_jobs() {
    if command -v sysctl >/dev/null 2>&1; then
        sysctl -n hw.ncpu 2>/dev/null || echo 4
    elif command -v nproc >/dev/null 2>&1; then
        nproc
    else
        echo 4
    fi
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        host|macos|linux|windows|wasm|ios|android|all) TARGET="$1" ;;
        run) RUN_AFTER="1" ;;
        test) BUILD_TESTS="ON"; RUN_AFTER="2" ;;
        --debug) BUILD_TYPE="Debug" ;;
        --release) BUILD_TYPE="Release" ;;
        --no-example) BUILD_EXAMPLE="OFF" ;;
        --no-tests) BUILD_TESTS="OFF" ;;
        --asan) ASAN="ON" ;;
        --metal) METAL="ON" ;;
        --asset-zip) ASSET_ZIP="ON" ;;
        --gzip) WASM_GZIP="ON" ;;
        --brotli) WASM_BROTLI="ON" ;;
        --clean) CLEAN="1" ;;
        --jobs) shift; JOBS="$1" ;;
        --jobs=*) JOBS="${1#*=}" ;;
        -h|--help)
            sed -n '2,30p' "$0" | sed 's/^# \{0,1\}//'
            exit 0
            ;;
        *) echo "build.sh: unknown argument '$1' (try --help)" >&2; exit 2 ;;
    esac
    shift
done

[[ -z "$JOBS" ]] && JOBS="$(detect_jobs)"

log()  { printf '\033[1;34m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m!!!\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[1;31mxxx\033[0m %s\n' "$*" >&2; exit 1; }

host_platform() {
    case "$(uname -s)" in
        Darwin) echo "macos" ;;
        Linux)  echo "linux" ;;
        MINGW*|MSYS*|CYGWIN*) echo "windows" ;;
        *) echo "unknown" ;;
    esac
}

# ---------------------------------------------------------------------------

# ---------------------------------------------------------------------------
# Общая конфигурация и сборка CMake
# ---------------------------------------------------------------------------
cmake_build() {
    local name="$1"; shift
    local dir="$ROOT/build/$name"
    local -a args=("$@")

    if [[ "$CLEAN" == "1" ]]; then
        log "Cleaning $dir"
        rm -rf "$dir"
    fi
    mkdir -p "$dir"

    log "Configuring $name ($BUILD_TYPE)"
    cmake -S "$ROOT" -B "$dir" \
        -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
        -DCR_BUILD_EXAMPLE="$BUILD_EXAMPLE" \
        -DCR_BUILD_TESTS="$BUILD_TESTS" \
        -DCR_ASAN="$ASAN" \
        -DCR_METAL="$METAL" \
        -DCR_ASSET_ZIP="$ASSET_ZIP" \
        "${args[@]+"${args[@]}"}"

    log "Building $name with $JOBS jobs"
    cmake --build "$dir" --parallel "$JOBS"

    echo "$dir"
}

# ---------------------------------------------------------------------------
# Платформенные драйверы
# ---------------------------------------------------------------------------
build_host() {
    local dir
    dir="$(cmake_build host)" >/dev/null
    dir="$ROOT/build/host"
    log "Build directory: $dir"

    if [[ "$RUN_AFTER" == "2" ]]; then
        local t="$dir/bin/crossrender_tests"
        [[ -x "$t" ]] || die "test binary not found at $t"
        log "Running tests"
        "$t"
    elif [[ "$RUN_AFTER" == "1" ]]; then
        local exe="$dir/bin/crossrender_example"
        [[ -x "$exe" ]] || die "example binary not found at $exe"
        log "Launching the example"
        "$exe" "$@"
    fi
}

build_wasm() {
    if ! command -v emcmake >/dev/null 2>&1; then
        if [[ -f "$HOME/emsdk/emsdk_env.sh" ]]; then
            # shellcheck disable=SC1091
            source "$HOME/emsdk/emsdk_env.sh" >/dev/null 2>&1 || true
        fi
    fi
    command -v emcmake >/dev/null 2>&1 || die "emcmake not found. Install emsdk and source emsdk_env.sh"

    local dir="$ROOT/build/wasm"
    [[ "$CLEAN" == "1" ]] && rm -rf "$dir"
    mkdir -p "$dir"
    log "Configuring WebAssembly (WebGL2)"
    emcmake cmake -S "$ROOT" -B "$dir" \
        -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
        -DCR_BUILD_EXAMPLE="$BUILD_EXAMPLE" \
        -DCR_WASM_GZIP="$WASM_GZIP" \
        -DCR_WASM_BROTLI="$WASM_BROTLI" \
        -DCR_BUILD_TESTS=OFF \
        -DCR_ASSET_ZIP="$ASSET_ZIP"
    log "Building WebAssembly"
    cmake --build "$dir" --parallel "$JOBS"
    if [[ "$WASM_GZIP" == "ON" || "$WASM_BROTLI" == "ON" ]]; then
        log "Serve it with:  python3 tools/serve_wasm.py $dir/bin 8080"
        log "then open http://localhost:8080/crossrender_example.html  (.gz/.br served with Content-Encoding)"
    else
        log "Serve it with:  (cd $dir/bin && python3 -m http.server 8080)  then open http://localhost:8080/crossrender_example.html"
    fi
}

build_ios() {
    [[ "$(uname -s)" == "Darwin" ]] || die "iOS builds require macOS"
    local dir="$ROOT/build/ios"
    [[ "$CLEAN" == "1" ]] && rm -rf "$dir"
    mkdir -p "$dir"
    log "Configuring iOS (arm64, Metal layer available via --metal)"
    cmake -S "$ROOT" -B "$dir" -G Xcode \
        -DCMAKE_SYSTEM_NAME=iOS \
        -DCMAKE_OSX_ARCHITECTURES=arm64 \
        -DCMAKE_OSX_DEPLOYMENT_TARGET=13.0 \
        -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
        -DCR_BUILD_EXAMPLE="$BUILD_EXAMPLE" \
        -DCR_BUILD_TESTS=OFF \
        -DCR_METAL="$METAL" \
        -DCR_ASSET_ZIP="$ASSET_ZIP"
    log "Building iOS"
    cmake --build "$dir" --config "$BUILD_TYPE" --parallel "$JOBS" || \
        warn "Xcode build failed (open $dir/crossrender.xcodeproj to inspect)"
    log "Open $dir/crossrender.xcodeproj to run on a device or simulator"
}

build_android() {
    : "${ANDROID_NDK_HOME:=${ANDROID_NDK_ROOT:-}}"
    if [[ -z "${ANDROID_NDK_HOME}" ]]; then
        for candidate in "$HOME/Library/Android/sdk/ndk"/* "$HOME/Android/Sdk/ndk"/*; do
            [[ -d "$candidate" ]] && ANDROID_NDK_HOME="$candidate"
        done
    fi
    [[ -n "${ANDROID_NDK_HOME}" && -d "${ANDROID_NDK_HOME}" ]] || \
        die "Android NDK not found. Set ANDROID_NDK_HOME."

    local dir="$ROOT/build/android"
    [[ "$CLEAN" == "1" ]] && rm -rf "$dir"
    mkdir -p "$dir"
    log "Configuring Android (arm64-v8a, OpenGL ES 3) with NDK at $ANDROID_NDK_HOME"
    cmake -S "$ROOT" -B "$dir" \
        -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake" \
        -DANDROID_ABI=arm64-v8a \
        -DANDROID_PLATFORM=android-24 \
        -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
        -DCR_BUILD_EXAMPLE="$BUILD_EXAMPLE" \
        -DCR_BUILD_TESTS=OFF \
        -DCR_ASSET_ZIP="$ASSET_ZIP"
    log "Building Android"
    cmake --build "$dir" --parallel "$JOBS"
    log "Result: $dir/bin/libcrossrender_example.so (wrap with an APK to install)"
}

build_linux() {
    local dir="$ROOT/build/linux"
    [[ "$CLEAN" == "1" ]] && rm -rf "$dir"
    mkdir -p "$dir"
    log "Configuring Linux (X11 + GLX, OpenGL 3.3 core)"
    cmake -S "$ROOT" -B "$dir" \
        -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
        -DCR_BUILD_EXAMPLE="$BUILD_EXAMPLE" \
        -DCR_BUILD_TESTS="$BUILD_TESTS"
    log "Building Linux"
    cmake --build "$dir" --parallel "$JOBS"
}

build_windows() {
    local dir="$ROOT/build/windows"
    [[ "$CLEAN" == "1" ]] && rm -rf "$dir"
    mkdir -p "$dir"
    if [[ "$(host_platform)" == "windows" ]]; then
        log "Configuring Windows (Win32 + WGL, OpenGL 3.3 core)"
        cmake -S "$ROOT" -B "$dir" \
            -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
            -DCR_BUILD_EXAMPLE="$BUILD_EXAMPLE" \
            -DCR_BUILD_TESTS="$BUILD_TESTS"
        cmake --build "$dir" --config "$BUILD_TYPE" --parallel "$JOBS"
    else
        command -v x86_64-w64-mingw32-g++ >/dev/null 2>&1 || \
            die "MinGW-w64 not found (brew install mingw-w64). Use a Windows host for an MSVC build."
        log "Cross-compiling Windows with MinGW-w64"
        cmake -S "$ROOT" -B "$dir" \
            -DCMAKE_SYSTEM_NAME=Windows \
            -DCMAKE_C_COMPILER=x86_64-w64-mingw32-gcc \
            -DCMAKE_CXX_COMPILER=x86_64-w64-mingw32-g++ \
            -DCMAKE_RC_COMPILER=x86_64-w64-mingw32-windres \
            -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
            -DCR_BUILD_EXAMPLE="$BUILD_EXAMPLE" \
            -DCR_BUILD_TESTS="$BUILD_TESTS"
        cmake --build "$dir" --parallel "$JOBS"
    fi
}

# ---------------------------------------------------------------------------
main() {
    case "$TARGET" in
        host)
            case "$(host_platform)" in
                macos)   build_host ;;
                linux)   build_linux ;;
                windows) build_windows ;;
                *) die "Unsupported host platform" ;;
            esac
            ;;
        macos)
            [[ "$(host_platform)" == "macos" ]] || die "macOS builds require macOS"
            build_host
            ;;
        linux)   build_linux ;;
        windows) build_windows ;;
        wasm)    build_wasm ;;
        ios)     build_ios ;;
        android) build_android ;;
        all)
            case "$(host_platform)" in
                macos) build_host; build_wasm || warn "WASM build skipped"; build_ios || warn "iOS build skipped" ;;
                linux) build_linux; build_wasm || warn "WASM build skipped"; build_windows || warn "Windows build skipped" ;;
                windows) build_windows ;;
            esac
            ;;
    esac
    log "Done."
}

main
