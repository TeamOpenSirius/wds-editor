# common — `wds_common` / `wds::common`

跨模块共享的高频原语层。只放「几乎所有上层都会用到」的类型与宏；谱面、渲染、音频、UI 业务逻辑禁止放入本模块。

## 职责

- 可 seek 的全局时间轴 `Timeline`
- 时间单位与播放状态（微秒精度，毫秒辅助 API）
- 条件编译日志宏 `WDS_LOG` / `WDS_LOG_IF`
- 可选崩溃处理钩子

## 结构

```text
common/
├── CMakeLists.txt
├── include/wds/common/
│   ├── common.hpp          # 伞头
│   ├── log.hpp             # WDS_LOG
│   ├── time.hpp            # Microseconds, PlaybackState, TimelineSnapshot
│   ├── timeline.hpp        # Timeline
│   └── crash_handler.hpp
├── src/
└── tests/
```

CMake 目标：`wds_common`（别名 `wds::common`，STATIC）。无第三方依赖。

## 主要接口

### `Timeline`（`<wds/common/timeline.hpp>`）

```cpp
wds::common::Timeline tl;
tl.seek_ms(1000);
tl.play();
tl.tick_ms(16);
auto snap = tl.snapshot();   // TimelineSnapshot { position, state }
tl.apply(snap);              // 与 audio Transport / core 对齐
```

| API | 说明 |
|-----|------|
| `position()` / `position_ms()` | 当前时间 |
| `seek` / `seek_ms` | 任意跳转（含回滚） |
| `play` / `pause` / `toggle_playback` | 播放状态 |
| `advance` / `tick_ms` | 仅在 Playing 时推进 |
| `snapshot` / `apply` | 与外部时钟交换 |

### 时间类型（`<wds/common/time.hpp>`）

- `Microseconds`：内部精度
- `PlaybackState`：`Playing` / `Paused`
- `TimelineSnapshot`：`{ position, state }` — audio `Transport::poll` 与 core `apply_timeline` 的契约类型
- `ms_to_us`：在 `int64` 边界饱和，不溢出；C++ `/` 朝零截断，最后可精确换算的毫秒是 `INT64_MIN/1000` 与 `INT64_MAX/1000`
- `us_to_ms_floor` / `us_to_ms_round`：后者按绝对值 ≥500µs 远离零取整，极值同样不溢出

### 日志（`<wds/common/log.hpp>`）

- Debug 构建启用 `WDS_ENABLE_LOGGING=1`；Release 编译掉日志体
- 使用：`WDS_LOG("msg {}", value);`

## 依赖

无。上层：`core`、`audio-player`、`renderer`、`interaction`、`ui` 均可依赖本模块。

## 测试

交叉编译时不编 `wds_common_tests`。macOS 产品门禁见仓库根 README；Linux 宿主仅测 common+core 可用 `scripts/run-host-core-tests.sh`。不要对 `build-win-x86_64` 跑 `ctest`。

```bash
ctest --test-dir build-macos-arm -R wds_common_tests --output-on-failure
```
