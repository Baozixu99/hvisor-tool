/**
 * @file hyperamp_linux_shm.c
 * @brief HyperAMP Linux 端共享内存队列实现 - 兼容 HighSpeedCProxy
 * 
 * 本文件提供 Linux 端的共享内存队列操作实现，包括：
 * - 物理内存映射 (/dev/mem)
 * - 双向队列操作 (Linux ↔ seL4)
 * - 与 seL4 微内核的消息通信
 */

#define _POSIX_C_SOURCE 199309L
#define _DEFAULT_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <time.h>
#include <errno.h>

#include "shm/hyperamp_shm_queue.h"

/* ==================== 配置定义 ==================== */

/* 共享内存物理地址 - 新版 HyperAMP 布局 (双向通信) */
#define SHM_TX_QUEUE_PADDR          0xDE000000UL  // Linux → seL4 队列
#define SHM_RX_QUEUE_PADDR          0xDE001000UL  // seL4 → Linux 队列 (偏移 4KB)
#define SHM_DATA_PADDR              0xDE002000UL  // 共享数据区 (偏移 8KB)

#define SHM_QUEUE_SIZE              (4 * 1024)    // 4KB 队列控制区 (实际 ~4068 bytes)
#define SHM_DATA_SIZE               (4 * 1024 * 1024)  // 4MB 数据区

#define SHM_TOTAL_SIZE              (SHM_QUEUE_SIZE * 2 + SHM_DATA_SIZE)  // 总计约 4.01MB

/* 队列配置 */
#define DEFAULT_QUEUE_CAPACITY      256
#define DEFAULT_BLOCK_SIZE          4096  // 与 HighSpeedCProxy 的 HSNET_MEM_BLOCK_SIZE 一致

/* Zone ID */
#define ZONE_ID_LINUX               0
#define ZONE_ID_SEL4                1

/* ==================== 全局状态 ==================== */

typedef struct {
    int fd_mem;                              // /dev/mem 文件描述符
    volatile void *shm_base;                 // 共享内存映射基址
    size_t shm_size;                         // 映射大小
    uint64_t phys_addr;                      // 物理地址
    
    volatile HyperampShmQueue *tx_queue;     // Linux → seL4 发送队列
    volatile HyperampShmQueue *rx_queue;     // seL4 → Linux 接收队列
    volatile void *data_region;              // 数据区基址
    
    int initialized;                         // 初始化标志
    
    // 统计信息
    uint32_t tx_count;
    uint32_t rx_count;
    uint32_t tx_errors;
    uint32_t rx_errors;
} HyperampLinuxContext;

static HyperampLinuxContext g_ctx = {0};

/* ==================== 私有函数 ==================== */

/**
 * @brief 映射物理内存到用户空间
 */
static int map_physical_memory(uint64_t phys_addr, size_t size)
{
    // 打开 /dev/mem
    g_ctx.fd_mem = open("/dev/mem", O_RDWR | O_SYNC);
    if (g_ctx.fd_mem < 0) {
        perror("[HyperAMP] Failed to open /dev/mem");
        return HYPERAMP_ERROR;
    }
    
    // 计算页对齐
    size_t page_size = sysconf(_SC_PAGESIZE);
    off_t page_offset = phys_addr & (page_size - 1);
    off_t page_aligned_addr = phys_addr & ~(page_size - 1);
    size_t map_size = size + page_offset;
    
    // 映射内存 (使用 MAP_SHARED，不缓存)
    void *mapped = mmap(NULL, map_size, 
                        PROT_READ | PROT_WRITE, 
                        MAP_SHARED, 
                        g_ctx.fd_mem, 
                        page_aligned_addr);
    
    if (mapped == MAP_FAILED) {
        perror("[HyperAMP] mmap failed");
        close(g_ctx.fd_mem);
        g_ctx.fd_mem = -1;
        return HYPERAMP_ERROR;
    }
    
    g_ctx.shm_base = (volatile void *)((char *)mapped + page_offset);
    g_ctx.shm_size = size;
    g_ctx.phys_addr = phys_addr;
    
    printf("[HyperAMP] Physical memory mapped:\n");
    printf("[HyperAMP]   Physical addr: 0x%lx\n", phys_addr);
    printf("[HyperAMP]   Virtual addr:  %p\n", g_ctx.shm_base);
    printf("[HyperAMP]   Size:          %zu bytes\n", size);
    
    return HYPERAMP_OK;
}

