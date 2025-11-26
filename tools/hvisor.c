#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <assert.h>
#include <stdint.h>
#include <poll.h>
#include <time.h>  
#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE             
#include <sys/time.h>  

#include <stdio.h>
#include "cJSON.h"
#include "event_monitor.h"
#include "hvisor.h"
#include "log.h"
#include "safe_cjson.h"
#include "virtio.h"
#include "zone_config.h"
#include "shm/channel.h"
#include "shm/msgqueue.h"
#include "shm/config/config_msgqueue.h"
#include "shm.h"
#include <sys/poll.h>
#include <sys/ioctl.h>
#include "shm/addr.h"
#include "shm/config/config_addr.h"
#include "shm/time_utils.h"
#include "shm/precision_timer.h"  // 高精度 ARM64 计时器
#include "hyper_amp_qos.h"  // QoS模块

// Global variables for signal handling
static volatile int running = 1;
struct timespec start_time;
struct timespec end_time;
struct timespec service_end_time;
struct timespec service_start_time;

// Signal handler function
static void signal_handler(int signal) {
    printf("\nReceived signal %d, shutting down gracefully...\n", signal);
    running = 0;
}
static void __attribute__((noreturn)) help(int exit_status) {
    printf("Hypervisor Management Tool\n\n");
    printf("Usage:\n");
    printf("  hvisor <command> [options]\n\n");
    printf("Commands:\n");
    printf("  zone start    <config.json>    Initialize an isolation zone\n");
    printf("  zone shutdown -id <zone_id>   Terminate a zone by ID\n");
    printf("  zone list                      List all active zones\n");
    printf("  virtio start  <virtio.json>    Activate virtio devices\n");
    printf("  shm hyper_amp <config> <data> <svc_id>  HyperAMP communication test\n");
    printf("  shm receiver                   Start SHM signal receiver\n");
    printf("  shm server    <config.json>    Start SHM service server\n\n");
    printf("Options:\n");
    printf("  --id <zone_id>    Specify zone ID for shutdown\n");
    printf("  --help            Show this help message\n\n");
    printf("Examples:\n");
    printf("  Start zone:      hvisor zone start /path/to/vm.json\n");
    printf("  Shutdown zone:   hvisor zone shutdown -id 1\n");
    printf("  List zones:      hvisor zone list\n");
    printf("  SHM server:      hvisor shm server /path/to/shm.json\n");
    printf("  HyperAMP client encrypt: ./hvisor shm hyper_amp shm_config.json \"hello\" 1\n");
    printf("  HyperAMP client decrypt: ./hvisor shm hyper_amp shm_config.json \"encrypted\" 2\n");
    printf("  HyperAMP service : ./hvisor shm hyper_amp_service shm_config.json\n");
    printf("HyperAMP client performance testing: ./hvisor shm hyper_amp_test shm_config.json \"hello\" 1\n");
    printf("HyperAMP service performance testing: ./hvisor shm hyper_amp_service_test shm_config.json\n");
    printf("\n");
    printf("QoS-Enabled HyperAMP Commands:\n");
    printf("  ./hvisor shm hyper_amp_qos <config> <data> <svc_id>  - QoS-aware client\n");
    printf("  ./hvisor shm hyper_amp_qos_service <config>          - QoS-aware server\n");
    printf("  ./hvisor shm qos_stats                               - Show QoS statistics\n");
    printf("\n");
    printf("QoS Features:\n");
    printf("  - Automatic service classification (REALTIME/THROUGHPUT/RELIABLE/BEST_EFFORT)\n");
    printf("  - Three-phase batch processing (Collect → Schedule → Cleanup)\n");
    printf("  - WRR priority scheduling with latency tracking\n");
    printf("  - QoS violation detection and reporting\n");
    exit(exit_status);
}

void *read_file(char *filename, u_int64_t *filesize) {
    int fd;
    struct stat st;
    void *buf;
    ssize_t len;

    fd = open(filename, O_RDONLY);
    if (fd < 0) {
        log_error("read_file: open file %s failed", filename);
        exit(1);
    }

    if (fstat(fd, &st) < 0) {
        log_error("read_file: fstat %s failed", filename);
        exit(1);
    }

    long page_size = sysconf(_SC_PAGESIZE);

    // Calculate buffer size, ensuring alignment to page boundary
    ssize_t buf_size = (st.st_size + page_size - 1) & ~(page_size - 1);

    buf = malloc(buf_size);
    memset(buf, 0, buf_size);

    len = read(fd, buf, st.st_size);
    if (len < 0) {
        perror("read_file: read failed");
        exit(1);
    }

    if (filesize)
        *filesize = len;

    close(fd);

    return buf;
}
// 从文件读取数据的辅助函数（用于test_shm）
char* read_data_from_file(const char* filename, size_t* data_size) {
    FILE* file = fopen(filename, "rb");
    if (!file) {
        printf("Error: Cannot open file '%s'\n", filename);
        return NULL;
    }
    
    // 获取文件大小
    fseek(file, 0, SEEK_END);
    long file_size = ftell(file);
    fseek(file, 0, SEEK_SET);
    
    if (file_size <= 0) {
        printf("Error: File '%s' is empty or cannot determine size\n", filename);
        fclose(file);
        return NULL;
    }
    
    if (file_size > 1024) {
        printf("Error: File '%s' too large (max 1024 bytes), got %ld bytes\n", filename, file_size);
        fclose(file);
        return NULL;
    }
    
    // 分配内存并读取文件内容
    char* data = malloc(file_size + 1);
    if (!data) {
        printf("Error: Memory allocation failed for file '%s'\n", filename);
        fclose(file);
        return NULL;
    }
    
    size_t bytes_read = fread(data, 1, file_size, file);
    if (bytes_read != file_size) {
        printf("Error: Failed to read complete file '%s'\n", filename);
        free(data);
        fclose(file);
        return NULL;
    }
    
    data[file_size] = '\0';  // 确保字符串以null结尾
    *data_size = file_size;
    fclose(file);
    return data;
}
int open_dev() {
    int fd = open("/dev/hvisor", O_RDWR);
    if (fd < 0) {
        log_error("Failed to open /dev/hvisor!");
        exit(1);
    }
    return fd;
}

// static void get_info(char *optarg, char **path, u64 *address) {
// 	char *now;
// 	*path = strtok(optarg, ",");
// 	now = strtok(NULL, "=");
// 	if (strcmp(now, "addr") == 0) {
// 		now = strtok(NULL, "=");
// 		*address = strtoull(now, NULL, 16);
// 	} else {
// 		help(1);
// 	}
// }

static __u64 load_image_to_memory(const char *path, __u64 load_paddr) {
    __u64 size, page_size,
        map_size; // Define variables: image size, page size, and map size
    int fd;       // File descriptor
    void *image_content,
        *virt_addr; // Pointers to image content and virtual address

    fd = open_dev();
    // Load image content into memory
    image_content = read_file(path, &size);

    page_size = sysconf(_SC_PAGESIZE);
    map_size = (size + page_size - 1) & ~(page_size - 1);

    // Map the physical memory to virtual memory
#ifdef LOONGARCH64
    virt_addr = (__u64)mmap(NULL, map_size, PROT_READ | PROT_WRITE | PROT_EXEC,
                            MAP_SHARED, fd, load_paddr);
#else
    virt_addr = mmap(NULL, map_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
                     load_paddr);
#endif

    if (virt_addr == MAP_FAILED) {
        perror("Error mapping memory");
        exit(1);
    }

    memmove(virt_addr, image_content, map_size);

    free(image_content);
    munmap(virt_addr, map_size);

    close(fd);
    return map_size;
}

#define CHECK_JSON_NULL(json_ptr, json_name)                                   \
    if (json_ptr == NULL) {                                                    \
        log_error("\'%s\' is missing in json file.", json_name);               \
        return -1;                                                             \
    }

#define CHECK_JSON_NULL_ERR_OUT(json_ptr, json_name)                           \
    if (json_ptr == NULL) {                                                    \
        log_error("\'%s\' is missing in json file.", json_name);               \
        goto err_out;                                                          \
    }

static int parse_arch_config(cJSON *root, zone_config_t *config) {
    cJSON *arch_config_json = SAFE_CJSON_GET_OBJECT_ITEM(root, "arch_config");
    CHECK_JSON_NULL(arch_config_json, "arch_config");
#ifdef ARM64
    cJSON *gic_version_json =
        SAFE_CJSON_GET_OBJECT_ITEM(arch_config_json, "gic_version");
    cJSON *gicd_base_json =
        SAFE_CJSON_GET_OBJECT_ITEM(arch_config_json, "gicd_base");
    cJSON *gicr_base_json =
        SAFE_CJSON_GET_OBJECT_ITEM(arch_config_json, "gicr_base");
    cJSON *gits_base_json =
        SAFE_CJSON_GET_OBJECT_ITEM(arch_config_json, "gits_base");
    cJSON *gicc_base_json =
        SAFE_CJSON_GET_OBJECT_ITEM(arch_config_json, "gicc_base");
    cJSON *gich_base_json =
        SAFE_CJSON_GET_OBJECT_ITEM(arch_config_json, "gich_base");
    cJSON *gicv_base_json =
        SAFE_CJSON_GET_OBJECT_ITEM(arch_config_json, "gicv_base");
    cJSON *gicc_offset_json =
        SAFE_CJSON_GET_OBJECT_ITEM(arch_config_json, "gicc_offset");
    cJSON *gicv_size_json =
        SAFE_CJSON_GET_OBJECT_ITEM(arch_config_json, "gicv_size");
    cJSON *gich_size_json =
        SAFE_CJSON_GET_OBJECT_ITEM(arch_config_json, "gich_size");
    cJSON *gicc_size_json =
        SAFE_CJSON_GET_OBJECT_ITEM(arch_config_json, "gicc_size");
    cJSON *gicd_size_json =
        SAFE_CJSON_GET_OBJECT_ITEM(arch_config_json, "gicd_size");
    cJSON *gicr_size_json =
        SAFE_CJSON_GET_OBJECT_ITEM(arch_config_json, "gicr_size");
    cJSON *gits_size_json =
        SAFE_CJSON_GET_OBJECT_ITEM(arch_config_json, "gits_size");
    CHECK_JSON_NULL(gic_version_json, "gic_version");
    CHECK_JSON_NULL(gicd_base_json, "gicd_base")
    CHECK_JSON_NULL(gicr_base_json, "gicr_base")
    CHECK_JSON_NULL(gicd_size_json, "gicd_size")
    CHECK_JSON_NULL(gicr_size_json, "gicr_size")

    char *gic_version = gic_version_json->valuestring;
    if (!strcmp(gic_version, "v2")) {
        CHECK_JSON_NULL(gicc_base_json, "gicc_base")
        CHECK_JSON_NULL(gich_base_json, "gich_base")
        CHECK_JSON_NULL(gicv_base_json, "gicv_base")
        CHECK_JSON_NULL(gicc_offset_json, "gicc_offset")
        CHECK_JSON_NULL(gicv_size_json, "gicv_size")
        CHECK_JSON_NULL(gich_size_json, "gich_size")
        CHECK_JSON_NULL(gicc_size_json, "gicc_size")
        config->arch_config.gicc_base =
            strtoull(gicc_base_json->valuestring, NULL, 16);
        config->arch_config.gich_base =
            strtoull(gich_base_json->valuestring, NULL, 16);
        config->arch_config.gicv_base =
            strtoull(gicv_base_json->valuestring, NULL, 16);
        config->arch_config.gicc_offset =
            strtoull(gicc_offset_json->valuestring, NULL, 16);
        config->arch_config.gicv_size =
            strtoull(gicv_size_json->valuestring, NULL, 16);
        config->arch_config.gich_size =
            strtoull(gich_size_json->valuestring, NULL, 16);
        config->arch_config.gicc_size =
            strtoull(gicc_size_json->valuestring, NULL, 16);
    } else if (strcmp(gic_version, "v3") != 0) {
        log_error("Invalid GIC version. It should be either of v2 or v3\n");
        return -1;
    }
    if (gits_base_json == NULL || gits_size_json == NULL) {
        log_warn("No gits fields in arch_config.\n");
    } else {
        config->arch_config.gits_base =
            strtoull(gits_base_json->valuestring, NULL, 16);
        config->arch_config.gits_size =
            strtoull(gits_size_json->valuestring, NULL, 16);
    }

    config->arch_config.gicd_base =
        strtoull(gicd_base_json->valuestring, NULL, 16);
    config->arch_config.gicr_base =
        strtoull(gicr_base_json->valuestring, NULL, 16);
    config->arch_config.gicd_size =
        strtoull(gicd_size_json->valuestring, NULL, 16);
    config->arch_config.gicr_size =
        strtoull(gicr_size_json->valuestring, NULL, 16);
#endif

#ifdef RISCV64
    cJSON *plic_base_json =
        SAFE_CJSON_GET_OBJECT_ITEM(arch_config_json, "plic_base");
    cJSON *plic_size_json =
        SAFE_CJSON_GET_OBJECT_ITEM(arch_config_json, "plic_size");
    cJSON *aplic_base_json =
        SAFE_CJSON_GET_OBJECT_ITEM(arch_config_json, "aplic_base");
    cJSON *aplic_size_json =
        SAFE_CJSON_GET_OBJECT_ITEM(arch_config_json, "aplic_size");

    if (plic_base_json == NULL || plic_size_json == NULL) {
        log_warn("Missing fields in arch_config.");
        return -1;
    }
    if (aplic_base_json == NULL || aplic_size_json == NULL) {
        log_warn("Missing fields in arch_config.");
        return -1;
    }

    config->arch_config.plic_base =
        strtoull(plic_base_json->valuestring, NULL, 16);
    config->arch_config.plic_size =
        strtoull(plic_size_json->valuestring, NULL, 16);
    config->arch_config.aplic_base =
        strtoull(aplic_base_json->valuestring, NULL, 16);
    config->arch_config.aplic_size =
        strtoull(aplic_size_json->valuestring, NULL, 16);
#endif

    return 0;
}

static int parse_pci_config(cJSON *root, zone_config_t *config) {
    cJSON *pci_config_json = SAFE_CJSON_GET_OBJECT_ITEM(root, "pci_config");
    if (pci_config_json == NULL) {
        log_warn("No pci_config field found.");
        return -1;
    }

#ifdef ARM64
    cJSON *ecam_base_json =
        SAFE_CJSON_GET_OBJECT_ITEM(pci_config_json, "ecam_base");
    cJSON *io_base_json =
        SAFE_CJSON_GET_OBJECT_ITEM(pci_config_json, "io_base");
    cJSON *pci_io_base_json =
        SAFE_CJSON_GET_OBJECT_ITEM(pci_config_json, "pci_io_base");
    cJSON *mem32_base_json =
        SAFE_CJSON_GET_OBJECT_ITEM(pci_config_json, "mem32_base");
    cJSON *pci_mem32_base_json =
        SAFE_CJSON_GET_OBJECT_ITEM(pci_config_json, "pci_mem32_base");
    cJSON *mem64_base_json =
        SAFE_CJSON_GET_OBJECT_ITEM(pci_config_json, "mem64_base");
    cJSON *pci_mem64_base_json =
        SAFE_CJSON_GET_OBJECT_ITEM(pci_config_json, "pci_mem64_base");
    cJSON *ecam_size_json =
        SAFE_CJSON_GET_OBJECT_ITEM(pci_config_json, "ecam_size");
    cJSON *io_size_json =
        SAFE_CJSON_GET_OBJECT_ITEM(pci_config_json, "io_size");
    cJSON *mem32_size_json =
        SAFE_CJSON_GET_OBJECT_ITEM(pci_config_json, "mem32_size");
    cJSON *mem64_size_json =
        SAFE_CJSON_GET_OBJECT_ITEM(pci_config_json, "mem64_size");

    CHECK_JSON_NULL(ecam_base_json, "ecam_base")
    CHECK_JSON_NULL(io_base_json, "io_base")
    CHECK_JSON_NULL(mem32_base_json, "mem32_base")
    CHECK_JSON_NULL(mem64_base_json, "mem64_base")
    CHECK_JSON_NULL(ecam_size_json, "ecam_size")
    CHECK_JSON_NULL(io_size_json, "io_size")
    CHECK_JSON_NULL(mem32_size_json, "mem32_size")
    CHECK_JSON_NULL(mem64_size_json, "mem64_size")
    CHECK_JSON_NULL(pci_io_base_json, "pci_io_base")
    CHECK_JSON_NULL(pci_mem32_base_json, "pci_mem32_base")
    CHECK_JSON_NULL(pci_mem64_base_json, "pci_mem64_base")

    config->pci_config.ecam_base =
        strtoull(ecam_base_json->valuestring, NULL, 16);
    config->pci_config.io_base = strtoull(io_base_json->valuestring, NULL, 16);
    config->pci_config.mem32_base =
        strtoull(mem32_base_json->valuestring, NULL, 16);
    config->pci_config.mem64_base =
        strtoull(mem64_base_json->valuestring, NULL, 16);
    config->pci_config.pci_io_base =
        strtoull(pci_io_base_json->valuestring, NULL, 16);
    config->pci_config.pci_mem32_base =
        strtoull(pci_mem32_base_json->valuestring, NULL, 16);
    config->pci_config.pci_mem64_base =
        strtoull(pci_mem64_base_json->valuestring, NULL, 16);
    config->pci_config.ecam_size =
        strtoull(ecam_size_json->valuestring, NULL, 16);
    config->pci_config.io_size = strtoull(io_size_json->valuestring, NULL, 16);
    config->pci_config.mem32_size =
        strtoull(mem32_size_json->valuestring, NULL, 16);
    config->pci_config.mem64_size =
        strtoull(mem64_size_json->valuestring, NULL, 16);
    cJSON *alloc_pci_devs_json =
        SAFE_CJSON_GET_OBJECT_ITEM(root, "alloc_pci_devs");
    int num_pci_devs = SAFE_CJSON_GET_ARRAY_SIZE(alloc_pci_devs_json);
    config->num_pci_devs = num_pci_devs;
    for (int i = 0; i < num_pci_devs; i++) {
        config->alloc_pci_devs[i] =
            SAFE_CJSON_GET_ARRAY_ITEM(alloc_pci_devs_json, i)->valueint;
    }
#endif

    return 0;
}

