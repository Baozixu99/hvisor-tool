# HyperAMP 多线程支持 - 集成指南

## 📁 文件说明

本次实现包含以下文件:

1. **hyper_amp_client_mt.c** - 多线程客户端实现
2. **hyper_amp_service_mt.c** - 多线程服务端实现  
3. **INTEGRATION_GUIDE.md** - 本集成指南

## 🔧 Phase 1: 队列锁保护 (已完成)

已在以下文件中添加队列锁:

### 修改的文件

#### `tools/include/shm/msgqueue.h` (第36-48行)
```c
struct AmpMsgQueue
{
	volatile uint32_t working_mark;
	uint16_t buf_size;
	volatile uint16_t empty_h;
	volatile uint16_t wait_h;
	volatile uint16_t proc_ing_h;

	ByteFlag queue_lock;  /* ← 新增: 队列操作锁 */

	struct MsgEntry entries[0];
}__attribute__((aligned(MEMORY_ALIGN_SIZE)));
```

#### `tools/shm/msgqueue.c`

**1. 初始化锁** (在 `msg_queue_init` 函数中):
```c
// 在第119行附近添加
/* 初始化队列锁 (用于多线程安全) */
byte_flag_ops.init(&msg_queue->queue_lock);
```

**2. 保护 `msg_queue_pop`** (第132-155行):
```c
static uint16_t msg_queue_pop(struct AmpMsgQueue* msg_queue, uint16_t* head)
{
  uint16_t ret = msg_queue->buf_size;

  /* 加锁保护队列操作 */
  byte_flag_ops.lock(&msg_queue->queue_lock);

  if (*head >= msg_queue->buf_size) 
  {
    byte_flag_ops.unlock(&msg_queue->queue_lock);
    return ret;
  }
  
  /* 摘链 */
  ret = *head;
  *head = msg_queue->entries[*head].nxt_idx;
  msg_queue->entries[ret].nxt_idx = msg_queue->buf_size;

  byte_flag_ops.unlock(&msg_queue->queue_lock);
  return ret;
}
```

**3. 保护 `msg_queue_push`** (第157-180行):
```c
static int32_t msg_queue_push(struct AmpMsgQueue* msg_queue, uint16_t* head, uint16_t msg_index)
{
  /* 加锁保护队列操作 */
  byte_flag_ops.lock(&msg_queue->queue_lock);

  if (msg_index >= msg_queue->buf_size)
  {
      printf("msg_queue_push_error: msg index error = %u, check it\n",msg_index);
      byte_flag_ops.unlock(&msg_queue->queue_lock);
      while(1) {}
  }
  
  if (*head == msg_queue->buf_size)
  {
      *head = msg_index;
      msg_queue->entries[msg_index].nxt_idx = msg_queue->buf_size;
  }
  else 
  {
      msg_queue->entries[msg_index].nxt_idx = *head;
      *head = msg_index;
  }
  
  byte_flag_ops.unlock(&msg_queue->queue_lock);
  return 0;
}
```

**4. 保护 `msg_queue_transfer`** (第182-203行):
```c
static int32_t msg_queue_transfer(struct AmpMsgQueue* msg_queue, 
  uint16_t* from_head, uint16_t* to_head)
{
  /* 加锁保护队列操作 */
  byte_flag_ops.lock(&msg_queue->queue_lock);
  
  if (*from_head >= msg_queue->buf_size)
  {
      printf("msg_queue_transfer_error: from_head error = %u, check it\n", 
        *from_head);
      byte_flag_ops.unlock(&msg_queue->queue_lock);
      while(1) {}
  }
  
  *to_head = *from_head;
  *from_head = msg_queue->buf_size;

  byte_flag_ops.unlock(&msg_queue->queue_lock);
  return 0;
}
```

**验证**: 重新编译后,队列操作已是线程安全的。

---

## 🔧 Phase 2: 集成多线程客户端

### 步骤 1: 添加头文件

在 `tools/hvisor.c` 的头部添加 (第39行附近):
```c
#include "shm/threads.h"    // 线程池支持
```

### 步骤 2: 复制客户端代码

从 `hyper_amp_client_mt.c` 复制以下内容到 `tools/hvisor.c`:

1. **ClientRequest 结构** (第22-28行)
2. **handle_client_request 函数** (第36-103行)  
3. **hyper_amp_client_test_multithread 函数** (第113-231行)

**插入位置**: 在 `hyper_amp_client_test` 函数之后

### 步骤 3: 添加命令处理

在 `main()` 函数的命令处理部分添加 (查找 `hyper_amp_test` 所在位置):
```c
else if(strcmp(argv[2], "hyper_amp_test") == 0) {
    hyper_amp_client_test(argc - 3, &argv[3]);
}
else if(strcmp(argv[2], "hyper_amp_test_mt") == 0) {  // ← 新增
    hyper_amp_client_test_multithread(argc - 3, &argv[3]);
}
```

### 步骤 4: 编译测试

```bash
cd /home/b/ft/hvisor-tool/tools
make clean
make

# 如果编译成功,测试单线程(基准)
./hvisor shm hyper_amp_test shm_config.json "hello" 1

# 测试多线程(4线程)
./hvisor shm hyper_amp_test_mt shm_config.json "hello" 1 4
```

---

## 🔧 Phase 3: 集成多线程服务端

### 步骤 1: 复制服务端代码

从 `hyper_amp_service_mt.c` 复制以下内容到 `tools/hvisor.c`:

1. **ServiceTask 结构** (第26-31行)
2. **hyperamp_encrypt_service 函数** (第38-44行)
3. **hyperamp_decrypt_service 函数** (第49-52行)
4. **process_service_task 函数** (第61-115行)
5. **hyper_amp_service_test_multithread 函数** (第126-316行)

