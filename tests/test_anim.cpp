// Тесты модуля анимации (треки, клипы, скелеты, смешивание поз,
// послойное воспроизведение, easing, пружины, твины).
#include "crossrender/anim/Anim.h"

#include "crossrender/test/Test.h"

#include <cmath>
#include <string>
#include <vector>

using namespace crossrender;

namespace {

// Клип, удерживающий один сустав при постоянной локальной трансляции.
AnimationClip ConstantClip(const char* name, const Vec3& value, f32 duration = 1.0f, bool looping = true) {
    AnimationClip clip;
    clip.name = name;
    clip.duration = duration;
    clip.looping = looping;
    AnimationClip::JointTrack track;
    track.joint = 0;
    track.hasTranslation = true;
    track.translation.AddKey(0.0f, value, Interpolation::Linear);
    clip.tracks.push_back(std::move(track));
    return clip;
}

// Клип, изменяющий X-трансляцию сустава от from до to за duration.
AnimationClip RampClip(const char* name, f32 from, f32 to, f32 duration = 1.0f, bool looping = true) {
    AnimationClip clip;
    clip.name = name;
    clip.duration = duration;
    clip.looping = looping;
    AnimationClip::JointTrack track;
    track.joint = 0;
    track.hasTranslation = true;
    track.translation.AddKey(0.0f, Vec3{from, 0, 0}, Interpolation::Linear);
    track.translation.AddKey(duration, Vec3{to, 0, 0}, Interpolation::Linear);
    clip.tracks.push_back(std::move(track));
    return clip;
}

// Клип, удерживающий один сустав при постоянном локальном повороте.
AnimationClip ConstantRotationClip(const char* name, const Quat& value, f32 duration = 1.0f) {
    AnimationClip clip;
    clip.name = name;
    clip.duration = duration;
    clip.looping = true;
    AnimationClip::JointTrack track;
    track.joint = 0;
    track.hasRotation = true;
    track.rotation.AddKey(0.0f, value, Interpolation::Linear);
    clip.tracks.push_back(std::move(track));
    return clip;
}

void SetLocalFromTrs(Pose* pose, int joint) {
    const usize i = static_cast<usize>(joint);
    pose->locals[i] = Mat4::Translate(pose->translations[i]) * pose->rotations[i].ToMat4() *
                      Mat4::Scale(pose->scales[i]);
}

}  // namespace

// ---------------------------------------------------------------------------
// Треки
// ---------------------------------------------------------------------------
ENG_TEST(Anim, TrackLinearMidpointAndClamp) {
    Track<Vec3> track;
    track.AddKey(0.0f, Vec3{0, 0, 0});
    track.AddKey(1.0f, Vec3{10, 0, 0});
    ENG_CHECK_NEAR(track.Sample(0.5f).x, 5.0f, 1e-4f);
    ENG_CHECK_NEAR(track.Sample(0.25f).x, 2.5f, 1e-4f);
    // До первого / после последнего ключа — клампинг в обоих случаях.
    ENG_CHECK_NEAR(track.Sample(-5.0f).x, 0.0f, 1e-4f);
    ENG_CHECK_NEAR(track.Sample(9.0f).x, 10.0f, 1e-4f);

    // Ключи, добавленные не по порядку, остаются работоспособными.
    Track<f32> scalar;
    scalar.AddKey(2.0f, 4.0f);
    scalar.AddKey(0.0f, 0.0f);
    scalar.AddKey(1.0f, 2.0f);
    ENG_CHECK_EQ(static_cast<int>(scalar.keys.size()), 3);
    ENG_CHECK_NEAR(scalar.Sample(0.5f), 1.0f, 1e-4f);
    ENG_CHECK_NEAR(scalar.Sample(1.5f), 3.0f, 1e-4f);
    scalar.Sort();
    ENG_CHECK_NEAR(scalar.keys[0].time, 0.0f, 1e-6f);
    ENG_CHECK_NEAR(scalar.keys[2].time, 2.0f, 1e-6f);
}

ENG_TEST(Anim, TrackStepHold) {
    Track<f32> track;
    track.AddKey(0.0f, 0.0f, Interpolation::Step);
    track.AddKey(1.0f, 10.0f, Interpolation::Step);
    ENG_CHECK_NEAR(track.Sample(0.0f), 0.0f, 1e-6f);
    ENG_CHECK_NEAR(track.Sample(0.5f), 0.0f, 1e-6f);
    ENG_CHECK_NEAR(track.Sample(0.999f), 0.0f, 1e-6f);
    ENG_CHECK_NEAR(track.Sample(1.0f), 10.0f, 1e-6f);
    ENG_CHECK_NEAR(track.Sample(2.0f), 10.0f, 1e-6f);
}

ENG_TEST(Anim, TrackQuatSlerp) {
    Track<Quat> track;
    track.AddKey(0.0f, Quat::Identity());
    track.AddKey(1.0f, Quat::FromAxisAngle(Vec3{0, 1, 0}, kPi * 0.5f));
    const Quat mid = track.Sample(0.5f);
    const Vec3 rotated = mid * Vec3{0, 0, 1};
    ENG_CHECK_NEAR(rotated.x, 0.7071067f, 1e-3f);  // sin(45 degrees)
    ENG_CHECK_NEAR(rotated.z, 0.7071067f, 1e-3f);  // cos(45 degrees)
}

