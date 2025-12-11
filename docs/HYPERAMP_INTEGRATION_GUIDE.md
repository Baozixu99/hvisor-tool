# HyperAMP 跨 VM 通信集成文档

**版本**: 1.0  
**日期**: 2025-12-11  
**状态**: ✅ 测试通过

---

## 1. 系统架构

### 1.1 角色定义

```
┌─────────────────────────────────────────────────────────────┐
│                      Phytium Pi 硬件平台                      │
│  ┌───────────────────────┐      ┌──────────────────────────┐│
│  │   seL4 Microkernel    │      │      Linux Kernel        ││
│  │  (Frontend/Client)    │◄────►│   (Backend/Server)       ││
│  │                       │      │                          ││
│  │  - 生成协议请求       │      │  - 处理网络转发          ││
│  │  - 处理响应数据       │      │  - 管理真实连接          ││
│  │  - 运行安全关键应用   │      │  - 提供网络协议栈        ││
│  └───────────────────────┘      └──────────────────────────┘│
│             │                              │                 │
│             └──────────► 共享内存 ◄────────┘                 │
│                   (Uncached MMIO)                            │
└─────────────────────────────────────────────────────────────┘
```

**设计原则**：
- **seL4 (Zone 1)**: 可信域，运行协议栈前端，生成请求但不直接访问网络
- **Linux (Zone 0)**: 非可信域，提供网络服务，但不处理敏感业务逻辑
- **通信方式**: 单向共享内存 + 环形队列 + 软件自旋锁

---

## 2. 内存布局

### 2.1 物理地址映射

基于实际运行日志的地址信息：

| 组件 | 物理地址 (PA) | 大小 | 属性 |
|------|---------------|------|------|
| **TX Queue** | `0xDE000000` | 4 KB | DEVICE_nGnRnE (Uncached) |
| **RX Queue** | `0xDE001000` | 4 KB | DEVICE_nGnRnE (Uncached) |
| **Data Region** | `0xDE002000` | 4 MB | DEVICE_nGnRnE (Uncached) |


### 2.2 虚拟地址映射

#### seL4 端（Zone 1）

| 组件 | 虚拟地址 (VA) | 用途 |
|------|---------------|------|
| **TX Queue** | `0x54E000` | seL4 写入请求给 Linux |
| **RX Queue** | `0x54F000` | seL4 读取 Linux 的响应 |
| **Data Region** | `0x550000` | 共享数据缓冲区 |

```c
// seL4 端队列语义
g_tx_queue = (volatile HyperampShmQueue *)0x54E000;  // 发送队列
g_rx_queue = (volatile HyperampShmQueue *)0x54F000;  // 接收队列
g_data_region = (volatile void *)0x550000;           // 数据区
```

#### Linux 端（Zone 0）

| 组件 | 虚拟地址 (VA) | 物理地址 (PA) | 用途 |
|------|---------------|---------------|------|
| **RX Queue** | `0xFFFF9276D000` | `0xDE000000` | Linux 读取 seL4 的请求 |
| **TX Queue** | `0xFFFF9276E000` | `0xDE001000` | Linux 写入响应给 seL4 |
| **Data Region** | `0xFFFF9276F000` | `0xDE002000` | 共享数据缓冲区 |

```c
// Linux 端队列语义（注意：RX/TX 与 seL4 相反）
g_ctx.rx_queue = 0xFFFF9276D000;  // 接收队列 (对应 seL4 的 TX)
g_ctx.tx_queue = 0xFFFF9276E000;  // 发送队列 (对应 seL4 的 RX)
g_ctx.data_region = 0xFFFF9276F000;
```

### 2.3 队列映射关系

**关键理解**：一方的 TX 是另一方的 RX！

```
seL4 TX Queue (PA: 0xDE000000) ════════► Linux RX Queue (PA: 0xDE000000)
                                         ║
Linux TX Queue (PA: 0xDE001000) ◄════════ seL4 RX Queue (PA: 0xDE001000)
```

seL4 写入 TX Queue (0xDE000000) → Linux 从 RX Queue (0xDE000000) 读取 
Linux 写入 TX Queue (0xDE001000) → seL4 从 RX Queue (0xDE001000) 读取 

### 2.4 Uncached 属性的重要性

**为什么必须使用 Uncached 内存**？

1. **多核/多VM 一致性**：
   - seL4 和 Linux 运行在不同的核心/虚拟机
   - CPU 缓存是私有的，不会自动同步
   - Uncached (DEVICE_nGnRnE) 强制所有访问直达主存

2. **避免数据竞争**：
   ```
   ❌ Cached 模式：
   seL4: 写入数据 → CPU1 Cache → (未刷新) → 主存
   Linux: 读取数据 ← CPU2 Cache ← (旧数据!) ← 主存
   
   ✅ Uncached 模式：
   seL4: 写入数据 ───────────────────► 主存
   Linux: 读取数据 ◄─────────────────── 主存 (最新数据)
   ```

3. **ARM64 内存属性**：
   - 将sel4映射的内存属性设置为Device-nGnRnE
   - **Device-nGnRnE**:DEVICE_nGnRnE 是 ARMv8 (AArch64) 架构中定义的一种内存属性 (Memory Attribute)，在 seL4 内核中被用来配置页表项（Page Table Entry），以控制 CPU 如何访问特定的物理内存区域
   - 将共享内存区域配置为 “最严格的、非缓存的设备内存”。

---

## 3. 通信机制

### 3.1 环形队列原理

```
┌──────────────────────────────────────────────────────────┐
│  HyperampShmQueue (4KB 控制块)                           │
├──────────────────────────────────────────────────────────┤
│  header: 0 ──┐    tail: 0                                │
│  capacity: 256 │  block_size: 4096                       │
│  phy_addr: 0xDE000000                                    │
└──────────────┼───────────────────────────────────────────┘
               │
               ▼
┌──────────────────────────────────────────────────────────┐
│  Data Region (4MB)                                       │
├──────────────────────────────────────────────────────────┤
│  [Slot 0: 4KB] [Slot 1: 4KB] ... [Slot 255: 4KB]       │
│      ▲                                                   │
│      └─ header+1 = 写入位置                             │
└──────────────────────────────────────────────────────────┘
```

**入队 (Enqueue)**：
1. 获取自旋锁 `queue_lock`
2. 检查队列是否满：`(header + 1) % capacity == tail`
3. 写入数据到：`data_region + (header + 1) * block_size`
4. 更新 `header = (header + 1) % capacity`
5. 刷新缓存到主存（Uncached 模式下是内存屏障）
6. 释放自旋锁

**出队 (Dequeue)**：
1. 失效缓存（确保读取最新数据）
2. 获取自旋锁
3. 检查队列是否空：`tail == header`
4. 读取数据从：`data_region + (tail + 1) * block_size`
5. 更新 `tail = (tail + 1) % capacity`
6. 释放自旋锁

### 3.2 软件自旋锁

**为什么不用原子指令？**

ARM64 的原子指令（LDXR/STXR）在 Uncached 内存上**会触发异常**！

**纯软件实现**：
```c
// 获取锁
while (1) {
    if (lock->lock_value == 0) {
        lock->lock_value = 1;
        BARRIER();
        if (lock->lock_value == 1) {
            // 成功获取
            return;
        }
    }
    // 自旋等待
}

// 释放锁
lock->lock_value = 0;
BARRIER();
```

**关键点**：
- 使用 `volatile` 防止编译器优化
- 使用 `DMB/DSB` 内存屏障保证顺序
- 在 Uncached 内存上安全工作

---

## 4. 核心 API 接口

### 4.1 seL4 端 HyperAMP API

#### 4.1.1 队列管理API

##### `hyperamp_queue_init` - 初始化共享内存队列

```c
int hyperamp_queue_init(volatile HyperampShmQueue *queue, 
                        const HyperampQueueConfig *config,
                        int is_creator);
```

**功能**: 初始化共享内存队列控制块

