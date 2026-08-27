# core — `wds_core` / `wds::core`

制谱器逻辑内核：谱面数据、编辑操作、多格式序列化、预览快照。  
命名空间对外类型多为 `wds::chart_editor`。禁止依赖 Vulkan、BASS、窗口或 UI。

## 职责

| 能力 | 实现 |
|------|------|
| 内存中编辑谱面 | `ChartDocument` / `ChartEditorEngine` |
| 编辑区 ↔ 预览同步 | 统一时间轴 + `PreviewSnapshot` |
| 任意 seek / 回滚 | `SeekableClock` + 按时间点求值的 `PreviewSnapshotBuilder` |
| Auto 预览判定 | `AutoJudgeSimulator` |
| Gimmick / Split Lane | `gimmick.hpp`、`SplitLaneSimulator` |
| 范围查询索引 | `ChartNoteIndex`（AVL） |
| 磁盘 I/O | **仅** `save_to_file` / `load_from_file` → `ChartSerializer` |

## 结构

```text
core/
├── CMakeLists.txt
├── include/
│   ├── wds/core/…          # 推荐：#include <wds/core/…>
│   └── *.hpp               # 扁平转发兼容层
├── src/
├── examples/               # wds_core_example（可选）
└── tests/ + fixtures/
```

CMake：`wds_core`（别名 `wds::core`）。依赖：`wds::common`。

## 内存与磁盘边界

- `add_note` / `update_note` / `remove_note` / `load_chart` 等 **不写盘**
- `is_dirty()` 标记未保存修改
- UI 应在用户显式保存时调用 `save_to_file()`

```text
编辑 → ChartDocument (内存) → ChartNoteIndex
预览 → PreviewSnapshotBuilder::rebuild_or_update
         ├ estimate_diff → IncrementalPatch 或 FullRebuild
保存 → ChartSerializer::save_to_file
```

## 主要接口

### `ChartEditorEngine`（主入口）

```cpp
#include <wds/core/chart_editor_engine.hpp>

wds::chart_editor::ChartEditorEngine engine;
int32_t id = engine.add_note(note);
engine.update_note(id, note);
engine.remove_note(id);

engine.apply_timeline(transport.poll(delta_us));  // 与 audio 对齐
engine.seek(12345);   // ms
const auto& snap = engine.snapshot();

engine.set_snapshot_callback([](const PreviewSnapshot& s) { /* sync renderer */ });
engine.save_to_file(path);
engine.load_from_file(path);
```

### `ChartDocument`

```cpp
auto& doc = engine.document();
doc.set_timing({.bpm = 180, .ticks_per_quarter = 480});
doc.tick_to_milliseconds(tick);
doc.to_notation_chart();   // 只读快照供预览重建
```

### `SeekableClock`

| 方法 | 说明 |
|------|------|
| `seek(ms)` | 任意跳转 |
| `play` / `pause` | 播放控制 |
| `tick(delta_ms)` | Playing 时推进 |
| `current_time_ms()` | 与编辑标尺共用 |

外部 transport 请优先 `engine.apply_timeline(TimelineSnapshot)`，而不是直接拨时钟。

### `PreviewSnapshot` / `PreviewSnapshotBuilder`

每帧 `rebuild_or_update`：

1. `estimate_diff` 估算 churn  
2. 过大 → `FullRebuild`（`clear_keep_capacity`）；否则 `IncrementalPatch`  
3. 就地更新 `PreviewNoteInstance` / split / concurrent

```cpp
snapshot.upsert_note(instance);
snapshot.find_note(id);
snapshot.clear_keep_capacity();
```

### 其他公开头

| 头文件 | 作用 |
|--------|------|
| `notation.hpp` / `types.hpp` | 谱面与音符类型 |
| `official_chart.hpp` / `sus_chart.hpp` | 官方 CSV / SUS 相关（仅保证 Ched 12 键窗口子集；lane offset 为有意设计） |
| `project.hpp` / `chart_session.hpp` | 工程与会话 |
| `edit_history.hpp` / `note_edit_ops.hpp` / `edit_grid.hpp` | 撤销与编辑操作 |
| `chart_serializer.hpp` | 读写盘 |
| `preview_config.hpp` | 预览阈值 |

音符 id：`kAutoNoteId = -1` 自动分配。写入 `.wdschart` 时对**副本**规范化为 `0..N-1`；内存会话 id 保持稳定（可稀疏），重新加载文件后才是稠密 id。

## 与 Unity 原版映射（摘要）

| 原版 | 本库 |
|------|------|
| `NotationEntity` | `NotationChart` / `NotationNote` |
| `IClock` / `GameClock` | `SeekableClock` |
| `AutoTouch` | `AutoJudgeSimulator` |
| `NoteObjectScheduler` | `PreviewSnapshotBuilder`（按时间求值） |

## 构建与测试

请用仓库根目录脚本（勿在 Linux 上做 native 产品 configure）。命令与交叉策略见根 README 与 [`cmake/CROSS_COMPILE.md`](../cmake/CROSS_COMPILE.md)。

- 单测：`wds_core_tests`、`wds_split_index_tests`。交叉时默认 `WDS_CORE_BUILD_TESTS=OFF`；若显式打开仍可能编译 PE，但 **不** 向 CTest 注册。
- Linux 宿主只跑 common+core：`./scripts/run-host-core-tests.sh`（不是产品构建，不能代替 macOS 门禁）。

```bash
./scripts/build-target.sh macos-arm
ctest --test-dir build-macos-arm -R 'wds_core_tests|wds_split_index_tests' --output-on-failure
```

样例谱：`tests/fixtures/normalized_chart.wdschart`。

## 基准（默认关）

`WDS_CORE_BUILD_BENCHMARKS=ON` 才编 `wds_core_bench`，**不**注册 CTest，也不是 CI 门禁。先核对与参考实现一致，再报中位数耗时（warmup 3、计时 20 次）：

- 索引更新：N=5000 音符、K=50 次改动；`update_note` 相对 `apply_note_updates` 的 `speed_ratio`
- combo：2k / 5k / 10k 组 Hold+星（每组 2 个音符）上 `collect_preview_combo_hits` 相对参考收集，以及规模增长比

```bash
./scripts/build-target.sh macos-arm --no-package -- -DWDS_CORE_BUILD_BENCHMARKS=ON
./build-macos-arm/core/wds_core_bench
```