// ---------------------------------------------------------------------------
// Сэмплирование клипов
// ---------------------------------------------------------------------------
ENG_TEST(Anim, ClipSamplesTranslationRotationScale) {
    AnimationClip clip;
    clip.name = "trs";
    clip.duration = 1.0f;
    clip.looping = false;
    AnimationClip::JointTrack track;
    track.joint = 0;
    track.hasTranslation = true;
    track.hasRotation = true;
    track.hasScale = true;
    track.translation.AddKey(0.0f, Vec3{0, 0, 0});
    track.translation.AddKey(1.0f, Vec3{4, 0, 0});
    track.rotation.AddKey(0.0f, Quat::Identity());
    track.rotation.AddKey(1.0f, Quat::FromAxisAngle(Vec3{0, 1, 0}, kPi * 0.5f));
    track.scale.AddKey(0.0f, Vec3{1, 1, 1});
    track.scale.AddKey(1.0f, Vec3{3, 1, 1});
    clip.tracks.push_back(std::move(track));

    std::vector<Mat4> locals;
    std::vector<Vec3> translations;
    std::vector<Quat> rotations;
    std::vector<Vec3> scales;
    clip.Sample(0.5f, &locals, &translations, &rotations, &scales);
    ENG_CHECK_EQ(static_cast<int>(translations.size()), 1);
    ENG_CHECK_EQ(static_cast<int>(locals.size()), 1);
    ENG_CHECK_NEAR(translations[0].x, 2.0f, 1e-3f);
    ENG_CHECK_NEAR(scales[0].x, 2.0f, 1e-3f);
    const Vec3 dir = rotations[0] * Vec3{0, 0, 1};
    ENG_CHECK_NEAR(dir.x, 0.7071067f, 1e-3f);
    ENG_CHECK_NEAR(dir.z, 0.7071067f, 1e-3f);
    // locals компонуются как translation * rotation * scale.
    const Vec3 origin = locals[0].TransformPoint(Vec3{0, 0, 0});
    ENG_CHECK_NEAR(origin.x, 2.0f, 1e-3f);
    const Vec3 unitY = locals[0].TransformDir(Vec3{0, 1, 0});
    ENG_CHECK_NEAR(unitY.y, 1.0f, 1e-3f);

    // За концом (без зацикливания) удерживается последний ключ.
    std::vector<Vec3> lateT;
    clip.Sample(5.0f, nullptr, &lateT, nullptr, nullptr);
    ENG_CHECK_NEAR(lateT[0].x, 4.0f, 1e-3f);
    ENG_CHECK_EQ(clip.FindTrack(0), 0);
    ENG_CHECK_EQ(clip.FindTrack(3), -1);
}

ENG_TEST(Anim, ClipLoopWrap) {
    AnimationClip clip = RampClip("loop", 0.0f, 10.0f, 1.0f, true);
    std::vector<Vec3> t;
    clip.Sample(0.25f, nullptr, &t, nullptr, nullptr);
    ENG_CHECK_NEAR(t[0].x, 2.5f, 1e-3f);
    clip.Sample(1.25f, nullptr, &t, nullptr, nullptr);
    ENG_CHECK_NEAR(t[0].x, 2.5f, 1e-3f);
    clip.Sample(2.75f, nullptr, &t, nullptr, nullptr);
    ENG_CHECK_NEAR(t[0].x, 7.5f, 1e-3f);
    ENG_CHECK_NEAR(clip.EffectiveDuration(), 1.0f, 1e-6f);
}

// ---------------------------------------------------------------------------
// События
// ---------------------------------------------------------------------------
ENG_TEST(Anim, EventsFireOncePerCrossing) {
    Skeleton skeleton = Skeleton::MakeChain(1);
    Animator animator;
    animator.SetSkeleton(&skeleton);
    AnimationClip clip;
    clip.name = "events";
    clip.duration = 1.0f;
    clip.looping = false;
    clip.events.push_back(AnimationEvent{0.1f, "e1", ""});
    clip.events.push_back(AnimationEvent{0.4f, "e2", ""});
    clip.events.push_back(AnimationEvent{0.9f, "e3", ""});
    const int index = animator.AddClip(clip);

    int callbacks = 0;
    std::vector<std::string> names;
    animator.SetEventCallback([&](const AnimationEvent& e) {
        ++callbacks;
        names.push_back(e.name);
    });
    animator.Play(index, 0.0f, 0, true, AnimPlayMode::ClampForever);

    // Один кадр, охватывающий два события, срабатывает оба — ровно один раз.
    animator.Update(0.5f);
    ENG_CHECK_EQ(static_cast<int>(animator.FiredEvents().size()), 2);
    ENG_CHECK_EQ(callbacks, 2);

    // Следующий кадр пересекает третье событие.
    animator.Update(0.5f);
    ENG_CHECK_EQ(static_cast<int>(animator.FiredEvents().size()), 1);
    ENG_CHECK_EQ(callbacks, 3);
    ENG_CHECK_STR_EQ(names[2], std::string("e3"));

    // Зажато на конце: повторного срабатывания нет.
    animator.Update(0.5f);
    ENG_CHECK_EQ(static_cast<int>(animator.FiredEvents().size()), 0);
    ENG_CHECK_EQ(callbacks, 3);
    ENG_CHECK(animator.LayerFinished(0));
    ENG_CHECK_NEAR(animator.LayerTime(0), 1.0f, 1e-4f);
}

ENG_TEST(Anim, EventsFireOncePerCrossingWhileLooping) {
    Skeleton skeleton = Skeleton::MakeChain(1);
    Animator animator;
    animator.SetSkeleton(&skeleton);
    AnimationClip clip;
    clip.name = "events-loop";
    clip.duration = 1.0f;
    clip.looping = true;
    clip.events.push_back(AnimationEvent{0.25f, "a", ""});
    clip.events.push_back(AnimationEvent{0.75f, "b", ""});
    const int index = animator.AddClip(clip);

    std::vector<std::string> names;
    animator.SetEventCallback([&](const AnimationEvent& e) { names.push_back(e.name); });
    animator.Play(index, 0.0f, 0, true, AnimPlayMode::Loop);

    animator.Update(0.5f);  // (0.0, 0.5]  -> a
    ENG_CHECK_EQ(static_cast<int>(names.size()), 1);
    ENG_CHECK_STR_EQ(names[0], std::string("a"));
    animator.Update(0.5f);  // (0.5, 1.0]  -> b (пересекает шов)
    ENG_CHECK_EQ(static_cast<int>(names.size()), 2);
    ENG_CHECK_STR_EQ(names[1], std::string("b"));
    animator.Update(0.5f);  // (0.0, 0.5]  -> снова a, b не должно сработать повторно
    ENG_CHECK_EQ(static_cast<int>(names.size()), 3);
    ENG_CHECK_STR_EQ(names[2], std::string("a"));
    // Кадр, покрывающий целый цикл, срабатывает каждое событие ровно один раз.
    animator.Update(1.0f);  // (0.5, 1.5] -> b @0.75 and a @1.25
    ENG_CHECK_EQ(static_cast<int>(names.size()), 5);
    ENG_CHECK_STR_EQ(names[3], std::string("b"));
    ENG_CHECK_STR_EQ(names[4], std::string("a"));
}