**参数**:
- `queue`: 队列控制块指针（指向共享内存）
- `config`: 队列配置参数
  - `map_mode`: 内存映射模式 (通常为 `HYPERAMP_MAP_MODE_CONTIGUOUS_BOTH`)
  - `capacity`: 队列容量（元素数量，建议256）
  - `block_size`: 每个元素大小（字节，建议4096）
  - `phy_addr`: 物理地址
  - `virt_addr`: 虚拟地址
- `is_creator`: 是否为创建者（seL4=1, Linux=0）

**返回值**: `HYPERAMP_OK` 成功，`HYPERAMP_ERROR` 失败

**示例**:
```c
volatile HyperampShmQueue *g_tx_queue = (volatile HyperampShmQueue *)0x54E000;

HyperampQueueConfig tx_config = {
    .map_mode = HYPERAMP_MAP_MODE_CONTIGUOUS_BOTH,
    .capacity = 256,
    .block_size = 4096,
    .phy_addr = 0xDE000000,
    .virt_addr = 0x54E000
};

if (hyperamp_queue_init(g_tx_queue, &tx_config, 1) != HYPERAMP_OK) {
    printf("Failed to initialize TX queue\n");
    return -1;
}
```

##### `hyperamp_queue_enqueue` - 入队操作

```c
int hyperamp_queue_enqueue(volatile HyperampShmQueue *queue,
                          uint32_t zone_id,
                          const void *data,
                          size_t data_len,
                          volatile void *virt_base);
```

**功能**: 将消息写入共享内存队列

**参数**:
- `queue`: 队列控制块指针
- `zone_id`: 当前zone ID（seL4=1）
- `data`: 要发送的数据指针
- `data_len`: 数据长度（不超过block_size）
- `virt_base`: 数据区虚拟基址（通常为`g_data_region`）

**返回值**: `HYPERAMP_OK` 成功，`HYPERAMP_ERROR` 失败（队列满或参数错误）

**注意事项**:
- 会自动获取自旋锁
- 检查队列是否满
- 写入后会刷新缓存到主存

**示例**:
```c
uint8_t msg_buf[4096];
// ... 构造消息 ...

int ret = hyperamp_queue_enqueue(
    g_tx_queue,              // TX队列
    1,                       // zone_id (seL4 = 1)
    msg_buf,                 // 数据
    msg_len,                 // 长度
    g_data_region            // 数据区基址
);
```

##### `hyperamp_queue_dequeue` - 出队操作

```c
int hyperamp_queue_dequeue(volatile HyperampShmQueue *queue,
                          uint32_t zone_id,
                          void *data,
                          size_t max_len,
                          size_t *actual_len,
                          volatile void *virt_base);
```

**功能**: 从共享内存队列读取消息

**参数**:
- `queue`: 队列控制块指针
- `zone_id`: 当前zone ID（seL4=1）
- `data`: 接收缓冲区指针
- `max_len`: 缓冲区最大长度
- `actual_len`: 输出实际读取的长度
- `virt_base`: 数据区虚拟基址

**返回值**: `HYPERAMP_OK` 成功，`HYPERAMP_ERROR` 失败（队列空）

**注意事项**:
- 读取前会失效缓存
- 会自动获取自旋锁
- 如果队列为空，返回错误（非阻塞）

#### 4.1.2 前端协议栈API

##### `frontend_proxy_init` - 初始化前端上下文

```c
void frontend_proxy_init(FrontendProxyContext *ctx,
                        volatile HyperampShmQueue *tx_queue,
                        volatile HyperampShmQueue *rx_queue);
```

**功能**: 初始化前端协议栈上下文

**参数**:
- `ctx`: 前端上下文结构体指针
- `tx_queue`: 发送队列（seL4→Linux）
- `rx_queue`: 接收队列（Linux→seL4）

**示例**:
```c
FrontendProxyContext frontend_ctx;
frontend_proxy_init(&frontend_ctx, g_tx_queue, g_rx_queue);
```

##### `frontend_send_message` - 发送代理消息

```c
int frontend_send_message(FrontendProxyContext *ctx,
                         uint8_t msg_type,
                         uint16_t frontend_sess,
                         uint16_t backend_sess,
                         const void *payload,
                         uint16_t payload_len);
```

**功能**: 构造并发送HyperAMP代理消息

**参数**:
- `ctx`: 前端上下文
- `msg_type`: 消息类型
  - `HYPERAMP_MSG_TYPE_DEV` (0): 设备消息
  - `HYPERAMP_MSG_TYPE_STRGY` (1): 策略消息
  - `HYPERAMP_MSG_TYPE_SESS` (2): 会话消息
  - `HYPERAMP_MSG_TYPE_DATA` (3): 数据消息
- `frontend_sess`: 前端会话ID
- `backend_sess`: 后端会话ID
- `payload`: 载荷数据指针
- `payload_len`: 载荷长度

**返回值**: `HYPERAMP_OK` 成功，`HYPERAMP_ERROR` 失败

##### `frontend_receive_response` - 接收后端响应

```c
int frontend_receive_response(FrontendProxyContext *ctx,
                             HyperampMsgHeader *out_hdr,
                             void *out_payload,
                             size_t max_payload_len,
                             size_t *actual_len);
```

**功能**: 从RX队列接收后端响应

**参数**:
- `ctx`: 前端上下文
- `out_hdr`: 输出消息头
- `out_payload`: 输出载荷缓冲区
- `max_payload_len`: 缓冲区最大长度
- `actual_len`: 输出实际载荷长度

**返回值**: `HYPERAMP_OK` 成功，`HYPERAMP_ERROR` 失败（队列空）

**示例**:
```c
HyperampMsgHeader resp_hdr;
char resp_payload[4096];
size_t resp_len;

int ret = frontend_receive_response(&frontend_ctx, 
                                   &resp_hdr, 
                                   resp_payload, 
                                   sizeof(resp_payload), 
                                   &resp_len);
if (ret == HYPERAMP_OK) {
    printf("Received response: type=%u, len=%zu\n", 
           resp_hdr.proxy_msg_type, resp_len);
}
```

##### `frontend_tcp_connect` - 发起TCP连接

```c
int frontend_tcp_connect(FrontendProxyContext *ctx,
                        const char *dst_ip,
                        uint16_t dst_port);
```

**功能**: 发送SESSION消息请求建立TCP连接

**参数**:
- `ctx`: 前端上下文
- `dst_ip`: 目标IP地址（字符串格式，如"8.8.8.8"）
- `dst_port`: 目标端口

**返回值**: 前端会话ID（>0），失败返回-1

**示例**:
```c
int sess_id = frontend_tcp_connect(&frontend_ctx, "8.8.8.8", 80);
if (sess_id > 0) {
    printf("TCP connection initiated, session_id=%d\n", sess_id);
}
```

##### `frontend_http_get` - 发送HTTP GET请求

```c
int frontend_http_get(FrontendProxyContext *ctx,
                     uint16_t frontend_sess,
                     uint16_t backend_sess,
                     const char *host,
                     const char *uri);
```

**功能**: 发送HTTP GET请求

**参数**:
- `ctx`: 前端上下文
- `frontend_sess`: 前端会话ID
- `backend_sess`: 后端会话ID
- `host`: 主机名（如"www.example.com"）
- `uri`: 请求URI（如"/index.html"）

**返回值**: `HYPERAMP_OK` 成功，`HYPERAMP_ERROR` 失败

##### `frontend_http_post` - 发送HTTP POST请求

```c
int frontend_http_post(FrontendProxyContext *ctx,
                      uint16_t frontend_sess,
                      uint16_t backend_sess,
                      const char *host,
                      const char *uri,
                      const char *body);
```

**功能**: 发送HTTP POST请求

**参数**:
- `ctx`: 前端上下文
- `frontend_sess`: 前端会话ID
- `backend_sess`: 后端会话ID
- `host`: 主机名
- `uri`: 请求URI
- `body`: POST请求体

**返回值**: `HYPERAMP_OK` 成功，`HYPERAMP_ERROR` 失败

##### `hyperamp_server_main_loop` - 主消息循环

