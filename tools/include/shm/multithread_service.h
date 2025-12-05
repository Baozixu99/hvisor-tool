#ifndef _MULTITHREAD_SERVICE_H_
#define _MULTITHREAD_SERVICE_H_

/**
 * HyperAMP 多线程服务端接口
 * 
 * 提供多线程并发处理请求的功能
 * 采用单消费者模式设计
 */

/**
 * 多线程服务端测试
 * 
 * @param argc 参数数量
 * @param argv 参数数组
 *   argv[0]: 配置文件路径
 *   argv[1]: 线程数量(可选,默认4)
 * @return 0成功, -1失败
 */
int hyper_amp_service_test_multithread(int argc, char* argv[]);

#endif // _MULTITHREAD_SERVICE_H_