static int zone_start_from_json(const char *json_config_path,
                                zone_config_t *config) {
    cJSON *root = NULL;

    FILE *file = fopen(json_config_path, "r");
    if (file == NULL) {
        log_error("Error opening json file: %s", json_config_path);
        exit(1);
    }
    fseek(file, 0, SEEK_END);
    long file_size = ftell(file);
    fseek(file, 0, SEEK_SET);
    char *buffer = malloc(file_size + 1);
    if (fread(buffer, 1, file_size, file) == 0) {
        log_error("Error reading json file: %s", json_config_path);
        goto err_out;
    }
    fclose(file);
    buffer[file_size] = '\0';

    // parse JSON
    root = SAFE_CJSON_PARSE(buffer);
    cJSON *zone_id_json = SAFE_CJSON_GET_OBJECT_ITEM(root, "zone_id");
    cJSON *cpus_json = SAFE_CJSON_GET_OBJECT_ITEM(root, "cpus");
    cJSON *name_json = SAFE_CJSON_GET_OBJECT_ITEM(root, "name");
    cJSON *memory_regions_json =
        SAFE_CJSON_GET_OBJECT_ITEM(root, "memory_regions");
    cJSON *kernel_filepath_json =
        SAFE_CJSON_GET_OBJECT_ITEM(root, "kernel_filepath");
    cJSON *dtb_filepath_json = SAFE_CJSON_GET_OBJECT_ITEM(root, "dtb_filepath");
    cJSON *kernel_load_paddr_json =
        SAFE_CJSON_GET_OBJECT_ITEM(root, "kernel_load_paddr");
    cJSON *dtb_load_paddr_json =
        SAFE_CJSON_GET_OBJECT_ITEM(root, "dtb_load_paddr");
    cJSON *entry_point_json = SAFE_CJSON_GET_OBJECT_ITEM(root, "entry_point");
    cJSON *interrupts_json = SAFE_CJSON_GET_OBJECT_ITEM(root, "interrupts");
    cJSON *ivc_configs_json = SAFE_CJSON_GET_OBJECT_ITEM(root, "ivc_configs");

    CHECK_JSON_NULL_ERR_OUT(zone_id_json, "zone_id")
    CHECK_JSON_NULL_ERR_OUT(cpus_json, "cpus")
    CHECK_JSON_NULL_ERR_OUT(name_json, "name")
    CHECK_JSON_NULL_ERR_OUT(memory_regions_json, "memory_regions")
    CHECK_JSON_NULL_ERR_OUT(kernel_filepath_json, "kernel_filepath")
    CHECK_JSON_NULL_ERR_OUT(dtb_filepath_json, "dtb_filepath")
    CHECK_JSON_NULL_ERR_OUT(kernel_load_paddr_json, "kernel_load_paddr")
    CHECK_JSON_NULL_ERR_OUT(dtb_load_paddr_json, "dtb_load_paddr")
    CHECK_JSON_NULL_ERR_OUT(entry_point_json, "entry_point")
    CHECK_JSON_NULL_ERR_OUT(interrupts_json, "interrupts")
    CHECK_JSON_NULL_ERR_OUT(ivc_configs_json, "ivc_configs")

    config->zone_id = zone_id_json->valueint;

    int num_cpus = SAFE_CJSON_GET_ARRAY_SIZE(cpus_json);

    for (int i = 0; i < num_cpus; i++) {
        config->cpus |=
            (1 << SAFE_CJSON_GET_ARRAY_ITEM(cpus_json, i)->valueint);
    }

    int num_memory_regions = SAFE_CJSON_GET_ARRAY_SIZE(memory_regions_json);
    int num_interrupts = SAFE_CJSON_GET_ARRAY_SIZE(interrupts_json);

    if (num_memory_regions > CONFIG_MAX_MEMORY_REGIONS ||
        num_interrupts > CONFIG_MAX_INTERRUPTS) {
        log_error("Exceeded maximum allowed regions/interrupts.");
        goto err_out;
    }

    // Iterate through each memory region of the zone
    // Including memory and MMIO regions of the zone
    config->num_memory_regions = num_memory_regions;
    for (int i = 0; i < num_memory_regions; i++) {
        cJSON *region = SAFE_CJSON_GET_ARRAY_ITEM(memory_regions_json, i);
        memory_region_t *mem_region = &config->memory_regions[i];

        const char *type_str =
            SAFE_CJSON_GET_OBJECT_ITEM(region, "type")->valuestring;
        if (strcmp(type_str, "ram") == 0) {
            mem_region->type = MEM_TYPE_RAM;
        } else if (strcmp(type_str, "io") == 0) {
            // io device
            mem_region->type = MEM_TYPE_IO;
        } else if (strcmp(type_str, "virtio") == 0) {
            // virtio device
            mem_region->type = MEM_TYPE_VIRTIO;
        } else {
            log_error("Unknown memory region type: %s", type_str);
            goto err_out;
        }

        mem_region->physical_start = strtoull(
            SAFE_CJSON_GET_OBJECT_ITEM(region, "physical_start")->valuestring,
            NULL, 16);
        mem_region->virtual_start = strtoull(
            SAFE_CJSON_GET_OBJECT_ITEM(region, "virtual_start")->valuestring,
            NULL, 16);
        mem_region->size = strtoull(
            SAFE_CJSON_GET_OBJECT_ITEM(region, "size")->valuestring, NULL, 16);

        log_debug("memory_region %d: type %d, physical_start %llx, "
                  "virtual_start %llx, size %llx",
                  i, mem_region->type, mem_region->physical_start,
                  mem_region->virtual_start, mem_region->size);
    }

    config->num_interrupts = num_interrupts;
    for (int i = 0; i < num_interrupts; i++) {
        config->interrupts[i] =
            SAFE_CJSON_GET_ARRAY_ITEM(interrupts_json, i)->valueint;
    }

    // ivc
    int num_ivc_configs = SAFE_CJSON_GET_ARRAY_SIZE(ivc_configs_json);
    if (num_ivc_configs > CONFIG_MAX_IVC_CONFIGS) {
        log_error("Exceeded maximum allowed ivc configs.");
        goto err_out;
    }

    config->num_ivc_configs = num_ivc_configs;
    for (int i = 0; i < num_ivc_configs; i++) {
        cJSON *ivc_config_json = SAFE_CJSON_GET_ARRAY_ITEM(ivc_configs_json, i);
        ivc_config_t *ivc_config = &config->ivc_configs[i];
        ivc_config->ivc_id =
            SAFE_CJSON_GET_OBJECT_ITEM(ivc_config_json, "ivc_id")->valueint;
        ivc_config->peer_id =
            SAFE_CJSON_GET_OBJECT_ITEM(ivc_config_json, "peer_id")->valueint;
        ivc_config->shared_mem_ipa = strtoull(
            SAFE_CJSON_GET_OBJECT_ITEM(ivc_config_json, "shared_mem_ipa")
                ->valuestring,
            NULL, 16);
        ivc_config->control_table_ipa = strtoull(
            SAFE_CJSON_GET_OBJECT_ITEM(ivc_config_json, "control_table_ipa")
                ->valuestring,
            NULL, 16);
        ivc_config->rw_sec_size =
            strtoull(SAFE_CJSON_GET_OBJECT_ITEM(ivc_config_json, "rw_sec_size")
                         ->valuestring,
                     NULL, 16);
        ivc_config->out_sec_size =
            strtoull(SAFE_CJSON_GET_OBJECT_ITEM(ivc_config_json, "out_sec_size")
                         ->valuestring,
                     NULL, 16);
        ivc_config->interrupt_num =
            SAFE_CJSON_GET_OBJECT_ITEM(ivc_config_json, "interrupt_num")
                ->valueint;
        ivc_config->max_peers =
            SAFE_CJSON_GET_OBJECT_ITEM(ivc_config_json, "max_peers")->valueint;
        log_info("ivc_config %d: ivc_id %d, peer_id %d, shared_mem_ipa %llx, "
                 "interrupt_num %d, max_peers %d\n",
                 i, ivc_config->ivc_id, ivc_config->peer_id,
                 ivc_config->shared_mem_ipa, ivc_config->interrupt_num,
                 ivc_config->max_peers);
    }
    config->entry_point = strtoull(entry_point_json->valuestring, NULL, 16);

    config->kernel_load_paddr =
        strtoull(kernel_load_paddr_json->valuestring, NULL, 16);

    config->dtb_load_paddr =
        strtoull(dtb_load_paddr_json->valuestring, NULL, 16);

    // Load kernel image to memory
    config->kernel_size = load_image_to_memory(
        kernel_filepath_json->valuestring,
        strtoull(kernel_load_paddr_json->valuestring, NULL, 16));

    // Load dtb to memory
    config->dtb_size = load_image_to_memory(
        dtb_filepath_json->valuestring,
        strtoull(dtb_load_paddr_json->valuestring, NULL, 16));

    log_info("Kernel size: %llu, DTB size: %llu", config->kernel_size,
             config->dtb_size);

    // check name length
    if (strlen(name_json->valuestring) > CONFIG_NAME_MAXLEN) {
        log_error("Zone name too long: %s", name_json->valuestring);
        goto err_out;
    }
    strncpy(config->name, name_json->valuestring, CONFIG_NAME_MAXLEN);

    log_info("Zone name: %s", config->name);

#ifndef LOONGARCH64

    // Parse architecture-specific configurations (interrupts for each platform)
    if (parse_arch_config(root, config))
        goto err_out;

    parse_pci_config(root, config);

#endif

    if (root)
        cJSON_Delete(root);
    if (buffer)
        free(buffer);

    int fd = open_dev();
    if (fd < 0) {
        perror("zone_start: open hvisor failed");
        goto err_out;
    }

    log_info("Calling ioctl to start zone: [%s]", config->name);

    int err = ioctl(fd, HVISOR_ZONE_START, config);

    if (err)
        perror("zone_start: ioctl failed");

    close(fd);

    return 0;
err_out:
    if (root)
        cJSON_Delete(root);
    if (buffer)
        free(buffer);
    return -1;
}

// ./hvisor zone start <path_to_config_file>
static int zone_start(int argc, char *argv[]) {
    char *json_config_path = NULL;
    zone_config_t config;
    int fd, ret;
    u_int64_t hvisor_config_version;

    if (argc != 4) {
        help(1);
    }
    json_config_path = argv[3];

    memset(&config, 0, sizeof(zone_config_t));

    fd = open_dev();
    ret = ioctl(fd, HVISOR_CONFIG_CHECK, &hvisor_config_version);
    close(fd);

    if (ret) {
        log_error("ioctl: hvisor config check failed, ret %d", ret);
        return -1;
    }

    if (hvisor_config_version != CONFIG_MAGIC_VERSION) {
        log_error("zone start failed because config versions mismatch, "
                  "hvisor-tool is 0x%x, hvisor is 0x%x",
                  CONFIG_MAGIC_VERSION, hvisor_config_version);
        return -1;
    } else {
        log_info("zone config check pass");
    }

    return zone_start_from_json(json_config_path, &config);
}

// ./hvisor zone shutdown -id 1
static int zone_shutdown(int argc, char *argv[]) {
    if (argc != 2 || strcmp(argv[0], "-id") != 0) {
        help(1);
    }
    __u64 zone_id;
    sscanf(argv[1], "%llu", &zone_id);
    int fd = open_dev();
    int err = ioctl(fd, HVISOR_ZONE_SHUTDOWN, zone_id);
    if (err)
        perror("zone_shutdown: ioctl failed");
    close(fd);
    return err;
}

static void print_cpu_list(__u64 cpu_mask, char *outbuf, size_t bufsize) {
    int found_cpu = 0;
    char *buf = outbuf;

    for (int i = 0; i < MAX_CPUS && buf - outbuf < bufsize; i++) {
        if ((cpu_mask & (1ULL << i)) != 0) {
            if (found_cpu) {
                *buf++ = ',';
                *buf++ = ' ';
            }
            snprintf(buf, bufsize - (buf - outbuf), "%d", i);
            buf += strlen(buf);
            found_cpu = 1;
        }
    }
    if (!found_cpu) {
        memcpy(outbuf, "none", 5);
    }
}

// ./hvisor zone list
static int zone_list(int argc, char *argv[]) {
    if (argc != 0) {
        help(1);
    }
    __u64 cnt = CONFIG_MAX_ZONES;
    zone_info_t *zones = malloc(sizeof(zone_info_t) * cnt);
    zone_list_args_t args = {cnt, zones};
    // printf("zone_list: cnt %llu, zones %p\n", cnt, zones);
    int fd = open_dev();
    int ret = ioctl(fd, HVISOR_ZONE_LIST, &args);
    if (ret < 0)
        perror("zone_list: ioctl failed");

    printf("| %11s     | %10s        | %9s       | %10s |\n", "zone_id", "cpus",
           "name", "status");

    for (int i = 0; i < ret; i++) {
        char cpu_list_str[256]; // Assuming this buffer size is enough
        memset(cpu_list_str, 0, sizeof(cpu_list_str));
        print_cpu_list(zones[i].cpus, cpu_list_str, sizeof(cpu_list_str));
        printf("| %15u | %17s | %15s | %10s |\n", zones[i].zone_id,
               cpu_list_str, zones[i].name,
               zones[i].is_err ? "error" : "running");
    }
    free(zones);
    close(fd);
    return ret;
}
char *open_json_file(const char *json_config_path) {
    FILE *file = fopen(json_config_path, "r");
    if (file == NULL) {
        printf("Error opening json file: %s\n", json_config_path);
        fprintf(stderr, "Error opening json file: %s\n", json_config_path);
        exit(1);
    }
    fseek(file, 0, SEEK_END);
    long file_size = ftell(file);
    fseek(file, 0, SEEK_SET);
    char *buffer = malloc(file_size + 1);
    if (fread(buffer, 1, file_size, file) == 0) {
        printf("Error reading json file: %s\n", json_config_path);
        fprintf(stderr, "Error reading json file: %s\n", json_config_path);
        goto err_out;
    }
    fclose(file);
    buffer[file_size] = '\0';

    // printf("Parsing json file: %s\n", json_config_path);

    return buffer;
err_out:
    free(buffer);
    return NULL;
}

#ifdef LOONGARCH64
static int start_exception_trace()
{
    int fd = open_dev();
    int ret = ioctl(fd, HVISOR_HC_START_EXCEPTION_TRACE, NULL);
    if (ret < 0)
        perror("start_exception_trace: ioctl failed");
    return ret;
}

static int end_exception_trace()
{
    int fd = open_dev();
    int ret = ioctl(fd, HVISOR_HC_END_EXCEPTION_TRACE, NULL);
    if (ret < 0)
        perror("end_exception_trace: ioctl failed");
    return ret;
}
#endif

#ifdef LOONGARCH64
// shared memory
static int test_shm_signal(int argc, char *argv[]) {
    // dst zone_id
    if (argc != 2 || strcmp(argv[0], "-id") != 0) {
        help(1);
    }
    __u64 target_zone_id;
    sscanf(argv[1], "%llu", &target_zone_id);

    shm_args_t args;    
    args.target_zone_id = target_zone_id;
    
    int fd = open_dev();
    int ret = ioctl(fd, HVISOR_SHM_SIGNAL, &args);
    if (ret < 0) {
        perror("test_shm_signal: ioctl failed");
    }
}

#endif
#ifdef ARM64
static int start_exception_trace()
{
    int fd = open_dev();
    int ret = ioctl(fd, HVISOR_HC_START_EXCEPTION_TRACE, NULL);
    if (ret < 0)
        perror("start_exception_trace: ioctl failed");
    return ret;
}

static int end_exception_trace()
{
    int fd = open_dev();
    int ret = ioctl(fd, HVISOR_HC_END_EXCEPTION_TRACE, NULL);
    if (ret < 0)
        perror("end_exception_trace: ioctl failed");
    return ret;
}
#endif

#ifdef ARM64
// shared memory
static int test_shm_signal(int argc, char *argv[]) {
    // dst zone_id
    if (argc != 2 || strcmp(argv[0], "-id") != 0) {
        help(1);
    }
    __u64 target_zone_id;
    sscanf(argv[1], "%llu", &target_zone_id);

    shm_args_t args;    
    args.target_zone_id = target_zone_id;
    
    int fd = open_dev();
    int ret = ioctl(fd, HVISOR_SHM_SIGNAL, &args);
    if (ret < 0) {
        perror("test_shm_signal: ioctl failed");
    }
}

