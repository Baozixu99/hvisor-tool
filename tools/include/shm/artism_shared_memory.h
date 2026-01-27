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

// Layer 2: Dynamic Shared Pool (50%)
#define ARTISM_DYNAMIC_BLOCKS   128         // 32KB Dynamic Shared
#define ARTISM_DYNAMIC_START_ID 128         // Dynamic blocks are IDs 128-255
#define ARTISM_BITMAP_WORDS     2           // 128 bits / 64 bits = 2 words

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

#define ARTISM_DESC_PER_Q   32 // Allow more descriptors than blocks to handle Dynamic bursts

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

// 4. Global Manager (Metadata Region)
typedef struct {
    // Shared Dynamic Pool Management
    volatile uint64_t dynamic_bitmap[ARTISM_BITMAP_WORDS]; // 0=Free, 1=Used
    ByteFlag bitmap_lock; // Spinlock for bitmap ops (or use atomic instructions)
    
    // Per-Queue Management
    ArtismQueue queues[ARTISM_NUM_QUEUES];
    
    // Debug Buffer for FG-WRR verification
    char debug_buffer[2048];
    
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