ENG_TEST(Anim, CollectEventsDirect) {
    AnimationClip clip;
    clip.duration = 2.0f;
    clip.looping = false;
    clip.events.push_back(AnimationEvent{0.5f, "x", ""});
    clip.events.push_back(AnimationEvent{1.5f, "y", ""});
    std::vector<const AnimationEvent*> out;
    clip.CollectEvents(0.0f, 1.0f, &out);
    ENG_CHECK_EQ(static_cast<int>(out.size()), 1);
    clip.CollectEvents(0.0f, 2.0f, &out);
    ENG_CHECK_EQ(static_cast<int>(out.size()), 2);
    clip.CollectEvents(0.5f, 1.5f, &out);  // открыт слева: 0.5 исключён, 1.5 включён
    ENG_CHECK_EQ(static_cast<int>(out.size()), 1);
}

ENG_TEST(Anim, ClipLoopWindowFromLoopStartToLoopEnd) {
    AnimationClip clip = RampClip("window", 0.0f, 10.0f, 2.0f, true);
    clip.loopStart = 0.5f;
    clip.loopEnd = 1.5f;
    ENG_CHECK_NEAR(clip.EffectiveDuration(), 1.0f, 1e-6f);
    std::vector<Vec3> t;
    clip.Sample(1.0f, nullptr, &t, nullptr, nullptr);
    ENG_CHECK_NEAR(t[0].x, 5.0f, 1e-3f);
    clip.Sample(0.5f, nullptr, &t, nullptr, nullptr);
    ENG_CHECK_NEAR(t[0].x, 2.5f, 1e-3f);
    clip.Sample(1.5f, nullptr, &t, nullptr, nullptr);
    ENG_CHECK_NEAR(t[0].x, 7.5f, 1e-3f);
    clip.Sample(2.5f, nullptr, &t, nullptr, nullptr);  // один полный цикл после начала
    ENG_CHECK_NEAR(t[0].x, 2.5f, 1e-3f);
    clip.Sample(2.1f, nullptr, &t, nullptr, nullptr);
    ENG_CHECK_NEAR(t[0].x, 5.5f, 1e-3f);
}

ENG_TEST(Anim, NonLoopingModeHoldsTheLastPoseOfALoopingClip) {
    Skeleton skeleton = Skeleton::MakeChain(1);
    Animator animator;
    animator.SetSkeleton(&skeleton);
    const int clip = animator.AddClip(RampClip("hold", 0.0f, 10.0f, 1.0f, true));
    animator.Play(clip, 0.0f, 0, true, AnimPlayMode::ClampForever);
    animator.Update(2.0f);
    ENG_CHECK(animator.LayerFinished(0));
    ENG_CHECK_NEAR(animator.LayerTime(0), 1.0f, 1e-4f);
    // На шве должен удерживаться последний ключ, а не возврат к первому.
    ENG_CHECK_NEAR(animator.CurrentPose().translations[0].x, 10.0f, 1e-3f);
}

// ---------------------------------------------------------------------------
// Смешивание
// ---------------------------------------------------------------------------
ENG_TEST(Anim, CrossfadeBetweenClips) {
    Skeleton skeleton = Skeleton::MakeChain(1);
    Animator animator;
    animator.SetSkeleton(&skeleton);
    const int a = animator.AddClip(ConstantClip("A", Vec3{0, 0, 0}));
    const int b = animator.AddClip(ConstantClip("B", Vec3{10, 0, 0}));

    animator.Play(a, 0.0f);
    animator.Update(0.1f);
    ENG_CHECK_NEAR(animator.CurrentPose().translations[0].x, 0.0f, 1e-3f);

    animator.Play(b, 0.4f);
    animator.Update(0.2f);  // середина фейда
    const f32 middle = animator.CurrentPose().translations[0].x;
    ENG_CHECK(middle > 0.0f && middle < 10.0f);
    ENG_CHECK_NEAR(middle, 5.0f, 0.5f);

    animator.Update(0.25f);  // фейд завершён
    ENG_CHECK_NEAR(animator.CurrentPose().translations[0].x, 10.0f, 1e-3f);
    ENG_CHECK_NEAR(animator.LayerWeight(0), 1.0f, 1e-4f);
}

