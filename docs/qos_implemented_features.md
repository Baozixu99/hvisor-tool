# QoS 已实现功能摘要（按客户端/服务端）

本文仅总结已确认落地并在代码中实现的功能，适用于系统文档与论文引用。

---

## 快速开始 (重要!)

### 命令对照表
```bash
# ❌ 错误 - 使用普通服务端,无法处理QoS消息
./hvisor shm hyper_amp_service shm_config.json

# ✅ 正确 - 使用QoS感知服务端
./hvisor shm hyper_amp_qos_service shm_config.json

# ✅ 正确 - 使用QoS感知客户端
./hvisor shm hyper_amp_qos shm_config.json "hello" 1
```

### 典型部署场景 (双 Linux Zone)
```bash
# Non-Root Linux (Zone 1) - 服务端
cd /home/arm64
./hvisor shm hyper_amp_qos_service shm_config.json

# 等待看到以下关键日志（确认服务端就绪）:
# [QoS-Server] Message queue initialized (working_mark=0xeeeeeeee, ...)
# [QoS-Server] QoS controller initialized (1MB, 64 batch)
# [QoS-Server] Waiting for requests...

# Root Linux (Zone 0) - 客户端
cd /home/arm64
./hvisor shm hyper_amp_qos shm_config.json "test_message" 66
```

### 常见错误避免
1. **服务端/客户端必须配对使用**
   - QoS客户端 (`hyper_amp_qos`) → QoS服务端 (`hyper_amp_qos_service`)
   - 普通客户端 (`hyper_amp`) → 普通服务端 (`hyper_amp_service`)

2. **配置文件必须一致**
   - 两个 Zone 使用同一份 `shm_config.json`

3. **服务 ID 必须有效**
   - 当前支持: 1(加密), 2(解密), 66(Echo)

---

## 完整测试说明

### 测试目标
验证 QoS 系统的核心功能:
1. **优先级分类**: 消息按 REALTIME/THROUGHPUT/RELIABLE/BEST_EFFORT 正确分类
2. **WRR 调度**: 按权重 4:2:2:1 进行差异化调度
3. **防饥饿**: 低优先级在高负载下仍能获得服务
4. **资源预留**: 关键类别在资源紧张时优先使用保留配额
5. **统计观测**: 准确输出各类队列与处理统计

### 环境准备

#### 1. 构建系统
```bash
# 在项目根目录
cd /home/b/ft/hvisor-tool
make -C tools clean
make -C tools
```

#### 2. 准备配置文件
使用或修改 `examples/` 下的共享内存配置 JSON（如 `examples/qemu-aarch64/with_virtio_blk_console/` 中的配置），确保:
- 共享内存区域已正确映射
- 服务端与客户端使用同一份配置
- 如有多个测试实例，确认资源配额足够

#### 3. 检查设备状态
```bash
# 检查事件设备是否存在
ls -l /dev/hshm0

# 若不存在,系统将自动退化为轮询模式(功能正常,延迟会略高)
```

### 测试用例设计

#### 测试 1: 基础连通性与分类验证

**目的**: 验证四个优先级类别均能正常工作

**步骤**:
```bash
# 终端1 (Non-Root Linux): 启动 QoS 感知服务端
cd /home/arm64
./hvisor shm hyper_amp_qos_service shm_config.json

# 终端2 (Root Linux): 分别发送不同优先级的请求
# 注意: 服务ID与QoS类别的映射关系在 tools/shm/qos.c 的 default_qos_profiles 中定义

# REALTIME 类别 - 加密/解密服务 (service_id=1 或 2)
./hvisor shm hyper_amp_qos shm_config.json "realtime_test" 1

# THROUGHPUT 类别 - 如果配置文件有其他服务映射
./hvisor shm hyper_amp_qos shm_config.json "throughput_test" 2

# BEST_EFFORT 类别 - Echo服务 (service_id=66)
./hvisor shm hyper_amp_qos shm_config.json "best_effort_test" 66

# 注意: 务必使用 hyper_amp_qos_service (而非 hyper_amp_service)
# 否则服务端无法识别 QoS 消息,客户端会超时
```

**预期结果**:
- 所有请求均收到响应
- 服务端日志显示 Phase1 分类、Phase2 调度、Phase3 清理，例如:
  ```
  [QoS-Phase1] Collected X messages
  [QoS-Phase2] Scheduled message from REALTIME queue
  [QoS-Phase2] Processing service_id=1 (ENCRYPT)
  [QoS-Phase3] Cleanup complete
  ```
