#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/slab.h>
// #include <asm/io.h>
#include "hvisor.h"
#include "zone_config.h"
#include <asm/cacheflush.h>
#include <linux/gfp.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/of_reserved_mem.h>
#include <linux/sched/signal.h>
#include <linux/string.h>
#include <linux/vmalloc.h>

// addition for acpi
#include <linux/pci.h>
#include <linux/acpi.h>
#include <linux/platform_device.h>
#include <linux/irq.h>
#include <linux/irqdesc.h>

#include <acpi/acpi.h>           
#include <acpi/acpi_bus.h>       
#include <acpi/acpi_drivers.h>   
#include <acpi/actbl.h>
#include <linux/efi.h>

struct virtio_bridge *virtio_bridge;
int virtio_irq = -1;
static struct task_struct *task = NULL;

// initial virtio el2 shared region
static int hvisor_init_virtio(void) {
    int err;
    if (virtio_irq == -1) {
        pr_err("virtio device is not available\n");
        return ENOTTY;
    }
    virtio_bridge = (struct virtio_bridge *)__get_free_pages(GFP_KERNEL, 0);
    if (virtio_bridge == NULL)
        return -ENOMEM;
    SetPageReserved(virt_to_page(virtio_bridge));
    // init device region
    memset(virtio_bridge, 0, sizeof(struct virtio_bridge));
    err = hvisor_call(HVISOR_HC_INIT_VIRTIO, __pa(virtio_bridge), 0);
    if (err)
        return err;
    return 0;
}

static void test_get_free_pages(void) {
    unsigned int order = min(get_order(0x10000000), 11);

    while(1) {
        void *area = (void *)__get_free_pages(GFP_KERNEL, order);
        if (area != NULL) {
            pr_info("order : %u, area : %px, pa : %llx\n", order, area, __pa(area));
        } else {
            order--;
        }
    }
}

// order <= 11, size <= 0x1000000 (8MB)
// 0x10000000 = 0x1000000 << 16 = 256MB
static unsigned long hvisor_m_alloc(kmalloc_info_t __user *arg) {
    // test_get_free_pages();
    kmalloc_info_t kmalloc_info;

    if (copy_from_user(&kmalloc_info, arg, sizeof(kmalloc_info))) {
        pr_err("hvisor: failed to copy from user\n");
        return 0;
    }

    __u64 reduced_size = kmalloc_info.size;

    void *area;
    unsigned int order = min(get_order(reduced_size), 11);

    // try allocate from big area to small area
    area = (void *)__get_free_pages(GFP_KERNEL, order);
    while(area == NULL) {
        if (order == 0) {
            pr_err("hvisor: failed to allocate memory, size %llx\n", kmalloc_info.size);
            return 0;
        }
        order--;
        area = (void *)__get_free_pages(GFP_KERNEL, order);
    }

    reduced_size = PAGE_SIZE << order;
    
    SetPageReserved(virt_to_page(area));

    memset(area, 0, kmalloc_info.size);

    if (reduced_size < kmalloc_info.size) {
        kmalloc_info.size -= reduced_size;
    } else {
        kmalloc_info.size = 0;
    }

    kmalloc_info.pa = __pa(area);

    // copy result back to user space
    if (copy_to_user(arg, &kmalloc_info, sizeof(kmalloc_info))) {
        pr_err("hvisor: failed to copy to user\n");
        return 0;
    }

    // pr_info("allocate memory: reduced_size %llx, order %u, area %px, size %llx, pa : %llx\n",
    //     reduced_size, order, area, kmalloc_info.size, __pa(area));
    
    return 0;
}

static int hvisor_m_free(kmalloc_info_t __user *arg) {
    // TODO: check this for Memory Region of Non root Zone!
    kmalloc_info_t kmalloc_info;

    if (copy_from_user(&kmalloc_info, arg, sizeof(kmalloc_info))) {
        pr_err("hvisor: failed to copy from user\n");
        return -EFAULT;
    }

    void *area = (void *)__va(kmalloc_info.pa);
    unsigned int order = get_order(kmalloc_info.size);

    // Clear the PageReserved bit
    ClearPageReserved(virt_to_page(area));

    // Free the allocated pages
    free_pages((unsigned long)area, order);

    // pr_info("freed memory: area %px, size %llx, pa : %llx\n",
    //     area, kmalloc_info.size, kmalloc_info.pa);
    return 0;
}

// TODO: remember to free the space
// TODO: finish the gva -> hpa mapping

// ===================== helper function =====================
static void 
print_acpi_rsdp(const struct acpi_table_rsdp *rsdp)
{
    if (!rsdp) {
        pr_err("RSDP pointer is NULL\n");
        return;
    }
    pr_info("ACPI RSDP Table:\n");
    pr_info("  Signature: %.8s\n", rsdp->signature);
    pr_info("  Checksum: 0x%02x\n", rsdp->checksum);
    pr_info("  OEM ID: %.6s\n", rsdp->oem_id);
    pr_info("  Revision: %u\n", rsdp->revision);
    pr_info("  RSDT Physical Address: 0x%08x\n", rsdp->rsdt_physical_address);
    
    /* ACPI 2.0+ fields */
    if (rsdp->revision >= 2) {
        pr_info("  Length: %u bytes\n", rsdp->length);
        pr_info("  XSDT Physical Address: 0x%016llx\n", rsdp->xsdt_physical_address);
        pr_info("  Extended Checksum: 0x%02x\n", rsdp->extended_checksum);
    }
}

