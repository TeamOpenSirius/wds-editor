#include <wds/chart_render/unity_api.hpp>

#include <cassert>
#include <cmath>
#include <memory>

using namespace wds::chart_render::unity;
using namespace wds::chart_render::unity::UnityEngine;

int main() {
  static_assert(static_cast<int>(ParticleSystemSimulationSpace::Custom) == 2);
  static_assert(static_cast<int>(ParticleSystemRenderMode::Mesh) == 4);
  static_assert(static_cast<int>(ParticleSystemCurveMode::TwoCurves) == 2);
  static_assert(static_cast<int>(ParticleSystemShapeType::Circle) == 10);
  static_assert(static_cast<int>(ParticleSystemShapeType::SingleSidedEdge) == 12);
  static_assert(static_cast<int>(ParticleSystemShapeType::BoxEdge) == 16);
  static_assert(static_cast<int>(ParticleSystemGradientMode::Color) == 0);
  static_assert(static_cast<int>(ParticleSystemGradientMode::Gradient) == 1);
  static_assert(static_cast<int>(ParticleSystemGradientMode::TwoColors) == 2);
  static_assert(static_cast<int>(ParticleSystemGradientMode::TwoGradients) == 3);
  static_assert(static_cast<int>(ParticleSystemGradientMode::RandomColor) == 4);
  static_assert(static_cast<int>(ParticleSystemAnimationMode::Grid) == 0);
  static_assert(static_cast<int>(ParticleSystemAnimationMode::Sprites) == 1);
  static_assert(static_cast<int>(ParticleSystemAnimationType::WholeSheet) == 0);
  static_assert(static_cast<int>(ParticleSystemAnimationType::SingleRow) == 1);
  static_assert(static_cast<int>(ParticleSystemVertexStream::Position) == 0);
  static_assert(static_cast<int>(ParticleSystemVertexStream::MeshIndex) == 45);

  AnimationCurve curve({{0.0f, 2.0f, 0.0f, 0.0f}, {1.0f, 4.0f, 0.0f, 0.0f}});
  assert(std::fabs(curve.Evaluate(0.5f) - 3.0f) < 1e-5f);
  assert(std::fabs(curve.Evaluate(0.25f) - 2.3125f) < 1e-5f);
  MinMaxCurve mm; mm.mode = ParticleSystemCurveMode::TwoConstants; mm.constantMin = 2.0f; mm.constant = 6.0f;
  assert(std::fabs(mm.Evaluate(0.0f, 0.25f) - 3.0f) < 1e-5f);
  mm.constantMax() = 8.0f;
  assert(std::fabs(mm.constant - 8.0f) < 1e-5f && std::fabs(mm.constantMax() - 8.0f) < 1e-5f);
  mm.curveMax().AddKey({0.0f, 1.0f, 0.0f, 0.0f});
  assert(mm.curve.length() == 1 && mm.curveMax().length() == 1);

  bool saw_nonzero = false;
  for (int i = 0; i < 32; ++i) {
    if (Random::Range(0, 1000) != 0) saw_nonzero = true;
  }
  assert(saw_nonzero);
  assert(Random::Range(3, 3) == 3);
  assert(std::fabs(Random::Range(2.0f, 2.0f) - 2.0f) < 1e-5f);
  const float fr = Random::Range(0.0f, 10.0f);
  assert(fr >= 0.0f && fr <= 10.0f);

  ParticleSystem ps;
  ps.main().startLifetime.constant = 2.0f;
  ps.main().startSize.constant = 0.5f;
  ps.main().startColor.color = {0.2f, 0.4f, 0.6f, 1.0f};
  ps.main().maxParticles = 8;
  assert(ps.main().maxParticles == 8);
  ps.trails().ratio = 0.25f;
  assert(std::fabs(ps.trails().ratio - 0.25f) < 1e-5f);
  ps.Play(); ps.Emit(3);
  assert(ps.particleCount() == 3 && ps.IsAlive());
  Particle out[3]{}; assert(ps.GetParticles(out, 3) == 3); assert(std::fabs(out[0].startSize - 0.5f) < 1e-5f);
  Particle offset_out[2]{}; assert(ps.GetParticles(offset_out, 2, 1) == 2);
  ps.Simulate(1.0f, true, false, true); assert(ps.particleCount() == 3);
  ParticleSystem rate_ps;
  rate_ps.main().startLifetime.constant = 5.0f;
  rate_ps.main().maxParticles = 32;
  rate_ps.emission().enabled = true;
  rate_ps.emission().rateOverTime.constant = 10.0f;
  rate_ps.Play(false);
  rate_ps.Simulate(1.0f, false, false, true);
  assert(rate_ps.particleCount() == 10);
  ParticleSystem parent;
  parent.main().startLifetime.constant = 0.5f;
  parent.main().maxParticles = 32;
  parent.emission().enabled = true;
  parent.emission().rateOverTime.constant = 10.0f;
  ParticleSystem child;
  child.main().startLifetime.constant = 2.0f;
  child.main().loop = false;
  child.main().playOnAwake = false;
  child.main().maxParticles = 32;
  child.emission().enabled = true;
  child.emission().rateOverTime.constant = 0.0f;
  Burst death_burst;
  death_burst.time = 0.0f;
  death_burst.count.constant = 1.0f;
  child.emission().SetBursts(&death_burst, 1);
  parent.subEmitters().enabled = true;
  parent.subEmitters().AddSubEmitter(
      {&child, ParticleSystemSubEmitterType::Death,
       ParticleSystemSubEmitterProperties::None, 1.0f});
  parent.Play(false);
  parent.Simulate(1.0f, true, false, true);
  assert(parent.particleCount() > 0);
  assert(child.particleCount() > 0);
  // Parent Stop(true) must not be required for Death; Stop(false) keeps
  // child Emission.enabled so TriggerSubEmitter still seeds.
  parent.Stop(false, ParticleSystemStopBehavior::StopEmittingAndClear);
  child.Stop(false, ParticleSystemStopBehavior::StopEmittingAndClear);
  child.emission().enabled = true;
  child.emission().SetBursts(&death_burst, 1);
  parent.Play(false);
  parent.Simulate(1.0f, true, false, true);
  assert(parent.particleCount() > 0);
  assert(child.particleCount() > 0);
  ps.Stop();
  assert(ps.particleCount() == 3 && ps.IsAlive());
  ps.Stop(true, ParticleSystemStopBehavior::StopEmittingAndClear);
  assert(ps.particleCount() == 0 && !ps.IsAlive());
  ps.set_useAutoRandomSeed(false); ps.set_randomSeed(77u); assert(ps.get_randomSeed() == 77u && !ps.get_useAutoRandomSeed());
  EmitParams params; params.particle.position = {1.0f, 2.0f, 3.0f}; params.startLifetimeSet = true; params.particle.startLifetime = 1.0f; params.particle.remainingLifetime = 1.0f; ps.Emit(params, 2); assert(ps.particleCount() == 2);
  ps.TriggerSubEmitter(0); ps.TriggerSubEmitter(0, out[0]);
  ps.customData().SetVector(ParticleSystemCustomData::Custom1, 0, mm); assert(ps.customData().GetVector(ParticleSystemCustomData::Custom1, 0).constantMin == 2.0f);
  SplitEffectResourceProvenance provenance; provenance.effectId = 101; provenance.bundleSha256 = "sha256"; provenance.objects.push_back({42, "ParticleSystem", "raw"}); ps.set_resource_provenance(provenance); assert(ps.resource_provenance().effectId == 101);

  Particle sized;
  sized.startSize = 0.5f;
  sized.startColor = {0.2f, 0.4f, 0.6f, 1.0f};
  assert(std::fabs(sized.GetCurrentSize() - 0.5f) < 1e-5f);
  assert(std::fabs(sized.GetCurrentColor().r - 0.2f) < 1e-5f);
  const Vector3 size3 = sized.GetCurrentSize3D();
  assert(std::fabs(size3.x - 1.0f) < 1e-5f);

  ps.textureSheetAnimation().mode = ParticleSystemAnimationMode::Sprites;
  ps.textureSheetAnimation().animation = ParticleSystemAnimationType::WholeSheet;
  auto sheet_sprite = std::make_shared<Sprite>();
  ps.textureSheetAnimation().AddSprite(sheet_sprite);
  assert(ps.textureSheetAnimation().spriteCount() == 1);
  assert(ps.textureSheetAnimation().GetSprite(0) == sheet_sprite);

  ps.particleSystemRenderer().maxParticleSize = 0.25f;
  assert(std::fabs(ps.particleSystemRenderer().maxParticleSize - 0.25f) < 1e-5f);
  const ParticleSystemVertexStream streams[] = {ParticleSystemVertexStream::Position, ParticleSystemVertexStream::Color};
  ps.particleSystemRenderer().SetActiveVertexStreams(streams, 2);
  assert(ps.particleSystemRenderer().activeVertexStreams.size() == 2);

  EmissionModule emission; Burst burst; burst.time = 0.25f; burst.cycleCount = 2; emission.SetBursts(&burst, 1); assert(emission.burstCount() == 1); Burst read{}; emission.GetBursts(&read, 1); assert(read.cycleCount == 2);
  SubEmittersModule sub; sub.AddSubEmitter({&ps, ParticleSystemSubEmitterType::Birth, ParticleSystemSubEmitterProperties::InheritColor, 1.0f}); assert(sub.subEmittersCount() == 1); sub.RemoveSubEmitter(0); assert(sub.subEmittersCount() == 0);

  GameObject go("api"); go.transform.localPosition = {1.0f, 2.0f, 3.0f}; assert(std::fabs(go.transform.TransformPoint({0,0,0}).x - 1.0f) < 1e-5f);
  Animator animator; animator.Play("SplitEffect_fadeIn_anim", 0, 0.0f); SpriteRenderer line; line.color = {1, 0.5f, 0.25f, 1}; line.enabled = true;
  assert(Mathf::Clamp(12, 0, 6) == 6 && Mathf::RoundToInt(1.6f) == 2 && Mathf::Repeat(9.0f, 8.0f) == 1.0f && Mathf::Max(1, 2) == 2);
  Time::time = 2.0f; assert(Time::time == 2.0f);
  auto original = std::make_shared<GameObject>("element"); auto clone = Instantiate(*original, &go.transform); assert(clone && clone->transform.parent == &go.transform && go.transform.childCount() == 1);
  auto templated = Instantiate(original, &go.transform);
  assert(templated && templated->transform.parent == &go.transform && go.transform.childCount() == 2);

  auto parent_go = std::make_shared<GameObject>("parent");
  auto host = std::make_shared<GameObject>("element-host");
  auto element = std::make_shared<SplitEffectElement>();
  element->gameObject = host.get();
  element->transform = &host->transform;
  auto cloned_element = Instantiate(element, &parent_go->transform);
  assert(cloned_element && cloned_element->transform && cloned_element->transform->parent == &parent_go->transform);
  assert(parent_go->transform.childCount() == 1);

  auto tree_parent = std::make_shared<GameObject>("tree-parent");
  auto tree_child = std::make_shared<GameObject>("tree-child");
  tree_child->transform.localPosition = {1.5f, -2.25f, 0.5f};
  tree_child->transform.SetParent(&tree_parent->transform, false);
  tree_parent->children_owned.push_back(tree_child);
  auto tree_clone = Instantiate(tree_parent);
  assert(tree_clone && tree_clone->transform.childCount() == 1);
  Transform* cloned_child = tree_clone->transform.GetChild(0);
  assert(cloned_child && cloned_child->gameObject);
  assert(cloned_child->gameObject != tree_parent.get());
  assert(cloned_child->gameObject != tree_child.get());
  assert(std::fabs(cloned_child->localPosition.x - 1.5f) < 1e-5f);
  assert(std::fabs(cloned_child->localPosition.y - (-2.25f)) < 1e-5f);
  assert(std::fabs(cloned_child->localPosition.z - 0.5f) < 1e-5f);
  return 0;
}