ENG_TEST(Anim, AdditiveZeroDeltaLeavesPoseUnchanged) {
    Skeleton skeleton = Skeleton::MakeChain(2);
    Pose base;
    base.Resize(2);
    base.translations[0] = Vec3{5, 0, 0};
    base.rotations[0] = Quat::FromAxisAngle(Vec3{0, 1, 0}, 0.7f);
    SetLocalFromTrs(&base, 0);
    SetLocalFromTrs(&base, 1);

    Pose zero;
    zero.Resize(2);  // identity locals == нулевая дельта

    Pose out;
    AdditivePose(&out, base, zero, 1.0f);
    ENG_CHECK_NEAR(out.translations[0].x, 5.0f, 1e-4f);
    const Vec3 baseDir = base.rotations[0] * Vec3{1, 0, 0};
    const Vec3 outDir = out.rotations[0] * Vec3{1, 0, 0};
    ENG_CHECK_NEAR(outDir.x, baseDir.x, 1e-4f);
    ENG_CHECK_NEAR(outDir.z, baseDir.z, 1e-4f);

    // Вес 0 тоже no-op, вес 1 с реальной дельтой компонуется.
    Pose delta;
    delta.Resize(2);
    delta.translations[1] = Vec3{0, 1, 0};
    delta.rotations[1] = Quat::FromAxisAngle(Vec3{0, 1, 0}, 0.5f);
    AdditivePose(&out, base, delta, 0.0f);
    ENG_CHECK_NEAR(out.translations[1].y, base.translations[1].y, 1e-4f);
    AdditivePose(&out, base, delta, 1.0f);
    ENG_CHECK_NEAR(out.translations[1].y, base.translations[1].y + 1.0f, 1e-4f);

    // Через аниматор: аддитивный слой с нулевой дельтой ничего не меняет.
    Animator animator;
    animator.SetSkeleton(&skeleton);
    const int main = animator.AddClip(ConstantClip("main", Vec3{5, 0, 0}));
    const int additive = animator.AddClip(ConstantClip("zero", Vec3{0, 0, 0}));
    animator.Play(main, 0.0f);
    animator.Update(0.1f);
    ENG_CHECK_NEAR(animator.CurrentPose().translations[0].x, 5.0f, 1e-3f);
    animator.PlayAdditive(additive, 1.0f, 1);
    animator.Update(0.1f);
    ENG_CHECK_NEAR(animator.CurrentPose().translations[0].x, 5.0f, 1e-3f);
    // Аддитивный слой не должен трогать суставы, которые базовый клип оставляет в bind-позе.
    ENG_CHECK_NEAR(animator.CurrentPose().translations[1].x, 1.0f, 1e-3f);

    // Реальный аддитивный клип добавляет дельту относительно bind-позы, умноженную на вес.
    const int deltaClip = animator.AddClip(ConstantClip("delta", Vec3{2, 0, 0}));
    animator.PlayAdditive(deltaClip, 0.5f, 1);
    animator.Update(0.1f);
    ENG_CHECK_NEAR(animator.CurrentPose().translations[0].x, 6.0f, 1e-3f);
    ENG_CHECK_NEAR(animator.CurrentPose().translations[1].x, 1.0f, 1e-3f);

    // Повороты компонуются (умножение кватернионов): база 20 градусов + дельта 30 градусов.
    Animator rotator;
    rotator.SetSkeleton(&skeleton);
    const int baseRot = rotator.AddClip(
        ConstantRotationClip("baseRot", Quat::FromAxisAngle(Vec3{0, 1, 0}, Radians(20.0f))));
    const int addRot = rotator.AddClip(
        ConstantRotationClip("addRot", Quat::FromAxisAngle(Vec3{0, 1, 0}, Radians(30.0f))));
    rotator.Play(baseRot, 0.0f);
    rotator.PlayAdditive(addRot, 1.0f, 1);
    rotator.Update(0.05f);
    const Vec3 axis = rotator.CurrentPose().rotations[0] * Vec3{0, 0, 1};
    ENG_CHECK_NEAR(axis.x, std::sin(Radians(50.0f)), 1e-3f);
    ENG_CHECK_NEAR(axis.z, std::cos(Radians(50.0f)), 1e-3f);
}

ENG_TEST(Anim, AdditiveDeltaIsRelativeToTheBindPose) {
    // Дочерний сустав имеет неединичный bind-поворот, поэтому здесь проверяется
    // дельта bind^-1 * sample в локальном пространстве (и обратная матрица за ней).
    Skeleton skeleton;
    skeleton.AddJoint("Root", -1);
    skeleton.AddJoint("Child", 0, Mat4::Translate(Vec3{1, 0, 0}) * Mat4::RotateY(Radians(30.0f)));
    skeleton.Finalise();
    ENG_CHECK_NEAR(skeleton.JointAt(1).localTranslation.x, 1.0f, 1e-4f);
    const Vec3 bindAxis = skeleton.JointAt(1).localRotation * Vec3{0, 0, 1};
    ENG_CHECK_NEAR(bindAxis.x, std::sin(Radians(30.0f)), 1e-3f);

    const auto childClip = [](const char* name, f32 degrees) {
        AnimationClip clip;
        clip.name = name;
        clip.duration = 1.0f;
        clip.looping = true;
        AnimationClip::JointTrack track;
        track.joint = 1;
        track.hasTranslation = true;
        track.hasRotation = true;
        track.translation.AddKey(0.0f, Vec3{1, 0, 0});
        track.rotation.AddKey(0.0f, Quat::FromAxisAngle(Vec3{0, 1, 0}, Radians(degrees)));
        clip.tracks.push_back(std::move(track));
        return clip;
    };

    Animator animator;
    animator.SetSkeleton(&skeleton);
    const int atBind = animator.AddClip(childClip("atBind", 30.0f));
    animator.Play(atBind, 0.0f);
    animator.Update(0.05f);
    const Vec3 before = animator.CurrentPose().rotations[1] * Vec3{0, 0, 1};

    // Аддитивный клип, стоящий ровно в bind-позе, является no-op.
    const int zero = animator.AddClip(childClip("zero", 30.0f));
    animator.PlayAdditive(zero, 1.0f, 1);
    animator.Update(0.05f);
    const Vec3 unchanged = animator.CurrentPose().rotations[1] * Vec3{0, 0, 1};
    ENG_CHECK_NEAR(unchanged.x, before.x, 1e-3f);
    ENG_CHECK_NEAR(unchanged.z, before.z, 1e-3f);

    // Клип на 60 градусов добавляет дельту в 30 градусов поверх базовой позы.
    const int add60 = animator.AddClip(childClip("add60", 60.0f));
    animator.PlayAdditive(add60, 1.0f, 1);
    animator.Update(0.05f);
    const Vec3 summed = animator.CurrentPose().rotations[1] * Vec3{0, 0, 1};
    ENG_CHECK_NEAR(summed.x, std::sin(Radians(60.0f)), 1e-3f);
    ENG_CHECK_NEAR(summed.z, std::cos(Radians(60.0f)), 1e-3f);
}

