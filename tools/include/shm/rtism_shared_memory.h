#ifndef _RTISM_SHARED_MEMORY_H_
#define _RTISM_SHARED_MEMORY_H_

#include "msgqueue.h"

// RTISM Configuration
#define RTISM_NUM_PRIORITIES    8
#define RTISM_TOTAL_SIZE        0x10000  // 64KB Total for Queues
#define RTISM_QUEUE_SIZE        (RTISM_TOTAL_SIZE / RTISM_NUM_PRIORITIES) // 8KB per Queue

// Reuse HyperAMP Physical Addresses
// We partition the SHM_PADDR_ROOT_Q (Linux->FreeRTOS) area
#define RTISM_SHM_BASE_PADDR    0xDE400000UL 

// Get Physical Address for a specific priority queue (0=Highest, 7=Lowest)
#define RTISM_GET_QUEUE_PADDR(prio) (RTISM_SHM_BASE_PADDR + ((prio) * RTISM_QUEUE_SIZE))

// Priority Definitions (Compatible with Paper)
#define RTISM_PRIO_HIGH_CRIT_START  0
#define RTISM_PRIO_LOW_CRIT_START   4

// Data Structures
// We reuse 'struct AmpMsgQueue' from msgqueue.h
// Each priority level has its own AmpMsgQueue instance

#endif // _RTISM_SHARED_MEMORY_H_
