/**
 * @file hyperamp_shm_queue.h
 * @brief HyperAMP 共享内存队列 - 完全兼容 HighSpeedCProxy 接口
 * 
 * 本文件实现与 HighSpeedCProxy/include/shared_mem_io.h 完全兼容的
 * 共享内存队列数据结构，同时添加了适用于 uncached 内存的软件自旋锁。
 * 
 * 设计目标：
 * 1. 结构布局与 HighSpeedCProxy 的 SharedMemoryPoolQueue 完全一致
 * 2. 支持飞腾派等 ARM 平台的 uncached 内存（不使用原子指令）
 * 3. 提供安全的多进程/多核访问支持
 */

#ifndef HYPERAMP_SHM_QUEUE_H
#define HYPERAMP_SHM_QUEUE_H

#include <stdint.h>
#include <stddef.h>

/* ==================== 常量定义 ==================== */

#define HYPERAMP_ERROR_ADDR             UINT64_MAX
#define HYPERAMP_MAX_MAP_TABLE_ENTRIES  125  /* 优化后: 使队列控制区正好 4KB (1页) */

/* 队列操作结果 */
#define HYPERAMP_OK                     0
#define HYPERAMP_ERROR                  (-1)

/* 内存映射模式 - 与 HighSpeedCProxy 完全一致 */
typedef enum {
    HYPERAMP_MAP_MODE_CONTIGUOUS_BOTH = 0,              // 物理地址连续，逻辑地址连续
    HYPERAMP_MAP_MODE_CONTIGUOUS_PHYS_DISCRETE_LOGICAL  // 物理地址连续，逻辑地址离散
} HyperampMapMode;

/* 消息常量 - 与 HighSpeedCProxy/message.h 一致 */
#define HYPERAMP_MSG_HDR_SIZE           8
#define HYPERAMP_MSG_MIN_SIZE           1
#define HYPERAMP_MSG_MAX_SIZE           4088
#define HYPERAMP_MSG_HDR_PLUS_MAX_SIZE  (HYPERAMP_MSG_HDR_SIZE + HYPERAMP_MSG_MAX_SIZE)

/* ==================== 软件自旋锁 (适用于 uncached 内存) ==================== */

/**
 * @brief 软件自旋锁结构
 * 
 * 使用 volatile + 内存屏障实现，不依赖原子指令（LDXR/STXR），
 * 可以在飞腾派等平台的 uncached 内存上安全工作。
 */
typedef struct {
    volatile uint32_t lock_value;     // 0 = 未锁定, 1 = 已锁定
    volatile uint32_t owner_zone_id;  // 持有锁的 zone ID (用于调试)
    volatile uint32_t lock_count;     // 加锁次数统计
    volatile uint32_t contention_count; // 竞争次数统计
} __attribute__((packed)) HyperampSpinlock;

/* 内存屏障宏 */
#if defined(__aarch64__) || defined(__arm__)
    #define HYPERAMP_DMB()   __asm__ volatile("dmb sy" ::: "memory")
    #define HYPERAMP_DSB()   __asm__ volatile("dsb sy" ::: "memory")
    #define HYPERAMP_ISB()   __asm__ volatile("isb" ::: "memory")
    
    /* 数据缓存清理 - 将缓存行刷新到主存 (用于共享内存写入) */
    static inline void hyperamp_cache_clean(volatile void *addr, size_t size) {
        // volatile char *p = (volatile char *)addr;
        // volatile char *end = p + size;
        // /* ARM64 缓存行通常是 64 字节 */
        // for (; p < end; p += 64) {
        //     __asm__ volatile("dc cvac, %0" : : "r"(p) : "memory");
        // }
        // __asm__ volatile("dsb sy" ::: "memory");
        
        (void)addr; (void)size;
        __asm__ volatile("dmb sy" ::: "memory");
    }
    
    /* 数据缓存失效 - 丢弃缓存内容，强制从内存读取 (用于共享内存读取) */
    static inline void hyperamp_cache_invalidate(volatile void *addr, size_t size) {
        // volatile char *p = (volatile char *)addr;
        // volatile char *end = p + size;
        // for (; p < end; p += 64) {
        //     __asm__ volatile("dc ivac, %0" : : "r"(p) : "memory");
        // }
        // __asm__ volatile("dsb sy" ::: "memory");
        (void)addr; (void)size;
        __asm__ volatile("dmb sy" ::: "memory");
    }