static void 
print_acpi_table_header(const struct acpi_table_header *header) {
    pr_info("ACPI Table Header:\n");
    pr_info("  Signature: %.4s\n", header->signature);
    pr_info("  Length: %u bytes\n", header->length);
    pr_info("  Revision: %u\n", header->revision);
    pr_info("  Checksum: 0x%02X\n", header->checksum);
    pr_info("  OEM ID: %.6s\n", header->oem_id);
    pr_info("  OEM Table ID: %.8s\n", header->oem_table_id);
    pr_info("  OEM Revision: 0x%08X\n", header->oem_revision);
    pr_info("  ASL Compiler ID: %.4s\n", header->asl_compiler_id);
    pr_info("  ASL Compiler Revision: 0x%08X\n\n", header->asl_compiler_revision);
}

// 1. allocate free pages ( >= 1)
// 2. copy old data (total pages) to new pages
// 3. return new addr (with offset)
// 4. compare the address kmalloc with the __pa, which is real physical address?
// +-------------------+
// | old  |MAP|         |
// +-------------------+
// ↑______↑
//  offset
//          +-------------------+
//          | new  |MAP|         |
//          +-------------------+
//          ↑______↑
//           offset

// three address
// 1. original pointer (physical address)
// 2. memory map address (virtual address?)
// 3. new pointer (physical address?)

// size?

// assert ori_addr != 0  (occupy part of area)
// assert ori_addr == 0  (occupy the whole area)
static __u64
hvisor_efi_general_memcpy(__u64 ori_addr, 
    void* object, int size, efi_boot_info_t *efi_boot_info) {
    // get the aligned size
    size_t aligned_size = _ALIGN_UP(size, PAGE_SIZE);

    if(aligned_size > PAGE_SIZE) {
        pr_info("hvisor: efi_copy, check it, unsupported size, size : %d\n", size);
        while(1) {} 
    }
    // allocate new area (free pages)
    void *area = (void *)__get_free_pages(GFP_KERNEL, 0);
    if (area == NULL) {
        pr_info("hvisor: efi_copy, failed to allocate memory\n");
    }
    // set the page as reserved
    SetPageReserved(virt_to_page(area));

    size_t base_addr = 0;
    __u64 offset = 0;

    void *map_addr = NULL;
    if (ori_addr != 0) {
        // get the map area (special), do it again!
        map_addr = memremap(ori_addr, size, MEMREMAP_WB);
        if (!map_addr) {
            pr_info("hvisor: hvisor_efi_general_memcpy fail to memremap\n");
            while(1) {}
        }    
        // get the base address of the map area
        base_addr = _ALIGN_DOWN((__u64)map_addr, PAGE_SIZE);// ok
        // copy old data to new area
        memcpy(area, (void *)base_addr, aligned_size);// ok
        // get the addr_with offset
        offset = ori_addr & (PAGE_SIZE - 1); // ok
    } else {
        memset(area, 0, aligned_size);
        offset = 0;
    }

    __u64 page_pa = __pa(area);
    __u64 ret_val = (unsigned long)area + offset;

    if (offset + size > aligned_size) {
        pr_info("hvisor: efi_copy, check it, offset + size > aligned_size\n");
        while(1) {}
    }

    // copy new object to new area (with right offset)
    memcpy(area + offset, object, size);

    __u64 ipa = 0;
    if (ori_addr != 0) {
        ipa = _ALIGN_DOWN(ori_addr, PAGE_SIZE);
        if (map_addr != NULL) {
            memunmap(map_addr);
        }
    } else {
        // ipa = page_pa;// one-to-one mapping
        ipa = 0;// special for the cmd_line_ptr
    }

    // update the efi_boot_info
    efi_boot_info->pages[efi_boot_info->count].ipa = ipa;
    efi_boot_info->pages[efi_boot_info->count].hpa = page_pa;
    efi_boot_info->pages[efi_boot_info->count].size = aligned_size;
    efi_boot_info->count++;

    pr_info("ori_addr: %px, map_addr: %px, object: %px, size: %d", 
        ori_addr, map_addr, object, size);
    pr_info("base_addr: %llx, offset: %llx, page_pa: %llx, ret_val: %llx, area : %px\n", 
        base_addr, offset, page_pa, ret_val, area);
        
    
    return __pa(ret_val);
}

static acpi_physical_address
hvisor_acpi_tb_get_root_table_entry(u8 *table_entry, u32 table_entry_size)
{
	u64 address64;
    ACPI_MOVE_64_TO_64(&address64, table_entry);
    return ((acpi_physical_address)(address64));
}

