# HyperAMP 多线程支持方案

## 📋 目录
1. [现有多线程架构分析](#现有多线程架构分析)
2. [hyper_amp_client_test 多线程改造方案](#客户端多线程方案)
3. [hyper_amp_service_test 多线程改造方案](#服务端多线程方案)
4. [实现步骤](#实现步骤)
5. [测试验证](#测试验证)

---

## 现有多线程架构分析

### ✅ 已有的多线程基础设施

#### 1. 自旋锁实现 (`spinlock.c` / `spinlock.h`)
```c
// 两种锁类型:
ByteFlag    // CAS原子锁,适合共享内存
MarkFlag    // 原子标志锁,适合代码区

// 操作接口:
byte_flag_ops.init()      // 初始化
byte_flag_ops.lock()      // 加锁 (自旋 + yield)
byte_flag_ops.unlock()    // 解锁
byte_flag_ops.try_lock()  // 尝试加锁
```

**特点**:
- ✅ 基于 C11 `<stdatomic.h>` 实现
- ✅ 使用 `sched_yield()` 避免 CPU 空转
- ✅ 支持跨进程共享 (通过共享内存)

#### 2. 线程池实现 (`threads.c` / `threads.h`)
```c
ThreadPool {
    pthread_mutex_t lock;       // 任务队列锁
    pthread_cond_t cond;        // 条件变量
    Task* task_queue;           // 任务队列
    pthread_t* threads;         // 工作线程数组
}

// 核心函数:
init_thread_pool()      // 创建线程池
add_task()              // 添加任务
worker()                // 工作线程函数
destroy_thread_pool()   // 销毁线程池
```

**关键设计**:
- ✅ 每个工作线程预分配一个 `struct Msg*`
- ✅ 任务通过 `handle_request()` 处理
- ✅ 使用 pthread 互斥锁保护任务队列

#### 3. 现有多线程客户端 (`setup_shm_client`)
```bash
./hvisor shm setup_shm_client <config.json> <input.txt> <output_dir> <threads>
```

**工作流程**:
1. 创建单个 `struct Client` (所有线程共享)
2. 创建线程池 (每个线程预分配 `Msg*`)
3. 从文件读取请求,分发给线程池
4. 线程并发处理请求

**问题分析**:
- ❌ 所有线程共享同一个 `amp_client`
- ❌ 消息队列操作无锁保护
- ❌ 只适用于特定场景 (文件驱动)

---

## 客户端多线程方案

### 设计目标
为 `hyper_amp_client_test` 添加多线程支持,使其能够:
- 并发发送多个请求
- 复用现有线程池基础设施
- 保持向后兼容 (单线程模式仍可用)

### 方案设计

#### 方案 A: 线程池模式 (推荐)

**架构**:
```
Main Thread
    ↓
Client Init (共享)
    ↓
Thread Pool (N 个工作线程)
    ├─ Thread 1 → Msg 1 → Queue Lock → Send
    ├─ Thread 2 → Msg 2 → Queue Lock → Send
    └─ Thread N → Msg N → Queue Lock → Send
```

**关键改动**:

1. **添加队列锁保护** (`msgqueue.c`):
```c
// 在 struct AmpMsgQueue 中添加锁字段
typedef struct AmpMsgQueue {
    // ...existing fields...
    ByteFlag queue_lock;  // ← 新增: 队列操作锁
} AmpMsgQueue;

// 修改 msg_queue_push
static int32_t msg_queue_push(...) {
    byte_flag_ops.lock(&msg_queue->queue_lock);  // ← 加锁
    
    // ...原有逻辑...
    
    byte_flag_ops.unlock(&msg_queue->queue_lock);  // ← 解锁
}
```

2. **创建并发客户端包装器**:
```c
// 新增函数: hyper_amp_client_test_mt()
static int hyper_amp_client_test_mt(int argc, char* argv[]) {
    char* config = argv[0];
    char* data_input = argv[1];
    uint32_t service_id = atoi(argv[2]);
    int num_threads = atoi(argv[3]);  // ← 新参数
    
    // 1. 初始化共享客户端
    parse_global_addr(config);
    struct Client* amp_client = malloc(sizeof(struct Client));
    client_ops.client_init(amp_client, ZONE_NPUcore_ID);
    
    // 2. 创建线程池
    ThreadPool* pool = init_thread_pool(num_threads, amp_client);
    
    // 3. 准备数据
    char* data = strdup(data_input);
    
    // 4. 提交任务 (重复 num_threads 次)
    for (int i = 0; i < num_threads; i++) {
        Request* req = malloc(sizeof(Request));
        req->service_id = service_id;
        req->data_string = strdup(data);
        req->size = strlen(data);
        add_task(pool, handle_single_request, req);
    }
    
    // 5. 等待完成
    while (!task_queue_is_empty(pool)) {
        sleep(1);
    }
    
    // 6. 清理
    destroy_thread_pool(pool);
    client_ops.client_destory(amp_client);
    free(amp_client);
    free(data);
}
```

3. **修改任务处理函数**:
```c
// 修改 handle_request 以支持单次请求
void handle_single_request(void* arg1, void* arg2) {
    Request* request = (Request*)arg1;
    struct Msg* msg = (struct Msg*)arg2;
    
    printf("[Thread %ld] Processing: service=%u, data=%s\n",
           pthread_self(), request->service_id, request->data_string);
    
    // 1. 设置消息
    msg->service_id = request->service_id;
    
    // 2. 分配共享内存 (需要加锁保护)
    char* shm_data = client_ops.shm_malloc(&amp_client, 
                                           request->size + 1, 
                                           MALLOC_TYPE_P);
    
    // 3. 逐字节拷贝数据
    for (int i = 0; i < request->size; i++) {
        shm_data[i] = request->data_string[i];
    }
    shm_data[request->size] = '\0';
    
    // 4. 设置消息offset
    msg->offset = client_ops.shm_addr_to_offset(shm_data);
    msg->length = request->size + 1;
    
    // 5. 发送并等待响应 (队列操作已加锁)
    if (client_ops.msg_send_and_notify(&amp_client, msg) == 0) {
        while (client_ops.msg_poll(msg) != 0) {
            usleep(1000);
        }
        printf("[Thread %ld] ✅ Response received\n", pthread_self());
    }
    
    // 6. 清理
    free(request->data_string);
    free(request);
}
```

#### 方案 B: 每线程独立客户端 (备选)

**架构**:
```
Main Thread
    ↓
┌─────────────┬─────────────┬─────────────┐
Thread 1      Thread 2      Thread N
↓             ↓             ↓
Client 1      Client 2      Client N
↓             ↓             ↓
Queue Part 1  Queue Part 2  Queue Part N
```

**优点**:
- 无需锁 (完全隔离)
- 性能更高

**缺点**:
- 需要队列分区
- 实现复杂度高

---

## 服务端多线程方案

### 设计目标
为 `hyper_amp_service_test` 添加多线程支持,使其能够:
- 并发处理多个客户端请求
- 负载均衡
- 避免队列竞争

### 方案设计

#### 方案 A: 单消费者模式 (推荐)

**架构**:
```
Main Thread (消息收集)
    ↓
Batch Collect
    ↓
Thread Pool (并行处理)
    ├─ Thread 1 → Service(msg1)
    ├─ Thread 2 → Service(msg2)
    └─ Thread N → Service(msgN)
```

**实现**:
```c
static int hyper_amp_service_test_mt(char* config, int num_threads) {
    // 1. 初始化 (同单线程版本)
    parse_global_addr(config);
    int mem_fd = open("/dev/mem", O_RDWR | O_SYNC);
    
    // 映射内存...
    struct AmpMsgQueue* root_msg_queue = ...;
    
    // 2. 创建服务线程池
    ThreadPool* pool = malloc(sizeof(ThreadPool));
    pthread_mutex_init(&pool->lock, NULL);
    pthread_cond_init(&pool->cond, NULL);
    pool->task_queue = NULL;
    pool->num_threads = num_threads;
    pool->stop = 0;
    
    pool->threads = malloc(sizeof(pthread_t) * num_threads);
    for (int i = 0; i < num_threads; i++) {
        pthread_create(&pool->threads[i], NULL, 
                      service_worker, &args);
    }
    
    // 3. 主循环 (单线程收集消息)
    while (running) {
        // 等待中断
        if (poll(&pfd, 1, 1000) > 0) {
            // 收集所有待处理消息
            int collected = 0;
            uint16_t head = root_msg_queue->proc_ing_h;
            
            while (head < root_msg_queue->buf_size && 
                   collected < MAX_BATCH) {
                // 获取消息
                struct MsgEntry* entry = get_entry(head);
                
                // 创建任务并分发给线程池
                ServiceTask* task = malloc(sizeof(ServiceTask));
                task->msg = entry->msg;
                task->buf_addr = buf_addr;
                task->msg_index = head;
                
                add_task(pool, process_service_task, task);
                
                head = entry->nxt_idx;
                collected++;
            }
            
            // 更新队列头
            root_msg_queue->proc_ing_h = head;
        }
    }
    
    // 4. 清理
    destroy_thread_pool(pool);
}
```

**服务处理线程**:
```c
void process_service_task(void* arg1, void* arg2) {
    ServiceTask* task = (ServiceTask*)arg1;
    struct Msg* msg = &task->msg;
    
    // 获取数据地址
    char* data = (char*)(task->buf_addr + msg->offset);
    
    // 执行服务
    switch (msg->service_id) {
        case 1:  // 加密
            hyperamp_encrypt_service(data, msg->length - 1, msg->length);
            break;
        case 2:  // 解密
            hyperamp_decrypt_service(data, msg->length - 1, msg->length);
            break;
        case 66: // Echo
            // 无需处理
            break;
    }
    
    // 更新状态
    msg->flag.deal_state = MSG_DEAL_STATE_YES;
    msg->flag.service_result = MSG_SERVICE_RET_SUCCESS;
    
    free(task);
}
```

---

## 实现步骤

### Phase 1: 添加队列锁保护 (必需)

**文件**: `tools/shm/msgqueue.c`

1. **修改队列结构**:
```c
// 在 msg_queue_init 中初始化锁
static int32_t msg_queue_init(struct AmpMsgQueue* msg_queue, uint32_t mem_len) {
    // ...existing code...
    
    // 初始化队列锁
    byte_flag_ops.init(&msg_queue->queue_lock);
    
    return 0;
}
```

2. **保护关键操作**:
```c
// msg_queue_push
static int32_t msg_queue_push(struct AmpMsgQueue* msg_queue, 
                               uint16_t* head, 
                               uint16_t msg_index) {
    byte_flag_ops.lock(&msg_queue->queue_lock);
    
    // ...原有逻辑...
    
    byte_flag_ops.unlock(&msg_queue->queue_lock);
    return 0;
}

// msg_queue_pop
static uint16_t msg_queue_pop(struct AmpMsgQueue* msg_queue, uint16_t* head) {
    byte_flag_ops.lock(&msg_queue->queue_lock);
    
    uint16_t msg_index = *head;
    // ...原有逻辑...
    
    byte_flag_ops.unlock(&msg_queue->queue_lock);
    return msg_index;
}

// msg_queue_transfer
static int32_t msg_queue_transfer(struct AmpMsgQueue* msg_queue, 
                                   uint16_t* src_head, 
                                   uint16_t* dst_head) {
    byte_flag_ops.lock(&msg_queue->queue_lock);
    
    // ...原有逻辑...
    
    byte_flag_ops.unlock(&msg_queue->queue_lock);
    return 0;
}
```

### Phase 2: 实现多线程客户端

**文件**: `tools/hvisor.c`

```c
// 添加新函数
static int hyper_amp_client_test_multithread(int argc, char* argv[]) {
    if (argc < 4) {
        printf("Usage: ./hvisor shm hyper_amp_test_mt <config> <data> <service_id> <threads>\n");
        return -1;
    }
    
    // 实现见上文 "方案 A"
    // ...
}

// 在 main() 中添加命令
else if(strcmp(argv[2], "hyper_amp_test_mt") == 0) {
    hyper_amp_client_test_multithread(argc - 3, &argv[3]);
}
```

### Phase 3: 实现多线程服务端

**文件**: `tools/hvisor.c`

```c
static int hyper_amp_service_test_multithread(char* config, int num_threads) {
    // 实现见上文 "方案 A"
    // ...
}

// 在 main() 中添加命令
else if(strcmp(argv[2], "hyper_amp_service_test_mt") == 0) {
    int threads = (argc >= 5) ? atoi(argv[4]) : 4;
    hyper_amp_service_test_multithread(argv[3], threads);
}
```

---

## 测试验证

### 单元测试

#### 1. 队列锁功能测试
```bash
# 创建测试程序验证锁的正确性
# 多线程并发 push/pop,验证无数据丢失
```

#### 2. 客户端并发测试
```bash
# 单线程基准
./hvisor shm hyper_amp_test config.json "hello" 1

# 多线程测试 (4线程)
./hvisor shm hyper_amp_test_mt config.json "hello" 1 4

# 预期: 
# - 4个请求都成功
# - 无崩溃或数据损坏
# - 日志显示并发处理
```

#### 3. 服务端并发测试
```bash
# 启动多线程服务端
./hvisor shm hyper_amp_service_test_mt config.json 4

# 从客户端并发发送请求
for i in {1..20}; do
    ./hvisor shm hyper_amp_test config.json "msg_$i" 1 &
done
wait

# 预期:
# - 所有请求都被处理
# - 服务端日志显示并行处理
```

### 压力测试

```bash
# 高并发场景 (100个并发请求)
./hvisor shm hyper_amp_test_mt config.json "stress_test" 1 100

# 长时间运行 (30分钟)
timeout 1800 ./hvisor shm hyper_amp_service_test_mt config.json 8

# 混合负载
# 启动服务端
./hvisor shm hyper_amp_service_test_mt config.json 4 &

# 并发发送不同服务类型
for i in {1..50}; do
    ./hvisor shm hyper_amp_test config.json "encrypt_$i" 1 &
    ./hvisor shm hyper_amp_test config.json "decrypt_$i" 2 &
    ./hvisor shm hyper_amp_test config.json "echo_$i" 66 &
done
wait
```

### 性能基准测试

```bash
# 单线程基准
time for i in {1..100}; do
    ./hvisor shm hyper_amp_test config.json "test" 1
done

# 多线程对比 (4线程)
time ./hvisor shm hyper_amp_test_mt config.json "test" 1 100

# 预期: 多线程版本时间 < 单线程时间 / 4 + 开销
```

---

## 兼容性说明

### 向后兼容
- ✅ 原有单线程函数保持不变
- ✅ 新增 `_mt` 后缀版本用于多线程
- ✅ 默认行为不改变

### 配置要求
- 队列大小建议 >= 线程数 * 2
- 共享内存建议 >= 线程数 * 平均消息大小 * 10

---

## 附录

### A. 数据结构定义

```c
// 服务任务
typedef struct ServiceTask {
    struct Msg msg;
    uint64_t buf_addr;
    uint16_t msg_index;
} ServiceTask;

// 客户端请求
typedef struct Request {
    uint32_t request_id;
    uint32_t service_id;
    char* data_string;
    uint32_t size;
    char* output_dir;
} Request;
```

### B. 锁性能对比

| 锁类型 | 加锁延迟 | 适用场景 | 跨进程 |
|--------|---------|---------|--------|
| ByteFlag (Spinlock) | ~100ns | 短临界区 | ✅ |
| pthread_mutex | ~1μs | 长临界区 | ❌ |
| atomic_flag | ~50ns | 最短临界区 | ✅ |

### C. 参考资料

- `tools/shm/spinlock.c` - 自旋锁实现
- `tools/shm/threads.c` - 线程池实现
- `tools/hvisor.c:setup_shm_client()` - 现有多线程客户端
