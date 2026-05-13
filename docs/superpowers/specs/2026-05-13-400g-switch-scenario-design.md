# 400G 单交换机 all-to-all 场景平台扩展设计

## 目标

在当前 `ns-3` 工作区内扩展一套可复用的平台能力，使其能够在工程语义上完整承载如下场景，而不是仅靠 `scratch` 层面的临时近似：

1. `5` 节点拓扑：`1` 台交换机 + `4` 台主机；
2. `400Gbps` host-switch 链路，单向传播时延 `4us`；
3. `12` 条同优先级大流在 `40us` 同时启动；
4. 每条流具有独立的初始发送速率；
5. 交换机支持：
   - `ECN` 分段标记曲线：`KMIN=700`、`KMAX=1600`、`PMAX=0.2`
   - `QCN` 风格的发送端速率反馈
   - `PFC` 风格的链路级暂停与恢复
   - 动态 `PFC` 阈值
   - `82MB` 共享缓存语义

本文档的目标不是“尽量近似能跑”，而是定义一条后续实现后可直接支撑论文与复现实验的工程路径。

## 当前平台现状

当前代码库已经具备以下基础：

1. `PointToPointNetDevice` + `TrafficControlHelper` + `QueueDisc` 的标准 ns-3 数据路径；
2. `NetDeviceQueueInterface::Stop/Wake()` 机制，可作为链路级流控的基础；
3. `RedQueueDisc` 与 `QueueDiscItem::Mark()`，可作为 ECN 标记路径的参考；
4. `OnOffApplication`，可直接表达“每条流具有独立初始速率”的发送模型；
5. 现有 `SirdQueueDisc`，但它只有：
   - 单 FIFO 内部队列；
   - 固定阈值 ECN 标记；
   - 满队列才丢包；
   - 没有共享缓存、QCN、PFC、动态阈值能力。

因此，当前平台能做拓扑、链路、负载矩阵和固定速率流量，但还不能完整表达你要求的交换机控制语义。

## 设计原则

本次扩展遵循四个原则：

1. **机制和场景分离**
   交换机控制逻辑放在可复用模块中，`scratch` 场景文件只负责装配。

2. **优先复用 ns-3 既有骨架**
   `ECN` 走 `QueueDisc` 标记路径，`PFC` 走 `NetDeviceQueueInterface::Stop/Wake()` 路径，避免把逻辑硬塞进 `PointToPointNetDevice` 的发送主线。

3. **允许“论文语义完备”，不追求以太网逐比特复刻**
   本次实现追求的是实验语义可解释、反馈回路闭合、参数可控，而不是重新实现一整套工业以太网交换芯片。

4. **先保证单场景跑通，再保证模块可复用**
   设计必须首先服务这次 `4-host/1-switch/12-flow/400G` 场景，但不能把逻辑写死在 `scratch` 里。

## 总体架构

本次扩展拆成四层。

### 1. 交换机队列层

新增一个专用交换机 queue disc，暂定名：

`SwitchSharedBufferQueueDisc`

职责：

1. 维护每个交换机出口的排队状态；
2. 维护全交换机共享缓存使用量；
3. 按 `KMIN/KMAX/PMAX` 执行概率型 ECN 标记；
4. 按 PFC 门限决定某个 ingress 是否需要暂停；
5. 在满足条件时触发 QCN 反馈事件；
6. 输出调试/论文所需 trace：
   - 每端口队列长度
   - 共享缓存占用
   - ECN 标记数
   - PFC 触发与恢复
   - QCN 触发次数

这个模块不负责“发送端如何调速”，只负责检测拥塞状态并产生控制信号。

### 2. 交换机反馈层

新增一个交换机控制器对象，暂定名：

`SwitchCongestionController`

职责：

1. 汇总 queue disc 上报的拥塞事件；
2. 将事件翻译成两类反馈：
   - `PFC pause/resume`
   - `QCN rate feedback`
3. 管理“哪个 ingress port 被暂停”“暂停多久”“什么时候恢复”；
4. 将反馈送达到对应的发送端流控对象。

这层的作用是把“交换机检测”与“发送端反应”解耦。否则后面一旦要调整 QCN 反馈公式，就会把队列代码和发送端代码一起拖乱。

### 3. 发送端速率控制层

新增一个发送端速率控制应用或控制器，暂定名：

`RateControlledFlowApp`

职责：

1. 按给定初始速率持续发送数据；
2. 接收 `QCN` 反馈后降低速率；
3. 在 `PFC pause` 期间停止发送，在 `resume` 后恢复；
4. 可选执行简单的恢复逻辑，使速率在反馈结束后逐步回升；
5. 输出每条流的速率时间序列和完成时间。

之所以不直接改 `OnOffApplication`，是因为这次需要显式接收控制反馈并动态修改发送速率；直接继承或复用其节拍逻辑更合适。

### 4. 场景装配层

新增一个独立场景文件，暂定名：

`scratch/qcn_400g_alltoall.cc`

职责：

1. 构建 `0` 号交换机、`1-4` 号主机；
2. 配置 `400Gbps`、`4us`、`4000B payload`、`82MB buffer`；
3. 安装交换机 queue disc 与控制器；
4. 创建 `12` 条 `RateControlledFlowApp`；
5. 设置每条流的：
   - 源
   - 目的
   - 启动时刻
   - 大小
   - 初始速率
6. 输出结果：
   - 各流 FCT
   - 各流速率轨迹
   - 各出口队列轨迹
   - PFC/QCN/ECN 事件统计