/**
 * @brief 取消内存映射
 */
static void unmap_physical_memory(void)
{
    if (g_ctx.shm_base) {
        size_t page_size = sysconf(_SC_PAGESIZE);
        off_t page_offset = g_ctx.phys_addr & (page_size - 1);
        void *map_base = (void *)((char *)g_ctx.shm_base - page_offset);
        size_t map_size = g_ctx.shm_size + page_offset;
        
        munmap(map_base, map_size);
        g_ctx.shm_base = NULL;
    }
    
    if (g_ctx.fd_mem >= 0) {
        close(g_ctx.fd_mem);
        g_ctx.fd_mem = -1;
    }
}

/* ==================== 公共 API ==================== */

/**
 * @brief 初始化 HyperAMP Linux 客户端
 * @param phys_addr 共享内存物理地址 (0 使用默认值)
 * @param is_creator 是否为队列创建者 (1=创建并初始化队列, 0=连接已存在的队列)
 * @return HYPERAMP_OK 成功, HYPERAMP_ERROR 失败
 */
int hyperamp_linux_init(uint64_t phys_addr, int is_creator)
{
    if (g_ctx.initialized) {
        printf("[HyperAMP] Already initialized\n");
        return HYPERAMP_OK;
    }
    
    // 使用默认地址 (TX Queue 起始地址)
    if (phys_addr == 0) {
        phys_addr = SHM_TX_QUEUE_PADDR;
    }
    
    printf("[HyperAMP] ========================================\n");
    printf("[HyperAMP] Initializing HyperAMP Linux Client\n");
    printf("[HyperAMP] ========================================\n");
    printf("[HyperAMP] Mode: %s\n", is_creator ? "CREATOR" : "CONNECTOR");
    printf("[HyperAMP] Physical address: 0x%lx\n", phys_addr);
    
    // 映射物理内存 (从 TX Queue 开始,映射整个区域)
    if (map_physical_memory(phys_addr, SHM_TOTAL_SIZE) != HYPERAMP_OK) {
        return HYPERAMP_ERROR;
    }
    
    // 新版 HyperAMP 内存布局 (管理信息在共享内存头部):
    // 0xde000000: TX Queue (64KB) - Linux 写, seL4 读
    // 0xde010000: RX Queue (64KB) - seL4 写, Linux 读
    // 0xde020000: Data Region (4MB) - 共享数据
    g_ctx.tx_queue = (volatile HyperampShmQueue *)g_ctx.shm_base;
    g_ctx.rx_queue = (volatile HyperampShmQueue *)((char *)g_ctx.shm_base + SHM_QUEUE_SIZE);
    g_ctx.data_region = (volatile void *)((char *)g_ctx.shm_base + 2 * SHM_QUEUE_SIZE);
    
    printf("[HyperAMP] Memory layout:\n");
    printf("[HyperAMP]   TX Queue:    %p (phys: 0x%lx)\n", 
           g_ctx.tx_queue, phys_addr);
    printf("[HyperAMP]   RX Queue:    %p (phys: 0x%lx)\n", 
           g_ctx.rx_queue, phys_addr + SHM_QUEUE_SIZE);
    printf("[HyperAMP]   Data Region: %p (phys: 0x%lx, size: %d bytes)\n", 
           g_ctx.data_region, phys_addr + 2 * SHM_QUEUE_SIZE, SHM_DATA_SIZE);
    
    // 初始化队列配置
    HyperampQueueConfig tx_config = {
        .map_mode = HYPERAMP_MAP_MODE_CONTIGUOUS_BOTH,
        .capacity = DEFAULT_QUEUE_CAPACITY,
        .block_size = DEFAULT_BLOCK_SIZE,
        .phy_addr = phys_addr,  // TX Queue 起始地址
        .virt_addr = (uint64_t)g_ctx.tx_queue,
    };
    
    HyperampQueueConfig rx_config = {
        .map_mode = HYPERAMP_MAP_MODE_CONTIGUOUS_BOTH,
        .capacity = DEFAULT_QUEUE_CAPACITY,
        .block_size = DEFAULT_BLOCK_SIZE,
        .phy_addr = phys_addr + SHM_QUEUE_SIZE,  // RX Queue 地址
        .virt_addr = (uint64_t)g_ctx.rx_queue,
    };
    
    // Linux 端是 TX 队列的创建者，seL4 端是 RX 队列的创建者
    // 但为了简化，这里让 Linux 端初始化两个队列
    if (is_creator) {
        printf("[HyperAMP] Initializing TX queue......\n");
        if (hyperamp_queue_init(g_ctx.tx_queue, &tx_config, 1) != HYPERAMP_OK) {
            printf("[HyperAMP] Failed to init TX queue\n");
            unmap_physical_memory();
            return HYPERAMP_ERROR;
        }
        
        printf("[HyperAMP] Initializing RX queue...\n");
        if (hyperamp_queue_init(g_ctx.rx_queue, &rx_config, 1) != HYPERAMP_OK) {
            printf("[HyperAMP] Failed to init RX queue\n");
            unmap_physical_memory();
            return HYPERAMP_ERROR;
        }
        
        // 清空数据区
        printf("[HyperAMP] Clearing data region...\n");
        hyperamp_safe_memset(g_ctx.data_region, 0, SHM_DATA_SIZE);
    } else {
        // 等待队列被初始化 (检查 capacity 字段而不是 magic,因为 magic 超出 4KB 边界)
        printf("[HyperAMP] Connecting to existing queues (no wait mode)...\n");
        
        /* 重要：清理 CPU 数据缓存，确保读取到 seL4 写入的最新数据 */
        __asm__ volatile("dc civac, %0" : : "r"(g_ctx.tx_queue) : "memory");
        __asm__ volatile("dc civac, %0" : : "r"(g_ctx.rx_queue) : "memory");
        __asm__ volatile("dsb sy" ::: "memory");
        __asm__ volatile("isb" ::: "memory");
        
        // 打印原始数据以调试
        printf("[HyperAMP] DEBUG: Raw TX Queue bytes (first 32):\n[HyperAMP]   ");
        volatile uint8_t *tx_bytes = (volatile uint8_t *)g_ctx.tx_queue;
        for (int i = 0; i < 32; i++) {
            printf("%02x ", tx_bytes[i]);
            if ((i + 1) % 16 == 0 && i < 31) printf("\n[HyperAMP]   ");
        }
        printf("\n");
        
        printf("[HyperAMP] DEBUG: Raw RX Queue bytes (first 32):\n[HyperAMP]   ");
        volatile uint8_t *rx_bytes = (volatile uint8_t *)g_ctx.rx_queue;
        for (int i = 0; i < 32; i++) {
            printf("%02x ", rx_bytes[i]);
            if ((i + 1) % 16 == 0 && i < 31) printf("\n[HyperAMP]   ");
        }
        printf("\n");
        
        // 直接读取 capacity 字段
        uint16_t tx_cap = hyperamp_safe_read_u16(g_ctx.tx_queue, offsetof(HyperampShmQueue, capacity));
        uint16_t rx_cap = hyperamp_safe_read_u16(g_ctx.rx_queue, offsetof(HyperampShmQueue, capacity));
        
        printf("[HyperAMP] TX capacity=%u (expected 256), RX capacity=%u (expected 256)\n", tx_cap, rx_cap);
        
        if (tx_cap == 0 && rx_cap == 0) {
            printf("[HyperAMP] WARNING: Both queues appear uninitialized!\n");
            printf("[HyperAMP] This means seL4's writes are NOT visible to Linux.\n");
            printf("[HyperAMP] Check Hvisor shared memory configuration!\n");
            unmap_physical_memory();
            return HYPERAMP_ERROR;
        }
        
        printf("[HyperAMP] Found at least one initialized queue, proceeding...\n");
    }
    
    g_ctx.initialized = 1;
    g_ctx.tx_count = 0;
    g_ctx.rx_count = 0;
    g_ctx.tx_errors = 0;
    g_ctx.rx_errors = 0;
    
    printf("[HyperAMP] Initialization complete!\n");
    
    // 安全读取队列信息（逐字节）
    uint32_t tx_magic = hyperamp_safe_read_u32(g_ctx.tx_queue, offsetof(HyperampShmQueue, magic));
    uint16_t tx_capacity = hyperamp_safe_read_u16(g_ctx.tx_queue, offsetof(HyperampShmQueue, capacity));
    uint16_t tx_block_size = hyperamp_safe_read_u16(g_ctx.tx_queue, offsetof(HyperampShmQueue, block_size));
    
    uint32_t rx_magic = hyperamp_safe_read_u32(g_ctx.rx_queue, offsetof(HyperampShmQueue, magic));
    uint16_t rx_capacity = hyperamp_safe_read_u16(g_ctx.rx_queue, offsetof(HyperampShmQueue, capacity));
    uint16_t rx_block_size = hyperamp_safe_read_u16(g_ctx.rx_queue, offsetof(HyperampShmQueue, block_size));
    
    printf("[HyperAMP] TX Queue: magic=0x%08x, capacity=%u, block_size=%u\n",
           tx_magic, tx_capacity, tx_block_size);
    printf("[HyperAMP] RX Queue: magic=0x%08x, capacity=%u, block_size=%u\n",
           rx_magic, rx_capacity, rx_block_size);
    printf("[HyperAMP] ========================================\n");
    
    return HYPERAMP_OK;
}

