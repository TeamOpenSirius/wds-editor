# audio-player — `wds_audio_player` / `wds::audio_player`

基于 BASS 的音乐 + 打击音效混音，以及驱动预览时钟的 `Transport`。  
仅依赖 `wds::common`；禁止依赖 `core` / `renderer` / Vulkan。

## 职责

- 打开音频设备、加载 BGM 与 SFX
- 将用户播放意图排队，在 `poll()` 内提交为 `TimelineSnapshot`
- 音量总线（master / music / sfx）与播放速率

## 结构

```text
audio-player/
├── CMakeLists.txt
├── include/wds/audio/
│   ├── audio.hpp           # 伞头
│   ├── audio_engine.hpp    # AudioEngine
│   ├── transport.hpp       # Transport
│   └── hit_sfx.hpp         # HitSfxPlayer
├── src/
├── tests/
└── third_party/bass/
    ├── include/bass.h + bassmix.h
    ├── macos-arm/libbass.dylib + libbassmix.dylib
    └── win-x86_64/bass.dll + bassmix.dll (+ .lib)
```

CMake：`wds_audio_player`（别名 `wds::audio_player`）。  
运行时库由打包脚本拷贝进发行包（勿依赖系统 BASS）。

## 主要接口

### `Transport`

```cpp
#include <wds/audio/audio.hpp>

wds::audio::Transport transport;
transport.initialize(effects_dir, bgm_path);
transport.request_play();
transport.request_seek_ms(12345);
transport.set_playback_rate(1.0f);
transport.set_chart_offset_ms(offset);

auto snap = transport.poll(wall_delta_us);  // 微秒墙钟，避免高刷截断
engine.apply_timeline(snap);                // 由 ui/core 层应用
transport.start_pending_music();            // 武装 SFX 后再真正出声
```

| API | 说明 |
|-----|------|
| `request_play/pause/toggle/seek_*` | 只入队，在 `poll` 生效 |
| `poll(wall_delta_us)` | 返回 `wds::common::TimelineSnapshot` |
| `set_chart_offset_ms` | 谱面相对音乐延迟（UI 用；播放头仍与音乐 1:1） |
| `set_playback_rate` | 预览时钟与 BGM 速率；SFX 采样率保持 1× |
| `audio()` | 访问底层 `AudioEngine` |

### `AudioEngine`

- `set_music_gain` / `set_sfx_gain`（0..1）

### `HitSfxPlayer`

按判定结果播放 `effects/` 下音效；由 preview 映射表调度。

## 依赖与平台

| 目标 | BASS / BASSmix 路径 |
|------|-----------|
| `macos-arm` | `third_party/bass/macos-arm/libbass.dylib` + `libbassmix.dylib` |
| `win-x86_64` | `third_party/bass/win-x86_64/bass.dll` + `bassmix.dll` |

Linux **不是**产品目标（无 BASS linux 二进制）；在 Linux 主机上请交叉编译 Windows。

## 测试

```bash
ctest -R wds_audio_player_tests
```

关闭测试：`-DWDS_AUDIO_BUILD_TESTS=OFF`。