```c
void hyperamp_server_main_loop(void);
```

**功能**: HyperAMP服务器主循环，负责：
1. 初始化TX/RX队列（seL4作为创建者）
2. 运行前端协议栈测试场景
3. 轮询接收后端响应

**注意**: 此函数会无限循环运行，通常在seL4的主任务中调用

**完整使用示例**:
```c
// 1. 定义全局变量（由kernel映射到共享内存）
volatile HyperampShmQueue *g_tx_queue = (volatile HyperampShmQueue *)0x54E000;
volatile HyperampShmQueue *g_rx_queue = (volatile HyperampShmQueue *)0x54F000;
volatile void *g_data_region = (volatile void *)0x550000;

// 2. 初始化队列
HyperampQueueConfig tx_config = {
    .map_mode = HYPERAMP_MAP_MODE_CONTIGUOUS_BOTH,
    .capacity = 256,
    .block_size = 4096,
    .phy_addr = 0xDE000000,
    .virt_addr = 0x54E000
};

hyperamp_queue_init(g_tx_queue, &tx_config, 1);  // seL4是创建者

// 3. 初始化前端协议栈
FrontendProxyContext frontend_ctx;
frontend_proxy_init(&frontend_ctx, g_tx_queue, g_rx_queue);

// 4. 建立TCP连接
int sess_id = frontend_tcp_connect(&frontend_ctx, "8.8.8.8", 80);

// 5. 等待SESSION响应
HyperampMsgHeader resp_hdr;
char resp_payload[512];
size_t resp_len;

while (1) {
    if (frontend_receive_response(&frontend_ctx, &resp_hdr, 
                                 resp_payload, sizeof(resp_payload), 
                                 &resp_len) == HYPERAMP_OK) {
        if (resp_hdr.proxy_msg_type == HYPERAMP_MSG_TYPE_SESS) {
            uint16_t backend_sess = resp_hdr.backend_sess_id;
            
            // 6. 发送HTTP GET请求
            frontend_http_get(&frontend_ctx, sess_id, backend_sess,
                            "www.example.com", "/index.html");
            break;
        }
    }
    // 轮询延迟
    for (volatile int i = 0; i < 100000; i++);
}

// 7. 接收HTTP响应
while (1) {
    if (frontend_receive_response(&frontend_ctx, &resp_hdr,
                                 resp_payload, sizeof(resp_payload),
                                 &resp_len) == HYPERAMP_OK) {
        if (resp_hdr.proxy_msg_type == HYPERAMP_MSG_TYPE_DATA) {
            printf("HTTP Response: %s\n", resp_payload);
            break;
        }
    }
    for (volatile int i = 0; i < 100000; i++);
}
```

### 4.2 Linux 端 HyperAMP API

#### 4.2.1 初始化和清理API

##### `hyperamp_linux_init` - 初始化Linux端通信

```c
int hyperamp_linux_init(uint64_t phys_addr, int is_creator);
```

**功能**: 初始化Linux端HyperAMP通信，包括：
1. 映射物理内存到用户空间（通过`/dev/hvisor`获得uncached映射）
2. 设置TX/RX队列指针
3. 等待或初始化队列

**参数**:
- `phys_addr`: 共享内存物理地址（通常为`0xDE000000`）
- `is_creator`: 是否创建队列（通常为0，由seL4创建）

**返回值**: `HYPERAMP_OK` 成功，`HYPERAMP_ERROR` 失败

**内部实现**:
- 打开`/dev/hvisor`设备文件
- 使用`mmap()`映射4MB+共享内存（包含2个4KB队列+4MB数据区）
- 设置队列指针：
  - `RX Queue = shm_base + 0` (物理地址0xDE000000，接收seL4请求)
  - `TX Queue = shm_base + 4096` (物理地址0xDE001000，发送响应给seL4)
  - `Data Region = shm_base + 8192` (物理地址0xDE002000)

**示例**:
```c
#include "shm/hyperamp_shm_queue.h"

// 初始化HyperAMP（Linux作为连接者）
if (hyperamp_linux_init(0xDE000000, 0) != HYPERAMP_OK) {
    fprintf(stderr, "Failed to initialize HyperAMP\n");
    return -1;
}

printf("HyperAMP initialized successfully\n");
```

**重要注意事项**:
- Linux必须在seL4初始化队列**之后**才能连接
- 函数会轮询检查`capacity`字段，等待seL4初始化完成
- 使用`/dev/hvisor`而非`/dev/mem`以获得uncached内存属性

##### `hyperamp_linux_cleanup` - 清理资源

```c
void hyperamp_linux_cleanup(void);
```

**功能**: 清理HyperAMP资源，取消内存映射，关闭文件描述符

**示例**:
```c
// 程序退出时清理
signal(SIGINT, signal_handler);

void signal_handler(int sig) {
    hyperamp_linux_cleanup();
    exit(0);
}
```

##### `hyperamp_linux_get_status` - 获取通信状态

```c
void hyperamp_linux_get_status(void);
```

**功能**: 打印当前通信统计信息，包括：
- TX消息计数和错误数
- RX消息计数和错误数
- 队列状态

#### 4.2.2 消息收发API

##### `hyperamp_linux_send` - 发送消息到seL4

```c
int hyperamp_linux_send(uint8_t msg_type, 
                       uint16_t frontend_sess_id,
                       uint16_t backend_sess_id,
                       const void *payload, 
                       uint16_t payload_len);
```

**功能**: 发送HyperAMP消息到seL4（Linux→seL4）

**参数**:
- `msg_type`: 消息类型
  - `HYPERAMP_MSG_TYPE_SESS` (2): 会话响应
  - `HYPERAMP_MSG_TYPE_DATA` (3): 数据响应
- `frontend_sess_id`: 前端会话ID（从请求中获取）
- `backend_sess_id`: 后端会话ID（由Linux分配）
- `payload`: 载荷数据指针
- `payload_len`: 载荷长度（不超过4088字节）

**返回值**: `HYPERAMP_OK` 成功，`HYPERAMP_ERROR` 失败

**注意事项**:
- 发送前会检查TX队列是否已初始化（`capacity>0`）
- 自动调用`CACHE_INVALIDATE`失效缓存
- 使用`hyperamp_queue_enqueue`写入TX队列

**示例**:
```c
// 发送SESSION响应
SessionCreatePayload sess_resp = {
    .protocol = PROXY_PROTO_TCP,
    .state = PROXY_STATE_ESTABLISHED,
    .src_port = 51000,
    .dst_port = 80,
    // ... 其他字段 ...
};

int ret = hyperamp_linux_send(
    HYPERAMP_MSG_TYPE_SESS,      // 会话消息
    frontend_sess_id,             // 前端会话ID
    2000,                         // 分配的后端会话ID
    &sess_resp,
    sizeof(sess_resp)
);

if (ret == HYPERAMP_OK) {
    printf("SESSION response sent\n");
}
```

##### `hyperamp_linux_recv` - 接收来自seL4的请求

```c
int hyperamp_linux_recv(HyperampMsgHeader *hdr,
                       void *payload,
                       uint16_t max_payload_len,
                       uint16_t *actual_payload_len);
```

**功能**: 从RX队列接收seL4发送的请求消息

**参数**:
- `hdr`: 输出消息头结构体
- `payload`: 输出载荷缓冲区
- `max_payload_len`: 缓冲区最大长度
- `actual_payload_len`: 输出实际载荷长度

**返回值**: `HYPERAMP_OK` 成功，`HYPERAMP_ERROR` 失败（队列空或未初始化）

**注意事项**:
- 非阻塞调用，队列为空时立即返回错误
- 读取前会检查RX队列是否已初始化
- 自动调用`CACHE_INVALIDATE`失效缓存
- 使用`hyperamp_queue_dequeue`从RX队列读取