- 客户端显示成功响应和延迟统计
- 统计输出显示四类队列均有计数

**实际测试验证**:
根据提供的日志,客户端已正确:
- ✅ 分类消息到对应 QoS 类别
- ✅ 分配资源 (从 reserved buffer)
- ✅ 写入共享内存并发送

**注意**: 如果看到超时错误,请检查:
1. Non-Root Linux 是否启动了 `hyper_amp_qos_service` (而非 `hyper_amp_service`)
2. 服务端日志是否显示收到消息并处理

#### 测试 2: WRR 权重验证

**目的**: 验证按权重 4:2:2:1 进行调度

**步骤**:
```bash
# 创建测试脚本 test_wrr.sh
cat > test_wrr.sh << 'EOF'
#!/bin/bash
CONFIG="shm_config.json"

# 同时发送大量不同优先级请求
for i in {1..100}; do
    ./hvisor shm hyper_amp_qos $CONFIG "rt_$i" 1 &      # REALTIME (加密服务)
done

for i in {1..100}; do
    ./hvisor shm hyper_amp_qos $CONFIG "tp_$i" 2 &     # REALTIME (解密服务)
done

for i in {1..100}; do
    ./hvisor shm hyper_amp_qos $CONFIG "be_$i" 66 &    # BEST_EFFORT (Echo)
done

wait
EOF

chmod +x test_wrr.sh

# 注意: 先在另一终端启动QoS服务端
# ./hvisor shm hyper_amp_qos_service shm_config.json

./test_wrr.sh
```

**预期结果**:
- 服务端统计显示各类处理次数比例接近 4:2:2:1
- REALTIME 消息的平均延迟 < THROUGHPUT < BEST_EFFORT
- 查看服务端日志中 Phase2 调度顺序,应体现权重差异

#### 测试 3: 防饥饿验证

**目的**: 验证低优先级在高压下仍有进度

**步骤**:
```bash
# 创建高压测试脚本 test_starvation.sh
cat > test_starvation.sh << 'EOF'
#!/bin/bash
CONFIG="shm_config.json"
DURATION=60  # 测试60秒

end_time=$(($(date +%s) + DURATION))

# 持续发送高优先级请求
while [ $(date +%s) -lt $end_time ]; do
    for i in {1..10}; do
        ./hvisor shm hyper_amp_qos $CONFIG "flood_rt" 1 &
    done
    sleep 0.1
done &
FLOOD_PID=$!

# 每5秒发送一个低优先级请求并记录时间
for i in {1..12}; do
    echo "[$(date +%s.%N)] Sending BEST_EFFORT request $i"
    ./hvisor shm hyper_amp_qos $CONFIG "be_check_$i" 66
    echo "[$(date +%s.%N)] BEST_EFFORT request $i completed"
    sleep 5
done

# 停止高优先级洪泛
kill $FLOOD_PID
wait
EOF

chmod +x test_starvation.sh
./test_starvation.sh 2>&1 | tee starvation_test.log
```

**预期结果**:
- 所有 BEST_EFFORT 请求最终都能完成(无超时/丢弃)
- 服务端统计显示 BEST_EFFORT 处理计数持续增长
- 日志显示每个 BEST_EFFORT 请求均在可接受时间内(如 <5秒)完成

#### 测试 4: 资源预留验证

**目的**: 验证资源紧张时预留机制的保护效果

**前置条件**: 在 `tools/shm/qos.c` 的 `qos_init_impl()` 中设置较小的总资源和明确的预留比例

**步骤**:
```bash
# 修改 qos.c 设置小容量(如 total_buffer=1000, 各类reserved分别为400/200/200/100)
# 重新编译
make -C tools

# 创建资源压力测试脚本 test_reservation.sh
cat > test_reservation.sh << 'EOF'
#!/bin/bash
CONFIG="shm_config.json"

# 同时发送超过资源上限的大消息请求
for i in {1..50}; do
    # 大消息(16KB)快速消耗资源
    dd if=/dev/urandom bs=16384 count=1 2>/dev/null | \
    ./hvisor shm hyper_amp_qos $CONFIG @- 1 &      # REALTIME
done

for i in {1..50}; do
    dd if=/dev/urandom bs=16384 count=1 2>/dev/null | \
    ./hvisor shm hyper_amp_qos $CONFIG @- 66 &     # BEST_EFFORT
done

wait
EOF

chmod +x test_reservation.sh
./test_reservation.sh 2>&1 | tee reservation_test.log
```