/**
 * @brief 关闭 HyperAMP Linux 客户端
 */
void hyperamp_linux_cleanup(void)
{
    if (!g_ctx.initialized) return;
    
    printf("[HyperAMP] Cleaning up...\n");
    printf("[HyperAMP] Statistics:\n");
    printf("[HyperAMP]   TX: %u sent, %u errors\n", g_ctx.tx_count, g_ctx.tx_errors);
    printf("[HyperAMP]   RX: %u received, %u errors\n", g_ctx.rx_count, g_ctx.rx_errors);
    
    unmap_physical_memory();
    
    g_ctx.initialized = 0;
    g_ctx.tx_queue = NULL;
    g_ctx.rx_queue = NULL;
    g_ctx.data_region = NULL;
    
    printf("[HyperAMP] Cleanup complete\n");
}

/**
 * @brief 发送消息到 seL4 (Linux -> seL4)
 * @param msg_type 消息类型 (HyperampMsgType)
 * @param frontend_sess_id 前端会话ID
 * @param backend_sess_id 后端会话ID
 * @param payload 载荷数据
 * @param payload_len 载荷长度
 * @return HYPERAMP_OK 成功, HYPERAMP_ERROR 失败
 */
int hyperamp_linux_send(uint8_t msg_type, 
                        uint16_t frontend_sess_id,
                        uint16_t backend_sess_id,
                        const void *payload, 
                        uint16_t payload_len)
{
    if (!g_ctx.initialized) {
        printf("[HyperAMP] Not initialized\n");
        return HYPERAMP_ERROR;
    }
    
    if (payload_len > HYPERAMP_MSG_MAX_SIZE) {
        printf("[HyperAMP] Payload too large: %u > %u\n", payload_len, HYPERAMP_MSG_MAX_SIZE);
        return HYPERAMP_ERROR;
    }
    
    // 准备消息缓冲区
    uint8_t msg_buf[HYPERAMP_MSG_HDR_PLUS_MAX_SIZE];
    HyperampMsgHeader *hdr = (HyperampMsgHeader *)msg_buf;
    
    hdr->version = 1;
    hdr->proxy_msg_type = msg_type;
    hdr->frontend_sess_id = frontend_sess_id;
    hdr->backend_sess_id = backend_sess_id;
    hdr->payload_len = payload_len;
    
    // 复制载荷
    if (payload && payload_len > 0) {
        memcpy(msg_buf + sizeof(HyperampMsgHeader), payload, payload_len);
    }
    
    size_t total_len = sizeof(HyperampMsgHeader) + payload_len;
    
    // 入队
    // 注意：数据区基址要跳过队列控制块
    volatile void *tx_data_base = (volatile void *)((char *)g_ctx.tx_queue + sizeof(HyperampShmQueue));
    
    int ret = hyperamp_queue_enqueue(g_ctx.tx_queue, ZONE_ID_LINUX, 
                                      msg_buf, total_len, tx_data_base);
    
    if (ret == HYPERAMP_OK) {
        g_ctx.tx_count++;
        printf("[HyperAMP] TX: type=%u, sess=%u/%u, len=%u (total: %u)\n",
               msg_type, frontend_sess_id, backend_sess_id, payload_len, g_ctx.tx_count);
    } else {
        g_ctx.tx_errors++;
        printf("[HyperAMP] TX failed: queue full? (errors: %u)\n", g_ctx.tx_errors);
    }
    
    return ret;
}

