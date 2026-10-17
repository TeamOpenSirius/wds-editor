#pragma once

// A small, typed C++ contract for the Unity 2022.3 APIs consumed by the
// offline split-effect controller.  This is deliberately a contract layer:
// open chart logic may call it without depending on Unity or a CLR.  Methods
// whose closed Unity implementation is unavailable are deterministic stubs;
// their status is exposed by ParticleSystem::backend_status().

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace wds::chart_render::unity {

class ParticleSystem;
class ParticleSystemRenderer;

namespace UnityEngine {

class GameObject;
class Transform;

struct Vector2 { float x = 0.0f, y = 0.0f; };
struct Vector3 {
  float x = 0.0f, y = 0.0f, z = 0.0f;
  constexpr Vector3() = default;
  constexpr Vector3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}
};
struct Vector4 { float x = 0.0f, y = 0.0f, z = 0.0f, w = 0.0f; };
struct Quaternion { float x = 0.0f, y = 0.0f, z = 0.0f, w = 1.0f; };
struct Color { float r = 1.0f, g = 1.0f, b = 1.0f, a = 1.0f; };
using Color32 = Color;
struct Bounds { Vector3 center{}; Vector3 size{}; };

enum class ParticleSystemSimulationSpace : std::uint8_t { Local = 0, World = 1, Custom = 2 };
enum class ParticleSystemScalingMode : std::uint8_t { Hierarchy = 0, Local = 1, Shape = 2 };
enum class ParticleSystemRenderSpace : std::uint8_t { View = 0, World = 1, Local = 2, Facing = 3, Velocity = 4 };
enum class ParticleSystemRenderMode : std::uint8_t { Billboard = 0, Stretch = 1, HorizontalBillboard = 2, VerticalBillboard = 3, Mesh = 4, None = 5 };
// Unity 2022.3 ParticleSystemShapeType ordinals (hole at 11). Not the Unity 5 sequential table.
enum class ParticleSystemShapeType : std::uint8_t {
  Sphere = 0, SphereShell = 1, Hemisphere = 2, HemisphereShell = 3, Cone = 4, Box = 5, Mesh = 6,
  ConeVolume = 7, ConeVolumeShell = 8, CircleDeprecated = 9, Circle = 10,
  SingleSidedEdge = 12, MeshRenderer = 13, SkinnedMeshRenderer = 14, BoxShell = 15,
  BoxEdge = 16, Donut = 17, Rectangle = 18, Sprite = 19, SpriteRenderer = 20
};
enum class ParticleSystemCurveMode : std::uint8_t { Constant = 0, Curve = 1, TwoCurves = 2, TwoConstants = 3 };
// ParticleSystem.MinMaxGradient.mode — not UnityEngine.GradientMode (Blend/Fixed).
enum class ParticleSystemGradientMode : std::uint8_t { Color = 0, Gradient = 1, TwoColors = 2, TwoGradients = 3, RandomColor = 4 };
enum class ParticleSystemStopAction : std::uint8_t { None = 0, Disable = 1, Destroy = 2, Callback = 3 };
enum class ParticleSystemSubEmitterType : std::uint8_t { Birth = 0, Collision = 1, Death = 2, Trigger = 3, Manual = 4 };
enum class ParticleSystemSubEmitterProperties : std::uint8_t { None = 0, InheritColor = 1, InheritSize = 2, InheritRotation = 4, InheritLifetime = 8, InheritDuration = 16 };
enum class ParticleSystemTrailMode : std::uint8_t { Particle = 0, Ribbon = 1 };
enum class ParticleSystemTrailTextureMode : std::uint8_t { Stretch = 0, Tile = 1, DistributePerSegment = 2, RepeatPerSegment = 3 };
enum class ParticleSystemNoiseQuality : std::uint8_t { Low = 0, Medium = 1, High = 2 };
enum class ParticleSystemAnimationMode : std::uint8_t { Grid = 0, Sprites = 1 };
enum class ParticleSystemAnimationType : std::uint8_t { WholeSheet = 0, SingleRow = 1 };
enum class ParticleSystemAnimationRowMode : std::uint8_t { Custom = 0, Random = 1 };
enum class ParticleSystemAnimationTimeMode : std::uint8_t { Lifetime = 0, Speed = 1 };
enum class ParticleSystemEmitterVelocityMode : std::uint8_t { Transform = 0, Rigidbody = 1, Custom = 2 };
enum class ParticleSystemShapeMultiModeValue : std::uint8_t { Random = 0, Loop = 1, PingPong = 2, BurstSpread = 3 };
enum class ParticleSystemCustomData : std::uint8_t { Custom1 = 0, Custom2 = 1 };
// Ordinals follow Unity 2022.3 Scripting API declaration order
// (https://docs.unity3d.com/2022.3/Documentation/ScriptReference/ParticleSystemVertexStream.html).
enum class ParticleSystemVertexStream : std::uint8_t {
  Position = 0,
  Normal = 1,
  Tangent = 2,
  Color = 3,
  UV = 4,
  UV2 = 5,
  UV3 = 6,
  UV4 = 7,
  AnimBlend = 8,
  AnimFrame = 9,
  Center = 10,
  VertexID = 11,
  SizeX = 12,
  SizeXY = 13,
  SizeXYZ = 14,
  Rotation = 15,
  Rotation3D = 16,
  RotationSpeed = 17,
  RotationSpeed3D = 18,
  Velocity = 19,
  Speed = 20,
  AgePercent = 21,
  InvStartLifetime = 22,
  StableRandomX = 23,
  StableRandomXY = 24,
  StableRandomXYZ = 25,
  StableRandomXYZW = 26,
  VaryingRandomX = 27,
  VaryingRandomXY = 28,
  VaryingRandomXYZ = 29,
  VaryingRandomXYZW = 30,
  Custom1X = 31,
  Custom1XY = 32,
  Custom1XYZ = 33,
  Custom1XYZW = 34,
  Custom2X = 35,
  Custom2XY = 36,
  Custom2XYZ = 37,
  Custom2XYZW = 38,
  NoiseSumX = 39,
  NoiseSumXY = 40,
  NoiseSumXYZ = 41,
  NoiseImpulseX = 42,
  NoiseImpulseXY = 43,
  NoiseImpulseXYZ = 44,
  MeshIndex = 45,
  ParticleIndex = 46,
  ColorPackedAsTwoFloats = 47,
  MeshAxisOfRotation = 48,
  NextTrailCenter = 49,
  PreviousTrailCenter = 50,
  PercentageAlongTrail = 51,
  TrailWidth = 52
};

struct Keyframe { float time = 0.0f; float value = 0.0f; float inTangent = 0.0f; float outTangent = 0.0f; };
struct AnimationCurve {
  std::vector<Keyframe> keys;
  AnimationCurve() = default;
  explicit AnimationCurve(std::vector<Keyframe> k) : keys(std::move(k)) {}
  float Evaluate(float time) const noexcept;
  int length() const noexcept { return static_cast<int>(keys.size()); }
  void AddKey(const Keyframe& key) { keys.push_back(key); }
};
struct MinMaxCurve {
  ParticleSystemCurveMode mode = ParticleSystemCurveMode::Constant;
  float constant = 0.0f;
  float constantMin = 0.0f;
  AnimationCurve curve{};
  AnimationCurve curveMin{};
  float curveMultiplier = 1.0f;
  float Evaluate(float time, float random01 = 0.0f) const noexcept;
  float& constantMax() noexcept { return constant; }
  const float& constantMax() const noexcept { return constant; }
  AnimationCurve& curveMax() noexcept { return curve; }
  const AnimationCurve& curveMax() const noexcept { return curve; }
};
struct GradientColorKey { Color color{}; float time = 0.0f; };
struct GradientAlphaKey { float alpha = 1.0f; float time = 0.0f; };
struct Gradient {
  std::vector<GradientColorKey> colorKeys;
  std::vector<GradientAlphaKey> alphaKeys;
  Color Evaluate(float time) const noexcept;
};
struct MinMaxGradient {
  ParticleSystemGradientMode mode = ParticleSystemGradientMode::Color;
  Color color{};
  Color colorMin{};
  Gradient gradient{};
  Gradient gradientMin{};
  Color Evaluate(float time, float random01 = 0.0f) const noexcept;
};

class Texture { public: virtual ~Texture() = default; int width = 0; int height = 0; };
class Texture2D : public Texture { public: Texture2D() = default; Texture2D(int w, int h) { width = w; height = h; } };
class Mesh { public: virtual ~Mesh() = default; std::vector<Vector3> vertices; std::vector<Vector2> uv; std::vector<std::uint32_t> triangles; };
class Shader;
class Material { public: virtual ~Material() = default; std::string name; std::shared_ptr<Texture> mainTexture; std::shared_ptr<Shader> shader; };
class Sprite { public: virtual ~Sprite() = default; std::shared_ptr<Texture> texture; Vector4 textureRect{}; };
class Shader { public: virtual ~Shader() = default; std::string name; static std::shared_ptr<Shader> Find(const std::string& shaderName) { auto result = std::make_shared<Shader>(); result->name = shaderName; return result; } };
class MeshFilter { public: virtual ~MeshFilter() = default; Mesh* sharedMesh = nullptr; Transform* transform = nullptr; };
class MeshRenderer { public: virtual ~MeshRenderer() = default; bool enabled = true; Material* sharedMaterial = nullptr; std::vector<Material*> materials; Transform* transform = nullptr; };
class SortingGroup { public: virtual ~SortingGroup() = default; int sortingOrder = 0; int sortingLayerID = 0; };
class AnimationClip { public: virtual ~AnimationClip() = default; std::string name; float length = 0.0f; bool legacy = false; };
class AnimatorController { public: virtual ~AnimatorController() = default; std::vector<std::shared_ptr<AnimationClip>> clips; };
class AssetBundle { public: virtual ~AssetBundle() = default; std::string name; };

class Transform {
 public:
  GameObject* gameObject = nullptr;
  Vector3 position{}; Vector3 localPosition{}; Vector3 localScale{1.0f, 1.0f, 1.0f}; Quaternion rotation{}; Quaternion localRotation{};
  Transform* parent = nullptr;
  std::vector<Transform*> children;
  int childCount() const noexcept { return static_cast<int>(children.size()); }
  Transform* GetChild(int index) const noexcept {
    if (index < 0 || children.empty() || static_cast<std::size_t>(index) >= children.size()) {
      return nullptr;
    }
    return children.data()[static_cast<std::size_t>(index)];
  }
  Vector3 TransformPoint(const Vector3& point) const noexcept;
  Vector3 InverseTransformPoint(const Vector3& point) const noexcept;
  Vector3 TransformDirection(const Vector3& direction) const noexcept;
  Vector3 InverseTransformDirection(const Vector3& direction) const noexcept;
  void SetParent(Transform* value, bool worldPositionStays = true) noexcept;
};
class GameObject {
 public:
  explicit GameObject(std::string n = {}); std::string name; bool activeSelf = true; Transform transform{};
  // Instantiate keeps child instances alive; Transform* children is not ownership.
  std::vector<std::shared_ptr<GameObject>> children_owned;
  // Authored Unity pathId of the source GameObject this instance was materialized from.
  std::int64_t openwds_path_id = 0;
  bool activeInHierarchy() const noexcept { return activeSelf; }
  void SetActive(bool value) noexcept { activeSelf = value; }
  MeshRenderer* meshRenderer = nullptr;
  ParticleSystemRenderer* particleSystemRenderer = nullptr;
};
inline std::shared_ptr<GameObject> Instantiate(const GameObject& original, Transform* parent = nullptr) {
  auto copy = std::make_shared<GameObject>(original.name);
  copy->activeSelf = original.activeSelf;
  copy->openwds_path_id = original.openwds_path_id;
  copy->transform.localPosition = original.transform.localPosition;
  copy->transform.localRotation = original.transform.localRotation;
  copy->transform.localScale = original.transform.localScale;
  copy->transform.gameObject = copy.get();
  // meshRenderer / particleSystemRenderer are not cloned here; leave them null
  // rather than sharing the original's component objects.
  const std::vector<Transform*> original_children = original.transform.children;
  for (Transform* child : original_children) {
    if (child == nullptr || child->gameObject == nullptr) continue;
    auto child_copy = Instantiate(*child->gameObject, &copy->transform);
    if (child_copy) copy->children_owned.push_back(std::move(child_copy));
  }
  if (parent) copy->transform.SetParent(parent, false);
  return copy;
}
inline std::shared_ptr<GameObject> Instantiate(const std::shared_ptr<GameObject>& original, Transform* parent = nullptr) {
  if (!original) return {};
  return Instantiate(*original, parent);
}
class Animator { public: virtual ~Animator() = default; bool enabled = true; float speed = 1.0f; Transform transform{}; void Update(float deltaTime) noexcept; void Play(const std::string& stateName, int layer = 0, float normalizedTime = 0.0f) noexcept; };
class SpriteRenderer { public: virtual ~SpriteRenderer() = default; bool enabled = true; Color color{}; Transform transform{}; GameObject* gameObject = nullptr; };
class MonoBehaviour { public: virtual ~MonoBehaviour() = default; GameObject* gameObject = nullptr; Transform* transform = nullptr; };
class SplitEffectElement : public MonoBehaviour {
 public:
  Color LineColor{};
  std::vector<std::shared_ptr<ParticleSystem>> LineEffects;
};
struct Mathf {
  static float Clamp(float value, float min, float max) noexcept { return value < min ? min : (value > max ? max : value); }
  static int Clamp(int value, int min, int max) noexcept { return value < min ? min : (value > max ? max : value); }
  static float Repeat(float t, float length) noexcept;
  static int RoundToInt(float value) noexcept;
  static float Max(float a, float b) noexcept { return a > b ? a : b; }
  static int Max(int a, int b) noexcept { return a > b ? a : b; }
};
struct Random { static int Range(int minInclusive, int maxExclusive) noexcept; static float Range(float minInclusive, float maxInclusive) noexcept; };
struct Time { inline static float time = 0.0f; inline static float deltaTime = 0.0f; };

template <typename T>
std::shared_ptr<T> Instantiate(const std::shared_ptr<T>& original, Transform* parent = nullptr) {
  if (!original) return {};
  if constexpr (std::is_same_v<T, GameObject>) {
    return Instantiate(*original, parent);
  } else {
    auto copy = std::make_shared<T>(*original);
    if constexpr (std::is_base_of_v<MonoBehaviour, T>) {
      if (parent) {
        if (copy->transform) copy->transform->SetParent(parent, false);
        else if (copy->gameObject) copy->gameObject->transform.SetParent(parent, false);
      }
    }
    return copy;
  }
}

} // namespace UnityEngine

