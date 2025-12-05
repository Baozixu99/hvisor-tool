#ifndef _MULTITHREAD_CLIENT_H_
#define _MULTITHREAD_CLIENT_H_

/**
 * HyperAMP 多线程客户端接口
 * 
 * 提供多线程并发发送请求的功能
 */

/**
 * 多线程客户端测试
 * 
 * @param argc 参数数量
 * @param argv 参数数组
 *   argv[0]: 配置文件路径 (shm_config.json)
 *   argv[1]: 数据内容
 *   argv[2]: 服务ID
 *   argv[3]: 线程数量
 *   argv[4]: 请求总数(可选,默认=线程数)
 * @return 0成功, -1失败
 */
int hyper_amp_client_test_multithread(int argc, char* argv[]);

#endif // _MULTITHREAD_CLIENT_H_
