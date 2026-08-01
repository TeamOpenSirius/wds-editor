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
| `official_chart.hpp` / `sus_chart.hpp` | 官方 CSV / SUS 相关 |
| `project.hpp` / `chart_session.hpp` | 工程与会话 |
| `edit_history.hpp` / `note_edit_ops.hpp` / `edit_grid.hpp` | 撤销与编辑操作 |
| `chart_serializer.hpp` | 读写盘 |
| `preview_config.hpp` | 预览阈值 |

音符 id：`kAutoNoteId = -1` 自动分配；规范化保存后为 `0..N-1`。

## 与 Unity 原版映射（摘要）

| 原版 | 本库 |
|------|------|
| `NotationEntity` | `NotationChart` / `NotationNote` |
| `IClock` / `GameClock` | `SeekableClock` |
| `AutoTouch` | `AutoJudgeSimulator` |
| `NoteObjectScheduler` | `PreviewSnapshotBuilder`（按时间求值） |

## 构建与测试

请用仓库根目录脚本（勿在 Linux 上做 native 产品 configure）：

```bash
# 仅内核示例（交叉 Win）
./scripts/build-target.sh win-x86_64 -- -DWDS_BUILD_RENDERER=OFF -DWDS_BUILD_UI=OFF
ctest --test-dir build-win-x86_64 -R wds_core_tests --output-on-failure
```

样例谱：`tests/fixtures/normalized_chart.wdschart`。
