# EdgeLive Player SDK 接口文档

本文档基于当前项目源码整理，主要来源于 [sdk/player.h](/home/vboxuser/桌面/projects/demo/sdk/player.h:1)、[core/player/player_engine.cpp](/home/vboxuser/桌面/projects/demo/core/player/player_engine.cpp:795) 和 [app/qt/mainwindow.cpp](/home/vboxuser/桌面/projects/demo/app/qt/mainwindow.cpp:239)。

## 1. 命名空间

所有公开接口都位于 `edgelive` 命名空间下。

## 2. 枚举定义

### 2.1 `PlayerState`

播放器状态。

| 枚举值 | 含义 |
| --- | --- |
| `Created` | 对象已创建，但尚未初始化 |
| `Initialized` | 已调用 `Init`，可以开始 `Play` |
| `Playing` | 正在播放 |
| `Stopped` | 已停止播放，可再次 `Play` |
| `Released` | 已释放，生命周期结束 |
| `Error` | 播放过程中发生错误 |

### 2.2 `MediaSourceType`

媒体源类型。

| 枚举值 | 含义 |
| --- | --- |
| `Auto` | 自动识别类型 |
| `File` | 本地文件或普通文件型输入 |
| `Rtsp` | RTSP 实时流 |
| `Rtmp` | RTMP 实时流 |

说明：
- 当 `PlayRequest.source.type = Auto` 时，内部会根据 URL 自动判断。
- `rtsp://` 会识别为 `Rtsp`。
- `rtmp://` 会识别为 `Rtmp`。
- 其它默认按 `File` 处理。

### 2.3 `VideoPixelFormat`

视频帧像素格式。

| 枚举值 | 含义 |
| --- | --- |
| `Bgra` | BGRA 32 位格式，适合直接给 UI 渲染 |
| `Yuv420p` | YUV420P 格式 |
| `Nv12` | NV12 格式 |

说明：
- 解码后如果输入格式适合直接复用，底层可能输出 `Yuv420p` 或 `Nv12`。
- 否则会转换成 `Bgra`。

## 3. 结构体定义

### 3.1 `MediaSourceOptions`

媒体源播放参数，也是最核心的配置结构。

| 字段 | 类型 | 默认值 | 含义 |
| --- | --- | --- | --- |
| `transport` | `std::string` | `"tcp"` | RTSP 传输方式，通常填 `tcp` 或 `udp`，当前示例固定用 `tcp` |
| `timeout_ms` | `int` | `5000` | 网络超时，单位毫秒，传给 FFmpeg 打开流和探测连接使用 |
| `buffer_size` | `int` | `1024 * 1024` | FFmpeg 输入缓冲大小，单位字节 |
| `packet_queue_capacity` | `int` | `512` | 音视频包队列容量基准值 |
| `drop_oldest_on_full` | `bool` | `true` | 队列满时是否丢弃最旧数据，适合直播低延迟场景 |
| `low_latency_mode` | `bool` | `false` | 是否启用低延迟模式，直播建议开启，文件播放通常关闭 |
| `video_decode_queue_limit` | `int` | `8` | 视频解码前包队列深度限制 |
| `audio_decode_queue_limit` | `int` | `16` | 音频解码前包队列深度限制 |
| `video_render_queue_limit` | `int` | `8` | 视频渲染前帧队列限制 |
| `fast_startup` | `bool` | `false` | 是否启用快速起播策略 |
| `defer_audio_until_first_video` | `bool` | `false` | 是否等首帧视频到达后再放音频，减少直播开头音视频错位 |
| `av_sync_dead_zone_ms` | `int` | `20` | 音视频同步死区，差值在该范围内不调整 |
| `av_sync_max_lead_ms` | `int` | `120` | 视频领先音频时，最大等待范围 |
| `av_sync_max_lag_ms` | `int` | `400` | 视频落后音频时，允许的最大滞后范围 |
| `av_sync_drop_late_video` | `bool` | `true` | 视频严重落后时是否允许丢帧追赶 |
| `adaptive_jitter_enabled` | `bool` | `true` | 是否启用自适应抗抖动控制 |
| `jitter_min_ms` | `int` | `150` | 抖动缓冲最小目标值，单位毫秒 |
| `jitter_max_ms` | `int` | `1200` | 抖动缓冲最大目标值，单位毫秒 |
| `jitter_target_ms_initial` | `int` | `300` | 抖动缓冲初始目标值，单位毫秒 |
| `jitter_eval_window_ms` | `int` | `2000` | 网络抖动评估窗口，单位毫秒 |
| `latency_cap_ms` | `int` | `2000` | 总时延上限，超过后会主动裁剪队列 |
| `video_recovery_enabled` | `bool` | `true` | 是否启用视频恢复机制 |
| `video_recovery_live_only` | `bool` | `true` | 是否只对直播启用视频恢复 |

