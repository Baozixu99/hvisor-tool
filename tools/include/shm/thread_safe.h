/**
 * Thread-Safe HyperAMP Communication
 * 
 * 提供多线程安全的客户端/服务端支持
 * 
 * 使用方法:
 * 1. 在初始化前调用 hyperamp_thread_init()
 * 2. 正常使用 client_ops 和 server 函数
 * 3. 程序结束前调用 hyperamp_thread_cleanup()
 */

#ifndef __HYPERAMP_THREAD_SAFE_H__
#define __HYPERAMP_THREAD_SAFE_H__

#include <pthread.h>
#include <stdint.h>

/**
 * 线程安全配置
 */
struct ThreadSafeConfig {
    int enable_fine_grained_lock;  // 是否启用细粒度锁 (Level 2)
    int max_concurrent_clients;    // 最大并发客户端数
    int enable_lock_debug;         // 是否启用锁调试日志
};

/**
 * 锁统计信息
 */
struct LockStats {
    uint64_t lock_contentions;     // 锁竞争次数
    uint64_t total_lock_time_us;   // 总加锁时间(微秒)
    uint64_t max_lock_hold_us;     // 最大持锁时间(微秒)
};

/**
 * 初始化线程安全子系统
 * 
 * @param config 配置参数,NULL表示使用默认配置
 * @return 0表示成功,-1表示失败
 */
int hyperamp_thread_init(struct ThreadSafeConfig* config);

/**
 * 清理线程安全子系统
 */
void hyperamp_thread_cleanup(void);

/**
 * 获取锁统计信息
 * 
 * @param stats 输出参数,接收统计信息
 * @return 0表示成功
 */
int hyperamp_get_lock_stats(struct LockStats* stats);

/**
 * 打印锁统计信息
 */
void hyperamp_print_lock_stats(void);

/* ============= 内部使用的锁接口 ============= */

/**
 * 队列锁 - 保护消息队列操作
 */
void hyperamp_queue_lock(void);
void hyperamp_queue_unlock(void);

/**
 * 通道锁 - 保护通道初始化和状态
 */
void hyperamp_channel_lock(void);
void hyperamp_channel_unlock(void);

/**
 * 内存锁 - 保护共享内存分配
 */
void hyperamp_memory_lock(void);
void hyperamp_memory_unlock(void);

/**
 * 配置锁 - 保护全局配置读写
 */
void hyperamp_config_lock(void);
void hyperamp_config_unlock(void);

/**
 * RAII 风格的锁守卫宏
 * 
 * 用法:
 *   HYPERAMP_LOCK_GUARD(queue) {
 *       // 临界区代码
 *   }
 */
#define HYPERAMP_LOCK_GUARD(type) \
    for (int _lock_##type = (hyperamp_##type##_lock(), 1); \
         _lock_##type; \
         hyperamp_##type##_unlock(), _lock_##type = 0)

#endif /* __HYPERAMP_THREAD_SAFE_H__ */