#endif

#include "shm/addr.h"
#include "shm/msg.h"
#include "shm/shm.h"
#include "shm/client.h"
#include "shm/config/config_zone.h"
#include "service/safe_service.h"

// ===== HyperAMP 安全服务加解密函数 =====

/**
 * AES-like 加密函数 (简化版) - Service ID 1
 * 使用简单的字节替换和位移操作模拟AES加密
 */
// HyperAMP加密服务实现（简单XOR加密）- 导出给QoS模块使用
int hyperamp_encrypt_service(char* data, int data_len, int max_len) {
    if (data == NULL || data_len <= 0 || max_len <= 0) {
        return -1;
    }
    
    printf("[HyperAMP] Executing encryption service (Service ID: 1)\n");
    printf("[HyperAMP] Input data length: %d bytes\n", data_len);
    
    // 显示原始数据
    printf("[HyperAMP] Original data: ");
    for (int i = 0; i < data_len; i++) {
        printf("0x%02x ", (unsigned char)data[i]);
    }
    printf("\n");    
    // 简化的加密密钥 (16字节)
    const unsigned char key[16] = {
        0x2b, 0x7e, 0x15, 0x16, 0x28, 0xae, 0xd2, 0xa6,
        0xab, 0xf7, 0x15, 0x88, 0x09, 0xcf, 0x4f, 0x3c
    };
    
    // 加密过程：多轮字节替换和XOR操作
        for (int i = 0; i < data_len; i++) {
            unsigned char byte = (unsigned char)data[i];
            
            // 简单XOR加密
            byte ^= key[i % 16];
            
            // // Round 2: 字节替换 (S-Box模拟)
            // byte = ((byte << 1) | (byte >> 7)) & 0xFF;  // 循环左移1位
            byte ^= 0x55;// 额外的固定XOR
            // // Round 3: 再次XOR
            // byte ^= key[(i + round) % 16];
            
            data[i] = (char)byte;
        }
    // 显示加密后数据
    printf("[HyperAMP] Encrypted data: ");
    for (int i = 0; i < data_len; i++) {
        printf("0x%02x ", (unsigned char)data[i]);
    }
    printf("\n");

    printf("[HyperAMP] Encryption completed successfully\n");
    return 0;
}

/**
 * AES-like 解密函数 (简化版) - Service ID 2  
 * 对应加密函数的逆向操作
 */
// HyperAMP解密服务实现（简单XOR解密）- 导出给QoS模块使用
int hyperamp_decrypt_service(char* data, int data_len, int max_len) {
    if (data == NULL || data_len <= 0 || max_len <= 0) {
        return -1;
    }
    
    printf("[HyperAMP] Executing decryption service (Service ID: 2)\n");
    printf("[HyperAMP] Input data length: %d bytes\n", data_len);
    
    // 显示加密数据
    printf("[HyperAMP] Encrypted data: ");
    for (int i = 0; i < data_len; i++) {
        printf("0x%02x ", (unsigned char)data[i]);
    }
    printf("\n");
    
    // 相同的解密密钥
    const unsigned char key[16] = {
        0x2b, 0x7e, 0x15, 0x16, 0x28, 0xae, 0xd2, 0xa6,
        0xab, 0xf7, 0x15, 0x88, 0x09, 0xcf, 0x4f, 0x3c
    };
    
    // 解密过程：完全逆向加密操作
    for (int i = 0; i < data_len; i++) {
        unsigned char byte = (unsigned char)data[i];
        
        // 逆向操作（与加密顺序相反）
        byte ^= 0x55;                // 逆向固定XOR
        byte ^= key[i % 16];         // 逆向密钥XOR
        
        data[i] = (char)byte;
    }
    
    // 显示解密后数据
    printf("[HyperAMP] Decrypted data: ");
    for (int i = 0; i < data_len; i++) {
        printf("0x%02x ", (unsigned char)data[i]);
    }
    printf("\n");
    
    printf("[HyperAMP] Decryption completed successfully\n");
    return 0;
}
static Service services[] = {
    {0, "null", 0},
    {1, "echo string", 0},
    {2, "flip the digit", 1},
    {3, "Caesar encrypt", 1},
    {4, "Caesar decrypt", 1},
    {5, "XOR encrypt", 1},
    {6, "XOR decrypt", 1},
    {7, "ROT13 encrypt", 1},    
    {8, "ROT13 decrypt", 1},
    {9, "DJB2 hash", 1},
    {10, "CRC32 hash", 1},
    {11, "FNV hash", 1},
    {12, "XORSHIFT random number generator", 1},
    {13, "LCG random number generator", 1},
    {14, "random string generator", 1},
    {15, "random character generator", 1}
};

static void print_services() {
    printf(" %-15s | %-40s | %-20s\n", "Safe Service ID", "Description", "Requires Return Value");
    printf("---------------------------------------------------------------\n");
    int size = sizeof(services) / sizeof(services[0]);
    for (int i = 0; i < size; i++) {
        printf(" %-15u | %-40s | %-20s\n", 
               services[i].id, 
               services[i].description, 
               services[i].need_fetch_data ? "Yes" : "No");
    }
}

int general_safe_service_request(struct Client* amp_client, 
    struct Msg* msg, uint32_t service_id, uint8_t* data, uint32_t data_size, char* output_path) {
    
    // step 1: reset msg
    msg_ops.msg_reset(msg);

    // step 2: malloc shared memory
    uint8_t* shm_data = (uint8_t*)client_ops.shm_malloc(amp_client, data_size + 1, MALLOC_TYPE_V);
    if (shm_data == NULL)
    {
        printf("[Error] shm malloc [size = %u, type = %u, ptr = NULL]\n", 
            data_size + 1, MALLOC_TYPE_V);
        return -1;
    }
    // printf("[Trace] shm malloc [size = %u, type = %u, ptr = %p]\n", 
    //     data_size + 1, MALLOC_TYPE_V, shm_data);
    
    // step 3: copy data to shared memory
    if (data_size > 0) {
        for (int j = 0; j < data_size; j++)
        {
            shm_data[j] = data[j];
        }    
        shm_data[data_size] = '\0';
        // printf("[Trace] shm_data: %s, data_size = %d\n", shm_data, data_size);
    }
    
    // step 4: fill msg
    msg->offset = client_ops.shm_addr_to_offset(shm_data);
    msg->length = data_size + 1;
    // printf("[Trace] info : msg fill [offset = 0x%x, length = %u]\n", msg->offset, msg->length);

    // step 5: send + notify
    if (client_ops.msg_send_and_notify(amp_client, msg) != 0)
    {
        printf("[Error] msg send [offset = %x, length = %u], check it\n", msg->offset, msg->length);
        return -1;
    }
    // printf("[Trace] msg send [offset = %x, length = %u]\n", msg->offset, msg->length);

    // step 6: wait for response
    // client_ops.set_client_request_cnt(amp_client); // += 1
    // uint32_t request_cnt = client_ops.get_client_request_cnt(amp_client); // record request cnt to r21 register
    // while(client_ops.msg_poll(msg, request_cnt) != 0) {
    //     printf("is polling...\n");
    //     sleep(3);
    // }
    while(client_ops.msg_poll(msg) != 0) {
        // printf("is polling...\n");
        // sleep(3);
    }


    // printf("[Trace] msg result [service_id = %u, result = %u]\n",
    //         msg->service_id, msg->flag.service_result);
    
    // step 7: fetch result data from shared memory
    if (services[service_id].need_fetch_data) {
        if (output_path!= NULL) {
            // write result data to file
            FILE *file;
            file = fopen(output_path, "w");
            if (file == NULL) {
                printf("open file failed\n");
                return -1;
            }
            fprintf(file, "%s", shm_data);
            fclose(file);
        } else {
            printf("service_id : %d, result %s\n", service_id, shm_data);
        }
    }

    // step 8: free shared memory
    client_ops.shm_free(amp_client, shm_data);

    return 0;
}
static int hyper_amp_client(int argc, char* argv[]) {
    // 参数检查
    if (argc < 3) {
        printf("Usage: hvisor shm hyper_amp <shm_json_path> <data|@filename> <service_id>\n");
        printf("Examples:\n");
        printf("  hvisor shm hyper_amp shm_config.json \"hello world\" 1\n");
        printf("  hvisor shm hyper_amp shm_config.json @data.txt 2\n");
        printf("  hvisor shm hyper_amp shm_config.json hex:48656c6c6f 2  (hex input)\n");
        return -1;
    }
    
    char* shm_json_path = argv[0];
    char* data_input = argv[1];
    uint32_t service_id = (argc >= 3) ? strtoul(argv[2], NULL, 10) : NPUCore_SERVICE_ECHO_ID;
    
    // 数据处理：支持直接字符串或从文件读取
    char* data_buffer = NULL;
    int data_size = 0;
    
    if (data_input[0] == '@') {
        // 从文件读取数据
        char* filename = data_input + 1; // 跳过 '@' 符号
        printf("Reading data from file: %s\n", filename);
        
        size_t file_data_size;
        char* file_content = read_data_from_file(filename, &file_data_size);
        if (file_content == NULL) {
            return -1;
        }
        
        printf("Successfully read %d bytes from file\n", (int)file_data_size);
        
        // 检查文件内容是否以 "hex:" 开头
        if (file_data_size >= 4 && strncmp(file_content, "hex:", 4) == 0) {
            // 文件内容是十六进制格式，进行解析
            char* hex_str = file_content + 4; // 跳过 "hex:" 前缀
            int hex_len = strlen(hex_str);
            
            // 移除可能的换行符
            if (hex_len > 0 && hex_str[hex_len-1] == '\n') {
                hex_str[hex_len-1] = '\0';
                hex_len--;
            }
            
            if (hex_len % 2 != 0) {
                printf("Error: Hex string in file must have even number of characters\n");
                free(file_content);
                return -1;
            }
            
            data_size = hex_len / 2;
            data_buffer = malloc(data_size + 1);
            if (data_buffer == NULL) {
                printf("Error: Memory allocation failed\n");
                free(file_content);
                return -1;
            }
            
            // 转换十六进制字符串为字节
            for (int i = 0; i < data_size; i++) {
                char hex_byte[3] = {hex_str[i*2], hex_str[i*2+1], '\0'};
                data_buffer[i] = (char)strtol(hex_byte, NULL, 16);
            }
            data_buffer[data_size] = '\0';
            
            printf("Using hex data from file: ");
            for (int i = 0; i < data_size; i++) {
                printf("%02x ", (unsigned char)data_buffer[i]);
            }
            printf("(%d bytes)\n", data_size);
            
            free(file_content);
        } else {
            // 文件内容是普通文本
            data_buffer = file_content;
            data_size = file_data_size;
            printf("Using file content as text: \"%s\" (%d bytes)\n", data_buffer, data_size);
        }
    } else if (strncmp(data_input, "hex:", 4) == 0) {
        // 十六进制输入支持: hex:48656c6c6f
        char* hex_str = data_input + 4; // 跳过 "hex:" 前缀
        int hex_len = strlen(hex_str);
        
        if (hex_len % 2 != 0) {
            printf("Error: Hex string must have even number of characters\n");
            return -1;
        }
        
        data_size = hex_len / 2;
        data_buffer = malloc(data_size + 1);
        if (data_buffer == NULL) {
            printf("Error: Memory allocation failed\n");
            return -1;
        }
        
        // 转换十六进制字符串为字节
        for (int i = 0; i < data_size; i++) {
            char hex_byte[3] = {hex_str[i*2], hex_str[i*2+1], '\0'};
            data_buffer[i] = (char)strtol(hex_byte, NULL, 16);
        }
        data_buffer[data_size] = '\0';
        
        printf("Using hex input: ");
        for (int i = 0; i < data_size; i++) {
            printf("%02x ", (unsigned char)data_buffer[i]);
        }
        printf("(%d bytes)\n", data_size);
    } else {
        // 直接使用字符串
        data_size = strlen(data_input);
        data_buffer = malloc(data_size + 1);
        if (data_buffer == NULL) {
            printf("Error: Memory allocation failed\n");
            return -1;
        }
        strcpy(data_buffer, data_input);
        printf("Using input string: \"%s\" (%d bytes)\n", data_buffer, data_size);
    }
    
    printf("=== Testing SHM Client ===\n");
    printf("Configuration: %s\n", shm_json_path);
    printf("Service ID: %u\n", service_id);
    printf("Data size: %d bytes\n", data_size);
    
    parse_global_addr(shm_json_path);
    //初始化客户端
    struct Client amp_client = { 0 };
    if (client_ops.client_init(&amp_client, ZONE_NPUcore_ID) != 0)
    {
        printf("error: client init failed\n");
        free(data_buffer);
        return -1;
    }
    printf("info: client init success\n");
    //获取空闲消息
    struct Msg *msg = client_ops.empty_msg_get(&amp_client, service_id);
    if (msg == NULL)
    {
        printf("error : empty msg get [service_id = %u]\n", service_id);
        free(data_buffer);
        return -1;
    }
    printf("info : empty msg get [service_id = %u]\n", service_id);

    // 重置消息
    msg_ops.msg_reset(msg);
    
    // 分配共享内存
    char* shm_data = (char*)client_ops.shm_malloc(&amp_client, data_size + 1, MALLOC_TYPE_P);
    if (shm_data == NULL)
    {
        printf("info: MALLOC_TYPE_P failed, trying MALLOC_TYPE_V...\n");
        shm_data = (char*)client_ops.shm_malloc(&amp_client, data_size + 1, MALLOC_TYPE_V);
    }
    if (shm_data == NULL)
    {
        printf("error : shm malloc failed [size = %u]\n", data_size + 1);
        free(data_buffer);
        return -1;
    }
    
    printf("info : shm malloc success [size = %u, ptr = %p]\n", data_size + 1, shm_data);
    
    // 复制数据到共享内存
    // memcpy(shm_data, data_buffer, data_size);
    // shm_data[data_size] = '\0';

    for (int j = 0; j < data_size; j++) {
        shm_data[j] = data_buffer[j];
    }
    shm_data[data_size] = '\0';

    // 设置消息
    msg->offset = client_ops.shm_addr_to_offset(shm_data);
    msg->length = data_size + 1;

    // 发送消息并通
    if (client_ops.msg_send_and_notify(&amp_client, msg) != 0)
    {
        printf("error : msg send failed [offset = 0x%x, length = %u]\n", msg->offset, msg->length);
        free(data_buffer);
        return -1;
    }
    
    printf("info : msg sent successfully\n");
    
    // 等待响应
    printf("info : waiting for Non-Root Linux to process the request...\n");
    while(client_ops.msg_poll(msg) != 0) {
        // 轮询等待响应
        printf("is polling...\n");
        sleep(3);

    }
        // 读取处理后的结果数据
    if (msg->flag.service_result == MSG_SERVICE_RET_SUCCESS) {
        printf("=== HyperAMP Service Result ===\n");
        
        // 获取处理后的实际数据大小（可能与输入大小不同）
        int result_data_size = msg->length > 0 ? msg->length - 1 : 0; // 减去末尾的 null terminator
        if (result_data_size <= 0) {
            result_data_size = data_size; // 回退到原始大小
        }

        if (service_id == 1) {
            printf("Encryption completed. Encrypted data:\n");
        } else if (service_id == 2) {
            printf("Decryption completed. Decrypted data:\n");
        } else {
            printf("Service %u completed. Result data:\n", service_id);
        }
        // 确定显示长度：小于等于256字节全部显示，超过则显示前64字节
        int display_length = result_data_size;
        bool truncated = false;
        if (result_data_size > 256) {
            display_length = 64;
            truncated = true;
        }
        
        // 生成输出文件名
        char output_filename[256];
        if (service_id == 1) {
            snprintf(output_filename, sizeof(output_filename), "encrypted_result.txt");
        } else if (service_id == 2) {
            snprintf(output_filename, sizeof(output_filename), "decrypted_result.txt");
        } else {
            snprintf(output_filename, sizeof(output_filename), "service_%u_result.txt", service_id);
        }
        
        // 打开文件准备保存
        FILE* output_file = fopen(output_filename, "wb");        
        // 安全地显示处理后的数据 -使用动态显示长度
        printf("Result: [");
        for (int i = 0; i < display_length; i++) {
            // 同时写入文件（如果文件打开成功）
            if (output_file != NULL) {
                fputc(shm_data[i], output_file);
            }
            
            if (shm_data[i] >= 32 && shm_data[i] <= 126) {  // 可打印字符
                printf("%c", shm_data[i]);
            } else if (shm_data[i] == '\n') {  // 换行符特殊处理
                printf("\\n");
            } else if (shm_data[i] == '\r') {  // 回车符特殊处理
                printf("\\r");
            } else if (shm_data[i] == '\t') {  // 制表符特殊处理
                printf("\\t");
            } else {
                printf("\\x%02x", (unsigned char)shm_data[i]);
            }
        }       
        if (truncated) {
            printf("... (showing first %d of %d bytes)", display_length, result_data_size);
        }
        printf("] (%d bytes)\n", result_data_size);
        
        // 显示十六进制格式 - 同样使用动态显示长度
        printf("Hex format: ");
        for (int i = 0; i < display_length; i++) {
            printf("%02x", (unsigned char)shm_data[i]);
        }
        if (truncated) {
            printf("... (showing first %d of %d bytes)", display_length, result_data_size);
        }
        printf("\n");
        // 如果数据被截断，提示查看完整内容的方法
        if (truncated) {
            printf("Note: Large data truncated for display. Full data saved to file.\n");
        }        
        // 如果是加密服务，提供解密命令提示 - 添加安全检查，使用实际结果大小
        if (service_id == 1 && result_data_size > 0 && result_data_size <= 64) {
            printf("\nTo decrypt, use: ./hvisor shm hyper_amp %s hex:", shm_json_path);
            for (int i = 0; i < result_data_size; i++) {
                printf("%02x", (unsigned char)shm_data[i]);
            }
            printf(" 2\n");
            printf("Or from file: ./hvisor shm hyper_amp %s @%s 2\n", shm_json_path, output_filename);
        } else if (service_id == 1 && result_data_size > 64) {
            printf("\nData too large for command line hex display. Use file input:\n");
            printf("./hvisor shm hyper_amp %s @%s 2\n", shm_json_path, output_filename);
        }
        
        printf("===============================\n");
    } else {
        printf("error : HyperAMP service failed [service_id = %u]\n", service_id);
    }
    
    // 注意：跳过 shm_free 因为在 HVisor 环境中会导致段错误
    // client_ops.shm_free(&amp_client, shm_data);
    
    printf("info : SHM test completed successfully\n");
    
    // 清理资源
    client_ops.empty_msg_put(&amp_client, msg);
    client_ops.client_destory(&amp_client);
    
    // 清理分配的数据缓冲区
    free(data_buffer);
    
    return 0;
}

