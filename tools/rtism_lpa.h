#ifndef _RTISM_LPA_H_
#define _RTISM_LPA_H_

#include <stdint.h>

typedef struct {
    int id;
    int criticality; // 1 = High, 0 = Low
    uint32_t period; // us
    uint32_t deadline; // us
    uint32_t size_bytes;
    
    // Output
    int assigned_priority; // 0-7
    double wcrt;
} RtismFlow;

void rtism_assign_priorities(RtismFlow flows[], int count);
void rtism_assign_priorities_tpa(RtismFlow flows[], int count);
void rtism_run_sched_test(void);

#endif
