#ifndef NETWORK_TOOLS_H
#define NETWORK_TOOLS_H

// === 核心修复：必须在 windows.h 之前包含 winsock2.h ===
#include <winsock2.h>
#include <windows.h>

// === 修复：将消息定义移至头文件，供 main.c 和 network_tools.c 共享 ===
#define WM_USER_LOG     (WM_USER + 100) // wParam=Progress, lParam=StringPtr
#define WM_USER_RESULT  (WM_USER + 101) // wParam=0, lParam=StringPtr (CSV row)
#define WM_USER_FINISH  (WM_USER + 102) // wParam=0, lParam=MessageString

// 任务类型定义
typedef enum {
    TASK_PING = 1,
    TASK_SCAN,
    TASK_SINGLE_SCAN,
    TASK_EXTRACT
} TaskType;

// 线程参数结构
typedef struct {
    HWND hwndNotify;      // 接收消息的主窗口句柄
    char* targetInput;    // 目标字符串
    char* portsInput;     // 端口字符串
    int retryCount;       // Ping次数
    int timeoutMs;        // 超时时间
} ThreadParams;

// 代理管理
int proxy_set_system(const char* ip, int port);
int proxy_unset_system();
void proxy_init_backup(); 

// 线程入口
unsigned int __stdcall thread_ping(void* arg);
unsigned int __stdcall thread_port_scan(void* arg);
unsigned int __stdcall thread_single_scan(void* arg);
unsigned int __stdcall thread_extract_ip(void* arg);

// 辅助
void free_thread_params(ThreadParams* params);

#endif // NETWORK_TOOLS_H
