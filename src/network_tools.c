#include "network_tools.h"
#include <wininet.h>
#include <iphlpapi.h>
#include <icmpapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <process.h>

#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "wininet.lib")

static DWORD g_originalProxyEnable = 0;
static wchar_t g_originalProxyServer[256] = {0};
static int g_hasBackup = 0;

// 全局停止信号
static volatile int g_stopSignal = 0;

void signal_stop_task() {
    g_stopSignal = 1;
}

void reset_stop_task() {
    g_stopSignal = 0;
}

// --- 字符串转换辅助 ---
char* wide_to_ansi(const wchar_t* wstr) {
    if (!wstr) return NULL;
    int len = WideCharToMultiByte(CP_ACP, 0, wstr, -1, NULL, 0, NULL, NULL);
    char* str = (char*)malloc(len);
    WideCharToMultiByte(CP_ACP, 0, wstr, -1, str, len, NULL, NULL);
    return str;
}

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

void post_result(HWND hwnd, const wchar_t* col1, const wchar_t* col2, const wchar_t* col3, const wchar_t* col4, const wchar_t* col5) {
    wchar_t buffer[1024];
    swprintf_s(buffer, 1024, L"%s|%s|%s|%s|%s", 
             col1 ? col1 : L"", col2 ? col2 : L"", col3 ? col3 : L"", col4 ? col4 : L"", col5 ? col5 : L"");
    
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

// --- 界面字体设置辅助 (新增) ---

// 回调函数：给单个子窗口设置字体
static BOOL CALLBACK EnumChildProcSetFont(HWND hWndChild, LPARAM lParam) {
    // 发送 WM_SETFONT 消息，TRUE 表示立即重绘
    SendMessage(hWndChild, WM_SETFONT, (WPARAM)lParam, TRUE);
    return TRUE;
}

// 主函数：将父窗口字体应用到所有子控件
void SetWindowFontToChildren(HWND hParent) {
    // 1. 获取父窗口当前的字体
    HFONT hFont = (HFONT)SendMessage(hParent, WM_GETFONT, 0, 0);

    // 2. 如果父窗口未设置字体（返回NULL），则使用系统默认的 GUI 字体
    if (hFont == NULL) {
        hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        // 顺便把父窗口也设置一下，保证统一
        SendMessage(hParent, WM_SETFONT, (WPARAM)hFont, TRUE);
    }

    // 3. 遍历所有子控件并应用字体
    EnumChildWindows(hParent, EnumChildProcSetFont, (LPARAM)hFont);
}

// --- 任务线程逻辑 ---

unsigned int __stdcall thread_ping(void* arg) {
    ThreadParams* p = (ThreadParams*)arg;
    int count;
    wchar_t** hosts = split_hosts(p->targetInput, &count);
    
    // [Fix] 保存窗口句柄，防止后续 free(p) 后无法访问
    HWND hwnd = p->hwndNotify; 
    
    HANDLE hIcmp = IcmpCreateFile();
    if (hIcmp == INVALID_HANDLE_VALUE) {
        post_finish(hwnd, L"错误: 无法创建 ICMP 句柄");
        free_string_list(hosts, count);
        free_thread_params(p);
        return 0;
    }

    char sendData[] = "NetToolPing";
    DWORD replySize = sizeof(ICMP_ECHO_REPLY) + sizeof(sendData);
    void* replyBuffer = malloc(replySize);

    for (int i = 0; i < count; i++) {
        // [Stop Check]
        if (g_stopSignal) { post_log(hwnd, 0, L"任务已中止"); break; }

        wchar_t statusMsg[256];
        swprintf_s(statusMsg, 256, L"正在 Ping (%d/%d): %s...", i + 1, count, hosts[i]);
        post_log(hwnd, (i * 100) / count, statusMsg);

        char* ansiHost = wide_to_ansi(hosts[i]);
        unsigned long ip = inet_addr(ansiHost);
        if (ip == INADDR_NONE) {
            struct hostent* he = gethostbyname(ansiHost);
            if (he) {
                ip = *(unsigned long*)he->h_addr_list[0];
            } else {
                post_result(hwnd, hosts[i], L"无效地址", L"N/A", L"100", L"N/A");
                free(ansiHost);
                continue;
            }
        }
        free(ansiHost);

        int success = 0;
        long totalRtt = 0;
        int lastTtl = 0;
        
        for (int j = 0; j < p->retryCount; j++) {
            if (g_stopSignal) break; // [Stop Check] 内部循环也检查

            DWORD ret = IcmpSendEcho(hIcmp, ip, sendData, sizeof(sendData), NULL, replyBuffer, replySize, p->timeoutMs);
            if (ret != 0) {
                PICMP_ECHO_REPLY reply = (PICMP_ECHO_REPLY)replyBuffer;
                if (reply->Status == IP_SUCCESS) {
                    success++;
                    totalRtt += reply->RoundTripTime;
                    lastTtl = reply->Options.Ttl;
                }
            }
            if (j < p->retryCount - 1) Sleep(100);
        }

        if (g_stopSignal) break; // [Stop Check]

        wchar_t rttStr[32], lossStr[32], ttlStr[32];
        if (success > 0) {
            swprintf_s(rttStr, 32, L"%.2f", (double)totalRtt / success);
            swprintf_s(lossStr, 32, L"%.0f", (double)(p->retryCount - success) / p->retryCount * 100.0);
            swprintf_s(ttlStr, 32, L"%d", lastTtl);
            post_result(hwnd, hosts[i], L"在线", rttStr, lossStr, ttlStr);
        } else {
            post_result(hwnd, hosts[i], L"超时", L"N/A", L"100", L"N/A");
        }
    }

    free(replyBuffer);
    IcmpCloseHandle(hIcmp);
    free_string_list(hosts, count);
    
    // [Fix] 先释放参数，但使用已保存的 hwnd 发送消息
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
    
    // [Fix] 保存窗口句柄
    HWND hwnd = p->hwndNotify;

    int total = hostCount * portCount;
    int current = 0;

    for (int i = 0; i < hostCount; i++) {
        if (g_stopSignal) break; // [Stop Check]

        char* ansiHost = wide_to_ansi(hosts[i]);
        
        for (int j = 0; j < portCount; j++) {
            if (g_stopSignal) break; // [Stop Check]

            current++;
            int port = ports[j];
            wchar_t msg[256];
            swprintf_s(msg, 256, L"扫描 (%d/%d): %s:%d", current, total, hosts[i], port);
            post_log(hwnd, (current * 100) / (total ? total : 1), msg);

            SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (sock == INVALID_SOCKET) continue;

            unsigned long mode = 1;
            ioctlsocket(sock, FIONBIO, &mode);

            struct sockaddr_in addr;
            addr.sin_family = AF_INET;
            addr.sin_port = htons(port);
            addr.sin_addr.s_addr = inet_addr(ansiHost);
            
            if (addr.sin_addr.s_addr == INADDR_NONE) {
                 struct hostent* he = gethostbyname(ansiHost);
                 if (he) memcpy(&addr.sin_addr, he->h_addr, he->h_length);
                 else { closesocket(sock); continue; }
            }

            connect(sock, (struct sockaddr*)&addr, sizeof(addr));

            fd_set writeFds;
            FD_ZERO(&writeFds);
            FD_SET(sock, &writeFds);
            
            struct timeval tv;
            tv.tv_sec = 2; 
            tv.tv_usec = 0;

            int ret = select(0, NULL, &writeFds, NULL, &tv);
            
            wchar_t portStr[16];
            swprintf_s(portStr, 16, L"%d", port);

            if (ret > 0) {
                post_result(hwnd, hosts[i], portStr, L"开放 (Open)", L"", L"");
            } 

            closesocket(sock);
        }
        free(ansiHost);
    }

    free_string_list(hosts, hostCount);
    free(ports);
    
    // [Fix] 使用保存的句柄
    free_thread_params(p);
    
    if (g_stopSignal) post_finish(hwnd, L"任务已由用户中止。");
    else post_finish(hwnd, L"批量端口扫描完成。");
    return 0;
}

unsigned int __stdcall thread_extract_ip(void* arg) {
    ThreadParams* p = (ThreadParams*)arg;
    
    // [Fix] 保存窗口句柄
    HWND hwnd = p->hwndNotify;
    wchar_t* content = p->targetInput;
    size_t len = wcslen(content);
    
    post_log(hwnd, 0, L"正在分析文本...");

    wchar_t currentIp[16] = {0};
    int idx = 0;
    int dots = 0;
    int lastCharWasDigit = 0;

    for (size_t i = 0; i <= len; i++) {
        if (g_stopSignal) break; // [Stop Check]

        wchar_t c = content[i];
        if (iswdigit(c)) {
            if (idx < 15) currentIp[idx++] = c;
            lastCharWasDigit = 1;
        } else if (c == L'.') {
            if (lastCharWasDigit && dots < 3) {
                if (idx < 15) currentIp[idx++] = c;
                dots++;
                lastCharWasDigit = 0;
            } else {
                idx = 0; dots = 0;
            }
        } else {
            if (dots == 3 && lastCharWasDigit && idx >= 7) {
                currentIp[idx] = L'\0';
                int parts[4];
                if (swscanf_s(currentIp, L"%d.%d.%d.%d", &parts[0], &parts[1], &parts[2], &parts[3]) == 4) {
                    if (parts[0]<=255 && parts[1]<=255 && parts[2]<=255 && parts[3]<=255) {
                        post_result(hwnd, currentIp, L"", L"", L"", L"");
                    }
                }
            }
            idx = 0; dots = 0; lastCharWasDigit = 0;
        }
    }

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

// --- 代理与注册表辅助 ---

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
