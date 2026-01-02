NetToolPro/   
├── CMakeLists.txt              # CMake 构建配置   
├── .github/   
│   └── workflows/   
│       └── build.yml           # GitHub Actions 自动构建脚本   
├── src/   
│   ├── main.cpp                # 程序入口   
│   ├── MainWindow.h            # 主窗口头文件   
│   ├── MainWindow.cpp          # 主窗口实现 (GUI逻辑)   
│   ├── WorkerThreads.h         # 后台任务线程头文件 (Ping, Scan, Extract)   
│   ├── WorkerThreads.cpp       # 后台任务实现   
│   ├── ProxyManager.h          # 系统代理管理头文件   
│   └── ProxyManager.cpp        # 系统代理管理实现   