/**
 * @brief 从 seL4 接收消息 (seL4 -> Linux)
 * @param hdr 输出：消息头
 * @param payload 输出：载荷缓冲区
 * @param max_payload_len 载荷缓冲区最大长度
 * @param actual_payload_len 输出：实际载荷长度
 * @return HYPERAMP_OK 成功, HYPERAMP_ERROR 失败（无消息或错误）
 */
int hyperamp_linux_recv(HyperampMsgHeader *hdr,
                        void *payload,
                        uint16_t max_payload_len,
                        uint16_t *actual_payload_len)
{
    if (!g_ctx.initialized || !hdr) {
        return HYPERAMP_ERROR;
    }
    
    uint8_t msg_buf[HYPERAMP_MSG_HDR_PLUS_MAX_SIZE];
    size_t actual_len = 0;
    
    volatile void *rx_data_base = (volatile void *)((char *)g_ctx.rx_queue + sizeof(HyperampShmQueue));
    
    int ret = hyperamp_queue_dequeue(g_ctx.rx_queue, ZONE_ID_LINUX,
                                      msg_buf, sizeof(msg_buf), &actual_len, rx_data_base);
    
    if (ret != HYPERAMP_OK) {
        return HYPERAMP_ERROR;  // 队列空
    }
    
    if (actual_len < sizeof(HyperampMsgHeader)) {
        g_ctx.rx_errors++;
        printf("[HyperAMP] RX: invalid message (too short: %zu)\n", actual_len);
        return HYPERAMP_ERROR;
    }
    
    // 解析消息头
    memcpy(hdr, msg_buf, sizeof(HyperampMsgHeader));
    
    // 复制载荷
    uint16_t payload_len = hdr->payload_len;
    if (payload_len > max_payload_len) {
        payload_len = max_payload_len;
    }
    
    if (payload && payload_len > 0) {
        memcpy(payload, msg_buf + sizeof(HyperampMsgHeader), payload_len);
    }
    
    if (actual_payload_len) {
        *actual_payload_len = payload_len;
    }
    
    g_ctx.rx_count++;
    printf("[HyperAMP] RX: type=%u, sess=%u/%u, len=%u (total: %u)\n",
           hdr->proxy_msg_type, hdr->frontend_sess_id, hdr->backend_sess_id,
           hdr->payload_len, g_ctx.rx_count);
    
    return HYPERAMP_OK;
}

