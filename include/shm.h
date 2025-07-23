// SPDX-License-Identifier: GPL-2.0-only
/**
 * Copyright (c) 2025 Syswonder
 *
 * Syswonder Website:
 *      https://www.syswonder.org
 *
 * Authors:
 *      Guowei Li <2401213322@stu.pku.edu.cn>
 */
#ifndef __SHM_H
#define __SHM_H
#include <linux/ioctl.h>
#include <linux/types.h>

#include "def.h"
#define HVISOR_SHM_USER_INFO _IOR('s', 0, shm_uinfo_t *)
#define HVISOR_SHM_SIGNAL_INFO _IOR('s', 1, shm_signal_info_t *)
#define CONFIG_MAX_SHM_CONFIGS 2
#define HVISOR_HC_SHM_INFO 5

#define SIGSHM 42

struct shm_control_table {
    volatile __u32 shm_id;
    volatile __u32 max_peers;
    volatile __u32 rw_sec_size;
    volatile __u32 out_sec_size;
    volatile __u32 peer_id;
    volatile __u32 ipi_invoke;
} __attribute__((packed));
typedef struct shm_control_table shm_cttable_t;

struct shm_user_info {
    int len;
    int shm_ids[CONFIG_MAX_SHM_CONFIGS];
};
typedef struct shm_user_info shm_uinfo_t;
// SHM signal information structure
struct shm_signal_info {
    __u32 signal_count;
    __u64 last_timestamp; 
    __u32 last_service_id;
    __u32 current_cpu;
};
typedef struct shm_signal_info shm_signal_info_t;
#endif