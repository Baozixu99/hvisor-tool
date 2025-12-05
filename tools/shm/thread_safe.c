/**
 * Thread-Safe Implementation for HyperAMP
 * 
 * Level 1: Coarse-grained locking
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include "shm/thread_safe.h"

/* ============= 全局锁定义 ============= */

static pthread_mutex_t queue_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t channel_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t memory_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t config_mutex = PTHREAD_MUTEX_INITIALIZER;

/* 读写锁用于配置 (多读少写场景) */
static pthread_rwlock_t config_rwlock = PTHREAD_RWLOCK_INITIALIZER;

/* ============= 配置和统计 ============= */

static struct ThreadSafeConfig global_config = {
    .enable_fine_grained_lock = 0,
    .max_concurrent_clients = 16,
    .enable_lock_debug = 0,
};

static struct LockStats global_stats = {0};
static int thread_safe_initialized = 0;

/* ============= 辅助函数 ============= */

static inline uint64_t get_timestamp_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + ts.tv_nsec / 1000;
}

/* ============= 公共API实现 ============= */

int hyperamp_thread_init(struct ThreadSafeConfig* config) {
    if (thread_safe_initialized) {
        fprintf(stderr, "[Thread-Safe] Already initialized\n");
        return 0;
    }
    
    if (config) {
        memcpy(&global_config, config, sizeof(struct ThreadSafeConfig));
    }
    
    printf("[Thread-Safe] Initializing with config:\n");
    printf("  Fine-grained lock: %s\n", 
           global_config.enable_fine_grained_lock ? "enabled" : "disabled");
    printf("  Max concurrent clients: %d\n", 
           global_config.max_concurrent_clients);
    printf("  Lock debug: %s\n", 
           global_config.enable_lock_debug ? "enabled" : "disabled");
    
    /* 初始化所有互斥锁 */
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_ERRORCHECK);
    
    /* 已经使用静态初始化器,这里只是验证 */
    
    pthread_mutexattr_destroy(&attr);
    
    thread_safe_initialized = 1;
    printf("[Thread-Safe] Initialization complete\n");
    
    return 0;
}

void hyperamp_thread_cleanup(void) {
    if (!thread_safe_initialized) {
        return;
    }
    
    printf("[Thread-Safe] Cleaning up...\n");
    
    hyperamp_print_lock_stats();
    
    /* 销毁互斥锁 */
    pthread_mutex_destroy(&queue_mutex);
    pthread_mutex_destroy(&channel_mutex);
    pthread_mutex_destroy(&memory_mutex);
    pthread_mutex_destroy(&config_mutex);
    pthread_rwlock_destroy(&config_rwlock);
    
    thread_safe_initialized = 0;
    printf("[Thread-Safe] Cleanup complete\n");
}

int hyperamp_get_lock_stats(struct LockStats* stats) {
    if (!stats) return -1;
    
    memcpy(stats, &global_stats, sizeof(struct LockStats));
    return 0;
}

void hyperamp_print_lock_stats(void) {
    printf("\n========== Lock Statistics ==========\n");
    printf("Lock contentions: %lu\n", global_stats.lock_contentions);
    printf("Total lock time: %lu μs (%.2f ms)\n", 
           global_stats.total_lock_time_us,
           global_stats.total_lock_time_us / 1000.0);
    printf("Max lock hold time: %lu μs\n", global_stats.max_lock_hold_us);
    printf("=====================================\n\n");
}

/* ============= 锁接口实现 ============= */

void hyperamp_queue_lock(void) {
    uint64_t start = get_timestamp_us();
    
    int ret = pthread_mutex_lock(&queue_mutex);
    if (ret != 0) {
        fprintf(stderr, "[Thread-Safe] Queue lock failed: %s\n", strerror(ret));
        abort();
    }
    
    uint64_t end = get_timestamp_us();
    uint64_t duration = end - start;
    
    if (duration > 100) {  // 超过100us算竞争
        __sync_fetch_and_add(&global_stats.lock_contentions, 1);
    }
    
    __sync_fetch_and_add(&global_stats.total_lock_time_us, duration);
    
    if (duration > global_stats.max_lock_hold_us) {
        global_stats.max_lock_hold_us = duration;
    }
    
    if (global_config.enable_lock_debug) {
        printf("[Thread-Safe] Queue locked (waited %lu μs)\n", duration);
    }
}

void hyperamp_queue_unlock(void) {
    int ret = pthread_mutex_unlock(&queue_mutex);
    if (ret != 0) {
        fprintf(stderr, "[Thread-Safe] Queue unlock failed: %s\n", strerror(ret));
        abort();
    }
    
    if (global_config.enable_lock_debug) {
        printf("[Thread-Safe] Queue unlocked\n");
    }
}

void hyperamp_channel_lock(void) {
    uint64_t start = get_timestamp_us();
    
    int ret = pthread_mutex_lock(&channel_mutex);
    if (ret != 0) {
        fprintf(stderr, "[Thread-Safe] Channel lock failed: %s\n", strerror(ret));
        abort();
    }
    
    uint64_t end = get_timestamp_us();
    uint64_t duration = end - start;
    
    if (duration > 100) {
        __sync_fetch_and_add(&global_stats.lock_contentions, 1);
    }
    
    __sync_fetch_and_add(&global_stats.total_lock_time_us, duration);
    
    if (global_config.enable_lock_debug) {
        printf("[Thread-Safe] Channel locked (waited %lu μs)\n", duration);
    }
}

void hyperamp_channel_unlock(void) {
    pthread_mutex_unlock(&channel_mutex);
    
    if (global_config.enable_lock_debug) {
        printf("[Thread-Safe] Channel unlocked\n");
    }
}

void hyperamp_memory_lock(void) {
    uint64_t start = get_timestamp_us();
    
    int ret = pthread_mutex_lock(&memory_mutex);
    if (ret != 0) {
        fprintf(stderr, "[Thread-Safe] Memory lock failed: %s\n", strerror(ret));
        abort();
    }
    
    uint64_t end = get_timestamp_us();
    uint64_t duration = end - start;
    
    if (duration > 100) {
        __sync_fetch_and_add(&global_stats.lock_contentions, 1);
    }
    
    __sync_fetch_and_add(&global_stats.total_lock_time_us, duration);
    
    if (global_config.enable_lock_debug) {
        printf("[Thread-Safe] Memory locked (waited %lu μs)\n", duration);
    }
}

void hyperamp_memory_unlock(void) {
    pthread_mutex_unlock(&memory_mutex);
    
    if (global_config.enable_lock_debug) {
        printf("[Thread-Safe] Memory unlocked\n");
    }
}

void hyperamp_config_lock(void) {
    pthread_rwlock_wrlock(&config_rwlock);
}

void hyperamp_config_unlock(void) {
    pthread_rwlock_unlock(&config_rwlock);
}
