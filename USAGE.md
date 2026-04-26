# HVisor 管理工具使用文档

## 1. 概述
`hvisor-tool` 是 HVisor 虚拟化系统的用户态管理套件，运行在 Root Linux (Zone 0) 中。它主要提供以下功能：
*   **分区管理**：启动、列出和关闭虚拟机分区 (Zones)。
*   **Virtio 设备仿真**：为 Non-Root 分区提供磁盘、网络、控制台和 GPU 模拟。
*   **HyperAMP 通信**：基于共享内存的高性能跨分区通信，支持多通道（CH0, CH1, CH2）。

---

## 2. 编译
执行以下命令进行编译（请确保已设置 `KDIR` 环境变量指向内核源码）：

```bash
make tools SHM=y ARCH=arm64
```
*注：`SHM=y` 是启用 HyperAMP 通信所必需的。*

---

## 3. 分区 (Zone) 管理

### 3.1 启动分区
```bash
./hvisor zone start <zone_config.json>
```
示例：`./hvisor zone start examples/qemu-aarch64/with_virtio_blk_console/zone1_linux.json`

### 3.2 列出分区
```bash
./hvisor zone list
```

### 3.3 关闭分区
```bash
./hvisor zone shutdown -id <zone_id>
```

---

## 4. Virtio 设备仿真
Virtio 守护进程必须在启动目标分区之前运行。

```bash
nohup ./hvisor virtio start <virtio_cfg.json> &
```

---

## 5. HyperAMP 通信工具 (重点)

### 5.1 hyperamp_linux 工具
`hyperamp_linux` 是用于与 seL4 等分区进行高速通信的专用工具，支持三通道切换。

#### 通用选项：
*   `-n <ID>`: 选择通道 (0, 1, 2)。自动对应物理基址。
*   `-c`: 初始化/创建队列（通常在首次启动时使用）。
*   `-w`: 发送后等待响应。
*   `-o <FILE>`: 将接收到的响应保存到文件。

#### 常用命令示例：

**1. 使用通道 0 发送回显 (Echo) 请求：**
```bash
./hyperamp_linux -n 0 -p "hello world" -w
```

**2. 使用通道 1 请求加密服务：**
```bash
./hyperamp_linux -n 1 -e "secret data" -w
```

**3. 使用通道 2 请求解密服务并将结果存入文件：**
```bash
./hyperamp_linux -n 2 -d @encrypted.bin -w -o decrypted.txt
```

**4. 接收所有通道消息：**
```bash
./hyperamp_linux -n 0 -r
./hyperamp_linux -n 1 -r
```

---

### 5.2 hvisor shm 接口
通过 `hvisor` 命令也可以进行 HyperAMP 通信，适用于需要 JSON 配置的场景。

**发送服务请求：**
```bash
./hvisor shm hyper_amp <shm_config.json> <data> <service_id> [channel_id]
```
*   `service_id`: 0 (Echo), 1 (Encrypt), 2 (Decrypt)。
*   `channel_id`: 可选，0-2。

示例（使用通道 2 发送）：
```bash
./hvisor shm hyper_amp shm_config.json "test message" 1 2
```

---

## 6. 常见问题
1. **总线错误 (Bus Error)**: 通常是因为映射的物理地址不正确或该区域未在设备树中预留。
2. **队列未就绪**: 请确保接收端分区（如 seL4）已启动并完成了共享内存的初始化。
3. **权限不足**: 访问 `/dev/hvisor` 或 `/dev/mem` 通常需要 root 权限。
