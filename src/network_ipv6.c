#include "network_modules.h"
#include "network_tools.h"
#include <iphlpapi.h>
#include <icmpapi.h>
#include <stdio.h>
#include <stdlib.h>

#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")

// --- IPv6 Ping ---
int ipv6_ping_host(struct sockaddr_in6* dest, int retry, int timeout, long* outRtt, int* outTtl) {
    HANDLE hIcmp = Icmp6CreateFile();
    if (hIcmp == INVALID_HANDLE_VALUE) return 0;

    // 构造请求数据
    char sendData[] = "NetToolPingV6";
    // 缓冲区大小必须足够大以容纳 ICMPV6_ECHO_REPLY 结构 + 数据 + 填充
    DWORD replySize = sizeof(ICMPV6_ECHO_REPLY) + sizeof(sendData) + 128; 
    void* replyBuffer = malloc(replySize);

    struct sockaddr_in6 source = {0};
    source.sin6_family = AF_INET6; // 让系统自动选择源地址

    int successCount = 0;
    long totalRtt = 0;
    int lastTtl = 0;

    for (int i = 0; i < retry; i++) {
        if (is_task_stopped()) break;

        // Icmp6SendEcho2 同步调用 (Event=NULL, ApcRoutine=NULL)
        DWORD ret = Icmp6SendEcho2(hIcmp, NULL, NULL, NULL, 
                                   &source, dest, 
                                   sendData, sizeof(sendData), NULL, 
                                   replyBuffer, replySize, timeout);
        
        if (ret != 0) {
            PICMPV6_ECHO_REPLY reply = (PICMPV6_ECHO_REPLY)replyBuffer;
            if (reply->Status == IP_SUCCESS) {
                successCount++;
                // IPv6 Reply 结构体中 RTT 字段名为 RoundTripTime
                totalRtt += reply->RoundTripTime; 
                // IPv6 这里通常不直接返回 TTL，但在 Options 中可能并没有 Ttl 字段
                // ICMPV6_ECHO_REPLY 结构体定义通常没有 Options.Ttl 
                // 这里我们暂且设为 0 或尝试从其他地方获取，标准 API 简单调用通常不返回对方的 HopLimit
                lastTtl = 0; 
            }
        }
        if (i < retry - 1) Sleep(100);
    }

    free(replyBuffer);
    IcmpCloseHandle(hIcmp);

    if (successCount > 0) {
        *outRtt = totalRtt / successCount;
        *outTtl = lastTtl;
        return 1;
    }
    return 0;
}

// --- IPv6 TCP Scan ---
int ipv6_tcp_scan(struct sockaddr_in6* dest, int port, int timeout) {
    SOCKET sock = socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) return 0;

    unsigned long mode = 1;
    ioctlsocket(sock, FIONBIO, &mode);

    // 复制目标地址并设置端口
    struct sockaddr_in6 addr = *dest;
    addr.sin6_port = htons(port);

    connect(sock, (struct sockaddr*)&addr, sizeof(addr));

    fd_set writeFds;
    FD_ZERO(&writeFds);
    FD_SET(sock, &writeFds);

    struct timeval tv;
    tv.tv_sec = timeout / 1000;
    tv.tv_usec = (timeout % 1000) * 1000;

    int ret = select(0, NULL, &writeFds, NULL, &tv);
    closesocket(sock);
    return (ret > 0);
}

// --- IPv6 提取逻辑 ---
// 简单扫描符合 Hex:Hex:Hex... 格式的字符串，并尝试验证
void ipv6_extract_search(const wchar_t* text, HWND hwnd) {
    size_t len = wcslen(text);
    wchar_t buf[64];
    int bufIdx = 0;
    
    // IPv6 可能包含 0-9, a-f, A-F, :
    for (size_t i = 0; i <= len; i++) {
        if (is_task_stopped()) break;
        wchar_t c = text[i];
        if (iswxdigit(c) || c == L':') {
            if (bufIdx < 63) buf[bufIdx++] = c;
        } else {
            if (bufIdx > 2) { // 至少有一些长度
                buf[bufIdx] = 0;
                // 验证是否包含至少两个冒号，且是有效地址
                if (wcschr(buf, L':') && wcschr(wcschr(buf, L':')+1, L':')) {
                    struct sockaddr_in6 sa;
                    int saLen = sizeof(sa);
                    // 使用 WSAStringToAddressW 验证格式
                    if (WSAStringToAddressW(buf, AF_INET6, NULL, (struct sockaddr*)&sa, &saLen) == 0) {
                         post_result(hwnd, buf, L"N/A", L"", L"", L"", L"");
                    }
                }
            }
            bufIdx = 0;
        }
    }
}
