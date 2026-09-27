//
// CrossRender
// Автор: xcodestream <xcodestream@gmail.com>
// Лицензия: MIT
// Дата: 15.05.2026
// Описание: скелетная анимация: ключевые треки, клипы, блендинг, твины и пружины.
//
#pragma once

#include "crossrender/core/Base.h"
#include "crossrender/core/Math.h"

#include <memory>
#include <string>
#include <vector>
#include <functional>
#include <unordered_map>

namespace crossrender {

// ---------------------------------------------------------------------------
// Скелет
// ---------------------------------------------------------------------------
struct Joint {
    std::string name;
    int parent = -1;
    Mat4 inverseBindMatrix = Mat4::Identity();
    Mat4 localBindPose = Mat4::Identity();
    Vec3 localTranslation{0, 0, 0};
    Quat localRotation{0, 0, 0, 1};
    Vec3 localScale{1, 1, 1};
};

class Skeleton {
public:
    Skeleton() = default;
    void Clear() { joints_.clear(); nameToIndex_.clear(); }
    int AddJoint(const std::string& name, int parent, const Mat4& localBind = Mat4::Identity());
    void Finalise();
    [[nodiscard]] int JointCount() const { return static_cast<int>(joints_.size()); }
    [[nodiscard]] Joint& JointAt(int i) { return joints_[static_cast<usize>(i)]; }
    [[nodiscard]] const Joint& JointAt(int i) const { return joints_[static_cast<usize>(i)]; }
    [[nodiscard]] int FindJoint(const std::string& name) const;
    [[nodiscard]] const std::vector<Joint>& Joints() const { return joints_; }
    [[nodiscard]] std::vector<Joint>& Joints() { return joints_; }
    // Число суставов в иерархии (используется примером для построения псевдоскелетов).
    [[nodiscard]] int MaxDepth() const;
    static Skeleton MakeHumanoid();
    static Skeleton MakeChain(int count = 4, f32 length = 1.0f);

private:
    std::vector<Joint> joints_;
    std::unordered_map<std::string, int> nameToIndex_;
};

// ---------------------------------------------------------------------------
// Анимационный клип
// ---------------------------------------------------------------------------
enum class Interpolation : u8 { Step, Linear, CubicSpline, Bezier };

template <typename T>
struct Keyframe {
    f32 time = 0;
    T value{};
    T inTangent{}, outTangent{};  // используются CubicSpline
    Interpolation interp = Interpolation::Linear;
};

template <typename T>
struct Track {
    std::vector<Keyframe<T>> keys;
    [[nodiscard]] T Sample(f32 time) const;
    void AddKey(f32 time, const T& value, Interpolation interp = Interpolation::Linear);
    void Sort();
};

using Vec3Track = Track<Vec3>;
using QuatTrack = Track<Quat>;
using FloatTrack = Track<f32>;

struct AnimationEvent {
    f32 time = 0;
    std::string name;
    std::string payload;
};

class AnimationClip {
public:
    std::string name;
    f32 duration = 0;
    f32 ticksPerSecond = 30.0f;
    bool looping = true;
    f32 loopStart = 0, loopEnd = 0;

    struct JointTrack {
        int joint = -1;
        Vec3Track translation;
        QuatTrack rotation;
        Vec3Track scale;
        bool hasTranslation = false, hasRotation = false, hasScale = false;
    };
    std::vector<JointTrack> tracks;
    std::vector<AnimationEvent> events;
    // Извлечение root motion.
    bool applyRootMotion = false;
    int rootJoint = 0;

    [[nodiscard]] int FindTrack(int joint) const;
    [[nodiscard]] f32 EffectiveDuration() const {
        return loopEnd > loopStart ? loopEnd - loopStart : duration;
    }
    // Семплирует клип в `locals` (изменяется до числа суставов скелета).
    void Sample(f32 time, std::vector<Mat4>* locals, std::vector<Vec3>* translations = nullptr,
                std::vector<Quat>* rotations = nullptr, std::vector<Vec3>* scales = nullptr) const;
    // События в интервале (from, to]; используются для игровых колбэков.
    void CollectEvents(f32 from, f32 to, std::vector<const AnimationEvent*>* out) const;