static void 
hvisor_copy_srat(__u64 address, 
    struct acpi_table_header *table, 
    efi_boot_info_t *efi_boot_info)
{
    struct acpi_subtable_header *subtable;
    char *end;

    struct acpi_table_srat *srat = (struct acpi_table_srat *)table;
    __u64 reserved = srat->reserved;
    __u32 table_revisrion = srat->table_revision;
    pr_info("hvisor: copy_srat, reserved: %llx, table_revision: %d\n", reserved, table_revisrion);

    int init_offset = sizeof(struct acpi_table_srat);
    subtable = (struct acpi_subtable_header *)((char *)table +
                                               init_offset);

    // using size of acpi_table_srat?
    end = (char *)table + table->length;

    pr_info("hvisor: parse_srat, length: %d, end: %px, \n", 
        table->length, end);

    print_acpi_table_header(table);

    // TODO: parse the memory request, not a fixed method
    int acc_offset = init_offset;
    
    int srat_mem_affinity_idx = 0;
    while ((char *)subtable < end) {
        pr_info("  SRAT subtable type: %d, length: %d\n", subtable->type,
                subtable->length);
        if(subtable->length == 0) {
            pr_info("[Attention] error, SRAT subtable length is 0, check it\n\n");
            while(1) {}
        }
        switch (subtable->type) {
            case ACPI_SRAT_TYPE_CPU_AFFINITY:
            {
                struct acpi_srat_cpu_affinity *p =
                    (struct acpi_srat_cpu_affinity *)subtable;
                pr_info("SRAT Processor (id[0x%02x] eid[0x%02x]) in proximity domain %d %s\n",
                        p->apic_id, p->local_sapic_eid,
                        p->proximity_domain_lo,
                        (p->flags & ACPI_SRAT_CPU_ENABLED) ?
                        "enabled" : "disabled");
            }
            break;
        
            case ACPI_SRAT_TYPE_MEMORY_AFFINITY:
            {
                struct acpi_srat_mem_affinity *p =
                    (struct acpi_srat_mem_affinity *)subtable;
                pr_info("SRAT Memory (0x%llx length 0x%llx) in proximity domain %d %s%s%s\n",
                        (unsigned long long)p->base_address,
                        (unsigned long long)p->length,
                        p->proximity_domain,
                        (p->flags & ACPI_SRAT_MEM_ENABLED) ?
                        "enabled" : "disabled",
                        (p->flags & ACPI_SRAT_MEM_HOT_PLUGGABLE) ?
                        " hot-pluggable" : "",
                        (p->flags & ACPI_SRAT_MEM_NON_VOLATILE) ?
                        " non-volatile" : "");
                srat_mem_affinity_idx++;

                // __u64 offset = (void *)p - (void *)table;
                // pr_info("[check it]!!! offset : 0x%llx, address : 0x%llx, p : %px\n, table : %px\n", 
                //     offset, address + offset, p, table);

                // if(srat_mem_affinity_idx == 2) {
                //     pr_info("hvisor: copy_srat, acc_offset: %d, address: %llx, p : %px, address + acc_offset: %llx\n", 
                //         acc_offset, address, p, address + acc_offset);
                //     struct acpi_srat_mem_affinity *object = 
                //         kmalloc(sizeof(struct acpi_srat_mem_affinity), GFP_KERNEL);
                //     object->length = 0x1000000;
                //     hvisor_efi_general_memcpy(
                //         address + acc_offset, p, object,
                //         sizeof(struct acpi_srat_mem_affinity), efi_boot_info);
                //     kfree(object);
                // }
            }
            break;
        }
        acc_offset += subtable->length;
        subtable = (struct acpi_subtable_header *)((char *)subtable + subtable->length);// update
    }
}

static void
hvisor_copy_mcfg(struct acpi_table_header *table)
{
    struct acpi_table_mcfg *mcfg;
	struct acpi_mcfg_allocation *mptr;
	int i, n;

	if (table->length < sizeof(struct acpi_table_mcfg)) {
        pr_info("hvisor_copy_mcfg, table->length < sizeof(struct acpi_table_mcfg)\n");
        while(1) ;
    }

    n = (table->length - sizeof(struct acpi_table_mcfg)) / sizeof(struct acpi_mcfg_allocation);
	mcfg = (struct acpi_table_mcfg *)table;
	mptr = (struct acpi_mcfg_allocation *) &mcfg[1];

    pr_info("hvisor: parse_mcfg, length: %d, n: %d\n", table->length, n);

    for (i = 0; i < n; i++, mptr++) {
        pr_info("hvisor: MCFG, segment: 0x%x, addr: 0x%llx, bus_start: 0x%x, bus_end: 0x%x\n", 
                mptr->pci_segment, mptr->address, mptr->start_bus_number, mptr->end_bus_number);
    }
}


static char *mps_inti_flags_polarity[] = { "dfl", "high", "res", "low" };
static char *mps_inti_flags_trigger[] = { "dfl", "edge", "res", "level" };
//注释LoongArch acpi相关逻辑
// static void hvisor_copy_apic(struct acpi_table_header *table)
// {
//     struct acpi_subtable_header *header;
// 	unsigned long madt_end, entry;
//     struct acpi_table_madt *madt = (struct acpi_table_madt *)table;

//     entry = (unsigned long)madt;
// 	madt_end = entry + madt->header.length;

// 	entry += sizeof(struct acpi_table_madt);
    
//     pr_info("=========== parse ACPI table ==========\n");
//     while (entry + sizeof(struct acpi_subtable_header) < madt_end) {
//         header = (struct acpi_subtable_header *)entry;

//         switch (header->type) {
//             case ACPI_MADT_TYPE_CORE_PIC:
//             {
//                 struct acpi_madt_core_pic *p = (struct acpi_madt_core_pic *)header;
//                 pr_info("CORE PIC (processor_id[0x%02x] core_id[0x%02x] %s)\n",
//                     p->processor_id, p->core_id,
//                     str_enabled_disabled(p->flags & ACPI_MADT_ENABLED));
//             }
//             break;
            
//             case ACPI_MADT_TYPE_LIO_PIC:
//             {
//                 struct acpi_madt_lio_pic *p = (struct acpi_madt_lio_pic *)header;
//                 pr_info("LIO PIC (version=%u, address=0x%llx, size=%u, cascade=[0x%02x,0x%02x], cascade_map=[0x%08x,0x%08x])\n",
//                     p->version, p->address, p->size,
//                     p->cascade[0], p->cascade[1],
//                     p->cascade_map[0], p->cascade_map[1]);
//             }
//             break;
            
//             case ACPI_MADT_TYPE_EIO_PIC:
//             {
//                 struct acpi_madt_eio_pic *p = (struct acpi_madt_eio_pic *)header;
//                 pr_info("EIO PIC (version=%u, cascade=0x%02x, node=0x%02x, node_map=0x%llx)\n",
//                     p->version, p->cascade, p->node, p->node_map);
//             }
//             break;
            
//             case ACPI_MADT_TYPE_MSI_PIC:
//             {
//                 struct acpi_madt_msi_pic *p = (struct acpi_madt_msi_pic *)header;
//                 pr_info("MSI PIC (version=%u, msg_address=0x%llx, start=%u, count=%u)\n",
//                     p->version, p->msg_address, p->start, p->count);
//             }
//             break;
            
