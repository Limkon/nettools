#include <winsock2.h>
#include <iphlpapi.h>
#include <icmpapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <process.h>

#pragma comment(lib, \"iphlpapi.lib\")
#pragma comment(lib, \"ws2_32.lib\")

// 自定义消息，用于通知主窗口
#define WM_USER_LOG     (WM_USER + 100) // wParam=Progress, lParam=StringPtr
#define WM_USER_RESULT  (WM_USER + 101) // wParam=0, lParam=StringPtr (CSV row)
#define WM_USER_FINISH  (WM_USER + 102) // wParam=0, lParam=MessageString

// 全局代理备份变量
static DWORD g_originalProxyEnable = 0;
static char g_originalProxyServer[256] = {0};
static int g_hasBackup = 0;

// --- 辅助工具 ---

// 简单的字符串分割并去重，返回字符串数组，count为传出数量
char** split_hosts(const char* input, int* count) {
    // 简单实现：将换行、逗号、空格视为分隔符
    // 实际项目中建议使用更健壮的解析
    if (!input) { *count = 0; return NULL; }
    
    char* copy = _strdup(input);
    int capacity = 10;
    char** list = (char**)malloc(sizeof(char*) * capacity);
    int n = 0;

    char* context = NULL;
    char* token = strtok_s(copy, \" \\t\\n\\r,\", &context);
    while (token) {
        if (n >= capacity) {
            capacity *= 2;
            list = (char**)realloc(list, sizeof(char*) * capacity);
        }
        // 简单去重逻辑略，此处直接添加
        list[n++] = _strdup(token);
        token = strtok_s(NULL, \" \\t\\n\\r,\", &context);
    }
    
    free(copy);
    *count = n;
    return list;
}

void free_string_list(char** list, int count) {
    for (int i = 0; i < count; i++) free(list[i]);
    free(list);
}

// 解析端口字符串 \"80, 443, 8080-8085\"
int* parse_ports(const char* portStr, int* count) {
    if (!portStr) { *count = 0; return NULL; }
    
    int* ports = (int*)malloc(sizeof(int) * 65535); // 最大可能
    int n = 0;
    
    char* copy = _strdup(portStr);
    char* context = NULL;
    char* token = strtok_s(copy, \",\", &context);
    
    while (token) {
        if (strchr(token, '-')) {
            int start, end;
            if (sscanf_s(token, \"%d-%d\", &start, &end) == 2) {
                for (int i = start; i <= end; i++) ports[n++] = i;
            }
        } else {
            int p = atoi(token);
            if (p > 0) ports[n++] = p;
        }
        token = strtok_s(NULL, \",\", &context);
    }
    free(copy);
    *count = n;
    return ports;
}

// 辅助：发送结果到 UI
void post_result(HWND hwnd, const char* col1, const char* col2, const char* col3, const char* col4, const char* col5) {
    // 格式化为 CSV 风格字符串传递给 UI 拆解，或者使用特定结构体
    // 这里为了简单，拼成 \"col1|col2|col3...\"
    char buffer[1024];
    snprintf(buffer, sizeof(buffer), \"%s|%s|%s|%s|%s\", 
             col1 ? col1 : \"\", col2 ? col2 : \"\", col3 ? col3 : \"\", col4 ? col4 : \"\", col5 ? col5 : \"\");
    
    char* msg = _strdup(buffer);
    PostMessage(hwnd, WM_USER_RESULT, 0, (LPARAM)msg);
}

void post_log(HWND hwnd, int progress, const char* text) {
    char* msg = _strdup(text);
    PostMessage(hwnd, WM_USER_LOG, (WPARAM)progress, (LPARAM)msg);
}

void post_finish(HWND hwnd, const char* msg) {
    PostMessage(hwnd, WM_USER_FINISH, 0, (LPARAM)_strdup(msg));
}

// --- Ping 任务 ---
unsigned int __stdcall thread_ping(void* arg) {
    ThreadParams* p = (ThreadParams*)arg;
    int count;
    char** hosts = split_hosts(p->targetInput, &count);
    
    HANDLE hIcmp = IcmpCreateFile();
    if (hIcmp == INVALID_HANDLE_VALUE) {
        post_finish(p->hwndNotify, \"错误: 无法创建 ICMP 句柄\");
        free_string_list(hosts, count);
        free_thread_params(p);
        return 0;
    }

    char sendData[] = \"NetToolPing\";
    DWORD replySize = sizeof(ICMP_ECHO_REPLY) + sizeof(sendData);
    void* replyBuffer = malloc(replySize);

    for (int i = 0; i < count; i++) {
        char statusMsg[256];
        snprintf(statusMsg, sizeof(statusMsg), \"正在 Ping (%d/%d): %s...\", i + 1, count, hosts[i]);
        post_log(p->hwndNotify, (i * 100) / count, statusMsg);

        unsigned long ip = inet_addr(hosts[i]);
        if (ip == INADDR_NONE) {
            struct hostent* he = gethostbyname(hosts[i]);
            if (he) {
                ip = *(unsigned long*)he->h_addr_list[0];
            } else {
                post_result(p->hwndNotify, hosts[i], \"无效地址\", \"N/A\", \"100\", \"N/A\");
                continue;
            }
        }

        int success = 0;
        long totalRtt = 0;
        int lastTtl = 0;
        
        for (int j = 0; j < p->retryCount; j++) {
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

        char rttStr[32], lossStr[32], ttlStr[32];
        if (success > 0) {
            snprintf(rttStr, 32, \"%.2f\", (double)totalRtt / success);
            snprintf(lossStr, 32, \"%.0f\", (double)(p->retryCount - success) / p->retryCount * 100.0);
            snprintf(ttlStr, 32, \"%d\", lastTtl);
            post_result(p->hwndNotify, hosts[i], \"在线\", rttStr, lossStr, ttlStr);
        } else {
            post_result(p->hwndNotify, hosts[i], \"超时\", \"N/A\", \"100\", \"N/A\");
        }
    }

    free(replyBuffer);
    IcmpCloseHandle(hIcmp);
    free_string_list(hosts, count);
    free_thread_params(p);
    
    post_finish(p->hwndNotify, \"批量 Ping 任务完成。\");
    return 0;
}

// --- 端口扫描任务 ---
unsigned int __stdcall thread_port_scan(void* arg) {
    ThreadParams* p = (ThreadParams*)arg;
    int hostCount, portCount;
    char** hosts = split_hosts(p->targetInput, &hostCount);
    int* ports = parse_ports(p->portsInput, &portCount);

    int total = hostCount * portCount;
    int current = 0;

    for (int i = 0; i < hostCount; i++) {
        for (int j = 0; j < portCount; j++) {
            current++;
            int port = ports[j];
            char msg[256];
            snprintf(msg, sizeof(msg), \"扫描 (%d/%d): %s:%d\", current, total, hosts[i], port);
            post_log(p->hwndNotify, (current * 100) / (total ? total : 1), msg);

            SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (sock == INVALID_SOCKET) continue;

            // 设置非阻塞模式
            unsigned long mode = 1;
            ioctlsocket(sock, FIONBIO, &mode);

            struct sockaddr_in addr;
            addr.sin_family = AF_INET;
            addr.sin_port = htons(port);
            addr.sin_addr.s_addr = inet_addr(hosts[i]);
            
            // 如果是域名则解析
            if (addr.sin_addr.s_addr == INADDR_NONE) {
                 struct hostent* he = gethostbyname(hosts[i]);
                 if (he) memcpy(&addr.sin_addr, he->h_addr, he->h_length);
                 else { closesocket(sock); continue; }
            }

            connect(sock, (struct sockaddr*)&addr, sizeof(addr));

            // 使用 select 等待连接结果
            fd_set writeFds;
            FD_ZERO(&writeFds);
            FD_SET(sock, &writeFds);
            
            struct timeval tv;
            tv.tv_sec = 2; // 2秒超时
            tv.tv_usec = 0;

            int ret = select(0, NULL, &writeFds, NULL, &tv);
            char portStr[16];
            snprintf(portStr, sizeof(portStr), \"%d\", port);

            if (ret > 0) {
                post_result(p->hwndNotify, hosts[i], portStr, \"开放 (Open)\", \"\", \"\");
            } 
            // 仅显示开放的端口，或者在此处扩展显示关闭的

            closesocket(sock);
        }
    }

    free_string_list(hosts, hostCount);
    free(ports);
    free_thread_params(p);
    post_finish(p->hwndNotify, \"批量端口扫描完成。\");
    return 0;
}

// --- 提取 IP 任务 ---
// 简单的纯C实现：查找数字.数字.数字.数字
unsigned int __stdcall thread_extract_ip(void* arg) {
    ThreadParams* p = (ThreadParams*)arg;
    char* content = p->targetInput;
    int len = strlen(content);
    
    post_log(p->hwndNotify, 0, \"正在分析文本...\");

    char currentIp[16] = {0};
    int idx = 0;
    int dots = 0;
    int lastCharWasDigit = 0;

    // 简单状态机
    for (int i = 0; i <= len; i++) {
        char c = content[i];
        if (isdigit((unsigned char)c)) {
            if (idx < 15) currentIp[idx++] = c;
            lastCharWasDigit = 1;
        } else if (c == '.') {
            if (lastCharWasDigit && dots < 3) {
                if (idx < 15) currentIp[idx++] = c;
                dots++;
                lastCharWasDigit = 0;
            } else {
                // Invalid sequence, reset
                idx = 0; dots = 0;
            }
        } else {
            // End of potential IP
            if (dots == 3 && lastCharWasDigit && idx >= 7) {
                currentIp[idx] = '\\0';
                // Validate ranges
                int parts[4];
                if (sscanf_s(currentIp, \"%d.%d.%d.%d\", &parts[0], &parts[1], &parts[2], &parts[3]) == 4) {
                    if (parts[0]<=255 && parts[1]<=255 && parts[2]<=255 && parts[3]<=255) {
                        post_result(p->hwndNotify, currentIp, \"\", \"\", \"\", \"\");
                    }
                }
            }
            idx = 0; dots = 0; lastCharWasDigit = 0;
        }
    }

    free_thread_params(p);
    post_finish(p->hwndNotify, \"IP 提取完成。\");
    return 0;
}

// --- 单个扫描 (简化复用端口扫描逻辑) ---
unsigned int __stdcall thread_single_scan(void* arg) {
    return thread_port_scan(arg); // 逻辑基本一致，只是输入只有一个IP
}

void free_thread_params(ThreadParams* params) {
    if (params) {
        if (params->targetInput) free(params->targetInput);
        if (params->portsInput) free(params->portsInput);
        free(params);
    }
}

// --- 代理管理 ---
HKEY open_internet_settings(REGSAM access) {
    HKEY hKey;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, \"Software\\\\Microsoft\\\\Windows\\\\CurrentVersion\\\\Internet Settings\", 0, access, &hKey) == ERROR_SUCCESS) {
        return hKey;
    }
    return NULL;
}

void refresh_internet_settings() {
    // 动态加载 Wininet 以避免链接问题
    HMODULE hWininet = LoadLibraryA(\"wininet.dll\");
    if (hWininet) {
        typedef BOOL (WINAPI *ISO)(HINTERNET, DWORD, LPVOID, DWORD);
        ISO pInternetSetOption = (ISO)GetProcAddress(hWininet, \"InternetSetOptionA\");
        if (pInternetSetOption) {
            pInternetSetOption(NULL, 39, NULL, 0); // INTERNET_OPTION_SETTINGS_CHANGED
            pInternetSetOption(NULL, 37, NULL, 0); // INTERNET_OPTION_REFRESH
        }
        FreeLibrary(hWininet);
    }
}

void proxy_init_backup() {
    if (g_hasBackup) return;
    HKEY hKey = open_internet_settings(KEY_READ);
    if (hKey) {
        DWORD size = sizeof(DWORD);
        RegQueryValueExA(hKey, \"ProxyEnable\", NULL, NULL, (LPBYTE)&g_originalProxyEnable, &size);
        size = sizeof(g_originalProxyServer);
        RegQueryValueExA(hKey, \"ProxyServer\", NULL, NULL, (LPBYTE)g_originalProxyServer, &size);
        g_hasBackup = 1;
        RegCloseKey(hKey);
    }
}

int proxy_set_system(const char* ip, int port) {
    proxy_init_backup();
    HKEY hKey = open_internet_settings(KEY_WRITE);
    if (!hKey) return 0;

    DWORD enable = 1;
    char server[128];
    snprintf(server, sizeof(server), \"%s:%d\", ip, port);

    RegSetValueExA(hKey, \"ProxyEnable\", 0, REG_DWORD, (BYTE*)&enable, sizeof(enable));
    RegSetValueExA(hKey, \"ProxyServer\", 0, REG_SZ, (BYTE*)server, strlen(server) + 1);
    
    RegCloseKey(hKey);
    refresh_internet_settings();
    return 1;
}

int proxy_unset_system() {
    if (!g_hasBackup) return 1;
    HKEY hKey = open_internet_settings(KEY_WRITE);
    if (!hKey) return 0;

    RegSetValueExA(hKey, \"ProxyEnable\", 0, REG_DWORD, (BYTE*)&g_originalProxyEnable, sizeof(DWORD));
    RegSetValueExA(hKey, \"ProxyServer\", 0, REG_SZ, (BYTE*)g_originalProxyServer, strlen(g_originalProxyServer) + 1);

    RegCloseKey(hKey);
    refresh_internet_settings();
    g_hasBackup = 0;
    return 1;
}