**示例**:
```c
HyperampMsgHeader req_hdr;
uint8_t payload[4096];
uint16_t payload_len;

// 轮询接收消息
while (1) {
    int ret = hyperamp_linux_recv(&req_hdr, payload, 
                                   sizeof(payload), &payload_len);
    
    if (ret == HYPERAMP_OK) {
        printf("Received message: type=%u, sess=%u, len=%u\n",
               req_hdr.proxy_msg_type,
               req_hdr.frontend_sess_id,
               payload_len);
        
        // 处理消息
        switch (req_hdr.proxy_msg_type) {
            case HYPERAMP_MSG_TYPE_SESS:
                handle_session(&req_hdr, payload, payload_len);
                break;
            case HYPERAMP_MSG_TYPE_DATA:
                handle_data(&req_hdr, payload, payload_len);
                break;
        }
    }
    
    usleep(1000);  // 1ms轮询间隔
}
```

##### `hyperamp_linux_send_data` - 便捷发送数据消息

```c
int hyperamp_linux_send_data(uint16_t session_id, 
                            const void *data, 
                            uint16_t data_len);
```

**功能**: 便捷函数，发送DATA类型消息

**参数**:
- `session_id`: 会话ID
- `data`: 数据指针
- `data_len`: 数据长度

**返回值**: `HYPERAMP_OK` 成功，`HYPERAMP_ERROR` 失败

##### `hyperamp_linux_has_message` - 检查是否有消息

```c
int hyperamp_linux_has_message(void);
```

**功能**: 非阻塞检查RX队列是否有消息

**返回值**: 1表示有消息，0表示无消息或未初始化

**示例**:
```c
if (hyperamp_linux_has_message()) {
    hyperamp_linux_recv(&hdr, payload, sizeof(payload), &len);
}
```

#### 4.2.3 底层队列API（与seL4共用）

##### `hyperamp_queue_enqueue` - 底层入队操作

```c
int hyperamp_queue_enqueue(volatile HyperampShmQueue *queue,
                          uint32_t zone_id,
                          const void *data,
                          size_t data_len,
                          volatile void *virt_base);
```

**参数**:
- `zone_id`: Linux端使用`ZONE_ID_LINUX` (0)

（其他参数和seL4端相同，详见4.1.1节）

##### `hyperamp_queue_dequeue` - 底层出队操作

```c
int hyperamp_queue_dequeue(volatile HyperampShmQueue *queue,
                          uint32_t zone_id,
                          void *data,
                          size_t max_len,
                          size_t *actual_len,
                          volatile void *virt_base);
```

**参数**:
- `zone_id`: Linux端使用`ZONE_ID_LINUX` (0)

（其他参数和seL4端相同，详见4.1.1节）

#### 4.2.4 完整的后端服务器示例

```c
#include <stdio.h>
#include <signal.h>
#include <unistd.h>
#include "shm/hyperamp_shm_queue.h"

static volatile int g_running = 1;
static uint16_t g_next_backend_sess = 2000;

// 信号处理：Ctrl+C退出
void signal_handler(int sig) {
    g_running = 0;
    hyperamp_linux_cleanup();
}

// 处理SESSION消息
void handle_session(HyperampMsgHeader *hdr, void *payload, uint16_t len) {
    printf("SESSION request from seL4: frontend_sess=%u\n", 
           hdr->frontend_sess_id);
    
    // 分配后端会话ID
    uint16_t backend_sess = g_next_backend_sess++;
    
    // 构造SESSION响应
    SessionCreatePayload *req = (SessionCreatePayload *)payload;
    SessionCreatePayload resp = *req;
    resp.state = PROXY_STATE_ESTABLISHED;
    
    // 发送响应
    hyperamp_linux_send(HYPERAMP_MSG_TYPE_SESS,
                       hdr->frontend_sess_id,
                       backend_sess,
                       &resp,
                       sizeof(resp));
    
    printf("SESSION response sent: backend_sess=%u\n", backend_sess);
}

// 处理DATA消息
void handle_data(HyperampMsgHeader *hdr, void *payload, uint16_t len) {
    printf("DATA request from seL4: %u bytes\n", len);
    
    // 模拟HTTP响应
    const char *http_resp = 
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/html\r\n"
        "Content-Length: 65\r\n\r\n"
        "<html><body><h1>Hello from Backend!</h1></body></html>";
    
    // 发送响应
    hyperamp_linux_send(HYPERAMP_MSG_TYPE_DATA,
                       hdr->frontend_sess_id,
                       hdr->backend_sess_id,
                       http_resp,
                       strlen(http_resp));
    
    printf("DATA response sent\n");
}

int main(void) {
    signal(SIGINT, signal_handler);
    
    printf("========================================\n");
    printf("  HyperAMP Backend Proxy (Linux)\n");
    printf("========================================\n\n");
    
    // 1. 初始化HyperAMP
    if (hyperamp_linux_init(0xDE000000, 0) != HYPERAMP_OK) {
        fprintf(stderr, "Failed to initialize HyperAMP\n");
        return 1;
    }
    
    printf("Ready to receive requests from seL4\n");
    printf("Press Ctrl+C to exit\n\n");
    
    // 2. 主循环：接收并处理消息
    HyperampMsgHeader req_hdr;
    uint8_t payload[4096];
    uint16_t payload_len;
    
    while (g_running) {
        int ret = hyperamp_linux_recv(&req_hdr, payload,
                                       sizeof(payload), &payload_len);
        
        if (ret == HYPERAMP_OK) {
            // 根据消息类型分发处理
            switch (req_hdr.proxy_msg_type) {
                case HYPERAMP_MSG_TYPE_SESS:
                    handle_session(&req_hdr, payload, payload_len);
                    break;
                    
                case HYPERAMP_MSG_TYPE_DATA:
                    handle_data(&req_hdr, payload, payload_len);
                    break;
                    
                default:
                    printf("Unknown message type: %u\n", 
                           req_hdr.proxy_msg_type);
                    break;
            }
        }
        
        usleep(1000);  // 1ms轮询间隔
    }
    
    // 3. 清理资源
    hyperamp_linux_get_status();  // 打印统计信息
    hyperamp_linux_cleanup();
    
    printf("Backend proxy terminated\n");
    return 0;
}
```

**编译命令**:
```bash
gcc -o hyperamp_backend \
    hyperamp_backend_proxy_sim.c \
    shm/hyperamp_linux_shm.c \
    -I./include \
    -lpthread
```

**运行**:
```bash
sudo ./hyperamp_backend
```

---

## 5. 关键注意事项

### 5.1 Cache 一致性 ⚠️

**问题**：CPU 缓存导致的数据不一致

**解决方案**：
1. **使用 Uncached 内存属性** (DEVICE_nGnRnE)
2. **内存屏障**：
   ```c
   #define HYPERAMP_DMB()   __asm__ volatile("dmb sy" ::: "memory")
   #define HYPERAMP_DSB()   __asm__ volatile("dsb sy" ::: "memory")
   #define HYPERAMP_BARRIER()   do { HYPERAMP_DMB(); HYPERAMP_DSB(); } while(0)
   ```
3. **在每次操作后调用屏障**：
   ```c
   // 写入后
   HYPERAMP_BARRIER();
   
   // 读取前
   HYPERAMP_BARRIER();
   ```

### 5.2 字节对齐 ⚠️

**问题**：非对齐访问在 Uncached 内存上可能失败

**解决方案**：
1. **使用 `__attribute__((packed))`**：
   ```c
   typedef struct {
       uint8_t  version;
       uint8_t  proxy_msg_type;
       uint16_t frontend_sess_id;
       uint16_t backend_sess_id;
       uint16_t payload_len;
   } __attribute__((packed)) HyperampMsgHeader;
   ```

2. **逐字节访问**：
   ```c
   static inline void hyperamp_safe_memcpy(volatile void *dst,
                                            const volatile void *src,
                                            size_t len)
   {
       volatile uint8_t *d = (volatile uint8_t *)dst;
       const volatile uint8_t *s = (const volatile uint8_t *)src;
       for (size_t i = 0; i < len; i++) {
           d[i] = s[i];
       }
       HYPERAMP_BARRIER();
   }
   ```