#else
    // #define HYPERAMP_DMB()   __asm__ volatile("mfence" ::: "memory")
    // #define HYPERAMP_DSB()   __asm__ volatile("mfence" ::: "memory")
    // #define HYPERAMP_ISB()   __asm__ volatile("" ::: "memory")
    
    // static inline void hyperamp_cache_clean(volatile void *addr, size_t size) {
    //     (void)addr; (void)size;
    //     __asm__ volatile("mfence" ::: "memory");
    // }
    
    // static inline void hyperamp_cache_invalidate(volatile void *addr, size_t size) {
    //     (void)addr; (void)size;
    //     __asm__ volatile("mfence" ::: "memory");
    // }
#endif

#define HYPERAMP_BARRIER()   do { HYPERAMP_DMB(); HYPERAMP_DSB(); } while(0)

/**
 * @brief 初始化自旋锁
 */
static inline void hyperamp_spinlock_init(volatile HyperampSpinlock *lock)
{
    if (!lock) return;
    
    volatile uint8_t *p = (volatile uint8_t *)lock;
    for (size_t i = 0; i < sizeof(HyperampSpinlock); i++) {
        p[i] = 0;
    }
    HYPERAMP_BARRIER();
    
    /* 刷新锁状态到主存 */
    hyperamp_cache_clean((volatile void *)lock, sizeof(HyperampSpinlock));
}

/**
 * @brief 获取自旋锁 (纯软件实现，无原子指令)
 * @param lock 锁指针
 * @param zone_id 当前 zone 的 ID (用于调试)
 */
static inline void hyperamp_spinlock_lock(volatile HyperampSpinlock *lock, uint32_t zone_id)
{
    if (!lock) return;
    
    int spin_count = 0;
    const int max_spin = 100000;
    
    while (1) {
        HYPERAMP_BARRIER();
        
        volatile uint32_t current = lock->lock_value;
        
        if (current == 0) {
            // 尝试获取锁
            lock->lock_value = 1;
            HYPERAMP_BARRIER();
            
            // 验证是否成功获取
            volatile uint32_t verify = lock->lock_value;
            if (verify == 1) {
                lock->owner_zone_id = zone_id;
                lock->lock_count++;
                HYPERAMP_BARRIER();
                
                /* 刷新锁状态到主存，确保其他核心/虚拟机能看到锁已被占用 */
                hyperamp_cache_clean((volatile void *)lock, sizeof(HyperampSpinlock));
                return;  // 成功获取锁
            }
        }
        
        // 锁被占用，自旋等待
        lock->contention_count++;
        spin_count++;
        
        // 简单的退避策略
        if (spin_count > max_spin) {
            spin_count = 0;
#if defined(__aarch64__) || defined(__arm__)
            __asm__ volatile("yield" ::: "memory");
#else
            __asm__ volatile("pause" ::: "memory");
#endif
        }
        
        // 短暂延迟
        for (volatile int i = 0; i < 100; i++) {
            HYPERAMP_BARRIER();
        }
    }
}

/**
 * @brief 释放自旋锁
 */
static inline void hyperamp_spinlock_unlock(volatile HyperampSpinlock *lock)
{
    if (!lock) return;
    
    HYPERAMP_BARRIER();
    lock->owner_zone_id = 0;
    lock->lock_value = 0;
    HYPERAMP_BARRIER();
    
    /* 关键：刷新锁状态到主存，确保其他核心/虚拟机能看到锁已释放 */
    hyperamp_cache_clean((volatile void *)lock, sizeof(HyperampSpinlock));
}

