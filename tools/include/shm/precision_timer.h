#ifndef PRECISION_TIMER_H
#define PRECISION_TIMER_H

#include <stdint.h>
#include <stdio.h>

/**
 * ARM64 System Counter 高精度计时器
 * 
 * 使用 ARM Generic Timer 的系统寄存器直接读取硬件计数器
 * 避免系统调用开销,提供纳秒级精度
 * 
 * 相关寄存器：
 * - CNTFRQ_EL0: 计数器频率寄存器（飞腾派为 50MHz）
 * - CNTPCT_EL0: 物理计数器寄存器（单调递增的 64 位计数值）
 * 
 * 注意：需要 Hypervisor 允许 Guest 访问这些 EL0 系统寄存器
 */

/**
 * 读取 ARM Generic Timer 频率
 * 
 * 从 CNTFRQ_EL0 系统寄存器读取计数器频率
 * 飞腾派开发板：50MHz (0x2faf080 = 50,000,000 Hz)
 * 
 * @return 计数器频率 (Hz)
 */
static inline uint64_t get_cntfrq(void) {
    uint64_t freq;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r" (freq));
    return freq;
}

/**
 * 读取 ARM Generic Timer 当前计数值
 * 
 * 尝试读取虚拟计数器 CNTVCT_EL0 (而非物理计数器 CNTPCT_EL0)
 * 虚拟计数器在虚拟化环境中通常更容易访问
 * 
 * @return 当前计数值 (ticks)
 */
static inline uint64_t get_cntpct(void) {
    uint64_t count;
    __asm__ volatile(
        "isb\n\t"   
        "mrs %0, cntvct_el0"         // 读取虚拟计数器 (替代物理计数器)
        : "=r" (count)
        :
        : "memory"
    );
    return count;
}

/**
 * 将 ticks 差值转换为微秒 (μs)
 * 
 * 计算公式：μs = (ticks * 1,000,000) / freq
 * 
 * 示例（飞腾派 50MHz）：
 * - 1 tick = 20 ns
 * - 50 ticks = 1 μs
 * - 50,000 ticks = 1 ms
 * 
 * @param ticks_start 起始计数值
 * @param ticks_end   结束计数值
 * @param freq        计数器频率 (Hz)
 * @return 时间差（微秒）
 */
static inline uint64_t ticks_to_us(uint64_t ticks_start, uint64_t ticks_end, uint64_t freq) {
    uint64_t ticks_diff = ticks_end - ticks_start;
    // 使用 64 位运算避免溢出
    // (ticks * 1000000) / freq
    return (ticks_diff * 1000000ULL) / freq;
}

/**
 * 将 ticks 差值转换为纳秒 (ns)
 * 
 * 计算公式：ns = (ticks * 1,000,000,000) / freq
 * 
 * @param ticks_start 起始计数值
 * @param ticks_end   结束计数值
 * @param freq        计数器频率 (Hz)
 * @return 时间差（纳秒）
 */
static inline uint64_t ticks_to_ns(uint64_t ticks_start, uint64_t ticks_end, uint64_t freq) {
    uint64_t ticks_diff = ticks_end - ticks_start;
    return (ticks_diff * 1000000000ULL) / freq;
}

/**
 * 将 ticks 差值转换为毫秒 (ms)
 * 
 * 计算公式：ms = (ticks * 1,000) / freq
 * 
 * @param ticks_start 起始计数值
 * @param ticks_end   结束计数值
 * @param freq        计数器频率 (Hz)
 * @return 时间差（毫秒）
 */
static inline uint64_t ticks_to_ms(uint64_t ticks_start, uint64_t ticks_end, uint64_t freq) {
    uint64_t ticks_diff = ticks_end - ticks_start;
    return (ticks_diff * 1000ULL) / freq;
}

/**
 * 将 ticks 差值转换为秒 (s)
 * 
 * 计算公式：s = ticks / freq
 * 
 * @param ticks_start 起始计数值
 * @param ticks_end   结束计数值
 * @param freq        计数器频率 (Hz)
 * @return 时间差（秒）
 */
static inline double ticks_to_seconds(uint64_t ticks_start, uint64_t ticks_end, uint64_t freq) {
    uint64_t ticks_diff = ticks_end - ticks_start;
    return (double)ticks_diff / (double)freq;
}

/**
 * 获取计数器精度信息
 * 
 * @param freq 计数器频率
 * @return 每个 tick 对应的纳秒数
 */
static inline double get_timer_precision_ns(uint64_t freq) {
    return 1000000000.0 / (double)freq;
}

/**
 * 打印计时器信息（调试用）
 */
static inline void print_timer_info(void) {
    uint64_t freq = get_cntfrq();
    double precision_ns = get_timer_precision_ns(freq);
    
    printf("=== ARM64 System Counter Information ===\n");
    printf("Frequency (CNTFRQ_EL0): %lu Hz (%.2f MHz)\n", freq, freq / 1000000.0);
    printf("Timer Precision: %.2f ns per tick\n", precision_ns);
    printf("Expected Frequency (Phytium Pi): 50,000,000 Hz (50 MHz)\n");
    printf("========================================\n");
}

#endif // PRECISION_TIMER_H