using namespace UnityEngine;

struct Particle {
  Vector3 position{}; Vector3 velocity{}; Vector3 animatedVelocity{}; Vector3 totalVelocity{};
  Vector3 axisOfRotation{}; Vector3 rotation3D{}; Color startColor{};
  float startSize = 1.0f; Vector3 startSize3D{1.0f, 1.0f, 1.0f};
  float rotation = 0.0f; float angularVelocity = 0.0f; Vector3 angularVelocity3D{};
  float startLifetime = 5.0f; float remainingLifetime = 5.0f; std::uint32_t randomSeed = 0;
  std::uint32_t meshIndex = 0; float lifetime { 5.0f };
  float GetCurrentSize() const noexcept { return startSize; }
  Vector3 GetCurrentSize3D() const noexcept {
    return startSize3D.x == 0.0f && startSize3D.y == 0.0f && startSize3D.z == 0.0f
               ? Vector3{startSize, startSize, startSize}
               : startSize3D;
  }
  Color GetCurrentColor() const noexcept { return startColor; }
};
struct EmitParams { Particle particle{}; bool positionSet = false; bool velocitySet = false; bool startSizeSet = false; bool startColorSet = false; bool randomSeedSet = false; bool startLifetimeSet = false; bool meshIndexSet = false; bool applyShapeToPosition = false; };