ENG_TEST(Anim, BlendPosesOverrideInterpolatesTrs) {
    Pose a;
    a.Resize(2);
    Pose b;
    b.Resize(2);
    b.translations[0] = Vec3{4, 0, 0};
    b.rotations[0] = Quat::FromAxisAngle(Vec3{0, 1, 0}, kPi * 0.5f);
    b.scales[0] = Vec3{2, 2, 2};
    SetLocalFromTrs(&b, 0);

    Pose out;
    BlendPoses(&out, a, b, 0.0f);
    ENG_CHECK_NEAR(out.translations[0].x, 0.0f, 1e-4f);
    BlendPoses(&out, a, b, 1.0f);
    ENG_CHECK_NEAR(out.translations[0].x, 4.0f, 1e-4f);
    ENG_CHECK_NEAR(out.scales[0].x, 2.0f, 1e-4f);
    const Vec3 full = out.rotations[0] * Vec3{0, 0, 1};
    ENG_CHECK_NEAR(full.x, 1.0f, 1e-3f);

    BlendPoses(&out, a, b, 0.5f);
    ENG_CHECK_NEAR(out.translations[0].x, 2.0f, 1e-4f);
    ENG_CHECK_NEAR(out.scales[0].x, 1.5f, 1e-4f);
    const Vec3 half = out.rotations[0] * Vec3{0, 0, 1};
    ENG_CHECK_NEAR(half.x, 0.7071067f, 1e-3f);

    // t клампится, а ненормализованные повороты на выходе нормализованы.
    Pose bad;
    bad.Resize(1);
    bad.rotations[0] = Quat{0, 3, 0, 4};
    BlendPoses(&out, a, bad, 4.0f);
    const Quat q = out.rotations[0];
    ENG_CHECK_NEAR(std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w), 1.0f, 1e-4f);
    ENG_CHECK_NEAR(q.w, 0.8f, 1e-4f);

    NormalizePose(&out);
    ENG_CHECK_NEAR(out.rotations[0].w, 0.8f, 1e-4f);
    ENG_CHECK_NEAR(out.locals[0].at(1, 1), 1.0f, 1e-4f);
}

ENG_TEST(Anim, MaskedBlendOnlyAffectsMaskedJoints) {
    Pose reference;
    reference.Resize(3);
    Pose target;
    target.Resize(3);
    for (int i = 0; i < 3; ++i) {
        target.translations[static_cast<usize>(i)] = Vec3{static_cast<f32>(i + 1), 0, 0};
        SetLocalFromTrs(&target, i);
    }
    BlendWeights weights;
    weights.Resize(3, 0.0f);
    weights.weights[1] = 1.0f;

    Pose out;
    BlendPosesMasked(&out, reference, target, weights);
    ENG_CHECK_NEAR(out.translations[0].x, 0.0f, 1e-4f);
    ENG_CHECK_NEAR(out.translations[1].x, 2.0f, 1e-4f);
    ENG_CHECK_NEAR(out.translations[2].x, 0.0f, 1e-4f);

    // Частичная маска смешивает пропорционально.
    weights.weights[2] = 0.5f;
    BlendPosesMasked(&out, reference, target, weights);
    ENG_CHECK_NEAR(out.translations[2].x, 1.5f, 1e-4f);

    // Маски слоёв аниматора используют имена костей.
    Skeleton skeleton = Skeleton::MakeChain(3);
    Animator animator;
    animator.SetSkeleton(&skeleton);
    const int base = animator.AddClip(ConstantClip("base", Vec3{0, 0, 0}));
    AnimationClip mover;
    mover.name = "mover";
    mover.duration = 1.0f;
    mover.looping = true;
    AnimationClip::JointTrack track;
    track.joint = 2;
    track.hasTranslation = true;
    track.translation.AddKey(0.0f, Vec3{9, 0, 0});
    mover.tracks.push_back(std::move(track));
    const int masked = animator.AddClip(mover);
    animator.Play(base, 0.0f);
    animator.Update(0.05f);
    animator.PlayMasked(masked, {"Chain2"}, 1.0f, 1);
    animator.Update(0.05f);
    ENG_CHECK_NEAR(animator.CurrentPose().translations[0].x, 0.0f, 1e-3f);
    ENG_CHECK_NEAR(animator.CurrentPose().translations[1].x, 1.0f, 1e-3f);
    ENG_CHECK_NEAR(animator.CurrentPose().translations[2].x, 9.0f, 1e-3f);
}

ENG_TEST(Anim, BlendSpace1DMidpointIsFiftyFifty) {
    Skeleton skeleton = Skeleton::MakeChain(1);
    Animator animator;
    animator.SetSkeleton(&skeleton);
    const int a = animator.AddClip(ConstantClip("bsA", Vec3{0, 0, 0}));
    const int b = animator.AddClip(ConstantClip("bsB", Vec3{10, 0, 0}));

    animator.PlayBlendSpace1D({a, b}, {0.0f, 1.0f}, 0.5f, 0.0f, 0);
    animator.Update(0.05f);
    ENG_CHECK_NEAR(animator.CurrentPose().translations[0].x, 5.0f, 1e-3f);

    // Вне порогов привязывается к ближайшему клипу.
    animator.PlayBlendSpace1D({a, b}, {0.0f, 1.0f}, -0.5f, 0.0f, 0);
    animator.Update(0.05f);
    ENG_CHECK_NEAR(animator.CurrentPose().translations[0].x, 0.0f, 1e-3f);
    animator.PlayBlendSpace1D({a, b}, {0.0f, 1.0f}, 4.0f, 0.0f, 0);
    animator.Update(0.05f);
    ENG_CHECK_NEAR(animator.CurrentPose().translations[0].x, 10.0f, 1e-3f);
}