### 5.3 初始化顺序 ⚠️

**关键原则**：Linux 先启动并映射共享内存，轮询等待 seL4 发送消息；seL4 后启动，初始化队列并开始通信。

#### 5.3.1 初始化流程图

```
时间线 →

┌──────────────────────────────────────────────────────────────────┐
│  Step 1: 加载内核驱动                                            │
└──────────────────────────────────────────────────────────────────┘
    │
    └─► 在 Linux 中执行:
          sudo insmod hvisor.ko
          - 创建 /dev/hvisor 设备节点
          - 注册 mmap 接口，支持 uncached 内存映射
          ✓ 驱动加载成功


┌──────────────────────────────────────────────────────────────────┐
│  Step 2: Linux 用户态程序启动 (先启动，等待 seL4)               │
└──────────────────────────────────────────────────────────────────┘
    │
    ├─► 打开 /dev/hvisor 设备文件:
    │     fd = open("/dev/hvisor", O_RDWR | O_SYNC)
    │
    ├─► 映射共享内存到用户空间:
    │     shm_base = mmap(NULL, 4MB+8KB, PROT_READ|PROT_WRITE, 
    │                      MAP_SHARED, fd, 0xDE000000)
    │     - 驱动自动应用 DEVICE_nGnRnE 属性（uncached）
    │     - RX Queue: VA 0xFFFF9276D000 (对应 PA 0xDE000000)
    │     - TX Queue: VA 0xFFFF9276E000 (对应 PA 0xDE001000)
    │     - Data Region: VA 0xFFFF9276F000 (对应 PA 0xDE002000)
    │     ✓ 内存映射成功
    │
    ├─► 设置队列指针（注意：Linux 此时**不初始化**队列）:
    │     g_ctx.rx_queue = shm_base + 0;      // PA 0xDE000000
    │     g_ctx.tx_queue = shm_base + 4096;   // PA 0xDE001000
    │     g_ctx.data_region = shm_base + 8192; // PA 0xDE002000
    │
    ├─► 进入轮询等待状态（等待 seL4 初始化队列）:
    │     printf("Waiting for seL4 to initialize queues...\n");
    │     
    │     while (1) {
    │         // 检查 RX Queue（seL4 的 TX Queue）是否已初始化
    │         CACHE_INVALIDATE(rx_queue);
    │         uint16_t capacity = read_u16(rx_queue, offset_of(capacity));
    │         
    │         if (capacity == 256) {
    │             printf("✓ RX Queue initialized by seL4\n");
    │             break;  // 队列已就绪
    │         }
    │         
    │         // 队列未就绪，继续等待
    │         usleep(10000);  // 休眠 10ms
    │     }
    │
    │     while (1) {
    │         // 检查 TX Queue（seL4 的 RX Queue）是否已初始化
    │         CACHE_INVALIDATE(tx_queue);
    │         uint16_t capacity = read_u16(tx_queue, offset_of(capacity));
    │         
    │         if (capacity == 256) {
    │             printf("✓ TX Queue initialized by seL4\n");
    │             break;
    │         }
    │         
    │         usleep(10000);
    │     }
    │
    └─► Linux 准备就绪，开始轮询接收消息:
          printf("Ready to receive messages from seL4\n");
          
          while (g_running) {
              ret = hyperamp_linux_recv(&req_hdr, payload, ...);
              if (ret == HYPERAMP_OK) {
                  // 处理消息并响应
              }
              usleep(1000);  // 1ms 轮询间隔
          }


┌──────────────────────────────────────────────────────────────────┐
│  Step 3: seL4 启动 (后启动，作为队列创建者)                      │
└──────────────────────────────────────────────────────────────────┘
    │
    ├─► Kernel 映射共享内存到虚拟地址:
    │     - TX Queue: PA 0xDE000000 → VA 0x54E000 (DEVICE_nGnRnE)
    │     - RX Queue: PA 0xDE001000 → VA 0x54F000 (DEVICE_nGnRnE)
    │     - Data Region: PA 0xDE002000 → VA 0x550000 (4MB)
    │     ✓ 内核页表配置完成
    │
    ├─► 用户态初始化 TX Queue (seL4 → Linux):
    │     hyperamp_queue_init(g_tx_queue, &tx_config, 1)  // is_creator=1
    │     - 写入 capacity = 256         ← Linux 轮询检测此字段
    │     - 写入 block_size = 4096
    │     - 写入 phy_addr = 0xDE000000
    │     - 写入 header = 0, tail = 0
    │     - 初始化 spinlock
    │     - 刷新缓存到主存 (DMB/DSB)
    │     ✓ TX Queue 初始化完成
    │
    ├─► 初始化 RX Queue (Linux → seL4):
    │     hyperamp_queue_init(g_rx_queue, &rx_config, 1)
    │     - 写入 capacity = 256
    │     - 写入 block_size = 4096
    │     - 写入 phy_addr = 0xDE001000
    │     - 刷新缓存到主存
    │     ✓ RX Queue 初始化完成
    │
    │     [此时 Linux 检测到 capacity=256，退出等待循环]
    │
    └─► seL4 准备就绪，开始发送请求:
          printf("Sending SESSION message to Linux...\n");
          frontend_tcp_connect(&frontend_ctx, "8.8.8.8", 80);


┌──────────────────────────────────────────────────────────────────┐
│  Step 4: 正常通信 (双向消息传递)                                 │
└──────────────────────────────────────────────────────────────────┘
    │
    ├─► seL4 发送 SESSION 请求:
    │     hyperamp_queue_enqueue(g_tx_queue, ...)  // 写入 PA 0xDE000000
    │     - 获取 spinlock
    │     - 写入消息到 data_region[header+1]
    │     - 更新 header 指针
    │     - 刷新缓存 (DMB/DSB)
    │     - 释放 spinlock
    │     ✓ 消息已发送
    │
    ├─► Linux 接收 SESSION 请求:
    │     hyperamp_linux_recv(...)  // 从 PA 0xDE000000 读取
    │     - 失效缓存 (CACHE_INVALIDATE)
    │     - 获取 spinlock
    │     - 读取消息从 data_region[tail+1]
    │     - 更新 tail 指针
    │     - 释放 spinlock
    │     ✓ 收到 SESSION 请求
    │     
    │     handle_session():
    │       - 分配 backend_sess_id = 2000
    │       - 构造 SESSION 响应
    │
    ├─► Linux 发送 SESSION 响应:
    │     hyperamp_linux_send(...)  // 写入 PA 0xDE001000
    │     - 失效缓存（确保读取最新队列状态）
    │     - 获取 spinlock
    │     - 写入消息到 data_region[header+1]
    │     - 更新 header 指针
    │     - 刷新缓存
    │     - 释放 spinlock
    │     ✓ 响应已发送
    │
    ├─► seL4 接收 SESSION 响应:
    │     hyperamp_queue_dequeue(g_rx_queue, ...)  // 从 PA 0xDE001000 读取
    │     - 失效缓存
    │     - 获取 spinlock
    │     - 读取消息
    │     - 更新 tail 指针
    │     - 释放 spinlock
    │     ✓ 收到 backend_sess_id=2000
    │
    ├─► seL4 发送 HTTP GET 请求 (DATA 消息):
    │     frontend_http_get(&frontend_ctx, sess_id, 2000, ...)
    │     ✓ DATA 消息已发送
    │
    ├─► Linux 接收并处理 HTTP 请求:
    │     handle_data():
    │       - 解析 HTTP 请求
    │       - 生成 HTTP 响应
    │     ✓ 处理完成
    │
    ├─► Linux 发送 HTTP 响应:
    │     hyperamp_linux_send(HYPERAMP_MSG_TYPE_DATA, ...)
    │     ✓ HTTP 响应已发送
    │
    └─► seL4 接收 HTTP 响应:
          printf("Received HTTP response: %s\n", payload);
          ✓ 通信完成
```

#### 5.3.2 时序图