**插入位置**: 在 `hyper_amp_service_test` 函数之后

**注意**: 如果 `hyperamp_encrypt_service` 和 `hyperamp_decrypt_service` 已存在于 `hvisor.c` 中,则跳过步骤1中的函数2和3。

### 步骤 2: 添加命令处理

在 `main()` 函数的命令处理部分添加:
```c
else if(strcmp(argv[2], "hyper_amp_service_test") == 0) {
    hyper_amp_service_test(argv[3]);
}
else if(strcmp(argv[2], "hyper_amp_service_test_mt") == 0) {  // ← 新增
    hyper_amp_service_test_multithread(argc - 3, &argv[3]);
}
```

### 步骤 3: 编译测试

```bash
cd /home/b/ft/hvisor-tool/tools
make clean
make

# 测试多线程服务端(需要sudo)
sudo ./hvisor shm hyper_amp_service_test_mt shm_config.json 8
```

---

## 🧪 完整测试流程

### 测试 1: 基础功能验证

```bash
# 终端1: 启动多线程服务端(8个工作线程)
cd /home/b/ft/hvisor-tool/tools
sudo ./hvisor shm hyper_amp_service_test_mt shm_config.json 8

# 终端2: 发送单个测试请求
cd /home/b/ft/hvisor-tool/tools
./hvisor shm hyper_amp_test shm_config.json "hello world" 1

# 预期: 服务端收到请求,加密处理,返回结果
```

### 测试 2: 多线程客户端

```bash
# 终端1: 服务端继续运行

# 终端2: 多线程客户端测试(4线程,4个请求)
./hvisor shm hyper_amp_test_mt shm_config.json "test message" 1 4

# 预期: 4个请求并发处理,显示每个请求的延迟
```

### 测试 3: 压力测试

```bash
# 终端1: 服务端继续运行

# 终端2: 压力测试(16线程,1000个请求)
./hvisor shm hyper_amp_test_mt shm_config.json "stress test" 66 16 1000

# 预期: 
# - 1000个请求全部成功
# - 显示吞吐量和平均延迟
# - 无崩溃或数据损坏
```

### 测试 4: 性能对比

```bash
# 单线程基准测试
time for i in {1..100}; do
    ./hvisor shm hyper_amp_test shm_config.json "test" 1
done

# 多线程性能测试
time ./hvisor shm hyper_amp_test_mt shm_config.json "test" 1 8 100

# 对比: 多线程版本应该快得多
```

---

## 📊 预期性能提升

基于现有的 62ms 端到端延迟:

| 场景 | 单线程 | 多线程(4线程) | 多线程(8线程) | 提升倍数 |
|------|--------|---------------|---------------|----------|
| 100个请求 | ~6200ms | ~1600ms | ~800ms | 7.8x |
| 吞吐量 | ~16 req/s | ~62 req/s | ~125 req/s | 7.8x |

---

## ⚠️ 注意事项

### 1. 编译要求
- 确保链接了 pthread: `-lpthread`
- 确保包含了线程池模块: `tools/shm/threads.c`

### 2. 运行权限
- 服务端需要 sudo 权限访问 `/dev/mem`
- 客户端无需特殊权限

### 3. 队列大小配置
- 建议队列大小 ≥ 线程数 * 2
- 在 `shm_config.json` 中配置:
```json
{
  "msg_queue_mem_size": 8192  // 根据线程数调整
}
```

### 4. 线程数选择
- **客户端**: 根据请求数量,建议 4-16 线程
- **服务端**: 根据 CPU 核心数,建议 核心数 * 2

### 5. 调试建议
如果遇到问题:
1. 先测试单线程版本确认基础功能正常
2. 从2个线程开始测试,逐步增加
3. 检查日志中的 `[Thread XXX]` 输出确认并发执行
4. 使用 `top -H` 查看线程运行情况

---

## 🔍 验证检查清单

- [ ] Phase 1: 队列锁已添加,编译无错误
- [ ] Phase 2: 多线程客户端编译成功
- [ ] Phase 3: 多线程服务端编译成功
- [ ] 测试1: 单个请求正常处理
- [ ] 测试2: 多线程客户端并发成功
- [ ] 测试3: 压力测试无崩溃
- [ ] 测试4: 性能提升符合预期

---

## 📝 代码位置索引

### hyper_amp_client_mt.c
- **行22-28**: ClientRequest 结构定义
- **行36-103**: handle_client_request 函数
- **行113-231**: hyper_amp_client_test_multithread 主函数

### hyper_amp_service_mt.c
- **行26-31**: ServiceTask 结构定义
- **行38-44**: hyperamp_encrypt_service 函数
- **行49-52**: hyperamp_decrypt_service 函数
- **行61-115**: process_service_task 函数
- **行126-316**: hyper_amp_service_test_multithread 主函数

### 需要修改的 hvisor.c 位置
- **头部**: 添加 `#include "shm/threads.h"`
- **hyper_amp_client_test 之后**: 插入客户端多线程代码
- **hyper_amp_service_test 之后**: 插入服务端多线程代码
- **main() 命令处理**: 添加 `hyper_amp_test_mt` 和 `hyper_amp_service_test_mt` 分支

---

## 🎯 完成!

按照以上步骤完成集成后,您将拥有:
- ✅ 线程安全的消息队列
- ✅ 高性能多线程客户端
- ✅ 可扩展的多线程服务端
- ✅ 完整的性能测试工具

有任何问题请参考源文件中的详细注释!
