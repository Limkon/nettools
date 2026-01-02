#pragma once
#include <QString>
#include <windows.h> // 需要包含 Windows 头文件

class ProxyManager {
public:
    // 设置系统代理
    static bool setSystemProxy(const QString& ip, int port);
    
    // 取消系统代理
    static bool unsetSystemProxy();

private:
    // 刷新系统网络设置，使代理立即生效
    static void refreshInternetSettings();
    
    // 备份用的变量（简单的静态存储，模拟 Python 逻辑）
    static DWORD originalProxyEnable;
    static QString originalProxyServer;
    static bool hasBackup;
};