```
Linux (Zone 0)                   Physical Memory              seL4 (Zone 1)
     │                                  │                           │
     │  1. insmod hvisor.ko             │                           │
     │     创建 /dev/hvisor              │                           │
     │                                   │                           │
     │  2. open(/dev/hvisor)            │                           │
     ├────────────────────────────────► │                           │
     │                                   │                           │
     │  3. mmap(PA 0xDE000000)          │                           │
     ├────────────────────────────────► │                           │
     │    PA 0xDE000000 → VA 0xFFFF...  │                           │
     │    (DEVICE_nGnRnE)                │                           │
     │    映射 4MB+8KB 共享内存          │                           │
     │                                   │                           │
     │  4. 轮询等待 seL4 初始化          │                           │
     │    while (capacity == 0) {        │                           │
     │      CACHE_INVALIDATE(0xDE000000);│                           │
     │◄────┼───capacity=0 ◄──────────────┤                           │
     │ ┌───┘  usleep(10ms)               │                           │
     │ │                                 │                           │
     │ │  ... 等待 seL4 启动 ...         │                           │
     │ │                                 │                           │
     │ │                                 │  5. Kernel boot           │
     │ │                                 │                           │
     │ │                                 │  6. map_it_frame_cap()    │
     │ │                                 │ ◄─────────────────────────┤
     │ │                                 │    PA 0xDE000000 → VA 0x54E000
     │ │                                 │    (DEVICE_nGnRnE)        │
     │ │                                 │                           │
     │ │                                 │  7. hyperamp_queue_init() │
     │ │                                 │    capacity=256 ───────►  │
     │ │                                 │    block_size=4096        │
     │ │                                 ├───────────────────────────┤
     │ │     capacity=256 ◄──────────────┤   (写入主存)              │
     │ └────►✓ 检测到已初始化            │    BARRIER()              │
     │    }  break;                      │                           │
     │                                   │                           │
     │  8. 进入接收循环                  │                           │
     │    while (g_running) {            │                           │
     │      ret = hyperamp_linux_recv(); │                           │
     │◄────┐  if (ret == OK) {...}       │                           │
     │ ┌───┘  usleep(1ms)                │                           │
     │ │                                 │                           │
     │ │                                 │  9. enqueue(TX, SESSION)  │
     │ │                                 │ ─────────────────────────►│
     │ │                                 │    写入 0xDE000000+4KB*N  │
     │ │ capacity=256 ◄──────────────────┤    BARRIER()              │
     │ └───► 10. dequeue(RX, SESSION) ◄──┤                           │
     │       CACHE_INVALIDATE            │                           │
     │       读取 0xDE000000+4KB*N       │                           │
     │       ✓ 收到 SESSION 请求         │                           │
     │                                   │                           │
     │  11. handle_session()             │                           │
     │      分配 backend_sess=2000       │                           │
     │                                   │                           │
     │  12. enqueue(TX, SESSION resp)    │                           │
     ├────────────────────────────────►  │                           │
     │     写入 0xDE001000+4KB*M         │                           │
     │     BARRIER()                     │                           │
     │                                   │                           │
     │                                   │  13. dequeue(RX, resp)    │
     │                                   │ ◄─────────────────────────┤
     │                                   │    CACHE_INVALIDATE       │
     │                                   │    读取 0xDE001000+4KB*M  │
     │                                   │    ✓ 收到 backend_sess=2000
     │                                   │                           │
     │                                   │  14. enqueue(TX, HTTP GET)│
     │                                   │ ─────────────────────────►│
     │  15. dequeue(RX, HTTP GET) ◄──────┤                           │
     │      ✓ 收到 DATA 请求             │                           │
     │                                   │                           │
     │  16. handle_data()                │                           │
     │      生成 HTTP 响应               │                           │
     │                                   │                           │
     │  17. enqueue(TX, HTTP resp)       │                           │
     ├────────────────────────────────►  │                           │
     │                                   │                           │
     │                                   │  18. dequeue(RX, resp)    │
     │                                   │ ◄─────────────────────────┤
     │                                   │    ✓ 收到 HTTP 响应       │
     │                                   │                           │
     ▼                                   ▼                           ▼
```

#### 5.3.3 关键检查点

**Linux 端（先启动，连接者）**:
```c
// Step 1: 加载驱动
// sudo insmod hvisor.ko

// Step 2: 打开设备并映射共享内存
int fd = open("/dev/hvisor", O_RDWR | O_SYNC);
void *shm_base = mmap(NULL, 4*1024*1024 + 8192, 
                      PROT_READ | PROT_WRITE,
                      MAP_SHARED, fd, 0xDE000000);

g_ctx.rx_queue = (volatile HyperampShmQueue *)shm_base;           // PA 0xDE000000
g_ctx.tx_queue = (volatile HyperampShmQueue *)(shm_base + 4096);  // PA 0xDE001000
g_ctx.data_region = (volatile void *)(shm_base + 8192);           // PA 0xDE002000

// Step 3: 轮询等待 seL4 初始化队列
printf("Waiting for seL4 to initialize queues...\n");

while (1) {
    CACHE_INVALIDATE(g_ctx.rx_queue);
    uint16_t capacity = hyperamp_safe_read_u16(
        g_ctx.rx_queue, 
        offsetof(HyperampShmQueue, capacity)
    );
    
    if (capacity == 0) {
        // seL4 尚未启动或未初始化
        usleep(10000);  // 等待 10ms
        continue;
    } else if (capacity == 256) {
        printf("✓ RX Queue initialized by seL4\n");
        break;  // 队列就绪
    } else {
        printf("ERROR: Unexpected capacity value: %u\n", capacity);
        return -1;
    }
}

// Step 4: 开始接收消息
printf("Ready to receive messages from seL4\n");
while (g_running) {
    ret = hyperamp_linux_recv(&req_hdr, payload, sizeof(payload), &len);
    if (ret == HYPERAMP_OK) {
        // 处理消息
    }
    usleep(1000);  // 1ms 轮询间隔
}
```

**seL4 端（后启动，创建者）**:
```c
// Step 1: 内核已映射共享内存（由 kernel 配置完成）
// TX Queue: PA 0xDE000000 → VA 0x54E000
// RX Queue: PA 0xDE001000 → VA 0x54F000

// Step 2: 用户态初始化队列（is_creator=1）
HyperampQueueConfig tx_config = {
    .capacity = 256,
    .block_size = 4096,
    .phy_addr = 0xDE000000,
    .virt_addr = 0x54E000
};

if (hyperamp_queue_init(g_tx_queue, &tx_config, 1) != HYPERAMP_OK) {
    printf("ERROR: Failed to initialize TX queue\n");
    return -1;
}

// ⚠️ 关键：初始化后必须刷新缓存到主存
hyperamp_cache_clean((volatile void *)g_tx_queue, sizeof(HyperampShmQueue));

// Step 3: 初始化 RX Queue
HyperampQueueConfig rx_config = {
    .capacity = 256,
    .block_size = 4096,
    .phy_addr = 0xDE001000,
    .virt_addr = 0x54F000
};

hyperamp_queue_init(g_rx_queue, &rx_config, 1);
hyperamp_cache_clean((volatile void *)g_rx_queue, sizeof(HyperampShmQueue));

printf("✓ Queues initialized, Linux can now detect them\n");

// Step 4: 开始发送请求
int sess_id = frontend_tcp_connect(&frontend_ctx, "8.8.8.8", 80);
```

#### 5.3.4 常见错误及解决方案

**错误 1**: Linux 启动后立即发送消息，不等待 seL4

```
❌ 症状：
[Linux] hyperamp_linux_send() 失败，TX Queue capacity=0
[Linux] 尝试入队时返回 HYPERAMP_ERROR

✅ 解决方案：
- Linux 必须在映射内存后进入轮询等待状态
- 检测 RX Queue 和 TX Queue 的 capacity 字段是否为 256
- 只有检测到队列已初始化后才能开始通信
```

**错误 2**: 双方都尝试创建队列（is_creator=1）

