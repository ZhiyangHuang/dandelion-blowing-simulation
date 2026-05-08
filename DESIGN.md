自定义真实多线程项目
DandelionOS
Real-Time User-Level Thread Scheduler with Visualized Particle Interaction

一、系统整体定位
这是一个：
用户态实时线程调度系统（User-Level Real-Time Scheduler）
外层套一个：
吹蒲公英交互可视化壳（Interactive Rendering Shell）
核心不是小游戏。
本质是：
用可视化粒子系统实时展示线程调度、抢占、中断、恢复、RR时间片轮转。

二、系统总架构
系统分为五层：
┌─────────────────────────────┐
│ UI Interaction Layer        │
│ (Buttons / User Events)     │
└──────────────┬──────────────┘
              ↓
┌─────────────────────────────┐
│ Task Submission Layer       │
│ Create / Queue / Replace    │
└──────────────┬──────────────┘
              ↓
┌─────────────────────────────┐
│ Scheduler Core              │
│ Priority + Preemption + RR  │
└──────────────┬──────────────┘
              ↓
┌─────────────────────────────┐
│ Worker Threads              │
│ Execute / Resume / Yield    │
└──────────────┬──────────────┘
              ↓
┌─────────────────────────────┐
│ RenderData Shared State     │
└──────────────┬──────────────┘
              ↓
┌─────────────────────────────┐
│ Renderer (Read Only)        │
└─────────────────────────────┘

三、核心设计原则

1. 渲染读写隔离
线程：
只能写 RenderData
Renderer：
只能读 RenderData
绝不允许：
Renderer -> 修改粒子状态
Renderer -> 修改 power
Renderer -> 修改 mouth 坐标
这是整个系统稳定性的基础。

2. 所有共享资源必须互斥
任何线程访问共享状态必须加锁。

3. 调度权高于任务执行权
任务永远不能“霸占线程”。
每帧调度器重新决定：
谁运行
谁挂起
谁恢复
谁结束

4. 抢占必须可恢复
只有：
supportResume == true
允许被中断后恢复。
否则：
直接 FINISHED。

四、全局共享状态

4.1 蒲公英世界状态
static float dandelionX;
static float dandelionY;

static int remainingParticles = 100;

static float power = 0.1f;
作用：
dandelionX / dandelionY
蒲公英中心坐标

remainingParticles
剩余附着粒子数
范围：
0 ~ 100

power
吹气力度
范围：
0.1 ~ 10
换算：
velocity = power * 100 px/s

4.2 嘴部检测状态
static float mouthX;
static float mouthY;
来源：
CameraTask
用途：
计算吹气方向与强度。

4.3 渲染共享结构
struct RenderData
{
   BackgroundLayer skyGrassLayer;

   CameraLayer cameraLayer;

   WindLayer windLayer;

   std::vector<ParticleRenderData> particles;

   UIRenderData ui;
};

图层职责

Layer 1：Background
静态：
天空
草地

Layer 2：Camera
动态：
摄像头画面
嘴部检测框

Layer 3：Wind
动态：
风线
风强颜色变化

Layer 4：Particle
动态：
附着粒子
漂浮粒子

Layer 5：UI
动态：
线程状态
粒子数量
power
设备状态

4.4 Mutex系统

particleMutex
保护：
粒子坐标
粒子生命周期
remainingParticles

powerMutex
保护：
power

renderMutex
保护：
整个：
RenderData

锁顺序必须固定：
particleMutex
→ powerMutex
→ renderMutex
避免死锁。
这是实现时必须严格遵守的。

五、全局帧率控制

const int SYSTEM_FPS = 60;
const double FRAME_TIME = 1.0 / SYSTEM_FPS;

每轮调度：
sleep_until(next_frame);
作用：

1. CPU节流
避免 busy wait。

2. RR统一时间片
所有粒子任务：
16ms
与：
60 FPS
同步。

3. 动画稳定
保证视觉连续。

六、线程系统

6.1 ThreadState
enum ThreadState
{
   IDLE,
   RUNNING,
   SLEEPING,
   CLOSED,
   WAITING
};

IDLE
线程空闲，可调度。

RUNNING
执行任务。

SLEEPING
线程存在，但不参与调度。
用于线程模式切换。

CLOSED
线程未创建 / 已销毁。

WAITING
阻塞等待资源。

6.2 线程显示逻辑

运行时：
Thread 2 running task CameraTask
Type: Priority 2

状态切换：
Thread 3 -> SLEEPING

6.3 线程模式

单线程模式
Thread1 = RUNNING / IDLE
Thread2 = SLEEPING / CLOSED
Thread3 = SLEEPING / CLOSED

双线程模式
Thread1 = RUNNING / IDLE
Thread2 OR Thread3 = RUNNING / IDLE
另一个 SLEEPING / CLOSED

三线程模式
全部活跃。

七、任务系统

7.1 TaskState
enum TaskState
{
   CREATED,
   RUNNING,
   INTERRUPTED,
   FINISHED,
   REQUEUED
};

7.2 Task接口
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

八、优先级调度体系

Priority 1 — System Critical
最高级。
可抢占：
P2
P3
结构：
LIFO
容量：
max = 2

覆盖规则
只检查栈顶。
若类型相同：
旧任务：
FINISHED
新任务覆盖。

Priority 2 — Functional Tasks
中优先级。
可抢占：
P3
不可抢占：
P1
结构：
FIFO
容量：
max = 5

去重规则
检查队首。
若相同：
替换。

若队列为空：
自动加入：
BatchParticleExecutionTask
这是保持系统持续动画推进的关键。

Priority 3 — Particle Tasks
最低优先级。
只能 RR。
结构：
FIFO
容量：
100
时间片：
RR_QUANTUM_MS = 16

