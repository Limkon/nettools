#include "network_modules.h"
#include "network_tools.h"
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>

// 简单的域名字符检查 (字母, 数字, -, .)
static int is_domain_char(wchar_t c) {
    return (iswalnum(c) || c == L'-' || c == L'.');
}

void domain_extract_search(const wchar_t* text, HWND hwnd) {
    size_t len = wcslen(text);
    wchar_t buf[256];
    int bufIdx = 0;
    int dotCount = 0;
    int hasAlpha = 0; // 确保包含字母，避免提取纯数字序列或IP

    for (size_t i = 0; i <= len; i++) {
        if (is_task_stopped()) break;

        wchar_t c = text[i];
        
        if (is_domain_char(c)) {
            if (bufIdx < 255) {
                // 如果是点，且前一个字符也是点，则是无效的（连续点）
                if (c == L'.' && bufIdx > 0 && buf[bufIdx-1] == L'.') {
                    // 重置，但保留当前点作为新开始（如果合适的话，不过通常连续点不是域名的开始）
                    bufIdx = 0; dotCount = 0; hasAlpha = 0;
                    continue;
                }
                
                buf[bufIdx++] = c;
                if (c == L'.') dotCount++;
                if (iswalpha(c)) hasAlpha = 1;
            }
        } else {
            // 遇到非域名字符，检查缓冲区内容是否为有效域名
            if (bufIdx > 0) {
                buf[bufIdx] = 0;
                
                // 验证逻辑：
                // 1. 至少有一个点
                // 2. 不能以点或连字符开头/结尾
                // 3. 必须包含字母 (排除 192.168.1.1 这样的纯IP，虽然它们是有效Host，但这里是"域名"提取)
                // 4. 长度限制 (通常域名至少3-4个字符，如 a.com)
                
                if (dotCount > 0 && hasAlpha && bufIdx >= 4) {
                    if (buf[0] != L'.' && buf[0] != L'-' && 
                        buf[bufIdx-1] != L'.' && buf[bufIdx-1] != L'-') {
                        
                        // 简单的去重可以在 UI 层做，或者这里不做
                        // 直接提交结果，第二列显示类型
                        post_result(hwnd, buf, L"域名/主机名", L"", L"", L"", L"");
                    }
                }
            }
            // 重置状态
            bufIdx = 0;
            dotCount = 0;
            hasAlpha = 0;
        }
    }
}