/**
 * @brief 发送原始数据消息 (便捷函数)
 */
int hyperamp_linux_send_data(uint16_t session_id, const void *data, uint16_t data_len)
{
    return hyperamp_linux_send(HYPERAMP_MSG_TYPE_DATA, session_id, 0, data, data_len);
}

/**
 * @brief 检查是否有待接收的消息
 */
int hyperamp_linux_has_message(void)
{
    if (!g_ctx.initialized) return 0;
    
    // 安全读取 header 和 tail
    uint16_t header = hyperamp_safe_read_u16(g_ctx.rx_queue, offsetof(HyperampShmQueue, header));
    uint16_t tail = hyperamp_safe_read_u16(g_ctx.rx_queue, offsetof(HyperampShmQueue, tail));
    
    return (tail != header);
}

/**
 * @brief 获取队列状态
 */
void hyperamp_linux_get_status(void)
{
    if (!g_ctx.initialized) {
        printf("[HyperAMP] Not initialized\n");
        return;
    }
    
    printf("[HyperAMP] ========== Queue Status ==========\n");
    printf("[HyperAMP] TX Queue (Linux -> seL4):\n");
    
    // 安全读取 TX 队列状态
    uint16_t tx_header = hyperamp_safe_read_u16(g_ctx.tx_queue, offsetof(HyperampShmQueue, header));
    uint16_t tx_tail = hyperamp_safe_read_u16(g_ctx.tx_queue, offsetof(HyperampShmQueue, tail));
    uint16_t tx_capacity = hyperamp_safe_read_u16(g_ctx.tx_queue, offsetof(HyperampShmQueue, capacity));
    uint32_t tx_enqueue = hyperamp_safe_read_u32(g_ctx.tx_queue, offsetof(HyperampShmQueue, enqueue_count));
    uint32_t tx_dequeue = hyperamp_safe_read_u32(g_ctx.tx_queue, offsetof(HyperampShmQueue, dequeue_count));
    
    printf("[HyperAMP]   header: %u, tail: %u, capacity: %u\n",
           tx_header, tx_tail, tx_capacity);
    
    // 计算队列长度
    uint16_t tx_length = (tx_header >= tx_tail) ? (tx_header - tx_tail) : (tx_capacity - tx_tail + tx_header);
    printf("[HyperAMP]   length: %u, enqueued: %u, dequeued: %u\n",
           tx_length, tx_enqueue, tx_dequeue);
    
    // 读取锁状态
    size_t tx_lock_offset = offsetof(HyperampShmQueue, queue_lock);
    uint32_t tx_lock_value = hyperamp_safe_read_u32(g_ctx.tx_queue, tx_lock_offset + offsetof(HyperampSpinlock, lock_value));
    uint32_t tx_lock_owner = hyperamp_safe_read_u32(g_ctx.tx_queue, tx_lock_offset + offsetof(HyperampSpinlock, owner_zone_id));
    uint32_t tx_lock_count = hyperamp_safe_read_u32(g_ctx.tx_queue, tx_lock_offset + offsetof(HyperampSpinlock, lock_count));
    uint32_t tx_lock_contention = hyperamp_safe_read_u32(g_ctx.tx_queue, tx_lock_offset + offsetof(HyperampSpinlock, contention_count));
    
    printf("[HyperAMP]   lock: value=%u, owner=%u, count=%u, contention=%u\n",
           tx_lock_value, tx_lock_owner, tx_lock_count, tx_lock_contention);
    
    printf("[HyperAMP] RX Queue (seL4 -> Linux):\n");
    
    // 安全读取 RX 队列状态
    uint16_t rx_header = hyperamp_safe_read_u16(g_ctx.rx_queue, offsetof(HyperampShmQueue, header));
    uint16_t rx_tail = hyperamp_safe_read_u16(g_ctx.rx_queue, offsetof(HyperampShmQueue, tail));
    uint16_t rx_capacity = hyperamp_safe_read_u16(g_ctx.rx_queue, offsetof(HyperampShmQueue, capacity));
    uint32_t rx_enqueue = hyperamp_safe_read_u32(g_ctx.rx_queue, offsetof(HyperampShmQueue, enqueue_count));
    uint32_t rx_dequeue = hyperamp_safe_read_u32(g_ctx.rx_queue, offsetof(HyperampShmQueue, dequeue_count));
    
    printf("[HyperAMP]   header: %u, tail: %u, capacity: %u\n",
           rx_header, rx_tail, rx_capacity);
    
    uint16_t rx_length = (rx_header >= rx_tail) ? (rx_header - rx_tail) : (rx_capacity - rx_tail + rx_header);
    printf("[HyperAMP]   length: %u, enqueued: %u, dequeued: %u\n",
           rx_length, rx_enqueue, rx_dequeue);
    
    // 读取锁状态
    size_t rx_lock_offset = offsetof(HyperampShmQueue, queue_lock);
    uint32_t rx_lock_value = hyperamp_safe_read_u32(g_ctx.rx_queue, rx_lock_offset + offsetof(HyperampSpinlock, lock_value));
    uint32_t rx_lock_owner = hyperamp_safe_read_u32(g_ctx.rx_queue, rx_lock_offset + offsetof(HyperampSpinlock, owner_zone_id));
    uint32_t rx_lock_count = hyperamp_safe_read_u32(g_ctx.rx_queue, rx_lock_offset + offsetof(HyperampSpinlock, lock_count));
    uint32_t rx_lock_contention = hyperamp_safe_read_u32(g_ctx.rx_queue, rx_lock_offset + offsetof(HyperampSpinlock, contention_count));
    
    printf("[HyperAMP]   lock: value=%u, owner=%u, count=%u, contention=%u\n",
           rx_lock_value, rx_lock_owner, rx_lock_count, rx_lock_contention);
    
    printf("[HyperAMP] Local Statistics:\n");
    printf("[HyperAMP]   TX: %u sent, %u errors\n", g_ctx.tx_count, g_ctx.tx_errors);
    printf("[HyperAMP]   RX: %u received, %u errors\n", g_ctx.rx_count, g_ctx.rx_errors);
    printf("[HyperAMP] ====================================\n");
}