补充说明：
- `low_latency_mode=true` 时，内部会启用更激进的实时策略，例如更小缓存、队列裁剪、直播追帧。
- RTSP 场景下，`transport` 会写入 FFmpeg 选项 `rtsp_transport`。
- RTMP 场景下，`low_latency_mode=true` 会附加 `rtmp_buffer=0` 和 `rtmp_live=live`。
- 非 RTSP 的低延迟场景下，内部会附加 FFmpeg 选项 `fflags=nobuffer`、`flags=low_delay`、`flush_packets=1`。

### 3.2 `MediaSource`

媒体源定义。

| 字段 | 类型 | 默认值 | 含义 |
| --- | --- | --- | --- |
| `type` | `MediaSourceType` | `Auto` | 媒体类型 |
| `url` | `std::string` | `""` | 播放地址，可以是本地文件路径、RTSP、RTMP |
| `options` | `MediaSourceOptions` | `{}` | 对应媒体源的播放配置 |

### 3.3 `PlayerInitConfig`

播放器初始化配置。

| 字段 | 类型 | 默认值 | 含义 |
| --- | --- | --- | --- |
| `log_tag` | `std::string` | `"EdgeLivePlayer"` | 日志标签，方便日志过滤 |

### 3.4 `PlayRequest`

播放请求。

| 字段 | 类型 | 默认值 | 含义 |
| --- | --- | --- | --- |
| `url` | `std::string` | `""` | 简化传参用的播放地址 |
| `source` | `MediaSource` | `{}` | 完整媒体源配置 |

说明：
- 如果 `source.url` 为空，内部会自动使用 `url`。
- 如果 `source.type=Auto`，内部会根据 `source.url` 自动识别。

### 3.5 `VideoFrame`

视频帧回调数据。

| 字段 | 类型 | 含义 |
| --- | --- | --- |
| `width` | `int` | 视频宽度 |
| `height` | `int` | 视频高度 |
| `stride` | `int` | 主平面步长，通常用于 `Bgra` |
| `strides[3]` | `int[3]` | 各平面步长，适合 YUV/NV12 |
| `pts_ms` | `int64_t` | 显示时间戳，单位毫秒 |
| `data` | `const uint8_t*` | 主数据指针，常用于单平面格式 |
| `planes[3]` | `const uint8_t*[3]` | 多平面图像数据指针 |
| `format` | `VideoPixelFormat` | 像素格式 |
| `buffer` | `std::shared_ptr<const std::vector<uint8_t>>` | 帧内存持有对象，业务侧应依赖它保证数据有效期 |

说明：
- 回调结束后不要假设裸指针长期有效，若要异步使用，必须连同 `buffer` 一起持有。

### 3.6 `AudioFrame`

音频帧回调数据。

| 字段 | 类型 | 含义 |
| --- | --- | --- |
| `sample_rate` | `int` | 采样率 |
| `channels` | `int` | 声道数 |
| `bytes_per_sample` | `int` | 每个采样占用字节数 |
| `pts_ms` | `int64_t` | 时间戳，单位毫秒 |
| `data_size` | `size_t` | 数据长度，单位字节 |
| `data` | `const uint8_t*` | PCM 数据指针 |