static int test_shm(int argc, char* argv[]) {
    if (argc < 3) {
        printf("Usage: hvisor shm test_shm <shm_json_path> <data|@filename> <service_id>\n");
        printf("Examples:\n");
        printf("  hvisor shm test_shm shm_config.json \"hello world\" 1\n");
        printf("  hvisor shm test_shm shm_config.json @data.txt 2\n");
        return -1;
    }

    char* shm_json_path = argv[0];
    char* data_input = argv[1];
    uint32_t service_id = (argc >= 3) ? strtoul(argv[2], NULL, 10) : NPUCore_SERVICE_ECHO_ID;
    
    // 数据处理：支持直接字符串或从文件读取
    char* data_buffer = NULL;
    int data_size = 0;
     if (data_input[0] == '@') {
        // 从文件读取数据
        char* filename = data_input + 1; // 跳过 '@' 符号
        printf("Reading data from file: %s\n", filename);
        
        size_t file_data_size;
        data_buffer = read_data_from_file(filename, &file_data_size);
        if (data_buffer == NULL) {
            return -1;
        }
        
        data_size = file_data_size;
        printf("Successfully read %d bytes from file\n", data_size);
    } else {
        // 直接使用字符串
        data_size = strlen(data_input);
        data_buffer = malloc(data_size + 1);
        if (data_buffer == NULL) {
            printf("Error: Memory allocation failed\n");
            return -1;
        }
        strcpy(data_buffer, data_input);
        printf("Using input string: \"%s\" (%d bytes)\n", data_buffer, data_size);
    }
    printf("=== Testing SHM Client ===\n");
    printf("Configuration: %s\n", shm_json_path);
    printf("Data: \"%s\" (%d bytes)\n", data_buffer, data_size);
    printf("Service ID: %u\n", service_id);

    parse_global_addr(shm_json_path);

    printf("=== Testing SHM Client ===\n");
    struct Client amp_client = { 0 };
    if (client_ops.client_init(&amp_client, ZONE_NPUcore_ID) != 0)
    {
        printf("client init failed\n");
        while(1) {}
    }
    printf("client init success\n");
    // Add detailed debug info about the client initialization result
    printf("debug: Client initialization completed, proceeding with memory operations\n");
    struct Msg *msg = client_ops.empty_msg_get(&amp_client, service_id);
    if (msg == NULL)
    {
        printf("error : empty msg get [service_id = %u]\n", service_id);
        free(data_buffer);
        return -1;
    }
    printf("info : empty msg get [service_id = %u]\n", service_id);


    int send_count = 1;  // 增加测试次数来观察内存分配模式
    printf("\n=== Memory Allocation Test: %d iterations ===\n", send_count);
    printf("Testing for potential memory leaks by monitoring allocation addresses\n");
    for (int i = 0; i < send_count; i++) {
        printf("\n--- Iteration %d ---\n", i + 1);
        msg_ops.msg_reset(msg);
        // TODO: compare to OpenAMP
        // using shared memory
        printf("debug: Trying to allocate from persistent block instead...\n");
        char* shm_data = (char*)client_ops.shm_malloc(&amp_client, data_size + 1, MALLOC_TYPE_P);
        if (shm_data == NULL)
        {
            printf("info: MALLOC_TYPE_P failed, trying MALLOC_TYPE_V with smaller size...\n");
            shm_data = (char*)client_ops.shm_malloc(&amp_client, 8, MALLOC_TYPE_V);  // Try smaller allocation
        }
        if (shm_data == NULL)
        {
            printf("error : shm malloc [idx = %d, size = %u, type = %u, ptr = NULL]\n", 
                i, data_size + 1, MALLOC_TYPE_V);
            return;
        }
        // 内存分配监控 - 记录地址和偏移量
        printf("=== MEMORY ALLOCATION TRACKING ===\n");
        printf("  Iteration: %d\n", i + 1);
        printf("  Allocated Address: %p\n", shm_data);
        printf("  Size Requested: %u bytes\n", data_size + 1);
        
        // 计算相对于前一次分配的偏移
        static char* prev_addr = NULL;
        if (prev_addr != NULL) {
            ptrdiff_t offset_diff = shm_data - prev_addr;
            printf("  Offset from previous: %ld bytes (0x%lx)\n", offset_diff, offset_diff);
        } else {
            printf("  -> First allocation\n");
        }
        prev_addr = shm_data;
        printf("=====================================\n");
         printf("info : shm malloc [idx = %d, size = %u, type = %u, ptr = %p]\n", 
             i, data_size + 1, MALLOC_TYPE_V, shm_data);
        // IMMEDIATE test right after allocation
        printf("debug: IMMEDIATE post-allocation test...\n");
        volatile char test_char = shm_data[0];  // Read
        shm_data[0] = 'Z';                      // Write
        printf("debug: IMMEDIATE test successful - read=0x%02x, wrote='Z'\n", test_char);
        shm_data[0] = test_char;                // Restore
        
        // Get the actual zone range from the client

        printf("debug: Allocated address: %p\n", shm_data);

        
        printf("debug: About to write to shm_data=%p, data='%s', data_size=%d, allocated_size=%d\n", 
            shm_data, data_buffer, data_size, data_size + 1);
        
        // Try the absolute minimum test first
        printf("debug: Testing single byte write...\n");
        char original = shm_data[0];
        shm_data[0] = 'X';
        printf("debug: Single byte write successful\n");
        shm_data[0] = original;  // restore
        printf("debug: Address range: %p to %p\n", shm_data, shm_data + data_size + 1);
        
        // Check page alignment and boundaries
        uintptr_t addr = (uintptr_t)shm_data;
        uintptr_t page_size = 4096;  // Assuming 4KB pages
        uintptr_t page_start = addr & ~(page_size - 1);
        uintptr_t page_end = page_start + page_size;
        printf("debug: Page info - addr=0x%lx, page_start=0x%lx, page_end=0x%lx\n", 
            addr, page_start, page_end);
        printf("debug: Write will span from 0x%lx to 0x%lx\n", 
            addr, addr + data_size + 1);
        
        if (addr + data_size + 1 > page_end) {
            printf("debug: WARNING - Write spans across page boundary!\n");
        }
        
        // Test memory access pattern around the allocated region
        printf("debug: Testing memory access pattern...\n");
        for (int test_i = 0; test_i < data_size + 2; test_i++) {
            printf("debug: Testing byte %d at address %p\n", test_i, &shm_data[test_i]);
            char test_val = shm_data[test_i];  // Read
            shm_data[test_i] = 'X';            // Write
            shm_data[test_i] = test_val;       // Restore
            printf("debug: Byte %d access OK\n", test_i);
        }
        
        // Add a delay to avoid potential race conditions with other zones
        printf("debug: Adding delay to avoid race conditions...\n");
        sleep(2);  // Increase delay to 2 seconds
        
        // Try simpler approach - avoid any bulk operations
        printf("debug: Copying data using original logic...\n");
        int j;
        for (j = 0; j < data_size; j++) {
            shm_data[j] = data_buffer[j];
        }
        shm_data[data_size] = '\0';
        printf("debug: About to call shm_addr_to_offset with shm_data=%p\n", shm_data);
        // printf("shm_data: %s, data_size = %d\n", shm_data, data_size);
        msg->offset = client_ops.shm_addr_to_offset(shm_data);
        printf("debug: shm_addr_to_offset returned offset=0x%x\n", msg->offset);

        msg->length = data_size + 1;
        printf("info : msg fill [idx = %d, offset = 0x%x, length = %u]\n", i, msg->offset, msg->length);

        // TODO: check this offset, if necessary ? can't use virtual address?
        
        // send + notify
        printf("debug: About to call msg_send_and_notify\n");

        if (client_ops.msg_send_and_notify(&amp_client, msg) != 0)
        {
            printf("error : msg send [idx = %d, offset = %x, length = %u], check it\n", i, msg->offset, msg->length);
            return;
        }
        printf("debug: msg_send_and_notify completed successfully\n");
        printf("info : msg send [idx = %d, offset = %x, length = %u]\n", i, msg->offset, msg->length);

        printf("enter polling...\n");
        /* 等待消息响应 */
        // client_ops.set_client_request_cnt(&amp_client); // += 1
        // uint32_t request_cnt = client_ops.get_client_request_cnt(&amp_client);
        // while(client_ops.msg_poll(msg, request_cnt) != 0) {
            
        // }
        while(client_ops.msg_poll(msg) != 0) {
            printf("is polling...\n");
            sleep(3);
        }
        printf("info : msg result [idx = %d, service_id = %u, result = %u]\n",
               i, msg->service_id, msg->flag.service_result);
        printf("debug:amp_client:%p,shm_data:%p\n", &amp_client, shm_data);
        // 方案：添加安全检查后尝试释放

        // 生产环境中需要修复 shm_free问题
        printf("debug: Skipping shm_free\n");
        // client_ops.shm_free(&amp_client, shm_data);
    }
    printf("Completed %d allocation cycles without calling shm_free\n", send_count);
    printf("=======================================\n");
    client_ops.empty_msg_put(&amp_client, msg);
    client_ops.client_destory(&amp_client);
    printf("debug: client_destory completed - test_shm finished successfully\n");
        
    // 清理分配的数据缓冲区
    free(data_buffer);
    printf("debug: Data buffer freed\n");
    
    return 0;
}

