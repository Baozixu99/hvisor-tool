#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <time.h>

#include "hvisor.h"
#include "shm/rtism_shared_memory.h"
#include "shm/msgqueue.h"  // For AmpMsgQueue struct, ops
#include "shm/precision_timer.h"  // High-precision ARM64 timer
#include "rtism_lpa.h"

// Device-safe memory copy (avoids NEON/SIMD instructions that cause Bus Error on uncached memory)
static void device_memcpy(volatile void *dst, const void *src, size_t len) {
    volatile char *d = (volatile char *)dst;
    const char *s = (const char *)src;
    for (size_t i = 0; i < len; i++) {
        d[i] = s[i];
    }
    __asm__ volatile("dmb sy" ::: "memory");
}

// Reusing HyperAMP definitions
#define MEM_DRIVE "/dev/mem"
#define HVISOR_DRIVE "/dev/hvisor"

// MMIO Control Region (Same as HyperAMP)
#define HYPERAMP_CTRL_PA    0x6e410000
#define HYPERAMP_CTRL_SIZE  64
struct HyperAMPCtrl {
    uint32_t ipi_trigger;
    uint32_t reserved[15];
};

static struct HyperAMPCtrl* rtism_mmio_ctrl = NULL;
static struct AmpMsgQueue* rtism_queues[RTISM_NUM_PRIORITIES]; // Virtual Addresses
static int mem_fd = -1;

// ----------------------------------------------------------------------
// 1. Initialization
// ----------------------------------------------------------------------
int rtism_init() {
    mem_fd = open(MEM_DRIVE, O_RDWR | O_SYNC);
    if (mem_fd < 0) {
        perror("[RTISM] Failed to open /dev/mem");
        return -1;
    }

    // Map MMIO Control
    rtism_mmio_ctrl = (struct HyperAMPCtrl*)mmap(NULL, HYPERAMP_CTRL_SIZE, 
                                                 PROT_READ | PROT_WRITE, MAP_SHARED, 
                                                 mem_fd, HYPERAMP_CTRL_PA);
    if (rtism_mmio_ctrl == MAP_FAILED) {
        perror("[RTISM] Failed to map MMIO");
        return -1;
    }

    // Map 8 Priority Queues
    // Base PA: RTISM_SHM_BASE_PADDR (0xDE400000)
    // Each Queue Size: RTISM_QUEUE_SIZE (8192 bytes)
    for (int i = 0; i < RTISM_NUM_PRIORITIES; i++) {
        unsigned long pa = RTISM_GET_QUEUE_PADDR(i);
        rtism_queues[i] = (struct AmpMsgQueue*)mmap(NULL, RTISM_QUEUE_SIZE,
                                                    PROT_READ | PROT_WRITE, MAP_SHARED,
                                                    mem_fd, pa);
        if (rtism_queues[i] == MAP_FAILED) {
            printf("[RTISM] Failed to map Queue %d at 0x%lx\n", i, pa);
            return -1;
        }

        // Initialize Queue Header if not already initialized
        // (Assuming simple init logic, or check mark)
        if (rtism_queues[i]->working_mark != INIT_MARK_INITIALIZED) {
             rtism_queues[i]->buf_size = 128; // Example size
             rtism_queues[i]->working_mark = INIT_MARK_INITIALIZED;
             rtism_queues[i]->empty_h = 0;
             rtism_queues[i]->wait_h = 0;
             rtism_queues[i]->proc_ing_h = 0;
             printf("[RTISM] Initialized Queue %d\n", i);
        }
    }
    
    // Warmup MMIO (Trigger Page Fault)
    rtism_mmio_ctrl->ipi_trigger = 0;
    
    printf("[RTISM] Driver Initialized. 8 Queues Mapped.\n");
    return 0;
}

// ----------------------------------------------------------------------
// 2. Dispatcher (Enqueue only, no IRQ trigger)
// ----------------------------------------------------------------------
int rtism_enqueue_flow(int priority, void* data, int len, int service_id) {
    if (priority < 0 || priority >= RTISM_NUM_PRIORITIES) return -1;
    
    struct AmpMsgQueue* queue = rtism_queues[priority];
    
    // Use priority-based offset to isolate data regions for each queue
    int data_offset = priority * 0x10000;
    
    // Map Data Buffer (Once)
    static char* data_base = NULL;
    if (!data_base) {
         data_base = mmap(NULL, 0x100000, PROT_READ|PROT_WRITE, MAP_SHARED, mem_fd, 0xDE000000);
    }
    
    device_memcpy(data_base + data_offset, data, len);
    
    // Update Queue
    int idx = queue->wait_h;
    queue->entries[idx].msg.offset = data_offset;
    queue->entries[idx].msg.length = len;
    queue->entries[idx].msg.service_id = service_id;
    queue->entries[idx].msg.flag.deal_state = 0;
    
    queue->wait_h = (queue->wait_h + 1) % queue->buf_size;
    
    __asm__ volatile ("dmb sy" ::: "memory");
    
    printf("[RTISM] Enqueued Flow (Prio %d) -> Offset %d\n", priority, data_offset);
    return 0;
}