/**
 * @brief 尝试获取自旋锁（非阻塞）
 * @return 0 成功获取锁, -1 锁被占用
 */
static inline int hyperamp_spinlock_trylock(volatile HyperampSpinlock *lock, uint32_t zone_id)
{
    if (!lock) return HYPERAMP_ERROR;
    
    HYPERAMP_BARRIER();
    volatile uint32_t current = lock->lock_value;
    
    if (current == 0) {
        lock->lock_value = 1;
        HYPERAMP_BARRIER();
        
        volatile uint32_t verify = lock->lock_value;
        if (verify == 1) {
            lock->owner_zone_id = zone_id;
            lock->lock_count++;
            HYPERAMP_BARRIER();
            return HYPERAMP_OK;
        }
    }
    
    return HYPERAMP_ERROR;
}

/* ==================== 地址映射表项 ==================== */

/**
 * @brief 地址映射表项 - 与 HighSpeedCProxy 的 MapTableEntry 完全一致
 */
typedef struct {
    uint64_t virt_addr;   // 虚拟地址
    uint64_t phy_addr;    // 物理地址
} __attribute__((packed)) HyperampMapTableEntry;

/* ==================== 共享内存池队列 ==================== */

/**
 * @brief 共享内存池队列 - 与 HighSpeedCProxy 的 SharedMemoryPoolQueue 完全兼容
 * 
 * 内存布局完全匹配 HighSpeedCProxy，以确保跨系统兼容性：
 * - map_mode1/map_mode2: 各端的内存映射模式
 * - header/tail: 环形缓冲区的头尾索引
 * - capacity: 队列容量（元素数量）
 * - block_size: 每个元素的固定大小
 * - phy_addr: 物理地址
 * - virt_addr1/virt_addr2: 各端的虚拟地址
 * - table1/table2: 离散映射时的地址映射表
 * 
 * 扩展部分（放在结构体末尾，不影响兼容性）：
 * - tx_lock/rx_lock: 发送/接收自旋锁
 */
typedef struct {
    /* === HighSpeedCProxy 兼容部分 (偏移量必须完全一致) === */
    
    uint8_t  map_mode1;    // Linux 端的内存映射模式
    uint8_t  map_mode2;    // 微内核端的内存映射模式
    uint16_t header;       // 环形缓冲区头索引（指向下一个出队位置）
    uint16_t tail;         // 环形缓冲区尾索引（指向下一个入队位置）
    uint16_t capacity;     // 队列容量（最大元素数量）
    uint16_t block_size;   // 每个元素的内存块大小
    uint16_t _reserved;    // 保留，对齐用
    
    uint64_t phy_addr;     // 队列控制块的物理地址
    uint64_t virt_addr1;   // Linux 端的虚拟地址
    uint64_t virt_addr2;   // 微内核端的虚拟地址
    
    HyperampMapTableEntry table1[HYPERAMP_MAX_MAP_TABLE_ENTRIES];  // Linux 端地址映射表
    HyperampMapTableEntry table2[HYPERAMP_MAX_MAP_TABLE_ENTRIES];  // 微内核端地址映射表
    
    /* === HyperAMP 扩展部分 (放在末尾，确保兼容性) === */
    
    HyperampSpinlock queue_lock;  // 队列操作锁
    
    uint32_t magic;               // 魔数，用于验证初始化状态
    uint32_t version;             // 版本号
    uint32_t enqueue_count;       // 入队计数
    uint32_t dequeue_count;       // 出队计数
    
} __attribute__((packed)) HyperampShmQueue;

/* 魔数定义 */
#define HYPERAMP_QUEUE_MAGIC        0x48415150  // "HAQP" - HyperAmp Queue Protocol

/* ==================== 消息头结构 ==================== */

/**
 * @brief 代理消息头 - 与 HighSpeedCProxy 的 ProxyMsgHeader 完全一致 (8字节)
 */
