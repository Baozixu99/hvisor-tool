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
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/kdev_t.h>
#include <linux/kernel.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_fdt.h>
#include <linux/of_irq.h>
#include <linux/poll.h>
#include <linux/sched/signal.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/uaccess.h>
#include <linux/wait.h>

#include "hvisor.h"
#include "shm.h"

#ifdef ARM64

struct shm_msg {
    __u64 len;
    __u64 shm_ct_ipas[CONFIG_MAX_SHM_CONFIGS];
    __u64 shm_shmem_ipas[CONFIG_MAX_SHM_CONFIGS];
    __u32 shm_ids[CONFIG_MAX_SHM_CONFIGS];
    __u32 shm_irqs[CONFIG_MAX_SHM_CONFIGS];
} __attribute__((packed));
typedef struct shm_msg shm_msg_t;

struct shm_dev {
    dev_t dev_id;
    struct cdev cdev;
    struct device *device;
    struct task_struct *task;
    wait_queue_head_t wq;
    int idx;
    int shm_id;
    int shm_irq;
    int received_irq; // receive irq count
        // SHM signal support
    int shm_signal_count;
    uint64_t last_shm_timestamp;
    uint32_t last_service_id;
};

shm_msg_t *shm_msg;

dev_t mdev_id;
static int dev_len;
static struct shm_dev *shm_devs;
static struct class *shm_class;

extern u8 __dtb_hshm_template_begin[], __dtb_hshm_template_end[];

static int hvisor_shm_msg(void) {
    int err = 0;
    int i;
    if (shm_msg == NULL) {
        shm_msg = kmalloc(sizeof(shm_msg_t), GFP_KERNEL);
        if (!shm_msg) {
            pr_err("Failed to allocate shm_msg\n");
            return -ENOMEM;
        }
        pr_info("Allocated shm_msg at %p\n", shm_msg);
    }

    // 清空结构体
    memset(shm_msg, 0, sizeof(*shm_msg));

    // 手动写死值
    shm_msg->len = 1;

    shm_msg->shm_ids[0] = 0;
    shm_msg->shm_ct_ipas[0] = 0xe0000000;
    shm_msg->shm_shmem_ipas[0] = 0xe0010000;
    shm_msg->shm_irqs[0] = 42;

    // 打印调试信息
    pr_info("shm_msg initialized with len: %llu\n", shm_msg->len);
    for (i = 0; i < shm_msg->len; i++) {
        pr_info("Config[%d]: id=%u, ct_ipa=0x%llx, shmem_ipa=0x%llx, irq=%u\n",
                i,
                shm_msg->shm_ids[i],
                shm_msg->shm_ct_ipas[i],
                shm_msg->shm_shmem_ipas[i],
                shm_msg->shm_irqs[i]);
    }

    return err;
}

static int hvisor_shm_signal_info(shm_signal_info_t __user *sinfo) {
    struct shm_dev *dev;
    int i, total_signals = 0;
    uint64_t latest_timestamp = 0;
    uint32_t latest_service_id = 0;
    
    // 汇总所有设备的 SHM 信号信息
    for (i = 0; i < dev_len; i++) {
        dev = &shm_devs[i];
        total_signals += dev->shm_signal_count;
        if (dev->last_shm_timestamp > latest_timestamp) {
            latest_timestamp = dev->last_shm_timestamp;
            latest_service_id = dev->last_service_id;
        }
    }
    
    sinfo->signal_count = total_signals;
    sinfo->last_timestamp = latest_timestamp;
    sinfo->last_service_id = latest_service_id;
    sinfo->current_cpu = smp_processor_id();
    
    return 0;
}

static int shm_open(struct inode *inode, struct file *file) {
    struct shm_dev *dev = container_of(inode->i_cdev, struct shm_dev, cdev);
    dev->task = get_current();
    file->private_data = dev;
    return 0;
}

static int hvisor_user_shm_msg(shm_uinfo_t __user *uinfo) {
    int i;
    uinfo->len = shm_msg->len;
    for (i = 0; i < shm_msg->len; i++) {
        uinfo->shm_ids[i] = shm_msg->shm_ids[i];
    }
    return 0;
}

