# 快速集成参考

## 📦 文件清单

```
tools/
├── hyper_amp_client_mt.c      # 多线程客户端实现 (只读参考)
├── hyper_amp_service_mt.c     # 多线程服务端实现 (只读参考)
├── INTEGRATION_GUIDE.md       # 详细集成指南
└── QUICK_REFERENCE.md         # 本文件
```

## ⚡ 快速集成 (3步搞定)

### Step 1: 添加头文件到 hvisor.c

```c
// 在 hvisor.c 第39行附近添加:
#include "shm/threads.h"
```

### Step 2: 复制客户端代码

从 `hyper_amp_client_mt.c` 复制到 `hvisor.c` (在 `hyper_amp_client_test` 函数后):

```c
// 复制这3个部分:
// 1. ClientRequest 结构 (22-28行)
typedef struct ClientRequest {
    uint32_t request_id;
    uint32_t service_id;
    char* data_string;
    uint32_t size;
    struct Client* amp_client;
} ClientRequest;

// 2. handle_client_request 函数 (36-103行)
void handle_client_request(void* arg1, void* arg2) { ... }

// 3. hyper_amp_client_test_multithread 函数 (113-231行)
static int hyper_amp_client_test_multithread(int argc, char* argv[]) { ... }
```

### Step 3: 添加命令处理到 main()

```c
// 在 main() 函数中查找 hyper_amp_test,在其后添加:
else if(strcmp(argv[2], "hyper_amp_test_mt") == 0) {
    hyper_amp_client_test_multithread(argc - 3, &argv[3]);
}
```

## 🧪 测试命令

```bash
# 编译
cd /home/b/ft/hvisor-tool/tools
make clean && make

# 测试单线程(基准)
./hvisor shm hyper_amp_test shm_config.json "hello" 1

# 测试多线程(4线程,4请求)
./hvisor shm hyper_amp_test_mt shm_config.json "hello" 1 4

# 压力测试(8线程,100请求)
./hvisor shm hyper_amp_test_mt shm_config.json "test" 1 8 100
```

## 📋 服务端集成 (可选)

### 复制服务端代码

从 `hyper_amp_service_mt.c` 复制到 `hvisor.c` (在 `hyper_amp_service_test` 函数后):

```c
// 1. ServiceTask 结构 (26-31行)
typedef struct ServiceTask { ... } ServiceTask;

// 2. 服务函数 (如果不存在)
void hyperamp_encrypt_service(...) { ... }
void hyperamp_decrypt_service(...) { ... }

// 3. process_service_task 函数 (61-115行)
void process_service_task(void* arg1, void* arg2) { ... }

// 4. hyper_amp_service_test_multithread 函数 (126-316行)
static int hyper_amp_service_test_multithread(...) { ... }
```

### 添加命令处理

```c
else if(strcmp(argv[2], "hyper_amp_service_test_mt") == 0) {
    hyper_amp_service_test_multithread(argc - 3, &argv[3]);
}
```

### 测试服务端

```bash
# 终端1: 启动服务端(8线程)
sudo ./hvisor shm hyper_amp_service_test_mt shm_config.json 8

# 终端2: 客户端测试
./hvisor shm hyper_amp_test_mt shm_config.json "hello" 1 4
```

## 🎯 完成检查

- [ ] `#include "shm/threads.h"` 已添加
- [ ] 客户端代码已复制
- [ ] 命令处理已添加到 main()
- [ ] 编译成功 `make clean && make`
- [ ] 测试成功

## 💡 提示

1. **代码位置**: 所有要复制的代码都在对应的 `.c` 文件中,有清晰的行号标注
2. **完整注释**: 每个函数都有详细的中文注释
3. **独立文件**: 不会修改你的原始 hvisor.c,所有代码都在独立文件中
4. **详细指南**: 如需更多细节,查看 `INTEGRATION_GUIDE.md`

## 🆘 遇到问题?

1. **编译错误**: 检查 `#include "shm/threads.h"` 是否添加
2. **链接错误**: 检查 Makefile 是否包含 `shm/threads.c`
3. **运行错误**: 先测试单线程版本确认基础功能正常

---

**全部代码已准备好,直接复制粘贴即可!** 🚀
