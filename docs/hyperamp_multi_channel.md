# HyperAMP 紧凑型多通道架构（CH0/CH1/CH2）总结与使用指南

## 1. 架构演进与设计目标

随着业务系统复杂度的提升，HyperAMP 跨环境（Linux <-> seL4）通信由单通道演进为独立隔离的多通道（CH0、CH1、CH2）。这种设计防止了高优通信（如设备控制）和大数据传输相互抢占锁或是产生数据拥塞。

核心挑战：早期使用 6MB 步长（stride）拉开通道会导致 seL4 测的虚拟地址空间以及页表槽位极速耗尽甚至导致 Crash。当前方案我们重构为 **紧凑型通道布局 (Compact Layout)**，在总计 **4MB** 的空间内塞入 3 个功能完整的隔离通道。

## 2. 物理内存布局与通道规划 (Total: 4MB)

硬件层的核心约束：ARM 架构下，原子级 Spinlock（`LDXR/STXR`）**必须要求所在内存是 Cacheable (NORMAL WB)**，而我们双系统（VM）跨界的数据通信区 **必须要求 Uncached (DEVICE_nGnRnE)** 来保持硬件一致性。这意味着同一个通道内，控制区和数据区的属性必须切割！

总体映射如下（基址：`0x7E000000`）：

| 通道 | 容量分配 | 起始物理地址 | 内存构成与硬件属性 (Queue / Data) |
| --- | --- | --- | --- |
| **CH0** | **2MB** | `0x7E000000` | RX(4KB) + TX(4KB) [`NORMAL WB`]<br>Data (~1.99MB) [`DEVICE`] (起始 `0x7E002000`) |
| **CH1** | **1MB** | `0x7E200000` | RX(4KB) + TX(4KB) [`NORMAL WB`]<br>Data (~0.99MB) [`DEVICE`] (起始 `0x7E202000`) |
| **CH2** | **1MB** | `0x7E300000` | RX(4KB) + TX(4KB) [`NORMAL WB`]<br>Data (~0.99MB) [`DEVICE`] (起始 `0x7E302000`) |

*注：通道内的 Queue 顺序依次为 RX Queue 和 TX Queue。*

---

## 3. seL4 端改造盘点与排雷总结

我们在实现这套多通道方案时，深入了内核底层进行了一系列复杂改造。如果你未来还要加通道，请务必关注以下内核修改点：

### 3.1 seL4 Capabilities (能力槽) 扩展
在 `libsel4/include/sel4/bootinfo_types.h` 中，为了容纳 3 个通道（共 6 个 Queue 映射），我们将初始 Root 能力槽的数量 `seL4_NumInitialCaps` 从 18 扩展到了 24。
- CH0 占用：`seL4_CapInitThreadShm_root_q` 和 `+1`
- CH1 占用：`seL4_CapInitThreadShm_root_q + 2` 和 `+3`
- CH2 占用：`seL4_CapInitThreadShm_root_q + 4` 和 `+5`

### 3.2 `boot.c` 的紧凑型循环映射修复
在内核级启动（`sel4test/kernel/src/arch/arm/kernel/boot.c`）为 3 个通道挂载虚拟内存时，最容易出现由于变量累加导致的 “虚拟地址重叠或漂移” 故障。
- **循环数组映射**：我们定义了 `uint64_t channel_offsets[3] = {0, 0x200000UL, 0x300000UL}`，通过对该数组的轮询，精准生成三个 PA（物理地址）映射到 Capability 槽中。
- **IPC Buffer 同步修正**：`msg[2]、msg[3]、msg[4]` 传递给通过 seL4 应用程序的必须是**通道对齐**的地址。此前就是因为循环累积副作用导致 IPC 传错了地址，我们在 `boot.c` 中强制将 IPC 传参改为 `shm_data_vaddr + 0x[Offset]` 的明确值，避开了错误。

### 3.3 `vspace.c` 的页表级精准属性切分
seL4 这边的所有物理地址块最终通过 `map_4MB_phys_to_vaddr` 进行映射。由于现在的 4MB 空间是被碎片化使用的（一段 Cached，一段 Uncached），我们在写入 AArch64 `PUD / PT` 页面属性时，强行加上了硬编码的物理地址打磨：
- 当识别为 `0x7E000000...0x7E002000` (或 2M/3M 偏移处) 时：赋给页表 `NORMAL`。
- 其他数据区统统赋为 `DEVICE_nGnRnE`。