static long shm_ioctl(struct file *file, unsigned int ioctl,
                      unsigned long arg) {
    int err = 0;
    switch (ioctl) {
    case HVISOR_SHM_USER_INFO:
        err = hvisor_user_shm_msg((shm_uinfo_t __user *)arg);
        break;
    case HVISOR_SHM_SIGNAL_INFO:
        err = hvisor_shm_signal_info((shm_signal_info_t __user *)arg);
        break;
    default:
        err = -EINVAL;
        break;
    }
    return err;
}

static int shm_map(struct file *filp, struct vm_area_struct *vma) {
    unsigned long long phys, offset;
    int err = 0, idx;
    size_t size = vma->vm_end - vma->vm_start;
    struct shm_dev *dev = filp->private_data;
    idx = dev->idx;
    phys = vma->vm_pgoff << PAGE_SHIFT;

    if (phys == 0) {
        // control table
        if (size != 0x1000) {
            pr_err("Invalid size for control table\n");
            return -EINVAL;
        }
        vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);
        err = remap_pfn_range(vma, vma->vm_start,
                              shm_msg->shm_ct_ipas[idx] >> PAGE_SHIFT, size,
                              vma->vm_page_prot);
    } else {
        // TODO: add check for memory
        offset = phys - 0x1000;
        err = remap_pfn_range(vma, vma->vm_start,
                              (shm_msg->shm_shmem_ipas[idx] + offset) >>
                                  PAGE_SHIFT,
                              size, vma->vm_page_prot);
    }
    if (err)
        return err;
    pr_info("shm region mmap succeed!\n");
    return 0;
}

static unsigned int shm_poll(struct file *filp,
                             struct poll_table_struct *wait) {
    __poll_t mask = 0;
    struct shm_dev *this_dev = (struct shm_dev *)filp->private_data;
    poll_wait(filp, &this_dev->wq, wait);
    if (this_dev->received_irq) {
        mask |= POLLIN;
        this_dev->received_irq = 0;
    }
    return mask;
}

static const struct file_operations shm_fops = {
    .owner = THIS_MODULE,
    .open = shm_open,
    .unlocked_ioctl = shm_ioctl,
    .compat_ioctl = shm_ioctl,
    .mmap = shm_map,
    .poll = shm_poll,
};

static irqreturn_t shm_irq_handler(int irq, void *dev_id) {
    int i;
    struct shm_dev *this_dev = NULL;
    pr_info("SHM test start");
    for (i = 0; i < dev_len; i++)
        if (dev_id == &shm_devs[i])
            this_dev = (struct shm_dev *)dev_id;
    if (!this_dev)
        return IRQ_NONE;

    this_dev->received_irq++;
    this_dev->shm_signal_count++;
    this_dev->last_shm_timestamp = ktime_get_ns();
    this_dev->last_service_id = 0; // 暂时设为0，后续可以通过其他方式获取
    pr_info("IVC: Received SHM signal on CPU %d, count: %d\n", 
            smp_processor_id(), this_dev->shm_signal_count);

    wake_up(&this_dev->wq);
    pr_info("SHM test end");
    return IRQ_HANDLED;
}

// static struct property *alloc_property(const char *name, int len) {
//     struct property *prop;
//     prop = kzalloc(sizeof(struct property), GFP_KERNEL);
//     prop->name = kstrdup(name, GFP_KERNEL);
//     prop->length = len;
//     prop->value = kzalloc(len, GFP_KERNEL);
//     return prop;
// }

// static int add_shm_device_node(void)
// {
//     int err, i, j, overlay_id;
//     struct device_node *node = NULL;
//     struct property *prop;
//     u32* values;
//     err = of_overlay_fdt_apply(__dtb_hshm_template_begin,
//         __dtb_hshm_template_end - __dtb_hshm_template_begin,
//         &overlay_id);
//     if (err) return err;

