# QoS集成到hvisor.c的详细说明

## 已完成的工作
1. ✅ 添加了QoS头文件引用: `#include "shm/qos.h"`
2. ✅ 添加了全局QoS控制器变量
3. ✅ 添加了QoS初始化和配置显示代码到`hyper_amp_client_with_qos`函数开头

## 需要手动修改的地方

### 1. 在 `hyper_amp_client_with_qos` 函数中修改

#### 位置1: 在"// 重置消息"之后添加QoS资源检查
**原代码(大约在3360行):**
```c
// 重置消息
msg_ops.msg_reset(msg);

// 分配共享内存
char* shm_data = (char*)client_ops.shm_malloc(&amp_client, data_size + 1, MALLOC_TYPE_P);
```

**修改为:**
```c
// 重置消息
msg_ops.msg_reset(msg);

// ===== QoS资源检查 =====
printf("[QoS] Checking resource availability...\n");
if (qos_ops.allocate_buffer(&global_qos_ctrl, qos_class, data_size + 1) != 0) {
    printf("[QoS] Warning: Insufficient reserved resources, using shared pool\n");
}

// ===== 创建QoS消息包装 =====
struct QoSMsg qos_msg = {0};
memcpy(&qos_msg.base_msg, msg, sizeof(struct Msg));
qos_msg.qos_class = qos_class;
qos_msg.priority = (qos_class == QOS_REALTIME) ? 3 : 
                   (qos_class == QOS_RELIABLE) ? 2 :
                   (qos_class == QOS_THROUGHPUT) ? 1 : 0;
qos_msg.send_timestamp = get_timestamp_us();

// 分配共享内存
uint64_t alloc_start = get_timestamp_us();
char* shm_data = (char*)client_ops.shm_malloc(&amp_client, data_size + 1, MALLOC_TYPE_P);
```

#### 位置2: 在shm_malloc成功后添加计时输出
**原代码:**
```c
printf("info : shm malloc success [size = %u, ptr = %p]\n", data_size + 1, shm_data);

// 复制数据到共享内存
```

**修改为:**
```c
uint64_t alloc_end = get_timestamp_us();
printf("[QoS] Memory allocated in %lu us\n", alloc_end - alloc_start);

// 复制数据到共享内存
```

#### 位置3: 在发送消息处添加计时
**原代码(大约在3395行):**
```c
// 发送消息并通
if (client_ops.msg_send_and_notify(&amp_client, msg) != 0)
{
    printf("error : msg send failed [offset = 0x%x, length = %u]\n", msg->offset, msg->length);
    free(data_buffer);
    return -1;
}

printf("info : msg sent successfully\n");
```

**修改为:**
```c
// 发送消息并通知
uint64_t send_start = get_timestamp_us();
if (client_ops.msg_send_and_notify(&amp_client, msg) != 0)
{
    printf("error : msg send failed [offset = 0x%x, length = %u]\n", msg->offset, msg->length);
    qos_ops.release_buffer(&global_qos_ctrl, qos_class, data_size + 1);
    free(data_buffer);
    return -1;
}
uint64_t send_end = get_timestamp_us();
printf("[QoS] Message sent in %lu us\n", send_end - send_start);
```

#### 位置4: 修改轮询逻辑
**原代码(大约在3403行):**
```c
// 等待响应
printf("info : waiting for Non-Root Linux to process the request...\n");
while(client_ops.msg_poll(msg) != 0) {
    // 轮询等待响应
    printf("is polling...\n");
    sleep(3);
}
```

**修改为:**
```c
// QoS感知的轮询策略
printf("[QoS] Waiting for response with %s polling strategy...\n", qos_class_names[qos_class]);
uint64_t poll_start = get_timestamp_us();
int poll_count = 0;

// 不同QoS类别使用不同的轮询间隔
int poll_interval_us = 0;
switch (qos_class) {
    case QOS_REALTIME:
        poll_interval_us = 10;      // 10微秒 - 积极轮询
        break;
    case QOS_RELIABLE:
        poll_interval_us = 100;     // 100微秒
        break;
    case QOS_THROUGHPUT:
        poll_interval_us = 1000;    // 1毫秒
        break;
    case QOS_BEST_EFFORT:
        poll_interval_us = 3000000; // 3秒
        break;
    default:
        poll_interval_us = 1000;
}

while(client_ops.msg_poll(msg) != 0) {
    poll_count++;
    if (poll_interval_us >= 1000000) {
        sleep(poll_interval_us / 1000000);
    } else {
        usleep(poll_interval_us);
    }
}

uint64_t poll_end = get_timestamp_us();
uint64_t total_latency = poll_end - send_start;
printf("[QoS] Response received after %d polls in %lu us\n", poll_count, poll_end - poll_start);
printf("[QoS] Total end-to-end latency: %lu us\n", total_latency);
```