//             case ACPI_MADT_TYPE_BIO_PIC:
//             {
//                 struct acpi_madt_bio_pic *p = (struct acpi_madt_bio_pic *)header;
//                 pr_info("BIO PIC (version=%u, address=0x%llx, size=%u, id=%u, gsi_base=%u)\n",
//                     p->version, p->address, p->size, p->id, p->gsi_base);
//             }
//             break;
            
//             case ACPI_MADT_TYPE_LPC_PIC:
//             {
//                 struct acpi_madt_lpc_pic *p = (struct acpi_madt_lpc_pic *)header;
//                 pr_info("LPC PIC (version=%u, address=0x%llx, size=%u, cascade=0x%02x)\n",
//                     p->version, p->address, p->size, p->cascade);
//             }
//             break;
            
//             default:
//                 pr_info("Found unsupported MADT entry (type = 0x%x)\n", header->type);
//                 break;
//         }
//         entry += header->length;
//     }
//     pr_info("\n");
// }

// =========== copy function (remapping the sub acpi table in fact) ==========
// only remapping the sub acpi table

// copy_efi_system_table
// -> copy_efi_config_table
//   -> copy_acpi_table 
//      -> copy_sub_acpi_table
//         -> copy_srat
//         -> copy_mcfg
//         -> copy_apic

// step1: memremap (map)
// step2: print check
// step3: kmalloc
// step4: copy data (memcpy)
// step5: memunmap (unmap)
// step6: update pointer (continue)

//注释LoongArch acpi相关逻辑
// static u8* copy_sub_acpi_table(acpi_physical_address address, 
//     efi_boot_info_t *efi_boot_info) {
    
//     u8 *table_entry = NULL;

//     // pr_info("hvisor: copy_sub_acpi_table, address: %llx\n", address);

//     // map acpi table header
//     struct acpi_table_header *sub_table = 
//         acpi_os_map_memory(address, sizeof(struct acpi_table_header));
    
//     if (!sub_table) {
//         pr_info("error, get header, acpi_sub_table is NULL\n");
//         while(1) ;
//     }
//     // print_acpi_table_header(sub_table);

//     // important acpi sub table
//     // ACPI_SIG_SRAT
//     // ACPI_SIG_MCFG
//     // ACPI_SIG_PPTT
//     // ACPI_SIG_APIC
//     // ACPI_SIG_IVRS

//     // not important acpi sub table
//     // ACPI_SIG_FACS
//     // ACPI_SIG_FACP
//     // ACPI_SIG_DSDT
//     // ACPI_SIG_VIAT
//     // ACPI_SIG_SLIT

//     // TODO: dynamic inject acpi sub table
//     // ACPI_SIG_SSDT

//     if (!memcmp(sub_table->signature, ACPI_SIG_SRAT, 4))      { // SRAT, System Resource Affinity Table
//         hvisor_copy_srat(address, sub_table, efi_boot_info);
//     } 
//     else if (!memcmp(sub_table->signature, ACPI_SIG_MCFG, 4)) { // MCFG, PCI Memory Mapped Configuration table
//         hvisor_copy_mcfg(sub_table);
//     } else if (!memcmp(sub_table->signature, ACPI_SIG_PPTT, 4)) { // PPTT, Processor Properties Topology Table
//         // ignore it
//     } else if (!memcmp(sub_table->signature, ACPI_SIG_MADT, 4)) { // APIC, Multiple APIC Description Table
//         hvisor_copy_apic(sub_table);
//     } else {
//         // pr_info("hvisor: copy_sub_acpi_table, unsupported table, sub_table->signature : %s\n", 
//         //     sub_table->signature);
//     }

//     // umap acpi table header
//     acpi_os_unmap_memory(sub_table, sizeof(struct acpi_table_header));
//     return table_entry;
// }

// static unsigned long 
// copy_acpi_table(unsigned long rsdp_address, 
//     efi_boot_info_t *efi_boot_info) {

//     unsigned long rsdp_ret = rsdp_address;

//     struct acpi_table_rsdp *rsdp;
//     // map rsdp
//     rsdp = acpi_os_map_memory(rsdp_address, sizeof(struct acpi_table_rsdp));
//     if (!rsdp) {
//         pr_info("error, rsdp is NULL\n");
//         while(1) {}
//     }
//     pr_info("rsdp address : %llx, rsdp : %px\n", rsdp_address, rsdp);
//     print_acpi_rsdp(rsdp);

// 	struct acpi_table_header *table;
// 	acpi_physical_address address;
// 	u32 length;
// 	u32 table_entry_size;
//     if ((rsdp->revision > 1) && rsdp->xsdt_physical_address) {
//         // get address of XSDT !!!
//         address = (acpi_physical_address)rsdp->xsdt_physical_address;
//         table_entry_size = ACPI_XSDT_ENTRY_SIZE;
//         pr_info("hvisor: XSDT address: 0x%lx\n", address);
//     } else {
//         pr_info("hvisor: Not support ACPI 1.0 or no XSDT\n");
//         while(1);
//     }

//     // ummap rsdp
//     acpi_os_unmap_memory(rsdp, sizeof(struct acpi_table_rsdp));

//     // map acpi table header
//     table = acpi_os_map_memory(address, sizeof(struct acpi_table_header));
// 	if (!table) {
//         pr_info("error, get header, acpi_table is NULL\n");
//         while(1) ;
// 	}
//     print_acpi_table_header(table);

//     // umap acpi table header
// 	acpi_os_unmap_memory(table, sizeof(struct acpi_table_header));

//     // get table acpi length
//     length = table->length;
// 	if (length < (sizeof(struct acpi_table_header) + table_entry_size)) {
//         pr_info("hvisor: invalid ACPI table length: %u\n", length);
//         while(1) ;
//     }

