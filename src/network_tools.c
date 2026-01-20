#include "network_tools.h"
#include "network_modules.h" // 引入新模块
#include <stdio.h>
#include <stdlib.h>
#include <process.h>

#pragma comment(lib, "wininet.lib")

static DWORD g_originalProxyEnable = 0;
static wchar_t g_originalProxyServer[256] = {0};
static int g_hasBackup = 0;
static volatile int g_stopSignal = 0;

void signal_stop_task() { g_stopSignal = 1; }
void reset_stop_task() { g_stopSignal = 0; }
int is_task_stopped() { return g_stopSignal; }

// --- 实现共享辅助函数 ---
char* wide_to_ansi(const wchar_t* wstr) {
    if (!wstr) return NULL;
    int len = WideCharToMultiByte(CP_ACP, 0, wstr, -1, NULL, 0, NULL, NULL);
    char* str = (char*)malloc(len);
    if(str) WideCharToMultiByte(CP_ACP, 0, wstr, -1, str, len, NULL, NULL);
    return str;
}

void gbk_to_wide(const char* gbk, wchar_t* buf, int bufLen) {
    if (!gbk || !buf) return;
    MultiByteToWideChar(CP_ACP, 0, gbk, -1, buf, bufLen);
}

// 辅助：解析域名/IP，优先返回 IPv4，支持 IPv6
// 返回: 0=失败, 4=IPv4, 6=IPv6. 结果存入 addrOut (sockaddr_in 或 sockaddr_in6)
int resolve_host(const wchar_t* host, void* addrOut) {
    struct addrinfoW hints = {0};
    hints.ai_family = AF_UNSPEC; // 允许 v4 和 v6
    hints.ai_socktype = SOCK_STREAM;
    
    struct addrinfoW* result = NULL;
    if (GetAddrInfoW(host, NULL, &hints, &result) != 0) return 0;

    int type = 0;
    // 策略：如果有 IPv4，优先使用 IPv4 (保持旧版兼容性)，否则使用 IPv6
    struct addrinfoW* ptr = result;
    struct addrinfoW* v6Res = NULL;

    while (ptr != NULL) {
        if (ptr->ai_family == AF_INET) {
            memcpy(addrOut, ptr->ai_addr, sizeof(struct sockaddr_in));
            type = 4;
            break; 
        } else if (ptr->ai_family == AF_INET6) {
            if (!v6Res) v6Res = ptr; // 记录第一个 v6 结果备用
        }
        ptr = ptr->ai_next;
    }

    if (type == 0 && v6Res) {
        memcpy(addrOut, v6Res->ai_addr, sizeof(struct sockaddr_in6));
        type = 6;
    }

    FreeAddrInfoW(result);
    return type;
}

// --- 保持原有的字符串处理辅助 ---
wchar_t** split_hosts(const wchar_t* input, int* count) {
    if (!input) { *count = 0; return NULL; }
    wchar_t* copy = _wcsdup(input);
    int capacity = 10;
    wchar_t** list = (wchar_t**)malloc(sizeof(wchar_t*) * capacity);
    int n = 0;
    wchar_t* context = NULL;
    wchar_t* token = wcstok_s(copy, L" \t\n\r,", &context);
    while (token) {
        if (n >= capacity) {
            capacity *= 2;
            list = (wchar_t**)realloc(list, sizeof(wchar_t*) * capacity);
        }
        list[n++] = _wcsdup(token);
        token = wcstok_s(NULL, L" \t\n\r,", &context);
    }
    free(copy);
    *count = n;
    return list;
}

void free_string_list(wchar_t** list, int count) {
    if(!list) return;
    for (int i = 0; i < count; i++) free(list[i]);
    free(list);
}

int* parse_ports(const wchar_t* portStr, int* count) {
    if (!portStr) { *count = 0; return NULL; }
    int* ports = (int*)malloc(sizeof(int) * 65535); 
    int n = 0;
    wchar_t* copy = _wcsdup(portStr);
    wchar_t* context = NULL;
    wchar_t* token = wcstok_s(copy, L",", &context);
    while (token) {
        if (wcschr(token, L'-')) {
            int start, end;
            if (swscanf_s(token, L"%d-%d", &start, &end) == 2) {
                for (int i = start; i <= end; i++) ports[n++] = i;
            }
        } else {
            int p = _wtoi(token);
            if (p > 0) ports[n++] = p;
        }
        token = wcstok_s(NULL, L",", &context);
    }
    free(copy);
    *count = n;
    return ports;
}

// --- UI 消息辅助 ---
void post_result(HWND hwnd, const wchar_t* col1, const wchar_t* col2, const wchar_t* col3, const wchar_t* col4, const wchar_t* col5, const wchar_t* col6) {
    wchar_t buffer[1024];
    swprintf_s(buffer, 1024, L"%s|%s|%s|%s|%s|%s", 
             col1 ? col1 : L"", col2 ? col2 : L"", col3 ? col3 : L"", 
             col4 ? col4 : L"", col5 ? col5 : L"", col6 ? col6 : L"-");
    wchar_t* msg = _wcsdup(buffer);
    PostMessageW(hwnd, WM_USER_RESULT, 0, (LPARAM)msg);
}

