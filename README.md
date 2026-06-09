# QtVideoPlayer - 基于 FFmpeg 的高性能多线程播放器

本项目是一款基于 **Qt 6** 与 **FFmpeg** 开发的视频播放引擎。采用多线程异步解码架构，深度优化了音视频同步算法，支持 4K 视频稳定播放。

## 推流测试工具

仓库内置了 `rtsp_loop_push`，用于把本地文件循环推送成更接近真实直播行为的 RTSP/RTMP 输入源。

- `file playback`
本地文件直接播放，不受墙钟限制，适合普通点播验证。
- `realtime loop push`
把本地文件按墙钟实时速率循环推送，默认强制实时节流，避免文件源无限供给导致播放器异常快放。
- `simulated-network push`
在实时推流基础上增加网络行为模拟，可注入启动延迟、抖动、突发停顿和限带宽，更适合验证 live 同步与抗抖动逻辑。

示例：

```bash
./rtsp_loop_push demo.mp4 rtsp://127.0.0.1:8554/live/test --copy
./rtsp_loop_push demo.mp4 rtsp://127.0.0.1:8554/live/test --mode simulated-network --jitter-ms 50 --burst-ms 150 --stall-every-ms 5000 --stall-duration-ms 300
```

说明：

- `--mode realtime` 为默认模式，会强制按实时速率推流。
- `--mode simulated-network` 会在本地代理层注入网络时序扰动，但不会修改媒体时间戳。
- `--re` 保留为兼容参数；新版本即使不传，也会默认启用实时节流。
- 若要复现真实直播问题，不要使用裸文件直推或绕过该工具直接把文件当无限数据源灌入播放器。

## 🛠 开发环境与依赖 (重要)

由于 FFmpeg 开发库文件较大，本仓库未包含其二进制文件。在尝试编译运行前，**请务必按照以下步骤配置环境**：

### 1. 环境要求
- **操作系统**：Windows (10/11)
- **编译器**：MinGW 64-bit (建议与 Qt 版本配套)
- **Qt 版本**：Qt 6.2 或更高版本
- **FFmpeg 版本**：FFmpeg 6.x (Shared 或 Dev 版本)

### 2. FFmpeg 目录配置
请前往 [FFmpeg 官网](https://ffmpeg.org/download.html) 或 [gyan.dev](https://www.gyan.dev/ffmpeg/builds/) 下载对应的 **Shared** 开发包，并解压至项目根目录下的 `3rdparty/ffmpeg` 文件夹中。

**必须确保目录结构如下所示**：
```text
videoplayer/
├── 3rdparty/
│   └── ffmpeg/
│       ├── bin/      # 存放 .dll 文件
│       ├── include/  # 存放头文件 (.h)
│       └── lib/      # 存放库文件 (.lib 或 .a)
├── src/              # 源代码
└── CMakeLists.txt