说明：
- 当前实现输出音频为重采样后的 `S16` PCM，因此一般 `bytes_per_sample=2`。

### 3.7 `PlayerStats`

播放器运行统计信息。

#### 生命周期与计数

| 字段 | 类型 | 含义 |
| --- | --- | --- |
| `start_time_ms` | `int64_t` | 当前一次播放或初始化开始时间 |
| `state_change_count` | `uint32_t` | 状态变化次数 |
| `play_count` | `uint32_t` | 调用成功播放次数 |
| `stop_count` | `uint32_t` | 停止次数 |
| `error_count` | `uint32_t` | 错误次数 |

#### 收包与队列

| 字段 | 类型 | 含义 |
| --- | --- | --- |
| `demuxed_video_packets` | `uint64_t` | 已解复用的视频包数量 |
| `demuxed_audio_packets` | `uint64_t` | 已解复用的音频包数量 |
| `dropped_video_packets` | `uint64_t` | 被丢弃的视频包数量 |
| `dropped_audio_packets` | `uint64_t` | 被丢弃的音频包数量 |
| `video_queue_size` | `size_t` | 当前视频包队列长度 |
| `audio_queue_size` | `size_t` | 当前音频包队列长度 |
| `max_video_queue_size` | `size_t` | 历史最大视频队列长度 |
| `max_audio_queue_size` | `size_t` | 历史最大音频队列长度 |

#### 首帧与建链耗时

| 字段 | 类型 | 含义 |
| --- | --- | --- |
| `dns_cost_ms` | `int64_t` | DNS 解析耗时 |
| `connect_cost_ms` | `int64_t` | TCP 建连耗时 |
| `handshake_cost_ms` | `int64_t` | 握手耗时 |
| `open_input_cost_ms` | `int64_t` | 打开输入源耗时 |
| `stream_info_cost_ms` | `int64_t` | 获取流信息耗时 |
| `first_read_cost_ms` | `int64_t` | 首次读到数据耗时 |
| `first_video_decode_cost_ms` | `int64_t` | 首次解出视频耗时 |
| `first_video_render_cost_ms` | `int64_t` | 首次渲染视频耗时 |
| `first_audio_decode_cost_ms` | `int64_t` | 首次解出音频耗时 |
| `first_audio_play_cost_ms` | `int64_t` | 首次播放音频耗时 |

说明：
- 未发生时通常为 `-1` 或 `0`，取决于字段初始化与更新时机。

#### 音频链路统计

| 字段 | 类型 | 含义 |
| --- | --- | --- |
| `audio_pop_packet_count` | `uint64_t` | 音频解码线程取包次数 |
| `audio_send_packet_count` | `uint64_t` | 送入音频解码器的包数量 |
| `audio_send_packet_fail_count` | `uint64_t` | 音频包送解码器失败次数 |
| `audio_receive_frame_fail_count` | `uint64_t` | 从音频解码器取帧失败次数 |
| `audio_decoded_frame_count` | `uint64_t` | 音频解码成功帧数 |
| `audio_invalid_param_skip_count` | `uint64_t` | 音频参数非法被跳过次数 |
| `audio_invalid_sample_rate_skip_count` | `uint64_t` | 音频采样率非法被跳过次数 |
| `audio_swr_convert_fail_count` | `uint64_t` | 音频重采样失败次数 |
| `audio_last_swr_convert_result` | `int` | 最近一次重采样返回值 |
| `audio_output_frame_count` | `uint64_t` | 输出到业务侧的音频帧数 |
| `audio_started_after_first_video` | `bool` | 是否在首帧视频后才开始音频输出 |
| `audio_decode_avg_cost_us` | `int64_t` | 音频平均解码耗时，单位微秒 |
| `audio_packet_wait_avg_us` | `int64_t` | 音频线程等包平均耗时，单位微秒 |
| `audio_output_fps` | `int` | 音频输出频率，按内部窗口统计 |