static int check_service_id(uint32_t service_id) {
    if (service_id == 0 || service_id >= NPUCore_SERVICE_MAX_ID) {
        return -1;
    } else {
        return 0;
    }
}
// 简化版测试服务端 - 验证共享内存数据读取
static int hyper_amp_service(char* shm_json_path) {
    printf("=== Non-Root Linux SHM Read Test ===\n");
    printf("Initializing as SHM server to read client data...\n");
    
    // step 1: 解析地址配置 (从JSON获取动态地址)
    parse_global_addr(shm_json_path);
    printf("[Test] Address configuration parsed\n");
    
    // step 2: 映射共享内存地址 (类似 channels_init 中的 mmap)
    // addr_infos[0] = buf, addr_infos[1] = zone0_msg, addr_infos[2] = zonex_msg
    
    // 打开 /dev/mem 来映射物理内存
    printf("[Test] Opening /dev/mem...\n");
    int mem_fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (mem_fd < 0) {
        printf("[Error] Failed to open /dev/mem: %s (errno=%d)\n", strerror(errno), errno);
        printf("[Info] Make sure you're running as root and /dev/mem is available\n");
        return -1;
    }
    printf("[Test] /dev/mem opened successfully (fd=%d)\n", mem_fd);
    
    // 映射缓冲区 (buf)
    printf("[Test] Mapping buffer: phys=0x%lx, len=0x%x\n", addr_infos[0].start, addr_infos[0].len);
    void* buf_virt = mmap(NULL, addr_infos[0].len, PROT_READ | PROT_WRITE, 
                         MAP_SHARED, mem_fd, addr_infos[0].start);
    if (buf_virt == MAP_FAILED) {
        printf("[Error] Failed to map buffer memory: %s\n", strerror(errno));
        close(mem_fd);
        return -1;
    }
    printf("[Test] Buffer mmap successful: %p\n", buf_virt);
    
    // 映射消息队列 - 测试：同时映射两个队列来调试
    printf("[Test] Mapping Non-Root Linux msg queue: phys=0x%lx, len=0x%x\n", addr_infos[2].start, addr_infos[2].len);
    void* msg_queue_virt = mmap(NULL, addr_infos[2].len, PROT_READ | PROT_WRITE,
                               MAP_SHARED, mem_fd, addr_infos[2].start);
    
    // 同时映射 Root Linux 的队列用于调试
    printf("[Test] Also mapping Root Linux msg queue for debugging: phys=0x%lx, len=0x%x\n", addr_infos[1].start, addr_infos[1].len);
    void* root_msg_queue_virt = mmap(NULL, addr_infos[1].len, PROT_READ | PROT_WRITE,
                                    MAP_SHARED, mem_fd, addr_infos[1].start);
    if (msg_queue_virt == MAP_FAILED) {
        printf("[Error] Failed to map message queue memory: %s\n", strerror(errno));
        munmap(buf_virt, addr_infos[0].len);
        close(mem_fd);
        return -1;
    }
    
    if (root_msg_queue_virt == MAP_FAILED) {
        printf("[Error] Failed to map root message queue memory: %s\n", strerror(errno));
        munmap(buf_virt, addr_infos[0].len);
        munmap(msg_queue_virt, addr_infos[2].len);
        close(mem_fd);
        return -1;
    }
    
    uint64_t buf_addr = (uint64_t)buf_virt;
    uint64_t msg_queue_addr = (uint64_t)msg_queue_virt;
    uint64_t root_msg_queue_addr = (uint64_t)root_msg_queue_virt;
    
    printf("[Test] Buffer mapped: 0x%lx -> 0x%lx (len: 0x%x)\n", 
           addr_infos[0].start, buf_addr, addr_infos[0].len);
    printf("[Test] Non-Root Linux MSG queue mapped: 0x%lx -> 0x%lx (len: 0x%x)\n", 
           addr_infos[2].start, msg_queue_addr, addr_infos[2].len);
    printf("[Test] Root Linux MSG queue mapped: 0x%lx -> 0x%lx (len: 0x%x)\n", 
           addr_infos[1].start, root_msg_queue_addr, addr_infos[1].len);
    
    if (msg_queue_addr == 0 || buf_addr == 0 || root_msg_queue_addr == 0) {
        printf("[Error] Failed to get required addresses\n");
        return -1;
    }
    
    // step 3: 简单初始化消息队列 (类似 channels_init())
    printf("[Test] Initializing message queue...\n");
    struct AmpMsgQueue* msg_queue = (struct AmpMsgQueue*)msg_queue_addr;
    
    printf("[Test] Before init - working_mark: 0x%x\n", msg_queue->working_mark);
    printf("[Test] Current buf_size: %u\n", msg_queue->buf_size);
    printf("[Test] Current empty_h: %u, wait_h: %u, proc_ing_h: %u\n", 
           msg_queue->empty_h, msg_queue->wait_h, msg_queue->proc_ing_h);
    
    // 设置为已初始化状态 - 这是关键步骤！
    msg_queue->working_mark = INIT_MARK_INITIALIZED;
    
    printf("[Test] After init - working_mark: 0x%x\n", msg_queue->working_mark);
    printf("[Test] Message queue marked as initialized!\n");
    
    // step 4: 显示消息队列状态
    printf("[Test] Non-Root Linux Message Queue Info:\n");
    printf("  Buffer size: %u\n", msg_queue->buf_size);
    printf("  Empty head: %u\n", msg_queue->empty_h);
    printf("  Wait head: %u\n", msg_queue->wait_h);
    printf("  Processing head: %u\n", msg_queue->proc_ing_h);
    
    // 同时检查 Root Linux 队列状态
    struct AmpMsgQueue* root_msg_queue = (struct AmpMsgQueue*)root_msg_queue_addr;
    printf("[Test] Root Linux Message Queue Info:\n");
    printf("  Working mark: 0x%x\n", root_msg_queue->working_mark);
    printf("  Buffer size: %u\n", root_msg_queue->buf_size);
    printf("  Empty head: %u\n", root_msg_queue->empty_h);
    printf("  Wait head: %u\n", root_msg_queue->wait_h);
    printf("  Processing head: %u\n", root_msg_queue->proc_ing_h);
    
    // step 5: 计算消息实体数组的起始地址 (类似 get_msg_entries_start_addr())
    uint64_t msg_entries_addr = msg_queue_addr + sizeof(struct AmpMsgQueue);
    uint64_t root_msg_entries_addr = root_msg_queue_addr + sizeof(struct AmpMsgQueue);
    printf("[Test] Non-Root message entries start at: 0x%lx\n", msg_entries_addr);
    printf("[Test] Root message entries start at: 0x%lx\n", root_msg_entries_addr);
    
    // step 6: 验证共享缓冲区可访问性 (安全检查)
    char* test_buf = (char*)buf_addr;
    printf("[Test] Testing buffer access...\n");
    printf("[Test] buf_addr = 0x%lx, len = 0x%x\n", buf_addr, addr_infos[0].len);
    
    // 更安全的测试 - 先尝试读取，再尝试写入
    if (buf_addr != 0 && addr_infos[0].len > 100) {
        printf("[Test] Attempting to read from buffer...\n");
        volatile char first_byte = test_buf[0];  // 测试读取
        printf("[Test] First byte read successful: 0x%02x\n", first_byte);
        
        printf("[Test] Attempting to write to buffer...\n");
        test_buf[0] = 'T';  // 测试单字节写入
        test_buf[1] = 'e';
        test_buf[2] = 's';
        test_buf[3] = 't';
        test_buf[4] = '\0';
        printf("[Test] Single byte write successful\n");
        
        printf("[Test] Buffer test: \"%s\"\n", test_buf);
    } else {
        printf("[Test] Buffer access skipped due to safety concerns (addr=0x%lx, len=0x%x)\n", 
               buf_addr, addr_infos[0].len);
    }
    
    // step 7: 监听并处理消息 (结合中断机制)
    printf("\n[Test] Waiting for messages from Root Linux...\n");
    printf("Press Ctrl+C to exit\n");
    //暂时用不到该功能running默认为1
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    // 打开SHM中断监听设备
    printf("[Test] Opening /dev/hshm0 for interrupt monitoring...\n");
    int shm_fd = open("/dev/hshm0", O_RDONLY);
    if (shm_fd < 0) {
        printf("[Warning] Failed to open /dev/hshm0: %s\n", strerror(errno));
        printf("[Info] Will use polling mode instead of interrupt mode\n");
    } else {
        printf("[Test] /dev/hshm0 opened successfully for interrupt monitoring\n");
    }
    
    struct pollfd pfd;
    if (shm_fd >= 0) {
        pfd.fd = shm_fd;
        pfd.events = POLLIN;
    }
    
    int msg_count = 0;
    int check_counter = 0;
    
    while (running && msg_count < 10) {
        bool should_check_messages = false;
        
        // 如果有中断设备，先检查中断
        if (shm_fd >= 0) {
            int ret = poll(&pfd, 1, 100); // 100ms超时
            if (ret > 0 && (pfd.revents & POLLIN)) {//PLOLLIN事件chunk
                printf("\033[2K\r");  // 清除当前行并回到行首
                printf("[Test] SHM interrupt received, checking for messages...\n");
                should_check_messages = true;
                
                // 读取中断信息
                shm_signal_info_t signal_info;
                if (ioctl(shm_fd, HVISOR_SHM_SIGNAL_INFO, &signal_info) == 0) {
                    printf("[Test] Signal count: %u, cpu: %u, service_id: %u\n", 
                           signal_info.signal_count, signal_info.current_cpu, signal_info.last_service_id);
                }
                fflush(stdout);  // 立即刷新输出
            }
        }
        
        // 定期检查或中断触发检查
        if (should_check_messages || (++check_counter % 10 == 0)) {
            // 首先检查 Non-Root Linux 自己的队列
            bool found_message = false;
            
            if (msg_queue->proc_ing_h < msg_queue->buf_size) {
                printf("\n[Test] *** MESSAGE DETECTED IN NON-ROOT QUEUE *** Processing message #%d\n", ++msg_count);
                found_message = true;
                
                // 处理 Non-Root 队列消息的逻辑...
                // (保留原有逻辑)
            }
            
            // 关键修改：检查并处理 Root Linux 队列中的消息
            if (!found_message && root_msg_queue->proc_ing_h < root_msg_queue->buf_size) {
                printf("\n[Test] *** PROCESSING MESSAGE FROM ROOT LINUX QUEUE *** Message #%d\n", ++msg_count);
                found_message = true;
                
                // 处理 Root Linux 队列中的消息
                uint16_t head = root_msg_queue->proc_ing_h;
                uint16_t msg_index = head;
                
                // 计算当前消息实体的地址
                uint64_t msg_entry_addr = root_msg_entries_addr + sizeof(struct MsgEntry) * head;
                struct MsgEntry* msg_entry = (struct MsgEntry*)msg_entry_addr;
                struct Msg* msg = &msg_entry->msg;
                
                printf("  Root Message details:\n");
                printf("    Index: %u\n", msg_index);
                printf("    Service ID: %u\n", msg->service_id);
                printf("    Offset: 0x%x\n", msg->offset);
                printf("    Length: %u\n", msg->length);
                printf("    Deal state: %u\n", msg->flag.deal_state);
                
                // 从共享内存读取数据
                if (msg->length > 0) {
                    char* data_ptr = (char*)(buf_addr + msg->offset);
                    printf("    Reading from buf_addr=0x%lx + offset=0x%x = 0x%lx\n", 
                           buf_addr, msg->offset, (uint64_t)data_ptr);
                    // 安全地显示数据内容 - 逐字节打印
                    printf("    *** DATA FROM ROOT LINUX: [");
                    for (int i = 0; i < msg->length && i < 32; i++) {  // 最多显示32字节
                        if (data_ptr[i] >= 32 && data_ptr[i] <= 126) {  // 可打印字符
                            printf("%c", data_ptr[i]);
                        } else {
                            printf("\\x%02x", (unsigned char)data_ptr[i]);
                        }
                    }
                    if (msg->length > 32) printf("...");
                    printf("] (%u bytes) ***\n", msg->length);                    
                    // ===== HyperAMP 安全服务处理 =====
                    printf("    Processing HyperAMP secure service (Service ID: %u)...\n", msg->service_id);
                    
                    int service_result = MSG_SERVICE_RET_SUCCESS;
                    bool data_modified = false;
                    
                    // 根据 service_id 执行相应的安全服务
                    switch (msg->service_id) {
                        case 1:  // 加密服务
                            printf("    [HyperAMP] Executing ENCRYPTION service\n");
                            if (hyperamp_encrypt_service(data_ptr, msg->length - 1, msg->length) == 0) {
                                printf("    [HyperAMP] Encryption completed successfully\n");
                                data_modified = true;
                            } else {
                                printf("    [HyperAMP] Encryption failed\n");
                                service_result = MSG_SERVICE_RET_FAIL;
                            }
                            break;
                            
                        case 2:  // 解密服务
                            printf("    [HyperAMP] Executing DECRYPTION service\n");
                            if (hyperamp_decrypt_service(data_ptr, msg->length - 1, msg->length) == 0) {
                                printf("    [HyperAMP] Decryption completed successfully\n");
                                data_modified = true;
                            } else {
                                printf("    [HyperAMP] Decryption failed\n");
                                service_result = MSG_SERVICE_RET_FAIL;
                            }
                            break;
                            
                        case 66:  // 测试服务 (Echo)
                            printf("    [HyperAMP] Executing ECHO test service\n");
                            // Echo服务不修改数据，只是测试通信
                            break;
                            
                        default:
                            printf("    [HyperAMP] Unknown service ID: %u, treating as echo\n", msg->service_id);
                            break;
                    }
                    
                    // 如果数据被修改，显示处理后的结果
                    if (data_modified) {
                        printf("    *** PROCESSED DATA: [");
                        for (int i = 0; i < msg->length && i < 32; i++) {
                            if (data_ptr[i] >= 32 && data_ptr[i] <= 126) {
                                printf("%c", data_ptr[i]);
                            } else {
                                printf("\\x%02x", (unsigned char)data_ptr[i]);
                            }
                        }
                        if (msg->length > 32) printf("...");
                        printf("] ***\n");
                    }
                    
                    // 标记消息已处理
                    printf("    Setting deal_state to MSG_DEAL_STATE_YES...\n");
                    msg->flag.deal_state = MSG_DEAL_STATE_YES;
                    msg->flag.service_result = service_result;
                    
                    printf("    *** HYPERAMP SERVICE COMPLETED! ***\n");
                    printf("    Root Linux should now detect completion and read processed data\n");
                } else {
                    printf("    No data to read (length=%u)\n", msg->length);
                    msg->flag.deal_state = MSG_DEAL_STATE_YES;
                    msg->flag.service_result = MSG_SERVICE_RET_FAIL;
                }
                
                // 更新 Root Linux 队列头
                uint16_t new_head = msg_entry->nxt_idx;
                root_msg_queue->proc_ing_h = new_head;
                msg_entry->nxt_idx = root_msg_queue->buf_size; // 标记为无效
                // 重置工作状态，允许下一次通信
                root_msg_queue->working_mark = MSG_QUEUE_MARK_IDLE;
                printf("    Updated Root Linux proc_ing_h: %u -> %u\n", head, new_head);
                printf("    Reset working_mark to IDLE (0x%x)\n", MSG_QUEUE_MARK_IDLE);
                printf("    Root Linux should now detect the state change and stop polling\n");
            }
            if (!found_message && check_counter % 50 == 0) {
                printf("\033[2K\r");  // 清除当前行并回到行首 
                // 每5秒显示一次等待状态，添加换行和刷新缓冲
                printf("[Test] Waiting... (Non-Root:%u, Root:%u)\n", 
                       msg_queue->proc_ing_h, root_msg_queue->proc_ing_h);
                fflush(stdout);  // 强制刷新输出缓冲区
            }
        }
        
        usleep(100000); // 100ms
    }
    
    printf("\n[Test] SHM read test completed. Processed %d messages.\n", msg_count);
    
    // step 8: 清理资源
    if (shm_fd >= 0) {
        close(shm_fd);
        printf("[Test] Closed /dev/hshm0\n");
    }
    munmap((void*)buf_addr, addr_infos[0].len);
    munmap((void*)msg_queue_addr, addr_infos[2].len);
    munmap((void*)root_msg_queue_addr, addr_infos[1].len);
    close(mem_fd);
    
    return 0;
}