---

## 4. Linux 端 (驱动及应用库) 适配情况

### 4.1 Hvisor 驱动 (`hvisor.c`) 的 `mmap` 控制
驱动的逻辑与 `vspace.c` 同样苛刻。当用户态调用一次跨跃整个通道的 `mmap` 时，`pgprot_noncached` 无法区分不同的物理块。我们在 `hvisor_map` 拦截了物理地址，基于分块逻辑只对 Data 区生效 Noncached 属性。

### 4.2 用户态库 (`hyperamp_linux_shm.c`) 的动态映射重构
旧版库中一直写死了宏 `SHM_DATA_SIZE (4MB)`。在新版中，我们已经在库入口 `hyperamp_linux_init` 中做了动态拆分：
- 根据你传入的物理基指（`phys_addr`），不仅会寻找对口的 `data_size`，还会用**恰当的尺寸去 `mmap`**。这就彻底杜绝了通道 1 的初始化动作去越界吞噬破坏通道 2 的页面和数据。

---

## 5. 快速上手：如何在新应用中调用对应通道？

当你想基于 HyperAMP 开发一个新的应用侧功能（或者替换现有的 proxy 模拟代码），你只需要调用初始化接口并传入你要的物理地址。

### 提供两个便利的宏：
```c
// 建议在你的包含头文件中引用
#define SHM_CH0_PADDR  0x7E000000UL  // 通道 0 (2MB) 
#define SHM_CH1_PADDR  0x7E200000UL  // 通道 1 (1MB)
#define SHM_CH2_PADDR  0x7E300000UL  // 通道 2 (1MB)
```

### 【示例】在 Linux 端基于通道 2 开发应用

```c
#include <stdio.h>
#include "hyperamp_linux_shm.h"

int main() {
    // 1. 初始化，指定使用 CH2。第二个参数 is_creator = 0 表示充当连接者（通常 seL4 为创建者）
    if (hyperamp_linux_init(SHM_CH2_PADDR, 0) != HYPERAMP_OK) {
        printf("HyperAMP CH2 启动失败！\n");
        return 1;
    }
    
    printf("HyperAMP CH2 已经就绪！\n");

    // 2. 发送消息
    uint8_t payload[] = "Hello from Linux over CH2!";
    hyperamp_linux_send(0x01, 1000, 2000, payload, sizeof(payload));

    // 3. 阻塞接收消息
    HyperampMsgHeader hdr;
    uint8_t recv_buf[4096];
    uint16_t actual_len;
    
    if (hyperamp_linux_recv(&hdr, recv_buf, sizeof(recv_buf), &actual_len) == HYPERAMP_OK) {
        printf("CH2 收到了来自 seL4 的回应！长度：%d\n", actual_len);
    }

    // 4. 退出清理
    hyperamp_linux_cleanup();
    return 0;
}
```

### 【扩展】如何在 seL4 端选择对应通道（修改点）

seL4 端的测试程序（如 `apps/hyperamp-server/src/main.c`）并没有直接的 `hyperamp_linux_init` 函数，它们是通过 `boot.c` 塞在 `IPC Buffer` 里的消息唤醒的。

如果你要测试 CH1 或 CH2，就必须像刚才我们做的那样，去修改 **内核 `boot.c` 末尾** 的 `ipcBuf` 写入偏移量：
```c
在 boot.c 替换为CH1 的偏移量
ipcBuf[3] = shm_data_vaddr + 0x200000UL;  // msg[2] (CH1 TX)
ipcBuf[4] = shm_data_vaddr + 0x201000UL;  // msg[3] (CH1 RX)
ipcBuf[5] = shm_data_vaddr + 0x202000UL;  // msg[4] (CH1 Data)
// 在 boot.c 替换为CH2 的偏移量
ipcBuf[3] = shm_data_vaddr + 0x300000UL;  // CH2 的 TX 寻址
ipcBuf[4] = shm_data_vaddr + 0x301000UL;  // CH2 的 RX 寻址
ipcBuf[5] = shm_data_vaddr + 0x302000UL;  // CH2 的 数据区寻址
```
然后重新使用 `ninja` 编译 seL4 镜像，让它把属于 CH2 的通道虚拟指针指引交给应用。
