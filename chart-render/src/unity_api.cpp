#include <wds/chart_render/unity_api.hpp>

#include <algorithm>
#include <cmath>

namespace wds::chart_render::unity {
namespace {
float clamp01(float x) noexcept { return std::max(0.0f, std::min(1.0f, x)); }
float lerp(float a, float b, float t) noexcept { return a + (b - a) * t; }
}

namespace UnityEngine {

float AnimationCurve::Evaluate(float time) const noexcept {
  if (keys.empty()) return 0.0f;
  if (keys.size() == 1) return keys.front().value;
  if (time <= keys.front().time) return keys.front().value;
  for (std::size_t i = 1; i < keys.size(); ++i) {
    if (time <= keys[i].time) {
      const Keyframe& a = keys[i - 1]; const Keyframe& b = keys[i];
      const float dt = b.time - a.time;
      if (dt <= 0.0f) return b.value;
      if (!std::isfinite(a.outTangent) || !std::isfinite(b.inTangent)) return a.value;
      const float t = (time - a.time) / dt;
      const float t2 = t * t;
      const float t3 = t2 * t;
      const float h00 = 2.0f * t3 - 3.0f * t2 + 1.0f;
      const float h10 = t3 - 2.0f * t2 + t;
      const float h01 = -2.0f * t3 + 3.0f * t2;
      const float h11 = t3 - t2;
      return h00 * a.value + h10 * a.outTangent * dt + h01 * b.value + h11 * b.inTangent * dt;
    }
  }
  return keys.back().value;
}

float MinMaxCurve::Evaluate(float time, float random01) const noexcept {
  random01 = clamp01(random01);
  switch (mode) {
    case ParticleSystemCurveMode::Constant: return constant;
    case ParticleSystemCurveMode::Curve: return curve.Evaluate(time) * curveMultiplier;
    case ParticleSystemCurveMode::TwoCurves: return lerp(curveMin.Evaluate(time), curve.Evaluate(time), random01) * curveMultiplier;
    case ParticleSystemCurveMode::TwoConstants: return lerp(constantMin, constant, random01);
  }
  return constant;
}

Color Gradient::Evaluate(float time) const noexcept {
  Color out{};
  if (!colorKeys.empty()) {
    if (time <= colorKeys.front().time) out = colorKeys.front().color;
    else if (time >= colorKeys.back().time) out = colorKeys.back().color;
    else for (std::size_t i = 1; i < colorKeys.size(); ++i) if (time <= colorKeys[i].time) {
      const auto& a = colorKeys[i - 1]; const auto& b = colorKeys[i]; const float d = b.time - a.time; const float t = d <= 0.0f ? 1.0f : (time - a.time) / d;
      out.r = lerp(a.color.r, b.color.r, t); out.g = lerp(a.color.g, b.color.g, t); out.b = lerp(a.color.b, b.color.b, t); out.a = lerp(a.color.a, b.color.a, t); break;
    }
  }
  if (!alphaKeys.empty()) {
    if (time <= alphaKeys.front().time) out.a = alphaKeys.front().alpha;
    else if (time >= alphaKeys.back().time) out.a = alphaKeys.back().alpha;
    else for (std::size_t i = 1; i < alphaKeys.size(); ++i) if (time <= alphaKeys[i].time) {
      const auto& a = alphaKeys[i - 1]; const auto& b = alphaKeys[i]; const float d = b.time - a.time; out.a = lerp(a.alpha, b.alpha, d <= 0.0f ? 1.0f : (time - a.time) / d); break;
    }
  }
  return out;
}

Color MinMaxGradient::Evaluate(float time, float random01) const noexcept {
  random01 = clamp01(random01);
  switch (mode) {
    case ParticleSystemGradientMode::Color: return color;
    case ParticleSystemGradientMode::Gradient: return gradient.Evaluate(time);
    case ParticleSystemGradientMode::TwoColors:
      return {lerp(colorMin.r, color.r, random01), lerp(colorMin.g, color.g, random01),
              lerp(colorMin.b, color.b, random01), lerp(colorMin.a, color.a, random01)};
    case ParticleSystemGradientMode::TwoGradients: {
      const Color a = gradientMin.Evaluate(time), b = gradient.Evaluate(time);
      return {lerp(a.r, b.r, random01), lerp(a.g, b.g, random01), lerp(a.b, b.b, random01), lerp(a.a, b.a, random01)};
    }
    case ParticleSystemGradientMode::RandomColor: return gradient.Evaluate(random01);
  }
  return color;
}

Vector3 Transform::TransformPoint(const Vector3& point) const noexcept {
  Vector3 p{point.x * localScale.x + localPosition.x, point.y * localScale.y + localPosition.y, point.z * localScale.z + localPosition.z};
  return parent ? parent->TransformPoint(p) : Vector3{p.x + position.x, p.y + position.y, p.z + position.z};
}
Vector3 Transform::InverseTransformPoint(const Vector3& point) const noexcept {
  const Vector3 p = parent ? parent->InverseTransformPoint(point) : Vector3{point.x - position.x, point.y - position.y, point.z - position.z};
  return {(p.x - localPosition.x) / (localScale.x == 0.0f ? 1.0f : localScale.x), (p.y - localPosition.y) / (localScale.y == 0.0f ? 1.0f : localScale.y), (p.z - localPosition.z) / (localScale.z == 0.0f ? 1.0f : localScale.z)};
}
Vector3 Transform::TransformDirection(const Vector3& direction) const noexcept {
  Vector3 d{direction.x * localScale.x, direction.y * localScale.y, direction.z * localScale.z}; return parent ? parent->TransformDirection(d) : d;
}
Vector3 Transform::InverseTransformDirection(const Vector3& direction) const noexcept {
  const Vector3 d = parent ? parent->InverseTransformDirection(direction) : direction;
  return {d.x / (localScale.x == 0.0f ? 1.0f : localScale.x), d.y / (localScale.y == 0.0f ? 1.0f : localScale.y), d.z / (localScale.z == 0.0f ? 1.0f : localScale.z)};
}
void Transform::SetParent(Transform* value, bool worldPositionStays) noexcept {
  const Vector3 world = worldPositionStays ? TransformPoint({0,0,0}) : Vector3{};
  if (parent) parent->children.erase(std::remove(parent->children.begin(), parent->children.end(), this), parent->children.end());
  parent = value;
  if (parent) { parent->children.push_back(this); if (worldPositionStays) localPosition = parent->InverseTransformPoint(world); }
}
GameObject::GameObject(std::string n) : name(std::move(n)) { transform.gameObject = this; }
void Animator::Update(float deltaTime) noexcept { (void)deltaTime; }
void Animator::Play(const std::string& stateName, int layer, float normalizedTime) noexcept { (void)stateName; (void)layer; (void)normalizedTime; }
float Mathf::Repeat(float t, float length) noexcept { if (length <= 0.0f) return 0.0f; return t - std::floor(t / length) * length; }
int Mathf::RoundToInt(float value) noexcept { return static_cast<int>(std::lround(value)); }
int Random::Range(int minInclusive, int maxExclusive) noexcept {
  if (maxExclusive <= minInclusive) return minInclusive;
  static std::uint32_t state = 1u;
  state = state * 1664525u + 1013904223u;
  const auto span = static_cast<std::uint32_t>(maxExclusive - minInclusive);
  return minInclusive + static_cast<int>(state % span);
}
float Random::Range(float minInclusive, float maxInclusive) noexcept {
  if (maxInclusive <= minInclusive) return minInclusive;
  static std::uint32_t state = 2463534242u;
  state = state * 1664525u + 1013904223u;
  const float t = static_cast<float>(state & 0x00FFFFFFu) / static_cast<float>(0x00FFFFFFu);
  return minInclusive + (maxInclusive - minInclusive) * t;
}

} // namespace UnityEngine

void EmissionModule::SetBursts(const Burst* values, int size) { bursts.clear(); if (values && size > 0) bursts.assign(values, values + size); }
void EmissionModule::GetBursts(Burst* values, int size) const { if (!values || size <= 0) return; const int n = std::min(size, burstCount()); for (int i=0;i<n;++i) values[i] = bursts[static_cast<std::size_t>(i)]; }
void SubEmittersModule::AddSubEmitter(const SubEmitter& value) { emitters.push_back(value); }
void SubEmittersModule::RemoveSubEmitter(int index) { if (index >= 0 && index < subEmittersCount()) emitters.erase(emitters.begin() + index); }
void CustomDataModule::SetVector(ParticleSystemCustomData stream, int component, const MinMaxCurve& curve) noexcept {
  MinMaxCurve* dst = nullptr;
  if (stream == ParticleSystemCustomData::Custom1) dst = component == 0 ? &custom1X : (component == 1 ? &custom1Y : nullptr);
  else dst = component == 0 ? &custom2X : (component == 1 ? &custom2Y : nullptr);
  if (dst) *dst = curve;
}
MinMaxCurve CustomDataModule::GetVector(ParticleSystemCustomData stream, int component) const noexcept {
  if (stream == ParticleSystemCustomData::Custom1) return component == 0 ? custom1X : custom1Y;
  return component == 0 ? custom2X : custom2Y;
}
void CustomDataModule::SetColor(ParticleSystemCustomData stream, const MinMaxGradient& gradient) noexcept { if (stream == ParticleSystemCustomData::Custom1) custom1Color = gradient; else custom2Color = gradient; }
MinMaxGradient CustomDataModule::GetColor(ParticleSystemCustomData stream) const noexcept { return stream == ParticleSystemCustomData::Custom1 ? custom1Color : custom2Color; }

namespace {
constexpr float kEmitU = 1.0f;

// live dump force-drag-2020-class-v1 (same hop as split_effect_limit_drag_on_total).
// Not 2022.3.
void iso_drag_scale(float k, float dt, float& vx, float& vy, float& vz) noexcept {
  if (k <= 1e-8f || dt <= 1e-8f) return;
  const float s = std::sqrt(vx * vx + vy * vy + vz * vz);
  if (s <= 1e-12f) {
    vx = 0.0f;
    vy = 0.0f;
    vz = 0.0f;
    return;
  }
  const float new_s = 1.0f / (1.0f / s + k * dt);
  const float m = new_s / s;
  vx *= m;
  vy *= m;
  vz *= m;
}

void limit_drag_on_total(float k, float dt, bool mul_vel, float& vx, float& vy, float& vz) noexcept {
  if (mul_vel) {
    iso_drag_scale(k, dt, vx, vy, vz);
    return;
  }
  if (k <= 1e-8f || dt <= 1e-8f) return;
  const float s = std::sqrt(vx * vx + vy * vy + vz * vz);
  if (s <= 1e-12f) {
    vx = 0.0f;
    vy = 0.0f;
    vz = 0.0f;
    return;
  }
  const float dec = k * dt;
  if (s <= dec) {
    vx = 0.0f;
    vy = 0.0f;
    vz = 0.0f;
    return;
  }
  const float m = (s - dec) / s;
  vx *= m;
  vy *= m;
  vz *= m;
}

Vector3 emitter_world(const ParticleSystem& ps) noexcept {
  const Transform* xf = ps.transform();
  if (xf == nullptr) return {};
  return xf->TransformPoint({0.0f, 0.0f, 0.0f});
}

int burst_count_of(const Burst& burst) noexcept {
  const float n = burst.count.Evaluate(0.0f, kEmitU);
  if (n <= 0.0f) return 0;
  return static_cast<int>(std::lround(n));
}
}  // namespace

ParticleSystem::ParticleSystem() : owner_(std::make_shared<GameObject>("ParticleSystem")) {}
ParticleSystem::ParticleSystem(std::shared_ptr<GameObject> owner) : owner_(std::move(owner)) { if (!owner_) owner_ = std::make_shared<GameObject>("ParticleSystem"); }

void ParticleSystem::reset_emit_state() noexcept {
  emit_accum_ = 0.0f;
  rod_accum_ = 0.0f;
  last_world_valid_ = false;
  bursts_consumed_ = false;
}

void ParticleSystem::Play(bool withChildren) noexcept {
  // Unity 2022.3 ParticleSystem.Play: Pause+Play resumes; Stop+Play starts
  // from the beginning. withChildren plays hierarchical / listed children.
  // Isolation dump uses Play(false) when SubEmitters is on so Death/Birth
  // children are not started independently.
  // https://docs.unity3d.com/2022.3/Documentation/ScriptReference/ParticleSystem.Play.html
  if (paused_) {
    paused_ = false;
    playing_ = true;
  } else if (!playing_) {
    playing_ = true;
    time_ = 0.0f;
    reset_emit_state();
  }
  if (!withChildren) return;
  for (auto& slot : subemitters_.emitters) {
    if (slot.subEmitter != nullptr && slot.subEmitter != this) slot.subEmitter->Play(true);
  }
}

void ParticleSystem::Pause(bool withChildren) noexcept {
  paused_ = true;
  if (!withChildren) return;
  for (auto& slot : subemitters_.emitters) {
    if (slot.subEmitter != nullptr && slot.subEmitter != this) slot.subEmitter->Pause(true);
  }
}

void ParticleSystem::Stop(bool withChildren, ParticleSystemStopBehavior stopBehavior) noexcept {
  playing_ = false;
  paused_ = false;
  if (stopBehavior == ParticleSystemStopBehavior::StopEmittingAndClear) Clear(withChildren);
  else if (withChildren) {
    for (auto& slot : subemitters_.emitters) {
      if (slot.subEmitter != nullptr && slot.subEmitter != this)
        slot.subEmitter->Stop(true, stopBehavior);
    }
  }
}

void ParticleSystem::Clear(bool withChildren) noexcept {
  particles_.clear();
  reset_emit_state();
  if (!withChildren) return;
  for (auto& slot : subemitters_.emitters) {
    if (slot.subEmitter != nullptr && slot.subEmitter != this) slot.subEmitter->Clear(true);
  }
}

void ParticleSystem::emit_new_particles(int count, const Vector3& at, bool trigger_birth) {
  if (count <= 0) return;
  const float duration = main_.duration > 1e-8f ? main_.duration : 1.0f;
  const float u_sys = main_.loop ? std::fmod(time_, duration) / duration : clamp01(time_ / duration);
  for (int i = 0; i < count; ++i) {
    if (main_.maxParticles > 0 && particleCount() >= main_.maxParticles) break;
    Particle p;
    p.startLifetime = main_.startLifetime.Evaluate(u_sys, kEmitU);
    if (p.startLifetime <= 1e-8f) p.startLifetime = 0.01f;
    p.remainingLifetime = p.startLifetime;
    p.startSize = main_.startSize.Evaluate(u_sys, kEmitU);
    if (main_.startSize3D) {
      p.startSize3D = {main_.startSizeX.Evaluate(u_sys, kEmitU), main_.startSizeY.Evaluate(u_sys, kEmitU),
                       main_.startSizeZ.Evaluate(u_sys, kEmitU)};
      if (p.startSize <= 1e-8f) p.startSize = p.startSize3D.x;
    } else {
      p.startSize3D = {p.startSize, p.startSize, p.startSize};
    }
    p.startColor = main_.startColor.Evaluate(u_sys, static_cast<float>((randomSeed_ + i) & 255u) / 255.0f);
    p.randomSeed = randomSeed_ + static_cast<std::uint32_t>(particles_.size());
    p.position = at;
    const float speed = main_.startSpeed.Evaluate(u_sys, kEmitU);
    // Dump-path emit: origin + startSpeed along local +Z. Shape scatter is
    // unproven (no measure on the Shape page for this stub); do not sample Sphere.
    Vector3 fwd{0.0f, 0.0f, 1.0f};
    if (const Transform* xf = transform()) fwd = xf->TransformDirection({0.0f, 0.0f, 1.0f});
    const float fl = std::sqrt(fwd.x * fwd.x + fwd.y * fwd.y + fwd.z * fwd.z);
    if (fl > 1e-8f) {
      fwd.x /= fl;
      fwd.y /= fl;
      fwd.z /= fl;
    } else {
      fwd = {0.0f, 0.0f, 1.0f};
    }
    p.velocity = {fwd.x * speed, fwd.y * speed, fwd.z * speed};
    p.totalVelocity = p.velocity;
    particles_.push_back(p);
    if (trigger_birth) fire_sub_emitters(ParticleSystemSubEmitterType::Birth, particles_.back());
  }
}

void ParticleSystem::fire_sub_emitters(ParticleSystemSubEmitterType type, Particle& particle) {
  if (!subemitters_.enabled) return;
  for (int i = 0; i < subemitters_.subEmittersCount(); ++i) {
    auto& slot = subemitters_.emitters[static_cast<std::size_t>(i)];
    if (slot.type != type || slot.subEmitter == nullptr || slot.subEmitter == this) continue;
    TriggerSubEmitter(i, particle);
  }
}

void ParticleSystem::emit_from_modules(float dt, float t0) {
  if (!playing_ || paused_ || !emission_.enabled || dt <= 0.0f) return;
  const float duration = main_.duration > 1e-8f ? main_.duration : 1.0f;
  const float u_sys = main_.loop ? std::fmod(time_, duration) / duration : clamp01(time_ / duration);
  const Vector3 at = emitter_world(*this);
  const float rot = emission_.rateOverTime.Evaluate(u_sys, kEmitU);
  if (rot > 0.0f) {
    emit_accum_ += rot * dt;
    const int n = static_cast<int>(std::floor(emit_accum_));
    if (n > 0) {
      emit_accum_ -= static_cast<float>(n);
      emit_new_particles(n, at, true);
    }
  }
  const float rod = emission_.rateOverDistance.Evaluate(u_sys, kEmitU);
  if (rod > 0.0f) {
    if (last_world_valid_) {
      const float dx = at.x - last_world_pos_.x;
      const float dy = at.y - last_world_pos_.y;
      const float dz = at.z - last_world_pos_.z;
      // live dump v1 RoD: seeds ≈ RoD × Σ|Δps.worldPosition|. Not 2022.3.
      rod_accum_ += rod * std::sqrt(dx * dx + dy * dy + dz * dz);
      const int n = static_cast<int>(std::floor(rod_accum_));
      if (n > 0) {
        rod_accum_ -= static_cast<float>(n);
        emit_new_particles(n, at, true);
      }
    }
    last_world_pos_ = at;
    last_world_valid_ = true;
  } else {
    last_world_pos_ = at;
    last_world_valid_ = true;
  }
  if (!bursts_consumed_) {
    const float t1 = time_;
    for (const auto& burst : emission_.bursts) {
      if (t0 <= burst.time && burst.time < t1) emit_new_particles(burst_count_of(burst), at, true);
    }
  }
}

void ParticleSystem::integrate_particles(float dt) {
  if (dt <= 0.0f) return;
  for (Particle& p : particles_) {
    const float life = p.startLifetime > 1e-8f ? p.startLifetime : 1.0f;
    const float age = std::max(0.0f, life - p.remainingLifetime);
    const float u = clamp01(age / life);
    if (force_.enabled) {
      // Unity 2022.3 Force over Lifetime: v += F·dt. Official field, not a hop.
      // https://docs.unity3d.com/2022.3/Documentation/Manual/PartSysForceOverLifeModule.html
      p.velocity.x += force_.x.Evaluate(u, kEmitU) * dt;
      p.velocity.y += force_.y.Evaluate(u, kEmitU) * dt;
      p.velocity.z += force_.z.Evaluate(u, kEmitU) * dt;
    }
    const float g_mod = main_.gravityModifier.Evaluate(u, kEmitU);
    // Unity Physics.gravity.y default −9.81 × gravityModifier. World Y.
    // Same constant as kUnityPhysicsGravityY (official_playfield.hpp). Official.
    constexpr float kPhysicsGravityY = -9.81f;
    p.velocity.y += kPhysicsGravityY * g_mod * dt;
    if (velocity_.enabled) {
      p.animatedVelocity.x = velocity_.x.Evaluate(u, kEmitU);
      p.animatedVelocity.y = velocity_.y.Evaluate(u, kEmitU);
      p.animatedVelocity.z = velocity_.z.Evaluate(u, kEmitU);
    } else {
      p.animatedVelocity = {};
    }
    p.totalVelocity.x = p.velocity.x + p.animatedVelocity.x;
    p.totalVelocity.y = p.velocity.y + p.animatedVelocity.y;
    p.totalVelocity.z = p.velocity.z + p.animatedVelocity.z;
    if (limit_.enabled) {
      // live dump force-drag-2020-class-v1. Drag total, then stored = total − anim.
      float k = limit_.drag.Evaluate(u, kEmitU);
      if (limit_.multiplyBySize) k *= p.startSize;
      limit_drag_on_total(k, dt, limit_.multiplyByVelocity, p.totalVelocity.x, p.totalVelocity.y,
                          p.totalVelocity.z);
      p.velocity.x = p.totalVelocity.x - p.animatedVelocity.x;
      p.velocity.y = p.totalVelocity.y - p.animatedVelocity.y;
      p.velocity.z = p.totalVelocity.z - p.animatedVelocity.z;
    }
    float sm = 1.0f;
    if (velocity_.enabled) sm = velocity_.speedModifier.Evaluate(u, kEmitU);
    // live dump v1 speedModifier: Δp ≈ total·dt·volSpeedMod. Not 2022.3.
    p.position.x += p.totalVelocity.x * dt * sm;
    p.position.y += p.totalVelocity.y * dt * sm;
    p.position.z += p.totalVelocity.z * dt * sm;
    p.remainingLifetime -= dt;
  }
  for (Particle& p : particles_) {
    if (p.remainingLifetime > 0.0f) continue;
    fire_sub_emitters(ParticleSystemSubEmitterType::Death, p);
  }
  particles_.erase(std::remove_if(particles_.begin(), particles_.end(),
                                  [](const Particle& p) { return p.remainingLifetime <= 0.0f; }),
                   particles_.end());
}

void ParticleSystem::Simulate(float t, bool withChildren, bool restart, bool fixedTimeStep) noexcept {
  (void)fixedTimeStep;
  if (restart) {
    time_ = 0.0f;
    particles_.clear();
    reset_emit_state();
  }
  const float dt = std::max(0.0f, t);
  if (paused_) {
    if (withChildren) {
      for (auto& slot : subemitters_.emitters) {
        if (slot.subEmitter != nullptr && slot.subEmitter != this)
          slot.subEmitter->Simulate(dt, true, false, fixedTimeStep);
      }
    }
    return;
  }
  const float t0 = time_;
  time_ += dt;
  emit_from_modules(dt, t0);
  integrate_particles(dt);
  if (!withChildren) return;
  for (auto& slot : subemitters_.emitters) {
    if (slot.subEmitter != nullptr && slot.subEmitter != this)
      slot.subEmitter->Simulate(dt, true, false, fixedTimeStep);
  }
}

void ParticleSystem::Emit(int count) {
  const float duration = main_.duration > 1e-8f ? main_.duration : 1.0f;
  const float u_sys = main_.loop ? std::fmod(time_, duration) / duration : clamp01(time_ / duration);
  for (int i=0; i<std::max(0,count); ++i) {
    Particle p;
    p.startLifetime = main_.startLifetime.Evaluate(time_);
    p.remainingLifetime = p.startLifetime;
    p.startSize = main_.startSize.Evaluate(time_);
    p.startColor = main_.startColor.Evaluate(u_sys, static_cast<float>((randomSeed_ + i) & 255u) / 255.0f);
    p.randomSeed = randomSeed_ + static_cast<std::uint32_t>(particles_.size());
    particles_.push_back(p);
  }
}
void ParticleSystem::Emit(const Particle& particle) { particles_.push_back(particle); }
void ParticleSystem::Emit(const Particle* particles, int count) { if (particles && count > 0) particles_.insert(particles_.end(), particles, particles + count); }
void ParticleSystem::Emit(const EmitParams& params, int count) { for (int i = 0; i < std::max(0, count); ++i) { Particle p = params.particle; if (!params.startLifetimeSet) { p.startLifetime = main_.startLifetime.Evaluate(time_); p.remainingLifetime = p.startLifetime; } if (!params.startSizeSet) p.startSize = main_.startSize.Evaluate(time_); particles_.push_back(p); } }
void ParticleSystem::Emit(const Vector3& position, const Vector3& velocity, float size, float lifetime, const Color& color) { Particle p; p.position = position; p.velocity = velocity; p.startSize = size; p.startLifetime = lifetime; p.remainingLifetime = lifetime; p.startColor = color; particles_.push_back(p); }

void ParticleSystem::TriggerSubEmitter(int subEmitterIndex) {
  if (subEmitterIndex < 0 || subEmitterIndex >= subemitters_.subEmittersCount()) return;
  for (Particle& p : particles_) TriggerSubEmitter(subEmitterIndex, p);
}

void ParticleSystem::TriggerSubEmitter(int subEmitterIndex, Particle& particle) {
  if (subEmitterIndex < 0 || subEmitterIndex >= subemitters_.subEmittersCount()) return;
  auto& slot = subemitters_.emitters[static_cast<std::size_t>(subEmitterIndex)];
  ParticleSystem* child = slot.subEmitter;
  if (child == nullptr || child == this) return;
  if (!child->playing_) child->Play(false);
  int n = 0;
  if (child->emission_.enabled) {
    for (const auto& burst : child->emission_.bursts) n += burst_count_of(burst);
  }
  child->bursts_consumed_ = true;
  if (n <= 0) return;
  child->emit_new_particles(n, particle.position, false);
}
int ParticleSystem::GetParticles(Particle* particles, int size, int offset) const {
  if (!particles || size <= 0 || offset < 0 || offset >= particleCount()) return 0;
  const int n = std::min(size, particleCount() - offset);
  std::copy_n(particles_.begin() + offset, static_cast<std::size_t>(n), particles);
  return n;
}

void ParticleSystemRenderer::SetActiveVertexStreams(const ParticleSystemVertexStream* streams, int count) {
  activeVertexStreams.clear();
  if (streams && count > 0) activeVertexStreams.assign(streams, streams + count);
}
void ParticleSystemRenderer::SetActiveVertexStreams(const std::vector<ParticleSystemVertexStream>& streams) {
  activeVertexStreams = streams;
}
int ParticleSystem::SetParticles(const Particle* particles, int size, int offset) { if (!particles || size <= 0 || offset < 0) return particleCount(); if (offset > particleCount()) particles_.resize(static_cast<std::size_t>(offset)); if (offset + size > particleCount()) particles_.resize(static_cast<std::size_t>(offset + size)); std::copy_n(particles, size, particles_.begin() + offset); return particleCount(); }
bool ParticleSystem::IsAlive(bool withChildren) const noexcept { (void)withChildren; return playing_ || !particles_.empty(); }

void configure_from_catalog(ParticleSystem& system, const CatalogParticleView& view) noexcept {
  system.main() = view.main; system.emission() = view.emission; system.shape() = view.shape; system.velocityOverLifetime() = view.velocity; system.forceOverLifetime() = view.force; system.limitVelocityOverLifetime() = view.limit; system.noise() = view.noise; system.trails() = view.trails; system.renderer() = view.renderer;
}

} // namespace wds::chart_render::unity