ENG_TEST(Anim, LayerWeightZeroContributesNothing) {
    Skeleton skeleton = Skeleton::MakeChain(2);
    Animator animator;
    animator.SetSkeleton(&skeleton);
    const int clip = animator.AddClip(ConstantClip("w0", Vec3{7, 0, 0}));
    animator.Play(clip, 0.0f);
    animator.Update(0.05f);
    ENG_CHECK_NEAR(animator.CurrentPose().translations[0].x, 7.0f, 1e-3f);

    animator.SetLayerWeight(0, 0.0f);
    animator.Update(0.05f);
    // Возврат к bind-позе: сустав 0 в начале координат, сустав 1 в своём bind-смещении.
    ENG_CHECK_NEAR(animator.LayerWeight(0), 0.0f, 1e-6f);
    ENG_CHECK_NEAR(animator.CurrentPose().translations[0].x, 0.0f, 1e-3f);
    ENG_CHECK_NEAR(animator.CurrentPose().translations[1].x, 1.0f, 1e-3f);
}

ENG_TEST(Anim, PingPongReversesAtTheEnds) {
    Skeleton skeleton = Skeleton::MakeChain(1);
    Animator animator;
    animator.SetSkeleton(&skeleton);
    const int clip = animator.AddClip(RampClip("pp", 0.0f, 10.0f, 1.0f, true));
    animator.Play(clip, 0.0f, 0, true, AnimPlayMode::PingPong);
    animator.SetLayerSpeed(0, 1.0f);

    animator.Update(0.25f);
    const f32 t1 = animator.LayerTime(0);
    ENG_CHECK_NEAR(t1, 0.25f, 1e-3f);
    animator.Update(0.5f);
    const f32 t2 = animator.LayerTime(0);
    ENG_CHECK_NEAR(t2, 0.75f, 1e-3f);
    ENG_CHECK(t2 > t1);
    animator.Update(0.5f);  // отражение на конце
    const f32 t3 = animator.LayerTime(0);
    ENG_CHECK_NEAR(t3, 0.75f, 1e-3f);
    animator.Update(0.25f);  // теперь движение назад
    const f32 t4 = animator.LayerTime(0);
    ENG_CHECK_NEAR(t4, 0.5f, 1e-3f);
    ENG_CHECK(t4 < t3);
    ENG_CHECK_NEAR(animator.CurrentPose().translations[0].x, 5.0f, 1e-2f);
}

// ---------------------------------------------------------------------------
// Скелет + математика поз
// ---------------------------------------------------------------------------
ENG_TEST(Anim, SkeletonHelpers) {
    Skeleton chain = Skeleton::MakeChain(3, 2.0f);
    ENG_CHECK_EQ(chain.JointCount(), 3);
    ENG_CHECK_EQ(chain.MaxDepth(), 3);
    ENG_CHECK_EQ(chain.FindJoint("Chain1"), 1);
    ENG_CHECK_EQ(chain.FindJoint("Missing"), -1);

    // Обратная bind-матрица инвертирует накопленную bind-цепочку.
    Mat4 world = Mat4::Identity();
    for (int i = 0; i < chain.JointCount(); ++i) world = world * chain.JointAt(i).localBindPose;
    const Mat4 product = world * chain.JointAt(2).inverseBindMatrix;
    ENG_CHECK_NEAR(product.at(0, 0), 1.0f, 1e-4f);
    ENG_CHECK_NEAR(product.at(3, 0), 0.0f, 1e-4f);
    ENG_CHECK_NEAR(product.at(3, 1), 0.0f, 1e-4f);
    ENG_CHECK_NEAR(chain.JointAt(2).localBindPose.at(3, 0), 2.0f, 1e-4f);

    Skeleton humanoid = Skeleton::MakeHumanoid();
    ENG_CHECK(humanoid.JointCount() >= 15);
    ENG_CHECK(humanoid.MaxDepth() >= 4);
    ENG_CHECK(humanoid.FindJoint("Head") >= 0);
    ENG_CHECK(humanoid.FindJoint("LeftHand") >= 0);
    ENG_CHECK(humanoid.FindJoint("RightFoot") >= 0);
    ENG_CHECK(humanoid.JointAt(humanoid.FindJoint("Hips")).parent == -1);
}

ENG_TEST(Anim, ComputeWorldMatchesHandComputedChain) {
    Skeleton skeleton = Skeleton::MakeChain(2, 1.0f);
    Pose pose;
    pose.Resize(2);
    pose.translations[0] = Vec3{0, 0, 0};
    pose.rotations[0] = Quat::FromAxisAngle(Vec3{0, 0, 1}, kPi * 0.5f);
    pose.scales[0] = Vec3{1, 1, 1};
    pose.translations[1] = Vec3{1, 0, 0};
    pose.rotations[1] = Quat::Identity();
    pose.scales[1] = Vec3{1, 1, 1};
    SetLocalFromTrs(&pose, 0);
    SetLocalFromTrs(&pose, 1);
    pose.ComputeWorld(skeleton);

    // world[1] = Rz(90) * T(1,0,0) -> начало в (0,1,0).
    const Vec3 origin = pose.world[1].TransformPoint(Vec3{0, 0, 0});
    ENG_CHECK_NEAR(origin.x, 0.0f, 1e-4f);
    ENG_CHECK_NEAR(origin.y, 1.0f, 1e-4f);
    ENG_CHECK_NEAR(origin.z, 0.0f, 1e-4f);
    // Локальная трансляция ребёнка поворачивается в систему координат родителя.
    const Vec3 axis = pose.world[1].TransformDir(Vec3{0, 0, 1});
    ENG_CHECK_NEAR(axis.z, 1.0f, 1e-4f);

    // Авторинг только через TRS (locals оставлены единичными) тоже компонуется.
    Pose trsOnly;
    trsOnly.Resize(2);
    trsOnly.translations[1] = Vec3{2, 0, 0};
    trsOnly.ComputeWorld(skeleton);
    ENG_CHECK_NEAR(trsOnly.world[1].TransformPoint(Vec3{0, 0, 0}).x, 2.0f, 1e-4f);
}