// Trigger IRQ separately
void rtism_trigger_irq(int service_id) {
    uint32_t target_zone = 1; 
    uint32_t packed = (target_zone << 16) | (service_id & 0xFFFF);
    __asm__ volatile ("dmb sy" ::: "memory");
    rtism_mmio_ctrl->ipi_trigger = packed;
    printf("[RTISM] Triggered IRQ for Zone %d\n", target_zone);
}

// Legacy API: Enqueue + Trigger (for single message sends)
int rtism_send_flow(int priority, void* data, int len, int service_id) {
    int ret = rtism_enqueue_flow(priority, data, len, service_id);
    if (ret == 0) {
        rtism_trigger_irq(service_id);
    }
    return ret;
}

// ----------------------------------------------------------------------
// 3. CLI Command
// ----------------------------------------------------------------------
int rtism_cmd_main(int argc, char* argv[]) {
    // Usage: hvisor rtism <cmd> [args]
    if (argc < 1) {
        printf("Usage: hvisor rtism [test|lpa]\n");
        return 0;
    }
    
    if (strcmp(argv[0], "lpa") == 0) {
        // Run LPA Demo
        RtismFlow test_flows[] = {
            {1, 1, 1000, 1000, 100}, // High Crit
            {2, 0, 2000, 5000, 200}, // Low Crit
            {3, 1, 500,  500,  50},  // High Crit Tight
        };
        rtism_assign_priorities(test_flows, 3);
        printf("[LPA] Result:\n");
        for(int i=0; i<3; i++) {
            printf("  Flow %d (Crit %d): Priority %d\n", 
                   test_flows[i].id, test_flows[i].criticality, test_flows[i].assigned_priority);
        }
    }
    else if (strcmp(argv[0], "test") == 0) {
        // hvisor rtism test <prio> <msg>
        if (argc < 3) {
            printf("Usage: test <prio> <msg>\n");
            return -1;
        }
        int prio = atoi(argv[1]);
        char* msg = argv[2];
        
        if (rtism_init() != 0) return -1;
        
        printf("[RTISM] Sending '%s' with Priority %d\n", msg, prio);
        rtism_send_flow(prio, msg, strlen(msg)+1, 1); // Service ID 1
    }
    else if (strcmp(argv[0], "auto") == 0) {
        // Automated Closed-Loop Test
        printf("[RTISM-AUTO] Starting Closed-Loop Verification...\n");
        if (rtism_init() != 0) return -1;

        // 1. Define Flows (Table I inspired)
        RtismFlow flows[] = {
            {1, 1, 1000, 800,  100, -1, 0}, // Flow 1: High Crit, Tight Deadline
            {2, 0, 5000, 5000, 200, -1, 0}, // Flow 2: Low Crit
            {3, 1, 2000, 2000, 150, -1, 0}, // Flow 3: High Crit
        };
        int count = 3;

        // 2. Run LPA
        rtism_assign_priorities(flows, count);

        // 3. Dispatch based on LPA results (Enqueue ALL first, then trigger ONE IRQ)
        printf("[RTISM-AUTO] Enqueuing Flows based on LPA (no IRQ yet)...\n");
        for(int i=0; i<count; i++) {
            char msg[64];
            snprintf(msg, 64, "AUTO_FLOW_%d_CRIT_%d", flows[i].id, flows[i].criticality);
            
            if (flows[i].assigned_priority >= 0) {
                rtism_enqueue_flow(flows[i].assigned_priority, msg, 64, 1);
            } else {
                printf("[RTISM-AUTO] Flow %d unschedulable, skipping.\n", flows[i].id);
            }
        }
        
        // 4. Now trigger a SINGLE IRQ to wake up FreeRTOS
        printf("[RTISM-AUTO] All flows enqueued. Triggering single IRQ...\n");
        rtism_trigger_irq(1);
        
        printf("[RTISM-AUTO] Test Complete. Check FreeRTOS logs for Priority Order.\n");
    }
    else if (strcmp(argv[0], "sched_test") == 0) {
        // 32-flow schedulability test (Paper Table III)
        rtism_run_sched_test();
    }
    else if (strcmp(argv[0], "throughput") == 0) {
        // Throughput test: ./hvisor rtism throughput <size_kb> <count>
        int size_kb = (argc >= 2) ? atoi(argv[1]) : 4;  // Default 4KB
        int count = (argc >= 3) ? atoi(argv[2]) : 100;  // Default 100 iterations
        
        if (rtism_init() != 0) return -1;
        
        int size_bytes = size_kb * 1024;
        char* test_data = malloc(size_bytes);
        if (!test_data) {
            printf("[RTISM-Throughput] Failed to allocate %d KB buffer\n", size_kb);
            return -1;
        }
        memset(test_data, 'A', size_bytes);
        
        printf("[RTISM-Throughput] Starting throughput test...\n");
        print_timer_info();  // Show timer information
        printf("  Data Size: %d KB\n", size_kb);
        printf("  Iterations: %d\n", count);
        printf("  Total Data: %d KB\n", size_kb * count);
        
        // Use precision_timer.h for high-precision timing
        uint64_t freq = get_cntfrq();
        uint64_t start = get_cntpct();
        
        for (int i = 0; i < count; i++) {
            rtism_enqueue_flow(0, test_data, size_bytes, 1);
        }
        // Trigger single IRQ after all data enqueued
        rtism_trigger_irq(1);
        
        uint64_t end = get_cntpct();
        
        double elapsed_sec = ticks_to_seconds(start, end, freq);
        uint64_t elapsed_us = ticks_to_us(start, end, freq);
        double total_mb = (double)(size_kb * count) / 1024.0;
        double throughput = total_mb / elapsed_sec;
        
        printf("[RTISM-Throughput] === Results ===\n");
        printf("  Elapsed Time: %.4f seconds (%lu μs)\n", elapsed_sec, elapsed_us);
        printf("  Throughput:   %.2f MB/s\n", throughput);
        
        if (throughput > 1000.0) {
            printf("[RTISM-Throughput] ✓ Excellent! Exceeds 1 GB/s\n");
        } else if (throughput > 100.0) {
            printf("[RTISM-Throughput] ✓ Good performance\n");
        }
        
        free(test_data);
    }
    else if (strcmp(argv[0], "calibrate") == 0) {
        // Calibration command: ./hvisor rtism calibrate
        // Measures τ_write (time per bit for shared memory write)
        printf("\n");
        printf("╔═══════════════════════════════════════════════════════════════════════╗\n");
        printf("║      RTISM Platform Calibration - Measuring τ_write and τ_read        ║\n");
        printf("╚═══════════════════════════════════════════════════════════════════════╝\n\n");
        
        if (rtism_init() != 0) return -1;
        
        print_timer_info();
        
        // Test different data sizes
        int test_sizes[] = {64, 128, 256, 512, 1024, 2048, 4096};
        int num_sizes = sizeof(test_sizes) / sizeof(test_sizes[0]);
        int iterations = 1000;
        
        printf("\n[Calibration] Measuring τ_write (Linux -> Shared Memory)...\n");
        printf("  Iterations per size: %d\n\n", iterations);
        
        printf("┌────────────┬─────────────┬─────────────┬─────────────────┐\n");
        printf("│ Size (B)   │  Time (μs)  │  Bits       │  τ_write (ns/b) │\n");
        printf("├────────────┼─────────────┼─────────────┼─────────────────┤\n");
        
        double total_tau_write = 0;
        int valid_measurements = 0;
        uint64_t freq = get_cntfrq();
        
        for (int s = 0; s < num_sizes; s++) {
            int size = test_sizes[s];
            char* test_data = malloc(size);
            if (!test_data) continue;
            memset(test_data, 'A', size);
            
            uint64_t start = get_cntpct();
            for (int i = 0; i < iterations; i++) {
                rtism_enqueue_flow(0, test_data, size, 1);
            }
            uint64_t end = get_cntpct();
            
            double total_time_us = (double)ticks_to_us(start, end, freq);
            double avg_time_us = total_time_us / iterations;
            int bits = size * 8;
            double tau_write_ns = (avg_time_us * 1000.0) / bits;  // ns per bit
            
            printf("│ %6d     │ %9.2f   │ %7d     │ %13.4f   │\n",
                   size, avg_time_us, bits, tau_write_ns);
            
            total_tau_write += tau_write_ns;
            valid_measurements++;
            
            free(test_data);
        }
        
        printf("└────────────┴─────────────┴─────────────┴─────────────────┘\n");
        
        double avg_tau_write = total_tau_write / valid_measurements;
        
        printf("\n[Calibration] === Results ===\n");
        printf("  Measured τ_write: %.4f ns/bit (%.6f μs/bit)\n", avg_tau_write, avg_tau_write / 1000.0);
        printf("  Paper τ_write:    50.0000 ns/bit (0.050000 μs/bit)\n");
        printf("\n");
        printf("  Ratio (Platform/Paper): %.2fx\n", avg_tau_write / 50.0);
        printf("\n");
        printf("[Calibration] To use measured values, update rtism_lpa.c:\n");
        printf("  #define TAU_WRITE_BIT %.6f  // μs/bit (measured on Phytium Pi)\n", avg_tau_write / 1000.0);
        printf("\n");
        printf("[Note] τ_read should be measured on FreeRTOS side.\n");
        printf("       Typically τ_read ≈ τ_write / 4 (based on paper ratio)\n");
        printf("       Estimated τ_read: %.6f μs/bit\n", avg_tau_write / 4000.0);
    }
    
    return 0;
}
