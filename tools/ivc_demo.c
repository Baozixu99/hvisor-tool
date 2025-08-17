// SPDX-License-Identifier: GPL-2.0-only
/**
 * Copyright (c) 2025 Syswonder
 *
 * Syswonder Website:
 * https://www.syswonder.org
 *
 * Authors:
 * Guowei Li <2401213322@stu.pku.edu.cn>
 */
#include "ivc.h"
#include <assert.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <time.h> // 新增头文件

volatile char *out, *in;
struct pollfd pfd;

static int open_dev() {
    int fd = open("/dev/hivc0", O_RDWR);
    if (fd < 0) {
        perror("open hvisor failed");
        exit(1);
    }
    return fd;
}

int main(int argc, char *argv[]) {
    printf("ivc_demo: starting\n");
    int fd, err, sig, ivc_id, is_send = 0, ret;
    ivc_uinfo_t ivc_info;
    void *tb_virt, *mem_virt;
    unsigned long long ct_ipa, offset;

    if (argc != 2) {
        printf("Usage: ivc_demo send|receive\n");
        return -1;
    }

    if (strcmp(argv[1], "send") == 0) {
        is_send = 1;
    } else if (strcmp(argv[1], "receive") == 0) {
        is_send = 0;
    } else {
        printf("Usage: ivc_demo send|receive\n");
        return -1;
    }

    fd = open_dev();

    pfd.fd = fd;
    pfd.events = POLLIN;
    tb_virt = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    offset = 0x1000;

    ivc_cttable_t *tb = (ivc_cttable_t *)tb_virt;
    printf("ivc_id: %d, max_peers: %d\n", tb->ivc_id, tb->max_peers);

    if (is_send) {
        out = mmap(NULL, tb->out_sec_size, PROT_READ | PROT_WRITE, MAP_SHARED,
                    fd, offset);
        offset += tb->out_sec_size;
        in = mmap(NULL, tb->out_sec_size, PROT_READ, MAP_SHARED, fd, offset);
        char *msg = "hello zone1! I'm zone0.";
        
        // 性能测试：记录客户端发送开始时间
        struct timespec start_time, end_time;
        clock_gettime(CLOCK_MONOTONIC, &start_time); // 计时起点

        strcpy(out, msg); // 将写共享内存的操作包含在计时范围内
        tb->ipi_invoke = 1;
        printf("ivc_demo: zone0 sent: %s\n", out);

        ret = poll(&pfd, 1, -1);
        if (pfd.revents & POLLIN){
            // 性能测试：记录客户端接收结束时间 
            clock_gettime(CLOCK_MONOTONIC, &end_time);
            long latency_us = (end_time.tv_sec - start_time.tv_sec) * 1000000L + 
                             (end_time.tv_nsec - start_time.tv_nsec) / 1000L;
            
            printf("ivc_demo: zone0 received: %s\n", in);
            printf("\n=== IVC_DEMO PERFORMANCE RESULTS ===\n");
            printf("[PERF] Total Round-Trip Latency: %ld us (%.3f ms)\n", 
                   latency_us, latency_us / 1000.0);
            printf("=====================================\n");
        } else {
            printf("ivc_demo: zone0 poll failed, ret is %d\n", ret);
        }
    } else {
        in = mmap(NULL, tb->out_sec_size, PROT_READ, MAP_SHARED, fd, offset);
        offset += tb->out_sec_size;
        out = mmap(NULL, tb->out_sec_size, PROT_READ | PROT_WRITE, MAP_SHARED,
                     fd, offset);
        
        ret = poll(&pfd, 1, -1);
        if (pfd.revents & POLLIN) {
            // 性能测试：记录服务端接收开始时间（在poll返回后）
            struct timespec service_start_time, service_end_time;
            clock_gettime(CLOCK_MONOTONIC, &service_start_time);
            
            printf("ivc_demo: zone1 received: %s\n", in);
            
            // 这里可以添加任何服务端处理数据的逻辑，例如：
            // int sum = 0; for(int i=0; i<10000; i++) sum+=i;
            
            strcpy(out, "I'm zone1. hello zone0! ");
            tb->ipi_invoke = 0; // 触发中断发送，通知客户端
            
            // 性能测试：记录服务端处理结束时间（在发送响应前）
            clock_gettime(CLOCK_MONOTONIC, &service_end_time);
            long service_time_us = (service_end_time.tv_sec - service_start_time.tv_sec) * 1000000L + 
                                   (service_end_time.tv_nsec - service_start_time.tv_nsec) / 1000L;
            
            printf("ivc_demo: zone1 sent: %s\n", out);
            printf("\n=== IVC_DEMO SERVICE PERFORMANCE ===\n");
            printf("[PERF-SERVICE] Processing Time: %ld us (%.3f ms)\n", 
                   service_time_us, service_time_us / 1000.0);
            printf("====================================\n");
        } else {
            printf("ivc_demo: zone1 poll failed, ret is %d\n", ret);
        }
    }

    close(fd);
    munmap(in, tb->out_sec_size);
    munmap(out, tb->out_sec_size);
    munmap(tb_virt, 0x1000);
    return 0;
}