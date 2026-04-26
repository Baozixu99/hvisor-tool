
#include "shm/config/config_common.h"
#include "shm/config/config_zone.h"
#include "shm/config/config_addr.h"
#include "shm/config/config_channel.h"
#include "shm/config/config_shm.h"
#include <stdio.h>

/* core_cfg.h */
struct ZoneInfo zone_infos[] = 
{
  { /* 0: Root Linux 节点 */
    .name = ZONE_LINUX_NAME,
    .id = ZONE_LINUX_ID
  },
  { /* 1： NPUcore 节点 */
    .name = ZONE_NPUcore_NAME,
    .id = ZONE_NPUcore_ID
  },
  { /* 2: RT-Thread 节点 */
    .name = ZONE_RT_Thread_ID,
    .id = ZONE_RT_Thread_ID
  },
  { /* 3: ZONE_SeL4 节点 */
    .name = ZONE_SeL4_NAME,
    .id = ZONE_SeL4_ID
  }
};


// GLOBAL ADDRESS INFOS
struct AddrInfo addr_infos[] =
{
  { /* 0: CH0 Linux Buffer */
      .start = 0x0, .len = 0
  },
  { /* 1: CH0 Linux -> NPUCore Queue */
      .start = 0x0, .len = 0
  },
  { /* 2: CH0 NPUCore Queue (Remote) */
      .start = 0x0, .len = 0
  },
  { /* 3: CH1 Linux Buffer */
      .start = 0x0, .len = 0
  },
  { /* 4: CH1 Linux -> NPUCore Queue */
      .start = 0x0, .len = 0
  },
  { /* 5: CH1 NPUCore Queue (Remote) */
      .start = 0x0, .len = 0
  },
  { /* 6: CH2 Linux Buffer */
      .start = 0x0, .len = 0
  },
  { /* 7: CH2 Linux -> NPUCore Queue */
      .start = 0x0, .len = 0
  },
  { /* 8: CH2 NPUCore Queue (Remote) */
      .start = 0x0, .len = 0
  },
};


/* channel_cfg.h */
struct ChannelInfo channel_infos[] = 
{
    { /* 通道0： RootLinux -> NPUcore */
      .channel_id = 0,
      .irq_req = 74,
      .irq_rsp = 74,
      .src_zone = LINUX_ZONE_INFO,
      .dst_zone = NPUCORE_ZONE_INFO,
      .src_queue = LINUX_2_NPUCore_MSG_QUEUE_ADDR_INFO,
      .dst_queue = NPUCore_2_NPUCore_MSG_QUEUE_ADDR_INFO
    },
    { /* 通道1： RootLinux -> NPUcore CH1 */
      .channel_id = 1,
      .irq_req = 74,
      .irq_rsp = 74,
      .src_zone = LINUX_ZONE_INFO,
      .dst_zone = NPUCORE_ZONE_INFO,
      .src_queue = LINUX_2_NPUCore_CH1_MSG_QUEUE_ADDR_INFO,
      .dst_queue = NPUCore_2_NPUCore_CH1_MSG_QUEUE_ADDR_INFO
    },
    { /* 通道2： RootLinux -> NPUcore CH2 */
      .channel_id = 2,
      .irq_req = 74,
      .irq_rsp = 74,
      .src_zone = LINUX_ZONE_INFO,
      .dst_zone = NPUCORE_ZONE_INFO,
      .src_queue = LINUX_2_NPUCore_CH2_MSG_QUEUE_ADDR_INFO,
      .dst_queue = NPUCore_2_NPUCore_CH2_MSG_QUEUE_ADDR_INFO
    },
    { /* null */
      .src_zone = NULL
    }   
};

/* shm_cfg.h */
struct ShmCfg shm_cfgs[] = 
{
  { /* Linux CH0 共享内存配置信息 */
    .zone = LINUX_ZONE_INFO,
    .zone_shm = LINUX_SHM_BUF_ADDR_INFO,
    .pblock_size = (1 * MB), // CH0 is 2MB total, split p/v
    .vblock_size = (1 * MB),
    .min_block_size = (512 * B),
    .bit_align = MEMORY_ALIGN_SIZE
  },
  { /* Linux CH1 共享内存配置信息 */
    .zone = LINUX_ZONE_INFO,
    .zone_shm = LINUX_SHM_CH1_BUF_ADDR_INFO,
    .pblock_size = (512 * KB), // CH1 is 1MB total
    .vblock_size = (512 * KB),
    .min_block_size = (512 * B),
    .bit_align = MEMORY_ALIGN_SIZE
  },
  { /* Linux CH2 共享内存配置信息 */
    .zone = LINUX_ZONE_INFO,
    .zone_shm = LINUX_SHM_CH2_BUF_ADDR_INFO,
    .pblock_size = (512 * KB), // CH2 is 1MB total
    .vblock_size = (512 * KB),
    .min_block_size = (512 * B),
    .bit_align = MEMORY_ALIGN_SIZE
  },
  {
    .zone = NULL
  }
};

static struct ShmCfg* shm_cfg_get_by_id(uint16_t target_id)
{
    int i = 0;
    for (i = 0; shm_cfgs[i].zone != NULL; i++)
    {
        if (shm_cfgs[i].zone->id == target_id)
        {
            // printf("shm_cfg_get_by_id_info: find shm cfg success, target id = %u\n", target_id);
            return &shm_cfgs[i];
        }
    }
    return NULL;
}

struct ShmCfgOps shm_cfg_ops = 
{
    .get_by_id = shm_cfg_get_by_id
};

struct ZoneInfo* zone_info = LINUX_ZONE_INFO;