**预期结果**:
- REALTIME 消息成功率 > BEST_EFFORT 成功率
- 服务端日志可能显示部分 BEST_EFFORT 入队失败(资源不足)
- 统计输出显示 REALTIME 使用了其保留配额

#### 测试 5: 端到端性能基准测试

**目的**: 建立性能基线,对比不同配置的影响

**步骤**:
```bash
# 创建性能测试脚本 test_performance.sh
cat > test_performance.sh << 'EOF'
#!/bin/bash
CONFIG="shm_config.json"
NUM_REQUESTS=1000

echo "=== Performance Baseline Test ==="
echo "Configuration: $CONFIG"
echo "Total requests per class: $NUM_REQUESTS"
echo ""

# 测试每个优先级类别
for SERVICE in 1 2 66; do
    case $SERVICE in
        1) CLASS="REALTIME" ;;
        2) CLASS="THROUGHPUT" ;;
        66) CLASS="BEST_EFFORT" ;;
    esac
    
    echo "Testing $CLASS (service_id=$SERVICE)..."
    START=$(date +%s.%N)
    
    for i in $(seq 1 $NUM_REQUESTS); do
        ./hvisor shm hyper_amp_qos $CONFIG "perf_test_$i" $SERVICE > /dev/null
    done
    
    END=$(date +%s.%N)
    DURATION=$(echo "$END - $START" | bc)
    THROUGHPUT=$(echo "scale=2; $NUM_REQUESTS / $DURATION" | bc)
    
    echo "  Duration: ${DURATION}s"
    echo "  Throughput: ${THROUGHPUT} req/s"
    echo ""
done

# 混合负载测试
echo "Testing MIXED workload..."
START=$(date +%s.%N)

for i in $(seq 1 $NUM_REQUESTS); do
    SERVICE=$((RANDOM % 3))
    case $SERVICE in
        0) SID=1 ;;
        1) SID=2 ;;
        2) SID=66 ;;
    esac
    ./hvisor shm hyper_amp_qos $CONFIG "mixed_$i" $SID > /dev/null &
    
    # 控制并发度
    if [ $((i % 20)) -eq 0 ]; then
        wait
    fi
done
wait

END=$(date +%s.%N)
DURATION=$(echo "$END - $START" | bc)
THROUGHPUT=$(echo "scale=2; $NUM_REQUESTS / $DURATION" | bc)

echo "  Duration: ${DURATION}s"
echo "  Throughput: ${THROUGHPUT} req/s"
echo ""
EOF

chmod +x test_performance.sh
./test_performance.sh 2>&1 | tee performance_test.log
```

**预期结果**:
- 获得各类别的吞吐基线
- REALTIME 在混合负载下延迟相对稳定
- 统计输出符合预期分布

### 结果分析与验证

#### 1. 查看服务端统计
```bash
# 方法1: 服务端退出时自动打印(如已集成)
# 方法2: 发送信号触发打印(如 SIGUSR1,需代码支持)
# 方法3: 调用独立统计命令
./hvisor shm qos_stats shm_config.json
```

**关键指标**:
```
QoS Statistics:
  REALTIME:      enqueued=400, processed=400, avg_queue_len=2.3
  THROUGHPUT:    enqueued=200, processed=200, avg_queue_len=1.8
  RELIABLE:      enqueued=200, processed=200, avg_queue_len=1.5
  BEST_EFFORT:   enqueued=100, processed=100, avg_queue_len=0.8
  
  Processing ratio: 4.0 : 2.0 : 2.0 : 1.0  ✓ (符合权重)
  
  Resource usage:
    REALTIME reserved: 320/400 (80%)
    Shared pool: 150/400 (37%)
```

#### 2. 分析客户端日志
```bash
# 提取延迟数据(假设客户端输出格式为 "Latency: XXX us")
grep "Latency:" *.log | awk '{print $2}' | sort -n > latencies.txt

# 计算P50/P95/P99
python3 << 'EOF'
import sys
data = sorted([float(x) for x in open('latencies.txt')])
n = len(data)
print(f"P50: {data[int(n*0.5)]:.2f} us")
print(f"P95: {data[int(n*0.95)]:.2f} us")
print(f"P99: {data[int(n*0.99)]:.2f} us")
EOF
```