    static AnimationClip MakeWave(f32 duration = 2.0f);
    static AnimationClip MakeBounce(f32 duration = 1.5f);
    static AnimationClip MakeWalk(int jointCount, f32 duration = 1.0f);
    static AnimationClip MakeSpin(f32 duration = 1.0f);
};

// ---------------------------------------------------------------------------
// Поза и блендинг
// ---------------------------------------------------------------------------
struct Pose {
    std::vector<Vec3> translations;
    std::vector<Quat> rotations;
    std::vector<Vec3> scales;
    std::vector<Mat4> locals;
    std::vector<Mat4> world;
    void Resize(int jointCount);
    [[nodiscard]] int JointCount() const { return static_cast<int>(rotations.size()); }
    void Reset();
    void ComputeWorld(const Skeleton& skeleton);
};

struct BlendWeights {
    std::vector<f32> weights;  // на сустав: 0 = взять референс, 1 = взять цель
    void Resize(int jointCount, f32 value = 1.0f);
};

enum class AnimBlendMode : u8 {
    Override,   // лерп к целевой позе
    Additive,   // добавляет цель (в локальном пространстве) поверх референса
    Multiply,   // умножение
    Masked,     // переопределяет только суставы, выбранные BlendWeights
};

// Базовые операции над позами (все реализации оригинальные).
void BlendPoses(Pose* out, const Pose& a, const Pose& b, f32 t,
                AnimBlendMode mode = AnimBlendMode::Override);
void BlendPosesMasked(Pose* out, const Pose& a, const Pose& b, const BlendWeights& weights);
void AdditivePose(Pose* out, const Pose& base, const Pose& additive, f32 weight);
void NormalizePose(Pose* pose);
void MirrorPose(Pose* pose, const Skeleton& skeleton);

// ---------------------------------------------------------------------------
// Animator: послойное воспроизведение с кроссфейдами и событиями
// ---------------------------------------------------------------------------
enum class AnimPlayMode : u8 { Once, Loop, PingPong, ClampForever };

struct AnimLayer {
    std::string name = "Base";
    int clipIndex = -1;
    f32 time = 0;
    f32 speed = 1.0f;
    f32 weight = 1.0f;
    AnimPlayMode mode = AnimPlayMode::Loop;
    AnimBlendMode blendMode = AnimBlendMode::Override;
    bool enabled = true;
    f32 fadeIn = 0.0f, fadeOut = 0.0f;
    f32 fadeTimer = 0.0f;
    bool fadingOut = false;
    bool finished = false;
    // Необязательная маска по суставам (пустая = влияет на все суставы).
    BlendWeights mask;
    std::vector<std::string> boneMaskNames;
};

class Animator {
public:
    Animator() = default;

    void SetSkeleton(Skeleton* skeleton);
    [[nodiscard]] Skeleton* GetSkeleton() const { return skeleton_; }

    int AddClip(AnimationClip clip);
    [[nodiscard]] int ClipCount() const { return static_cast<int>(clips_.size()); }
    [[nodiscard]] AnimationClip& Clip(int i) { return clips_[static_cast<usize>(i)]; }
    [[nodiscard]] const AnimationClip& Clip(int i) const { return clips_[static_cast<usize>(i)]; }
    [[nodiscard]] int FindClip(const std::string& name) const;

    // Играет клип на слое, с кроссфейдом от того, что там было.
    void Play(int clipIndex, f32 fadeSeconds = 0.2f, int layer = 0, bool restart = true,
              AnimPlayMode mode = AnimPlayMode::Loop);
    void Stop(int layer = 0, f32 fadeSeconds = 0.0f);
    void Pause(bool paused) { paused_ = paused; }
    [[nodiscard]] bool Paused() const { return paused_; }
    void SetLayerWeight(int layer, f32 weight);
    [[nodiscard]] f32 LayerWeight(int layer) const;
    void SetLayerSpeed(int layer, f32 speed);
    // Аддитивно смешивает клип поверх текущего результата.
    void PlayAdditive(int clipIndex, f32 weight, int layer = 1);
    // Маскированное переопределение по именованным костям.
    void PlayMasked(int clipIndex, const std::vector<std::string>& bones, f32 weight, int layer = 1);
    // Blend space: смешивает 2-4 клипа по 1D/2D-параметру.
    void PlayBlendSpace1D(const std::vector<int>& clipIndices, const std::vector<f32>& thresholds,
                          f32 parameter, f32 fadeSeconds = 0.15f, int layer = 0);

