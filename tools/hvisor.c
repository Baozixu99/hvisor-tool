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

#include "cJSON.h"
#include "event_monitor.h"
#include "hvisor.h"
#include "log.h"
#include "safe_cjson.h"
#include "virtio.h"
#include "zone_config.h"
static void __attribute__((noreturn)) help(int exit_status) {
    printf("Hypervisor Management Tool\n\n");
    printf("Usage:\n");
    printf("  hvisor <command> [options]\n\n");
    printf("Commands:\n");
    printf("  zone start    <config.json>    Initialize an isolation zone\n");
    printf("  zone shutdown -id <zone_id>   Terminate a zone by ID\n");
    printf("  zone list                      List all active zones\n");
    printf("  virtio start  <virtio.json>    Activate virtio devices\n\n");
    printf("Options:\n");
    printf("  --id <zone_id>    Specify zone ID for shutdown\n");
    printf("  --help            Show this help message\n\n");
    printf("Examples:\n");
    printf("  Start zone:    hvisor zone start /path/to/vm.json\n");
    printf("  Shutdown zone: hvisor zone shutdown -id 1\n");
    printf("  List zones:    hvisor zone list\n");
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

static int test_shm(char* shm_json_path) {
    parse_global_addr(shm_json_path);

    struct Client amp_client = { 0 };
    if (client_ops.client_init(&amp_client, ZONE_NPUcore_ID) != 0)
    {
        printf("client init failed\n");
        while(1) {}
    }
    // printf("client init success\n");
    
    struct Msg *msg = client_ops.empty_msg_get(&amp_client, NPUCore_SERVICE_ECHO_ID);
    if (msg == NULL)
    {
        printf("error : empty msg get [service_id = %u]\n", NPUCore_SERVICE_ECHO_ID);
        return;
    }
    // printf("info : empty msg get [service_id = %u]\n", NPUCore_SERVICE_ECHO_ID);

    char data[100] = "hello world";
    int data_size = strlen(data);
    int send_count = 1;
    for (int i = 0; i < send_count; i++) {
        msg_ops.msg_reset(msg);
        // TODO: compare to OpenAMP
        // using shared memory
        char* shm_data = (char*)client_ops.shm_malloc(&amp_client, data_size + 1, MALLOC_TYPE_V);
        if (shm_data == NULL)
        {
            printf("error : shm malloc [idx = %d, size = %u, type = %u, ptr = NULL]\n", 
                i, data_size + 1, MALLOC_TYPE_V);
            return;
        }
        // printf("info : shm malloc [idx = %d, size = %u, type = %u, ptr = %p]\n", 
        //     i, data_size + 1, MALLOC_TYPE_V, shm_data);
        
        int j = 0;
        for (j = 0; j < data_size; j++)
        {
            shm_data[j] = data[j];
        }
        shm_data[data_size] = '\0';
        
        // printf("shm_data: %s, data_size = %d\n", shm_data, data_size);
        msg->offset = client_ops.shm_addr_to_offset(shm_data);
        msg->length = data_size + 1;
        // printf("info : msg fill [idx = %d, offset = 0x%x, length = %u]\n", i, msg->offset, msg->length);

        // TODO: check this offset, if necessary ? can't use virtual address?
        
        // send + notify
        if (client_ops.msg_send_and_notify(&amp_client, msg) != 0)
        {
            printf("error : msg send [idx = %d, offset = %x, length = %u], check it\n", i, msg->offset, msg->length);
            return;
        }
        // printf("info : msg send [idx = %d, offset = %x, length = %u]\n", i, msg->offset, msg->length);

        // printf("enter polling...\n");
        /* 等待消息响应 */
        // client_ops.set_client_request_cnt(&amp_client); // += 1
        // uint32_t request_cnt = client_ops.get_client_request_cnt(&amp_client);
        // while(client_ops.msg_poll(msg, request_cnt) != 0) {
            
        // }
        while(client_ops.msg_poll(msg) != 0) {
            // printf("is polling...\n");
            // sleep(3);
        }
        
        // printf("info : msg result [idx = %d, service_id = %u, result = %u]\n",
        //        i, msg->service_id, msg->flag.service_result);

        client_ops.shm_free(&amp_client, shm_data);
    }
    client_ops.empty_msg_put(&amp_client, msg);
    client_ops.client_destory(&amp_client);
}

static int check_service_id(uint32_t service_id) {
    if (service_id == 0 || service_id >= NPUCore_SERVICE_MAX_ID) {
        return -1;
    } else {
        return 0;
    }
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

#include "shm.h"
#include <sys/poll.h>
#include <sys/ioctl.h>
static volatile int running = 1;

void signal_handler(int sig)
{
    printf("Received signal %d, exiting...\n", sig);
    running = 0;
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
            // hvisor shm test_shm <shm_json_path>
            test_shm(argv[3]);
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
        else {
            help(1);
        }
    }
    else {
        help(1);
    }
    return err ? 1 : 0;
}