#### 3. 验证清单

**客户端侧验证** (Root Linux):
- [ ] **初始化成功**: 看到 `[QoS-Client] QoS controller initialized (1MB buffer, 64 slots)`
- [ ] **资源配置正确**: 显示四类资源分配 (REALTIME/THROUGHPUT/RELIABLE/BEST_EFFORT)
- [ ] **分类正确**: `[QoS-Send] Message Classification` 显示正确的 QoS Class
  - service_id=1或2 → REALTIME
  - service_id=66 → BEST_EFFORT
- [ ] **资源分配成功**: `[QoS] Allocated X bytes from reserved buffer`
- [ ] **收到响应**: 无 `[Error] Timeout` 错误

**服务端侧验证** (Non-Root Linux):
- [ ] **QoS初始化**: 启动时显示 `[QoS] Initialized - Buffer: 1048576 bytes, Slots: 64`
- [ ] **三阶段日志**: 
  - Phase 1: `[QoS-Phase1] Collected X messages`
  - Phase 2: `[QoS-Phase2] Scheduled message from XXX queue`
  - Phase 3: `[QoS-Phase3] Cleanup complete`
- [ ] **调度权重体现**: 在混合负载下,各类处理次数比例接近 4:2:2:1 (±10%误差)
- [ ] **防饥饿验证**: BEST_EFFORT 队列在高压下仍有处理记录

**系统级验证**:
- [ ] **分类正确性**: 四类消息均能正确入队对应 QoS 队列
- [ ] **调度权重**: 处理比例接近 4:2:2:1 (±10%误差可接受)
- [ ] **防饥饿**: BEST_EFFORT 在高压下仍有稳定进度,无请求超时
- [ ] **资源预留**: 高压时 REALTIME 成功率 > BEST_EFFORT
- [ ] **优先级差异**: REALTIME 平均延迟 < THROUGHPUT < BEST_EFFORT
- [ ] **系统稳定性**: 长时间运行无崩溃,错误有日志且安全退化
- [ ] **统计准确性**: 队列长度/处理计数/资源使用率数据合理

**日志示例 - 正确的客户端输出**:
```
[QoS-Client] QoS controller initialized (1MB buffer, 64 slots)
[QoS-Send] Message Classification:
  Service ID: 1
  QoS Class: REALTIME
  Priority: Highest (1)
[QoS] Allocated 13 bytes from reserved buffer (class 0)
[QoS-Client] Message sent, waiting for response...
[QoS-Client] ✅ Response received successfully
[QoS-Client] End-to-end latency: 1234 us
```

**日志示例 - 正确的服务端输出**:
```
[QoS] Initialized - Buffer: 1048576 bytes, Slots: 64
[QoS-Phase1] Starting collection...
[QoS-Phase1] Collected 3 messages
[QoS-Phase2] WRR Scheduling...
[QoS-Phase2] Scheduled message from REALTIME queue
[QoS-Phase2] Processing service_id=1 (ENCRYPT)
[QoS-Phase3] Cleanup complete
```

### 常见问题排查

#### Q0: 客户端超时但日志显示消息已发送 (最常见!)

**症状**:
```
[QoS-Send] Message Classification:
  Service ID: 1
  QoS Class: REALTIME
  ...
[QoS-Client] Message sent, waiting for response...
[Error] Timeout waiting for response
```

**可能原因**:

**原因1**: 服务端使用了错误的命令
- ❌ 错误: `./hvisor shm hyper_amp_service` (不理解QoS消息)
- ✅ 正确: `./hvisor shm hyper_amp_qos_service` (QoS感知)

**原因2**: 服务端队列未初始化（客户端显示 `working_mark=0x0`）
- **症状**: 
  - 客户端日志显示 `Checking target zone queue: working_mark=0x0`
  - 服务端日志显示 `Message queue initialized (working_mark=0x0, ...)` ← 错误！
- **原因**: `msg_queue_ops.init()` 不会设置 `working_mark`，需要手动设置
- **正确值**: `working_mark=0xeeeeeeee` (INIT_MARK_INITIALIZED) 然后设置为 `0xbbbbbbbb` (MSG_QUEUE_MARK_IDLE)
- **解决**: 已在最新代码中修复，需重新编译

