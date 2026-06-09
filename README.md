# EdgeLivePlayer

`EdgeLivePlayer` 是一个基于 `Qt 6` 和 `FFmpeg` 的低延迟直播播放器工程，仓库内同时包含：

- Qt 播放器示例程序 `edgelive_demo_qt`
- RTSP 循环推流工具 `rtsp_loop_push`
- 本地 RTSP 服务启动脚本 `start_rtsp_server.sh`

这个 README 以当前 Linux 开发环境为准，整理了编译、启动服务和本地推流的完整步骤。

## 项目目录

常用文件和目录：

- `CMakeLists.txt`：项目入口构建文件
- `app/qt`：Qt 播放器示例
- `core`：播放核心逻辑
- `tools/rtsp_loop_push.cpp`：循环推流工具源码
- `start_rtsp_server.sh`：启动本地 RTSP 服务
- `mediamtx` / `mediamtx.yml`：本地 RTSP 服务程序与配置
- `test.mp4`：本地测试视频

## 环境要求

编译前请确认系统已安装：

- `cmake` 3.16 或更高版本
- `g++`，支持 `C++17`
- `Qt 6`：`Widgets`、`Multimedia`、`OpenGLWidgets`
- `FFmpeg` 开发库，目录位于 `3rdparty/ffmpeg`

`FFmpeg` 目录结构需要类似下面这样：

```text
3rdparty/ffmpeg/
├── include/
├── lib/
└── bin/      # Windows 场景常见；Linux 不强制要求
```

## 编译

在项目根目录执行：

```bash
cd /home/vboxuser/桌面/projects/demo
cmake -S . -B build_codex
cmake --build build_codex -j
```

编译完成后，常用产物包括：

- `./build_codex/rtsp_loop_push`
- `./build_codex/app/qt/edgelive_demo_qt`

如果你已经生成过 `build_codex`，也可以直接重新编译：

```bash
cd /home/vboxuser/桌面/projects/demo
cmake --build build_codex -j
```

## 启动本地 RTSP 服务

在项目根目录启动：

```bash
cd /home/vboxuser/桌面/projects/demo && ./start_rtsp_server.sh
```

这个脚本会启动仓库内自带的 `mediamtx`，并读取 `mediamtx.yml` 配置。

默认情况下，你可以向本机地址推流：

```text
rtsp://127.0.0.1:5540/live
```

## 推流命令

你当前使用的推流命令如下：

```bash
./build_codex/rtsp_loop_push /home/vboxuser/桌面/projects/demo/test.mp4 rtsp://127.0.0.1:5540/live --mode realtime --copy
```

如果希望从项目根目录完整执行，可以直接用：

```bash
cd /home/vboxuser/桌面/projects/demo && ./build_codex/rtsp_loop_push /home/vboxuser/桌面/projects/demo/test.mp4 rtsp://127.0.0.1:5540/live --mode realtime --copy
```

参数说明：

- `test.mp4`：本地输入视频
- `rtsp://127.0.0.1:5540/live`：推送目标地址
- `--mode realtime`：按实时速率推流，模拟直播节奏
- `--copy`：尽量直接复用编码流，减少转码开销

## 推荐测试流程

建议按下面顺序测试：

1. 编译项目
2. 启动本地 RTSP 服务
3. 执行 `rtsp_loop_push` 推流
4. 启动播放器并播放 `rtsp://127.0.0.1:5540/live`

示例：

```bash
cd /home/vboxuser/桌面/projects/demo
cmake --build build_codex -j
./start_rtsp_server.sh
```

新开一个终端执行：

```bash
cd /home/vboxuser/桌面/projects/demo && ./build_codex/rtsp_loop_push /home/vboxuser/桌面/projects/demo/test.mp4 rtsp://127.0.0.1:5540/live --mode realtime --copy
```

如果需要启动 Qt 播放器：

```bash
cd /home/vboxuser/桌面/projects/demo && ./build_codex/app/qt/edgelive_demo_qt
```

然后在播放器中输入：

```text
rtsp://127.0.0.1:5540/live
```

## GitHub 上传前建议

推到 GitHub 之前，建议先确认这些内容没有被误提交：

- `build/`
- `build-linux/`
- `build_codex/`
- `build_linux/`
- 大体积测试文件
- 本地 IDE 配置目录

你当前仓库已经有 `.gitignore`，提交前再执行一次：

```bash
git status
```

确认没有不想上传的构建产物后，再执行提交和推送。
