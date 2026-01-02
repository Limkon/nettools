#include "network_tools.h"
#include <wininet.h>
#include <iphlpapi.h>
#include <icmpapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <process.h>
#include <string.h> // for memcpy, memset

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
    if(str) WideCharToMultiByte(CP_ACP, 0, wstr, -1, str, len, NULL, NULL);
    return str;
}

// GBK 转 Unicode (用于解析纯真IP库中文)
void gbk_to_wide(const char* gbk, wchar_t* buf, int bufLen) {
    if (!gbk || !buf) return;
    MultiByteToWideChar(CP_ACP, 0, gbk, -1, buf, bufLen);
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

// --- QQWry 纯真IP库解析逻辑 ---

static unsigned char* load_qqwry(size_t* outSize) {
    FILE* f;
    if (fopen_s(&f, "qqwry.dat", "rb") != 0 || !f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char* data = (unsigned char*)malloc(sz);
    if (data) fread(data, 1, sz, f);
    fclose(f);
    if (outSize) *outSize = sz;
    return data;
}

static unsigned int read_int3(unsigned char* p) {
    return p[0] | (p[1] << 8) | (p[2] << 16);
}

static unsigned int read_int4(unsigned char* p) {
    return p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24);
}

static void read_qqwry_string(unsigned char* data, size_t size, unsigned int offset, char* buf, int bufSize) {
    if (offset >= size) { buf[0] = 0; return; }
    unsigned char* p = data + offset;
    int i = 0;
    while (offset + i < size && p[i] != 0 && i < bufSize - 1) {
        buf[i] = p[i];
        i++;
    }
    buf[i] = 0;
}

static void get_qqwry_location(unsigned char* data, size_t size, const char* ansiIp, wchar_t* outBuf, int outLen) {
    if (!data || !ansiIp) { wcscpy_s(outBuf, outLen, L""); return; }

    unsigned long ip = inet_addr(ansiIp);
    if (ip == INADDR_NONE) { wcscpy_s(outBuf, outLen, L"无效IP"); return; }
    ip = ntohl(ip); // QQWry uses Host Byte Order (Big Endian stored as LE int?? No, it stores IP as LE integer)
                    // inet_addr returns Network Byte Order (BE).
                    // x86 is LE. 
                    // Example: 1.0.0.0. 
                    // Network: 01 00 00 00. 
                    // QQWry stores 1.0.0.0 as 0x01000000 (if treated as LE integer).
                    // Actually, let's just reverse bytes.
    ip = ((ip & 0xFF) << 24) | ((ip & 0xFF00) << 8) | ((ip & 0xFF0000) >> 8) | ((ip & 0xFF000000) >> 24);

    unsigned int firstIndex = read_int4(data);
    unsigned int lastIndex = read_int4(data + 4);
    
    unsigned int l = 0, r = (lastIndex - firstIndex) / 7;
    unsigned int indexOffset = 0;

    while (l <= r) {
        unsigned int m = (l + r) / 2;
        unsigned int offset = firstIndex + m * 7;
        if (offset + 7 > size) break;
        
        unsigned int startIp = read_int4(data + offset);
        if (ip < startIp) {
            r = m - 1;
        } else {
            unsigned int recordOffset = read_int3(data + offset + 4);
            if (recordOffset + 4 > size) break;
            unsigned int endIp = read_int4(data + recordOffset);
            if (ip > endIp) {
                l = m + 1;
            } else {
                indexOffset = recordOffset;
                break;
            }
        }
    }

    if (indexOffset == 0) { wcscpy_s(outBuf, outLen, L"未知"); return; }

    unsigned int pos = indexOffset + 4; // Skip EndIP
    unsigned char mode = data[pos];
    char country[256] = {0};
    char area[256] = {0};
    unsigned int countryOffset = 0;

    if (mode == 1) {
        countryOffset = read_int3(data + pos + 1);
        pos = countryOffset;
        mode = data[pos];
        if (mode == 2) {
            countryOffset = read_int3(data + pos + 1);
            read_qqwry_string(data, size, countryOffset, country, sizeof(country));
            pos += 4; // Skip redirect
            // Read area
        } else {
            read_qqwry_string(data, size, pos, country, sizeof(country));
            pos += (strlen(country) + 1);
        }
    } else if (mode == 2) {
        countryOffset = read_int3(data + pos + 1);
        read_qqwry_string(data, size, countryOffset, country, sizeof(country));
        pos += 4;
    } else {
        read_qqwry_string(data, size, pos, country, sizeof(country));
        pos += (strlen(country) + 1);
    }

    // Read Area (Area usually follows country unless country was redirected)
    // Area processing is complex in QQWry due to compression, simplified here:
    // If mode was 1, we jumped. If mode was 2, we jumped for country but area is at original pos+4.
    
    // Simplest robust way for area (often less critical):
    // Just returning Country is often enough, but let's try appending.
    
    // Combining GBK strings
    wchar_t wCountry[256];
    gbk_to_wide(country, wCountry, 256);
    
    // Clean up "CZ88.NET" which is default filler
    if (wcsstr(wCountry, L"CZ88.NET")) wcscpy_s(wCountry, 256, L"");

    wcscpy_s(outBuf, outLen, wCountry);
    if (wcslen(outBuf) == 0) wcscpy_s(outBuf, outLen, L"未知");
}


// --- UI 消息辅助 ---

// [修改] 增加 col6 处理，默认值为 "-" 以防止列表错位
void post_result(HWND hwnd, const wchar_t* col1, const wchar_t* col2, const wchar_t* col3, const wchar_t* col4, const wchar_t* col5, const wchar_t* col6) {
    wchar_t buffer[1024];
    swprintf_s(buffer, 1024, L"%s|%s|%s|%s|%s|%s", 
             col1 ? col1 : L"", 
             col2 ? col2 : L"", 
             col3 ? col3 : L"", 
             col4 ? col4 : L"", 
             col5 ? col5 : L"",
             col6 ? col6 : L"-"); // 若无 col6，填充占位符
    
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

// --- 任务线程逻辑 ---

unsigned int __stdcall thread_ping(void* arg) {
    ThreadParams* p = (ThreadParams*)arg;
    int count;
    wchar_t** hosts = split_hosts(p->targetInput, &count);
    HWND hwnd = p->hwndNotify; 

    // [New] 加载 IP 库
    unsigned char* qqwryData = NULL;
    size_t qqwrySize = 0;
    if (p->showLocation) {
        qqwryData = load_qqwry(&qqwrySize);
        if (!qqwryData) post_log(hwnd, 0, L"警告: 未找到 qqwry.dat，无法显示归属地");
    }
    
    HANDLE hIcmp = IcmpCreateFile();
    if (hIcmp == INVALID_HANDLE_VALUE) {
        post_finish(hwnd, L"错误: 无法创建 ICMP 句柄");
        free_string_list(hosts, count);
        if (qqwryData) free(qqwryData);
        free_thread_params(p);
        return 0;
    }

    char sendData[] = "NetToolPing";
    DWORD replySize = sizeof(ICMP_ECHO_REPLY) + sizeof(sendData);
    void* replyBuffer = malloc(replySize);

    for (int i = 0; i < count; i++) {
        if (g_stopSignal) { post_log(hwnd, 0, L"任务已中止"); break; }

        wchar_t statusMsg[256];
        swprintf_s(statusMsg, 256, L"正在 Ping (%d/%d): %s...", i + 1, count, hosts[i]);
        post_log(hwnd, (i * 100) / count, statusMsg);

        char* ansiHost = wide_to_ansi(hosts[i]);
        
        // [New] 计算归属地
        wchar_t location[256] = {0};
        if (p->showLocation && qqwryData) {
            get_qqwry_location(qqwryData, qqwrySize, ansiHost, location, 256);
        }

        unsigned long ip = inet_addr(ansiHost);
        int validIp = 1;
        if (ip == INADDR_NONE) {
            struct hostent* he = gethostbyname(ansiHost);
            if (he) {
                ip = *(unsigned long*)he->h_addr_list[0];
                // 如果是域名解析出的IP，再次查询归属地
                if (p->showLocation && qqwryData) {
                    struct in_addr in;
                    in.s_addr = ip;
                    get_qqwry_location(qqwryData, qqwrySize, inet_ntoa(in), location, 256);
                }
            } else {
                post_result(hwnd, hosts[i], L"无效地址", L"N/A", L"100", L"N/A", location);
                free(ansiHost);
                validIp = 0;
            }
        }
        
        if (!validIp) continue;
        free(ansiHost);

        int success = 0;
        long totalRtt = 0;
        int lastTtl = 0;
        
        for (int j = 0; j < p->retryCount; j++) {
            if (g_stopSignal) break; 
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

        if (g_stopSignal) break; 

        wchar_t rttStr[32], lossStr[32], ttlStr[32];
        if (success > 0) {
            swprintf_s(rttStr, 32, L"%.2f", (double)totalRtt / success);
            swprintf_s(lossStr, 32, L"%.0f", (double)(p->retryCount - success) / p->retryCount * 100.0);
            swprintf_s(ttlStr, 32, L"%d", lastTtl);
            post_result(hwnd, hosts[i], L"在线", rttStr, lossStr, ttlStr, location);
        } else {
            post_result(hwnd, hosts[i], L"超时", L"N/A", L"100", L"N/A", location);
        }
    }

    free(replyBuffer);
    IcmpCloseHandle(hIcmp);
    free_string_list(hosts, count);
    if (qqwryData) free(qqwryData);
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

    // [New] 加载 IP 库
    unsigned char* qqwryData = NULL;
    size_t qqwrySize = 0;
    if (p->showLocation) {
        qqwryData = load_qqwry(&qqwrySize);
        if (!qqwryData) post_log(hwnd, 0, L"警告: 未找到 qqwry.dat");
    }

    int total = hostCount * portCount;
    int current = 0;

    for (int i = 0; i < hostCount; i++) {
        if (g_stopSignal) break; 

        char* ansiHost = wide_to_ansi(hosts[i]);
        
        // [New] 归属地
        wchar_t location[256] = {0};
        if (p->showLocation && qqwryData) {
            get_qqwry_location(qqwryData, qqwrySize, ansiHost, location, 256);
        }
        
        for (int j = 0; j < portCount; j++) {
            if (g_stopSignal) break; 

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
                 if (he) {
                     memcpy(&addr.sin_addr, he->h_addr, he->h_length);
                     // Update location for domain
                     if (p->showLocation && qqwryData) {
                         get_qqwry_location(qqwryData, qqwrySize, inet_ntoa(addr.sin_addr), location, 256);
                     }
                 }
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
                // [New] Pass location
                post_result(hwnd, hosts[i], portStr, L"开放 (Open)", location, L"", L"");
            } 

            closesocket(sock);
        }
        free(ansiHost);
    }

    free_string_list(hosts, hostCount);
    free(ports);
    if (qqwryData) free(qqwryData);
    free_thread_params(p);
    
    if (g_stopSignal) post_finish(hwnd, L"任务已由用户中止。");
    else post_finish(hwnd, L"批量端口扫描完成。");
    return 0;
}

unsigned int __stdcall thread_extract_ip(void* arg) {
    ThreadParams* p = (ThreadParams*)arg;
    HWND hwnd = p->hwndNotify;
    wchar_t* content = p->targetInput;
    size_t len = wcslen(content);
    
    // [New] 加载 IP 库 (Extract 任务也支持)
    unsigned char* qqwryData = NULL;
    size_t qqwrySize = 0;
    if (p->showLocation) {
        qqwryData = load_qqwry(&qqwrySize);
    }

    post_log(hwnd, 0, L"正在分析文本...");

    wchar_t currentIp[16] = {0};
    int idx = 0;
    int dots = 0;
    int lastCharWasDigit = 0;

    for (size_t i = 0; i <= len; i++) {
        if (g_stopSignal) break;

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
                        wchar_t location[256] = {0};
                        if (p->showLocation && qqwryData) {
                             char* ansi = wide_to_ansi(currentIp);
                             get_qqwry_location(qqwryData, qqwrySize, ansi, location, 256);
                             free(ansi);
                        }
                        post_result(hwnd, currentIp, location, L"", L"", L"", L"");
                    }
                }
            }
            idx = 0; dots = 0; lastCharWasDigit = 0;
        }
    }

    if (qqwryData) free(qqwryData);
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

// ... (Proxy 相关的代码保持不变) ...
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
