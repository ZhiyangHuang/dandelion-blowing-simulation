# DandelionOS

Real-Time User-Level Thread Scheduler with Visualized Particle Interaction

## 0. Canonical Rule

本文件是当前唯一有效设计。

旧版本、重复段落、互相冲突的描述一律以本文件靠前且更明确的规则为准。
代码实现如果与本文件冲突，应以后续实现修正代码为目标，而不是反过来修改设计去迁就代码。

这次整理后的重点只聚焦三条主线：

1. 摄像头任务
2. 麦克风任务
3. 粒子任务

线程、UI、历史实现细节都服从这三条主线。

---

## 1. System Goal

DandelionOS 不是小游戏。
它是一个用户态实时线程调度系统，外层包一层“吹蒲公英”的可视化交互壳。

系统要表达的核心是：

- 任务创建
- 排队
- 顺序消费
- 抢占
- 中断
- 恢复
- RR 粒子推进

其中最重要的真实交互链是：

`CameraTask -> MicrophoneTask -> GenerateParticleTask / BreezeTask -> BatchParticleExecutionTask -> SingleParticleTask(P3...)`

这条链一旦开始，就应该向前消费，不应该在中途回头重新开启 CameraTask。

---

## 2. Core Principles

### 2.1 RenderData Read/Write Isolation

- 任务线程可以写 `RenderData`
- Renderer 只能读 `RenderData`
- Renderer 绝不能修改：
  - 粒子状态
  - `power`
  - `mouthX / mouthY`

### 2.2 Shared State Must Be Locked

所有共享状态访问必须加锁。

锁顺序固定为：

`particleMutex -> powerMutex -> renderMutex`

### 2.3 Scheduling Has Higher Priority Than Execution

任务不能无限霸占线程。
调度器每帧都可以重新决定：

- 谁运行
- 谁等待
- 谁恢复
- 谁结束

### 2.4 Only Resumable Tasks Can Be Interrupted

只有 `supportResume == true` 的任务允许被中断后恢复。
否则直接 `FINISHED`。

---

## 3. Global Shared State

### 3.1 World State

```cpp
static float dandelionX;
static float dandelionY;

static float mouthX;
static float mouthY;

static int remainingParticles = 100;
static float power = 0.1f;
```

含义：

- `dandelionX / dandelionY`
  - 蒲公英中心坐标
- `mouthX / mouthY`
  - 当前嘴部中心坐标
- `remainingParticles`
  - 还附着在蒲公英上的粒子数
- `power`
  - 当前吹气强度，范围 `0.1 ~ 10`

速度换算规则固定为：

`velocity = power * 100 px/s`

### 3.2 Device Availability State

设备可用性不是“每一帧重新发明”的。

系统在初始化阶段要完成一次设备探测，形成两个稳定标志：

```cpp
cameraDeviceAvailable
microphoneDeviceAvailable
```

规则：

- 初始化时探测到摄像头可用，则本轮运行中它就是“设备可用”
- 初始化时探测到麦克风可用，则本轮运行中它就是“设备可用”
- 某一帧没有拿到样本，不等于设备不可用
- 样本 stale、没检测到脸、没检测到声音，只是“本帧输入无效”，不是设备消失

也就是说：

- `device available` 是启动时或 reset 时的能力判断
- `sample ready / face detected / voice detected` 是运行时样本判断

这两层状态必须分开。

### 3.3 RenderData

```cpp
struct RenderData
{
    BackgroundLayer backgroundLayer;
    CameraLayer cameraLayer;
    WindLayer windLayer;
    std::vector<ParticleRenderData> particles;
    UIRenderData ui;
};
```

图层职责：

- `BackgroundLayer`
  - 天空
  - 草地
- `CameraLayer`
  - 摄像头画面或摄像头状态
  - 嘴部框/嘴部标记
- `WindLayer`
  - 风线
  - 风强变化
- `ParticleLayer`
  - 附着粒子
  - 漂浮粒子
- `UI`
  - 线程状态
  - 队列状态
  - 粒子数量
  - 设备状态
  - 当前输入焦点

---

## 4. Thread Model

### 4.1 ThreadState

```cpp
enum ThreadState
{
    IDLE,
    RUNNING,
    SLEEPING,
    CLOSED,
    WAITING
};
```

### 4.2 Thread Mode

默认模式必须是单线程：

- `Thread1 = RUNNING / IDLE`
- `Thread2 = SLEEPING / CLOSED`
- `Thread3 = SLEEPING / CLOSED`

双线程模式：

- `Thread1 = RUNNING / IDLE`
- `Thread2 or Thread3 = RUNNING / IDLE`
- 另一个 `SLEEPING / CLOSED`

三线程模式：

- 三个线程都活跃

注意：

线程数量变化只影响同时可执行的 worker 数量。
任务链的逻辑顺序不因为线程数变化而改写。

---

## 5. Task Model

### 5.1 TaskState

```cpp
enum TaskState
{
    CREATED,
    RUNNING,
    INTERRUPTED,
    FINISHED,
    REQUEUED
};
```