#### 位置5: 在MSG_SERVICE_RET_SUCCESS分支结束后添加QoS指标更新
**在显示完结果数据后(大约在3480行),添加:**
```c
    // ===== QoS指标更新 =====
    struct QoSQueue *queue = &global_qos_ctrl.queues[qos_class];
    qos_ops.update_metrics(&queue->metrics, &qos_msg, 
                           msg->flag.service_result == MSG_SERVICE_RET_SUCCESS);
    
    // 检查QoS违约
    int violations = qos_ops.check_qos_violation(&queue->metrics, profile);
    if (violations > 0) {
        printf("[QoS] ⚠ Warning: %d QoS violation(s) detected\n", violations);
    } else {
        printf("[QoS] ✓ QoS requirements met\n");
    }
    
    // 显示关键指标
    printf("[QoS] Performance Metrics:\n");
    printf("  Average Latency: %lu us\n", queue->metrics.avg_latency_us);
    printf("  Max Latency: %lu us\n", queue->metrics.max_latency_us);
    printf("  Success Rate: %.2f%%\n", 
           (float)queue->metrics.success_count / queue->metrics.total_messages * 100);
    printf("  Total Messages: %lu\n", queue->metrics.total_messages);
```

#### 位置6: 在最后的return之前添加资源释放
**原代码:**
```c
    free(data_buffer);
    
    return 0;
}
```

**修改为:**
```c
    // 释放QoS资源
    qos_ops.release_buffer(&global_qos_ctrl, qos_class, data_size + 1);
    free(data_buffer);
    
    printf("\n[QoS] ========== Transaction Completed ==========\n\n");
    
    return 0;
}
```

---

### 2. 在 main() 函数中添加新命令

**在处理"hyper_amp"命令的else if分支后添加:**
```c
else if(strcmp(argv[2], "hyper_amp_qos") == 0) {
    // hvisor shm hyper_amp_qos <shm_json_path> <data|@filename> <service_id>
    if (argc < 5) {
        printf("Usage: ./hvisor shm hyper_amp_qos <shm_json_path> <data|@filename> <service_id>\n");
        printf("Examples:\n");
        printf("  ./hvisor shm hyper_amp_qos shm_config.json \"hello\" 1     # REALTIME\n");
        printf("  ./hvisor shm hyper_amp_qos shm_config.json @data.txt 9    # THROUGHPUT\n");
        printf("  ./hvisor shm hyper_amp_qos shm_config.json \"secret\" 3    # RELIABLE\n");
        return -1;
    }
    hyper_amp_client_with_qos(argc - 3, &argv[3]);
}
else if(strcmp(argv[2], "qos_stats") == 0) {
    // hvisor shm qos_stats - 显示QoS统计信息
    if (!qos_initialized) {
        printf("QoS not initialized. Run a QoS command first.\n");
        return -1;
    }
    qos_ops.print_qos_stats(&global_qos_ctrl);
}
```

---

### 3. 更新Makefile

**在tools/Makefile中,确保添加QoS对象文件:**
```makefile
OBJS += shm/qos.o
```

---

## 编译和测试

### 编译:
```bash
cd /home/b/ft/hvisor-tool
make clean
make all ARCH=arm64 LOG=LOG_INFO KDIR=~/linux
```

### 测试QoS功能:
```bash
# 测试REALTIME服务(Echo - Service ID 1)
./hvisor shm hyper_amp_qos shm_config.json "hello" 1

# 测试THROUGHPUT服务(Hash - Service ID 9)
./hvisor shm hyper_amp_qos shm_config.json "large_data" 9

# 测试RELIABLE服务(Crypto - Service ID 3)
./hvisor shm hyper_amp_qos shm_config.json "secret" 3

# 查看QoS统计信息
./hvisor shm qos_stats
```

### 预期输出:
```
========== QoS-Enhanced HyperAMP Client ==========
[QoS] Service Profile:
  Service ID: 1
  QoS Class: REALTIME
  Max Latency: 100 us
  Min Bandwidth: 10 Mbps
  Preemption: Yes
==================================================

[QoS] Checking resource availability...
[QoS] Memory allocated in 25 us
[QoS] Message sent in 15 us
[QoS] Waiting for response with REALTIME polling strategy...
[QoS] Response received after 10 polls in 85 us
[QoS] Total end-to-end latency: 100 us
[QoS] ✓ QoS requirements met
[QoS] Performance Metrics:
  Average Latency: 98 us
  Max Latency: 120 us
  Success Rate: 100.00%
  Total Messages: 1
```

---

## 论文数据收集

修改完成后,可以收集以下数据用于论文:

1. **延迟对比**: 运行相同服务100次,对比`hyper_amp`和`hyper_amp_qos`的延迟
2. **QoS类别性能**: 测试4种QoS类别的平均延迟、最大延迟、抖动
3. **资源利用率**: 通过`qos_stats`查看各QoS队列的使用情况
4. **混合负载**: 同时运行多个不同QoS类别的请求,验证优先级调度

---

## 故障排查

如果遇到编译错误:
1. 检查`shm/qos.h`和`shm/qos.c`是否在正确位置
2. 检查Makefile是否包含`shm/qos.o`
3. 检查`get_timestamp_us()`函数是否可用

如果运行时出错:
1. 检查QoS初始化是否成功
2. 查看是否有资源分配失败的warning
3. 使用`qos_stats`命令检查QoS状态