**原因3**: 客户端未正确通知服务端（最新发现！）
- **症状**:
  - 客户端显示 `working_mark=0xbbbbbbbb`（队列就绪）
  - 客户端显示 `Message sent, waiting for response...`
  - 服务端显示 `Waiting for requests...` 但**没有任何Phase1/2/3处理日志**
  - 客户端最终超时
- **原因**: 
  - 客户端使用了 `msg_send()` 而不是 `msg_send_and_notify()`
  - `msg_send()` 只将消息放入队列,不执行transfer也不发送中断
  - `msg_send_and_notify()` 会执行 `transfer(wait_h → proc_ing_h)` 并发送中断通知服务端
  - 服务端的 `poll()` 因为没收到中断一直超时,永远不会处理消息
- **解决**: 已在最新代码中修复（客户端改用msg_send_and_notify），需重新编译

**验证方法**:
```bash
# 检查Non-Root Linux的服务端进程
ps aux | grep hvisor

# 查看服务端启动日志,应该看到:
# [QoS] Initialized - Buffer: 1048576 bytes, Slots: 64
# [QoS] Resource allocation: ...
# [QoS-Server] Message queue initialized (working_mark=0xbbbbbbbb, buf_size=X)

# 发送消息后应该立即看到:
# [QoS-Server] ========== BATCH PROCESSING #1 ==========
# [QoS-Server] Phase 1: Collecting messages...
# [QoS-Server] Phase 2: WRR Scheduling...
# [QoS-Server] Phase 3: Cleaning up...
```

**解决步骤**:
1. 停止当前服务端 (Ctrl+C)
2. 确认已使用最新编译的版本（包含队列初始化和transfer修复）
3. 使用正确命令重启:
   ```bash
   ./hvisor shm hyper_amp_qos_service shm_config.json
   ```
4. 等待服务端显示 "Message queue initialized" 后再运行客户端

#### Q1: 服务端无响应
```bash
# 检查共享内存映射
cat /proc/<server_pid>/maps | grep shm

# 检查进程状态
ps aux | grep hvisor

# 查看系统日志
dmesg | tail -50
```

#### Q2: 客户端超时
**典型现象**:
```
[QoS-Client] Message sent, waiting for response...
[Error] Timeout waiting for response
```

**常见原因及解决**:
1. **服务端类型不匹配** (最常见!)
   - 问题: 使用了 `hyper_amp_service` 而非 `hyper_amp_qos_service`
   - 解决: 确保服务端使用 QoS 感知命令:
     ```bash
     ./hvisor shm hyper_amp_qos_service shm_config.json
     ```
   
2. **服务 ID 未注册**
   - 确认服务 ID 已在 `tools/shm/qos.c` 的 `default_qos_profiles` 中定义
   - 当前支持: 1(ENCRYPT), 2(DECRYPT), 66(ECHO)

3. **队列满或资源不足**
   - 查看服务端统计,检查是否有入队失败日志
   - 临时解决: 重启服务端清空队列

4. **配置文件不一致**
   - 验证客户端与服务端使用相同的 JSON 配置
   - 检查共享内存地址映射是否一致

#### Q3: 统计数据异常
- 确认测试负载均匀分布
- 检查是否有入队失败(容量/资源不足)
- 验证权重配置未被修改

#### Q4: 延迟波动大
- 确认是否在轮询模式(无 /dev/hshm0)
- 检查系统负载(CPU/内存)
- 增加测试样本量以平滑抖动

### 高级测试(可选)

#### 参数扫描对比实验
```bash
# 修改 qos.c 中的权重配置,重新编译并测试
# 对比配置: 4:2:2:1 vs 8:2:1:1 vs 1:1:1:1
# 绘制不同权重下的延迟分布图

# 修改队列容量(MAX_QUEUE_SIZE)
# 对比: 32 vs 64 vs 128

# 修改资源预留比例
# 对比: 无预留(0%) vs 20% vs 40%
```

#### 长稳测试
```bash
# 运行24小时持续负载,监控:
# - 内存泄漏(valgrind/pmap)
# - 性能退化(定时采样吞吐)
# - 统计一致性(计数器溢出/累加准确性)
```