## 关键机制设计

### A. ECN 标记

当前 `SirdQueueDisc` 只有固定阈值 ECN，这不够。

新的交换机 queue disc 需要支持：

1. 当队列占用 `< KMIN` 时，不标记；
2. 当队列占用在 `[KMIN, KMAX)` 时，按线性概率标记；
3. 当队列占用 `>= KMAX` 时，以 `PMAX` 或等效上限概率标记；
4. 若场景需要“更像 DCTCP/RoCE”的行为，可允许在高区间进一步上升到 `1.0`，但默认参数必须忠实表达你给出的 `KMIN/KMAX/PMAX`。

### B. 共享缓存

`82MB` 的语义不能只是“每个端口各自一个 `82MB` 队列上限”，那是错的。

应实现为：

1. 交换机维护全局共享缓存池；
2. 每个出口队列申请共享缓存；
3. 入队前先检查全局剩余缓存；
4. PFC 和丢包判定依赖端口占用与全局占用，而不是单一固定本地队列大小。

这一步是本次“完美实现”的关键。如果没有共享缓存语义，动态 PFC 阈值就没有真实依托。

### C. PFC

这里实现的是“PFC 风格语义”，即：

1. 当某个出口队列超过暂停门限时，暂停与其对应 ingress 路径上的发送；
2. 当该出口队列回落到恢复门限以下时，恢复发送。

工程上不必强行生成真实以太网 pause frame 比特串，但必须满足：

1. pause/resume 有明确触发源；
2. pause/resume 是按 ingress 方向生效，不是全局停机；
3. trace 中可观察到暂停开始与结束时刻。

### D. 动态 PFC 阈值

动态阈值不能只写成一个固定参数名。

最小实现要求：

1. PFC 触发阈值随共享缓存剩余空间变化；
2. 支持每端口在共享缓存充裕时拥有更高可占用空间；
3. 支持在共享缓存趋紧时自动收紧暂停门限。

这一部分可先用一个明确、可解释的函数实现，例如基于当前剩余共享缓存比例调整 `pause threshold`。关键是要把门限和全局缓存状态绑起来，而不是继续沿用静态阈值。

### E. QCN

本次实现不追求工业级 QCN 全细节，但必须闭合“交换机检测拥塞 -> 发送端降速”的回路。

最小完备语义：

1. 交换机在检测到出口持续拥塞时，定位引起拥塞的流或 ingress；
2. 交换机向源端发送一条反馈事件；
3. 源端收到反馈后降低该流发送速率；
4. 在一段时间无新反馈后，源端允许速率逐步恢复。

这就足够支撑论文中“开启 QCN/ECN/PFC 的 400G 小规模 all-to-all 场景”。

## 文件落点

建议新增/修改以下文件：

1. 新增交换机模块
   - `src/traffic-control/model/switch-shared-buffer-queue-disc.h`
   - `src/traffic-control/model/switch-shared-buffer-queue-disc.cc`
   - `src/traffic-control/model/switch-congestion-controller.h`
   - `src/traffic-control/model/switch-congestion-controller.cc`
   - `src/traffic-control/wscript`

2. 新增发送端流量/调速模块
   - `src/applications/model/rate-controlled-flow-app.h`
   - `src/applications/model/rate-controlled-flow-app.cc`
   - `src/applications/wscript`

3. 新增测试
   - `src/traffic-control/test/switch-shared-buffer-queue-disc-test-suite.cc`
   - `src/applications/test/rate-controlled-flow-app-test-suite.cc`
   - 对应 `wscript`

4. 新增场景
   - `scratch/qcn_400g_alltoall.cc`

5. 新增文档
   - `docs/qcn_400g_alltoall_paper_writeup_zh.md`

## 预期输出

实现完成后，平台应至少能稳定输出：

1. `12` 条流的 FCT；
2. 每条流的瞬时发送速率轨迹；
3. 每个交换机出口队列长度轨迹；
4. 全局共享缓存占用轨迹；
5. ECN 标记统计；
6. PFC pause/resume 时间线；
7. QCN 反馈次数与每条流的降速事件。

## 风险

### 风险 1：QCN 粒度

若交换机侧难以准确把拥塞归因到单条流，可以先按 `ingress port` 粒度反馈，而不是按五元组或 socket 粒度。

### 风险 2：PFC 与 QCN 相互作用

如果两套机制同时触发，发送端可能出现“刚恢复就再降速”的抖动。实现时需要给出明确优先级：

1. `PFC pause` 优先于正常发包；
2. `QCN` 只影响恢复后的发送速率上限。

### 风险 3：完全工业复刻并非必要

这次目标是“场景机制完备且可解释”，不是复刻某款交换芯片。只要共享缓存、动态阈值、PFC、QCN、ECN 这五个回路都闭合，论文表达就是站得住的。

## 结论

要让这个 `400G` 小规模 all-to-all 场景“完美运行”，必须扩展平台能力，而不是只在 `scratch` 中拼参数。合理的工程路径是：

1. 以 `QueueDisc` 为核心新增交换机共享缓存与拥塞检测模块；
2. 以 `NetDeviceQueueInterface::Stop/Wake()` 为基础实现 `PFC` 风格暂停；
3. 新增发送端 rate controller，实现 `QCN` 反馈闭环；
4. 再在独立场景文件中装配 `5` 节点、`12` 流、`400G` 参数。

这样实现后，这个场景不仅能跑，而且后续还能作为新的平台能力被别的实验复用。
