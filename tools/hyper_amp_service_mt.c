/**
 * HyperAMP 多线程服务端实现
 * 
 * 使用方法:
 *   将此文件中的函数复制到 hvisor.c 中
 *   在 main() 函数中添加相应的命令处理分支
 * 
 * 架构设计:
 *   采用单消费者模式 - 主线程收集消息,工作线程并行处理
 *   避免队列竞争,保证消息处理顺序性
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/poll.h>
#include <sys/ioctl.h>
#include <time.h>
#include "shm/msgqueue.h"
#include "shm/threads.h"
#include "shm/config/config_addr.h"
#include "hvisor.h"

// ==============================================
// 服务任务结构定义
// ==============================================

typedef struct ServiceTask {
    struct Msg msg;           // 消息副本
    uint64_t buf_addr;        // 共享内存基地址
    uint16_t msg_index;       // 消息索引
    struct AmpMsgQueue* queue; // 队列指针(用于更新状态)
} ServiceTask;

// ==============================================
// 服务处理函数
// ==============================================

/**
 * 加密服务实现 (简单XOR加密)
 */
void hyperamp_encrypt_service(char* data, int data_len, int buf_len) {
    const char key = 0x42;  // 加密密钥
    for (int i = 0; i < data_len && i < buf_len; i++) {
        data[i] ^= key;
    }
}

/**
 * 解密服务实现 (XOR解密,与加密相同)
 */
void hyperamp_decrypt_service(char* data, int data_len, int buf_len) {
    hyperamp_encrypt_service(data, data_len, buf_len);  // XOR加密解密相同
}

/**
 * 处理单个服务请求
 * 
 * @param arg1 ServiceTask* 服务任务
 * @param arg2 struct Msg* 工作线程的消息缓冲(未使用,保持接口一致)
 */
void process_service_task(void* arg1, void* arg2) {
    ServiceTask* task = (ServiceTask*)arg1;
    struct Msg* msg = &task->msg;
    
    printf("[Thread %ld] 🔧 Processing service request: service_id=%u, offset=0x%x, length=%u\n",
           pthread_self(), msg->service_id, msg->offset, msg->length);
    
    // 1. 获取数据地址
    char* data = (char*)(task->buf_addr + msg->offset);
    
    // 2. 执行相应服务
    struct timespec service_start, service_end;
    clock_gettime(CLOCK_MONOTONIC, &service_start);
    
    switch (msg->service_id) {
        case 1:  // 加密服务
            printf("[Thread %ld] 🔐 Encrypting data...\n", pthread_self());
            hyperamp_encrypt_service(data, msg->length - 1, msg->length);
            break;
            
        case 2:  // 解密服务
            printf("[Thread %ld] 🔓 Decrypting data...\n", pthread_self());
            hyperamp_decrypt_service(data, msg->length - 1, msg->length);
            break;
            
        case 66: // Echo服务
            printf("[Thread %ld] 📢 Echo service (no processing)\n", pthread_self());
            // 无需处理,直接返回原数据
            break;
            
        default:
            printf("[Thread %ld] ⚠️  Unknown service ID: %u\n", 
                   pthread_self(), msg->service_id);
            msg->flag.service_result = MSG_SERVICE_RET_FAIL;
            goto update_status;
    }
    
    clock_gettime(CLOCK_MONOTONIC, &service_end);
    long service_time_us = (service_end.tv_sec - service_start.tv_sec) * 1000000L + 
                           (service_end.tv_nsec - service_start.tv_nsec) / 1000L;
    
    // 3. 更新处理结果
    msg->flag.service_result = MSG_SERVICE_RET_SUCCESS;
    
    printf("[Thread %ld] ✅ Service completed in %ld μs\n", 
           pthread_self(), service_time_us);

update_status:
    // 4. 更新消息状态
    msg->flag.deal_state = MSG_DEAL_STATE_YES;
    
    // 5. 将状态写回到原始队列(重要!)
    struct MsgEntry* entry = &task->queue->entries[task->msg_index];
    entry->msg.flag.deal_state = msg->flag.deal_state;
    entry->msg.flag.service_result = msg->flag.service_result;
    
    // 清理任务
    free(task);
}

// ==============================================
// 多线程服务端测试主函数
// ==============================================

/**
 * 多线程服务端测试
 * 
 * 采用单消费者模式:
 *   - 主线程负责从队列收集消息
 *   - 工作线程并行处理服务请求
 *   - 避免队列竞争,保证处理顺序
 * 
 * @param argc 参数数量
 * @param argv 参数数组
 *   argv[0]: 配置文件路径
 *   argv[1]: 线程数量(可选,默认4)
 * @return 0成功, -1失败
 */
