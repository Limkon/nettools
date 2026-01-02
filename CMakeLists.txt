cmake_minimum_required(VERSION 3.16)

project(NetToolPro VERSION 3.7 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 查找 Qt6 组件
find_package(Qt6 REQUIRED COMPONENTS Widgets Network Core)

# 源文件列表
set(PROJECT_SOURCES
    src/main.cpp
    src/MainWindow.cpp
    src/WorkerThreads.cpp
    src/ProxyManager.cpp
)

set(PROJECT_HEADERS
    src/MainWindow.h
    src/WorkerThreads.h
    src/ProxyManager.h
)

# 如果是 Windows，链接必要的系统库 (用于ICMP Ping和注册表操作)
if(WIN32)
    list(APPEND SYSTEM_LIBS Iphlpapi Ws2_32 Advapi32)
endif()

# 定义可执行文件 (WIN32 选项用于隐藏控制台窗口)
add_executable(NetToolPro WIN32
    ${PROJECT_SOURCES}
    ${PROJECT_HEADERS}
)

# 链接库
target_link_libraries(NetToolPro PRIVATE
    Qt6::Widgets
    Qt6::Network
    Qt6::Core
    ${SYSTEM_LIBS}
)

# 处理 Qt 的 MOC (Meta-Object Compiler) 等自动生成
set_property(TARGET NetToolPro PROPERTY CXX_STANDARD 17)
set_property(TARGET NetToolPro PROPERTY AUTOMOC ON)
set_property(TARGET NetToolPro PROPERTY AUTOUIC ON)
set_property(TARGET NetToolPro PROPERTY AUTORCC ON)
