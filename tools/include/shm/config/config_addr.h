#ifndef _CONFIG_ADDR_H_
#define _CONFIG_ADDR_H_

struct AddrInfo
{
  uint64_t start; /* 起始地址：兼容32/64位 */
  uint32_t len;   /* 长度 */
};

// TODO: define a global addr_infos array (get from .json)
// .json just allocate and map region 
// the actual addr should be written in the .h and .c

/* 所有核间通信可能会用到的内存块地址信息 */
extern struct AddrInfo addr_infos[];

// Channel 0 (Existing)
#define LINUX_SHM_BUF_ADDR_INFO               (&addr_infos[0])
#define LINUX_2_NPUCore_MSG_QUEUE_ADDR_INFO   (&addr_infos[1])
#define NPUCore_2_NPUCore_MSG_QUEUE_ADDR_INFO (&addr_infos[2])

// Channel 1
#define LINUX_SHM_CH1_BUF_ADDR_INFO               (&addr_infos[3])
#define LINUX_2_NPUCore_CH1_MSG_QUEUE_ADDR_INFO   (&addr_infos[4])
#define NPUCore_2_NPUCore_CH1_MSG_QUEUE_ADDR_INFO (&addr_infos[5])

// Channel 2
#define LINUX_SHM_CH2_BUF_ADDR_INFO               (&addr_infos[6])
#define LINUX_2_NPUCore_CH2_MSG_QUEUE_ADDR_INFO   (&addr_infos[7])
#define NPUCore_2_NPUCore_CH2_MSG_QUEUE_ADDR_INFO (&addr_infos[8])

#endif // _CONFIG_ADDR_H_