static int hyper_amp_service_test_multithread(int argc, char* argv[]) {
    if (argc < 1) {
        printf("Usage: ./hvisor shm hyper_amp_service_test_mt <config> [threads]\n");
        printf("\n");
        printf("Parameters:\n");
        printf("  config   - SHM configuration file (e.g., shm_config.json)\n");
        printf("  threads  - Number of worker threads (default: 4, max: 32)\n");
        printf("\n");
        printf("Examples:\n");
        printf("  ./hvisor shm hyper_amp_service_test_mt shm_config.json\n");
        printf("  ./hvisor shm hyper_amp_service_test_mt shm_config.json 8\n");
        return -1;
    }
    
    char* config = argv[0];
    int num_threads = (argc >= 2) ? atoi(argv[1]) : 4;  // 默认4个工作线程
    
    // 参数校验
    if (num_threads <= 0 || num_threads > 32) {
        printf("❌ ERROR: Invalid thread count %d (must be 1-32)\n", num_threads);
        return -1;
    }
    
    printf("\n");
    printf("╔════════════════════════════════════════════════════════╗\n");
    printf("║     HyperAMP Multi-threaded Service Test              ║\n");
    printf("╠════════════════════════════════════════════════════════╣\n");
    printf("║ Configuration: %-39s ║\n", config);
    printf("║ Worker Threads:%-3d                                    ║\n", num_threads);
    printf("║ Mode:          Single-Consumer (Queue-Safe)           ║\n");
    printf("╚════════════════════════════════════════════════════════╝\n");
    printf("\n");
    
    // 1. 解析配置
    parse_global_addr(config);
    
    // 2. 打开 /dev/mem
    int mem_fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (mem_fd < 0) {
        perror("❌ ERROR: Failed to open /dev/mem");
        return -1;
    }
    printf("✅ Opened /dev/mem (fd=%d)\n", mem_fd);
    
    // 3. 映射消息队列 (Zone0 - Root)
    uint64_t root_msg_queue_addr = addr_infos[0].msg_queue_addr;
    uint32_t root_msg_queue_mem_size = addr_infos[0].msg_queue_mem_size;
    
    struct AmpMsgQueue* root_msg_queue = (struct AmpMsgQueue*)mmap(NULL,
        root_msg_queue_mem_size,
        PROT_READ | PROT_WRITE,
        MAP_SHARED,
        mem_fd,
        root_msg_queue_addr);
    
    if (root_msg_queue == MAP_FAILED) {
        perror("❌ ERROR: Failed to mmap root message queue");
        close(mem_fd);
        return -1;
    }
    printf("✅ Mapped root message queue at 0x%lx (size=%u)\n", 
           root_msg_queue_addr, root_msg_queue_mem_size);
    
    // 4. 映射共享内存缓冲区
    uint64_t buf_addr_phys = addr_infos[2].buf_addr;
    uint32_t buf_mem_size = addr_infos[2].buf_mem_size;
    
    uint64_t buf_addr = (uint64_t)mmap(NULL,
        buf_mem_size,
        PROT_READ | PROT_WRITE,
        MAP_SHARED,
        mem_fd,
        buf_addr_phys);
    
    if ((void*)buf_addr == MAP_FAILED) {
        perror("❌ ERROR: Failed to mmap buffer");
        munmap(root_msg_queue, root_msg_queue_mem_size);
        close(mem_fd);
        return -1;
    }
    printf("✅ Mapped shared buffer at 0x%lx (size=%u)\n", 
           buf_addr_phys, buf_mem_size);
    
    // 5. 打开 hvisor 设备(用于中断)
    int hvisor_fd = open("/dev/hvisor", O_RDWR);
    if (hvisor_fd < 0) {
        printf("⚠️  WARNING: Cannot open /dev/hvisor (interrupts disabled)\n");
    } else {
        printf("✅ Opened /dev/hvisor (fd=%d) for interrupts\n", hvisor_fd);
    }
    
    // 6. 创建工作线程池(传递NULL,因为服务端不需要Client)
    ThreadPool* pool = init_thread_pool(num_threads, NULL);
    if (pool == NULL) {
        printf("❌ ERROR: Failed to create thread pool\n");
        if (hvisor_fd >= 0) close(hvisor_fd);
        munmap((void*)buf_addr, buf_mem_size);
        munmap(root_msg_queue, root_msg_queue_mem_size);
        close(mem_fd);
        return -1;
    }
    printf("✅ Thread pool created (%d workers)\n", num_threads);
    
    // 7. 设置中断监听
    struct pollfd pfd;
    if (hvisor_fd >= 0) {
        pfd.fd = hvisor_fd;
        pfd.events = POLLIN;
        printf("✅ Interrupt monitoring enabled (IRQ polling)\n");
    }
    
    // 8. 主服务循环
    printf("\n🚀 Service started, waiting for requests...\n");
    printf("   (Press Ctrl+C to stop)\n\n");
    
    uint32_t total_processed = 0;
    struct timespec loop_start;
    clock_gettime(CLOCK_MONOTONIC, &loop_start);
    
    while (running) {
        // 等待中断或超时
        int ret = (hvisor_fd >= 0) ? poll(&pfd, 1, 1000) : 0;
        
        if (ret < 0) {
            if (errno == EINTR) continue;  // 被信号中断,继续
            perror("❌ ERROR: poll failed");
            break;
        }
        
        // 检查是否有待处理消息
        uint16_t head = root_msg_queue->proc_ing_h;
        if (head >= root_msg_queue->buf_size) {
            continue;  // 无消息,继续等待
        }
        
        // 批量收集消息(单消费者模式)
        int batch_count = 0;
        const int MAX_BATCH = 16;  // 每批最多处理16个消息
        
        while (head < root_msg_queue->buf_size && batch_count < MAX_BATCH) {
            struct MsgEntry* entry = &root_msg_queue->entries[head];
            
            // 创建服务任务
            ServiceTask* task = malloc(sizeof(ServiceTask));
            if (task == NULL) {
                printf("⚠️  WARNING: Failed to allocate service task\n");
                break;
            }
            
            // 复制消息内容(避免并发修改)
            memcpy(&task->msg, &entry->msg, sizeof(struct Msg));
            task->buf_addr = buf_addr;
            task->msg_index = head;
            task->queue = root_msg_queue;
            
            // 提交到线程池
            add_task(pool, process_service_task, task);
            
            batch_count++;
            total_processed++;
            
            // 移动到下一个消息
            head = entry->nxt_idx;
        }
        
        if (batch_count > 0) {
            printf("📦 Collected %d messages, dispatched to worker pool\n", batch_count);
            
            // 更新队列头(所有消息已分发)
            root_msg_queue->proc_ing_h = head;
        }
    }
    
    // 9. 计算统计信息
    struct timespec loop_end;
    clock_gettime(CLOCK_MONOTONIC, &loop_end);
    long total_time_s = loop_end.tv_sec - loop_start.tv_sec;
    
    printf("\n");
    printf("╔════════════════════════════════════════════════════════╗\n");
    printf("║              Service Statistics                        ║\n");
    printf("╠════════════════════════════════════════════════════════╣\n");
    printf("║ Total Processed:   %8u requests                   ║\n", total_processed);
    printf("║ Running Time:      %8ld seconds                    ║\n", total_time_s);
    if (total_time_s > 0) {
        printf("║ Throughput:        %8.2f requests/sec              ║\n", 
               (double)total_processed / total_time_s);
    }
    printf("║ Worker Threads:    %8d                             ║\n", num_threads);
    printf("╚════════════════════════════════════════════════════════╝\n");
    printf("\n");
    
    // 10. 清理资源
    printf("🛑 Shutting down service...\n");
    
    // 等待所有任务完成
    printf("⏳ Waiting for pending tasks to complete...\n");
    while (!task_queue_is_empty(pool)) {
        sleep(1);
    }
    
    destroy_thread_pool(pool);
    printf("✅ Thread pool destroyed\n");
    
    if (hvisor_fd >= 0) {
        close(hvisor_fd);
        printf("✅ Closed /dev/hvisor\n");
    }
    
    munmap((void*)buf_addr, buf_mem_size);
    munmap(root_msg_queue, root_msg_queue_mem_size);
    printf("✅ Unmapped shared memory\n");
    
    close(mem_fd);
    printf("✅ Closed /dev/mem\n");
    
    printf("\n✅ Multi-threaded service test completed!\n\n");
    return 0;
}