struct ResourceObjectProvenance {
  std::int64_t pathId = 0;
  std::string serializedType;
  std::string rawDataSha256;
};
struct SplitEffectResourceProvenance {
  int effectId = 0;
  std::string assetVersion = "1.96.0";
  std::string bundleSha256;
  std::vector<ResourceObjectProvenance> objects;
};

struct MainModule {
  bool enabled = true; float duration = 5.0f; bool loop = true; bool playOnAwake = true;
  MinMaxCurve startDelay{}; MinMaxCurve startLifetime{ParticleSystemCurveMode::Constant, 5.0f};
  MinMaxCurve startSpeed{ParticleSystemCurveMode::Constant, 5.0f}; MinMaxGradient startColor{};
  MinMaxCurve startSize{ParticleSystemCurveMode::Constant, 1.0f}; MinMaxCurve startSizeX{ParticleSystemCurveMode::Constant, 1.0f};
  MinMaxCurve startSizeY{ParticleSystemCurveMode::Constant, 1.0f}; MinMaxCurve startSizeZ{ParticleSystemCurveMode::Constant, 1.0f};
  bool startSize3D = false; MinMaxCurve startRotation{}; MinMaxCurve startRotationX{}; MinMaxCurve startRotationY{}; MinMaxCurve startRotationZ{}; bool startRotation3D = false;
  MinMaxCurve gravityModifier{}; ParticleSystemSimulationSpace simulationSpace = ParticleSystemSimulationSpace::Local;
  Transform* customSimulationSpace = nullptr; ParticleSystemScalingMode scalingMode = ParticleSystemScalingMode::Hierarchy;
  float simulationSpeed = 1.0f; ParticleSystemStopAction stopAction = ParticleSystemStopAction::None; bool ringBufferMode = false;
  float ringBufferLoopRangeMin = 0.0f; float ringBufferLoopRangeMax = 1.0f; bool useUnscaledTime = false;
  int maxParticles = 1000;
  ParticleSystemEmitterVelocityMode emitterVelocityMode = ParticleSystemEmitterVelocityMode::Transform;
};

