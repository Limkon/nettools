#include "network_modules.h"
#include "network_tools.h" // for post_result
#include <iphlpapi.h>
#include <icmpapi.h>
#include <stdio.h>
#include <stdlib.h>

#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")

// --- QQWry 纯真IP库逻辑 (仅 IPv4) ---
static unsigned char* g_qqwryData = NULL;
static size_t g_qqwrySize = 0;

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

void ipv4_init_qqwry() {
    if (g_qqwryData) return;
    FILE* f;
    if (fopen_s(&f, "qqwry.dat", "rb") != 0 || !f) return;
    fseek(f, 0, SEEK_END);
    g_qqwrySize = ftell(f);
    fseek(f, 0, SEEK_SET);
    g_qqwryData = (unsigned char*)malloc(g_qqwrySize);
    if (g_qqwryData) fread(g_qqwryData, 1, g_qqwrySize, f);
    fclose(f);
}

void ipv4_cleanup_qqwry() {
    if (g_qqwryData) { free(g_qqwryData); g_qqwryData = NULL; }
    g_qqwrySize = 0;
}

void ipv4_get_location(const char* ansiIp, wchar_t* outBuf, int outLen) {
    if (!g_qqwryData || !ansiIp) { wcscpy_s(outBuf, outLen, L""); return; }
    unsigned long ip = inet_addr(ansiIp);
    if (ip == INADDR_NONE) { wcscpy_s(outBuf, outLen, L""); return; }
    ip = ntohl(ip);
    
    // Reverse bytes for LE comparison in QQWry structure
    ip = ((ip & 0xFF) << 24) | ((ip & 0xFF00) << 8) | ((ip & 0xFF0000) >> 8) | ((ip & 0xFF000000) >> 24);

    unsigned int firstIndex = read_int4(g_qqwryData);
    unsigned int lastIndex = read_int4(g_qqwryData + 4);
    unsigned int l = 0, r = (lastIndex - firstIndex) / 7;
    unsigned int indexOffset = 0;

    while (l <= r) {
        unsigned int m = (l + r) / 2;
        unsigned int offset = firstIndex + m * 7;
        if (offset + 7 > g_qqwrySize) break;
        unsigned int startIp = read_int4(g_qqwryData + offset);
        if (ip < startIp) r = m - 1;
        else {
            unsigned int recordOffset = read_int3(g_qqwryData + offset + 4);
            if (recordOffset + 4 > g_qqwrySize) break;
            unsigned int endIp = read_int4(g_qqwryData + recordOffset);
            if (ip > endIp) l = m + 1;
            else { indexOffset = recordOffset; break; }
        }
    }

    if (indexOffset == 0) { wcscpy_s(outBuf, outLen, L"未知"); return; }

    unsigned int pos = indexOffset + 4;
    unsigned char mode = g_qqwryData[pos];
    char country[256] = {0};
    unsigned int countryOffset = 0;

    if (mode == 1) {
        countryOffset = read_int3(g_qqwryData + pos + 1);
        pos = countryOffset;
        mode = g_qqwryData[pos];
        if (mode == 2) {
            countryOffset = read_int3(g_qqwryData + pos + 1);
            read_qqwry_string(g_qqwryData, g_qqwrySize, countryOffset, country, sizeof(country));
        } else {
            read_qqwry_string(g_qqwryData, g_qqwrySize, pos, country, sizeof(country));
        }
    } else if (mode == 2) {
        countryOffset = read_int3(g_qqwryData + pos + 1);
        read_qqwry_string(g_qqwryData, g_qqwrySize, countryOffset, country, sizeof(country));
    } else {
        read_qqwry_string(g_qqwryData, g_qqwrySize, pos, country, sizeof(country));
    }

    gbk_to_wide(country, outBuf, outLen);
    if (wcsstr(outBuf, L"CZ88.NET")) wcscpy_s(outBuf, outLen, L"");
    if (wcslen(outBuf) == 0) wcscpy_s(outBuf, outLen, L"未知");
}

int ipv4_ping_host(unsigned long ip, int retry, int timeout, long* outRtt, int* outTtl) {
    HANDLE hIcmp = IcmpCreateFile();
    if (hIcmp == INVALID_HANDLE_VALUE) return 0;

    char sendData[] = "NetToolPing";
    DWORD replySize = sizeof(ICMP_ECHO_REPLY) + sizeof(sendData);
    void* replyBuffer = malloc(replySize);
    
    int successCount = 0;
    long totalRtt = 0;
    int lastTtl = 0;

    for (int i = 0; i < retry; i++) {
        if (is_task_stopped()) break;
        DWORD ret = IcmpSendEcho(hIcmp, ip, sendData, sizeof(sendData), NULL, replyBuffer, replySize, timeout);
        if (ret != 0) {
            PICMP_ECHO_REPLY reply = (PICMP_ECHO_REPLY)replyBuffer;
            if (reply->Status == IP_SUCCESS) {
                successCount++;
                totalRtt += reply->RoundTripTime;
                lastTtl = reply->Options.Ttl;
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

int ipv4_tcp_scan(unsigned long ip, int port, int timeout) {
    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) return 0;

    unsigned long mode = 1;
    ioctlsocket(sock, FIONBIO, &mode);

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = ip;

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

void ipv4_extract_search(const wchar_t* text, HWND hwnd, int showLocation) {
    wchar_t currentIp[16] = {0};
    int idx = 0;
    int dots = 0;
    int lastCharWasDigit = 0;
    size_t len = wcslen(text);

    for (size_t i = 0; i <= len; i++) {
        if (is_task_stopped()) break;
        wchar_t c = text[i];
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
                        if (showLocation) {
                             char* ansi = wide_to_ansi(currentIp);
                             ipv4_get_location(ansi, location, 256);
                             free(ansi);
                        }
                        post_result(hwnd, currentIp, location, L"", L"", L"", L"");
                    }
                }
            }
            idx = 0; dots = 0; lastCharWasDigit = 0;
        }
    }
}
