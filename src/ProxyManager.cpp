#include "ProxyManager.h"
#include <winreg.h>
#include <wininet.h>
#include <QSettings> // 虽然用 WinAPI，但 Qt 字符串转换更方便
#include <QDebug>

// 初始化静态成员
DWORD ProxyManager::originalProxyEnable = 0;
QString ProxyManager::originalProxyServer = "";
bool ProxyManager::hasBackup = false;

// 辅助函数：打开注册表项
HKEY openInternetSettings(REGSAM access) {
    HKEY hKey;
    const char* subKey = "Software\\Microsoft\\Windows\\CurrentVersion\\Internet Settings";
    if (RegOpenKeyExA(HKEY_CURRENT_USER, subKey, 0, access, &hKey) == ERROR_SUCCESS) {
        return hKey;
    }
    return nullptr;
}

bool ProxyManager::setSystemProxy(const QString& ip, int port) {
    HKEY hKey = openInternetSettings(KEY_READ | KEY_WRITE);
    if (!hKey) return false;

    // 1. 备份当前设置 (仅在第一次设置时备份)
    if (!hasBackup) {
        DWORD dataSize = sizeof(DWORD);
        RegQueryValueExA(hKey, "ProxyEnable", nullptr, nullptr, (LPBYTE)&originalProxyEnable, &dataSize);
        
        char buffer[256] = {0};
        dataSize = sizeof(buffer);
        if (RegQueryValueExA(hKey, "ProxyServer", nullptr, nullptr, (LPBYTE)buffer, &dataSize) == ERROR_SUCCESS) {
            originalProxyServer = QString::fromLocal8Bit(buffer);
        }
        hasBackup = true;
    }

    // 2. 设置新值
    DWORD enable = 1;
    QString proxyAddr = QString("%1:%2").arg(ip).arg(port);
    std::string proxyAddrStr = proxyAddr.toStdString();

    long res1 = RegSetValueExA(hKey, "ProxyEnable", 0, REG_DWORD, (const BYTE*)&enable, sizeof(enable));
    long res2 = RegSetValueExA(hKey, "ProxyServer", 0, REG_SZ, (const BYTE*)proxyAddrStr.c_str(), proxyAddrStr.length() + 1);

    RegCloseKey(hKey);

    if (res1 == ERROR_SUCCESS && res2 == ERROR_SUCCESS) {
        refreshInternetSettings();
        return true;
    }
    return false;
}

bool ProxyManager::unsetSystemProxy() {
    if (!hasBackup) return true; // 没有设置过，不需要恢复

    HKEY hKey = openInternetSettings(KEY_WRITE);
    if (!hKey) return false;

    std::string serverStr = originalProxyServer.toStdString();

    RegSetValueExA(hKey, "ProxyEnable", 0, REG_DWORD, (const BYTE*)&originalProxyEnable, sizeof(originalProxyEnable));
    if (!originalProxyServer.isEmpty()) {
        RegSetValueExA(hKey, "ProxyServer", 0, REG_SZ, (const BYTE*)serverStr.c_str(), serverStr.length() + 1);
    } else {
        // 如果原来没有代理服务器字符串，可以置空或删除该值，这里简单置空
        RegSetValueExA(hKey, "ProxyServer", 0, REG_SZ, (const BYTE*)"", 1);
    }

    RegCloseKey(hKey);
    refreshInternetSettings();
    
    hasBackup = false;
    return true;
}

void ProxyManager::refreshInternetSettings() {
    // 通知系统设置已更改
    InternetSetOption(nullptr, INTERNET_OPTION_SETTINGS_CHANGED, nullptr, 0);
    InternetSetOption(nullptr, INTERNET_OPTION_REFRESH, nullptr, 0);
}
