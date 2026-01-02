#ifndef NETWORK_TOOLS_H
#define NETWORK_TOOLS_H

#include <winsock2.h>
#include <windows.h>
#include <tchar.h>

// 消息定义
#define WM_USER_LOG     (WM_USER + 100) 
#define WM_USER_RESULT  (WM_USER + 101) 
#define WM_USER_FINISH  (WM_USER + 102) 

typedef enum {
    TASK_PING = 1,
    TASK_SCAN,
    TASK_SINGLE_SCAN,
    TASK_EXTRACT
} TaskType;

typedef struct {
    HWND hwndNotify;
    wchar_t* targetInput;  
    wchar_t* portsInput;   
    int retryCount;
    int timeoutMs;
} ThreadParams;

// 任务控制
void signal_stop_task(); // 发送停止信号
void reset_stop_task();  // 重置停止信号

// 代理管理
int proxy_set_system(const wchar_t* ip, int port);
int proxy_unset_system();
void proxy_init_backup(); 

// 线程入口
unsigned int __stdcall thread_ping(void* arg);
unsigned int __stdcall thread_port_scan(void* arg);
unsigned int __stdcall thread_single_scan(void* arg);
unsigned int __stdcall thread_extract_ip(void* arg);

void free_thread_params(ThreadParams* params);

#endif // NETWORK_TOOLS_H