    void SetLayerTime(int layer, f32 time);
    [[nodiscard]] f32 LayerTime(int layer) const;
    [[nodiscard]] f32 LayerNormalizedTime(int layer) const;
    [[nodiscard]] bool LayerFinished(int layer) const;

    void Update(f32 dt);

    [[nodiscard]] const Pose& CurrentPose() const { return pose_; }
    [[nodiscard]] Pose& MutablePose() { return pose_; }
    // Быстрый доступ к мировой матрице сустава.
    [[nodiscard]] const Mat4* WorldMatrices() const { return pose_.world.data(); }
    [[nodiscard]] const Mat4& JointWorld(int joint) const;

    // События, сработавшие во время последнего Update.
    [[nodiscard]] const std::vector<const AnimationEvent*>& FiredEvents() const { return fired_; }
    void SetEventCallback(std::function<void(const AnimationEvent&)> cb) { eventCb_ = std::move(cb); }

    // Подобие foot IK: прижимает мировую трансляцию сустава к цели.
    void SetIkTarget(int joint, const Vec3& worldPos, f32 weight);
    void ClearIkTargets() { ikTargets_.clear(); }

    void Reset();
    void RebuildLayerMasks();

    [[nodiscard]] const std::vector<AnimLayer>& Layers() const { return layers_; }
    [[nodiscard]] int LayerCount() const { return static_cast<int>(layers_.size()); }
    void EnsureLayerCount(int count);

private:
    Skeleton* skeleton_ = nullptr;
    std::vector<AnimationClip> clips_;
    std::vector<AnimLayer> layers_;
    Pose pose_, scratchA_, scratchB_;
    std::vector<const AnimationEvent*> fired_;
    std::vector<std::pair<int, std::pair<Vec3, f32>>> ikTargets_;
    std::function<void(const AnimationEvent&)> eventCb_;
    bool paused_ = false;
    f32 lastDt_ = 0;
};

// ---------------------------------------------------------------------------
// Простые утилиты твинов/пружин (2D-анимация, UI, карточная игра)
// ---------------------------------------------------------------------------
enum class EaseType : u8 {
    Linear, InQuad, OutQuad, InOutQuad, InCubic, OutCubic, InOutCubic,
    InQuart, OutQuart, InOutQuart, InSine, OutSine, InOutSine,
    InExpo, OutExpo, InOutExpo, InBack, OutBack, InOutBack,
    InElastic, OutElastic, InOutElastic, InBounce, OutBounce, InOutBounce,
    OutCirc, InCirc, InOutCirc
};

f32 ApplyEase(EaseType type, f32 t);
[[nodiscard]] inline f32 ApplyEase01(EaseType type, f32 t) { return ApplyEase(type, Clamp(t, 0.0f, 1.0f)); }

// Пружина, независимая от частоты кадров (позиция + скорость).
struct Spring {
    f32 value = 0, velocity = 0, target = 0;
    f32 stiffness = 180.0f, damping = 22.0f;
    void Update(f32 dt);
    void Snap(f32 v) {
        value = target = v;
        velocity = 0;
    }
};

// Таймлайн, управляющий несколькими скалярными треками (используется анимациями меню/карт).
class Tween {
public:
    void To(f32 from, f32 to, f32 duration, EaseType ease, f32 delay = 0.0f);
    void Update(f32 dt);
    [[nodiscard]] f32 Value() const { return value_; }
    [[nodiscard]] bool Finished() const { return finished_; }
    void Restart();
    void SetSpeed(f32 s) { speed_ = s; }

private:
    f32 from_ = 0, to_ = 0, duration_ = 1, elapsed_ = 0, delay_ = 0, value_ = 0, speed_ = 1;
    EaseType ease_ = EaseType::Linear;
    bool finished_ = false;
};

}  // namespace crossrender