typedef struct {
    uint8_t  version;           // 协议版本
    uint8_t  proxy_msg_type;    // 消息类型: 0=设备, 1=策略, 2=会话, 3=数据
    uint16_t frontend_sess_id;  // 前端会话ID
    uint16_t backend_sess_id;   // 后端会话ID
    uint16_t payload_len;       // 载荷长度
} __attribute__((packed)) HyperampMsgHeader;

/* 消息类型定义 - 与 HighSpeedCProxy 一致 */
typedef enum {
    HYPERAMP_MSG_TYPE_DEV = 0,     // 设备消息
    HYPERAMP_MSG_TYPE_STRGY = 1,   // 策略消息
    HYPERAMP_MSG_TYPE_SESS = 2,    // 会话消息
    HYPERAMP_MSG_TYPE_DATA = 3,    // 数据消息
    HYPERAMP_MSG_TYPE_SERVICE = 0x10 // 服务调用: frontend_sess_id = service_id
} HyperampMsgType;

/* ==================== 队列配置结构 ==================== */

/**
 * @brief 队列初始化配置 - 与 HighSpeedCProxy 的 SharedMemoryPoolQueueConfig 兼容
 */
typedef struct {
    uint16_t map_mode;      // 内存映射模式
    uint16_t capacity;      // 队列容量
    uint16_t block_size;    // 块大小
    uint16_t _reserved;
    uint64_t phy_addr;      // 物理地址
    uint64_t virt_addr;     // 虚拟地址
} HyperampQueueConfig;

/* ==================== 队列操作宏 ==================== */

/**
 * @brief 计算消息总大小
 */
#define HYPERAMP_MSG_TOTAL_SIZE(hdr) \
    (sizeof(HyperampMsgHeader) + (hdr)->payload_len)

/**
 * @brief 获取 header 指向的元素虚拟地址
 * 注意：数据区从 (header+1)*block_size 开始，header=0 时第一个数据块在 block_size 偏移处
 */
#define HYPERAMP_QUEUE_HEADER_VIRT_ADDR(queue, virt_base) \
    ((uint64_t)(virt_base) + (uint64_t)((queue)->header + 1) * (uint64_t)(queue)->block_size)

/**
 * @brief 获取 tail 指向的元素虚拟地址
 */
#define HYPERAMP_QUEUE_TAIL_VIRT_ADDR(queue, virt_base) \
    ((uint64_t)(virt_base) + (uint64_t)((queue)->tail + 1) * (uint64_t)(queue)->block_size)

/**
 * @brief 检查队列是否为空
 */
#define HYPERAMP_QUEUE_IS_EMPTY(queue) \
    ((queue)->tail == (queue)->header)

/**
 * @brief 检查队列是否已满
 */
#define HYPERAMP_QUEUE_IS_FULL(queue) \
    ((((queue)->header + 1) % (queue)->capacity) == (queue)->tail)

/**
 * @brief 计算队列中的元素数量
 */
#define HYPERAMP_QUEUE_LENGTH(queue) \
    (((queue)->header >= (queue)->tail) ? \
     ((queue)->header - (queue)->tail) : \
     ((queue)->capacity - (queue)->tail + (queue)->header))

/* ==================== 安全内存操作函数 ==================== */

/**
 * @brief 安全的 memset（逐字节，适用于 uncached 内存）
 */
static inline void hyperamp_safe_memset(volatile void *dst, uint8_t val, size_t len)
{
    volatile uint8_t *p = (volatile uint8_t *)dst;
    for (size_t i = 0; i < len; i++) {
        p[i] = val;
    }
    HYPERAMP_BARRIER();
}

/**
 * @brief 安全的 memcpy（逐字节，适用于 uncached 内存）
 */
static inline void hyperamp_safe_memcpy(volatile void *dst, const volatile void *src, size_t len)
{
    volatile uint8_t *d = (volatile uint8_t *)dst;
    const volatile uint8_t *s = (const volatile uint8_t *)src;
    for (size_t i = 0; i < len; i++) {
        d[i] = s[i];
    }
    HYPERAMP_BARRIER();
}