#### 音视频同步统计

| 字段 | 类型 | 含义 |
| --- | --- | --- |
| `av_offset_avg_ms` | `int64_t` | 音视频时间差平均值 |
| `av_offset_max_ms` | `int64_t` | 音视频时间差最大值 |
| `av_offset_min_ms` | `int64_t` | 音视频时间差最小值 |
| `av_offset_sample_count` | `uint64_t` | 音视频差值采样次数 |
| `video_sync_drop_count` | `uint64_t` | 为追音频而丢掉的视频帧次数 |
| `last_av_offset_ms` | `int64_t` | 最近一次音视频差值 |
| `sync_video_pts_ms` | `int64_t` | 当前参与同步计算的视频 PTS |
| `sync_audio_clock_ms` | `int64_t` | 当前使用的音频时钟 |
| `sync_raw_diff_ms` | `int64_t` | 原始音视频差值 |
| `sync_final_delay_ms` | `int64_t` | 本次最终决定的等待时长 |
| `sync_action` | `std::string` | 本次同步动作，常见值有 `RENDER_NOW`、`WAIT`、`DROP` |

#### 渲染统计

| 字段 | 类型 | 含义 |
| --- | --- | --- |
| `render_stat_wall_time_ms` | `int64_t` | 渲染统计窗口实际墙钟时间 |
| `render_stat_video_pts_delta_ms` | `int64_t` | 渲染窗口内视频 PTS 推进量 |
| `render_stat_audio_clock_delta_ms` | `int64_t` | 渲染窗口内音频时钟推进量 |
| `render_stat_drops` | `uint64_t` | 渲染窗口内丢帧数 |
| `render_stat_avg_diff_ms` | `int64_t` | 渲染窗口平均音视频差值 |
| `video_decoded_frame_count` | `uint64_t` | 视频解码成功帧数 |
| `video_rendered_frame_count` | `uint64_t` | 视频渲染输出帧数 |
| `video_decode_avg_cost_us` | `int64_t` | 视频平均解码耗时，单位微秒 |
| `video_render_avg_cost_us` | `int64_t` | 视频平均渲染耗时，单位微秒 |
| `video_packet_wait_avg_us` | `int64_t` | 视频线程等包平均耗时，单位微秒 |
| `video_decode_fps` | `int` | 视频解码频率 |
| `video_render_fps` | `int` | 视频渲染频率 |

#### 抖动控制与延迟控制

| 字段 | 类型 | 含义 |
| --- | --- | --- |
| `jitter_target_ms` | `int` | 当前抖动控制目标缓存 |
| `jitter_current_ms` | `int` | 当前实际缓存时长 |
| `jitter_interarrival_std_ms` | `double` | 包到达间隔标准差 |
| `loss_rate` | `double` | 推测丢包率 |
| `reorder_rate` | `double` | 推测乱序率 |
| `jitter_mode_switch_count` | `uint64_t` | 抖动模式切换次数 |
| `latency_cap_trigger_count` | `uint64_t` | 延迟上限触发次数 |
| `latency_cap_dropped_video_packets` | `uint64_t` | 因超时延裁掉的视频包数 |
| `latency_cap_dropped_audio_packets` | `uint64_t` | 因超时延裁掉的音频包数 |

#### 视频恢复统计

| 字段 | 类型 | 含义 |
| --- | --- | --- |
| `video_recovery_enter_count` | `uint64_t` | 进入视频恢复模式次数 |
| `video_recovery_exit_count` | `uint64_t` | 退出视频恢复模式次数 |
| `video_recovery_drop_count` | `uint64_t` | 恢复期间丢弃的数据量统计 |
| `last_video_recovery_duration_ms` | `int64_t` | 最近一次恢复持续时长 |