九、调度器核心

9.1 调度顺序
固定：
P1
↓
P2
↓
P3

9.2 抢占规则

P1进入
立即打断：
P2
P3

P2进入
只能打断：
P3

P3
不可主动抢占。
仅轮转。

9.3 同优先级抢占
你定义的是：
到时间打断优先级最低的任务
 若同级，打断第一个线程
整理后：
if (quantumExpired)
{
   preempt(lowestPriorityRunningTask);

   if (tie)
       preempt(firstScheduledThread);
}

9.4 resume机制

若：
supportResume == true
保存上下文：
TaskContext
重新入队。

否则：
FINISHED

十、系统启动流程

StartTask（P1）

Phase 1：线程初始化
根据硬件能力：

3线程
T1 RUNNING
T2 SLEEPING
T3 SLEEPING

2线程
T1 RUNNING
T2 SLEEPING
T3 CLOSED

1线程
T1 RUNNING
T2 CLOSED
T3 CLOSED

Phase 2：创建图层
按顺序：
skyGrassLayer
cameraLayer
windLayer
particleLayer

Phase 3：UI按钮

第一栏
Camera Toggle

第二栏
Microphone Toggle

第三栏
Reset
Change Dandelion
Breeze

第四栏
线程 + / -

第五栏
Exit

Phase 4：提交ResetTask
push(P1, ResetTask)

十一、ResetTask（P1）

Step 1 清空系统
打断全部线程。
若任务不可中断：
线程进入 WAITING。

清空：
P2队列（保留运行中的CameraTask）
P3队列

Step 2 初始化变量
remainingParticles = 100;
power = 0.1f;
mouth = dandelion center;

Step 3 重建100粒子
圆形均匀分布。

Step 4 更新UI
显示：
线程状态
剩余粒子
Camera
Microphone
Mouth
Power

Step 5 自动恢复
若无CameraTask：
加入：
CameraTask

十二、功能任务链（P2）
这是系统的主功能流水线。

CameraTask
功能：（只有这里使用mediapipe完成，这是python任务，其他的是C++）
检测嘴部。直到嘴巴张开结束。

运行：
更新：
mouthX
mouthY
cameraLayer

若Camera不可用：
切换：
MicrophoneTask

结束：
加入：
MicrophoneTask

队列约束：
CameraTask 必须在 MicrophoneTask 前

MicrophoneTask
功能：(内部设置一个状态，当任务开启时状态第一次就保持，直到麦克风第一次检查到声音，状态关闭方便后续声音结束后任务消失。)
生成 power

计算：
power = f(volume, direction)
限制：
0.1 ≤ power ≤ 10

视觉：
音量越强：
风线越深。

若麦克风不可用：
切：
BreezeTask

结束：
加入：
GenerateParticleTask

队列规则：
不能在 CameraTask 前。

GenerateParticleTask
作用：
批量创建 P3。

流程：
1
检查队列容量

2
根据 power：
count = floor(power * k)

3
生成多个 SingleParticleTask

4
更新渲染

BreezeTask
降级模拟。

固定：
power = 5
生成单粒子。

ChangeDandelionTask
重新生成蒲公英。

重建：
100粒子。

若系统空闲：
查询所有线程。
若无CameraTask：
加入：
CameraTask

十三、粒子执行体系

BatchParticleExecutionTask
作用：
统一推进世界时间。

首次：
记录：
lastTime

每秒：
power -= 0.01
直到：
power >= 0.1

结束：
不保存局部时间。
因为它是世界tick。

SingleParticleTask（P3）
每个粒子独立任务。这是写入任务，将坐标写入任务状态里，给渲染任务读取。

局部：
float x;
float y;

运行：
deltaTime = 16ms
distance = velocity * deltaTime

边界：
若超窗体：
FINISHED
删除渲染。

被打断：
保存：
x,y
重新入队尾。

十四、线程模式切换
注意：
你这里设计得很对。
不是销毁线程。
是：
降级为 SLEEPING
这是用户态线程池正确做法。

一线程模式

停止：
T2 T3

保存任务顺序：
先T3
后T2
压回队列。

状态：
T2 -> SLEEPING
T3 -> SLEEPING

两线程模式
停止：
T3

保存任务。

状态：
T3 -> SLEEPING
T2 -> IDLE

三线程模式
恢复：
T2 -> IDLE
T3 -> IDLE

十五、退出任务

ExitTask（P1）
执行：
shutdown();

释放：
所有线程
所有mutex
RenderData
Device handles

十六、完整系统生命周期
StartTask
  ↓
ResetTask
  ↓
CameraTask
  ↓
MicrophoneTask
  ↓
GenerateParticleTask / BreezeTask
  ↓
BatchParticleExecutionTask
  ↓
SingleParticleTasks RR
期间：
任何时刻：
Priority1 可抢占全部

十七、运行时真实行为（一次吹气）
用户吹气：
↓
CameraTask检测嘴
↓
MicrophoneTask算power
↓
GenerateParticleTask创建粒子
↓
多个SingleParticleTask入P3
↓
调度器RR轮转
↓
粒子飞散
↓
BatchTask衰减power
↓
恢复CameraTask监听下一次吹气

系统循环链条
Start
↓
Reset
↓
Scheduler Loop
   ├── Camera (resident)
   ├── Mic (resident)
   ├── Batch (daemon)
   ├── Generate (event)
   └── Particle RR


十八、系统本质总结
这套系统完整实现后，本质是：

1
用户态线程调度器
包含：
抢占
RR
resume
queue replacement

2
实时共享状态同步系统
包含：
mutex
render isolation
lock ordering

3
可视化OS模拟器
线程状态直接映射到动画。

4
粒子驱动任务计算平台
每个粒子 = 独立可调度任务
