#include "shm/client.h"
#include "shm/shm.h"
#include "shm/config/config_msgqueue.h"
#include "shm/msgqueue.h"
#include <stdio.h>

static int set_r21_zero(void) {
  // asm volatile("move $r21, $zero");
  asm volatile("mov x18, xzr" : : : "x18");//arm架构为x18
}

// agic for LoongArch64 :-)
static int check_r21_incremented(uint32_t request_idx) {
    uint64_t current_value;

    while (1) {
        // asm volatile("move %0, $r21" : "=r"(current_value));
        asm volatile("mov %0, x18" : "=r"(current_value));//arm架构为x18
        // printf("current_value = %lx\n", current_value);
        if (current_value == request_idx) {
            return 0;
        }
        if (current_value != request_idx) {
            return 1;
        }
    }
    printf("check_r21_incremented error???, can't reach here, check it\n");
    while (1) {
    }
}

static int32_t client_init(struct Client* raw_client, uint32_t remote_zone_id)
{
  ASSERT(raw_client!= NULL);
  // ASSERT(raw_client->init_mark != INIT_MARK_INITIALIZED); // 防止重复初始化
  // ASSERT(raw_client->init_mark != INIT_MARK_DESTORYED);   // 防止销毁后再次初始化

  if (channel_ops.channels_init() != 0)// init all channels
  {
      // raw_client->init_mark = INIT_MARK_DESTORYED;
      printf("client_init_error: channels init fail\n");
      while(1) {}
  } else {
      // printf("client_init_info: channels init success\n");
  }

  /* 获取目标通道 */
  struct Channel* target_channel = channel_ops.target_channel_get(remote_zone_id);

  if (target_channel == NULL) /* 没有获取到目标核心的通道 */
  {
      // raw_client->init_mark = INIT_MARK_DESTORYED;
      printf("client_init_error: get target channel fail = %u\n", remote_zone_id);
      while(1) {}
  } else {
    // printf("client_init_info: get target channel success = %u\n",remote_zone_id);
  }

  while (channel_ops.channel_is_ready(target_channel) != 0) 
  /* 检查目标通道是否已经被启用 */
  {
      // printf("channel is ready????\n");
      // sleep(3);
  }
  // printf("client_init_info: target channel [%u] is ready\n", remote_zone_id);
  
  
  target_channel->msg_queue->working_mark = MSG_QUEUE_MARK_IDLE;
  //!!! reuse the msg_queue->working_mark instead of msg_queue_mutex->msg_queue_mark

  raw_client->remote_channel = target_channel;
  raw_client->msg_cnt = 0U;
  raw_client->shm_cnt = 0U;


  // using shared memory
  if (shm_ops.shm_init() != 0)
  {
    // raw_client->init_mark = INIT_MARK_DESTORYED;
    printf("client_init_error: init shm fail\n");
    while(1) {}
  }
  
  // raw_client->init_mark = INIT_MARK_INITIALIZED;

  set_r21_zero();
  return 0;
}

static int32_t client_destory(struct Client* client)
{
  // printf("client_destory_info: client destory\n");

  ASSERT(client != NULL);

  if (channel_ops.channels_destroy() != 0)// init all channels
  {
      printf("client_destory: channel_destroy fail\n");
      while(1) {}
  } else {
      // printf("client_init_info: channel_destroy success\n");
  }

  if (shm_ops.shm_destory() != 0)
  {
      printf("client_destory: shm_destory fail\n");
      while(1) {}
  }

  // if (client->init_mark == INIT_MARK_INITIALIZED) {
  //   if (client->msg_cnt > 0)
  //   {
  //     printf("client_destory: hold msg\n");
  //     // return -1;
  //     while(1) {}
  //   }
  //   if (client->shm_cnt > 0)
  //   {
  //     printf("client_destory: hold shm\n");
  //     // return -1;
  //     while(1) {}
  //   }
  //   client->init_mark = INIT_MARK_RAW_STATE;
  // }

  return 0;
}

