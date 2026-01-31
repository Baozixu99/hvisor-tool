#ifndef _ARTISM_SHARED_MEMORY_H_
#define _ARTISM_SHARED_MEMORY_H_

#include <stdint.h>
#include "spinlock.h"

// ============================================================================
// ARTISM Memory Constants (Fair Comparison to RTISM)
// ============================================================================
#define ARTISM_TOTAL_SIZE       0x10000     // 64KB Total (Same as RTISM)
#define ARTISM_BLOCK_SIZE       256         // 256 Bytes per Block
#define ARTISM_TOTAL_BLOCKS     256         // 64KB / 256B = 256 Blocks

// Layer 1: Static Reserve (50%)
#define ARTISM_STATIC_BLOCKS    128         // 32KB Total Static
#define ARTISM_NUM_QUEUES       8           // 8 Priority Queues
#define ARTISM_BLOCKS_PER_Q     16          // 128 / 8 = 16 Blocks (4KB) per Queue

// Layer 2: Dynamic Shared Pool (Remaining after static)
// 64KB total - 16KB meta = 48KB data = 192 blocks
// Static: 8Q * 16 = 128 blocks
// Dynamic: 192 - 128 = 64 blocks
#define ARTISM_DYNAMIC_BLOCKS   64          // 16KB Dynamic Shared (Corrected)
#define ARTISM_DYNAMIC_START_ID 128         // Dynamic blocks are IDs 128-191
#define ARTISM_BITMAP_WORDS     1           // 64 bits / 64 bits = 1 word

// Base Address (Partitioned from RTISM/HyperAMP region)
// We use the same base, assuming ARTISM replaces RTISM in usage
#define ARTISM_SHM_BASE_PADDR   0xDE400000UL 

// Traffic Types for Semantic Mapping
#define ARTISM_TRAFFIC_RT       0  // Real-Time (Overwrite)
#define ARTISM_TRAFFIC_HR       1  // High-Reliability (Blocking)
#define ARTISM_TRAFFIC_HT       2  // High-Throughput (Borrowing)
#define ARTISM_TRAFFIC_BE       3  // Best-Effort (Drop)

// ============================================================================
// Data Structures
// ============================================================================

// 1. Memory Block (The actual data unit)
typedef struct {
    uint8_t data[ARTISM_BLOCK_SIZE];
} __attribute__((aligned(256), packed)) ArtismBlock;

// 2. Queue Header (Static Cyclic Buffer Metadata)
// Each queue manages its own static blocks [0-15] locally implicitly
// e.g., Queue 0 has blocks 0-15, Queue 1 has 16-31...
typedef struct {
    volatile uint16_t head;      // Write Index (0 - 65535, mask with 15)
    volatile uint16_t tail;      // Read Index
    // Note: We don't store block IDs here because they are fixed for Static Layer
    // But for "Entry", we need to point to either Static or Dynamic block
} __attribute__((aligned(64))) ArtismQueueHeader;

// 3. Queue Entry (Descriptor)
// This is NOT stored in the Block. This is stored in a separate small descriptor ring?
// Wait, to keep it simple and consistent with "Block" management:
// We can use the first few bytes of the Block as the metadata header?
// OR, we maintain a separate Descriptor Ring in the Metadata Region.
// Let's use Separate Descriptor Ring for efficient scanning.

// Increased to 64 to ensure WRR scheduler has enough packets to demonstrate weight ratio.
#define ARTISM_DESC_PER_Q   64

typedef struct {
    volatile uint16_t block_id; // 0-255 (0xFFFF = Invalid)
    uint16_t len;               // Data length
    uint8_t  type;              // Traffic Type
    uint8_t  flags;             // Bit 0: Is_Dynamic
} EntryDesc;

typedef struct {
    ArtismQueueHeader info;
    EntryDesc descs[ARTISM_DESC_PER_Q]; // The Ring Buffer of Descriptors
} ArtismQueue;

// ============================================================================
// RTT Measurement: ACK Ring Buffer (FreeRTOS -> Linux)
// ============================================================================
#define ARTISM_ACK_RING_SIZE    64

typedef struct {
    volatile uint32_t seq_id;     // Sequence ID from probe
    volatile uint32_t queue_idx;  // Source queue
    volatile uint32_t status;     // 0=empty, 1=ready to read
} ArtismAck;

// Spinlock (must match FreeRTOS definition exactly for struct alignment!)
typedef struct {
    volatile uint32_t flag;
} ArtismSpinlock;

// Debug Statistics Structure (Telemetry)
// Aligned to 64 bytes to prevent false sharing and ensure atomic updates visibility
typedef struct {
    volatile uint32_t rx_count[ARTISM_NUM_QUEUES];      // Packets received successfully
    volatile uint32_t drop_count[ARTISM_NUM_QUEUES];    // Packets dropped (for BE test)
    volatile uint32_t block_count[ARTISM_NUM_QUEUES];   // Times blocked (for HR test)
    volatile uint32_t curr_weight[ARTISM_NUM_QUEUES];   // Current scheduler weight
    volatile uint32_t deadline_miss[ARTISM_NUM_QUEUES]; // Deadline violations
    volatile uint32_t max_weight_seen[ARTISM_NUM_QUEUES]; // Max weight reached (for Adaptive test)
    
    // Reset request flag: Linux sets to 1, FreeRTOS clears after reset
    volatile uint32_t reset_weights_request;
    
    uint32_t _padding[9]; // Pad to align struct size to cache line multiple
} __attribute__((aligned(64))) ArtismDebugStats;

