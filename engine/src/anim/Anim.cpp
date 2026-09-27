// Скелетная анимация: треки ключевых кадров, скелеты, сэмплирование клипов,
// смешивание поз (crossfade / additive / masked / layered), blend-пространства,
// плавные твины, пружины.
//
// Соглашения, используемые по всему файлу:
//   * поза хранит локальные TRS (`translations`/`rotations`/`scales`) плюс
//     собранную локальную матрицу (`locals`) и цепочку мировых матриц (`world`).
//     Любая мутация синхронизирует и то, и другое.
//   * вращения всегда нормализованы; смешивание использует Slerp (короткая дуга).
//   * публичный заголовок заморожен, поэтому каждый хелпер ниже локален для файла,
//     а Animator переиспользует `scratchA_` (сэмпл клипа слоя) и `scratchB_` (снимок незавершённого crossfade) вместо дополнительных полей.
#include "crossrender/anim/Anim.h"

#include "crossrender/core/Log.h"

#include <cmath>
#include <string>
#include <utility>
#include <algorithm>

namespace crossrender {

// Определены здесь (а не ниже), чтобы хелперы запекания клипов могли их
// инстанцировать; специализированные Sample для каждого типа объявлены заранее,
// чтобы неявного инстанцирования не происходило до их определений.
template <typename T>
void Track<T>::AddKey(f32 time, const T& value, Interpolation interp) {
    Keyframe<T> k;
    k.time = time;
    k.value = value;
    k.interp = interp;
    // Ключи хранятся отсортированными (клипы часто собираются из нескольких источников).
    usize lo = 0, hi = keys.size();
    while (lo < hi) {
        const usize mid = (lo + hi) / 2;
        if (keys[mid].time <= time) lo = mid + 1;
        else hi = mid;
    }
    keys.insert(keys.begin() + static_cast<std::ptrdiff_t>(lo), k);
}

template <typename T>
void Track<T>::Sort() {
    std::stable_sort(keys.begin(), keys.end(),
                     [](const Keyframe<T>& a, const Keyframe<T>& b) { return a.time < b.time; });
}

template <>
Vec3 Track<Vec3>::Sample(f32 time) const;
template <>
Quat Track<Quat>::Sample(f32 time) const;
template <>
f32 Track<f32>::Sample(f32 time) const;

namespace {

// ===========================================================================
// Небольшие общие хелперы
// ===========================================================================

// Замыкание в [0, d).
[[nodiscard]] f32 Wrap01(f32 t, f32 d) {
    if (d <= kEpsilon) return 0.0f;
    f32 r = std::fmod(t, d);
    if (r < 0.0f) r += d;
    return r;
}

// Отображение ping-pong: время цикла в [0, 2d) -> время клипа в [0, d].
[[nodiscard]] f32 PingPongTime(f32 cycle, f32 d) {
    if (d <= kEpsilon) return 0.0f;
    const f32 c = Wrap01(cycle, d * 2.0f);
    return c <= d ? c : (d * 2.0f - c);
}

[[nodiscard]] Mat4 ComposeTRS(const Vec3& t, const Quat& r, const Vec3& s) {
    return Mat4::Translate(t) * r.ToMat4() * Mat4::Scale(s);
}

[[nodiscard]] bool MatrixIsIdentity(const Mat4& m) {
    for (int c = 0; c < 4; ++c) {
        for (int r = 0; r < 4; ++r) {
            const f32 want = (c == r) ? 1.0f : 0.0f;
            if (std::fabs(m.at(c, r) - want) > 1e-6f) return false;
        }
    }
    return true;
}

[[nodiscard]] bool TrsIsDefault(const Vec3& t, const Quat& r, const Vec3& s) {
    if (std::fabs(t.x) > 1e-6f || std::fabs(t.y) > 1e-6f || std::fabs(t.z) > 1e-6f) return false;
    if (std::fabs(r.x) > 1e-6f || std::fabs(r.y) > 1e-6f || std::fabs(r.z) > 1e-6f ||
        std::fabs(r.w - 1.0f) > 1e-6f)
        return false;
    if (std::fabs(s.x - 1.0f) > 1e-6f || std::fabs(s.y - 1.0f) > 1e-6f ||
        std::fabs(s.z - 1.0f) > 1e-6f)
        return false;
    return true;
}

// Корректное извлечение вращения для используемого здесь column-major layout Mat4
// (at(c, r) == строка r, столбец c). Примечание: crossrender::Quat::FromMat4 в Math.h путает
// недиагональные члены и даёт обратное вращение, поэтому модуль анимации
// использует эту локальную реализацию (Math.h принадлежит модулю core).
[[nodiscard]] Quat QuatFromMatrix(const Mat4& m) {
    const f32 m00 = m.at(0, 0), m11 = m.at(1, 1), m22 = m.at(2, 2);
    const f32 trace = m00 + m11 + m22;
    Quat q;
    if (trace > 0.0f) {
        const f32 s = std::sqrt(trace + 1.0f) * 2.0f;  // 4w
        q.w = 0.25f * s;
        q.x = (m.at(1, 2) - m.at(2, 1)) / s;
        q.y = (m.at(2, 0) - m.at(0, 2)) / s;
        q.z = (m.at(0, 1) - m.at(1, 0)) / s;
    } else if (m00 > m11 && m00 > m22) {
        const f32 s = std::sqrt(1.0f + m00 - m11 - m22) * 2.0f;  // 4x
        q.x = 0.25f * s;
        q.y = (m.at(1, 0) + m.at(0, 1)) / s;
        q.z = (m.at(2, 0) + m.at(0, 2)) / s;
        q.w = (m.at(1, 2) - m.at(2, 1)) / s;
    } else if (m11 > m22) {
        const f32 s = std::sqrt(1.0f + m11 - m00 - m22) * 2.0f;  // 4y
        q.x = (m.at(1, 0) + m.at(0, 1)) / s;
        q.y = 0.25f * s;
        q.z = (m.at(2, 1) + m.at(1, 2)) / s;
        q.w = (m.at(2, 0) - m.at(0, 2)) / s;
    } else {
        const f32 s = std::sqrt(1.0f + m22 - m00 - m11) * 2.0f;  // 4z
        q.x = (m.at(2, 0) + m.at(0, 2)) / s;
        q.y = (m.at(2, 1) + m.at(1, 2)) / s;
        q.z = 0.25f * s;
        q.w = (m.at(0, 1) - m.at(1, 0)) / s;
    }
    return q.Normalized();
}

// Разложение аффинной матрицы на трансляцию / вращение / масштаб (положительные
// масштабы; зеркальный базис сворачивается в отрицательный масштаб по X).
void DecomposeTRS(const Mat4& m, Vec3* t, Quat* r, Vec3* s) {
    *t = Vec3{m.at(3, 0), m.at(3, 1), m.at(3, 2)};
    Vec3 c0{m.at(0, 0), m.at(0, 1), m.at(0, 2)};
    Vec3 c1{m.at(1, 0), m.at(1, 1), m.at(1, 2)};
    Vec3 c2{m.at(2, 0), m.at(2, 1), m.at(2, 2)};
    f32 sx = Length(c0), sy = Length(c1), sz = Length(c2);
    if (sx > kEpsilon) c0 = c0 / sx; else c0 = Vec3{1, 0, 0};
    if (sy > kEpsilon) c1 = c1 / sy; else c1 = Vec3{1, 0, 0};
    if (sz > kEpsilon) c2 = c2 / sz; else c2 = Vec3{0, 0, 1};
    if (Dot(c0, Cross(c1, c2)) < 0.0f) {  // левосторонний базис: отражаем X
        sx = -sx;
        c0 = -c0;
    }
    Mat4 rot;
    rot.at(0, 0) = c0.x; rot.at(0, 1) = c0.y; rot.at(0, 2) = c0.z;
    rot.at(1, 0) = c1.x; rot.at(1, 1) = c1.y; rot.at(1, 2) = c1.z;
    rot.at(2, 0) = c2.x; rot.at(2, 1) = c2.y; rot.at(2, 2) = c2.z;
    *s = Vec3{sx, sy, sz};
    *r = QuatFromMatrix(rot);
}

// ---------------------------------------------------------------------------
// Поиск ключевого кадра в треке
// ---------------------------------------------------------------------------

// Находит сегмент [ia, ib], содержащий `time`, и его нормализованный параметр.
// Времена до первого / после последнего ключа фиксируются на крайних ключах.
template <typename T>
bool FindSegment(const std::vector<Keyframe<T>>& keys, f32 time, usize* ia, usize* ib, f32* t) {
    if (keys.size() < 2) return false;
    if (time <= keys[0].time) {
        *ia = 0;
        *ib = 1;
        *t = 0.0f;
        return true;
    }
    if (time >= keys[keys.size() - 1].time) {
        *ia = keys.size() - 2;
        *ib = keys.size() - 1;
        *t = 1.0f;
        return true;
    }
    usize lo = 0, hi = keys.size() - 1;
    while (lo + 1 < hi) {
        const usize mid = (lo + hi) / 2;
        if (keys[mid].time <= time) lo = mid;
        else hi = mid;
    }
    *ia = lo;
    *ib = lo + 1;
    const f32 span = keys[*ib].time - keys[*ia].time;
    *t = span > kEpsilon ? (time - keys[*ia].time) / span : 1.0f;
    return true;
}

// Базис кубического Эрмита, применённый к сегменту; общий для скалярных/векторных треков.
template <typename T, typename Blend>
[[nodiscard]] T HermiteSegment(const Keyframe<T>& a, const Keyframe<T>& b, f32 t, f32 span, Blend blend) {
    const f32 t2 = t * t;
    const f32 t3 = t2 * t;
    const f32 h00 = 2.0f * t3 - 3.0f * t2 + 1.0f;
    const f32 h10 = t3 - 2.0f * t2 + t;
    const f32 h01 = -2.0f * t3 + 3.0f * t2;
    const f32 h11 = t3 - t2;
    return blend(a.value, h00, a.outTangent, h10 * span, b.value, h01, b.inTangent, h11 * span);
}

[[nodiscard]] bool QuatIsIdentity(const Quat& q) {
    return std::fabs(q.x) < 1e-5f && std::fabs(q.y) < 1e-5f && std::fabs(q.z) < 1e-5f &&
           std::fabs(q.w - 1.0f) < 1e-5f;
}

// ---------------------------------------------------------------------------
// Хелперы поз
// ---------------------------------------------------------------------------

void ComposeLocalAt(Pose* p, usize i) {
    p->locals[i] = ComposeTRS(p->translations[i], p->rotations[i], p->scales[i]);
}

[[nodiscard]] f32 JointWeight(const std::vector<f32>* mask, usize i, f32 weight) {
    if (mask && i < mask->size()) return weight * Clamp((*mask)[i], 0.0f, 1.0f);
    return weight;
}

// dst <- lerp(dst, sample, weight) или аддитивная/мультипликативная композиция
// привязанного к бинду локального дельта-сэмпла, по суставу.
void BlendLayerInto(Pose* dst, const Pose& sample, f32 weight, AnimBlendMode mode,
                    const std::vector<f32>* mask) {
    const usize n = dst->translations.size();
    const usize m = sample.translations.size();
    for (usize i = 0; i < n; ++i) {
        const f32 w = JointWeight(mask, i, weight);
        if (w <= 0.0f || i >= m) continue;
        switch (mode) {
            case AnimBlendMode::Additive:
            case AnimBlendMode::Multiply: {
                // Компонуем сэмпл поверх накопителя. `sample` здесь — локальная
                // дельта относительно бинда: Animator прогоняет сэмплы клипов
                // через MakeBindRelative() перед смешиванием, и публичный
                // AdditivePose() документирует то же соглашение.
                dst->translations[i] = dst->translations[i] + sample.translations[i] * w;
                dst->rotations[i] =
                    (dst->rotations[i] * Quat::Slerp(Quat::Identity(), sample.rotations[i], w)).Normalized();
                dst->scales[i] = dst->scales[i] * Lerp(Vec3{1, 1, 1}, sample.scales[i], w);
                break;
            }
            default: {
                dst->translations[i] = Lerp(dst->translations[i], sample.translations[i], w);
                dst->rotations[i] = Quat::Slerp(dst->rotations[i], sample.rotations[i], w).Normalized();
                dst->scales[i] = Lerp(dst->scales[i], sample.scales[i], w);
                break;
            }
        }
        ComposeLocalAt(dst, i);
    }
}

// Crossfade: результат = lerp(bind, lerp(snapshot, sample, fade), weight).
void CrossfadeInto(Pose* dst, const Skeleton& skeleton, const Pose& snapshot, const Pose& sample, f32 fade,
                   f32 weight, const std::vector<f32>* mask) {
    const usize n = dst->translations.size();
    for (usize i = 0; i < n; ++i) {
        const f32 w = JointWeight(mask, i, weight);
        Vec3 bindT{0, 0, 0};
        Quat bindR = Quat::Identity();
        Vec3 bindS{1, 1, 1};
        if (static_cast<int>(i) < skeleton.JointCount()) {
            const Joint& j = skeleton.JointAt(static_cast<int>(i));
            bindT = j.localTranslation;
            bindR = j.localRotation;
            bindS = j.localScale;
        }
        const Vec3 srcT = i < snapshot.translations.size() ? snapshot.translations[i] : bindT;
        const Quat srcR = i < snapshot.rotations.size() ? snapshot.rotations[i] : bindR;
        const Vec3 srcS = i < snapshot.scales.size() ? snapshot.scales[i] : bindS;
        const Vec3 smpT = i < sample.translations.size() ? sample.translations[i] : bindT;
        const Quat smpR = i < sample.rotations.size() ? sample.rotations[i] : bindR;
        const Vec3 smpS = i < sample.scales.size() ? sample.scales[i] : bindS;

        dst->translations[i] = Lerp(bindT, Lerp(srcT, smpT, fade), w);
        dst->rotations[i] = Quat::Slerp(bindR, Quat::Slerp(srcR, smpR, fade), w).Normalized();
        dst->scales[i] = Lerp(bindS, Lerp(srcS, smpS, fade), w);
        ComposeLocalAt(dst, i);
    }
}

// Заполняет позу бинд-позой (позой покоя) скелета.
void BuildBindPose(const Skeleton& skeleton, Pose* pose) {
    const int n = skeleton.JointCount();
    pose->Resize(n);
    for (int i = 0; i < n; ++i) {
        const Joint& j = skeleton.JointAt(i);
        pose->translations[static_cast<usize>(i)] = j.localTranslation;
        pose->rotations[static_cast<usize>(i)] = j.localRotation;
        pose->scales[static_cast<usize>(i)] = j.localScale;
        pose->locals[static_cast<usize>(i)] = ComposeTRS(j.localTranslation, j.localRotation, j.localScale);
    }
}

// Преобразует сэмплированную позу на месте в дельта-позу относительно бинда,
// в локальном пространстве:  delta.local = bind.local^-1 * sample.local.  Слои
// additive/multiply используют это, чтобы клип в бинд-позе был идеальным no-op.
void MakeBindRelative(const Skeleton& skeleton, Pose* pose) {
    const usize n = pose->translations.size();
    for (usize i = 0; i < n; ++i) {
        Vec3 bindT{0, 0, 0};
        Quat bindR = Quat::Identity();
        Vec3 bindS{1, 1, 1};
        if (static_cast<int>(i) < skeleton.JointCount()) {
            const Joint& j = skeleton.JointAt(static_cast<int>(i));
            bindT = j.localTranslation;
            bindR = j.localRotation;
            bindS = j.localScale;
        }
        const bool degenerate = std::fabs(bindS.x) < kEpsilon || std::fabs(bindS.y) < kEpsilon ||
                                std::fabs(bindS.z) < kEpsilon;
        const Mat4 bindLocal = degenerate ? Mat4::Identity() : ComposeTRS(bindT, bindR, bindS);
        const Mat4 sampleLocal = ComposeTRS(pose->translations[i], pose->rotations[i], pose->scales[i]);
        const Mat4 delta = bindLocal.Inverse() * sampleLocal;
        Vec3 dt{0, 0, 0};
        Quat dr = Quat::Identity();
        Vec3 ds{1, 1, 1};
        DecomposeTRS(delta, &dt, &dr, &ds);
        pose->translations[i] = dt;
        pose->rotations[i] = dr;
        pose->scales[i] = ds;
        pose->locals[i] = ComposeTRS(dt, dr, ds);
    }
}

// ---------------------------------------------------------------------------
// События
// ---------------------------------------------------------------------------

// События строго внутри (from, to]. `looping` разбивает диапазон через стык
// цикла, чтобы событие, пересечённое фреймом ровно один раз, сработало ровно один раз.
void CollectEventsImpl(const AnimationClip& clip, f32 from, f32 to, bool looping,
                       std::vector<const AnimationEvent*>* out) {
    if (!out) return;
    out->clear();
    if (to < from) std::swap(from, to);
    const f32 d = clip.EffectiveDuration();
    const f32 base = clip.loopEnd > clip.loopStart ? clip.loopStart : 0.0f;
    const f32 span = to - from;
    const f32 eps = 1e-6f;

    if (d <= kEpsilon || !looping) {
        // Не-зацикленный: простая проверка (from, to].
        for (const AnimationEvent& e : clip.events) {
            if (e.time > from && e.time <= to + eps) out->push_back(&e);
        }
        return;
    }
    if (span >= d - eps) {
        // Фрейм покрывает целый цикл (или больше): каждое событие срабатывает
        // ровно один раз, в порядке пересечения.
        const f32 fo2 = Wrap01(from - base, d);
        std::vector<std::pair<f32, const AnimationEvent*>> ordered;
        ordered.reserve(clip.events.size());
        for (const AnimationEvent& e : clip.events) {
            f32 x = Wrap01(e.time - base, d) - fo2;
            if (x <= 0.0f) x += d;
            ordered.push_back({x, &e});
        }
        std::stable_sort(ordered.begin(), ordered.end(),
                         [](const std::pair<f32, const AnimationEvent*>& a,
                            const std::pair<f32, const AnimationEvent*>& b) { return a.first < b.first; });
        for (const auto& entry : ordered) out->push_back(entry.second);
        return;
    }

    const f32 fo = Wrap01(from - base, d);
    const f32 ft = Wrap01(to - base, d);
    const bool wrapped = ft < fo || std::fabs(span - d) < eps;
    for (const AnimationEvent& e : clip.events) {
        const f32 x = Wrap01(e.time - base, d);
        if (!wrapped) {
            if (x > fo + eps && x <= ft + eps) out->push_back(&e);
        } else {
            // Два сегмента: (fo, d], затем [0, ft].
            if (x > fo + eps) out->push_back(&e);
            else if (x <= ft + eps) out->push_back(&e);
        }
    }
}

// ---------------------------------------------------------------------------
// Запекание клипов blend-пространства
// ---------------------------------------------------------------------------

void MergeTimes(const std::vector<f32>& a, const std::vector<f32>& b, std::vector<f32>* out) {
    out->clear();
    out->insert(out->end(), a.begin(), a.end());
    out->insert(out->end(), b.begin(), b.end());
    std::sort(out->begin(), out->end());
    out->erase(std::unique(out->begin(), out->end()), out->end());
}

// Строит один трек, значение которого в каждом объединённом времени ключа —
// смесь двух исходных треков (отсутствующие каналы смешиваются со значением по умолчанию).
void BlendVec3Track(const Vec3Track& a, const Vec3Track& b, const Vec3& da, const Vec3& db, f32 t,
                    Vec3Track* out) {
    std::vector<f32> timesA, timesB, times;
    for (const auto& k : a.keys) timesA.push_back(k.time);
    for (const auto& k : b.keys) timesB.push_back(k.time);
    MergeTimes(timesA, timesB, &times);
    out->keys.clear();
    for (f32 time : times) {
        const Vec3 va = a.keys.empty() ? da : a.Sample(time);
        const Vec3 vb = b.keys.empty() ? db : b.Sample(time);
        out->AddKey(time, Lerp(va, vb, t), Interpolation::Linear);
    }
}

void BlendQuatTrack(const QuatTrack& a, const QuatTrack& b, f32 t, QuatTrack* out) {
    std::vector<f32> timesA, timesB, times;
    for (const auto& k : a.keys) timesA.push_back(k.time);
    for (const auto& k : b.keys) timesB.push_back(k.time);
    MergeTimes(timesA, timesB, &times);
    out->keys.clear();
    for (f32 time : times) {
        const Quat va = a.keys.empty() ? Quat::Identity() : a.Sample(time);
        const Quat vb = b.keys.empty() ? Quat::Identity() : b.Sample(time);
        out->AddKey(time, Quat::Slerp(va, vb, t).Normalized(), Interpolation::Linear);
    }
}

// Запекает `lerp(a, b, t)` в новый клип (используется PlayBlendSpace1D, поскольку
// AnimLayer может ссылаться только на один индекс клипа).
[[nodiscard]] AnimationClip BlendClips(const AnimationClip& a, const AnimationClip& b, f32 t) {
    AnimationClip out;
    out.name = a.name + "+" + b.name;
    out.duration = MaxT(a.duration, b.duration);
    out.ticksPerSecond = a.ticksPerSecond;
    out.looping = a.looping;
    out.loopStart = a.loopStart;
    out.loopEnd = a.loopEnd;

    const auto findTrack = [](const AnimationClip& c, int joint) -> const AnimationClip::JointTrack* {
        for (const auto& tr : c.tracks)
            if (tr.joint == joint) return &tr;
        return nullptr;
    };

    for (const auto& ta : a.tracks) {
        const AnimationClip::JointTrack* tb = findTrack(b, ta.joint);
        AnimationClip::JointTrack outTrack;
        outTrack.joint = ta.joint;
        outTrack.hasTranslation = ta.hasTranslation || (tb && tb->hasTranslation);
        outTrack.hasRotation = ta.hasRotation || (tb && tb->hasRotation);
        outTrack.hasScale = ta.hasScale || (tb && tb->hasScale);
        if (outTrack.hasTranslation) {
            const Vec3Track empty;
            BlendVec3Track(ta.translation, tb ? tb->translation : empty, Vec3{0, 0, 0}, Vec3{0, 0, 0}, t,
                           &outTrack.translation);
        }
        if (outTrack.hasRotation) {
            const QuatTrack empty;
            BlendQuatTrack(ta.rotation, tb ? tb->rotation : empty, t, &outTrack.rotation);
        }
        if (outTrack.hasScale) {
            const Vec3Track empty;
            BlendVec3Track(ta.scale, tb ? tb->scale : empty, Vec3{1, 1, 1}, Vec3{1, 1, 1}, t,
                           &outTrack.scale);
        }
        out.tracks.push_back(std::move(outTrack));
    }
    for (const auto& tb : b.tracks) {
        if (findTrack(a, tb.joint)) continue;
        AnimationClip::JointTrack outTrack;
        outTrack.joint = tb.joint;
        outTrack.hasTranslation = tb.hasTranslation;
        outTrack.hasRotation = tb.hasRotation;
        outTrack.hasScale = tb.hasScale;
        if (outTrack.hasTranslation) {
            const Vec3Track empty;
            BlendVec3Track(empty, tb.translation, Vec3{0, 0, 0}, Vec3{0, 0, 0}, t, &outTrack.translation);
        }
        if (outTrack.hasRotation) {
            const QuatTrack empty;
            BlendQuatTrack(empty, tb.rotation, t, &outTrack.rotation);
        }
        if (outTrack.hasScale) {
            const Vec3Track empty;
            BlendVec3Track(empty, tb.scale, Vec3{1, 1, 1}, Vec3{1, 1, 1}, t, &outTrack.scale);
        }
        out.tracks.push_back(std::move(outTrack));
    }
    // События берутся из доминирующего исходного клипа.
    const AnimationClip& dominant = t <= 0.5f ? a : b;
    out.events = dominant.events;
    return out;
}

// Зеркальное имя пары левый/правый или пустая строка.
[[nodiscard]] std::string MirrorJointName(const std::string& name) {
    static const struct {
        const char* a;
        const char* b;
    } kPairs[] = {{"Left", "Right"}, {"Right", "Left"}, {"left", "right"}, {"right", "left"},
                  {"_L", "_R"},      {"_R", "_L"},      {".L", ".R"},       {".R", ".L"},
                  {" L", " R"},      {" R", " L"}};
    for (const auto& p : kPairs) {
        const std::string a = p.a;
        const std::string b = p.b;
        const usize pos = name.find(a);
        if (pos != std::string::npos) {
            std::string out = name;
            out.replace(pos, a.size(), b);
            return out;
        }
    }
    return std::string();
}

}  // namespace

// ===========================================================================
// Skeleton
// ===========================================================================
int Skeleton::AddJoint(const std::string& name, int parent, const Mat4& localBind) {
    if (parent >= static_cast<int>(joints_.size())) {
        ENG_LOGW("anim", "Skeleton::AddJoint('%s'): parent %d does not exist, using root", name.c_str(),
                 parent);
        parent = -1;
    }
    Joint j;
    j.name = name;
    j.parent = parent;
    DecomposeTRS(localBind, &j.localTranslation, &j.localRotation, &j.localScale);
    j.localBindPose = localBind;
    j.inverseBindMatrix = Mat4::Identity();
    joints_.push_back(j);
    const int index = static_cast<int>(joints_.size()) - 1;
    // Оставляем первую регистрацию дублирующегося имени (скиннинг ищет по имени).
    nameToIndex_.emplace(name, index);
    return index;
}

void Skeleton::Finalise() {
    const int n = static_cast<int>(joints_.size());
    nameToIndex_.clear();
    for (int i = 0; i < n; ++i) nameToIndex_.emplace(joints_[static_cast<usize>(i)].name, i);
    if (n == 0) return;

    std::vector<Mat4> world(static_cast<usize>(n), Mat4::Identity());
    // Топологический проход: родители выводятся раньше детей без предположений о порядке хранения.
    std::vector<u8> done(static_cast<usize>(n), 0);
    std::vector<int> order;
    order.reserve(static_cast<usize>(n));
    for (int pass = 0; pass < n; ++pass) {
        bool progress = false;
        for (int i = 0; i < n; ++i) {
            if (done[static_cast<usize>(i)]) continue;
            const int p = joints_[static_cast<usize>(i)].parent;
            if (p < 0 || p >= n || done[static_cast<usize>(p)]) {
                order.push_back(i);
                done[static_cast<usize>(i)] = 1;
                progress = true;
            }
        }
        if (!progress) break;  // цикл среди родителей: остаток считаем корнями
    }
    for (int i = 0; i < n; ++i)
        if (!done[static_cast<usize>(i)]) order.push_back(i);

    for (int i : order) {
        Joint& j = joints_[static_cast<usize>(i)];
        j.localBindPose = ComposeTRS(j.localTranslation, j.localRotation, j.localScale);
        const int p = j.parent;
        world[static_cast<usize>(i)] =
            (p >= 0 && p < n) ? world[static_cast<usize>(p)] * j.localBindPose : j.localBindPose;
        j.inverseBindMatrix = world[static_cast<usize>(i)].Inverse();
    }
}

int Skeleton::FindJoint(const std::string& name) const {
    const auto it = nameToIndex_.find(name);
    if (it != nameToIndex_.end() && it->second >= 0 && it->second < static_cast<int>(joints_.size()) &&
        joints_[static_cast<usize>(it->second)].name == name) {
        return it->second;
    }
    for (usize i = 0; i < joints_.size(); ++i)
        if (joints_[i].name == name) return static_cast<int>(i);
    return -1;
}

int Skeleton::MaxDepth() const {
    const int n = static_cast<int>(joints_.size());
    int best = 0;
    for (int i = 0; i < n; ++i) {
        int depth = 1;
        int p = joints_[static_cast<usize>(i)].parent;
        int guard = 0;
        while (p >= 0 && p < n && guard++ <= n) {
            ++depth;
            p = joints_[static_cast<usize>(p)].parent;
        }
        if (depth > best) best = depth;
    }
    return best;
}

Skeleton Skeleton::MakeHumanoid() {
    Skeleton s;
    const int hips = s.AddJoint("Hips", -1, Mat4::Translate(Vec3{0, 1.0f, 0}));
    const int spine = s.AddJoint("Spine", hips, Mat4::Translate(Vec3{0, 0.2f, 0}));
    const int chest = s.AddJoint("Chest", spine, Mat4::Translate(Vec3{0, 0.2f, 0}));
    const int neck = s.AddJoint("Neck", chest, Mat4::Translate(Vec3{0, 0.15f, 0}));
    s.AddJoint("Head", neck, Mat4::Translate(Vec3{0, 0.12f, 0}));
    const int lShoulder = s.AddJoint("LeftShoulder", chest, Mat4::Translate(Vec3{0.1f, 0.1f, 0}));
    const int lUpperArm = s.AddJoint("LeftUpperArm", lShoulder, Mat4::Translate(Vec3{0.15f, 0, 0}));
    const int lLowerArm = s.AddJoint("LeftLowerArm", lUpperArm, Mat4::Translate(Vec3{0.25f, 0, 0}));
    s.AddJoint("LeftHand", lLowerArm, Mat4::Translate(Vec3{0.22f, 0, 0}));
    const int rShoulder = s.AddJoint("RightShoulder", chest, Mat4::Translate(Vec3{-0.1f, 0.1f, 0}));
    const int rUpperArm = s.AddJoint("RightUpperArm", rShoulder, Mat4::Translate(Vec3{-0.15f, 0, 0}));
    const int rLowerArm = s.AddJoint("RightLowerArm", rUpperArm, Mat4::Translate(Vec3{-0.25f, 0, 0}));
    s.AddJoint("RightHand", rLowerArm, Mat4::Translate(Vec3{-0.22f, 0, 0}));
    const int lUpperLeg = s.AddJoint("LeftUpperLeg", hips, Mat4::Translate(Vec3{0.09f, -0.05f, 0}));
    const int lLowerLeg = s.AddJoint("LeftLowerLeg", lUpperLeg, Mat4::Translate(Vec3{0, -0.4f, 0}));
    s.AddJoint("LeftFoot", lLowerLeg, Mat4::Translate(Vec3{0, -0.4f, 0}));
    const int rUpperLeg = s.AddJoint("RightUpperLeg", hips, Mat4::Translate(Vec3{-0.09f, -0.05f, 0}));
    const int rLowerLeg = s.AddJoint("RightLowerLeg", rUpperLeg, Mat4::Translate(Vec3{0, -0.4f, 0}));
    s.AddJoint("RightFoot", rLowerLeg, Mat4::Translate(Vec3{0, -0.4f, 0}));
    s.Finalise();
    return s;
}

Skeleton Skeleton::MakeChain(int count, f32 length) {
    if (count < 1) count = 1;
    if (length == 0.0f) length = 1.0f;
    Skeleton s;
    for (int i = 0; i < count; ++i) {
        const Mat4 local = (i == 0) ? Mat4::Identity() : Mat4::Translate(Vec3{length, 0, 0});
        s.AddJoint("Chain" + std::to_string(i), i - 1, local);
    }
    s.Finalise();
    return s;
}

// ===========================================================================
// Специализации Track<T>::Sample (AddKey/Sort находятся в начале файла)
// ===========================================================================
template <>
Vec3 Track<Vec3>::Sample(f32 time) const {
    if (keys.empty()) return Vec3{0, 0, 0};
    if (keys.size() == 1) return keys[0].value;
    usize ia = 0, ib = 1;
    f32 t = 0.0f;
    if (!FindSegment(keys, time, &ia, &ib, &t)) return keys[0].value;
    const f32 span = keys[ib].time - keys[ia].time;
    switch (keys[ia].interp) {
        case Interpolation::Step:
            // Держит значение до следующего ключа, затем переключается на него.
            return t >= 1.0f ? keys[ib].value : keys[ia].value;
        case Interpolation::CubicSpline:
            return HermiteSegment<Vec3>(keys[ia], keys[ib], t, span,
                                        [](const Vec3& a, f32 wa, const Vec3& ta, f32 wta, const Vec3& b,
                                           f32 wb, const Vec3& tb, f32 wtb) {
                                            return a * wa + ta * wta + b * wb + tb * wtb;
                                        });
        case Interpolation::Bezier:
            // У ключей нет безье-контрольных точек; используем сглаженный lerp.
            return Lerp(keys[ia].value, keys[ib].value, t * t * (3.0f - 2.0f * t));
        case Interpolation::Linear:
        default:
            return Lerp(keys[ia].value, keys[ib].value, t);
    }
}

template <>
Quat Track<Quat>::Sample(f32 time) const {
    if (keys.empty()) return Quat::Identity();
    if (keys.size() == 1) return keys[0].value.Normalized();
    usize ia = 0, ib = 1;
    f32 t = 0.0f;
    if (!FindSegment(keys, time, &ia, &ib, &t)) return keys[0].value.Normalized();
    const Keyframe<Quat>& a = keys[ia];
    const Keyframe<Quat>& b = keys[ib];
    switch (a.interp) {
        case Interpolation::Step:
            return (t >= 1.0f ? b.value : a.value).Normalized();
        case Interpolation::CubicSpline: {
            // Эрмит по компонентам по короткой дуге; откат к Slerp, когда у ключей
            // нет авторских касательных (частый случай).
            if (QuatIsIdentity(a.outTangent) && QuatIsIdentity(b.inTangent))
                return Quat::Slerp(a.value, b.value, t).Normalized();
            Quat qa = a.value;
            Quat qb = b.value;
            Quat ta = a.outTangent;
            Quat tb = b.inTangent;
            const f32 dot = qa.x * qb.x + qa.y * qb.y + qa.z * qb.z + qa.w * qb.w;
            if (dot < 0.0f) {
                qb = Quat{-qb.x, -qb.y, -qb.z, -qb.w};
                tb = Quat{-tb.x, -tb.y, -tb.z, -tb.w};
            }
            const f32 span = b.time - a.time;
            const f32 t2 = t * t;
            const f32 t3 = t2 * t;
            const f32 h00 = 2.0f * t3 - 3.0f * t2 + 1.0f;
            const f32 h10 = t3 - 2.0f * t2 + t;
            const f32 h01 = -2.0f * t3 + 3.0f * t2;
            const f32 h11 = t3 - t2;
            return Quat{qa.x * h00 + ta.x * (h10 * span) + qb.x * h01 + tb.x * (h11 * span),
                        qa.y * h00 + ta.y * (h10 * span) + qb.y * h01 + tb.y * (h11 * span),
                        qa.z * h00 + ta.z * (h10 * span) + qb.z * h01 + tb.z * (h11 * span),
                        qa.w * h00 + ta.w * (h10 * span) + qb.w * h01 + tb.w * (h11 * span)}
                .Normalized();
        }
        case Interpolation::Bezier:
            return Quat::Slerp(a.value, b.value, t * t * (3.0f - 2.0f * t)).Normalized();
        case Interpolation::Linear:
        default:
            return Quat::Slerp(a.value, b.value, t).Normalized();
    }
}

template <>
f32 Track<f32>::Sample(f32 time) const {
    if (keys.empty()) return 0.0f;
    if (keys.size() == 1) return keys[0].value;
    usize ia = 0, ib = 1;
    f32 t = 0.0f;
    if (!FindSegment(keys, time, &ia, &ib, &t)) return keys[0].value;
    const f32 span = keys[ib].time - keys[ia].time;
    switch (keys[ia].interp) {
        case Interpolation::Step:
            return t >= 1.0f ? keys[ib].value : keys[ia].value;
        case Interpolation::CubicSpline:
            return HermiteSegment<f32>(keys[ia], keys[ib], t, span,
                                       [](f32 a, f32 wa, f32 ta, f32 wta, f32 b, f32 wb, f32 tb, f32 wtb) {
                                           return a * wa + ta * wta + b * wb + tb * wtb;
                                       });
        case Interpolation::Bezier:
            return Lerp(keys[ia].value, keys[ib].value, t * t * (3.0f - 2.0f * t));
        case Interpolation::Linear:
        default:
            return Lerp(keys[ia].value, keys[ib].value, t);
    }
}

template struct Track<Vec3>;
template struct Track<Quat>;
template struct Track<f32>;

// ===========================================================================
// AnimationClip
// ===========================================================================
int AnimationClip::FindTrack(int joint) const {
    for (usize i = 0; i < tracks.size(); ++i)
        if (tracks[i].joint == joint) return static_cast<int>(i);
    return -1;
}

void AnimationClip::Sample(f32 time, std::vector<Mat4>* locals, std::vector<Vec3>* translations,
                           std::vector<Quat>* rotations, std::vector<Vec3>* scales) const {
    int jointCount = 0;
    for (const JointTrack& t : tracks)
        if (t.joint >= jointCount) jointCount = t.joint + 1;
    if (translations) jointCount = MaxT(jointCount, static_cast<int>(translations->size()));
    if (rotations) jointCount = MaxT(jointCount, static_cast<int>(rotations->size()));
    if (scales) jointCount = MaxT(jointCount, static_cast<int>(scales->size()));
    if (locals) jointCount = MaxT(jointCount, static_cast<int>(locals->size()));
    const usize n = static_cast<usize>(MaxT(jointCount, 0));

    // Только resize (существующие значения сохраняются: не отслеживаемые суставы
    // сохраняют то, что подготовил вызывающий, — так Animator сэмплирует поверх бинд-позы).
    if (translations) translations->resize(n, Vec3{0, 0, 0});
    if (rotations) rotations->resize(n, Quat{0, 0, 0, 1});
    if (scales) scales->resize(n, Vec3{1, 1, 1});
    if (locals) locals->resize(n, Mat4::Identity());

    const f32 dur = EffectiveDuration();
    const f32 base = loopEnd > loopStart ? loopStart : 0.0f;
    f32 t = time;
    if (dur > kEpsilon) {
        if (looping) {
            // Времена вне [base, base+dur] замыкаются по кругу; точный стык цикла
            // продолжает сэмплировать последний ключ, чтобы не-зацикленный режим мог его удержать.
            if (t < base || t > base + dur) t = base + Wrap01(t - base, dur);
        } else {
            t = Clamp(t, base, base + dur);
        }
    } else {
        t = base;
    }

    for (const JointTrack& track : tracks) {
        if (track.joint < 0 || track.joint >= static_cast<int>(n)) continue;
        const usize j = static_cast<usize>(track.joint);
        Vec3 tv = translations ? (*translations)[j] : Vec3{0, 0, 0};
        Quat rv = rotations ? (*rotations)[j] : Quat{0, 0, 0, 1};
        Vec3 sv = scales ? (*scales)[j] : Vec3{1, 1, 1};
        // Пустой канал не трогает значение вызывающего (Animator засеивает scratch-позу
        // бинд-позой, поэтому неотслеживаемые каналы остаются в бинде).
        if (track.hasTranslation && !track.translation.keys.empty()) tv = track.translation.Sample(t);
        if (track.hasRotation && !track.rotation.keys.empty()) rv = track.rotation.Sample(t).Normalized();
        if (track.hasScale && !track.scale.keys.empty()) sv = track.scale.Sample(t);
        if (translations) (*translations)[j] = tv;
        if (rotations) (*rotations)[j] = rv;
        if (scales) (*scales)[j] = sv;
        if (locals) (*locals)[j] = ComposeTRS(tv, rv, sv);
    }
}

void AnimationClip::CollectEvents(f32 from, f32 to, std::vector<const AnimationEvent*>* out) const {
    CollectEventsImpl(*this, from, to, looping, out);
}

AnimationClip AnimationClip::MakeWave(f32 duration) {
    if (duration <= 0.0f) duration = 2.0f;
    AnimationClip c;
    c.name = "Wave";
    c.duration = duration;
    c.ticksPerSecond = 30.0f;
    c.looping = true;
    JointTrack t;
    t.joint = 0;
    t.hasTranslation = true;
    const int steps = 16;
    for (int i = 0; i <= steps; ++i) {
        const f32 u = static_cast<f32>(i) / static_cast<f32>(steps);
        t.translation.AddKey(u * duration, Vec3{0, std::sin(u * kTau) * 0.5f, 0}, Interpolation::Linear);
    }
    c.tracks.push_back(std::move(t));
    return c;
}

AnimationClip AnimationClip::MakeBounce(f32 duration) {
    if (duration <= 0.0f) duration = 1.5f;
    AnimationClip c;
    c.name = "Bounce";
    c.duration = duration;
    c.ticksPerSecond = 30.0f;
    c.looping = true;
    JointTrack t;
    t.joint = 0;
    t.hasTranslation = true;
    t.hasScale = true;
    static const f32 kTimes[] = {0.0f, 0.35f, 0.5f, 0.65f, 0.8f, 1.0f};
    static const f32 kHeights[] = {0.0f, 1.0f, 0.0f, 0.4f, 0.0f, 0.0f};
    static const f32 kSquash[] = {0.9f, 1.1f, 0.8f, 1.05f, 1.0f, 1.0f};
    for (int i = 0; i < 6; ++i) {
        t.translation.AddKey(kTimes[i] * duration, Vec3{0, kHeights[i], 0}, Interpolation::Linear);
        t.scale.AddKey(kTimes[i] * duration, Vec3{kSquash[i], kSquash[i], kSquash[i]}, Interpolation::Linear);
    }
    c.tracks.push_back(std::move(t));
    return c;
}

AnimationClip AnimationClip::MakeWalk(int jointCount, f32 duration) {
    if (duration <= 0.0f) duration = 1.0f;
    if (jointCount < 1) jointCount = 1;
    AnimationClip c;
    c.name = "Walk";
    c.duration = duration;
    c.ticksPerSecond = 30.0f;
    c.looping = true;
    const int steps = 8;
    for (int j = 0; j < jointCount; ++j) {
        JointTrack t;
        t.joint = j;
        t.hasRotation = true;
        t.hasTranslation = (j == 0);
        const f32 phase = static_cast<f32>(j) * 0.7f;
        for (int s = 0; s <= steps; ++s) {
            const f32 u = static_cast<f32>(s) / static_cast<f32>(steps);
            const f32 a = u * kTau + phase;
            const Quat q = Quat::FromAxisAngle(Vec3{0, 0, 1}, std::sin(a) * 0.35f) *
                           Quat::FromAxisAngle(Vec3{1, 0, 0}, std::cos(a) * 0.25f);
            t.rotation.AddKey(u * duration, q, Interpolation::Linear);
        }
        if (t.hasTranslation) {
            for (int s = 0; s <= steps; ++s) {
                const f32 u = static_cast<f32>(s) / static_cast<f32>(steps);
                t.translation.AddKey(u * duration, Vec3{0, std::fabs(std::sin(u * kTau)) * 0.05f, 0},
                                     Interpolation::Linear);
            }
        }
        c.tracks.push_back(std::move(t));
    }
    return c;
}

AnimationClip AnimationClip::MakeSpin(f32 duration) {
    if (duration <= 0.0f) duration = 1.0f;
    AnimationClip c;
    c.name = "Spin";
    c.duration = duration;
    c.ticksPerSecond = 30.0f;
    c.looping = true;
    JointTrack t;
    t.joint = 0;
    t.hasRotation = true;
    // Ключи четверть-оборота держат каждый Slerp на короткой дуге (полный обход 2*pi).
    for (int s = 0; s <= 4; ++s) {
        const f32 u = static_cast<f32>(s) / 4.0f;
        t.rotation.AddKey(u * duration, Quat::FromAxisAngle(Vec3{0, 1, 0}, u * kTau), Interpolation::Linear);
    }
    c.tracks.push_back(std::move(t));
    return c;
}

// ===========================================================================
// Pose
// ===========================================================================
void Pose::Resize(int jointCount) {
    const usize n = static_cast<usize>(MaxT(jointCount, 0));
    translations.resize(n, Vec3{0, 0, 0});
    rotations.resize(n, Quat{0, 0, 0, 1});
    scales.resize(n, Vec3{1, 1, 1});
    locals.resize(n, Mat4::Identity());
    world.resize(n, Mat4::Identity());
}

void Pose::Reset() {
    for (Vec3& t : translations) t = Vec3{0, 0, 0};
    for (Quat& r : rotations) r = Quat{0, 0, 0, 1};
    for (Vec3& s : scales) s = Vec3{1, 1, 1};
    for (Mat4& m : locals) m = Mat4::Identity();
    for (Mat4& m : world) m = Mat4::Identity();
}

void Pose::ComputeWorld(const Skeleton& skeleton) {
    const usize n = std::min(locals.size(), static_cast<usize>(MaxT(skeleton.JointCount(), 0)));
    if (world.size() < locals.size()) world.resize(locals.size(), Mat4::Identity());

    // Проход совместимости: поза, созданная только через TRS-поля (её локальные
    // матрицы всё ещё единичные), сначала собирается в матрицы.
    for (usize i = 0; i < n; ++i) {
        if (MatrixIsIdentity(locals[i]) && !TrsIsDefault(translations[i], rotations[i], scales[i])) {
            locals[i] = ComposeTRS(translations[i], rotations[i], scales[i]);
        }
    }

    // Топологический обход: родители всегда оказываются раньше своих детей.
    std::vector<int> order;
    order.reserve(n);
    std::vector<u8> done(n, 0);
    const int ni = static_cast<int>(n);
    for (int pass = 0; pass < ni; ++pass) {
        bool progress = false;
        for (int i = 0; i < ni; ++i) {
            if (done[static_cast<usize>(i)]) continue;
            const int p = skeleton.JointAt(i).parent;
            if (p < 0 || p >= ni || done[static_cast<usize>(p)]) {
                order.push_back(i);
                done[static_cast<usize>(i)] = 1;
                progress = true;
            }
        }
        if (!progress) break;
    }
    for (int i = 0; i < ni; ++i)
        if (!done[static_cast<usize>(i)]) order.push_back(i);

    for (int i : order) {
        const usize ui = static_cast<usize>(i);
        const int p = skeleton.JointAt(i).parent;
        world[ui] = (p >= 0 && p < ni) ? world[static_cast<usize>(p)] * locals[ui] : locals[ui];
    }
    for (usize i = n; i < world.size(); ++i) world[i] = Mat4::Identity();
}

void BlendWeights::Resize(int jointCount, f32 value) {
    weights.assign(static_cast<usize>(MaxT(jointCount, 0)), value);
}

// ===========================================================================
// Смешивание поз
// ===========================================================================
void BlendPoses(Pose* out, const Pose& a, const Pose& b, f32 t, AnimBlendMode mode) {
    if (!out) return;
    t = Clamp(t, 0.0f, 1.0f);
    if (mode == AnimBlendMode::Additive || mode == AnimBlendMode::Multiply) {
        AdditivePose(out, a, b, t);
        return;
    }
    const int n = a.JointCount();
    out->Resize(n);
    const int m = b.JointCount();
    for (int i = 0; i < n; ++i) {
        const usize ui = static_cast<usize>(i);
        const bool inB = i < m;
        const Vec3 bt = inB ? b.translations[ui] : Vec3{0, 0, 0};
        const Quat br = inB ? b.rotations[ui] : Quat{0, 0, 0, 1};
        const Vec3 bs = inB ? b.scales[ui] : Vec3{1, 1, 1};
        out->translations[ui] = Lerp(a.translations[ui], bt, t);
        out->rotations[ui] = Quat::Slerp(a.rotations[ui], br, t).Normalized();
        out->scales[ui] = Lerp(a.scales[ui], bs, t);
        ComposeLocalAt(out, ui);
    }
}

void BlendPosesMasked(Pose* out, const Pose& a, const Pose& b, const BlendWeights& weights) {
    if (!out) return;
    const int n = a.JointCount();
    out->Resize(n);
    const int m = b.JointCount();
    for (int i = 0; i < n; ++i) {
        const usize ui = static_cast<usize>(i);
        // Отсутствующая/пустая маска означает «воздействовать на каждый сустав».
        const f32 w = ui < weights.weights.size() ? Clamp(weights.weights[ui], 0.0f, 1.0f) : 1.0f;
        const bool inB = i < m;
        const Vec3 bt = inB ? b.translations[ui] : Vec3{0, 0, 0};
        const Quat br = inB ? b.rotations[ui] : Quat{0, 0, 0, 1};
        const Vec3 bs = inB ? b.scales[ui] : Vec3{1, 1, 1};
        out->translations[ui] = Lerp(a.translations[ui], bt, w);
        out->rotations[ui] = Quat::Slerp(a.rotations[ui], br, w).Normalized();
        out->scales[ui] = Lerp(a.scales[ui], bs, w);
        ComposeLocalAt(out, ui);
    }
}

void AdditivePose(Pose* out, const Pose& base, const Pose& additive, f32 weight) {
    if (!out) return;
    // Аддитивная поза трактуется без скелета/бинд-позы, поэтому `additive`
    // считается уже приведённой к бинду локальной дельтой (её бинд — единичная
    // локальная трансформация). Результат — эталон, скомпонованный с этой дельтой:
    //     out.local = base.local * delta(additive.local, weight)
    // что даёт no-op для нулевой дельты (единичный TRS) и осмысленное поведение
    // для вращений (умножение кватернионов) и трансляций (смещение * weight).
    weight = Clamp(weight, 0.0f, 1.0f);
    const int n = base.JointCount();
    out->Resize(n);
    const int m = additive.JointCount();
    for (int i = 0; i < n; ++i) {
        const usize ui = static_cast<usize>(i);
        const bool inB = i < m;
        const Vec3 dt = inB ? additive.translations[ui] : Vec3{0, 0, 0};
        const Quat dr = inB ? additive.rotations[ui] : Quat{0, 0, 0, 1};
        const Vec3 ds = inB ? additive.scales[ui] : Vec3{1, 1, 1};
        out->translations[ui] = base.translations[ui] + dt * weight;
        out->rotations[ui] =
            (base.rotations[ui] * Quat::Slerp(Quat::Identity(), dr, weight)).Normalized();
        out->scales[ui] = base.scales[ui] * Lerp(Vec3{1, 1, 1}, ds, weight);
        ComposeLocalAt(out, ui);
    }
}

void NormalizePose(Pose* pose) {
    if (!pose) return;
    const usize n = pose->rotations.size();
    for (usize i = 0; i < n; ++i) {
        pose->rotations[i] = pose->rotations[i].Normalized();
        ComposeLocalAt(pose, i);
    }
}

void MirrorPose(Pose* pose, const Skeleton& skeleton) {
    if (!pose) return;
    const int n = std::min(pose->JointCount(), skeleton.JointCount());
    if (n <= 0) return;
    const std::vector<Vec3> srcT = pose->translations;
    const std::vector<Quat> srcR = pose->rotations;
    const std::vector<Vec3> srcS = pose->scales;
    for (int i = 0; i < n; ++i) {
        const usize ui = static_cast<usize>(i);
        // Меняем левую/правую пару местами, если скелет их так называет, затем
        // отражаем локальную трансформацию относительно плоскости YZ (x -> -x, q -> (x,-y,-z,w)).
        int src = i;
        const std::string mirrorName = MirrorJointName(skeleton.JointAt(i).name);
        if (!mirrorName.empty()) {
            const int m = skeleton.FindJoint(mirrorName);
            if (m >= 0 && m < n && m != i) src = m;
        }
        const usize us = static_cast<usize>(src);
        const Vec3 t = us < srcT.size() ? srcT[us] : Vec3{0, 0, 0};
        const Quat q = us < srcR.size() ? srcR[us] : Quat{0, 0, 0, 1};
        const Vec3 s = us < srcS.size() ? srcS[us] : Vec3{1, 1, 1};
        pose->translations[ui] = Vec3{-t.x, t.y, t.z};
        pose->rotations[ui] = Quat{q.x, -q.y, -q.z, q.w}.Normalized();
        pose->scales[ui] = s;
        ComposeLocalAt(pose, ui);
    }
}

// ===========================================================================
// Animator
// ===========================================================================
void Animator::SetSkeleton(Skeleton* skeleton) {
    skeleton_ = skeleton;
    Reset();
    EnsureLayerCount(MaxT(static_cast<int>(layers_.size()), 1));
    RebuildLayerMasks();
}

int Animator::AddClip(AnimationClip clip) {
    clips_.push_back(std::move(clip));
    return static_cast<int>(clips_.size()) - 1;
}

int Animator::FindClip(const std::string& name) const {
    for (usize i = 0; i < clips_.size(); ++i)
        if (clips_[i].name == name) return static_cast<int>(i);
    return -1;
}

void Animator::EnsureLayerCount(int count) {
    if (count < 0) count = 0;
    while (static_cast<int>(layers_.size()) < count) {
        AnimLayer layer;
        const int index = static_cast<int>(layers_.size());
        layer.name = index == 0 ? "Base" : ("Layer" + std::to_string(index));
        layers_.push_back(std::move(layer));
    }
}

void Animator::Play(int clipIndex, f32 fadeSeconds, int layer, bool restart, AnimPlayMode mode) {
    if (clipIndex < 0 || clipIndex >= static_cast<int>(clips_.size())) {
        ENG_LOGW("anim", "Animator::Play: invalid clip index %d", clipIndex);
        return;
    }
    EnsureLayerCount(layer + 1);
    if (layer < 0) return;
    AnimLayer& l = layers_[static_cast<usize>(layer)];
    const bool hadClip = l.clipIndex >= 0;
    const bool canCrossfade = hadClip && fadeSeconds > kEpsilon && skeleton_ != nullptr &&
                              pose_.JointCount() == skeleton_->JointCount() && pose_.JointCount() > 0;
    if (canCrossfade) {
        // Поза, которая сейчас на экране, становится источником crossfade. Буфер
        // снимков всего один, поэтому любой другой незавершённый фейд завершается сейчас.
        scratchB_ = pose_;
        for (usize i = 0; i < layers_.size(); ++i) {
            AnimLayer& other = layers_[i];
            if (&other != &l && other.fadeIn > kEpsilon && other.fadeTimer < other.fadeIn)
                other.fadeTimer = other.fadeIn;
        }
        l.fadeIn = fadeSeconds;
        l.fadeTimer = 0.0f;
    } else {
        l.fadeIn = 0.0f;
        l.fadeTimer = 0.0f;
    }
    if (restart || !hadClip) {
        const AnimationClip& clip = clips_[static_cast<usize>(clipIndex)];
        l.time = clip.loopEnd > clip.loopStart ? clip.loopStart : 0.0f;
    }
    l.clipIndex = clipIndex;
    l.mode = mode;
    l.blendMode = AnimBlendMode::Override;  // обычный Play сбрасывает любую маску костей
    l.mask.weights.clear();
    l.boneMaskNames.clear();
    l.enabled = true;
    l.fadingOut = false;
    l.finished = false;
    l.fadeOut = 0.0f;
}

void Animator::Stop(int layer, f32 fadeSeconds) {
    if (layer < 0 || layer >= static_cast<int>(layers_.size())) return;
    AnimLayer& l = layers_[static_cast<usize>(layer)];
    if (fadeSeconds <= kEpsilon) {
        l.enabled = false;
        l.clipIndex = -1;
        l.fadingOut = false;
        l.fadeTimer = 0.0f;
        l.finished = false;
        return;
    }
    l.fadingOut = true;
    l.fadeOut = fadeSeconds;
    l.fadeTimer = 0.0f;
    l.fadeIn = 0.0f;
}

void Animator::SetLayerWeight(int layer, f32 weight) {
    EnsureLayerCount(layer + 1);
    if (layer < 0) return;
    layers_[static_cast<usize>(layer)].weight = MaxT(weight, 0.0f);
}

f32 Animator::LayerWeight(int layer) const {
    if (layer < 0 || layer >= static_cast<int>(layers_.size())) return 0.0f;
    return layers_[static_cast<usize>(layer)].weight;
}

void Animator::SetLayerSpeed(int layer, f32 speed) {
    EnsureLayerCount(layer + 1);
    if (layer < 0) return;
    layers_[static_cast<usize>(layer)].speed = speed;
}

void Animator::PlayAdditive(int clipIndex, f32 weight, int layer) {
    if (clipIndex < 0 || clipIndex >= static_cast<int>(clips_.size())) {
        ENG_LOGW("anim", "Animator::PlayAdditive: invalid clip index %d", clipIndex);
        return;
    }
    EnsureLayerCount(layer + 1);
    if (layer < 0) return;
    AnimLayer& l = layers_[static_cast<usize>(layer)];
    l.clipIndex = clipIndex;
    l.time = 0.0f;
    l.mode = AnimPlayMode::Loop;
    l.blendMode = AnimBlendMode::Additive;
    l.weight = MaxT(weight, 0.0f);
    l.enabled = true;
    l.fadeIn = 0.0f;
    l.fadeOut = 0.0f;
    l.fadeTimer = 0.0f;
    l.fadingOut = false;
    l.finished = false;
    l.mask.weights.clear();
    l.boneMaskNames.clear();
}

void Animator::PlayMasked(int clipIndex, const std::vector<std::string>& bones, f32 weight, int layer) {
    if (clipIndex < 0 || clipIndex >= static_cast<int>(clips_.size())) {
        ENG_LOGW("anim", "Animator::PlayMasked: invalid clip index %d", clipIndex);
        return;
    }
    EnsureLayerCount(layer + 1);
    if (layer < 0) return;
    AnimLayer& l = layers_[static_cast<usize>(layer)];
    l.clipIndex = clipIndex;
    l.time = 0.0f;
    l.mode = AnimPlayMode::Loop;
    l.blendMode = AnimBlendMode::Masked;
    l.weight = MaxT(weight, 0.0f);
    l.boneMaskNames = bones;
    l.enabled = true;
    l.fadeIn = 0.0f;
    l.fadeOut = 0.0f;
    l.fadeTimer = 0.0f;
    l.fadingOut = false;
    l.finished = false;
    RebuildLayerMasks();
}

void Animator::PlayBlendSpace1D(const std::vector<int>& clipIndices, const std::vector<f32>& thresholds,
                                f32 parameter, f32 fadeSeconds, int layer) {
    if (clipIndices.empty()) {
        ENG_LOGW("anim", "Animator::PlayBlendSpace1D: no clips");
        return;
    }
    EnsureLayerCount(layer + 1);
    const auto valid = [&](int index) { return index >= 0 && index < static_cast<int>(clips_.size()); };
    if (clipIndices.size() == 1 || thresholds.size() != clipIndices.size()) {
        if (valid(clipIndices[0])) Play(clipIndices[0], fadeSeconds, layer, true, AnimPlayMode::Loop);
        return;
    }
    if (parameter <= thresholds.front()) {
        if (valid(clipIndices.front()))
            Play(clipIndices.front(), fadeSeconds, layer, true, AnimPlayMode::Loop);
        return;
    }
    if (parameter >= thresholds.back()) {
        if (valid(clipIndices.back()))
            Play(clipIndices.back(), fadeSeconds, layer, true, AnimPlayMode::Loop);
        return;
    }
    usize i = 0;
    while (i + 1 < thresholds.size() && thresholds[i + 1] < parameter) ++i;
    if (i + 1 >= clipIndices.size()) i = clipIndices.size() - 2;
    if (!valid(clipIndices[i]) || !valid(clipIndices[i + 1])) {
        ENG_LOGW("anim", "Animator::PlayBlendSpace1D: invalid clip in blend space");
        return;
    }
    const f32 t0 = thresholds[i];
    const f32 t1 = thresholds[i + 1];
    const f32 blend = t1 > t0 ? Clamp((parameter - t0) / (t1 - t0), 0.0f, 1.0f) : 0.0f;
    // AnimLayer ссылается на один клип, поэтому точка blend-пространства
    // запекается в новый клип один раз и затем проигрывается как любой другой клип.
    AnimationClip baked = BlendClips(clips_[static_cast<usize>(clipIndices[i])],
                                     clips_[static_cast<usize>(clipIndices[i + 1])], blend);
    const int index = AddClip(std::move(baked));
    Play(index, fadeSeconds, layer, true, AnimPlayMode::Loop);
}

void Animator::SetLayerTime(int layer, f32 time) {
    EnsureLayerCount(layer + 1);
    if (layer < 0) return;
    AnimLayer& l = layers_[static_cast<usize>(layer)];
    if (l.clipIndex < 0 || l.clipIndex >= static_cast<int>(clips_.size())) {
        l.time = time;
        return;
    }
    const AnimationClip& clip = clips_[static_cast<usize>(l.clipIndex)];
    const f32 d = clip.EffectiveDuration();
    const f32 base = clip.loopEnd > clip.loopStart ? clip.loopStart : 0.0f;
    if (d <= kEpsilon) {
        l.time = base;
        return;
    }
    switch (l.mode) {
        case AnimPlayMode::Once:
        case AnimPlayMode::ClampForever:
            l.time = Clamp(time, base, base + d);
            break;
        case AnimPlayMode::PingPong:
            l.time = base + Wrap01(time - base, d * 2.0f);
            break;
        case AnimPlayMode::Loop:
        default:
            l.time = base + Wrap01(time - base, d);
            break;
    }
}

f32 Animator::LayerTime(int layer) const {
    if (layer < 0 || layer >= static_cast<int>(layers_.size())) return 0.0f;
    const AnimLayer& l = layers_[static_cast<usize>(layer)];
    if (l.clipIndex < 0 || l.clipIndex >= static_cast<int>(clips_.size())) return l.time;
    const AnimationClip& clip = clips_[static_cast<usize>(l.clipIndex)];
    const f32 d = clip.EffectiveDuration();
    const f32 base = clip.loopEnd > clip.loopStart ? clip.loopStart : 0.0f;
    if (l.mode == AnimPlayMode::PingPong && d > kEpsilon) return base + PingPongTime(l.time - base, d);
    return l.time;
}

f32 Animator::LayerNormalizedTime(int layer) const {
    if (layer < 0 || layer >= static_cast<int>(layers_.size())) return 0.0f;
    const AnimLayer& l = layers_[static_cast<usize>(layer)];
    if (l.clipIndex < 0 || l.clipIndex >= static_cast<int>(clips_.size())) return 0.0f;
    const AnimationClip& clip = clips_[static_cast<usize>(l.clipIndex)];
    const f32 d = clip.EffectiveDuration();
    if (d <= kEpsilon) return 0.0f;
    const f32 base = clip.loopEnd > clip.loopStart ? clip.loopStart : 0.0f;
    return Clamp((LayerTime(layer) - base) / d, 0.0f, 1.0f);
}

bool Animator::LayerFinished(int layer) const {
    if (layer < 0 || layer >= static_cast<int>(layers_.size())) return false;
    return layers_[static_cast<usize>(layer)].finished;
}

void Animator::Update(f32 dt) {
    fired_.clear();
    if (dt < 0.0f) dt = 0.0f;
    if (paused_) dt = 0.0f;
    lastDt_ = dt;
    if (!skeleton_) return;
    const int jointCount = skeleton_->JointCount();
    if (jointCount <= 0) return;

    pose_.Resize(jointCount);
    scratchA_.Resize(jointCount);
    scratchB_.Resize(jointCount);

    // Накопитель каждый фрейм стартует с бинд-позы скелета: слой с весом 0
    // (или полное отсутствие слоёв) поэтому не даёт никакого вклада.
    for (int i = 0; i < jointCount; ++i) {
        const Joint& j = skeleton_->JointAt(i);
        const usize ui = static_cast<usize>(i);
        pose_.translations[ui] = j.localTranslation;
        pose_.rotations[ui] = j.localRotation;
        pose_.scales[ui] = j.localScale;
        pose_.locals[ui] = ComposeTRS(j.localTranslation, j.localRotation, j.localScale);
    }

    const auto fireEvent = [&](const AnimationEvent* e) {
        fired_.push_back(e);
        if (eventCb_) eventCb_(*e);
    };

    std::vector<const AnimationEvent*> eventBuf;
    bool contributed = false;

    for (usize li = 0; li < layers_.size(); ++li) {
        AnimLayer& layer = layers_[li];
        if (!layer.enabled) continue;
        if (layer.clipIndex < 0 || layer.clipIndex >= static_cast<int>(clips_.size())) continue;
        const AnimationClip& clip = clips_[static_cast<usize>(layer.clipIndex)];

        // ---- fade in / fade out ------------------------------------------
        if (layer.fadingOut) {
            layer.fadeTimer += dt;
        } else if (layer.fadeIn > kEpsilon && layer.fadeTimer < layer.fadeIn) {
            layer.fadeTimer = MinT(layer.fadeTimer + dt, layer.fadeIn);
        }
        if (layer.fadingOut && layer.fadeOut > kEpsilon && layer.fadeTimer >= layer.fadeOut) {
            layer.enabled = false;
            layer.fadingOut = false;
            layer.clipIndex = -1;
            layer.fadeTimer = 0.0f;
            continue;
        }
        f32 fadeInT = 1.0f;
        f32 fadeOutT = 1.0f;
        if (layer.fadeIn > kEpsilon) fadeInT = Clamp(layer.fadeTimer / layer.fadeIn, 0.0f, 1.0f);
        if (layer.fadingOut && layer.fadeOut > kEpsilon)
            fadeOutT = 1.0f - Clamp(layer.fadeTimer / layer.fadeOut, 0.0f, 1.0f);
        // `staticWeight` — установившийся вклад слоя; рампа fade-in — это множитель
        // crossfade, и её нельзя применять дважды.
        const f32 staticWeight = MaxT(layer.weight, 0.0f) * fadeOutT;
        const f32 weight = staticWeight * fadeInT;

        // ---- продвижение времени -------------------------------------------------
        const f32 dur = clip.EffectiveDuration();
        const f32 base = clip.loopEnd > clip.loopStart ? clip.loopStart : 0.0f;
        const f32 oldTime = layer.time;
        const f32 advance = layer.speed * dt;
        f32 sampleTime = oldTime;
        f32 evFrom = oldTime;
        f32 evTo = oldTime;
        bool haveEvents = false;
        bool evLooping = false;
        bool evReversed = false;

        if (dur <= kEpsilon) {
            layer.time = base;
            sampleTime = base;
        } else {
            switch (layer.mode) {
                case AnimPlayMode::Loop: {
                    const f32 next = oldTime + advance;
                    layer.time = base + Wrap01(next - base, dur);
                    sampleTime = layer.time;
                    evFrom = oldTime;
                    evTo = next;
                    haveEvents = true;
                    evLooping = true;
                    break;
                }
                case AnimPlayMode::PingPong: {
                    const f32 cycle = dur * 2.0f;
                    const f32 next = oldTime + advance;
                    const f32 tOld = base + PingPongTime(oldTime - base, dur);
                    const f32 tNew = base + PingPongTime(next - base, dur);
                    layer.time = base + Wrap01(next - base, cycle);
                    sampleTime = tNew;
                    evFrom = tOld;
                    evTo = tNew;
                    haveEvents = true;
                    evReversed = tNew < tOld;
                    break;
                }
                case AnimPlayMode::Once:
                case AnimPlayMode::ClampForever: {
                    f32 next = oldTime + advance;
                    if (next >= base + dur) {
                        next = base + dur;
                        layer.finished = true;
                    }
                    if (next < base) next = base;
                    layer.time = next;
                    sampleTime = next;
                    evFrom = oldTime;
                    evTo = next;
                    haveEvents = true;
                    break;
                }
            }
        }

        // ---- события: ровно один раз за пересечение (oldTime, newTime] -------
        if (haveEvents && weight > 0.0f) {
            eventBuf.clear();
            CollectEventsImpl(clip, evFrom, evTo, evLooping, &eventBuf);
            if (evReversed) {
                for (usize k = eventBuf.size(); k > 0; --k) fireEvent(eventBuf[k - 1]);
            } else {
                for (const AnimationEvent* e : eventBuf) fireEvent(e);
            }
        }

        // ---- сэмплируем клип поверх бинд-позы ----------------------
        for (int i = 0; i < jointCount; ++i) {
            const Joint& j = skeleton_->JointAt(i);
            const usize ui = static_cast<usize>(i);
            scratchA_.translations[ui] = j.localTranslation;
            scratchA_.rotations[ui] = j.localRotation;
            scratchA_.scales[ui] = j.localScale;
        }
        clip.Sample(sampleTime, &scratchA_.locals, &scratchA_.translations, &scratchA_.rotations,
                    &scratchA_.scales);
        if (layer.blendMode == AnimBlendMode::Additive || layer.blendMode == AnimBlendMode::Multiply) {
            // Аддитивные слои трактуют сэмпл как дельту от бинд-позы.
            MakeBindRelative(*skeleton_, &scratchA_);
        }

        // ---- смешиваем в накопитель ----------------------------------
        const std::vector<f32>* mask = layer.mask.weights.empty() ? nullptr : &layer.mask.weights;
        const bool crossfading = layer.fadeIn > kEpsilon && layer.fadeTimer < layer.fadeIn &&
                                 layer.blendMode != AnimBlendMode::Additive &&
                                 layer.blendMode != AnimBlendMode::Multiply;
        if (!contributed && crossfading && scratchB_.JointCount() == jointCount) {
            CrossfadeInto(&pose_, *skeleton_, scratchB_, scratchA_, fadeInT, staticWeight, mask);
        } else {
            BlendLayerInto(&pose_, scratchA_, weight, layer.blendMode, mask);
        }
        if (weight > 0.0f) contributed = true;
    }

    // ---- IK: однопроходный сдвиг позиции к каждой мировой цели -----------
    if (!ikTargets_.empty()) {
        pose_.ComputeWorld(*skeleton_);
        for (const auto& target : ikTargets_) {
            const int joint = target.first;
            const Vec3& worldTarget = target.second.first;
            const f32 w = Clamp(target.second.second, 0.0f, 1.0f);
            if (joint < 0 || joint >= jointCount || w <= 0.0f) continue;
            const usize uj = static_cast<usize>(joint);
            const Mat4& world = pose_.world[uj];
            const Vec3 current{world.at(3, 0), world.at(3, 1), world.at(3, 2)};
            Vec3 delta = (worldTarget - current) * w;
            const int parent = skeleton_->JointAt(joint).parent;
            if (parent >= 0 && parent < jointCount)
                delta = pose_.world[static_cast<usize>(parent)].Inverse().TransformDir(delta);
            pose_.translations[uj] = pose_.translations[uj] + delta;
            ComposeLocalAt(&pose_, uj);
        }
    }

    pose_.ComputeWorld(*skeleton_);
}

const Mat4& Animator::JointWorld(int joint) const {
    static const Mat4 kIdentity = Mat4::Identity();
    if (joint < 0 || joint >= static_cast<int>(pose_.world.size())) return kIdentity;
    return pose_.world[static_cast<usize>(joint)];
}

void Animator::SetIkTarget(int joint, const Vec3& worldPos, f32 weight) {
    if (joint < 0) return;
    for (auto& target : ikTargets_) {
        if (target.first == joint) {
            target.second.first = worldPos;
            target.second.second = weight;
            return;
        }
    }
    ikTargets_.push_back({joint, {worldPos, weight}});
}

void Animator::Reset() {
    fired_.clear();
    ikTargets_.clear();
    lastDt_ = 0.0f;
    const int jointCount = skeleton_ ? skeleton_->JointCount() : 0;
    pose_.Resize(jointCount);
    scratchA_.Resize(jointCount);
    scratchB_.Resize(jointCount);
    if (skeleton_ && jointCount > 0) {
        BuildBindPose(*skeleton_, &pose_);
        BuildBindPose(*skeleton_, &scratchB_);
    }
    for (AnimLayer& l : layers_) {
        l.time = 0.0f;
        l.fadeTimer = 0.0f;
        l.fadingOut = false;
        l.finished = false;
        l.enabled = l.clipIndex >= 0;
    }
}

void Animator::RebuildLayerMasks() {
    const int jointCount = skeleton_ ? skeleton_->JointCount() : 0;
    for (AnimLayer& l : layers_) {
        if (l.boneMaskNames.empty() || jointCount <= 0 || !skeleton_) {
            l.mask.weights.clear();
            continue;
        }
        l.mask.Resize(jointCount, 0.0f);
        for (const std::string& name : l.boneMaskNames) {
            const int joint = skeleton_->FindJoint(name);
            if (joint >= 0 && joint < jointCount) l.mask.weights[static_cast<usize>(joint)] = 1.0f;
        }
    }
}

// ===========================================================================
// Функции плавности (easing)
// ===========================================================================
f32 ApplyEase(EaseType type, f32 t) {
    // Точные конечные точки для каждой кривой (включая семейства с перелётом).
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    const f32 c1 = 1.70158f;
    const f32 c2 = c1 * 1.525f;
    const f32 c3 = c1 + 1.0f;
    const f32 c4 = kTau / 3.0f;
    const f32 c5 = kTau / 4.5f;
    const auto outBounce = [](f32 x) {
        const f32 n1 = 7.5625f;
        const f32 d1 = 2.75f;
        if (x < 1.0f / d1) return n1 * x * x;
        if (x < 2.0f / d1) {
            x -= 1.5f / d1;
            return n1 * x * x + 0.75f;
        }
        if (x < 2.5f / d1) {
            x -= 2.25f / d1;
            return n1 * x * x + 0.9375f;
        }
        x -= 2.625f / d1;
        return n1 * x * x + 0.984375f;
    };
    switch (type) {
        case EaseType::Linear:
            return t;
        case EaseType::InQuad:
            return t * t;
        case EaseType::OutQuad:
            return 1.0f - (1.0f - t) * (1.0f - t);
        case EaseType::InOutQuad:
            return t < 0.5f ? 2.0f * t * t : 1.0f - std::pow(-2.0f * t + 2.0f, 2.0f) / 2.0f;
        case EaseType::InCubic:
            return t * t * t;
        case EaseType::OutCubic:
            return 1.0f - std::pow(1.0f - t, 3.0f);
        case EaseType::InOutCubic:
            return t < 0.5f ? 4.0f * t * t * t : 1.0f - std::pow(-2.0f * t + 2.0f, 3.0f) / 2.0f;
        case EaseType::InQuart:
            return t * t * t * t;
        case EaseType::OutQuart:
            return 1.0f - std::pow(1.0f - t, 4.0f);
        case EaseType::InOutQuart:
            return t < 0.5f ? 8.0f * t * t * t * t : 1.0f - std::pow(-2.0f * t + 2.0f, 4.0f) / 2.0f;
        case EaseType::InSine:
            return 1.0f - std::cos(t * kPi / 2.0f);
        case EaseType::OutSine:
            return std::sin(t * kPi / 2.0f);
        case EaseType::InOutSine:
            return -(std::cos(kPi * t) - 1.0f) / 2.0f;
        case EaseType::InExpo:
            return std::pow(2.0f, 10.0f * t - 10.0f);
        case EaseType::OutExpo:
            return 1.0f - std::pow(2.0f, -10.0f * t);
        case EaseType::InOutExpo:
            return t < 0.5f ? std::pow(2.0f, 20.0f * t - 10.0f) / 2.0f
                            : (2.0f - std::pow(2.0f, -20.0f * t + 10.0f)) / 2.0f;
        case EaseType::InBack:
            return c3 * t * t * t - c1 * t * t;
        case EaseType::OutBack:
            return 1.0f + c3 * std::pow(t - 1.0f, 3.0f) + c1 * std::pow(t - 1.0f, 2.0f);
        case EaseType::InOutBack:
            return t < 0.5f ? (std::pow(2.0f * t, 2.0f) * ((c2 + 1.0f) * 2.0f * t - c2)) / 2.0f
                            : (std::pow(2.0f * t - 2.0f, 2.0f) *
                                   ((c2 + 1.0f) * (t * 2.0f - 2.0f) + c2) +
                               2.0f) /
                                  2.0f;
        case EaseType::InElastic:
            return -std::pow(2.0f, 10.0f * t - 10.0f) * std::sin((t * 10.0f - 10.75f) * c4);
        case EaseType::OutElastic:
            return std::pow(2.0f, -10.0f * t) * std::sin((t * 10.0f - 0.75f) * c4) + 1.0f;
        case EaseType::InOutElastic:
            return t < 0.5f
                       ? -(std::pow(2.0f, 20.0f * t - 10.0f) * std::sin((20.0f * t - 11.125f) * c5)) / 2.0f
                       : (std::pow(2.0f, -20.0f * t + 10.0f) * std::sin((20.0f * t - 11.125f) * c5)) /
                                 2.0f +
                             1.0f;
        case EaseType::InBounce:
            return 1.0f - outBounce(1.0f - t);
        case EaseType::OutBounce:
            return outBounce(t);
        case EaseType::InOutBounce:
            return t < 0.5f ? (1.0f - outBounce(1.0f - 2.0f * t)) / 2.0f
                            : (1.0f + outBounce(2.0f * t - 1.0f)) / 2.0f;
        case EaseType::OutCirc:
            return std::sqrt(1.0f - std::pow(t - 1.0f, 2.0f));
        case EaseType::InCirc:
            return 1.0f - std::sqrt(1.0f - std::pow(t, 2.0f));
        case EaseType::InOutCirc:
            return t < 0.5f ? (1.0f - std::sqrt(1.0f - std::pow(2.0f * t, 2.0f))) / 2.0f
                            : (std::sqrt(1.0f - std::pow(-2.0f * t + 2.0f, 2.0f)) + 1.0f) / 2.0f;
    }
    return t;
}

// ===========================================================================
// Spring
// ===========================================================================
void Spring::Update(f32 dt) {
    if (dt <= 0.0f) return;
    // Полунеявный Эйлер с подшагами: устойчив при переменном времени фрейма.
    const f32 maxStep = 1.0f / 60.0f;
    int substeps = static_cast<int>(std::ceil(dt / maxStep));
    substeps = MinT(MaxT(substeps, 1), 32);
    const f32 h = dt / static_cast<f32>(substeps);
    for (int i = 0; i < substeps; ++i) {
        const f32 accel = stiffness * (target - value) - damping * velocity;
        velocity += accel * h;
        value += velocity * h;
    }
    if (std::fabs(value - target) < 1e-5f && std::fabs(velocity) < 1e-4f) {
        value = target;
        velocity = 0.0f;
    }
}

// ===========================================================================
// Tween
// ===========================================================================
void Tween::To(f32 from, f32 to, f32 duration, EaseType ease, f32 delay) {
    from_ = from;
    to_ = to;
    duration_ = duration;
    delay_ = MaxT(delay, 0.0f);
    ease_ = ease;
    // Отрицательное прошедшее время означает «ждём в задержке» (Restart может проиграть заново).
    elapsed_ = -delay_;
    value_ = from;
    finished_ = false;
}

void Tween::Update(f32 dt) {
    if (finished_ || dt == 0.0f) return;
    elapsed_ += dt * speed_;
    if (elapsed_ < 0.0f) {
        // Всё ещё внутри задержки (или идём назад до старта).
        value_ = from_;
        if (speed_ < 0.0f) {
            elapsed_ = 0.0f;
            finished_ = true;
        }
        return;
    }
    if (duration_ <= kEpsilon) {
        elapsed_ = duration_;
        value_ = to_;
        finished_ = true;
        return;
    }
    if (elapsed_ >= duration_) {
        elapsed_ = duration_;
        value_ = to_;
        finished_ = true;
        return;
    }
    const f32 t = elapsed_ / duration_;
    value_ = from_ + (to_ - from_) * ApplyEase01(ease_, t);
}

void Tween::Restart() {
    elapsed_ = -delay_;
    value_ = from_;
    finished_ = false;
}

}  // namespace crossrender