---

以上功能均已在代码中实现并形成可运行路径,覆盖"分类入队 → 加权调度 → 资源管理 → 处理执行 → 统计打印"的完整闭环。-

## 客户端功能

- 功能：QoS 感知的发送路径（分类→可选预留→写入共享内存→发送→等待→释放）
  - 效果：不同请求按 REALTIME/THROUGHPUT/RELIABLE/BEST_EFFORT 分类进入对应优先级通道；在资源紧张时，关键类别优先使用其保留配额，降低被挤出概率。
  - 设计决策：复用现有 `struct Msg` 与共享内存布局，使用 `QoSMsg` 浅拷贝承载 QoS 元数据，零 ABI 侵入。

- 功能：端到端时延与吞吐的基础统计输出
  - 效果：在客户端侧汇总并打印请求计数、基本延迟与速率信息，便于对比不同权重/配额配置的影响（延迟包含等待/轮询开销）。
  - 设计决策：不依赖跨核时钟同步，将观测点放在客户端，保证跨环境可用性。

- 功能：中断/轮询双模等待
  - 效果：优先使用 `/dev/hshm0` 事件等待（低开销）；设备不可用时自动退化为轮询，保证功能可用性。
  - 设计决策：以可落地为前提，在不同硬件/驱动条件下行为一致。

- 功能：资源释放与对称回收
  - 效果：请求完成后归还占用配额，先回补该类保留额度，余量回到共享池，避免资源泄漏与长期不公平。
  - 设计决策：与服务端一致的“保留优先 + 共享补充”策略，形成闭环。

- 功能：健壮性与边界保护
  - 效果：对消息偏移/长度与队列容量进行检查，异常输入不影响进程稳定性。
  - 设计决策：以安全为第一目标，优先防止越界与环路传播到后续阶段。

---

## 服务端功能

- 功能：三阶段批处理数据流（Phase1 收集与清理 → Phase2 WRR 调度与处理 → Phase3 队列复位）
  - 效果：在批处理范式下稳定推进队列，降低并发访问带来的环路/重复处理风险；处理完成后根队列回到可接受状态，便于持续运行。
  - 设计决策：在 Phase1 记录“已访问节点”并即时清理 `nxt_idx`；保持与现有收发通道的弱耦合。

- 功能：多级 QoS 调度（WRR 加权轮询）
  - 效果：按权重 4:2:2:1 对四类队列分配服务机会，采用“统一信用重置”保证低优先级在高压下仍有进度，实现优先级差异化与防饥饿。
  - 设计决策：选择简单可验证的 WRR；`schedule()` 仅选择不移除元素，调用方在处理后显式 `dequeue()`，以保持与现有调用路径的兼容性。

- 功能：按类环形队列与浅拷贝入队
  - 效果：每类固定容量环形缓冲维护 head/tail/计数；入队采用浅拷贝，低开销、不破坏原 ABI；容量满时按策略失败返回，避免隐式丢弃。
  - 设计决策：将 QoS 逻辑与底层通信解耦，必要时可独立扩容/调参。

- 功能：双层资源管理（保留池 + 共享池）
  - 效果：关键类别在资源紧张时获得最低保障水线；非关键类别仍可从共享池竞争剩余资源，整体吞吐与公平性兼顾。
  - 设计决策：分层配额与“保留优先分配、释放先回补保留”的对称规则，行为可预测、易于调参。

- 功能：统计与观测性
  - 效果：输出各类队列长度、调度/处理计数等运行时信息，支撑实验对比与问题定位。
  - 设计决策：提供独立的 `print_qos_stats()` 接口，避免与业务逻辑耦合。

- 功能：事件驱动优先、轮询退化
  - 效果：通过 `poll(/dev/hshm0)` 异步等待新事件；无事件设备时自动切换为轮询模式，系统持续可用。
  - 设计决策：用最小改动兼容多种部署环境。

- 功能：错误处理与安全退化
  - 效果：对调度结果为空、入队失败、未知服务 ID 等情况给出日志与安全处理分支，避免服务线程异常退出。
  - 设计决策：将错误视为常态输入场景进行处理，提升长时间运行稳定性。

---

以上功能均已在代码中实现并形成可运行路径，覆盖“分类入队 → 加权调度 → 资源管理 → 处理执行 → 统计打印”的完整闭环。