//     // map acpi table again, this time for copying (acpi table array)

//     table = acpi_os_map_memory(address, length);
// 	if (!table) {
//         pr_info("hvisor: error, get table array, acpi_table is NULL\n");
//         while(1) ;
// 	}

//     // TODO: verify checksum
//     // acpi_status status = acpi_ut_verify_checksum(table, length);
// 	// if (ACPI_FAILURE(status)) {
// 	// 	acpi_os_unmap_memory(table, length);
//     //     pr_info("hvisor: ACPI table checksum verification failed\n");
//     //     while(1) ;
//     // }
    
// 	u32 table_count = (u32)((table->length - sizeof(struct acpi_table_header)) / table_entry_size);
//     u8 *table_entry = ACPI_ADD_PTR(u8, table, sizeof(struct acpi_table_header));

//     pr_info("hvisor: ACPI table length: %u, count %u, table_entry_size: %u\n", 
//         length, table_count, table_entry_size);

//     for (int i = 0; i < table_count; i++) {
// 		/* Get the table physical address (32-bit for RSDT, 64-bit for XSDT) */
// 		address = hvisor_acpi_tb_get_root_table_entry(table_entry, table_entry_size);

//         // pr_info("hvisor: ACPI table entry %llx\n", table_entry);
// 		/* Skip NULL entries in RSDT/XSDT */
// 		if (!address) {
// 			goto next_table;
// 		}

//         copy_sub_acpi_table(address, efi_boot_info);

//         // u8* table_entry_new = copy_sub_acpi_table(address);

// next_table:
// 		table_entry += table_entry_size;
// 	}
//     // umap 
// 	// acpi_os_unmap_memory(table, length);
// 	acpi_os_unmap_memory(table, length + 16);

//     return rsdp_ret;
// }


// // static efi_config_table_t *
// static unsigned long 
// copy_efi_config_table(efi_system_table_t *efi_systab, 
//     efi_boot_info_t *efi_boot_info) {

//     // map config_tables_old
//     unsigned long efi_config_table = (unsigned long)efi_systab->tables;
//     int size = efi_systab->nr_tables * sizeof(efi_config_table_t);
//     void *config_tables_old = memremap(efi_config_table, size, MEMREMAP_WB);

//     // kmalloc a new config table
//     // efi_config_table_t *config_tables_new = kmalloc(size, GFP_KERNEL);
//     // if (!config_tables_new) {
//     //     pr_err("Failed to allocate memory for copy, efi_config_table_t, size : %d\n", size);
//     //     return NULL;
//     // }
//     // efi_config_table_64_t *tbl64_new = (void *)config_tables_new;
//     // memcpy(tbl64_new, tbl64_old, size);
//     // pr_info("[check point 1] copy efi_config_table, addr1 : %llx, addr2 : %llx, addr3 : %llx, size : %d\n", 
//     //     efi_systab, config_tables_old, config_tables_new, size);

//     // iterate the configuration tables and try to copy the configruation table
//     efi_config_table_64_t *tbl64_old = (void *)config_tables_old;
//     for (int i = 0; i < efi_systab->nr_tables; i++) {
//         if (!efi_guidcmp(tbl64_old[i].guid, ACPI_20_TABLE_GUID)) {
//             // continue to copy ACPI table
//             // unsigned long new_table = copy_acpi_table(tbl64_old[i].table);
//             copy_acpi_table(tbl64_old[i].table, efi_boot_info);
//             // tbl64_new[i].table = new_table;
//         }
//         // ============ print guid and table address ==========
//         // efi_guid_t *guid_tmp;
//         // unsigned long table_tmp;   
//         // guid_tmp = &tbl64_old[i].guid;
//         // table_tmp = tbl64_old[i].table;
//         // char guid_str[EFI_VARIABLE_GUID_LEN + 1];
//         // efi_guid_to_str(guid_tmp, guid_str);
//         // pr_info("EFI Configuration Table: %s, table: 0x%lx\n", 
//         //     guid_str, table_tmp);
//     }
    
//     // unmap config_tables_old
//     memunmap(config_tables_old);
//     // return config_tables_new;
//     return 0;
// }

// // try to maintain the structure of efi system table
// orignal : physical address, need memremap
// // static efi_system_table_t *
// static unsigned long 
// copy_efi_system_table(__u64 estp_orig, 
//     efi_boot_info_t *efi_boot_info) {
    
//     int size = sizeof(efi_system_table_t);
//     efi_system_table_t *efi_sys_table = memremap(estp_orig, size, MEMREMAP_WB);
//     if (!efi_sys_table) {
//         pr_info("hvisor: copy_efi_system_table, failed to map efi system table\n");
//         while(1) {}
//     }
//     // efi_system_table_t *estp_copy = kmalloc(size, GFP_KERNEL);
//     // if (!estp_copy) {
//     //     pr_err("Failed to allocate memory for copy, efi_system_table_t\n");
//     //     return NULL;
//     // }
//     // memcpy(estp_copy, efi_sys_table, size);
//     // pr_info("[check point 0] copy efi_system_table, addr1 : %llx, addr2 : %llx, addr3: %llx, size : %d\n", 
//     //     estp_orig, efi_sys_table, estp_copy, size);

//     // continue to copy the config tables
//     // estp_copy->tables = copy_efi_config_table(efi_sys_table);
//     copy_efi_config_table(efi_sys_table, efi_boot_info);

//     // free space
//     memunmap(efi_sys_table);
//     // return estp_copy;
//     return 0;
// }
//注释LoongArch acpi相关逻辑
// static unsigned long 
// hvisor_copy_efi_system_table(efi_boot_info_t __user *arg) {
//     // get the original efi system table pointer

