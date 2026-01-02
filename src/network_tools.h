#ifndef NETWORK_TOOLS_H
#define NETWORK_TOOLS_H

// 必须在 windows.h 之前包含 winsock2.h 以避免 Winsock 1.x 冲突
#include <winsock2.h>
#include <windows.h>

// 任务类型定义，用于主线程识别消息来源
typedef enum {
    TASK_PING = 1,
    TASK_SCAN,
    TASK_SINGLE_SCAN,
    TASK_EXTRACT
} TaskType;

// 用于传递给线程的参数结构
typedef struct {
    HWND hwndNotify;      // 接收消息的主窗口句柄
    char* targetInput;    // 目标字符串 (IP列表或文本)
    char* portsInput;     // 端口字符串
    int retryCount;       // Ping次数
    int timeoutMs;        // 超时时间
} ThreadParams;

// 代理管理
int proxy_set_system(const char* ip, int port);
int proxy_unset_system();
void proxy_init_backup(); // 初始化备份

// 线程入口函数
unsigned int __stdcall thread_ping(void* arg);
unsigned int __stdcall thread_port_scan(void* arg);
unsigned int __stdcall thread_single_scan(void* arg);
unsigned int __stdcall thread_extract_ip(void* arg);

// 辅助函数：释放参数内存
void free_thread_params(ThreadParams* params);

#endif // NETWORK_TOOLS_H