/* ==================== 测试主函数 ==================== */

#ifdef HYPERAMP_TEST_MAIN

static void print_usage(const char *prog)
{
    printf("Usage: %s [options]\n", prog);
    printf("Options:\n");
    printf("  -c          Create/initialize queues (default: connect to existing)\n");
    printf("  -a ADDR     Physical address in hex (default: 0x%lx)\n", SHM_TX_QUEUE_PADDR);
    printf("  -s MSG      Send a test message\n");
    printf("  -r          Receive messages\n");
    printf("  -t          Run interactive test\n");
    printf("  -h          Show this help\n");
}

int main(int argc, char *argv[])
{
    int is_creator = 0;
    uint64_t phys_addr = 0;
    int do_send = 0;
    int do_recv = 0;
    int do_test = 0;
    char *send_msg = NULL;
    
    int opt;
    while ((opt = getopt(argc, argv, "ca:s:rth")) != -1) {
        switch (opt) {
            case 'c':
                is_creator = 1;
                break;
            case 'a':
                phys_addr = strtoull(optarg, NULL, 16);
                break;
            case 's':
                do_send = 1;
                send_msg = optarg;
                break;
            case 'r':
                do_recv = 1;
                break;
            case 't':
                do_test = 1;
                break;
            case 'h':
            default:
                print_usage(argv[0]);
                return (opt == 'h') ? 0 : 1;
        }
    }
    
    printf("========================================\n");
    printf("HyperAMP Linux Client Test\n");
    printf("========================================\n");
    
    // 初始化
    if (hyperamp_linux_init(phys_addr, is_creator) != HYPERAMP_OK) {
        printf("Failed to initialize HyperAMP\n");
        return 1;
    }
    
    // 发送测试消息
    if (do_send && send_msg) {
        printf("\nSending message: %s\n", send_msg);
        if (hyperamp_linux_send_data(1, send_msg, strlen(send_msg)) == HYPERAMP_OK) {
            printf("Message sent successfully\n");
        } else {
            printf("Failed to send message\n");
        }
    }
    
    // 接收消息
    if (do_recv) {
        printf("\nReceiving messages...\n");
        HyperampMsgHeader hdr;
        uint8_t payload[4096];
        uint16_t payload_len;
        
        while (hyperamp_linux_recv(&hdr, payload, sizeof(payload) - 1, &payload_len) == HYPERAMP_OK) {
            payload[payload_len] = '\0';
            printf("Received: type=%u, len=%u, data=[%s]\n", 
                   hdr.proxy_msg_type, payload_len, payload);
        }
        printf("No more messages\n");
    }
    
    // 交互测试
    if (do_test) {
        printf("\nInteractive test mode\n");
        printf("Commands: s <msg> (send), r (receive), q (quit), ? (status)\n");
        
        char line[256];
        while (1) {
            printf("> ");
            fflush(stdout);
            
            if (!fgets(line, sizeof(line), stdin)) break;
            
            // 去除换行符
            size_t len = strlen(line);
            if (len > 0 && line[len-1] == '\n') line[len-1] = '\0';
            
            if (line[0] == 'q') {
                break;
            } else if (line[0] == '?') {
                hyperamp_linux_get_status();
            } else if (line[0] == 's' && line[1] == ' ') {
                char *msg = line + 2;
                hyperamp_linux_send_data(1, msg, strlen(msg));
            } else if (line[0] == 'r') {
                HyperampMsgHeader hdr;
                uint8_t payload[4096];
                uint16_t payload_len;
                
                if (hyperamp_linux_recv(&hdr, payload, sizeof(payload) - 1, &payload_len) == HYPERAMP_OK) {
                    payload[payload_len] = '\0';
                    printf("Received: type=%u, len=%u, data=[%s]\n", 
                           hdr.proxy_msg_type, payload_len, payload);
                } else {
                    printf("No message available\n");
                }
            } else {
                printf("Unknown command. Use ? for status, s <msg> to send, r to receive, q to quit\n");
            }
        }
    }
    
    // 显示最终状态
    hyperamp_linux_get_status();
    
    // 清理
    hyperamp_linux_cleanup();
    
    return 0;
}

#endif /* HYPERAMP_TEST_MAIN */