ENG_TEST(Anim, ComputeWorldHandlesUnsortedJointStorage) {
    Skeleton skeleton;
    skeleton.AddJoint("A", -1);
    skeleton.AddJoint("B", 0);
    skeleton.AddJoint("C", 1);
    // Переворачиваем иерархию, чтобы дети шли перед родителями в порядке хранения.
    skeleton.JointAt(0).parent = 1;
    skeleton.JointAt(1).parent = 2;
    skeleton.JointAt(2).parent = -1;
    skeleton.JointAt(0).localTranslation = Vec3{0, 1, 0};
    skeleton.JointAt(1).localTranslation = Vec3{0, 1, 0};
    skeleton.JointAt(2).localTranslation = Vec3{0, 0, 0};
    skeleton.Finalise();

    Pose pose;
    pose.Resize(3);
    for (int i = 0; i < 3; ++i) {
        const usize ui = static_cast<usize>(i);
        pose.translations[ui] = skeleton.JointAt(i).localTranslation;
        SetLocalFromTrs(&pose, i);
    }
    pose.ComputeWorld(skeleton);
    ENG_CHECK_NEAR(pose.world[2].at(3, 1), 0.0f, 1e-4f);
    ENG_CHECK_NEAR(pose.world[1].at(3, 1), 1.0f, 1e-4f);
    ENG_CHECK_NEAR(pose.world[0].at(3, 1), 2.0f, 1e-4f);
}

ENG_TEST(Anim, MirrorPoseSwapsLeftRightAndFlipsX) {
    Skeleton skeleton = Skeleton::MakeHumanoid();
    const int left = skeleton.FindJoint("LeftUpperArm");
    const int right = skeleton.FindJoint("RightUpperArm");
    ENG_CHECK(left >= 0 && right >= 0);

    Pose pose;
    pose.Resize(skeleton.JointCount());
    pose.Reset();
    pose.translations[static_cast<usize>(left)] = Vec3{0.5f, 0.25f, 0};
    pose.rotations[static_cast<usize>(left)] = Quat::FromAxisAngle(Vec3{0, 1, 0}, 0.6f);
    SetLocalFromTrs(&pose, left);
    MirrorPose(&pose, skeleton);

    ENG_CHECK_NEAR(pose.translations[static_cast<usize>(right)].x, -0.5f, 1e-4f);
    ENG_CHECK_NEAR(pose.translations[static_cast<usize>(right)].y, 0.25f, 1e-4f);
    ENG_CHECK_NEAR(pose.rotations[static_cast<usize>(right)].y, -std::sin(0.3f), 1e-4f);
    ENG_CHECK_NEAR(pose.rotations[static_cast<usize>(right)].w, std::cos(0.3f), 1e-4f);
    // Исходный сустав после зеркалирования получает локаль парного сустава (единичную).
    ENG_CHECK_NEAR(pose.translations[static_cast<usize>(left)].x, 0.0f, 1e-4f);
}

ENG_TEST(Anim, IkTargetNudgesJointWorldPosition) {
    Skeleton skeleton = Skeleton::MakeChain(2, 1.0f);
    Animator animator;
    animator.SetSkeleton(&skeleton);
    animator.Update(0.0f);
    ENG_CHECK_NEAR(animator.JointWorld(1).at(3, 0), 1.0f, 1e-4f);

    animator.SetIkTarget(1, Vec3{3, 0.5f, 0}, 1.0f);
    animator.Update(0.0f);
    ENG_CHECK_NEAR(animator.JointWorld(1).at(3, 0), 3.0f, 1e-3f);
    ENG_CHECK_NEAR(animator.JointWorld(1).at(3, 1), 0.5f, 1e-3f);

    animator.ClearIkTargets();
    animator.SetIkTarget(1, Vec3{3, 0, 0}, 0.5f);
    animator.Update(0.0f);
    ENG_CHECK_NEAR(animator.JointWorld(1).at(3, 0), 2.0f, 1e-3f);
}

ENG_TEST(Anim, ClipBuildersAreSane) {
    Skeleton skeleton = Skeleton::MakeChain(6, 1.0f);
    const AnimationClip wave = AnimationClip::MakeWave(2.0f);
    ENG_CHECK_NEAR(wave.duration, 2.0f, 1e-5f);
    ENG_CHECK(wave.looping);
    ENG_CHECK(!wave.tracks.empty());
    std::vector<Vec3> waveT;
    wave.Sample(0.5f, nullptr, &waveT, nullptr, nullptr);
    ENG_CHECK_NEAR(waveT[0].y, 0.5f, 1e-3f);  // область пика sin(pi/4 * ...)

    const AnimationClip bounce = AnimationClip::MakeBounce(1.5f);
    ENG_CHECK(bounce.FindTrack(0) >= 0);
    const AnimationClip walk = AnimationClip::MakeWalk(6, 1.0f);
    ENG_CHECK_EQ(static_cast<int>(walk.tracks.size()), 6);
    const AnimationClip spin = AnimationClip::MakeSpin(1.0f);
    std::vector<Quat> spinR;
    spin.Sample(0.5f, nullptr, nullptr, &spinR, nullptr);
    const Vec3 spun = spinR[0] * Vec3{0, 0, 1};
    ENG_CHECK_NEAR(spun.z, -1.0f, 1e-3f);  // пол-оборота вокруг Y

    // Проигрывание билдера на реальном скелете удерживает нетрекаемые суставы в bind-позе.
    Animator animator;
    animator.SetSkeleton(&skeleton);
    const int index = animator.AddClip(walk);
    animator.Play(index, 0.0f);
    animator.Update(0.1f);
    ENG_CHECK_NEAR(animator.CurrentPose().translations[0].x, 0.0f, 1e-4f);
}