### 3.8 `PlayerCallbacks`

业务层传入的回调集合。

| 字段 | 类型 | 含义 |
| --- | --- | --- |
| `on_video_frame` | `std::function<void(const VideoFrame&)>` | 收到视频帧时回调 |
| `on_audio_frame` | `std::function<void(const AudioFrame&)>` | 收到音频帧时回调 |
| `get_audio_clock_ms` | `std::function<int64_t()>` | 业务侧提供当前真实音频播放时钟 |
| `on_state_changed` | `std::function<void(PlayerState)>` | 状态变化回调 |
| `on_error` | `std::function<void(int, const std::string&)>` | 错误回调，带错误码和错误信息 |
| `on_stats` | `std::function<void(const PlayerStats&)>` | 统计信息回调 |

重点说明：
- `get_audio_clock_ms` 很重要，播放器会优先使用它做音视频同步。
- 如果业务层不提供 `get_audio_clock_ms`，内部会退回到估算时钟。
- `on_stats` 不会每帧都回调，内部有节流逻辑。

## 4. 接口类 `IPlayer`

### 4.1 `bool Init(const PlayerInitConfig& config, const PlayerCallbacks& callbacks)`

初始化播放器。

参数说明：

| 参数 | 类型 | 含义 |
| --- | --- | --- |
| `config` | `const PlayerInitConfig&` | 初始化配置 |
| `callbacks` | `const PlayerCallbacks&` | 回调函数集合 |

返回值：
- `true`：初始化成功
- `false`：当前状态不允许初始化

调用要求：
- 一般在 `CreatePlayer()` 后调用一次。
- 当前实现允许在 `Created` 或 `Stopped` 状态下调用。

### 4.2 `bool Play(const PlayRequest& request)`

开始播放。

参数说明：

| 参数 | 类型 | 含义 |
| --- | --- | --- |
| `request` | `const PlayRequest&` | 播放请求，包含地址与配置 |

返回值：
- `true`：启动播放成功
- `false`：启动失败

行为说明：
- 如果 `request.source.url` 为空，会使用 `request.url`。
- 如果 URL 为空，会通过 `on_error` 报错。
- 内部调用前会先执行一次 `Stop()`，确保上次播放被清理。
- 当前实现会启动解复用线程、音频解码线程、视频解码线程、视频渲染线程。

### 4.3 `bool Stop()`

停止播放。

返回值：
- `true`：停止成功

行为说明：
- 会通知各线程退出并等待线程结束。
- 会清空队列、释放 FFmpeg 运行时资源。
- `Playing` 或 `Error` 状态下停止后会切换为 `Stopped`。

### 4.4 `bool Release()`

释放播放器。

返回值：
- `true`：释放成功

行为说明：
- 内部会先执行 `Stop()`。
- 最终状态会变成 `Released`。
- `Released` 后不应再继续使用该对象。

### 4.5 `PlayerState GetState() const`

获取当前状态。

返回值：
- 当前播放器状态枚举值。

### 4.6 `PlayerStats GetStats() const`

获取当前统计信息快照。

返回值：
- 当前的 `PlayerStats` 副本。

## 5. 工厂函数

### `std::unique_ptr<IPlayer> CreatePlayer()`

创建播放器实例。

返回值：
- 一个 `IPlayer` 智能指针，当前实际实现类为 `PlayerEngine`。

## 6. 当前项目中的默认播放配置

Qt 示例在 [app/qt/mainwindow.cpp](/home/vboxuser/桌面/projects/demo/app/qt/mainwindow.cpp:239) 中对不同源做了如下默认设置。

### 6.1 文件播放

| 配置项 | 值 |
| --- | --- |
| `type` | `File` |
| `transport` | `tcp` |
| `timeout_ms` | `5000` |
| `buffer_size` | `1048576` |
| `low_latency_mode` | `false` |
| `packet_queue_capacity` | `256` |
| `video_decode_queue_limit` | `8` |
| `audio_decode_queue_limit` | `16` |
| `video_render_queue_limit` | `3` |
| `fast_startup` | `false` |
| `defer_audio_until_first_video` | `false` |