```
❌ 症状：
数据竞争，队列状态不一致，header/tail 指针混乱

✅ 解决方案：
- seL4: is_creator=1 （后启动，但是创建者）
- Linux: is_creator=0 （先启动，只映射不初始化）
- 明确角色分工：seL4 负责初始化，Linux 负责等待
```

**错误 3**: 忘记刷新/失效缓存

```
❌ 症状：
seL4 初始化后，Linux 仍然读取到 capacity=0
或 seL4 发送消息后，Linux 读取到的是旧数据

✅ 解决方案：
- seL4 初始化后调用 hyperamp_cache_clean() 或 BARRIER()
- Linux 每次读取前调用 CACHE_INVALIDATE()
- 确保使用 DEVICE_nGnRnE 内存属性（通过 /dev/hvisor 自动设置）
```

**错误 4**: 使用 magic 字段判断初始化

```
❌ 问题：
magic 字段在 offset 4052，超出 4KB 页边界
如果只映射了 4KB，访问 magic 会导致段错误

✅ 解决方案：
- 使用 capacity 字段（offset 6，在第一页内）
- capacity == 256 表示队列已初始化
- 映射至少 4MB+8KB 以包含完整的队列和数据区
```

**错误 5**: Linux 端先调用 hyperamp_queue_init()

```
❌ 症状：
seL4 和 Linux 同时写入队列控制块，导致数据不一致

✅ 解决方案：
- Linux 端**不要**调用 hyperamp_queue_init()
- Linux 只负责映射内存和轮询等待
- 所有队列初始化由 seL4 完成
```

#### 5.3.5 初始化状态机

```
┌───────────────────────────────────────────────────────────────┐
│ Linux State Machine (先启动)                                  │
└───────────────────────────────────────────────────────────────┘

┌─────────────┐
│   BOOT      │  Linux 系统启动
└──────┬──────┘
       │
       ▼
┌─────────────┐
│ LOAD_DRIVER │  sudo insmod hvisor.ko
└──────┬──────┘  创建 /dev/hvisor 设备节点
       │
       ▼
┌─────────────┐
│ OPEN_DEV    │  fd = open("/dev/hvisor", O_RDWR | O_SYNC)
└──────┬──────┘
       │
       ▼
┌─────────────┐
│  MMAP_SHM   │  shm_base = mmap(..., PA 0xDE000000)
└──────┬──────┘  映射 4MB+8KB 共享内存（uncached）
       │         RX Queue = shm_base + 0 (PA 0xDE000000)
       │         TX Queue = shm_base + 4096 (PA 0xDE001000)
       │         Data Region = shm_base + 8192 (PA 0xDE002000)
       ▼
┌─────────────┐
│ WAIT_INIT   │  printf("Waiting for seL4 to initialize queues...")
└──────┬──────┘
       │
       │ ◄─────────────┐ 
       ▼                │ capacity == 0 (seL4 未启动)
┌─────────────┐        │
│ POLL_CHECK  │────────┘ usleep(10ms)
└──────┬──────┘  CACHE_INVALIDATE(rx_queue)
       │          capacity = read_u16(rx_queue, offset_of(capacity))
       │ capacity == 256 (检测到 seL4 已初始化)
       ▼
┌─────────────┐
│   READY     │  printf("Ready to receive messages from seL4")
└──────┬──────┘  准备就绪，进入接收循环
       │
       ▼
┌─────────────┐
│  RECV_LOOP  │  while (g_running) {
└──────┬──────┘    ret = hyperamp_linux_recv(...)
       │            if (ret == OK) { handle_message() }
       │◄───────┐   usleep(1ms)
       └────────┘  }


┌───────────────────────────────────────────────────────────────┐
│ seL4 State Machine (后启动)                                   │
└───────────────────────────────────────────────────────────────┘

┌─────────────┐
│   BOOT      │  seL4 Kernel 启动
└──────┬──────┘
       │
       ▼
┌─────────────┐
│ MAP_MEMORY  │  Kernel 映射共享内存到虚拟地址空间
└──────┬──────┘  TX Queue: PA 0xDE000000 → VA 0x54E000 (DEVICE_nGnRnE)
       │         RX Queue: PA 0xDE001000 → VA 0x54F000 (DEVICE_nGnRnE)
       │         Data Region: PA 0xDE002000 → VA 0x550000 (4MB)
       ▼
┌─────────────┐
│ INIT_TX_Q   │  hyperamp_queue_init(g_tx_queue, ..., is_creator=1)
└──────┬──────┘  写入 capacity=256, block_size=4096, header=0, tail=0
       │         BARRIER() 刷新缓存到主存
       │         [此时 Linux 检测到 capacity=256，退出等待循环]
       ▼
┌─────────────┐
│ INIT_RX_Q   │  hyperamp_queue_init(g_rx_queue, ..., is_creator=1)
└──────┬──────┘  写入 capacity=256, block_size=4096
       │         BARRIER() 刷新缓存
       ▼
┌─────────────┐
│   READY     │  printf("Queues initialized")
└──────┬──────┘  准备就绪，可以发送请求
       │
       ▼
┌─────────────┐
│  SEND_MSG   │  frontend_tcp_connect(&frontend_ctx, "8.8.8.8", 80)
└──────┬──────┘  hyperamp_queue_enqueue(g_tx_queue, ...)
       │
       ▼
┌─────────────┐
│  RECV_RESP  │  while (1) {
└──────┬──────┘    ret = hyperamp_queue_dequeue(g_rx_queue, ...)
       │            if (ret == OK) { process_response() }
       │◄───────┐   usleep(轮询延迟)
       └────────┘  }
```

#### 5.3.6 调试技巧

**Linux 端：监控初始化等待过程**:
```c
// 在 hyperamp_linux_init() 中添加详细日志
printf("[Linux] Waiting for seL4 to initialize queues...\n");
printf("[Linux] RX Queue VA: %p (PA: 0x%lx)\n", g_ctx.rx_queue, 0xDE000000UL);
printf("[Linux] TX Queue VA: %p (PA: 0x%lx)\n", g_ctx.tx_queue, 0xDE001000UL);

int wait_count = 0;
while (1) {
    CACHE_INVALIDATE(g_ctx.rx_queue);
    uint16_t capacity = hyperamp_safe_read_u16(
        g_ctx.rx_queue, 
        offsetof(HyperampShmQueue, capacity)
    );
    
    if (capacity == 256) {
        printf("[Linux] ✓ RX Queue initialized after %d iterations (%.1f seconds)\n", 
               wait_count, wait_count * 0.01);
        break;
    }
    
    wait_count++;
    if (wait_count % 100 == 0) {  // 每秒打印一次
        printf("[Linux] Still waiting for seL4... (%d iterations, capacity=%u)\n", 
               wait_count, capacity);
    }
    
    usleep(10000);  // 10ms
}

printf("[Linux] Ready to receive messages from seL4\n");
```

**seL4 端：打印队列初始化状态**:
```c
// 初始化前查看内存状态
printf("[seL4] TX Queue BEFORE init (first 16 bytes): ");
volatile uint8_t *p = (volatile uint8_t *)g_tx_queue;
for (int i = 0; i < 16; i++) {
    printf("%02x ", p[i]);
}
printf("\n");

// 执行初始化
HyperampQueueConfig tx_config = { ... };
hyperamp_queue_init(g_tx_queue, &tx_config, 1);
hyperamp_cache_clean((volatile void *)g_tx_queue, sizeof(HyperampShmQueue));

// 初始化后验证
hyperamp_cache_invalidate((volatile void *)g_tx_queue, 64);
printf("[seL4] TX Queue AFTER init (first 16 bytes): ");
for (int i = 0; i < 16; i++) {
    printf("%02x ", p[i]);
}
printf("\n");

// 验证 capacity 字段
uint16_t capacity = hyperamp_safe_read_u16(g_tx_queue, 
                                            offsetof(HyperampShmQueue, capacity));
printf("[seL4] TX Queue capacity: %u (expected: 256)\n", capacity);
```