// Non-root Linux 服务端实现 - 处理来自 root Linux 的安全服务请求
static int shm_server(char* shm_json_path) {
    printf("=== Non-Root Linux SHM Server ===\n");
    printf("Initializing SHM server to handle requests from Root Linux...\n");
    
    // step 1: 解析配置并初始化地址信息
    parse_global_addr(shm_json_path);
    printf("[Server] Address configuration parsed\n");
    
    // step 2: 初始化通道（作为接收端）
    if (channel_ops.channels_init() != 0) {
        printf("[Error] Failed to initialize channels\n");
        return -1;
    }
    printf("[Server] Channels initialized\n");
    
    // step 3: 获取用于接收消息的通道（从Root Linux到当前zone）
    struct Channel* receive_channel = channel_ops.target_channel_get(ZONE_LINUX_ID);
    if (receive_channel == NULL) {
        printf("[Error] Failed to get receive channel from Root Linux\n");
        return -1;
    }
    printf("[Server] Receive channel obtained\n");
    
    // step 4: 初始化共享内存管理
    struct Client server_client = {0};
    if (client_ops.client_init(&server_client, ZONE_LINUX_ID) != 0) {
        printf("[Error] Failed to initialize server client\n");
        return -1;
    }
    printf("[Server] Server client initialized\n");
    
    // step 5: 检查消息队列是否就绪
    if (channel_ops.channel_is_ready(receive_channel) != 0) {
        printf("[Error] Channel is not ready\n");
        return -1;
    }
    printf("[Server] Channel is ready, waiting for messages...\n");
    
    // step 6: 主循环 - 处理消息
    int processed_count = 0;
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    while (running) {
        // 检查消息队列是否有待处理的消息
        if (receive_channel->msg_queue->proc_ing_h >= receive_channel->msg_queue->buf_size) {
            // 没有待处理的消息，短暂休眠
            usleep(1000); // 1ms
            continue;
        }
        
        // step 7: 从处理队列中取出消息
        uint16_t msg_index = msg_queue_ops.pop(receive_channel->msg_queue, 
                                             &receive_channel->msg_queue->proc_ing_h);
        
        if (msg_index >= receive_channel->msg_queue->buf_size) {
            // 队列为空，继续等待
            usleep(1000);
            continue;
        }
        
        // step 8: 获取消息对象
        struct MsgEntry* msg_entry = &receive_channel->msg_queue->entries[msg_index];
        struct Msg* msg = &msg_entry->msg;
        
        printf("\n[Server] Processing message #%d:\n", ++processed_count);
        printf("  Service ID: %u\n", msg->service_id);
        printf("  Offset: 0x%x\n", msg->offset);
        printf("  Length: %u\n", msg->length);
        printf("  Deal State: %u\n", msg->flag.deal_state);
        
        // step 9: 从共享内存读取数据
        void* shm_data = client_ops.shm_offset_to_addr(msg->offset);
        if (shm_data == NULL) {
            printf("[Error] Failed to get shared memory data\n");
            msg->flag.service_result = MSG_SERVICE_RET_FAIL;
            msg->flag.deal_state = MSG_DEAL_STATE_YES;
            continue;
        }
        
        char* input_data = (char*)shm_data;
        printf("  Input Data: \"%s\"\n", input_data);
        
        // step 10: 根据 service_id 处理请求
        switch (msg->service_id) {
            case NPUCore_SERVICE_ECHO_ID:
                // Echo 服务 - 直接返回原数据（测试用）
                printf("  Processing ECHO service\n");
                // 数据已经在共享内存中，无需修改
                msg->flag.service_result = MSG_SERVICE_RET_SUCCESS;
                break;
                
            case NPUCore_SERVICE_FLIP_ID:
                // 翻转字符串服务
                printf("  Processing FLIP service\n");
                int len = strlen(input_data);
                for (int i = 0; i < len / 2; i++) {
                    char temp = input_data[i];
                    input_data[i] = input_data[len - 1 - i];
                    input_data[len - 1 - i] = temp;
                }
                msg->flag.service_result = MSG_SERVICE_RET_SUCCESS;
                break;
                
            case NPUCore_SERVICE_CAESAR_ENCRYPT_ID:
                // Caesar 加密（简单位移3）
                printf("  Processing CAESAR ENCRYPT service\n");
                len = strlen(input_data);
                for (int i = 0; i < len; i++) {
                    if (input_data[i] >= 'a' && input_data[i] <= 'z') {
                        input_data[i] = ((input_data[i] - 'a' + 3) % 26) + 'a';
                    } else if (input_data[i] >= 'A' && input_data[i] <= 'Z') {
                        input_data[i] = ((input_data[i] - 'A' + 3) % 26) + 'A';
                    }
                }
                msg->flag.service_result = MSG_SERVICE_RET_SUCCESS;
                break;
                
            case NPUCore_SERVICE_CAESAR_DECRYPT_ID:
                // Caesar 解密（反向位移3）
                printf("  Processing CAESAR DECRYPT service\n");
                len = strlen(input_data);
                for (int i = 0; i < len; i++) {
                    if (input_data[i] >= 'a' && input_data[i] <= 'z') {
                        input_data[i] = ((input_data[i] - 'a' - 3 + 26) % 26) + 'a';
                    } else if (input_data[i] >= 'A' && input_data[i] <= 'Z') {
                        input_data[i] = ((input_data[i] - 'A' - 3 + 26) % 26) + 'A';
                    }
                }
                msg->flag.service_result = MSG_SERVICE_RET_SUCCESS;
                break;
            case NPUCore_SERVICE_XOR_ENCRYPT_ID:
            case NPUCore_SERVICE_XOR_DECRYPT_ID:
                // XOR 加密/解密（使用密钥0x42）
                printf("  Processing XOR ENCRYPT/DECRYPT service\n");
                len = strlen(input_data);
                for (int i = 0; i < len; i++) {
                    input_data[i] ^= 0x42;
                }
                msg->flag.service_result = MSG_SERVICE_RET_SUCCESS;
                break;
                
            case NPUCore_SERVICE_ROT13_ENCRYPT_ID:
            case NPUCore_SERVICE_ROT13_DECRYPT_ID:
                // ROT13 加密/解密
                printf("  Processing ROT13 ENCRYPT/DECRYPT service\n");
                len = strlen(input_data);
                for (int i = 0; i < len; i++) {
                    if (input_data[i] >= 'a' && input_data[i] <= 'z') {
                        input_data[i] = ((input_data[i] - 'a' + 13) % 26) + 'a';
                    } else if (input_data[i] >= 'A' && input_data[i] <= 'Z') {
                        input_data[i] = ((input_data[i] - 'A' + 13) % 26) + 'A';
                    }
                }
                msg->flag.service_result = MSG_SERVICE_RET_SUCCESS;
                break;
                
            case NPUCore_SERVICE_DJB2_HASH_ID:
                // DJB2 哈希
                printf("  Processing DJB2 HASH service\n");
                {
                    unsigned long hash = 5381;
                    int c;
                    char* str = input_data;
                    while ((c = *str++)) {
                        hash = ((hash << 5) + hash) + c;
                    }
                    snprintf(input_data, msg->length, "%lu", hash);
                }
                msg->flag.service_result = MSG_SERVICE_RET_SUCCESS;
                break;
                
            case NPUCore_SERVICE_CRC32_HASH_ID:
                // 简单CRC32哈希
                printf("  Processing CRC32 HASH service\n");
                {
                    unsigned int crc = 0xFFFFFFFF;
                    char* str = input_data;
                    while (*str) {
                        crc ^= *str++;
                        for (int j = 0; j < 8; j++) {
                            crc = (crc >> 1) ^ (0xEDB88320 & (-(crc & 1)));
                        }
                    }
                    snprintf(input_data, msg->length, "%08X", crc ^ 0xFFFFFFFF);
                }
                msg->flag.service_result = MSG_SERVICE_RET_SUCCESS;
                break;
                
            case NPUCore_SERVICE_FNV_HASH_ID:
                // FNV哈希
                printf("  Processing FNV HASH service\n");
                {
                    unsigned int hash = 2166136261U;
                    char* str = input_data;
                    while (*str) {
                        hash ^= *str++;
                        hash *= 16777619;
                    }
                    snprintf(input_data, msg->length, "%08X", hash);
                }
                msg->flag.service_result = MSG_SERVICE_RET_SUCCESS;
                break;
                
            case NPUCore_SERVICE_XORSHIFT_RAND_NUMBER_ID:
                // XORSHIFT随机数生成
                printf("  Processing XORSHIFT RANDOM service\n");
                {
                    static uint32_t state = 1;
                    if (state == 1) state = 123456789; // 初始种子
                    state ^= state << 13;
                    state ^= state >> 17;
                    state ^= state << 5;
                    snprintf(input_data, msg->length, "%u", state);
                }
                msg->flag.service_result = MSG_SERVICE_RET_SUCCESS;
                break;
                
            case NPUCore_SERVICE_LCG_RAND_NUMBER_ID:
                // LCG随机数生成
                printf("  Processing LCG RANDOM service\n");
                {
                    static uint32_t state = 1;
                    state = (state * 1103515245 + 12345) & 0x7fffffff;
                    snprintf(input_data, msg->length, "%u", state);
                }
                msg->flag.service_result = MSG_SERVICE_RET_SUCCESS;
                break;
                
            case NPUCore_SERVICE_RAND_STRING_ID:
                // 随机字符串生成
                printf("  Processing RANDOM STRING service\n");
                {
                    const char charset[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
                    static uint32_t state = 12345;
                    int len = msg->length - 1;
                    if (len > 63) len = 63; // 限制长度
                    for (int i = 0; i < len; i++) {
                        state = (state * 1103515245 + 12345) & 0x7fffffff;
                        input_data[i] = charset[state % (sizeof(charset) - 1)];
                    }
                    input_data[len] = '\0';
                }
                msg->flag.service_result = MSG_SERVICE_RET_SUCCESS;
                break;
                
            case NPUCore_SERVICE_RAND_CHAR_ID:
                // 随机字符生成
                printf("  Processing RANDOM CHAR service\n");
                {
                    static uint32_t state = 54321;
                    state = (state * 1103515245 + 12345) & 0x7fffffff;
                    char rand_char = 'A' + (state % 26);
                    snprintf(input_data, msg->length, "%c", rand_char);
                }
                msg->flag.service_result = MSG_SERVICE_RET_SUCCESS;
                break;
                
                
            default:
                printf("  Unknown service ID: %u\n", msg->service_id);
                msg->flag.service_result = MSG_SERVICE_RET_NOT_EXITS;
                break;
        }
        
        // step 11: 标记消息已处理
        msg->flag.deal_state = MSG_DEAL_STATE_YES;
        
        printf("  Result: %s\n", input_data);
        printf("  Service Result: %u\n", msg->flag.service_result);
        printf("  Message processed successfully\n");
        
        // step 12: 将消息移回空闲队列
        if (msg_queue_ops.push(receive_channel->msg_queue, 
                              &receive_channel->msg_queue->empty_h, msg_index) != 0) {
            printf("[Error] Failed to return message to empty queue\n");
        }
        
        // step 13: 检查是否所有消息都处理完成
        if (receive_channel->msg_queue->proc_ing_h >= receive_channel->msg_queue->buf_size) {
            // 所有消息处理完成，标记队列为空闲状态
            receive_channel->msg_queue->working_mark = MSG_QUEUE_MARK_IDLE;
            printf("[Server] All messages processed, queue marked as IDLE\n");
        }
    }
    
    // step 14: 清理资源
    client_ops.client_destory(&server_client);
    printf("\n[Server] SHM server shutting down\n");
    printf("Total messages processed: %d\n", processed_count);
    
    return 0;
}
static int safe_service(int argc, char *argv[]) {
    // service_id data data_size
    if (argc != 4) {
        help(1);
    } 
    // 0. shm_json_path
    parse_global_addr(argv[0]);

    // 1. service_id (uint32_t)
    uint32_t service_id = 0;
    sscanf(argv[1], "%u", &service_id);
    if (check_service_id(service_id)) {
        printf("[Error] invalid service_id\n");
        print_services();
        return -1;
    }

    // 2. data (bytes array, don't care the type)
    uint8_t* data = argv[2];

    // 3. data_size (uint32_t)
    uint32_t data_size;
    sscanf(argv[3], "%u", &data_size);
    if (data_size == 0) {
        // if provided data_size is 0, use the length of data_size as msg->length
        data_size = strlen(data);
    }

    // printf("[Check] argv[0] : %s, service_id : %u, data : %s, data_size : %u\n",
    //      argv[0], service_id, data, data_size);

    // step 1: init client
    struct Client amp_client = {0};
    if (client_ops.client_init(&amp_client, ZONE_NPUcore_ID) != 0)
    {
        printf("[Error] client init failed\n");
        return -1;
    }
    // printf("[Trace] client init success\n");

    // step 2: get empty msg
    struct Msg *msg = client_ops.empty_msg_get(&amp_client, service_id);
    if (msg == NULL)
    {
        printf("[Error] empty msg get [service_id = %u]\n", service_id);
        return -1;
    }
    // printf("[Trace] empty msg get [service_id = %u]\n", service_id);

    // step 3: do safe service request
    int ret = general_safe_service_request(&amp_client, msg, service_id, data, data_size, NULL);
    if (ret != 0) {
        printf("[Error] safe service request failed\n");
        return -1;
    }

    // step 4: put empty msg
    client_ops.empty_msg_put(&amp_client, msg);   
    // printf("[Trace] empty msg put [service_id = %u]\n", service_id);
    
    // step 5: destory client
    client_ops.client_destory(&amp_client);
    // printf("[Trace] client destory [service_id = %u]\n", service_id);
}

#include "shm/threads.h"
#include "shm/requests.h"

static int parse_and_enqueue(ThreadPool* pool, struct Client* client, const char *input_line, 
    uint32_t request_id, char* output_dir) {
    // Example input line: "1 "PTejVyahf" 01"
    Request *request = (Request*)malloc(sizeof(Request));

    uint8_t data_string[256]; // Adjust size as needed
    unsigned int service_id, size;
    
    int parsed = sscanf(input_line, "%u \"%255[^\"]\" %u", &service_id, data_string, &size);
    if (parsed != 3) {
        printf("parse ok %s\n", input_line);
        return -1;
    }
    
    request->data_string = strdup(data_string);
    if (!request->data_string) {
        perror("Failed to allocate memory for data string");
        return -1;
    }
    request->request_id = request_id;
    request->service_id = service_id;
    request->size = size;
    request->client = client;
    // request->output_dir = output_dir;

    request->output_dir = (char *)malloc(strlen(output_dir) + 1 + 10 + 4);
    if (!request->output_dir) {
        perror("Failed to allocate memory for new output dir");
        free(request->data_string);
        free(request);
        return -1;
    }

    // +1 for '/', +10 for request_id, +4 for ".txt" and null terminator
    snprintf(request->output_dir, strlen(output_dir) + 1 + 10 + 4, "%s/%u.txt", output_dir, request_id);

    // printf("Enqueuing request: request_id=%u, id=%u, data_string=%s,
    // size=%u\n",
    //     request.request_id, request.id, request.data_string, request.size);
    // printf("enqueue request: request_id=%u, output_dir_path=%s\n", 
    //     request_id, request->output_dir);
    // enqueue(queue, request);

    add_task(pool, handle_request, request);
    return 0;
}

static void read_input_file(ThreadPool* pool, struct Client* client, 
    char* input_file_path, char* output_dir) {
    
    struct stat file_stat;
    time_t last_mtime = 0;
    off_t last_size = 0;

    FILE *file = fopen(input_file_path, "r");
    while (file == NULL) {
        file = fopen(input_file_path, "r");
    }

    char line[1024];
    int request_id = 1;

    while (1) {
        while (fgets(line, sizeof(line), file) != NULL) {
            if (parse_and_enqueue(pool, client, line, request_id++, output_dir) != 0) {
                return;
            }
        }
        clearerr(file);
        fseek(file, 0, SEEK_CUR);
        sleep(1);
    }
}

static void setup_shm_client(int argc, char *argv[]) {
    // [0]. shm_json_path
    char *shm_json_path = argv[0];
    parse_global_addr(shm_json_path);

    // [1]. input_file_path
    char *input_file_path = argv[1];
    
    // [2]. output_dir_path
    char *output_dir_path = argv[2];

    // [3]. threads_num
    int threads_num = atoi(argv[3]);

    // step 1: init client
    // struct Client amp_client = {0};
    struct Client *amp_client = (struct Client*)malloc(sizeof(struct Client));
    memset(amp_client, 0, sizeof(struct Client));

    if (client_ops.client_init(amp_client, ZONE_NPUcore_ID) != 0)
    {
        printf("[Error] client init failed\n");
        return -1;
    }
    // printf("client init success\n");

    // step 2: create threads in threads pool
    ThreadPool* threadpool = setup_threads_pool(threads_num, amp_client);
    if (threadpool == NULL) {
        return -1;
    }
    // printf("thread_pool init success\n");

    // step 3: read input file and send requests to threads
    read_input_file(threadpool, amp_client, input_file_path, output_dir_path);
    
    // printf("read_input_file success\n");

    // step 4: wait ...
    while(!task_queue_is_empty(threadpool)) {
        // wait for all tasks to be completed
        sleep(1);
    }

    // step 5: free_request_queue(queue);
    destroy_thread_pool(threadpool);
    
    // step 6: destory client
    client_ops.client_destory(amp_client);
    
    free(amp_client);
}


static void generate_random_string(char *str, size_t length) {
    const char charset[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    size_t charset_size = sizeof(charset) - 1;
    for (size_t i = 0; i < length - 1; i++) {
        int key = rand() % charset_size;
        str[i] = charset[key];
    }
    str[length - 1] = '\0';
}

static void service_request_gen(int argc, char *argv[]) {
    if (argc < 2) {
        help(1);
    }
    char *input_file_path = argv[0];
    int num_iterations = atoi(argv[1]);

    srand(time(NULL));
    FILE *file = fopen(input_file_path, "a");
    if (file == NULL) {
        perror("Failed to open file");
        return;
    }
    for (int i = 0; i < num_iterations; i++) {
        size_t random_length = 5 + rand() % (20 - 5 + 1);
        char *random_string = (char *)malloc(random_length + 1);
        if (random_string == NULL) {
            perror("Failed to allocate memory for random string");
            fclose(file);
            return;
        }
        generate_random_string(random_string, random_length);
        fprintf(file, "1 \"%s\" 0\n", random_string);
        free(random_string);
    }
    fprintf(file, "0\n");
    fclose(file);
}
int test(int argc, char *argv[]) {
    int fd;
    int ret;
    shm_args_t args;
    int a;
    int b;
    // Open hvisor device
    fd = open("/dev/hvisor", O_RDWR);
    if (fd < 0) {
        printf("Failed to open /dev/hvisor: %s\n", strerror(errno));
        return -1;
    }
    
    // Prepare arguments
    args.target_zone_id = strtoul(argv[0], NULL, 10); 
    args.service_id = strtoul(argv[1], NULL, 10);    
    args.swi = 0; // Reserved for future use

    // sscanf(argv[0], "%d", &a);
    // sscanf(argv[1], "%d", &b);
    // printf("a: %d, b: %d\n", a, b);
    // Send ioctl
    ret = ioctl(fd, HVISOR_SHM_SIGNAL, &args);
    if (ret < 0) {
        printf("HVISOR_SHM_SIGNAL ioctl failed: %s\n", strerror(errno));
        close(fd);
        return -1;
    }
    
    close(fd);
    printf("Successfully sent SHM signal to zone %lu with service_id %lu\n", 
           args.target_zone_id, args.service_id);
    return 0;
}




void print_timestamp(uint64_t ns)
{
    uint64_t sec = ns / 1000000000ULL;
    uint64_t nsec = ns % 1000000000ULL;
    printf("%lu.%09lu", sec, nsec);
}

int shm_receiver() {
    int fd;
    struct pollfd pfd;
    int ret;
    shm_signal_info_t signal_info;
    int last_signal_count = 0;
    
    printf("SHM-based SHM Signal Receiver\n");
    printf("=============================\n");
    
    // 安装信号处理程序
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    // 打开 SHM 设备（使用第一个 SHM 设备）
    fd = open("/dev/hshm0", O_RDONLY);
    if (fd < 0) {
        perror("Failed to open /dev/hshm0");
        printf("Make sure the SHM driver is loaded and SHM is configured\n");
        return 1;
    }
    
    printf("Monitoring SHM signals via SHM driver (Press Ctrl+C to exit)...\n\n");
    
    // 设置 poll 结构
    pfd.fd = fd;
    pfd.events = POLLIN;
    
    while (running) {
        // 轮询设备状态
        ret = poll(&pfd, 1, 1000); // 1 秒超时
        
        if (ret < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("poll failed");
            break;
        }
        
        // 获取 SHM 信号信息
        ret = ioctl(fd, HVISOR_SHM_SIGNAL_INFO, &signal_info);
        if (ret < 0) {
            perror("Failed to get SHM signal info");
            continue;
        }
        
        // 检查是否有新的信号
        if (signal_info.signal_count > last_signal_count) {
            printf("New SHM signals detected!\n");
            printf("  Total signals: %u (new: %u)\n", 
                   signal_info.signal_count, 
                   signal_info.signal_count - last_signal_count);
            printf("  Last timestamp: ");
            print_timestamp(signal_info.last_timestamp);
            printf("\n");
            printf("  Last service ID: %u\n", signal_info.last_service_id);
            printf("  Current CPU: %u\n", signal_info.current_cpu);
            printf("\n");
            
            last_signal_count = signal_info.signal_count;
            
            // 在这里添加你的自定义处理逻辑
            // 例如：
            // - 处理不同的 service_id
            // - 读取共享内存数据
            // - 向 root linux 发送响应
        }
        
        // 检查是否有 IVC 事件
        if (pfd.revents & POLLIN) {
            printf("IVC event detected (possibly SHM signal)\n");
        }
        
        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            printf("Device error or disconnection\n");
            break;
        }
    }
    
    close(fd);
    printf("\nReceived total %u SHM signals\n", signal_info.signal_count);
    printf("Exiting gracefully\n");
    
    return 0;
}
static int hyper_amp_service_test(char* shm_json_path) {
    printf("=== Non-Root Linux SHM Read Test ===\n");
    printf("Initializing as SHM server to read client data...\n");
    
    // 打开性能日志文件
    FILE* perf_log = fopen("service_performance.log", "w");
    if (perf_log == NULL) {
        printf("[Warning] Failed to open performance log file: %s\n", strerror(errno));
        printf("[Info] Performance data will not be saved to file\n");
    } else {
        // 写入表头
        fprintf(perf_log, "Message编号\t服务处理时间\n");
        fflush(perf_log);
        printf("[Info] Performance log file created: service_performance.log\n");
    }
    
    // step 1: 解析地址配置 (从JSON获取动态地址)
    parse_global_addr(shm_json_path);
    printf("[Test] Address configuration parsed\n");
    
    // step 2: 映射共享内存地址 (类似 channels_init 中的 mmap)
    // addr_infos[0] = buf, addr_infos[1] = zone0_msg, addr_infos[2] = zonex_msg
    
    // 打开 /dev/mem 来映射物理内存
    printf("[Test] Opening /dev/mem...\n");
    int mem_fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (mem_fd < 0) {
        printf("[Error] Failed to open /dev/mem: %s (errno=%d)\n", strerror(errno), errno);
        printf("[Info] Make sure you're running as root and /dev/mem is available\n");
        return -1;
    }
    printf("[Test] /dev/mem opened successfully (fd=%d)\n", mem_fd);
    
    // 映射缓冲区 (buf)
    printf("[Test] Mapping buffer: phys=0x%lx, len=0x%x\n", addr_infos[0].start, addr_infos[0].len);
    void* buf_virt = mmap(NULL, addr_infos[0].len, PROT_READ | PROT_WRITE, 
                         MAP_SHARED, mem_fd, addr_infos[0].start);
    if (buf_virt == MAP_FAILED) {
        printf("[Error] Failed to map buffer memory: %s\n", strerror(errno));
        close(mem_fd);
        return -1;
    }
    printf("[Test] Buffer mmap successful: %p\n", buf_virt);
    
    // 映射消息队列 - 测试：同时映射两个队列来调试
    printf("[Test] Mapping Non-Root Linux msg queue: phys=0x%lx, len=0x%x\n", addr_infos[2].start, addr_infos[2].len);
    void* msg_queue_virt = mmap(NULL, addr_infos[2].len, PROT_READ | PROT_WRITE,
                               MAP_SHARED, mem_fd, addr_infos[2].start);
    
    // 同时映射 Root Linux 的队列用于调试
    printf("[Test] Also mapping Root Linux msg queue for debugging: phys=0x%lx, len=0x%x\n", addr_infos[1].start, addr_infos[1].len);
    void* root_msg_queue_virt = mmap(NULL, addr_infos[1].len, PROT_READ | PROT_WRITE,
                                    MAP_SHARED, mem_fd, addr_infos[1].start);
    if (msg_queue_virt == MAP_FAILED) {
        printf("[Error] Failed to map message queue memory: %s\n", strerror(errno));
        munmap(buf_virt, addr_infos[0].len);
        close(mem_fd);
        return -1;
    }
    
    if (root_msg_queue_virt == MAP_FAILED) {
        printf("[Error] Failed to map root message queue memory: %s\n", strerror(errno));
        munmap(buf_virt, addr_infos[0].len);
        munmap(msg_queue_virt, addr_infos[2].len);
        close(mem_fd);
        return -1;
    }
    
    uint64_t buf_addr = (uint64_t)buf_virt;
    uint64_t msg_queue_addr = (uint64_t)msg_queue_virt;
    uint64_t root_msg_queue_addr = (uint64_t)root_msg_queue_virt;
    
    printf("[Test] Buffer mapped: 0x%lx -> 0x%lx (len: 0x%x)\n", 
           addr_infos[0].start, buf_addr, addr_infos[0].len);
    printf("[Test] Non-Root Linux MSG queue mapped: 0x%lx -> 0x%lx (len: 0x%x)\n", 
           addr_infos[2].start, msg_queue_addr, addr_infos[2].len);
    printf("[Test] Root Linux MSG queue mapped: 0x%lx -> 0x%lx (len: 0x%x)\n", 
           addr_infos[1].start, root_msg_queue_addr, addr_infos[1].len);
    
    if (msg_queue_addr == 0 || buf_addr == 0 || root_msg_queue_addr == 0) {
        printf("[Error] Failed to get required addresses\n");
        return -1;
    }
    
    // step 3: 简单初始化消息队列 (类似 channels_init())
    printf("[Test] Initializing message queue...\n");
    struct AmpMsgQueue* msg_queue = (struct AmpMsgQueue*)msg_queue_addr;
    
    printf("[Test] Before init - working_mark: 0x%x\n", msg_queue->working_mark);
    printf("[Test] Current buf_size: %u\n", msg_queue->buf_size);
    printf("[Test] Current empty_h: %u, wait_h: %u, proc_ing_h: %u\n", 
           msg_queue->empty_h, msg_queue->wait_h, msg_queue->proc_ing_h);
    
    // 设置为已初始化状态 - 这是关键步骤！
    msg_queue->working_mark = INIT_MARK_INITIALIZED;
    
    printf("[Test] After init - working_mark: 0x%x\n", msg_queue->working_mark);
    printf("[Test] Message queue marked as initialized!\n");
    
    // step 4: 显示消息队列状态
    printf("[Test] Non-Root Linux Message Queue Info:\n");
    printf("  Buffer size: %u\n", msg_queue->buf_size);
    printf("  Empty head: %u\n", msg_queue->empty_h);
    printf("  Wait head: %u\n", msg_queue->wait_h);
    printf("  Processing head: %u\n", msg_queue->proc_ing_h);
    
    // 同时检查 Root Linux 队列状态
    struct AmpMsgQueue* root_msg_queue = (struct AmpMsgQueue*)root_msg_queue_addr;
    printf("[Test] Root Linux Message Queue Info:\n");
    printf("  Working mark: 0x%x\n", root_msg_queue->working_mark);
    printf("  Buffer size: %u\n", root_msg_queue->buf_size);
    printf("  Empty head: %u\n", root_msg_queue->empty_h);
    printf("  Wait head: %u\n", root_msg_queue->wait_h);
    printf("  Processing head: %u\n", root_msg_queue->proc_ing_h);
    
    // step 5: 计算消息实体数组的起始地址 (类似 get_msg_entries_start_addr())
    uint64_t msg_entries_addr = msg_queue_addr + sizeof(struct AmpMsgQueue);
    uint64_t root_msg_entries_addr = root_msg_queue_addr + sizeof(struct AmpMsgQueue);
    printf("[Test] Non-Root message entries start at: 0x%lx\n", msg_entries_addr);
    printf("[Test] Root message entries start at: 0x%lx\n", root_msg_entries_addr);
    
    // step 6: 验证共享缓冲区可访问性 (安全检查)
    char* test_buf = (char*)buf_addr;
    printf("[Test] Testing buffer access...\n");
    printf("[Test] buf_addr = 0x%lx, len = 0x%x\n", buf_addr, addr_infos[0].len);
    
    // 更安全的测试 - 先尝试读取，再尝试写入
    if (buf_addr != 0 && addr_infos[0].len > 100) {
        printf("[Test] Attempting to read from buffer...\n");
        volatile char first_byte = test_buf[0];  // 测试读取
        printf("[Test] First byte read successful: 0x%02x\n", first_byte);
        
        printf("[Test] Attempting to write to buffer...\n");
        test_buf[0] = 'T';  // 测试单字节写入
        test_buf[1] = 'e';
        test_buf[2] = 's';
        test_buf[3] = 't';
        test_buf[4] = '\0';
        printf("[Test] Single byte write successful\n");
        
        printf("[Test] Buffer test: \"%s\"\n", test_buf);
    } else {
        printf("[Test] Buffer access skipped due to safety concerns (addr=0x%lx, len=0x%x)\n", 
               buf_addr, addr_infos[0].len);
    }
    
    // step 7: 监听并处理消息 (结合中断机制)
    printf("\n[Test] Waiting for messages from Root Linux...\n");
    printf("Press Ctrl+C to exit\n");
    //暂时用不到该功能running默认为1
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    // 打开SHM中断监听设备
    printf("[Test] Opening /dev/hshm0 for interrupt monitoring...\n");
    int shm_fd = open("/dev/hshm0", O_RDONLY);
    if (shm_fd < 0) {
        printf("[Warning] Failed to open /dev/hshm0: %s\n", strerror(errno));
        printf("[Info] Will use polling mode instead of interrupt mode\n");
    } else {
        printf("[Test] /dev/hshm0 opened successfully for interrupt monitoring\n");
    }
    
    struct pollfd pfd;
    if (shm_fd >= 0) {
        pfd.fd = shm_fd;
        pfd.events = POLLIN;
    }
    
    int msg_count = 0;
    int check_counter = 0;
    
    // 获取 ARM64 计时器频率
    uint64_t timer_freq = get_cntfrq();
    printf("[PERF] ARM64 Timer Frequency: %lu Hz (%.2f MHz)\n", 
           timer_freq, timer_freq / 1000000.0);
    printf("[PERF] Timer Precision: %.2f ns per tick\n\n", 
           get_timer_precision_ns(timer_freq));

    while (running) {
        bool should_check_messages = false;
        uint64_t service_ticks_start = 0;  // 服务端接收中断的时间戳
        
        // 如果有中断设备，先检查中断
        if (shm_fd >= 0) {
            int ret = poll(&pfd, 1, -1); // 程序阻塞，等待中断
            
            // 记录服务端接收到中断的时间戳（使用高精度计时器）
            service_ticks_start = get_cntpct();
            
            if (ret > 0 && (pfd.revents & POLLIN)) {//PLOLLIN事件chunk
                printf("\033[2K\r");  // 清除当前行并回到行首
                printf("[Test] SHM interrupt received (tick=%lu), checking for messages...\n", 
                       service_ticks_start);
                should_check_messages = true;

            }
        }
        // 中断触发检查
        if (should_check_messages) {
            // 首先检查 Non-Root Linux 自己的队列
            bool found_message = false;
          
            // if (msg_queue->proc_ing_h < msg_queue->buf_size) {
            //     printf("\n[Test] *** MESSAGE DETECTED IN NON-ROOT QUEUE *** Processing message #%d\n", ++msg_count);
            //     found_message = true;
            //     // 处理 Non-Root 队列消息的逻辑...
            //     // (保留原有逻辑)
            // }
            // 关键修改：检查并处理 Root Linux 队列中的消息
            if (!found_message && root_msg_queue->proc_ing_h < root_msg_queue->buf_size) {
                printf("\n[Test] *** PROCESSING MESSAGE FROM ROOT LINUX QUEUE *** Message #%d\n", ++msg_count);
                found_message = true;
                
                // 处理 Root Linux 队列中的消息
                uint16_t head = root_msg_queue->proc_ing_h;
                uint16_t msg_index = head;
                
                // 计算当前消息实体的地址
                uint64_t msg_entry_addr = root_msg_entries_addr + sizeof(struct MsgEntry) * head;
                struct MsgEntry* msg_entry = (struct MsgEntry*)msg_entry_addr;
                struct Msg* msg = &msg_entry->msg;
                
                printf("  Root Message details:\n");
                printf("    Index: %u\n", msg_index);
                printf("    Service ID: %u\n", msg->service_id);
                printf("    Offset: 0x%x\n", msg->offset);
                printf("    Length: %u\n", msg->length);
                printf("    Deal state: %u\n", msg->flag.deal_state);
                
                // 从共享内存读取数据
                if (msg->length > 0) {
                    char* data_ptr = (char*)(buf_addr + msg->offset);
                    printf("    Reading from buf_addr=0x%lx + offset=0x%x = 0x%lx\n", 
                           buf_addr, msg->offset, (uint64_t)data_ptr);
                    // 安全地显示数据内容 - 逐字节打印
                    printf("    *** DATA FROM ROOT LINUX: [");
                    for (int i = 0; i < msg->length && i < 32; i++) {  // 最多显示32字节
                        if (data_ptr[i] >= 32 && data_ptr[i] <= 126) {  // 可打印字符
                            printf("%c", data_ptr[i]);
                        } else {
                            printf("\\x%02x", (unsigned char)data_ptr[i]);
                        }
                    }
                    if (msg->length > 32) printf("...");
                    printf("] (%u bytes) ***\n", msg->length);                    
                    // ===== HyperAMP 安全服务处理 =====
                    printf("    Processing HyperAMP secure service (Service ID: %u)...\n", msg->service_id);
                    
                    int service_result = MSG_SERVICE_RET_SUCCESS;
                    bool data_modified = false;
                    
                    // 根据 service_id 执行相应的安全服务
                    switch (msg->service_id) {
                        case 1:  // 加密服务
                            printf("    [HyperAMP] Executing ENCRYPTION service\n");
                            if (hyperamp_encrypt_service(data_ptr, msg->length - 1, msg->length) == 0) {
                                printf("    [HyperAMP] Encryption completed successfully\n");
                                data_modified = true;
                            } else {
                                printf("    [HyperAMP] Encryption failed\n");
                                service_result = MSG_SERVICE_RET_FAIL;
                            }
                            break;
                            
                        case 2:  // 解密服务
                            printf("    [HyperAMP] Executing DECRYPTION service\n");
                            if (hyperamp_decrypt_service(data_ptr, msg->length - 1, msg->length) == 0) {
                                printf("    [HyperAMP] Decryption completed successfully\n");
                                data_modified = true;
                            } else {
                                printf("    [HyperAMP] Decryption failed\n");
                                service_result = MSG_SERVICE_RET_FAIL;
                            }
                            break;
                            
                        case 66:  // 测试服务 (Echo)
                            printf("    [HyperAMP] Executing ECHO test service\n");
                            // Echo服务不修改数据，只是测试通信
                            break;
                            
                        default:
                            printf("    [HyperAMP] Unknown service ID: %u, treating as echo\n", msg->service_id);
                            break;
                    }
                    
                    // 如果数据被修改，显示处理后的结果
                    if (data_modified) {
                        printf("    *** PROCESSED DATA: [");
                        for (int i = 0; i < msg->length && i < 32; i++) {
                            if (data_ptr[i] >= 32 && data_ptr[i] <= 126) {
                                printf("%c", data_ptr[i]);
                            } else {
                                printf("\\x%02x", (unsigned char)data_ptr[i]);
                            }
                        }
                        if (msg->length > 32) printf("...");
                        printf("] ***\n");
                    }
                    
                    // 标记消息已处理
                    printf("    Setting deal_state to MSG_DEAL_STATE_YES...\n");
                    msg->flag.deal_state = MSG_DEAL_STATE_YES;
                    msg->flag.service_result = service_result;
                    
                    
                } else {
                    printf("    No data to read (length=%u)\n", msg->length);
                    msg->flag.deal_state = MSG_DEAL_STATE_YES;
                    msg->flag.service_result = MSG_SERVICE_RET_FAIL;
                }
                // 更新 Root Linux 队列头
                uint16_t new_head = msg_entry->nxt_idx;
                root_msg_queue->proc_ing_h = new_head;
                msg_entry->nxt_idx = root_msg_queue->buf_size; // 标记为无效
                // 重置工作状态，允许下一次通信
                root_msg_queue->working_mark = MSG_QUEUE_MARK_IDLE;
                printf("    Updated Root Linux proc_ing_h: %u -> %u\n", head, new_head);
                printf("    Reset working_mark to IDLE (0x%x)\n", MSG_QUEUE_MARK_IDLE);
                printf("    Root Linux should now detect the state change and stop polling\n");
                
                // ==============================================
                // 高精度性能测试：记录服务端处理结束时间并计算处理时长
                // ==============================================
                uint64_t service_ticks_end = get_cntpct();
                
                uint64_t service_time_us = ticks_to_us(service_ticks_start, service_ticks_end, timer_freq);
                uint64_t service_time_ns = ticks_to_ns(service_ticks_start, service_ticks_end, timer_freq);
                    
                printf("\n=== HYPERAMP SERVICE HIGH-PRECISION PERFORMANCE ===\n");
                printf("[PERF-SERVICE] Processing Time: %lu μs (%.3f ms) [%lu ns]\n", 
                       service_time_us, service_time_us / 1000.0, service_time_ns);
                printf("[PERF-SERVICE] Message Size: %u bytes\n", msg->length);
                printf("[PERF-SERVICE] Service ID: %u\n", msg->service_id);
                if (service_time_us > 0) {
                    printf("[PERF-SERVICE] Throughput: %.2f bytes/sec (%.2f KB/sec)\n", 
                           msg->length * 1000000.0 / service_time_us,
                           msg->length * 1000.0 / service_time_us);
                }
                printf("[PERF-SERVICE] Ticks: start=%lu, end=%lu, diff=%lu\n",
                       service_ticks_start, service_ticks_end, 
                       service_ticks_end - service_ticks_start);
                printf("===================================================\n\n");
                
                // 保存性能数据到文件
                if (perf_log != NULL) {
                    fprintf(perf_log, "%d\t%.3f ms\n", msg_count, service_time_us / 1000.0);
                    fflush(perf_log);  // 立即刷新到文件
                }
                
                printf("    *** HYPERAMP SERVICE COMPLETED! ***\n");
            }
            if (!found_message && check_counter % 50 == 0) {
                printf("\033[2K\r");  // 清除当前行并回到行首 
                // 每5秒显示一次等待状态，添加换行和刷新缓冲
                printf("[Test] Waiting... (Non-Root:%u, Root:%u)\n", 
                       msg_queue->proc_ing_h, root_msg_queue->proc_ing_h);
                fflush(stdout);  // 强制刷新输出缓冲区
            }
        }
        
    }
    
    printf("\n[Test] SHM read test completed. Processed %d messages.\n", msg_count);
    
    // 关闭性能日志文件
    if (perf_log != NULL) {
        fclose(perf_log);
        printf("[Info] Performance data saved to service_performance.log\n");
    }
    
    // step 8: 清理资源
    if (shm_fd >= 0) {
        close(shm_fd);
        printf("[Test] Closed /dev/hshm0\n");
    }
    munmap((void*)buf_addr, addr_infos[0].len);
    munmap((void*)msg_queue_addr, addr_infos[2].len);
    munmap((void*)root_msg_queue_addr, addr_infos[1].len);
    close(mem_fd);
    
    return 0;
}

static int hyper_amp_client_test(int argc, char* argv[]) {
    // 参数检查
    if (argc < 3) {
        printf("Usage: ./hvisor shm hyper_amp_test <shm_json_path> <data|@filename> <service_id>\n");
        printf("Examples:\n");
        printf("  ./hvisor shm hyper_amp_test shm_config.json \"hello world\" 1\n");
        printf("  ./hvisor shm hyper_amp_test shm_config.json @data.txt 2\n");
        printf("  ./hvisor shm hyper_amp_test shm_config.json hex:48656c6c6f 2  (hex input)\n");
        return -1;
    }
    
    char* shm_json_path = argv[0];
    char* data_input = argv[1];
    uint32_t service_id = (argc >= 3) ? strtoul(argv[2], NULL, 10) : NPUCore_SERVICE_ECHO_ID;
    // 数据处理：支持直接字符串或从文件读取
    char* data_buffer = NULL;
    int data_size = 0;
    // 直接使用字符串
    data_size = strlen(data_input);
    data_buffer = malloc(data_size + 1);
    if (data_buffer == NULL) {
        printf("Error: Memory allocation failed\n");
        return -1;
    }
    strcpy(data_buffer, data_input);
    printf("Using input string: \"%s\" (%d bytes)\n", data_buffer, data_size);
    parse_global_addr(shm_json_path);
    //初始化客户端
    struct Client amp_client = { 0 };
    if (client_ops.client_init(&amp_client, ZONE_NPUcore_ID) != 0)
    {
        printf("error: client init failed\n");
        free(data_buffer);
        return -1;
    }
    // printf("info: client init success\n");
    //获取空闲消息
    struct Msg *msg = client_ops.empty_msg_get(&amp_client, service_id);
    if (msg == NULL)
    {
        printf("error : empty msg get [service_id = %u]\n", service_id);
        free(data_buffer);
        return -1;
    }

    // 重置消息
    msg_ops.msg_reset(msg);
    
    // 分配共享内存
    char* shm_data = (char*)client_ops.shm_malloc(&amp_client, data_size + 1, MALLOC_TYPE_P);
    if (shm_data == NULL)
    {
        // printf("info: MALLOC_TYPE_P failed, trying MALLOC_TYPE_V...\n");
        shm_data = (char*)client_ops.shm_malloc(&amp_client, data_size + 1, MALLOC_TYPE_V);
    }
    if (shm_data == NULL)
    {
        printf("error : shm malloc failed [size = %u]\n", data_size + 1);
        free(data_buffer);
        return -1;
    }
    
    
    // 复制数据到共享内存
    // memcpy(shm_data, data_buffer, data_size);
    // shm_data[data_size] = '\0';

    for (int j = 0; j < data_size; j++) {
        shm_data[j] = data_buffer[j];
    }
    shm_data[data_size] = '\0';

    // 设置消息
    msg->offset = client_ops.shm_addr_to_offset(shm_data);
    msg->length = data_size + 1;

    // ==============================================
    // 高精度性能测试：使用 ARM64 System Counter
    // ==============================================
    
    // 获取计数器频率（只需读取一次）
    uint64_t timer_freq = get_cntfrq();
    printf("\n[PERF] ARM64 Timer Frequency: %lu Hz (%.2f MHz)\n", 
           timer_freq, timer_freq / 1000000.0);
    printf("[PERF] Timer Precision: %.2f ns per tick\n", 
           get_timer_precision_ns(timer_freq));
    
    // 记录发送前的时间戳（硬件计数器）
    uint64_t ticks_start = get_cntpct();

    // 发送消息并通知
    if (client_ops.msg_send_and_notify(&amp_client, msg) != 0)
    {
        printf("error : msg send failed [offset = 0x%x, length = %u]\n", msg->offset, msg->length);
        free(data_buffer);
        return -1;
    }

    // ==============================================
    // 性能测试：轮询计数和中间时间点
    // ==============================================
    uint64_t ticks_poll_start = get_cntpct();
    int poll_count = 0;
    
    while(client_ops.msg_poll(msg) != 0) {
        // 轮询等待响应
        poll_count++;
    }
    
    // 记录响应接收完成的时间戳
    uint64_t ticks_end = get_cntpct();

    // 计算各项延迟（微秒）
    uint64_t latency_us = ticks_to_us(ticks_start, ticks_end, timer_freq);
    uint64_t poll_time_us = ticks_to_us(ticks_poll_start, ticks_end, timer_freq);
    uint64_t send_to_poll_us = ticks_to_us(ticks_start, ticks_poll_start, timer_freq);
    
    // 计算纳秒级延迟（更高精度）
    uint64_t latency_ns = ticks_to_ns(ticks_start, ticks_end, timer_freq);
                        
    printf("\n=== HYPERAMP HIGH-PRECISION PERFORMANCE RESULTS ===\n");
    printf("[PERF] Total Round-Trip Latency: %lu μs (%.3f ms) [%lu ns]\n", 
           latency_us, latency_us / 1000.0, latency_ns);
    printf("[PERF] Send-to-Poll Time: %lu μs (%.3f ms)\n", 
           send_to_poll_us, send_to_poll_us / 1000.0);
    printf("[PERF] Polling Time: %lu μs (%.3f ms)\n", 
           poll_time_us, poll_time_us / 1000.0);
    printf("[PERF] Poll Count: %d iterations\n", poll_count);
    
    // 计算吞吐量
    if (latency_us > 0) {
        printf("[PERF] Throughput: %.2f bytes/sec (%.2f KB/sec)\n", 
               data_size * 1000000.0 / latency_us,
               data_size * 1000.0 / latency_us);
    }
    
    // 计算 ticks 信息（调试用）
    printf("[PERF] Ticks: start=%lu, end=%lu, diff=%lu\n",
           ticks_start, ticks_end, ticks_end - ticks_start);
    // 计算 ticks 信息（调试用）
    printf("[PERF] Ticks: start=%lu, end=%lu, diff=%lu\n",
           ticks_start, ticks_end, ticks_end - ticks_start);
    
    // 性能评估
    if (latency_us <= 10000) {  // 10ms
        printf("[PERF] Latency Status: 🟢 EXCELLENT (≤10ms)\n");
    } else if (latency_us <= 50000) {  // 50ms  
        printf("[PERF] Latency Status: 🟡 GOOD (≤50ms)\n");
    } else if (latency_us <= 100000) {  // 100ms
        printf("[PERF] Latency Status: 🟠 ACCEPTABLE (≤100ms)\n");
    } else {
        printf("[PERF] Latency Status: 🔴 NEEDS OPTIMIZATION (>100ms)\n");
    }
    printf("===================================================\n\n");

        // 读取处理后的结果数据
    if (msg->flag.service_result == MSG_SERVICE_RET_SUCCESS) {
        printf("=== HyperAMP Service Result ===\n");
        
        // 获取处理后的实际数据大小（可能与输入大小不同）
        int result_data_size = msg->length > 0 ? msg->length - 1 : 0; // 减去末尾的 null terminator
        if (result_data_size <= 0) {
            result_data_size = data_size; // 回退到原始大小
        }

        if (service_id == 1) {
            printf("Encryption completed. Encrypted data:\n");
        } else if (service_id == 2) {
            printf("Decryption completed. Decrypted data:\n");
        } else {
            printf("Service %u completed. Result data:\n", service_id);
        }
        // 确定显示长度：小于等于256字节全部显示，超过则显示前64字节
        int display_length = result_data_size;
        bool truncated = false;
        if (result_data_size > 256) {
            display_length = 64;
            truncated = true;
        }
        
        // 生成输出文件名
        char output_filename[256];
        if (service_id == 1) {
            snprintf(output_filename, sizeof(output_filename), "encrypted_result.txt");
        } else if (service_id == 2) {
            snprintf(output_filename, sizeof(output_filename), "decrypted_result.txt");
        } else {
            snprintf(output_filename, sizeof(output_filename), "service_%u_result.txt", service_id);
        }
        
        // 打开文件准备保存
        FILE* output_file = fopen(output_filename, "wb");        
        // 安全地显示处理后的数据 -使用动态显示长度
        printf("Result: [");
        for (int i = 0; i < display_length; i++) {
            // 同时写入文件（如果文件打开成功）
            if (output_file != NULL) {
                fputc(shm_data[i], output_file);
            }
            
            if (shm_data[i] >= 32 && shm_data[i] <= 126) {  // 可打印字符
                printf("%c", shm_data[i]);
            } else if (shm_data[i] == '\n') {  // 换行符特殊处理
                printf("\\n");
            } else if (shm_data[i] == '\r') {  // 回车符特殊处理
                printf("\\r");
            } else if (shm_data[i] == '\t') {  // 制表符特殊处理
                printf("\\t");
            } else {
                printf("\\x%02x", (unsigned char)shm_data[i]);
            }
        }       
        if (truncated) {
            printf("... (showing first %d of %d bytes)", display_length, result_data_size);
        }
        printf("] (%d bytes)\n", result_data_size);
        
        // 显示十六进制格式 - 同样使用动态显示长度
        printf("Hex format: ");
        for (int i = 0; i < display_length; i++) {
            printf("%02x", (unsigned char)shm_data[i]);
        }
        if (truncated) {
            printf("... (showing first %d of %d bytes)", display_length, result_data_size);
        }
        printf("\n");
        // 如果数据被截断，提示查看完整内容的方法
        if (truncated) {
            printf("Note: Large data truncated for display. Full data saved to file.\n");
        }        
        // 如果是加密服务，提供解密命令提示 - 添加安全检查，使用实际结果大小
        if (service_id == 1 && result_data_size > 0 && result_data_size <= 64) {
            printf("\nTo decrypt, use: ./hvisor shm hyper_amp_test %s hex:", shm_json_path);
            for (int i = 0; i < result_data_size; i++) {
                printf("%02x", (unsigned char)shm_data[i]);
            }
            printf(" 2\n");
            printf("Or from file: ./hvisor shm hyper_amp_test %s @%s 2\n", shm_json_path, output_filename);
        } else if (service_id == 1 && result_data_size > 64) {
            printf("\nData too large for command line hex display. Use file input:\n");
            printf("./hvisor shm hyper_amp_test %s @%s 2\n", shm_json_path, output_filename);
        }
        
        printf("===============================\n");
    } else {
        printf("error : HyperAMP service failed [service_id = %u]\n", service_id);
    }
    
    // 注意：跳过 shm_free 因为在 HVisor 环境中会导致段错误
    // client_ops.shm_free(&amp_client, shm_data);
    
    printf("info : SHM test completed successfully\n");
    
    // 清理资源
    client_ops.empty_msg_put(&amp_client, msg);
    client_ops.client_destory(&amp_client);
    
    // 清理分配的数据缓冲区
    free(data_buffer);
    
    return 0;
}


int main(int argc, char *argv[]) {
    int err = 0;

    if (argc < 2) {
        help(1);
    }   
    if (strcmp(argv[1], "zone") == 0) {
        if (argc < 3) {
            help(1);
        }
        if (strcmp(argv[2], "start") == 0) {
            err = zone_start(argc, argv);
        } else if (strcmp(argv[2], "shutdown") == 0) {
            err = zone_shutdown(argc - 3, &argv[3]);
        } else if (strcmp(argv[2], "list") == 0) {
            err = zone_list(argc - 3, &argv[3]);
        } else {
            help(1);
        }
    } else if (strcmp(argv[1], "virtio") == 0) {
        if (argc < 3) {
            help(1);
        }
        if (strcmp(argv[2], "start") == 0) {
            err = virtio_start(argc, argv);
        } else {
            help(1);
        }
    }
    else if (strcmp(argv[1], "start_exception_trace") == 0)
    {
        err = start_exception_trace();
    }
    else if (strcmp(argv[1], "end_exception_trace") == 0)
    {
        err = end_exception_trace();
    }
    // shared memory
    else if (strcmp(argv[1], "shm") == 0) {
        if (argc < 3) {
            help(1);
        }
        if (strcmp(argv[2], "test_signal") == 0) {   
            // hvisor shm test_signal -id <dst_zone_id>
            err = test_shm_signal(argc - 3, &argv[3]);
        }
        else if(strcmp(argv[2], "test_shm") == 0) {
            // hvisor shm test_shm <shm_json_path> <data|@filename> <service_id>
            if (argc < 5) {
                printf("Usage: hvisor shm test_shm <shm_json_path> <data|@filename> <service_id>\n");
                printf("Examples:\n");
                printf("  hvisor shm test_shm shm_config.json \"hello world\" 1\n");
                printf("  hvisor shm test_shm shm_config.json @data.txt 2\n");
                return -1;
            }
            test_shm(argc - 3, &argv[3]);
        } 
        else if(strcmp(argv[2], "safe_service") == 0) { 
            // hvisor shm safe_service <shm_json_path> <service_id> <data> <data_size>
            safe_service(argc - 3, &argv[3]);
        } 
        else if(strcmp(argv[2], "show_safe_service") == 0) {
            // hvisor shm show_safe_service
            print_services();
        }
        else if(strcmp(argv[2], "setup_shm_client") == 0) {
            // hvisor shm setup_shm_client <shm_json_path> <input.txt> <output_dir> <threads>
            setup_shm_client(argc - 3, &argv[3]);
        }
        else if(strcmp(argv[2], "setup_request_gen") == 0) {
            // hvisor shm request_gen <input_file_path> <num_iterations>
            service_request_gen(argc - 3, &argv[3]);
        }
        else if(strcmp(argv[2], "test") == 0) {
            // hvisor shm test 1 0 
            test(argc-3, &argv[3]);
        }
        else if(strcmp(argv[2], "receiver") == 0) {
            // hvisor shm receiver 
            shm_receiver();
        }
        else if(strcmp(argv[2], "server") == 0) {
            // hvisor shm server <shm_json_path>
            if (argc < 4) {
                printf("Usage: hvisor shm server <shm_json_path>\n");
                printf("Example: hvisor shm server /path/to/shm_config.json\n");
                help(1);
            }
            shm_server(argv[3]);
        }
        else if(strcmp(argv[2], "hyper_amp_service") == 0) {
            // hvisor shm hyper_amp_service <shm_json_path>
            if (argc < 4) {
                printf("Usage: ./hvisor shm hyper_amp_service <shm_json_path>\n");
                printf("Example: ./hvisor shm hyper_amp_service /path/to/shm_config.json\n");
                help(1);
            }
            hyper_amp_service(argv[3]);
        }
        else if(strcmp(argv[2], "hyper_amp") == 0) {
            // hvisor shm hyper_amp <shm_json_path> <data|@filename> <service_id>
            if (argc < 5) {
                printf("Usage: ./hvisor shm hyper_amp <shm_json_path> <data|@filename> <service_id>\n");
                printf("Examples:\n");
                printf("  ./hvisor shm hyper_amp shm_config.json \"hello world\" 1\n");
                printf("  ./hvisor shm hyper_amp shm_config.json @data.txt 2\n");
                return -1;
            }
            hyper_amp_client(argc - 3, &argv[3]);
        }
        else if(strcmp(argv[2], "hyper_amp_service_test") == 0) {
            // hvisor shm hyper_amp_service <shm_json_path>
            if (argc < 4) {
                printf("Usage: ./hvisor shm hyper_amp_service_test <shm_json_path>\n");
                printf("Example: ./hvisor shm hyper_amp_service_test shm_config.json\n");
                help(1);
            }
            hyper_amp_service_test(argv[3]);
        }
        else if(strcmp(argv[2], "hyper_amp_test") == 0) {
            // hvisor shm hyper_amp <shm_json_path> <data|@filename> <service_id>
            if (argc < 5) {
                printf("Usage: ./hvisor shm hyper_amp_test <shm_json_path> <data|@filename> <service_id>\n");
                printf("Examples:\n");
                printf("  ./hvisor shm hyper_amp_test shm_config.json \"hello world\" 1\n");
                printf("  ./hvisor shm hyper_amp_test shm_config.json @data.txt 2\n");
                return -1;
            }
            hyper_amp_client_test(argc - 3, &argv[3]);
        }
        // ========== QoS命令 ==========
        else if(strcmp(argv[2], "hyper_amp_qos") == 0) {
            // hvisor shm hyper_amp_qos <shm_json_path> <data|@filename> <service_id>
            if (argc < 5) {
                printf("Usage: ./hvisor shm hyper_amp_qos <shm_json_path> <data|@filename> <service_id>\n");
                printf("Examples:\n");
                printf("  ./hvisor shm hyper_amp_qos shm_config.json \"hello world\" 1\n");
                printf("  ./hvisor shm hyper_amp_qos shm_config.json @data.txt 2\n");
                printf("\nFeatures:\n");
                printf("  - Automatic QoS classification (REALTIME/THROUGHPUT/RELIABLE/BEST_EFFORT)\n");
                printf("  - Latency tracking and violation detection\n");
                printf("  - Performance statistics\n");
                return -1;
            }
            // 解析参数
            char* shm_json = argv[3];
            char* data_input = argv[4];
            uint32_t service_id = (uint32_t)atoi(argv[5]);
            
            err = hyper_amp_qos_client(shm_json, data_input, service_id);
        }
        else if(strcmp(argv[2], "hyper_amp_qos_service") == 0) {
            // hvisor shm hyper_amp_qos_service <shm_json_path>
            if (argc < 4) {
                printf("Usage: ./hvisor shm hyper_amp_qos_service <shm_json_path>\n");
                printf("Example: ./hvisor shm hyper_amp_qos_service shm_config.json\n");
                printf("\nFeatures:\n");
                printf("  - Three-phase batch processing\n");
                printf("  - WRR (Weighted Round Robin) scheduling\n");
                printf("  - Priority-based message processing\n");
                printf("  - Circular reference detection\n");
                printf("  - NULL safety checks\n");
                return -1;
            }
            err = hyper_amp_qos_service(argv[3]);
        }
        else if(strcmp(argv[2], "qos_stats") == 0) {
            // P0修复: 添加qos_stats命令
            // hvisor shm qos_stats
            printf("[QoS-Stats] Printing QoS statistics...\n");
            err = hyper_amp_qos_print_stats();
        }
        else {
            help(1);
        }
    }
    else {
        help(1);
    }
    return err ? 1 : 0;
}