struct EmissionModule {
  bool enabled = true; MinMaxCurve rateOverTime{}; MinMaxCurve rateOverDistance{}; std::vector<struct Burst> bursts;
  int burstCount() const noexcept { return static_cast<int>(bursts.size()); }
  void SetBursts(const Burst* values, int size); void GetBursts(Burst* values, int size) const;
};
struct Burst { float time = 0.0f; MinMaxCurve count{}; int cycleCount = 1; float repeatInterval = 0.01f; float probability = 1.0f; };
struct ShapeModule {
  bool enabled = true; ParticleSystemShapeType shapeType = ParticleSystemShapeType::Cone; float angle = 25.0f; float radius = 1.0f; float radiusThickness = 1.0f; float length = 5.0f; float arc = 360.0f; float arcSpread = 0.0f; float arcSpeed = 0.0f;
  ParticleSystemShapeMultiModeValue arcMode = ParticleSystemShapeMultiModeValue::Random; Vector3 position{}; Vector3 rotation{}; Vector3 scale{1.0f, 1.0f, 1.0f}; float randomDirectionAmount = 0.0f; float sphericalDirectionAmount = 0.0f; float randomPositionAmount = 0.0f; bool alignToDirection = false;
  Mesh* mesh = nullptr; Sprite* sprite = nullptr; void* meshRenderer = nullptr;
};
struct VelocityOverLifetimeModule { bool enabled = false; ParticleSystemSimulationSpace space = ParticleSystemSimulationSpace::Local; MinMaxCurve x{}, y{}, z{}, orbitalX{}, orbitalY{}, orbitalZ{}, orbitalOffsetX{}, orbitalOffsetY{}, orbitalOffsetZ{}, radial{}, speedModifier{ParticleSystemCurveMode::Constant, 1.0f}; };
struct ForceOverLifetimeModule { bool enabled = false; ParticleSystemSimulationSpace space = ParticleSystemSimulationSpace::Local; MinMaxCurve x{}, y{}, z{}, xMultiplier{}, yMultiplier{}, zMultiplier{}; bool randomized = false; };
struct LimitVelocityOverLifetimeModule { bool enabled = false; ParticleSystemSimulationSpace space = ParticleSystemSimulationSpace::Local; MinMaxCurve limit{}, dampen{}; MinMaxCurve limitX{}, limitY{}, limitZ{}, drag{}; bool separateAxes = false; bool multiplyBySize = false; bool multiplyByVelocity = false; };
struct ColorOverLifetimeModule { bool enabled = false; MinMaxGradient color{}; };
struct ColorBySpeedModule { bool enabled = false; MinMaxGradient color{}; MinMaxCurve range{}; };
struct SizeOverLifetimeModule { bool enabled = false; MinMaxCurve size{}, x{}, y{}, z{}; bool separateAxes = false; };
struct SizeBySpeedModule { bool enabled = false; MinMaxCurve size{}, x{}, y{}, z{}, range{}; bool separateAxes = false; };
struct RotationOverLifetimeModule { bool enabled = false; MinMaxCurve x{}, y{}, z{}; bool separateAxes = false; };
struct RotationBySpeedModule { bool enabled = false; MinMaxCurve x{}, y{}, z{}, range{}; bool separateAxes = false; };
struct NoiseModule {
  bool enabled = false; ParticleSystemNoiseQuality quality = ParticleSystemNoiseQuality::Low;
  MinMaxCurve strength{}, frequency{ParticleSystemCurveMode::Constant, 0.5f}, scrollSpeed{};
  bool damping = false; bool separateAxes = false;
  MinMaxCurve strengthX{}, strengthY{}, strengthZ{};
  int octaveCount = 1; float octaveScale = 0.5f; float octaveMultiplier = 0.5f;
  MinMaxCurve positionAmount{}, rotationAmount{}, sizeAmount{}, remap{}, remapY{}, remapZ{};
  bool remapEnabled = false;
};
struct InheritVelocityModule { bool enabled = false; int mode = 0; MinMaxCurve curve{}; };
struct LightsModule { bool enabled = false; bool ratio = false; float ratioValue = 1.0f; void* light = nullptr; };
struct TextureSheetAnimationModule {
  bool enabled = false;
  ParticleSystemAnimationMode mode = ParticleSystemAnimationMode::Grid;
  ParticleSystemAnimationType animation = ParticleSystemAnimationType::WholeSheet;
  ParticleSystemAnimationTimeMode timeMode = ParticleSystemAnimationTimeMode::Lifetime;
  int numTilesX = 1; int numTilesY = 1;
  MinMaxCurve frameOverTime{};
  MinMaxCurve startFrame{};
  int cycleCount = 1;
  float fps = 0.0f;
  int rowIndex = 0;
  bool useRandomRow = false;
  ParticleSystemAnimationRowMode rowMode = ParticleSystemAnimationRowMode::Custom;
  int uvChannelMask = -1;
  std::vector<std::shared_ptr<Sprite>> sprites_;
  void AddSprite(std::shared_ptr<Sprite> sprite) { sprites_.push_back(std::move(sprite)); }
  void SetSprite(int index, std::shared_ptr<Sprite> sprite) {
    if (index < 0) return;
    if (index >= static_cast<int>(sprites_.size())) sprites_.resize(static_cast<std::size_t>(index) + 1);
    sprites_[static_cast<std::size_t>(index)] = std::move(sprite);
  }
  std::shared_ptr<Sprite> GetSprite(int index) const {
    if (index < 0 || index >= static_cast<int>(sprites_.size())) return {};
    return sprites_[static_cast<std::size_t>(index)];
  }
  int spriteCount() const { return static_cast<int>(sprites_.size()); }
};
struct TrailModule { bool enabled = false; ParticleSystemTrailMode mode = ParticleSystemTrailMode::Particle; float ratio = 1.0f; MinMaxCurve lifetime{}, widthOverTrail{}; bool dieWithParticles = true; bool sizeAffectsWidth = false; bool inheritParticleColor = true; ParticleSystemTrailTextureMode textureMode = ParticleSystemTrailTextureMode::Stretch; float minVertexDistance = 0.1f; MinMaxGradient colorOverLifetime{}; MinMaxGradient colorOverTrail{}; };
struct RendererModule { bool enabled = true; ParticleSystemRenderMode renderMode = ParticleSystemRenderMode::Billboard; ParticleSystemRenderSpace alignment = ParticleSystemRenderSpace::View; float lengthScale = 2.0f; float velocityScale = 0.0f; float cameraVelocityScale = 0.0f; float maxParticleSize = 0.5f; int sortingOrder = 0; Mesh* mesh = nullptr; Material* material = nullptr; std::vector<Material*> materials; bool freeformStretching = false; bool rotateWithStretchDirection = false; Vector3 flip{}; };
struct SubEmitter { class ParticleSystem* subEmitter = nullptr; ParticleSystemSubEmitterType type = ParticleSystemSubEmitterType::Birth; ParticleSystemSubEmitterProperties properties = ParticleSystemSubEmitterProperties::None; float probability = 1.0f; };
struct SubEmittersModule { bool enabled = false; std::vector<SubEmitter> emitters; int subEmittersCount() const noexcept { return static_cast<int>(emitters.size()); } void AddSubEmitter(const SubEmitter& value); void RemoveSubEmitter(int index); };
struct LifetimeByEmitterSpeedModule { bool enabled = false; MinMaxCurve curve{}; };
struct ExternalForcesModule { bool enabled = false; MinMaxCurve multiplier{}; float influenceFilter = 1.0f; };
struct CollisionModule { bool enabled = false; ParticleSystemSimulationSpace mode = ParticleSystemSimulationSpace::World; MinMaxCurve dampen{}, bounce{}, lifetimeLoss{}; };
struct TriggerModule { bool enabled = false; int inside = 0; int outside = 0; int enter = 0; int exit = 0; };
struct CustomDataModule { bool enabled = false; MinMaxCurve custom1X{}, custom1Y{}, custom2X{}, custom2Y{}; MinMaxGradient custom1Color{}, custom2Color{}; void SetVector(ParticleSystemCustomData stream, int component, const MinMaxCurve& curve) noexcept; MinMaxCurve GetVector(ParticleSystemCustomData stream, int component) const noexcept; void SetColor(ParticleSystemCustomData stream, const MinMaxGradient& gradient) noexcept; MinMaxGradient GetColor(ParticleSystemCustomData stream) const noexcept; };