//     efi_boot_info_t *efi_boot_info = kmalloc(sizeof(efi_boot_info_t), GFP_KERNEL);
//     if (copy_from_user(efi_boot_info, arg, sizeof(efi_boot_info_t))) {
//         pr_err("hvisor: efi_boot_info failed to copy from user\n");
//         kfree(efi_boot_info);
//         return -EFAULT;
//     }

//     // get the original efi system table pointer
//     __u64 estp_orig = hvisor_call(HVISOR_HC_GET_EFI_SYSTEM_TABLE, 0, 0);

//     pr_info("copy_efi_system_table: estp_orig: %llx\n", estp_orig);
//     // pr_info("copy_efi_system_table: cmdline_ptr: %llx\n", cmdline_ptr_ori);

//     // int size = 100;
//     // char *efi_cmdline_ptr_map = memremap(cmdline_ptr_ori, size, MEMREMAP_WB);
//     // if (!efi_cmdline_ptr_map) {
//     //     pr_info("hvisor: failed to map cmdline_ptr\n");
//     //     while(1) {}
//     // }

//     // hvisor_efi_general_memcpy(cmdline_ptr_ori, efi_cmdline_ptr_map, 
//     //     efi_boot_info->cmd_line, strlen(efi_boot_info->cmd_line), efi_boot_info);
//     // memunmap(efi_cmdline_ptr_map);
    
//     __u64 cmd_line_address = hvisor_efi_general_memcpy(0, 
//         efi_boot_info->cmd_line, strlen(efi_boot_info->cmd_line), efi_boot_info);

//     // hvisor_call(HVISOR_HC_SET_EFI_CMDLINE, cmd_line_address, 0);
//     // pr_info("hvisor: set efi cmdline to %s, address: %llx\n", 
//     //     efi_boot_info->cmd_line, cmd_line_address);
//     // pr_info("check this, wait\n");
//     // while(1) {
//     // };

//     // efi_system_table_t *estp_copy = copy_efi_system_table(estp_orig);
//     copy_efi_system_table(estp_orig, efi_boot_info);

//     if (copy_to_user(arg, efi_boot_info,  sizeof(efi_boot_info_t))) {
//         pr_err("hvisor: failed to copy to user\n");
//         kfree(efi_boot_info);
//         return -EFAULT;
//     }

//     kfree(efi_boot_info);
//     return 0;
//     // return estp_copy;
// }

// finish virtio req and send result to el2
static int hvisor_finish_req(void) {
    int err;
    err = hvisor_call(HVISOR_HC_FINISH_REQ, 0, 0);
    if (err)
        return err;
    return 0;
}

// static int flush_cache(__u64 phys_start, __u64 size)
// {
//     struct vm_struct *vma;
//     int err = 0;
//     size = PAGE_ALIGN(size);
//     vma = __get_vm_area(size, VM_IOREMAP, VMALLOC_START, VMALLOC_END);
//     if (!vma)
//     {
//         pr_err("hvisor: failed to allocate virtual kernel memory for
//         image\n"); return -ENOMEM;
//     }
//     vma->phys_addr = phys_start;

//     if (ioremap_page_range((unsigned long)vma->addr, (unsigned
//     long)(vma->addr + size), phys_start, PAGE_KERNEL_EXEC))
//     {
//         pr_err("hvisor: failed to ioremap image\n");
//         err = -EFAULT;
//         goto unmap_vma;
//     }
//     // flush icache will also flush dcache
//     flush_icache_range((unsigned long)(vma->addr), (unsigned long)(vma->addr
//     + size));

// unmap_vma:
//     vunmap(vma->addr);
//     return err;
// }bash
static int hvisor_config_check(u64 __user *arg) {
    int err = 0;
    u64 *config;
    config = kmalloc(sizeof(u64), GFP_KERNEL);
    err = hvisor_call(HVISOR_HC_CONFIG_CHECK, __pa(config), 0);

    if (err != 0) {
        pr_err("hvisor.ko: failed to get hvisor config\n");
    }

    if (copy_to_user(arg, config, sizeof(u64))) {
        pr_err("hvisor.ko: failed to copy to user\n");
        kfree(config);
        return -EFAULT;
    }

    kfree(config);
    return err;
}
static int hvisor_zone_start(zone_config_t __user *arg) {
    int err = 0;
    zone_config_t *zone_config = kmalloc(sizeof(zone_config_t), GFP_KERNEL);

    if (zone_config == NULL) {
        pr_err("hvisor: failed to allocate memory for zone_config\n");
    }

    if (copy_from_user(zone_config, arg, sizeof(zone_config_t))) {
        pr_err("hvisor: failed to copy from user\n");
        kfree(zone_config);
        return -EFAULT;
    }

    // flush_cache(zone_config->kernel_load_paddr, zone_config->kernel_size);
    // flush_cache(zone_config->dtb_load_paddr, zone_config->dtb_size);

    pr_info("hvisor: calling hypercall to start zone\n");

    err = hvisor_call(HVISOR_HC_START_ZONE, __pa(zone_config),
                      sizeof(zone_config_t));
    kfree(zone_config);
    return err;
}

#ifndef LOONGARCH64
static int is_reserved_memory(unsigned long phys, unsigned long size) {
    struct device_node *parent, *child;
    struct reserved_mem *rmem;
    phys_addr_t mem_base;
    size_t mem_size;
    int count = 0;
    parent = of_find_node_by_path("/reserved-memory");
    count = of_get_child_count(parent);

    for_each_child_of_node(parent, child) {
        rmem = of_reserved_mem_lookup(child);
        mem_base = rmem->base;
        mem_size = rmem->size;
        if (mem_base <= phys && (mem_base + mem_size) >= (phys + size)) {
            return 1;
        }
    }
    return 0;
}
#endif