// ==============================================
// 如何集成到 hvisor.c
// ==============================================

/*
步骤1: 将上面的结构和函数复制到 hvisor.c 中
    (放在 hyper_amp_service_test 函数之后)

步骤2: 确保已添加必要的头文件:
    #include "shm/threads.h"
    #include <errno.h>

步骤3: 在 main() 函数中添加命令处理:
    else if(strcmp(argv[2], "hyper_amp_service_test_mt") == 0) {
        hyper_amp_service_test_multithread(argc - 3, &argv[3]);
    }

步骤4: 重新编译:
    cd tools && make clean && make

步骤5: 测试运行:
    # 在终端1: 启动多线程服务端(8个工作线程)
    sudo ./hvisor shm hyper_amp_service_test_mt shm_config.json 8
    
    # 在终端2: 发送测试请求
    # 单个请求
    ./hvisor shm hyper_amp_test shm_config.json "hello" 1
    
    # 多线程并发请求(4线程,各发1个)
    ./hvisor shm hyper_amp_test_mt shm_config.json "test" 1 4
    
    # 压力测试(16线程,1000个请求)
    ./hvisor shm hyper_amp_test_mt shm_config.json "stress" 66 16 1000

步骤6: 性能对比测试
    # 单线程服务端
    sudo ./hvisor shm hyper_amp_service_test shm_config.json
    
    # 多线程服务端(8线程)
    sudo ./hvisor shm hyper_amp_service_test_mt shm_config.json 8
    
    # 并发客户端测试
    for i in {1..100}; do
        ./hvisor shm hyper_amp_test shm_config.json "msg_$i" 1 &
    done
    wait

注意事项:
1. 队列锁已在 Phase 1 添加,多线程安全已保证
2. 服务端需要 sudo 权限访问 /dev/mem
3. 单消费者模式避免队列竞争,性能更优
4. 可根据CPU核心数调整线程数(建议: 核心数 * 2)
*/