enum class BackendStatus : std::uint8_t { OpenDeterministicStub = 0, UnityClosedBackend = 1 };

enum class ParticleSystemStopBehavior : std::uint8_t { StopEmittingAndClear = 0, StopEmitting = 1 };

class ParticleSystemRenderer {
 public:
  bool enabled = true;
  ParticleSystemRenderMode renderMode = ParticleSystemRenderMode::Billboard;
  ParticleSystemRenderSpace alignment = ParticleSystemRenderSpace::View;
  float lengthScale = 2.0f;
  float velocityScale = 0.0f;
  float cameraVelocityScale = 0.0f;
  float maxParticleSize = 0.5f;
  float minParticleSize = 0.0f;
  Vector3 pivot{};
  Vector3 flip{};
  int sortingOrder = 0;
  Mesh* mesh = nullptr;
  Material* material = nullptr;
  std::vector<Material*> materials;
  bool freeformStretching = false;
  bool rotateWithStretchDirection = false;
  std::vector<ParticleSystemVertexStream> activeVertexStreams;
  void SetActiveVertexStreams(const ParticleSystemVertexStream* streams, int count);
  void SetActiveVertexStreams(const std::vector<ParticleSystemVertexStream>& streams);
};

class ParticleSystem {
 public:
  ParticleSystem(); explicit ParticleSystem(std::shared_ptr<GameObject> owner);
  MainModule& main() noexcept { return main_; } const MainModule& main() const noexcept { return main_; }
  EmissionModule& emission() noexcept { return emission_; }
  const EmissionModule& emission() const noexcept { return emission_; }
  ShapeModule& shape() noexcept { return shape_; }
  const ShapeModule& shape() const noexcept { return shape_; }
  VelocityOverLifetimeModule& velocityOverLifetime() noexcept { return velocity_; }
  const VelocityOverLifetimeModule& velocityOverLifetime() const noexcept { return velocity_; }
  ForceOverLifetimeModule& forceOverLifetime() noexcept { return force_; }
  const ForceOverLifetimeModule& forceOverLifetime() const noexcept { return force_; }
  LimitVelocityOverLifetimeModule& limitVelocityOverLifetime() noexcept { return limit_; }
  const LimitVelocityOverLifetimeModule& limitVelocityOverLifetime() const noexcept { return limit_; }
  ColorOverLifetimeModule& colorOverLifetime() noexcept { return color_; }
  const ColorOverLifetimeModule& colorOverLifetime() const noexcept { return color_; }
  ColorBySpeedModule& colorBySpeed() noexcept { return colorSpeed_; }
  const ColorBySpeedModule& colorBySpeed() const noexcept { return colorSpeed_; }
  SizeOverLifetimeModule& sizeOverLifetime() noexcept { return size_; }
  const SizeOverLifetimeModule& sizeOverLifetime() const noexcept { return size_; }
  SizeBySpeedModule& sizeBySpeed() noexcept { return sizeSpeed_; }
  const SizeBySpeedModule& sizeBySpeed() const noexcept { return sizeSpeed_; }
  RotationOverLifetimeModule& rotationOverLifetime() noexcept { return rotation_; }
  const RotationOverLifetimeModule& rotationOverLifetime() const noexcept { return rotation_; }
  RotationBySpeedModule& rotationBySpeed() noexcept { return rotationSpeed_; }
  const RotationBySpeedModule& rotationBySpeed() const noexcept { return rotationSpeed_; }
  NoiseModule& noise() noexcept { return noise_; }
  const NoiseModule& noise() const noexcept { return noise_; }
  InheritVelocityModule& inheritVelocity() noexcept { return inherit_; }
  const InheritVelocityModule& inheritVelocity() const noexcept { return inherit_; }
  LightsModule& lights() noexcept { return lights_; }
  const LightsModule& lights() const noexcept { return lights_; }
  TextureSheetAnimationModule& textureSheetAnimation() noexcept { return texture_; }
  const TextureSheetAnimationModule& textureSheetAnimation() const noexcept { return texture_; }
  TrailModule& trails() noexcept { return trails_; }
  const TrailModule& trails() const noexcept { return trails_; }
  RendererModule& renderer() noexcept { return renderer_; } const RendererModule& renderer() const noexcept { return renderer_; }
  ParticleSystemRenderer& particleSystemRenderer() noexcept { return particleSystemRenderer_; }
  const ParticleSystemRenderer& particleSystemRenderer() const noexcept { return particleSystemRenderer_; }
  SubEmittersModule& subEmitters() noexcept { return subemitters_; }
  const SubEmittersModule& subEmitters() const noexcept { return subemitters_; }
  LifetimeByEmitterSpeedModule& lifetimeByEmitterSpeed() noexcept { return lifetimeSpeed_; }
  const LifetimeByEmitterSpeedModule& lifetimeByEmitterSpeed() const noexcept { return lifetimeSpeed_; }
  ExternalForcesModule& externalForces() noexcept { return externalForces_; }
  const ExternalForcesModule& externalForces() const noexcept { return externalForces_; }
  CollisionModule& collision() noexcept { return collision_; }
  const CollisionModule& collision() const noexcept { return collision_; }
  TriggerModule& trigger() noexcept { return trigger_; }
  const TriggerModule& trigger() const noexcept { return trigger_; }
  CustomDataModule& customData() noexcept { return customData_; }
  const CustomDataModule& customData() const noexcept { return customData_; }
  const std::vector<Particle>& particles() const noexcept { return particles_; }
  int particleCount() const noexcept { return static_cast<int>(particles_.size()); }
  float time() const noexcept { return time_; } bool isPlaying() const noexcept { return playing_; } bool isPaused() const noexcept { return paused_; }
  std::uint32_t randomSeed() const noexcept { return randomSeed_; } void setRandomSeed(std::uint32_t value) noexcept { randomSeed_ = value; }
  bool useAutoRandomSeed() const noexcept { return autoRandomSeed_; } void setUseAutoRandomSeed(bool value) noexcept { autoRandomSeed_ = value; }
  bool get_useAutoRandomSeed() const noexcept { return useAutoRandomSeed(); } void set_useAutoRandomSeed(bool value) noexcept { setUseAutoRandomSeed(value); }
  std::uint32_t get_randomSeed() const noexcept { return randomSeed_; } void set_randomSeed(std::uint32_t value) noexcept { setRandomSeed(value); }
  bool get_isPlaying() const noexcept { return isPlaying(); } bool get_isPaused() const noexcept { return isPaused(); }
  GameObject* gameObject() noexcept { return owner_.get(); }
  const GameObject* gameObject() const noexcept { return owner_.get(); }
  Transform* transform() noexcept { return owner_ ? &owner_->transform : nullptr; }
  const Transform* transform() const noexcept { return owner_ ? &owner_->transform : nullptr; }
  BackendStatus backend_status() const noexcept { return BackendStatus::OpenDeterministicStub; }
  void set_resource_provenance(SplitEffectResourceProvenance value) { provenance_ = std::move(value); }
  const SplitEffectResourceProvenance& resource_provenance() const noexcept { return provenance_; }

