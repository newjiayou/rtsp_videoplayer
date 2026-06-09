# Level 4: TTFF 与低延迟模式实施计划

## 目标

- 将播放器首帧时间（TTFF）相对计划2基线下降 30% 以上。
- 在局域网 RTSP/RTMP 场景下，将首帧进入 300ms 级。
- 控制解码前缓存堆积，避免端到端延迟随播放时间增长。

## 本次代码改造范围

- `sdk/player.h`
- `core/player/player_engine.h`
- `core/player/player_engine.cpp`
- `app/qt/mainwindow.cpp`

## 已落地的能力

1. 低延迟 preset
- `low_latency_mode=true` 时启用：
- `fflags=nobuffer`
- `flags=low_delay`
- `flush_packets=1`
- `probesize=32768`
- `analyzeduration=0`
- `rtsp: reorder_queue_size=0`
- `rtmp: rtmp_buffer=0`

2. 首帧路径分段计时
- `dns_cost_ms`
- `connect_cost_ms`
- `handshake_cost_ms`
- `first_read_cost_ms`
- `first_video_decode_cost_ms`
- `first_video_render_cost_ms`
- `first_audio_decode_cost_ms`
- `first_audio_play_cost_ms`

3. 队列限深
- 低延迟模式下收紧 `packet_queue_capacity`
- 记录 `max_video_queue_size` / `max_audio_queue_size`
- 解码阶段按 `video_decode_queue_limit` / `audio_decode_queue_limit` 抑制积压

4. 快速起播
- `fast_startup=true`
- `defer_audio_until_first_video=true`
- 启动阶段优先抢视频首帧，音频在首帧后并入

## 参数建议

### 局域网 RTSP

```text
transport=tcp
timeout_ms=1500~3000
buffer_size=65536~262144
packet_queue_capacity=16~32
video_decode_queue_limit=4~8
audio_decode_queue_limit=8~16
video_render_queue_limit=2~3
low_latency_mode=true
fast_startup=true
defer_audio_until_first_video=true
```

### 局域网 RTMP

```text
timeout_ms=1500~3000
buffer_size=65536~262144
packet_queue_capacity=16~32
video_decode_queue_limit=4~8
audio_decode_queue_limit=8~16
video_render_queue_limit=2~3
low_latency_mode=true
fast_startup=true
defer_audio_until_first_video=true
```

### 文件/点播

```text
low_latency_mode=false
fast_startup=false
保留较大 packet queue，避免误丢包影响完整播放
```

## TTFF 对比报告模板

### 测试环境

- 日期：
- 分支 / commit：
- 媒体源：
- 协议：
- 网络环境：局域网 / 跨网
- 分辨率 / 编码：

### 结果表

| 版本 | DNS | Connect | Handshake | FirstPacket | FirstDecode | FirstRender | FirstAudio | TTFF(video) | TTFF(audio) | MaxVQ | MaxAQ |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 计划2基线 |  |  |  |  |  |  |  |  |  |  |  |
| 计划4优化后 |  |  |  |  |  |  |  |  |  |  |  |
| 改善幅度 |  |  |  |  |  |  |  |  |  |  |  |

### 门禁判定

- TTFF(video) 相对计划2下降是否 `>30%`：
- 局域网场景是否进入 `300ms` 级：
- 队列峰值是否持续受控：
- 是否出现音视频起播异常或花屏：

## 当前实现说明

- `DNS/Connect/Handshake` 目前不是直接读取 FFmpeg 内部事件，而是：
- `DNS` 与 `Connect` 通过预探测 socket 实测
- `Handshake` 通过 `open_input - dns - connect` 近似估算
- 这个方案足够支撑计划4阶段的优化对比，但若后续要做严格协议级分析，建议再接入自定义 IO/协议级埋点。
