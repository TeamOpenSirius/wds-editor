# OpenWDS OfflinePreview SplitLane resources

This library is pinned to OpenWDS SplitLane asset version `1.96.0`. The
authoritative source is the OpenWDS OfflinePreview scene and its open runtime
controller:

* `wds-resources/Assets/OpenWDS/Runtime/OfflineRhythmPreview.cs`
* `wds-resources/Assets/OpenWDS/Runtime/ClientAssetsShared/SplitEffectController.cs`
* `wds-resources/Assets/OpenWDS/Runtime/ClientAssetsShared/SplitEffectElement.cs`

The runtime resource tree is the repo-root `resources/` directory (shipped
with every package; the generator that produced it is not part of this
repository). It contains 790 bundles, each with an `.objects.tsv`
identity/PPtr index and an `.objects.jsonl` complete field view, so callers
can inspect fields without parsing a Unity bundle.

The CMake target is `wds::openwds_split_effects`. It uses the stage2
`wds::unity_particle_api` contract and the low-level official playfield
formulas exported by `wds::core`; it does not depend on the higher-level
`wds::chart_render` library. The public entry points are:

```cpp
#include <wds/chart_render/openwds_split_effects/controller.hpp>
using namespace wds::chart_render::openwds_split_effects;

auto effect = create_controller(11570);  // strict: nullptr for unknown IDs
const LoadedEffect& source = effect->loaded_resource();
effect->initialize(false, 100);
effect->on_fade_in(3, 7);
effect->on_fade_out();
effect->clear();
```

`OfflineRhythmPreview` is the standalone facade for the recovered
`OfflineRhythmPreview.cs` scheduling and note-position logic. Configure it with
Unity-compatible `Transform*` values, then call `on_enable` and `update` (or
`evaluate`) to apply the authored lane, span, travel, loop, and despawn rules.
Its scene/audio/render integration remains outside this open contract.

For resource-only inspection, call
`load_effect(effect_id, resource_root)`; strict lookup is provided by
`resource_for_effect`, and the complete public ID set is returned by
`resource_effect_ids()`.

`load_effect` reads the selected effect's complete bundle closure, exposes each
object's `(bundle,pathId)`, serialized type, raw-data SHA-256, complete JSON
field record, and PPtr field targets. It keeps repeated references as repeated
references to the same source identity; it does not merge path IDs across
bundles. Unity's declared `unity default resources` are reported as known
external references. Any other missing target is reported in
`unresolved_references` and makes `resolves_all_references` false.

`Controller` is the source controller facade. Its serialized controller
arrays, element groups, and ParticleSystem references come from the loaded
object graph; the lifecycle methods follow the open controller protocol:

| OpenWDS method | C++ method |
| --- | --- |
| `Initialize` | `Controller::initialize` |
| `OnFadeIn` | `Controller::on_fade_in` |
| `OnFadeOut` | `Controller::on_fade_out` |
| `Clear` | `Controller::clear` |
| `ApplyOriginalSplitPositions` | private `apply_original_split_positions` |
| `SetLineX` | private `set_line_x` |

The actual serialized `_splitLines`, `_splitEffectElements`, and each element's
`_lineEffects` references determine materialized ParticleSystem instances.
Authored line and element names, local transforms, and element `LineColor`
metadata are consumed before the runtime opacity adjustment. An empty or null
`LineEffects` array remains an element with no ParticleSystem placeholder,
matching the source controller. Each active line gets an Instantiate-style
clone while `source_identities` records the authored physical object it came
from. Typed Unity module
deserialization is intentionally the stage2 API boundary: every authored field
is retained in `LoadedEffect::objects[*].serialized_json` and the API stub
receives resource provenance, but module properties are not claimed to have
been reconstructed into typed runtime values here. No field is silently
replaced with an older lossy catalog default.
The typed API reports `OpenDeterministicStub` for closed Unity physics,
rendering, and callback internals; this library therefore makes no Unity
runtime or screenshot-equivalence claim.

Build and contract check:

```sh
cmake -S . -B build-macos-arm
cmake --build build-macos-arm --target wds_openwds_split_effects_contract_tests
build-macos-arm/chart-render/openwds-split-effects/wds_openwds_split_effects_contract_tests
```
