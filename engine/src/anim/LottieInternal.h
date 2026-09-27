//
// CrossRender
// Автор: xcodestream <xcodestream@gmail.com>
// Лицензия: MIT
// Дата: 15.05.2026
// Описание: внутреннее состояние реализации Lottie: данные фигур и ключевых кадров парсера.
//
#pragma once

#include "crossrender/anim/Lottie.h"

#include <string>
#include <vector>
#include <unordered_map>

namespace crossrender {

// ---------------------------------------------------------------------------
// Служебные данные геометрии
// ---------------------------------------------------------------------------
// Одна кубическая траектория с семантикой рычагов bodymovin: кривая из v[k] в
// v[k+1] задана как v[k] + o[k] -> v[k+1] + i[k+1] (абсолютные контрольные точки).
struct LottiePathHandle {
    std::vector<Vec2> v, i, o;
    bool closed = false;
};

// Цветовые стопы градиента + геометрия для заливок `gf`/`gs`.
struct LottieGradient {
    std::vector<f32> offsets;
    std::vector<Color> colors;
    std::vector<f32> opacities;  // на каждый стоп, 0..1
    bool valid() const {
        return !offsets.empty() && colors.size() == offsets.size();
    }
};

// Служебные данные текстового слоя (текстовый документ + аниматоры).
struct LottieTextAnimator {
    f32 start = 0, end = 1;
    bool hasTracking = false;
    LottieProperty tracking;
    bool hasFill = false;
    LottieProperty fillColor;
    bool hasOpacity = false;
    LottieProperty opacity;
};

struct LottieTextInfo {
    std::string text;
    std::string fontFamily;
    f32 fontSize = 24;
    Color fill = Color::White;
    Color stroke = Color::Black;
    f32 strokeWidth = 0;
    int justification = 0;
    f32 tracking = 0;
    f32 lineHeight = 0;
    bool strokeOverFill = false;
    std::vector<LottieTextAnimator> animators;
};

// ---------------------------------------------------------------------------
// Черновые данные времени парсинга
// ---------------------------------------------------------------------------
// Собираются по индексам, потому что объекты фигур/слоёв перемещаются (и меняют
// адрес) в процессе сборки в принадлежащие им векторы. Проход после парсинга в
// порядке разбора связывает каждую черновую запись с её финальным объектом.
struct LottieLayerScratch {
    bool hasText = false;
    LottieTextInfo text;
    bool hasTimeRemap = false;
    LottieProperty timeRemap;
    int blendMode = 0;
    // Маски, по порядку (параллельно `LottieLayer::masks`).
    std::vector<LottiePathHandle> maskPaths;
};

// Одна запись на каждый вызов `ParseShape`, в порядке разбора (включая
// трансформ-шейпы — они просто не несут неподдерживаемых данных).
struct LottieShapeScratch {
    LottieGradient gradient;
    bool hasGradient = false;
    LottieProperty offset;
    bool hasOffset = false;
    int mergeMode = 0;
    LottiePathHandle path;
    bool hasPath = false;
};

// Определение вложенного типа реализации, объявленного (но не определённого) в
// замороженном публичном заголовке.
using LottieImpl = LottieAnimation::Impl;

struct LottieAnimation::Impl {
    // Истинно, когда плеер ограничен явным сегментом [start,end].
    bool hasSegment = false;
    // Ассеты, индексированные по id (родители/мэтты находят слои по индексу bodymovin).
    std::unordered_map<int, const LottieAsset*> assetsById;

    // Боковые таблицы с ключами-адресами (безопасно: заполняются только после
    // того, как дерево объектов заняло финальное хранилище, и далее не меняются).
    std::unordered_map<const LottieShape*, LottieGradient> gradients;
    std::unordered_map<const LottieShape*, LottieProperty> offsetPaths;
    std::unordered_map<const LottieShape*, int> mergeModes;
    std::unordered_map<const LottieProperty*, LottiePathHandle> paths;
    std::unordered_map<const LottieLayer*, LottieTextInfo> text;
    std::unordered_map<const LottieLayer*, LottieProperty> timeRemap;
    std::unordered_map<const LottieLayer*, int> blendModes;
    std::unordered_map<const LottieLayer*, int> matteMode;

    // ---- черновые данные парсинга (очищаются после регистрации) -----------
    std::vector<LottieShapeScratch> shapeSlots;
    std::vector<LottieLayerScratch> layerSlots;

    void Clear() {
        assetsById.clear();
        gradients.clear();
        offsetPaths.clear();
        mergeModes.clear();
        paths.clear();
        text.clear();
        timeRemap.clear();
        blendModes.clear();
        matteMode.clear();
        shapeSlots.clear();
        layerSlots.clear();
        hasSegment = false;
    }
};

}  // namespace crossrender