static int hvisor_zone_list(zone_list_args_t __user *arg) {
    int ret;
    zone_info_t *zones;
    zone_list_args_t args;

    /* Copy user provided arguments to kernel space */
    if (copy_from_user(&args, arg, sizeof(zone_list_args_t))) {
        pr_err("hvisor: failed to copy from user\n");
        return -EFAULT;
    }

    zones = kmalloc(args.cnt * sizeof(zone_info_t), GFP_KERNEL);
    memset(zones, 0, args.cnt * sizeof(zone_info_t));

    ret = hvisor_call(HVISOR_HC_ZONE_LIST, __pa(zones), args.cnt);
    if (ret < 0) {
        pr_err("hvisor: failed to get zone list\n");
        goto out;
    }
    // copy result back to user space
    if (copy_to_user(args.zones, zones, ret * sizeof(zone_info_t))) {
        pr_err("hvisor: failed to copy to user\n");
        goto out;
    }
out:
    kfree(zones);
    return ret;
}

static int hvisor_shm_signal(shm_args_t __user *arg) {
    int ret;
    shm_args_t shm_signal_info;

    /* Copy user provided arguments to kernel space */
    if (copy_from_user(&shm_signal_info, arg, sizeof(shm_args_t))) {
        pr_err("hvisor: failed to copy from user\n");
        return -EFAULT;
    }

    ret = hvisor_call(HVISOR_SHM_SIGNAL, shm_signal_info.target_zone_id, 
        shm_signal_info.service_id);
    if (ret < 0) {
        pr_info("hvisor: failed to do shm signal\n");
        while(1) {}
    }

    ret = 0;

    return ret;
}

static long hvisor_ioctl(struct file *file, unsigned int ioctl,
                         unsigned long arg) {
    int err = 0;
    switch (ioctl) {
    case HVISOR_INIT_VIRTIO:
        err = hvisor_init_virtio();
        task = get_current(); // get hvisor user process
        break;
    case HVISOR_ZONE_START:
        err = hvisor_zone_start((zone_config_t __user *)arg);
        break;
    case HVISOR_ZONE_SHUTDOWN:
        err = hvisor_call(HVISOR_HC_SHUTDOWN_ZONE, arg, 0);
        break;
    case HVISOR_ZONE_LIST:
        err = hvisor_zone_list((zone_list_args_t __user *)arg);
        break;
    case HVISOR_FINISH_REQ:
        err = hvisor_finish_req();
        break;
    case HVISOR_CONFIG_CHECK:
        err = hvisor_config_check((u64 __user *)arg);
        break;
#ifdef LOONGARCH64
    case HVISOR_ZONE_M_ALLOC:
        err = hvisor_m_alloc((kmalloc_info_t __user *)arg);
        break;
    case HVISOR_ZONE_M_FREE:
        err = hvisor_m_free((kmalloc_info_t __user *)arg);
        break;
    case HVISOR_COPY_EFI_SYSTEM_TABLE:
        err = hvisor_copy_efi_system_table((efi_boot_info_t __user *)arg);
        return err;
    case HVISOR_CLEAR_INJECT_IRQ:
        err = hvisor_call(HVISOR_HC_CLEAR_INJECT_IRQ, 0, 0);
        break;
    case HVISOR_HC_START_EXCEPTION_TRACE:
        err = hvisor_call(HVISOR_HC_START_EXCEPTION_TRACE, 0, 0);
        break;
    case HVISOR_HC_END_EXCEPTION_TRACE:
        err = hvisor_call(HVISOR_HC_END_EXCEPTION_TRACE, 0, 0);
        break;
    case HVISOR_SHM_SIGNAL:
        err = hvisor_shm_signal((shm_args_t __user *)arg);
        break;
#endif
#ifdef ARM64
    case HVISOR_ZONE_M_ALLOC:
        err = hvisor_m_alloc((kmalloc_info_t __user *)arg);
        break;
    case HVISOR_ZONE_M_FREE:
        err = hvisor_m_free((kmalloc_info_t __user *)arg);
        break;
    case HVISOR_CLEAR_INJECT_IRQ:
        err = hvisor_call(HVISOR_HC_CLEAR_INJECT_IRQ, 0, 0);
        break;
    case HVISOR_HC_START_EXCEPTION_TRACE:
        err = hvisor_call(HVISOR_HC_START_EXCEPTION_TRACE, 0, 0);
        break;
    case HVISOR_HC_END_EXCEPTION_TRACE:
        err = hvisor_call(HVISOR_HC_END_EXCEPTION_TRACE, 0, 0);
        break;
    case HVISOR_SHM_SIGNAL:
        err = hvisor_shm_signal((shm_args_t __user *)arg);
        break;
#endif
    default:
        err = -EINVAL;
        break;
    }
    return err;
}

