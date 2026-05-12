# DandelionOS

一个基于 C++ 的可视化线程调度项目。当前可运行版本使用：

- `main.cpp` 作为主程序入口，也是最重要的主程序
- SDL2 负责窗口与可视化
- Python + MediaPipe 负责摄像头嘴部检测桥接
- C++ 原生 `waveIn` 负责麦克风输入

`DESIGN.md` 是系统设计说明，`README.md` 重点讲如何在另一台机器上部署和运行。

**主程序说明**

`main.cpp` 是项目主入口，负责：

- 启动运行时
- 自动拉起 `python_mediapipe_bridge.py`
- 驱动调度器循环
- 打开 SDL 可视化窗口

README 不再介绍测试用文件，避免和正式运行入口混淆。

**运行环境**

当前部署路径默认面向 `Windows`。

原因：

- `run_live.bat` 使用 MinGW `g++`
- `python_mediapipe_bridge.py` 走本地摄像头
- 麦克风桥接在 C++ 里使用 `winmm / waveIn`

如果换到别的系统，需要自己改编译链和音频采集部分。

**异地部署步骤**

下面的步骤适合“把项目发给另一台 Windows 机器后，从零开始部署”。

1. 安装基础工具

- 安装 `Python 3`
- 安装 `MSYS2`
- 在 `MSYS2` 中安装 `mingw64` 的 `g++`
- 安装 `SDL2` 开发库，并确保它位于 `C:\msys64\mingw64`

2. 准备 Python 依赖

进入项目根目录后执行：

```powershell
python -m pip install --upgrade pip
python -m pip install opencv-python mediapipe
```

3. 确认模型文件存在

项目根目录必须有：

- `face_landmarker.task`

这是 `python_mediapipe_bridge.py` 启动 MediaPipe Face Landmarker 时要加载的模型文件。没有它，摄像头桥接无法正常工作。

4. 确认目录中保留这些运行文件

部署到远端机器时，至少要带上下面这些文件：

- `main.cpp`
- `thread.h`
- `scheduler.cpp`
- `simulation.cpp`
- `events.cpp`
- `bridge_io.cpp`
- `render_sdl.cpp`
- `python_mediapipe_bridge.py`
- `run_live.bat`
- `run_mediapipe_bridge.bat`
- `face_landmarker.task`
- `DESIGN.md`
- `README.md`

5. 检查 `run_live.bat` 里的 MinGW 路径

当前脚本默认：

```bat
set "MINGW_ROOT=C:\msys64\mingw64"
```

如果远端机器的 MSYS2 不在这个位置，请先把这个路径改成实际安装目录。

6. 编译主程序

在项目根目录直接运行：

```powershell
.\run_live.bat
```

这个脚本会先编译，再启动程序。它内部实际编译的是：

```bat
g++ -std=c++17 -Wall -Wextra -pedantic ^
  -I"%MINGW_ROOT%\include\SDL2" ^
  main.cpp scheduler.cpp simulation.cpp events.cpp bridge_io.cpp render_sdl.cpp ^
  -L"%MINGW_ROOT%\lib" -lmingw32 -lSDL2main -lSDL2 ^
  -o dandelion_live.exe
```

7. 首次启动时的实际行为

程序启动后，`main.cpp` 会自动做这些事：

- 初始化运行时
- 创建 SDL 窗口
- 删除旧的桥接 JSON 文件
- 自动启动 `python_mediapipe_bridge.py`
- 等待摄像头桥接写入第一份 `camera_bridge_latest.json`
- 进入主调度循环

8. 如果自动拉起摄像头桥接失败

可以手动开一个终端，在项目目录执行：

```powershell
python .\python_mediapipe_bridge.py
```

然后再重新运行：

```powershell
.\dandelion_live.exe
```

或者再次直接执行：

```powershell
.\run_live.bat
```

**运行时控制**

SDL 窗口支持以下快捷键：

- `q` 或 `Esc`：退出
- `r`：重置
- `c`：切换蒲公英位置
- `1` / `2` / `3`：切换线程模式
- `x`：强制触发 breeze fallback

**部署检查清单**

如果异地部署后无法运行，按这个顺序检查：

1. `python --version` 是否正常
2. `opencv-python` 和 `mediapipe` 是否已经安装
3. `face_landmarker.task` 是否在项目根目录
4. `run_live.bat` 中的 `MINGW_ROOT` 是否正确
5. `C:\msys64\mingw64\include\SDL2` 和 `C:\msys64\mingw64\lib` 是否存在
6. 摄像头是否能被本机 Python/OpenCV 打开
7. 麦克风是否被 Windows 正常识别

**常见问题**

1. 双击后窗口没起来

先在终端里运行 `.\run_live.bat`，这样能直接看到编译错误或 Python 报错。

2. 编译失败，提示找不到 SDL2

一般是 `MSYS2 / MinGW / SDL2` 路径不对。优先检查 `run_live.bat` 里的 `MINGW_ROOT`。

3. 程序起来了，但没有摄像头输入

先单独运行：

```powershell
python .\python_mediapipe_bridge.py
```

看它是否缺少依赖、打不开摄像头，或者无法读取 `face_landmarker.task`。

4. 程序起来了，但麦克风没有反应

当前版本的麦克风输入来自 C++ 原生 `waveIn`。如果远端机器麦克风权限、驱动或默认录音设备异常，运行时就可能没有有效输入。