  void Play(bool withChildren = true) noexcept; void Pause(bool withChildren = true) noexcept;
  void Stop(bool withChildren = true, ParticleSystemStopBehavior stopBehavior = ParticleSystemStopBehavior::StopEmitting) noexcept;
  void Clear(bool withChildren = true) noexcept; void Simulate(float t, bool withChildren = true, bool restart = true, bool fixedTimeStep = true) noexcept;
  void Emit(int count); void Emit(const Particle& particle); void Emit(const Particle* particles, int count);
  void Emit(const EmitParams& params, int count); void Emit(const Vector3& position, const Vector3& velocity, float size, float lifetime, const Color& color);
  void TriggerSubEmitter(int subEmitterIndex); void TriggerSubEmitter(int subEmitterIndex, Particle& particle);
  int GetParticles(Particle* particles, int size, int offset = 0) const; int SetParticles(const Particle* particles, int size, int offset = 0);
  bool IsAlive(bool withChildren = true) const noexcept;

 private:
  std::shared_ptr<GameObject> owner_;
  MainModule main_{}; EmissionModule emission_{}; ShapeModule shape_{}; VelocityOverLifetimeModule velocity_{}; ForceOverLifetimeModule force_{}; LimitVelocityOverLifetimeModule limit_{}; ColorOverLifetimeModule color_{}; ColorBySpeedModule colorSpeed_{}; SizeOverLifetimeModule size_{}; SizeBySpeedModule sizeSpeed_{}; RotationOverLifetimeModule rotation_{}; RotationBySpeedModule rotationSpeed_{}; NoiseModule noise_{}; InheritVelocityModule inherit_{}; LightsModule lights_{}; TextureSheetAnimationModule texture_{}; TrailModule trails_{}; RendererModule renderer_{}; ParticleSystemRenderer particleSystemRenderer_{}; SubEmittersModule subemitters_{}; LifetimeByEmitterSpeedModule lifetimeSpeed_{}; ExternalForcesModule externalForces_{}; CollisionModule collision_{}; TriggerModule trigger_{}; CustomDataModule customData_{};
  std::vector<Particle> particles_; float time_ = 0.0f; bool playing_ = false; bool paused_ = false; std::uint32_t randomSeed_ = 1; bool autoRandomSeed_ = true;
  float emit_accum_ = 0.0f; float rod_accum_ = 0.0f; Vector3 last_world_pos_{}; bool last_world_valid_ = false;
  bool bursts_consumed_ = false;
  SplitEffectResourceProvenance provenance_{};
  void reset_emit_state() noexcept;
  void emit_new_particles(int count, const Vector3& at, bool trigger_birth);
  void fire_sub_emitters(ParticleSystemSubEmitterType type, Particle& particle);
  void integrate_particles(float dt);
  void emit_from_modules(float dt, float t0);
};

// Typed catalog adapter used by the open split-effect controller.  It copies
// every serialized module field into the public API modules; no JSON reaches
// the controller.
struct CatalogParticleView {
  int effectId = 0; int particleIndex = 0; MainModule main{}; EmissionModule emission{}; ShapeModule shape{}; VelocityOverLifetimeModule velocity{}; ForceOverLifetimeModule force{}; LimitVelocityOverLifetimeModule limit{}; NoiseModule noise{}; TrailModule trails{}; RendererModule renderer{};
};
void configure_from_catalog(ParticleSystem& system, const CatalogParticleView& view) noexcept;

} // namespace wds::chart_render::unity