/**
 * @brief 安全读取 uint16_t（逐字节）
 */
static inline uint16_t hyperamp_safe_read_u16(const volatile void *addr, size_t offset)
{
    const volatile uint8_t *p = (const volatile uint8_t *)addr;
    uint16_t val = 0;
    for (int i = 0; i < 2; i++) {
        val |= ((uint16_t)p[offset + i]) << (i * 8);
    }
    HYPERAMP_BARRIER();
    return val;
}

/**
 * @brief 安全读取 uint32_t（逐字节）
 */
static inline uint32_t hyperamp_safe_read_u32(const volatile void *addr, size_t offset)
{
    const volatile uint8_t *p = (const volatile uint8_t *)addr;
    uint32_t val = 0;
    for (int i = 0; i < 4; i++) {
        val |= ((uint32_t)p[offset + i]) << (i * 8);
    }
    HYPERAMP_BARRIER();
    return val;
}

/**
 * @brief 安全读取 uint64_t（逐字节）
 */
static inline uint64_t hyperamp_safe_read_u64(const volatile void *addr, size_t offset)
{
    const volatile uint8_t *p = (const volatile uint8_t *)addr;
    uint64_t val = 0;
    for (int i = 0; i < 8; i++) {
        val |= ((uint64_t)p[offset + i]) << (i * 8);
    }
    HYPERAMP_BARRIER();
    return val;
}

/* ==================== 队列操作函数 ==================== */

/**
 * @brief 初始化共享内存队列
 * @param queue 队列指针（指向共享内存）
 * @param config 配置参数
 * @param is_creator 是否为队列创建者（创建者负责初始化所有字段）
 * @return HYPERAMP_OK 成功, HYPERAMP_ERROR 失败
 */
