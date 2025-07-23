#ifndef __HVISOR_H
#define __HVISOR_H
#include <linux/ioctl.h>
#include <linux/types.h>

#include "def.h"
#include "zone_config.h"

#define MMAP_SIZE 4096
#define MAX_REQ 32
#define MAX_DEVS 4
#define MAX_CPUS 32
#define MAX_ZONES MAX_CPUS

// for HVISOR_ZONE_M_ALLOC and HVISOR_ZONE_M_FREE
struct kmalloc_info {
    __u64 pa;  // Physical address for HVISOR_ZONE_M_ALLOC/FREE
    __u64 size;
};
typedef struct kmalloc_info kmalloc_info_t;


#define SIGHVI 10
// receive request from el2
struct device_req {
    __u64 src_cpu;
    __u64 address; // zone's ipa
    __u64 size;
    __u64 value;
    __u32 src_zone;
    __u8 is_write;
    __u8 need_interrupt;
    __u16 padding;
};

struct device_res {
    __u32 target_zone;
    __u32 irq_id;
};

struct virtio_bridge {
    __u32 req_front;
    __u32 req_rear;
    __u32 res_front;
    __u32 res_rear;
    struct device_req req_list[MAX_REQ];
    struct device_res res_list[MAX_REQ];
    __u64 cfg_flags[MAX_CPUS]; // avoid false sharing, set cfg_flag to u64
    __u64 cfg_values[MAX_CPUS];
    // TODO: When config is okay to use, remove these. It's ok to remove.
    __u64 mmio_addrs[MAX_DEVS];
    __u8 mmio_avail;
    __u8 need_wakeup;
};

struct ioctl_zone_list_args {
    __u64 cnt;
    zone_info_t *zones;
};

typedef struct ioctl_zone_list_args zone_list_args_t;

// ioctl definitions
#define HVISOR_INIT_VIRTIO _IO(1, 0) // virtio device init
#define HVISOR_GET_TASK _IO(1, 1)
#define HVISOR_FINISH_REQ _IO(1, 2) // finish one virtio req
#define HVISOR_ZONE_START _IOW(1, 3, zone_config_t *)
#define HVISOR_ZONE_SHUTDOWN _IOW(1, 4, __u64)
#define HVISOR_ZONE_LIST _IOR(1, 5, zone_list_args_t *)
#define HVISOR_CONFIG_CHECK _IOR(1, 6, __u64 *)
#define HVISOR_CLEAR_INJECT_IRQ _IO(1, 6) 
#ifdef LOONGARCH64
#define HVISOR_CLEAR_INJECT_IRQ _IO(1, 6) // used for ioctl
#define HVISOR_COPY_EFI_SYSTEM_TABLE _IOW(1, 9, efi_boot_info_t *)
#endif /* LOONGARCH64 */

// used for ioctl

#define HVISOR_ZONE_M_ALLOC _IOW(1, 7, kmalloc_info_t *)
#define HVISOR_ZONE_M_FREE _IOW(1, 8, kmalloc_info_t *)
#define HVISOR_SHM_SIGNAL _IOW(1, 10, shm_args_t *)


// Hypercall definitions
#define HVISOR_HC_INIT_VIRTIO 0
#define HVISOR_HC_FINISH_REQ 1
#define HVISOR_HC_START_ZONE 2
#define HVISOR_HC_SHUTDOWN_ZONE 3
#define HVISOR_HC_ZONE_LIST 4
#define HVISOR_HC_CONFIG_CHECK 6

#ifdef LOONGARCH64
#define HVISOR_HC_CLEAR_INJECT_IRQ 5
#define HVISOR_HC_START_EXCEPTION_TRACE 6
#define HVISOR_HC_END_EXCEPTION_TRACE 7
#endif
#ifdef ARM64
#define HVISOR_HC_CLEAR_INJECT_IRQ 5
#define HVISOR_HC_START_EXCEPTION_TRACE 6
#define HVISOR_HC_END_EXCEPTION_TRACE 7
#endif
#define HVISOR_HC_GET_EFI_SYSTEM_TABLE 8
#define HVISOR_HC_SET_EFI_CMDLINE 9
#define HVISOR_SHM_SIGNAL 10


#ifdef LOONGARCH64
static inline __u64 hvisor_call(__u64 code, __u64 arg0, __u64 arg1) {
    register __u64 a0 asm("a0") = code;
    register __u64 a1 asm("a1") = arg0;
    register __u64 a2 asm("a2") = arg1;
    // asm volatile ("hvcl"); // not supported by loongarch gcc now
    // hvcl 0 is 0x002b8000
    __asm__(".word 0x002b8000" : "+r"(a0), "+r"(a1), "+r"(a2));
    return a0;
}
#endif /* LOONGARCH64 */


// my addition for acpi-table
#define ACPI_MOVE_64_TO_64(d, s)        *(u64 *)(void *)(d) = *(u64 *)(void *)(s)
struct paging_info {
    __u64 ipa;
    __u64 hpa;
    __u64 size;
};

#define EFI_TABLE_PAGING_COUNT 20
#define LENGTH_OF_CMDLINE 100
struct efi_boot_info {    
    struct paging_info pages[EFI_TABLE_PAGING_COUNT];
    int count;
    char cmd_line[LENGTH_OF_CMDLINE];
};

typedef struct efi_boot_info efi_boot_info_t;

#define _ALIGN_UP(addr, size)	(((addr)+((size)-1))&(~((typeof(addr))(size)-1)))
#define _ALIGN_DOWN(addr, size)	((addr)&(~((typeof(addr))(size)-1)))


struct shm_args {
    __u64 target_zone_id;
    __u64 service_id;
    __u64 swi;
};
typedef struct shm_args shm_args_t;

#define CHECK_JSON_NULL(json_ptr, json_name)                                   \
    if (json_ptr == NULL) {                                                    \
        fprintf(stderr, "\'%s\' is missing in json file.\n", json_name);       \
        return -1;                                                             \
    }

#define CHECK_JSON_NULL_ERR_OUT(json_ptr, json_name)                           \
    if (json_ptr == NULL) {                                                    \
        fprintf(stderr, "\'%s\' is missing in json file.\n", json_name);       \
        goto err_out;                                                          \
    }

typedef unsigned long long my_u_int64_t; 

// void *my_read_file(char *filename, my_u_int64_t *filesize);
char *open_json_file(const char *json_config_path);

// for shm
struct Payload {
    my_u_int64_t msg_data[0];
    /* 消息本体：负载放置在共享内存中 */
};

// int general_safe_service_request(struct Client *amp_client, struct Msg *msg,
//                                  uint32_t service_id, uint8_t *data,
//                                  uint32_t data_size, char *output_path);
#endif /* __HVISOR_H */