**验证物理地址映射**:
```c
// seL4 端
uint64_t pa = hyperamp_safe_read_u64(g_tx_queue, 
                                      offsetof(HyperampShmQueue, phy_addr));
printf("[seL4] TX Queue phy_addr: 0x%lx (expected: 0xDE000000)\n", pa);

// Linux 端
uint64_t pa = hyperamp_safe_read_u64(g_ctx.rx_queue, 
                                      offsetof(HyperampShmQueue, phy_addr));
printf("[Linux] RX Queue phy_addr: 0x%lx (expected: 0xDE000000)\n", pa);
```

**检查启动顺序是否正确**:
```bash
# 正确的启动流程：

# Terminal 1: Linux 端（先启动）
sudo insmod driver/hvisor.ko
sudo ./tools/hyperamp_backend
# 输出应该显示：
# [Linux] Waiting for seL4 to initialize queues...
# [Linux] Still waiting for seL4... (100 iterations, capacity=0)
# [Linux] Still waiting for seL4... (200 iterations, capacity=0)
# ...

# Terminal 2: seL4 端（后启动）
# 启动 seL4 系统
# 输出应该显示：
# [seL4] TX Queue BEFORE init: 00 00 00 00 00 00 00 00 ...
# [seL4] Initializing TX Queue...
# [seL4] TX Queue AFTER init: 00 00 00 00 00 00 00 01 00 10 00 ...
#                                             ↑capacity=256

# Terminal 1: Linux 端（自动检测到）
# [Linux] ✓ RX Queue initialized after 237 iterations (2.4 seconds)
# [Linux] ✓ TX Queue initialized
# [Linux] Ready to receive messages from seL4

# ✅ 如果看到这些输出，说明初始化顺序正确
```

**常见问题诊断**:
```c
// 问题：Linux 永远等不到 seL4
// 诊断步骤：
printf("[Linux] Checking memory mapping:\n");
printf("  shm_base = %p\n", shm_base);
printf("  rx_queue = %p\n", g_ctx.rx_queue);
printf("  tx_queue = %p\n", g_ctx.tx_queue);

// 手动读取前 64 字节
printf("[Linux] RX Queue raw bytes:\n");
for (int i = 0; i < 64; i++) {
    if (i % 16 == 0) printf("  %04x: ", i);
    printf("%02x ", ((volatile uint8_t *)g_ctx.rx_queue)[i]);
    if (i % 16 == 15) printf("\n");
}

// 如果全是 0xFF 或随机值，说明：
// 1. 物理地址映射错误
// 2. seL4 还没启动
// 3. seL4 启动失败
```




### 5.4 队列容量规划

**当前配置**：
- Capacity: 256 条消息
- Block Size: 4096 字节/消息
- 总数据区: 4 MB = 1024 条消息的容量（但队列只能存 256 条索引）

**后续可调整如下**：
```c
// 高吞吐场景
capacity = 512;
block_size = 2048;  // 2KB per message

// 大消息场景
capacity = 128;
block_size = 8192;  // 8KB per message
```

### 5.5 错误处理

```c
// 1. 队列满
if (ret == HYPERAMP_ERROR) {
    printf("Queue full, retry later\n");
    usleep(1000);
}

// 2. 队列空
if (ret == HYPERAMP_ERROR) {
    // Normal, just continue polling
}

// 3. 锁超时（如果实现了超时机制）
if (spin_count > MAX_SPIN) {
    printf("Lock timeout, possible deadlock\n");
}
```

---

## 6. 测试验证

### 6.1 测试场景

根据实际运行日志，已验证的场景：
通信流程:
seL4 端:

发送消息: 2 条 (1 SESSION + 1 DATA)
接收响应: 2 条 (1 SESSION Response + 1 DATA Response)
Linux 端:

接收消息: 2 条
发送响应: 2 条

✅ seL4 → Linux: SESSION 请求 (TCP Connect to 8.8.8.8:80)
✅ Linux → seL4: SESSION 响应 (backend_sess=2000)
✅ seL4 → Linux: DATA 请求 (HTTP GET)
✅ Linux → seL4: DATA 响应 (HTTP 200 OK with HTML)

测试结果：
✅ **Scenario 1: TCP Connect**
- seL4 发送 SESSION 消息
- Linux 模拟建立连接
- 返回 backend_sess_id=2000

✅ **Scenario 2: HTTP GET**
- seL4 发送 HTTP GET 请求（396 字节）
- Linux 生成 HTTP 响应（118 字节）
- seL4 成功解析响应内容

### 6.2 性能指标

从日志观察：
- 目前使用的是轮询的方式，需要优化轮询频率
- **消息大小**: 最大 4088 字节（4096 - 8 字节头）

---

## 7. 故障排查

### 7.1 常见问题

**问题 1**: Linux 读不到 seL4 发送的数据

```bash
# 检查物理地址映射
[Linux] RX Queue: 0xDE000000 ← 必须与 seL4 TX Queue 物理地址一致
[seL4]  TX Queue: paddr=0xDE000000 ✓

# 检查队列初始化
[Linux] capacity=0 ← 表示 seL4 还未初始化
[seL4]  TX Queue AFTER init: capacity=256 ✓
```

**问题 2**: seL4 写入后 Linux 看到的是旧数据

```c
// 确认 Uncached 属性
[kernel] map_it_frame_cap: PA 0xde000000 -> DEVICE_nGnRnE ✓

// 确认内存屏障
hyperamp_cache_clean(...);  // Uncached 模式下等效于 DMB
```

**问题 3**: 自旋锁死锁

```c
// 检查 zone_id
seL4:  zone_id = 1 ✓
Linux: zone_id = 0 ✓

// 检查锁的释放
hyperamp_spinlock_unlock(&queue->queue_lock);  // 必须在所有路径上调用
```

---

## 8. 未来优化方向

1. **中断通知机制**：替代轮询，降低 CPU 占用
2. **零拷贝 API**：`hyperamp_queue_alloc_slot()` 直接在共享内存写入
3. **批量操作**：一次性处理多条消息
4. **流量控制**：实现背压机制防止队列溢出
5. **诊断工具**：队列状态监控、性能分析

---


## 附录 A: 数据结构定义

### HyperampShmQueue (4068 字节)

```c
typedef struct {
    uint8_t  map_mode1;              // Offset 0
    uint8_t  map_mode2;              // Offset 1
    uint16_t header;                 // Offset 2
    uint16_t tail;                   // Offset 4
    uint16_t capacity;               // Offset 6
    uint16_t block_size;             // Offset 8
    uint16_t _reserved;              // Offset 10
    uint64_t phy_addr;               // Offset 12
    uint64_t virt_addr1;             // Offset 20
    uint64_t virt_addr2;             // Offset 28
    HyperampMapTableEntry table1[125];  // Offset 36
    HyperampMapTableEntry table2[125];  // Offset 2036
    HyperampSpinlock queue_lock;     // Offset 4036
    uint32_t magic;                  // Offset 4052
    uint32_t version;                // Offset 4056
    uint32_t enqueue_count;          // Offset 4060
    uint32_t dequeue_count;          // Offset 4064
} __attribute__((packed)) HyperampShmQueue;
```

### HyperampMsgHeader (8 字节)

```c
typedef struct {
    uint8_t  version;           // Offset 0
    uint8_t  proxy_msg_type;    // Offset 1
    uint16_t frontend_sess_id;  // Offset 2
    uint16_t backend_sess_id;   // Offset 4
    uint16_t payload_len;       // Offset 6
} __attribute__((packed)) HyperampMsgHeader;
```

---

## 附录 B: 相关源代码

**Linux端：**
- `tools/include/shm/hyperamp_shm_queue.h`
- `tools/hyperamp_backend_proxy_sim.c`
- `tools/shm/hyperamp_linux_shm.c`
- `driver/hvisor.c`

**sel4端：**
- `kernel/src/arch/arm/64/kernel/vspace.c`
- `kernel/src/arch/arm/kernel/boot.c`
- `sel4test/projects/sel4test/apps/hyperamp-server/*`   