static inline int hyperamp_queue_init(volatile HyperampShmQueue *queue, 
                                       const HyperampQueueConfig *config,
                                       int is_creator)
{
    printf("[HyperAmp] Initializing shared memory queue...\n");
    if (!queue || !config) return HYPERAMP_ERROR;
    if (config->block_size == 0 || config->capacity == 0) return HYPERAMP_ERROR;
    printf("[HyperAmp] Queue config: block_size=%d, capacity=%d, map_mode=%d\n",
           config->block_size, config->capacity, config->map_mode);
    if (is_creator) {
        // 创建者：使用安全的逐字节写入方式（避免编译器优化为不安全的指令）
        printf("[HyperAmp] Writing fields byte by byte...\n");
        volatile uint8_t *p = (volatile uint8_t *)queue;
        
        // 写入 map_mode1 和 map_mode2
        p[0] = config->map_mode;
        p[1] = config->map_mode;
        HYPERAMP_BARRIER();
        
        // 写入 header (uint16_t, offset 2)
        p[2] = 0;
        p[3] = 0;
        HYPERAMP_BARRIER();
        
        // 写入 tail (uint16_t, offset 4)
        p[4] = 0;
        p[5] = 0;
        HYPERAMP_BARRIER();
        
        // 写入 capacity (uint16_t, offset 6)
        uint16_t cap = config->capacity;
        p[6] = cap & 0xFF;
        p[7] = (cap >> 8) & 0xFF;
        HYPERAMP_BARRIER();
        
        // 写入 block_size (uint16_t, offset 8)
        uint16_t bs = config->block_size;
        p[8] = bs & 0xFF;
        p[9] = (bs >> 8) & 0xFF;
        HYPERAMP_BARRIER();
        
        // 写入 _reserved (uint16_t, offset 10)
        p[10] = 0;
        p[11] = 0;
        HYPERAMP_BARRIER();
        
        printf("[HyperAmp] Writing phy_addr...\n");
        // 写入 phy_addr (uint64_t, offset 12) - 逐字节
        uint64_t pa = config->phy_addr;
        for (int i = 0; i < 8; i++) {
            p[12 + i] = (pa >> (i * 8)) & 0xFF;
        }
        HYPERAMP_BARRIER();
        
        printf("[HyperAmp] Writing virt_addr1...\n");
        // 写入 virt_addr1 (uint64_t, offset 20)
        uint64_t va = config->virt_addr;
        for (int i = 0; i < 8; i++) {
            p[20 + i] = (va >> (i * 8)) & 0xFF;
        }
        HYPERAMP_BARRIER();
        
        printf("[HyperAmp] Writing virt_addr2...\n");
        // 写入 virt_addr2 (uint64_t, offset 28) - 清零
        for (int i = 0; i < 8; i++) {
            p[28 + i] = 0;
        }
        HYPERAMP_BARRIER();
        
        // 跳过地址映射表（table1 和 table2），不需要初始化
        printf("[HyperAmp] Skipping address mapping tables...\n");
        
        // 初始化自旋锁（在映射表之后的偏移）
        printf("[HyperAmp] Initializing spinlock...\n");
        size_t lock_offset = offsetof(HyperampShmQueue, queue_lock);
        volatile HyperampSpinlock *lock = (volatile HyperampSpinlock *)&p[lock_offset];
        hyperamp_spinlock_init(lock);
        
        // 初始化扩展字段
        printf("[HyperAmp] Writing magic and version...\n");
        size_t magic_offset = offsetof(HyperampShmQueue, magic);
        
        // 写入 magic (uint32_t) - 逐字节
        uint32_t magic = HYPERAMP_QUEUE_MAGIC;
        for (int i = 0; i < 4; i++) {
            p[magic_offset + i] = (magic >> (i * 8)) & 0xFF;
        }
        HYPERAMP_BARRIER();
        
        // 写入 version (uint32_t)
        uint32_t version = 1;
        for (int i = 0; i < 4; i++) {
            p[magic_offset + 4 + i] = (version >> (i * 8)) & 0xFF;
        }
        HYPERAMP_BARRIER();
        
        // 写入 enqueue_count (uint32_t) - 清零
        for (int i = 0; i < 4; i++) {
            p[magic_offset + 8 + i] = 0;
        }
        HYPERAMP_BARRIER();
        
        // 写入 dequeue_count (uint32_t) - 清零
        for (int i = 0; i < 4; i++) {
            p[magic_offset + 12 + i] = 0;
        }
        HYPERAMP_BARRIER();
        
        /* 关键：刷新整个队列控制块的缓存到主存，确保其他核心/虚拟机能看到最新数据 */
        printf("[HyperAmp] Flushing cache to memory...\n");
        hyperamp_cache_clean((volatile void *)queue, sizeof(HyperampShmQueue));
        
        printf("[HyperAmp] Queue initialization complete!\n");
    } else {
        // 非创建者：只设置自己的虚拟地址
        queue->virt_addr2 = config->virt_addr;
        HYPERAMP_BARRIER();
        
        /* 刷新修改后的虚拟地址字段到主存 */
        hyperamp_cache_clean((volatile void *)&queue->virt_addr2, 8);
    }
    
    return HYPERAMP_OK;
}

/**
 * @brief 检查队列是否已初始化（安全的逐字节读取）
 */
static inline int hyperamp_queue_is_initialized(volatile HyperampShmQueue *queue)
{
    if (!queue) return 0;
    
    HYPERAMP_BARRIER();
    
    // 不使用 magic 字段(offset 4052 > 4096,会跨页),改用 capacity 字段(offset 6)
    size_t capacity_offset = offsetof(HyperampShmQueue, capacity);
    volatile uint8_t *p = (volatile uint8_t *)queue;
    
    uint16_t capacity = 0;
    for (int i = 0; i < 2; i++) {
        capacity |= ((uint16_t)p[capacity_offset + i]) << (i * 8);
    }
    
    HYPERAMP_BARRIER();
    // 队列已初始化的标志: capacity > 0
    return (capacity > 0);
}