### 5.2 Task Interface

```cpp
struct Task
{
    int id;
    TaskType type;
    PriorityLevel priority;
    TaskState state;
    bool supportResume;

    virtual void execute() = 0;
    virtual void resume() = 0;
};
```

---

## 6. Queue and Priority Rules

### 6.1 Priority

- `P1`: system critical
- `P2`: functional chain tasks
- `P3`: particle tasks

调度顺序永远是：

`P1 -> P2 -> P3`

### 6.2 Preemption

- `P1` 可打断 `P2` 和 `P3`
- `P2` 只能打断 `P3`
- `P3` 不主动打断任何高优先级任务，只做 RR

### 6.3 P3 RR

```cpp
const int RR_QUANTUM_MS = 16;
```

每个 `SingleParticleTask` 每次只推进一个 RR 时间片。

---

## 7. Canonical Input and Particle Pipeline

这是本文件最重要的部分。

### 7.1 Initialization Rule

启动或 Reset 完成后，要先探测设备能力，然后按下面规则只放入一个输入起点：

#### Case A: Camera available

初始化只开启：

`CameraTask`

#### Case B: No camera, microphone available

初始化不要让所有线程卡在等待 CameraTask。
应直接开启：

`MicrophoneTask`

#### Case C: Neither camera nor microphone available

不自动创建 `CameraTask`。
系统保持空闲，等待显式用户事件或后续 reset/restart 触发。

### 7.2 One-Way Consumption Rule

从输入开始后的功能链必须是单向消费：

`CameraTask -> MicrophoneTask -> GenerateParticleTask / BreezeTask -> BatchParticleExecutionTask -> SingleParticleTask...`

规则：

- 一旦链条开始向后推进，就不回头重新插入 `CameraTask`
- `CameraTask` 不是永久 resident 循环任务
- `MicrophoneTask` 不是永久 resident 循环任务
- `BatchParticleExecutionTask` 不是“永远在 P2 里自转”的任务

它们都是这条链上的阶段性任务。

### 7.3 Reopen CameraTask Rule

重新开启 `CameraTask` 的唯一方法是：

`P1 == 0 && P2 == 0 && P3 == 0`

也就是三个优先级队列全部为空时，系统才允许重新回到输入起点。

这条规则是硬约束。

不允许：

- 粒子还没清空时重新开 `CameraTask`
- `BatchParticleExecutionTask` 还没结束时回头开 `CameraTask`
- 正在 `MicrophoneTask` 链中途时回头插入 `CameraTask`

---

## 8. CameraTask

### 8.1 Responsibility

`CameraTask` 负责：

- 获取摄像头样本状态
- 更新 `mouthX / mouthY`
- 判断嘴是否有效打开
- 决定是否把链条交给 `MicrophoneTask`

### 8.2 Availability Semantics

`CameraTask` 要区分三类状态：

1. 设备不可用
2. 设备可用但当前样本无效
3. 设备可用且当前样本有效

其中：

- 设备不可用：来自初始化能力探测
- 样本无效：比如没脸、闭嘴、没正视、样本 stale

### 8.3 Lifecycle

如果 `cameraDeviceAvailable == true`：

- `CameraTask` 启动后持续消费自己的阶段
- 它的结束条件来自内部判断，不来自外层“随机轮到别的 resident 任务”
- 只有当嘴部打开条件成立，或者确认要降级到麦克风路径时，它才结束并交棒

### 8.4 Output Rule

`CameraTask` 结束后只能做一件事：

- 推入 `MicrophoneTask`

不能同时回头再给自己排一个新的 `CameraTask`。

---

## 9. MicrophoneTask

### 9.1 Responsibility

`MicrophoneTask` 负责：

- 获取麦克风样本
- 计算 `power`
- 判断是正常吹气、fallback，还是没有有效声音

### 9.2 Availability Semantics

和摄像头一样，要分清：

1. 设备不可用
2. 设备可用但本帧样本无效
3. 设备可用且本帧样本有效

### 9.3 Lifecycle

`MicrophoneTask` 不是无限 resident 监听任务。

它的正确行为是：

- 开始后锁住自己的阶段
- 保持状态直到第一次检测到有效声音或 fallback 条件
- 一旦拿到本轮要消费的输入，就结束自己，把控制交给下游任务

也就是说：

`MicrophoneTask` 的打断点必须来自它内部的“第一次有效输入”方程，而不是来自外部 resident 轮转。

### 9.4 Output Rule

麦克风阶段结束后：

- 如果有正常声音，推入 `GenerateParticleTask`
- 如果麦克风不可用或只满足 fallback 条件，推入 `BreezeTask`
- 如果没有有效输入，则本轮链条结束，不回头自动重开 `CameraTask`

因为重新开启 `CameraTask` 只能等到：

`P1 == 0 && P2 == 0 && P3 == 0`

---

## 10. GenerateParticleTask and BreezeTask

### 10.1 GenerateParticleTask

职责：