static struct Msg* client_empty_msg_get(struct Client* client, uint32_t remote_service_id)
{
  // printf("client_empty_msg_get_info: client empty msg get\n");
  ASSERT(client != NULL);

  struct Msg* msg = channel_ops.empty_msg_get(client->remote_channel);

  if (msg != NULL)
  {
      msg->service_id = remote_service_id;
      client->msg_cnt++;
  }
  
  return msg;
}

static int32_t client_empty_msg_put(struct Client* client, struct Msg* empty_msg)
{
  // printf("client_empty_msg_put_info: client empty msg put\n");
  ASSERT(client != NULL);
  ASSERT(empty_msg != NULL);

  if (channel_ops.empty_msg_put(client->remote_channel, empty_msg) == 0)
  {
    client->msg_cnt--;
    return 0;
  }
  
  return -1;
}

static int32_t client_msg_send(struct Client* client, struct Msg* full_msg)
{
  ASSERT(client != NULL);
  ASSERT(full_msg != NULL);

  return channel_ops.msg_send(client->remote_channel, full_msg);
}

static int32_t client_msg_send_and_notify(struct Client* client, struct Msg* full_msg)
{ 
  // printf("client msg send and notify enter\n");
  ASSERT(client != NULL);
  ASSERT(full_msg != NULL);

  return channel_ops.msg_send_and_notify(client->remote_channel, full_msg);
}

static int32_t client_msg_notify(struct Client* client)
{
  // printf("client_msg_notify_info: client msg notify\n");
  ASSERT(client != NULL);

  return channel_ops.channel_notify(client->remote_channel);
}

// static int32_t client_msg_poll(struct Msg* aim_msg, uint32_t request_idx)
static int32_t client_msg_poll(struct Msg* aim_msg)
{
  // printf("client_msg_poll_info: client msg poll\n");
  ASSERT(aim_msg != NULL);
  return msg_ops.msg_is_dealt(aim_msg);
  // return check_r21_incremented(request_idx);
}

static void* client_shm_malloc(struct Client* client, uint32_t size, enum MallocType type)
{
  // printf("client_shm_malloc_info: client shm malloc\n");

  void* result = NULL;
  result = shm_ops.shm_malloc(size, type);
  
  if (result != NULL)
  {
    client->shm_cnt++;
  }
  return result;
}

static void 
client_shm_free(struct Client* client, void* ptr)
{
  // printf("client_shm_free_info: client shm free\n");
  if (ptr != NULL)
  {
      client->shm_cnt--;
  }  
  shm_ops.shm_free(ptr);
}


static uint64_t client_shm_addr_to_offset(void* ptr)
{
  return shm_ops.addr_to_offset(ptr);
}

static void* client_shm_offset_to_addr(uint64_t offset)
{
  return shm_ops.offset_to_addr(offset);
}

static uint32_t get_client_request_cnt(struct Client* client)
{ 
    return client->request_cnt;
}

static void set_client_request_cnt(struct Client* client)
{
    client->request_cnt++;
}

struct ClientOps client_ops = 
{
    .client_init = client_init,
    .client_destory = client_destory,
    .empty_msg_get = client_empty_msg_get,
    .empty_msg_put = client_empty_msg_put,
    .msg_send = client_msg_send,
    .msg_send_and_notify = client_msg_send_and_notify,
    .msg_notify = client_msg_notify,
    .msg_poll = client_msg_poll,
    // for shared memory
    .shm_malloc = client_shm_malloc,
    .shm_free = client_shm_free,
    // pay attention to the offset
    .shm_addr_to_offset = client_shm_addr_to_offset,
    .shm_offset_to_addr = client_shm_offset_to_addr,
    .get_client_request_cnt = get_client_request_cnt,
    .set_client_request_cnt = set_client_request_cnt
};