/**
 * @brief 入队操作（带锁）- 与 HighSpeedCProxy 的 SHMP_QUEUE_ENQUEUE 逻辑一致
 * @param queue 队列指针
 * @param zone_id 当前 zone ID
 * @param data 要入队的数据
 * @param data_len 数据长度
 * @param virt_base 数据区的虚拟地址基址
 * @return HYPERAMP_OK 成功, HYPERAMP_ERROR 失败
 */
static inline int hyperamp_queue_enqueue(volatile HyperampShmQueue *queue,
                                          uint32_t zone_id,
                                          const void *data,
                                          size_t data_len,
                                          volatile void *virt_base)
{
    if (!queue || !data || data_len == 0) return HYPERAMP_ERROR;
    if (data_len > queue->block_size) return HYPERAMP_ERROR;
    
    // 获取锁
    hyperamp_spinlock_lock(&queue->queue_lock, zone_id);
    
    // 计算新的 header
    uint16_t new_header = queue->header + 1;
    if (new_header >= queue->capacity) {
        new_header -= queue->capacity;
    }
    
    // 检查是否会导致队列满（header 追上 tail）
    if (new_header == queue->tail) {
        hyperamp_spinlock_unlock(&queue->queue_lock);
        return HYPERAMP_ERROR;  // 队列满
    }
    
    // 计算数据写入地址：使用当前 header + 1 的位置
    // 数据区布局：[控制块][slot 0][slot 1]...[slot N-1]
    // header 指向最后一个有效数据的位置，新数据写入 header+1
    uint64_t write_addr = (uint64_t)virt_base + (uint64_t)(queue->header + 1) * queue->block_size;
    
    // 写入数据
    hyperamp_safe_memcpy((volatile void *)write_addr, data, data_len);
    
    // 更新 header
    queue->header = new_header;
    queue->enqueue_count++;
    
    HYPERAMP_BARRIER();
    
    /* 刷新写入的数据到内存 */
    hyperamp_cache_clean((volatile void *)write_addr, data_len);
    /* 刷新队列控制块到内存 */
    hyperamp_cache_clean((volatile void *)queue, 64);
    
    // 释放锁
    hyperamp_spinlock_unlock(&queue->queue_lock);
    
    return HYPERAMP_OK;
}

/**
 * @brief 出队操作（带锁）- 与 HighSpeedCProxy 的 SHMP_QUEUE_DEQUEUE 逻辑一致
 * @param queue 队列指针
 * @param zone_id 当前 zone ID
 * @param data 接收数据的缓冲区
 * @param max_len 缓冲区最大长度
 * @param actual_len 实际读取的长度（输出）
 * @param virt_base 数据区的虚拟地址基址
 * @return HYPERAMP_OK 成功, HYPERAMP_ERROR 失败
 */
static inline int hyperamp_queue_dequeue(volatile HyperampShmQueue *queue,
                                          uint32_t zone_id,
                                          void *data,
                                          size_t max_len,
                                          size_t *actual_len,
                                          volatile void *virt_base)
{
    if (!queue || !data || max_len == 0) return HYPERAMP_ERROR;
    
    /* 在读取前失效缓存，确保读取到最新数据 */
    hyperamp_cache_invalidate((volatile void *)queue, 64);
    
    // 获取锁
    hyperamp_spinlock_lock(&queue->queue_lock, zone_id);
    // printf("[HyperAmp] Dequeue: header=%d, tail=%d\n", queue->header, queue->tail);
    // fflush(stdout);
    // 检查队列是否为空
    if (queue->tail == queue->header) {
        hyperamp_spinlock_unlock(&queue->queue_lock);
        return HYPERAMP_ERROR;  // 队列空
    }
    // 计算读取地址：tail + 1 的位置
    uint64_t read_addr = (uint64_t)virt_base + (uint64_t)(queue->tail + 1) * queue->block_size;
    
    /* 失效数据区缓存，确保读取到最新数据 */
    hyperamp_cache_invalidate((volatile void *)read_addr, queue->block_size);
    
    // 计算实际读取长度
    size_t read_len = (max_len < queue->block_size) ? max_len : queue->block_size;
    
    // 读取数据
    hyperamp_safe_memcpy(data, (const volatile void *)read_addr, read_len);
    
    if (actual_len) {
        *actual_len = read_len;
    }
    
    // 更新 tail
    uint16_t new_tail = queue->tail + 1;
    if (new_tail >= queue->capacity) {
        new_tail -= queue->capacity;
    }
    queue->tail = new_tail;
    queue->dequeue_count++;
    
    HYPERAMP_BARRIER();
    
    // 释放锁
    hyperamp_spinlock_unlock(&queue->queue_lock);
    return HYPERAMP_OK;
}