// Kernel mmap handler
static int hvisor_map(struct file *filp, struct vm_area_struct *vma) {
    unsigned long phys;
    int err;
    if (vma->vm_pgoff == 0) {
        // virtio_bridge must be aligned to one page.
        phys = virt_to_phys(virtio_bridge);
        // vma->vm_flags |= (VM_IO | VM_LOCKED | (VM_DONTEXPAND | VM_DONTDUMP));
        // Not sure should we add this line.
        err = remap_pfn_range(vma, vma->vm_start, phys >> PAGE_SHIFT,
                              vma->vm_end - vma->vm_start, vma->vm_page_prot);
        if (err)
            return err;
        pr_info("virtio bridge mmap succeed!\n");
        pr_info("virtio bridge mmap succeed, boneinscri check this\n");
    } else {
        size_t size = vma->vm_end - vma->vm_start;
        // TODO: add check for non root memory region.
        // memremap(0x50000000, 0x30000000, MEMREMAP_WB);
        // vm_pgoff is the physical page number.
        // if (!is_reserved_memory(vma->vm_pgoff << PAGE_SHIFT, size)) {
        //     pr_err("The physical address to be mapped is not within the
        //     reserved memory\n"); return -EFAULT;
        // }
        
        // HyperAMP shared memory regions: use uncached mapping
        // TX Queue: 0x7E000000, RX Queue: 0x7E001000, Data Region: 0x7E002000
        unsigned long phys_addr = vma->vm_pgoff << PAGE_SHIFT;

        // HyperAMP shared memory region (0x7E000000 - 0x7E402000, ~4MB)
        if (phys_addr >= 0x7E000000UL && phys_addr < 0x7E500000UL) {
            vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);
            pr_info("HyperAMP shared memory mapped at PA %#lx with uncached protection (size: %#lx)\n", 
                    phys_addr, size);
            pr_info("  vm_page_prot pgprot value: %#lx (should have non-cacheable bits set)\n",
                    pgprot_val(vma->vm_page_prot));
        }
        // HyperAMP MMIO control region: 0x6e410000
        else if (phys_addr == 0x6e410000UL) {
            vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);
            pr_info("HyperAMP MMIO control region mapped at PA %#lx with uncached protection\n", phys_addr);
        }
        
        err = remap_pfn_range(vma, vma->vm_start, vma->vm_pgoff, size,
                              vma->vm_page_prot);
        if (err)
            return err;
        // pr_info("non root region mmap succeed!\n");
    }
    return 0;
}

static const struct file_operations hvisor_fops = {
    .owner = THIS_MODULE,
    .unlocked_ioctl = hvisor_ioctl,
    .compat_ioctl = hvisor_ioctl,
    .mmap = hvisor_map,
};

static struct miscdevice hvisor_misc_dev = {
    .minor = MISC_DYNAMIC_MINOR,
    .name = "hvisor",
    .fops = &hvisor_fops,
};

// Interrupt handler for Virtio device.
static irqreturn_t virtio_irq_handler(int irq, void *dev_id) {
    struct siginfo info;
    if (dev_id != &hvisor_misc_dev) {
        return IRQ_NONE;
    }

    memset(&info, 0, sizeof(struct siginfo));
    info.si_signo = SIGHVI;
    info.si_code = SI_QUEUE;
    info.si_int = 1;
    // Send signal SIGHVI to hvisor user task
    if (task != NULL) {
        // pr_info("send signal to hvisor device\n");
        if (send_sig_info(SIGHVI, (struct kernel_siginfo *)&info, task) < 0) {
            pr_err("Unable to send signal\n");
        }
    }
    return IRQ_HANDLED;
}


static void list_acpi_irqs(void)
{
    pr_info("Listing PCI device IRQs...\n");
    struct pci_dev *dev = NULL;
    for_each_pci_dev(dev) {
        if (dev->irq) {
            pr_info("PCI device %04x:%04x (bus %02x:%02x.%d) IRQ: %d\n",
                   dev->vendor, dev->device,
                   dev->bus->number, PCI_SLOT(dev->devfn), PCI_FUNC(dev->devfn),
                   dev->irq);
        }
    }
}

/*
** Module Init function
*/

static int __init hvisor_init(void) {
    int err;
    struct device_node *node = NULL;
    err = misc_register(&hvisor_misc_dev);
    if (err) {
        pr_err("hvisor_misc_register failed!!!\n");
        return err;
    } else {
        pr_info("hvisor_misc_register succeed!!!\n");
    }

    // static unsigned long phys_addr;
    // for (int i = 0; i < 20; i++) {
    //     unsigned long size = 0x10000 << i;
    //     phys_addr = hvisor_m_alloc(size);
    //     if (!phys_addr) {
    //         pr_err("Failed to allocate memory for hvisor\n");
    //     } else {
    //         pr_info("hvisor, i: %lx, allocated memory at physical address 0x%lx\n", size, phys_addr);
    //     }    
    // }
    
    // struct acpi_device *adev;
    // int irq;
    // struct miscdevice *mdev = &hvisor_misc_dev;
    // adev = ACPI_COMPANION(hvisor_misc_dev.this_device);
    // irq = platform_get_irq(to_platform_device(mdev->this_device), 0);
    // pr_info("hvisor_misc_dev irq is %d\n", irq);
    
    // probe hvisor virtio device.
    // The irq number must be retrieved from dtb node, because it is different
    // from GIC's IRQ number.
    node = of_find_node_by_path("/hvisor_virtio_device");
    if (!node) {
        list_acpi_irqs();
        pr_info("need to consider ACPI device node???\n");
        pr_info("hvisor_virtio_device node not found in dtb, can't use virtio "
                "devices\n");
    } else {
        virtio_irq = of_irq_get(node, 0);
        err = request_irq(virtio_irq, virtio_irq_handler,
                          IRQF_SHARED | IRQF_TRIGGER_RISING,
                          "hvisor_virtio_device", &hvisor_misc_dev);
        if (err)
            goto err_out;
    }
    of_node_put(node);
    pr_info("hvisor init done!!!\n");
    return 0;

err_out:
    pr_err("hvisor cannot register IRQ, err is %d\n", err);
    if (virtio_irq != -1)
        free_irq(virtio_irq, &hvisor_misc_dev);
    misc_deregister(&hvisor_misc_dev);
    return err;
}

/*
** Module Exit function
*/
static void __exit hvisor_exit(void) {
    if (virtio_irq != -1)
        free_irq(virtio_irq, &hvisor_misc_dev);
    if (virtio_bridge != NULL) {
        ClearPageReserved(virt_to_page(virtio_bridge));
        free_pages((unsigned long)virtio_bridge, 0);
    }
    misc_deregister(&hvisor_misc_dev);
    pr_info("hvisor exit!!!\n");
}

module_init(hvisor_init);
module_exit(hvisor_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("KouweiLee <15035660024@163.com>");
MODULE_DESCRIPTION("The hvisor device driver");
MODULE_VERSION("1:0.0");