- 根据 `power` 计算生成数量
- 创建多个 `SingleParticleTask`
- 更新粒子附着/脱离状态

流程：

1. 检查 `P3` 容量
2. 根据 `power` 计算本轮数量
3. 为每个粒子创建 `SingleParticleTask`
4. 推入 `BatchParticleExecutionTask`

### 10.2 BreezeTask

职责：

- 作为降级路径
- 固定 `power = 5`
- 创建一个单粒子任务
- 然后也要进入 `BatchParticleExecutionTask`

### 10.3 No Backtracking

无论是 `GenerateParticleTask` 还是 `BreezeTask`，都只能继续往后推。

不能：

- 重新开 `CameraTask`
- 重新开 `MicrophoneTask`
- 在粒子链没清空时回到输入阶段

---

## 11. BatchParticleExecutionTask

### 11.1 Responsibility

`BatchParticleExecutionTask` 是世界级粒子推进任务。

它负责：

- 记录 world tick 的起始时间
- 更新本轮 world delta
- 推动粒子阶段向 `P3` RR 执行
- 控制 `power` 的时间衰减
- 清理完成后的粒子可视状态

### 11.2 Lifecycle

它在输入链之后启动。

它的定位不是永久常驻任务，而是这轮粒子系统的批处理调度入口。

只有在本轮粒子任务全部完成后，它才结束。

### 11.3 Decay Rule

每秒：

`power -= 0.01`

下限为：

`0.1`

### 11.4 Output Rule

`BatchParticleExecutionTask` 不回头开启输入任务。

它只向后服务于：

`SingleParticleTask(P3...)`

等 `P3` 清空之后，整个系统才允许回到输入起点。

---

## 12. SingleParticleTask (P3)

### 12.1 Responsibility

每个粒子是一个独立的 `P3` 任务。

职责：

- 保存自己的局部坐标
- 保存自己的飞行方向
- 每次 RR 推进一个时间片
- 将结果写回粒子共享状态与渲染层

### 12.2 Movement Rule

每次运行：

- `deltaTime = 16 ms`
- `distance = velocity * deltaTime`
- `velocity = power * 100 px/s`

粒子每次只推进一个时间片的位移。

### 12.3 Finish Rule

如果粒子移动到窗口边缘：

- 该粒子任务 `FINISHED`
- 从 `P3` 中移除
- 粒子可视记录进入清理流程

不允许再回头触发输入任务。

---

## 13. Reset Rule

`ResetTask(P1)` 做的事情：

1. 清空当前任务链
2. 重置全局坐标和粒子状态
3. 重新探测设备能力
4. 按初始化规则选择新的输入起点

Reset 后的起点规则仍然是：

- 有摄像头：从 `CameraTask` 开始
- 无摄像头但有麦克风：从 `MicrophoneTask` 开始
- 两者都无：保持空闲

---

## 14. SDL Visualization Requirements

可视化必须清楚展示以下内容：

### 14.1 Device State

- Camera device available / unavailable
- Camera sample state
- Microphone device available / unavailable
- Microphone sample state

### 14.2 Input Focus

必须显式显示当前输入焦点：

- `CAMERA FOCUS`
- `MIC FOCUS`
- `GATE OPEN`
- `FREE FOCUS`

### 14.3 Queue State

- `P1` 数量
- `P2` 数量
- `P3` 数量

### 14.4 Particle State

- 剩余附着粒子数
- 当前排队粒子任务数
- 飞行中的粒子
- 边界清理状态

---

## 15. Non-Negotiable Rules

下面这些是当前版本最重要、最不能再被旧段落改写的规则：

1. 初始化探测到摄像头可用，就把它视为本轮运行中设备可用；麦克风同理。
2. 没有摄像头时，不允许让所有线程在等待 CameraTask 的状态下空转；必须直接从 `MicrophoneTask` 开始。
3. `CameraTask -> MicrophoneTask -> BatchParticleExecutionTask` 属于顺序消费链，推进后不回头。
4. `GenerateParticleTask / BreezeTask` 只能向后推动粒子链，不能回头重新开输入任务。
5. 重新开启 `CameraTask` 的唯一条件是：`P1 == 0 && P2 == 0 && P3 == 0`。
6. 摄像头和麦克风阶段的结束/切换，应由任务内部条件决定，而不是由外层 resident 循环碰运气决定。

---

## 16. Canonical Execution Summary

最简完整流程如下：

### Startup

- 探测 camera / microphone device availability
- 选择输入起点

### Input Phase

- 如果有 camera：`CameraTask`
- 否则如果有 microphone：`MicrophoneTask`

### Blow Phase

- `CameraTask` 成功后进入 `MicrophoneTask`
- `MicrophoneTask` 成功后进入：
  - `GenerateParticleTask`
  - 或 `BreezeTask`

### World Phase

- `BatchParticleExecutionTask`
- `SingleParticleTask...` in `P3 RR`

### Restart Condition

只有当：

`P1 == 0 && P2 == 0 && P3 == 0`

才允许重新回到 `CameraTask` 作为下一轮输入起点。