/**
 * @brief 窥视队首元素（不出队）
 */
static inline int hyperamp_queue_peek(volatile HyperampShmQueue *queue,
                                       uint32_t zone_id,
                                       void *data,
                                       size_t max_len,
                                       size_t *actual_len,
                                       volatile void *virt_base)
{
    if (!queue || !data || max_len == 0) return HYPERAMP_ERROR;
    
    hyperamp_spinlock_lock(&queue->queue_lock, zone_id);
    
    if (queue->tail == queue->header) {
        hyperamp_spinlock_unlock(&queue->queue_lock);
        return HYPERAMP_ERROR;
    }
    
    uint64_t read_addr = (uint64_t)virt_base + (uint64_t)(queue->tail + 1) * queue->block_size;
    size_t read_len = (max_len < queue->block_size) ? max_len : queue->block_size;
    
    hyperamp_safe_memcpy(data, (const volatile void *)read_addr, read_len);
    
    if (actual_len) {
        *actual_len = read_len;
    }
    
    HYPERAMP_BARRIER();
    hyperamp_spinlock_unlock(&queue->queue_lock);
    
    return HYPERAMP_OK;
}

/**
 * @brief 分配一个入队槽位（不锁定，用于零拷贝场景）
 * @param queue 队列指针
 * @param zone_id 当前 zone ID
 * @param slot_addr 输出：槽位的虚拟地址
 * @param virt_base 数据区基址
 * @return HYPERAMP_OK 成功, HYPERAMP_ERROR 失败
 */
static inline int hyperamp_queue_alloc_slot(volatile HyperampShmQueue *queue,
                                             uint32_t zone_id,
                                             uint64_t *slot_addr,
                                             volatile void *virt_base)
{
    if (!queue || !slot_addr) return HYPERAMP_ERROR;
    
    hyperamp_spinlock_lock(&queue->queue_lock, zone_id);
    
    uint16_t next_header = (queue->header + 1) % queue->capacity;
    
    if (next_header == queue->tail) {
        hyperamp_spinlock_unlock(&queue->queue_lock);
        return HYPERAMP_ERROR;
    }
    
    // 返回当前可写入的槽位地址
    *slot_addr = (uint64_t)virt_base + (uint64_t)(queue->header + 1) * queue->block_size;
    
    // 更新 header
    queue->header = next_header;
    queue->enqueue_count++;
    
    HYPERAMP_BARRIER();
    hyperamp_spinlock_unlock(&queue->queue_lock);
    
    return HYPERAMP_OK;
}

/**
 * @brief 释放一个出队槽位（不锁定，用于零拷贝场景）
 */
static inline int hyperamp_queue_release_slot(volatile HyperampShmQueue *queue,
                                               uint32_t zone_id)
{
    if (!queue) return HYPERAMP_ERROR;
    
    hyperamp_spinlock_lock(&queue->queue_lock, zone_id);
    
    if (queue->tail == queue->header) {
        hyperamp_spinlock_unlock(&queue->queue_lock);
        return HYPERAMP_ERROR;
    }
    
    uint16_t new_tail = (queue->tail + 1) % queue->capacity;
    queue->tail = new_tail;
    queue->dequeue_count++;
    
    HYPERAMP_BARRIER();
    hyperamp_spinlock_unlock(&queue->queue_lock);
    
    return HYPERAMP_OK;
}

#endif /* HYPERAMP_SHM_QUEUE_H */