// 4. Global Manager (Metadata Region)
typedef struct {
    // Shared Dynamic Pool Management
    volatile uint64_t dynamic_bitmap[ARTISM_BITMAP_WORDS]; // 0=Free, 1=Used
    ArtismSpinlock bitmap_lock; // Spinlock for bitmap ops (matches FreeRTOS)
    
    // Per-Queue Management
    ArtismQueue queues[ARTISM_NUM_QUEUES];
    
    // Debug Statistics for Verification (Telemetry)
    // Aligned to 64 bytes to prevent false sharing
    ArtismDebugStats stats;
    
    // ACK Ring Buffer for RTT Measurement
    ArtismAck ack_ring[ARTISM_ACK_RING_SIZE];
    volatile uint32_t ack_head;  // FreeRTOS writes (increments)
    volatile uint32_t ack_tail;  // Linux reads (increments)
    
    // ========================================================================
    // RTT Profiling: Timestamps at each critical point (FreeRTOS side)
    // All values are raw counter ticks from cntvct_el0 (50MHz = 20ns/tick)
    // ========================================================================
    volatile uint64_t prof_isr_entry;      // When IRQ handler fires
    volatile uint64_t prof_task_wakeup;    // When server task resumes
    volatile uint64_t prof_packet_read;    // After reading packet from SHM
    volatile uint64_t prof_ack_write;      // After writing ACK to ring
    volatile uint64_t prof_cache_flush;    // After DC CVAC cache clean
    volatile uint64_t prof_freq;           // Timer frequency for conversion
    volatile uint32_t prof_seq_id;         // Which seq this profile is for
    
    // ========================================================================
    // Clock Sync Test: Verify cntvct_el0 synchronization across VMs
    // ========================================================================
    volatile uint64_t sync_linux_t1;       // Linux writes before IRQ
    volatile uint64_t sync_freertos_t2;    // FreeRTOS writes after receiving IRQ
    volatile uint32_t sync_test_flag;      // 0=idle, 1=test requested, 2=test complete
    
    // Four-Message Exchange (NTP-style) for accurate offset calculation
    // Round 1: Linux(t1) -> FreeRTOS(t2)
    // Round 2: FreeRTOS(t3) -> Linux(t4)
    volatile uint64_t sync_t1;             // Linux send time (Round 1)
    volatile uint64_t sync_t2;             // FreeRTOS receive time (Round 1)
    volatile uint64_t sync_t3;             // FreeRTOS send time (Round 2)
    volatile uint64_t sync_t4;             // Linux receive time (Round 2)
    volatile uint32_t sync_phase;          // 0=idle, 1=round1, 2=round2_ready, 3=complete
    
    // ========================================================================
    // Latency Test: Direct one-way latency measurement (requires synced clocks)
    // 因为 hvisor 设置了 CNTVOFF_EL2 = 0，Linux 和 FreeRTOS 的 cntvct_el0 是同步的
    // 可以直接计算单向时延: latency = t2_freertos - t1_linux
    // ========================================================================
    volatile uint64_t latency_t2;          // FreeRTOS 接收时间戳 (ticks)
    volatile uint64_t latency_ns;          // 计算的单向时延 (纳秒)
    volatile uint32_t latency_count;       // 测量计数
    volatile uint32_t _latency_padding;    // 对齐填充
    
} __attribute__((aligned(4096))) ArtismMeta; // 4KB aligned for Metadata

// 5. Root Structure
typedef struct {
    ArtismMeta meta;        // Metadata at start
    // Padding to 4KB or appropriate offset? 
    // Let's put Blocks at a fixed offset, e.g., 16KB offset, to leave room for meta
    uint8_t reserved[0x4000 - sizeof(ArtismMeta)]; 
    
    // Data Blocks Region
    // This starts at offset 0x4000 (16KB)
    // Size: 256 * 256 = 64KB... wait, total size is 64KB?
    // If Total is 64KB, we can't have 64KB blocks + Metadata.
    // ADJUSTMENT: Total Size 64KB INCLUDES Metadata.
    
    // RE-CALCULATION for 64KB Total:
    // Metadata Size ~= 8 * 32 * 8 bytes + bitmap ~= 2KB + overhead.
    // Let's reserve 4KB for Metadata.
    // Remaining 60KB for Blocks.
    // 60KB / 256B = 240 Blocks.
    // Static: 8 Q * 14 blocks = 112 blocks.
    // Dynamic: 128 blocks (32KB).
    // Total 240 blocks fit.
    
    ArtismBlock blocks[0]; // Flexible array, pointing to 0x4000
} ArtismSharedLayout;

// Helper Macros
#define ARTISM_META_SIZE        0x4000      // 16KB reserved for Meta (Generous)
#define ARTISM_DATA_OFFSET      ARTISM_META_SIZE

#endif // _ARTISM_SHARED_MEMORY_H_