void post_log(HWND hwnd, int progress, const wchar_t* text) {
    wchar_t* msg = _wcsdup(text);
    PostMessageW(hwnd, WM_USER_LOG, (WPARAM)progress, (LPARAM)msg);
}

void post_finish(HWND hwnd, const wchar_t* msg) {
    PostMessageW(hwnd, WM_USER_FINISH, 0, (LPARAM)_wcsdup(msg));
}

// --- 任务线程逻辑 (重构后使用模块) ---

unsigned int __stdcall thread_ping(void* arg) {
    ThreadParams* p = (ThreadParams*)arg;
    int count;
    wchar_t** hosts = split_hosts(p->targetInput, &count);
    HWND hwnd = p->hwndNotify; 

    // 初始化 v4 IP库
    if (p->showLocation) ipv4_init_qqwry();

    for (int i = 0; i < count; i++) {
        if (g_stopSignal) { post_log(hwnd, 0, L"任务已中止"); break; }

        wchar_t statusMsg[256];
        swprintf_s(statusMsg, 256, L"正在 Ping (%d/%d): %s...", i + 1, count, hosts[i]);
        post_log(hwnd, (i * 100) / count, statusMsg);

        // 解析地址 (支持 IPv4 和 IPv6)
        union {
            struct sockaddr_in v4;
            struct sockaddr_in6 v6;
        } addr;
        
        int type = resolve_host(hosts[i], &addr);
        
        wchar_t location[256] = {0};
        int success = 0;
        long avgRtt = 0;
        int ttl = 0;

        if (type == 4) {
            // 获取 IPv4 归属地
            if (p->showLocation) {
                 ipv4_get_location(inet_ntoa(addr.v4.sin_addr), location, 256);
            }
            // 调用 v4 模块
            success = ipv4_ping_host(addr.v4.sin_addr.s_addr, p->retryCount, p->timeoutMs, &avgRtt, &ttl);
        } 
        else if (type == 6) {
            // IPv6 暂不支持归属地，显示占位
            wcscpy_s(location, 256, L"IPv6地址");
            // 调用 v6 模块
            success = ipv6_ping_host(&addr.v6, p->retryCount, p->timeoutMs, &avgRtt, &ttl);
        }
        else {
             post_result(hwnd, hosts[i], L"无效地址", L"N/A", L"100", L"N/A", L"未知");
             continue;
        }

        if (g_stopSignal) break;

        wchar_t rttStr[32], lossStr[32], ttlStr[32];
        if (success) {
            swprintf_s(rttStr, 32, L"%ld", avgRtt);
            swprintf_s(lossStr, 32, L"0"); // 简化：模块返回成功即视为连通，不计算丢包率
            swprintf_s(ttlStr, 32, L"%d", ttl);
            post_result(hwnd, hosts[i], L"在线", rttStr, lossStr, ttlStr, location);
        } else {
            post_result(hwnd, hosts[i], L"超时", L"N/A", L"100", L"N/A", location);
        }
    }

    free_string_list(hosts, count);
    if (p->showLocation) ipv4_cleanup_qqwry();
    free_thread_params(p);
    
    if (g_stopSignal) post_finish(hwnd, L"任务已由用户中止。");
    else post_finish(hwnd, L"批量 Ping 任务完成。");
    return 0;
}

unsigned int __stdcall thread_port_scan(void* arg) {
    ThreadParams* p = (ThreadParams*)arg;
    int hostCount, portCount;
    wchar_t** hosts = split_hosts(p->targetInput, &hostCount);
    int* ports = parse_ports(p->portsInput, &portCount);
    HWND hwnd = p->hwndNotify;

    if (p->showLocation) ipv4_init_qqwry();

    int total = hostCount * portCount;
    int current = 0;

    for (int i = 0; i < hostCount; i++) {
        if (g_stopSignal) break; 
        
        union {
            struct sockaddr_in v4;
            struct sockaddr_in6 v6;
        } addr;
        
        int type = resolve_host(hosts[i], &addr);
        wchar_t location[256] = {0};

        if (type == 4 && p->showLocation) {
            ipv4_get_location(inet_ntoa(addr.v4.sin_addr), location, 256);
        } else if (type == 6) {
             wcscpy_s(location, 256, L"IPv6地址");
        }

        for (int j = 0; j < portCount; j++) {
            if (g_stopSignal) break; 
            current++;
            int port = ports[j];
            wchar_t msg[256];
            swprintf_s(msg, 256, L"扫描 (%d/%d): %s:%d", current, total, hosts[i], port);
            post_log(hwnd, (current * 100) / (total ? total : 1), msg);

            int open = 0;
            if (type == 4) {
                open = ipv4_tcp_scan(addr.v4.sin_addr.s_addr, port, 2000); // 2秒超时
            } else if (type == 6) {
                open = ipv6_tcp_scan(&addr.v6, port, 2000);
            }

            if (open) {
                wchar_t portStr[16];
                swprintf_s(portStr, 16, L"%d", port);
                post_result(hwnd, hosts[i], portStr, L"开放 (Open)", location, L"", L"");
            } 
        }
    }

    free_string_list(hosts, hostCount);
    free(ports);
    if (p->showLocation) ipv4_cleanup_qqwry();
    free_thread_params(p);
    
    if (g_stopSignal) post_finish(hwnd, L"任务已由用户中止。");
    else post_finish(hwnd, L"批量端口扫描完成。");
    return 0;
}