//     struct of_changeset overlay_changeset;
//     of_changeset_init(&overlay_changeset);
// 	node = of_find_node_by_path("/hvisor_shm_device");

//     // TODO: 加入对gic interrupt cell的探测，以及错误处理
//     prop = alloc_property("interrupts", sizeof(u32)*3*dev_len);
//     values = prop->value;
//     for(i=0; i<dev_len; i++) {
//         j = i * 3;
//         values[j++] = 0x00;
//         values[j++] = shm_msg->shm_irqs[i] - 32;
//         values[j++] = 0x01;
//     }
//     of_changeset_add_property(&overlay_changeset, node, prop);
//     of_changeset_apply(&overlay_changeset);
//     return 0;
// }

static int __init shm_init(void) {
    int err, i, soft_irq;
    struct device_node *node = NULL;
    hvisor_shm_msg();
    dev_len = shm_msg->len;
    
    pr_info("dev_len: %d\n", dev_len);
    shm_devs = kmalloc(sizeof(struct shm_dev) * dev_len, GFP_KERNEL);
    err = alloc_chrdev_region(&mdev_id, 0, dev_len, "hshm");
    if (err)
        goto err1;
    pr_info("shm get major id: %d\n", MAJOR(mdev_id));

    shm_class = class_create(THIS_MODULE, "hshm");
    if (IS_ERR(shm_class)) {
        err = PTR_ERR(shm_class);
        goto err1;
    }

    for (i = 0; i < dev_len; i++) {
        shm_devs[i].shm_id = shm_msg->shm_ids[i];
        shm_devs[i].dev_id = MKDEV(MAJOR(mdev_id), i);
        shm_devs[i].idx = i;
        shm_devs[i].cdev.owner = THIS_MODULE;
        shm_devs[i].received_irq = 0;
        shm_devs[i].shm_signal_count = 0;
        shm_devs[i].last_shm_timestamp = 0;
        shm_devs[i].last_service_id = 0;
        init_waitqueue_head(&shm_devs[i].wq);
        cdev_init(&shm_devs[i].cdev, &shm_fops);
        err = cdev_add(&shm_devs[i].cdev, shm_devs[i].dev_id, 1);
        if (err)
            goto err2;
        shm_devs[i].device = device_create(shm_class, NULL, shm_devs[i].dev_id,
                                           NULL, "hshm%d", shm_devs[i].shm_id);
        if (IS_ERR(shm_devs[i].device)) {
            err = PTR_ERR(shm_devs[i].device);
            goto err2;
        }
    }
    node = of_find_node_by_path("/hvisor_shm_device");
    if (!node) {
        // add_shm_device_node();
        pr_info("hvisor_shm_device node not found in dtb, can't use shm\n");
    } else {
        for (i = 0; i < dev_len; i++) {
            soft_irq = of_irq_get(node, i);
            pr_info("soft_irq: %d\n", soft_irq);
            err = request_irq(soft_irq, shm_irq_handler,
                              IRQF_SHARED | IRQF_TRIGGER_RISING,
                              "hvisor_shm_device", &shm_devs[i]);
            if (err) {
                pr_err("request irq failed\n");
                goto err2;
            }
        }
    }
    of_node_put(node);
    pr_info("shm init!!!\n");
    return 0;

err2:
    for (i = 0; i < dev_len; i++) {
        cdev_del(&shm_devs[i].cdev);
        device_destroy(shm_class, shm_devs[i].dev_id);
    }
    class_destroy(shm_class);
    unregister_chrdev_region(mdev_id, dev_len);
err1:
    kfree(shm_msg);
    kfree(shm_devs);
    return err;
}

static void __exit shm_exit(void) {
    // TODO
    pr_info("shm exit done!!!\n");
}

#else
// for other architecture we implement empty functions
// because different linux versions has different kernel interfaces
// TODO: add support for other architectures
static int __init shm_init(void) { return 0; }
static void __exit shm_exit(void) { return; }

#endif

module_init(shm_init);
module_exit(shm_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("The hvisor device driver");
MODULE_VERSION("1:0.0");