// ---------------------------------------------------------------------------
// Easing / пружина / твин
// ---------------------------------------------------------------------------
ENG_TEST(Anim, EaseEndpointsAreExact) {
    const int count = 28;  // все значения EaseType
    for (int i = 0; i < count; ++i) {
        const EaseType type = static_cast<EaseType>(i);
        ENG_CHECK_NEAR(ApplyEase(type, 0.0f), 0.0f, 1e-6f);
        ENG_CHECK_NEAR(ApplyEase(type, 1.0f), 1.0f, 1e-6f);
        ENG_CHECK_MSG(ApplyEase(type, 0.37f) == ApplyEase(type, 0.37f), "ease must not be NaN");
    }
    ENG_CHECK_NEAR(ApplyEase(EaseType::Linear, 0.25f), 0.25f, 1e-6f);
    ENG_CHECK_NEAR(ApplyEase(EaseType::InQuad, 0.5f), 0.25f, 1e-6f);
    ENG_CHECK_NEAR(ApplyEase(EaseType::OutQuad, 0.5f), 0.75f, 1e-6f);
    ENG_CHECK_NEAR(ApplyEase(EaseType::InOutQuad, 0.25f), 0.125f, 1e-6f);
    ENG_CHECK_NEAR(ApplyEase01(EaseType::InOutCubic, 0.5f), 0.5f, 1e-6f);
    ENG_CHECK(ApplyEase(EaseType::OutBack, 0.5f) > 0.5f);   // вылет за пределы (overshoot)
    ENG_CHECK(ApplyEase(EaseType::InBack, 0.5f) < 0.5f);
    ENG_CHECK(ApplyEase(EaseType::OutElastic, 0.1f) > 1.0f);
}

ENG_TEST(Anim, SpringConvergesToTarget) {
    Spring spring;
    spring.value = 0.0f;
    spring.velocity = 0.0f;
    spring.target = 10.0f;
    for (int i = 0; i < 600; ++i) spring.Update(1.0f / 60.0f);
    ENG_CHECK_NEAR(spring.value, 10.0f, 0.05f);

    // Большой переменный шаг кадра не должен расходиться (субшаги).
    Spring coarse;
    coarse.value = 0.0f;
    coarse.target = 5.0f;
    for (int i = 0; i < 120; ++i) coarse.Update(0.1f);
    ENG_CHECK_NEAR(coarse.value, 5.0f, 0.05f);
    ENG_CHECK(std::fabs(coarse.velocity) < 1.0f);

    Spring snap;
    snap.Snap(3.0f);
    ENG_CHECK_NEAR(snap.value, 3.0f, 1e-6f);
}

ENG_TEST(Anim, TweenReachesEndAndFinishes) {
    Tween tween;
    tween.To(0.0f, 100.0f, 1.0f, EaseType::OutQuad, 0.2f);
    ENG_CHECK(!tween.Finished());
    tween.Update(0.1f);
    ENG_CHECK_NEAR(tween.Value(), 0.0f, 1e-6f);  // всё ещё в задержке
    tween.Update(0.5f);
    ENG_CHECK(!tween.Finished());
    ENG_CHECK(tween.Value() > 0.0f && tween.Value() < 100.0f);
    tween.Update(1.0f);
    ENG_CHECK(tween.Finished());
    ENG_CHECK_NEAR(tween.Value(), 100.0f, 1e-4f);

    tween.Restart();
    ENG_CHECK(!tween.Finished());
    ENG_CHECK_NEAR(tween.Value(), 0.0f, 1e-6f);

    tween.SetSpeed(4.0f);
    tween.Update(0.2f);
    ENG_CHECK(!tween.Finished());
    tween.Update(0.2f);
    ENG_CHECK(tween.Finished());
    ENG_CHECK_NEAR(tween.Value(), 100.0f, 1e-4f);

    // Твины нулевой длительности завершаются сразу.
    Tween instant;
    instant.To(5.0f, 7.0f, 0.0f, EaseType::Linear);
    instant.Update(0.016f);
    ENG_CHECK(instant.Finished());
    ENG_CHECK_NEAR(instant.Value(), 7.0f, 1e-6f);
}

ENG_TEST(Anim, AnimatorClipLookupAndLayers) {
    Skeleton skeleton = Skeleton::MakeChain(2);
    Animator animator;
    animator.SetSkeleton(&skeleton);
    const int a = animator.AddClip(ConstantClip("walk", Vec3{1, 0, 0}));
    const int b = animator.AddClip(ConstantClip("run", Vec3{2, 0, 0}));
    ENG_CHECK_EQ(a, 0);
    ENG_CHECK_EQ(b, 1);
    ENG_CHECK_EQ(animator.ClipCount(), 2);
    ENG_CHECK_EQ(animator.FindClip("run"), 1);
    ENG_CHECK_EQ(animator.FindClip("fly"), -1);

    animator.EnsureLayerCount(3);
    ENG_CHECK_EQ(animator.LayerCount(), 3);
    animator.SetLayerSpeed(1, 2.0f);
    animator.Play(a, 0.0f, 0, true, AnimPlayMode::Loop);
    animator.SetLayerTime(0, 0.5f);
    ENG_CHECK_NEAR(animator.LayerTime(0), 0.5f, 1e-4f);
    ENG_CHECK_NEAR(animator.LayerNormalizedTime(0), 0.5f, 1e-4f);

    animator.Play(b, 0.0f, 1);
    animator.Update(0.25f);
    ENG_CHECK_NEAR(animator.LayerTime(1), 0.5f, 1e-3f);  // скорость 2
    animator.Pause(true);
    animator.Update(0.25f);
    ENG_CHECK_NEAR(animator.LayerTime(1), 0.5f, 1e-3f);  // на паузе
    animator.Pause(false);

    // Плавное затухание через Stop.
    animator.Stop(1, 0.2f);
    animator.Update(0.1f);
    ENG_CHECK(animator.LayerWeight(1) >= 0.0f);
    animator.Update(0.2f);
    ENG_CHECK(!animator.Layers()[1].enabled);
    animator.Reset();
    ENG_CHECK(!animator.LayerFinished(1));
}