unsigned int __stdcall thread_extract_ip(void* arg) {
    ThreadParams* p = (ThreadParams*)arg;
    HWND hwnd = p->hwndNotify;
    
    if (p->showLocation) ipv4_init_qqwry();

    post_log(hwnd, 0, L"正在分析文本...");
    
    // 依次执行 V4 和 V6 提取
    ipv4_extract_search(p->targetInput, hwnd, p->showLocation);
    ipv6_extract_search(p->targetInput, hwnd);

    if (p->showLocation) ipv4_cleanup_qqwry();
    free_thread_params(p);
    
    if (g_stopSignal) post_finish(hwnd, L"任务已由用户中止。");
    else post_finish(hwnd, L"IP 提取完成。");
    return 0;
}

unsigned int __stdcall thread_single_scan(void* arg) {
    return thread_port_scan(arg); 
}

void free_thread_params(ThreadParams* params) {
    if (params) {
        if (params->targetInput) free(params->targetInput);
        if (params->portsInput) free(params->portsInput);
        free(params);
    }
}

// ... Proxy 代码保持原样，无需修改 ...
// (省略 Proxy 相关代码，因其仅涉及注册表操作且原代码未变，请保留原有实现)
HKEY open_internet_settings(REGSAM access) {
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Internet Settings", 0, access, &hKey) == ERROR_SUCCESS) {
        return hKey;
    }
    return NULL;
}

void refresh_internet_settings() {
    HMODULE hWininet = LoadLibraryW(L"wininet.dll");
    if (hWininet) {
        typedef BOOL (WINAPI *ISO)(HINTERNET, DWORD, LPVOID, DWORD);
        ISO pInternetSetOption = (ISO)GetProcAddress(hWininet, "InternetSetOptionW");
        if (pInternetSetOption) {
            pInternetSetOption(NULL, 39, NULL, 0); 
            pInternetSetOption(NULL, 37, NULL, 0); 
        }
        FreeLibrary(hWininet);
    }
}

void proxy_init_backup() {
    if (g_hasBackup) return;
    HKEY hKey = open_internet_settings(KEY_READ);
    if (hKey) {
        DWORD size = sizeof(DWORD);
        RegQueryValueExW(hKey, L"ProxyEnable", NULL, NULL, (LPBYTE)&g_originalProxyEnable, &size);
        size = sizeof(g_originalProxyServer);
        RegQueryValueExW(hKey, L"ProxyServer", NULL, NULL, (LPBYTE)g_originalProxyServer, &size);
        g_hasBackup = 1;
        RegCloseKey(hKey);
    }
}

int proxy_set_system(const wchar_t* ip, int port) {
    proxy_init_backup();
    HKEY hKey = open_internet_settings(KEY_WRITE);
    if (!hKey) return 0;

    DWORD enable = 1;
    wchar_t server[128];
    swprintf_s(server, 128, L"%s:%d", ip, port);

    RegSetValueExW(hKey, L"ProxyEnable", 0, REG_DWORD, (BYTE*)&enable, sizeof(enable));
    RegSetValueExW(hKey, L"ProxyServer", 0, REG_SZ, (BYTE*)server, (wcslen(server) + 1) * sizeof(wchar_t));
    
    RegCloseKey(hKey);
    refresh_internet_settings();
    return 1;
}

int proxy_unset_system() {
    if (!g_hasBackup) return 1;
    HKEY hKey = open_internet_settings(KEY_WRITE);
    if (!hKey) return 0;

    RegSetValueExW(hKey, L"ProxyEnable", 0, REG_DWORD, (BYTE*)&g_originalProxyEnable, sizeof(DWORD));
    RegSetValueExW(hKey, L"ProxyServer", 0, REG_SZ, (BYTE*)g_originalProxyServer, (wcslen(g_originalProxyServer) + 1) * sizeof(wchar_t));

    RegCloseKey(hKey);
    refresh_internet_settings();
    g_hasBackup = 0;
    return 1;
}