特点：
- 更偏向平稳完整播放。
- 不主动追求极低延迟。

### 6.2 RTSP/RTMP 直播播放

| 配置项 | 值 |
| --- | --- |
| `low_latency_mode` | `true` |
| `packet_queue_capacity` | `512` |
| `fast_startup` | RTSP 下为 `true` |
| `defer_audio_until_first_video` | RTSP 下为 `true` |
| `jitter_min_ms` | `400` |
| `jitter_target_ms_initial` | `500` |
| `jitter_max_ms` | `600` |
| `jitter_eval_window_ms` | `800` |
| `av_sync_max_lead_ms` | `80` |
| `av_sync_max_lag_ms` | `250` |
| `latency_cap_ms` | `800` |

特点：
- 更偏向抗网络抖动与控延迟。
- 允许通过丢旧数据、追关键帧等方式追回实时性。

## 7. 推荐调用顺序

```cpp
auto player = edgelive::CreatePlayer();

edgelive::PlayerInitConfig init_config;
init_config.log_tag = "DemoPlayer";

edgelive::PlayerCallbacks cbs;
cbs.on_video_frame = [](const edgelive::VideoFrame& frame) {
    // 渲染视频
};
cbs.on_audio_frame = [](const edgelive::AudioFrame& frame) {
    // 播放音频
};
cbs.get_audio_clock_ms = []() -> int64_t {
    return 0;
};
cbs.on_state_changed = [](edgelive::PlayerState state) {
};
cbs.on_error = [](int code, const std::string& msg) {
};
cbs.on_stats = [](const edgelive::PlayerStats& stats) {
};

player->Init(init_config, cbs);

edgelive::PlayRequest req;
req.url = "rtsp://127.0.0.1/live/test";
req.source.url = req.url;
req.source.type = edgelive::MediaSourceType::Rtsp;
req.source.options.low_latency_mode = true;

player->Play(req);
player->Stop();
player->Release();
```

## 8. 使用注意事项

1. `Init` 建议先于 `Play` 调用，否则 `Play` 可能因为状态不对而失败。
2. `Release` 之后不要再次调用 `Play`。
3. 业务层若自行播放音频，建议实现 `get_audio_clock_ms`，这样同步效果更稳定。
4. 直播场景建议开启 `low_latency_mode`，并合理设置 `jitter_*` 与 `latency_cap_ms`。
5. 文件播放不建议盲目开启低延迟模式，否则可能影响完整性与平滑性。
6. `VideoFrame` 和 `AudioFrame` 回调中的裸指针只适合即时消费，不建议长期保存。

## 9. 可直接复用的参数建议

### 本地文件

```cpp
req.source.type = edgelive::MediaSourceType::File;
req.source.options.low_latency_mode = false;
req.source.options.packet_queue_capacity = 256;
req.source.options.video_render_queue_limit = 3;
```

### RTSP 低延迟

```cpp
req.source.type = edgelive::MediaSourceType::Rtsp;
req.source.options.transport = "tcp";
req.source.options.low_latency_mode = true;
req.source.options.fast_startup = true;
req.source.options.defer_audio_until_first_video = true;
req.source.options.jitter_min_ms = 400;
req.source.options.jitter_target_ms_initial = 500;
req.source.options.jitter_max_ms = 600;
req.source.options.latency_cap_ms = 800;
```

### RTMP 低延迟

```cpp
req.source.type = edgelive::MediaSourceType::Rtmp;
req.source.options.low_latency_mode = true;
req.source.options.packet_queue_capacity = 512;
req.source.options.av_sync_max_lead_ms = 80;
req.source.options.av_sync_max_lag_ms = 250;
req.source.options.latency_cap_ms = 800;
```
