cmake_minimum_required(VERSION 3.16)

project(NetToolPro VERSION 3.7 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets Network Core)

# --- 修改点 1: 定义资源文件变量 ---
set(PROJECT_RESOURCES
    resources/resources.qrc
)

set(WIN_RC_FILE "")
if(WIN32)
    set(WIN_RC_FILE resources/NetToolPro.rc)
endif()
# -------------------------------

set(PROJECT_SOURCES
    src/main.cpp
    src/MainWindow.cpp
    src/WorkerThreads.cpp
    src/ProxyManager.cpp
    # --- 修改点 2: 加入资源文件 ---
    ${PROJECT_RESOURCES}
    ${WIN_RC_FILE}
)

set(PROJECT_HEADERS
    src/MainWindow.h
    src/WorkerThreads.h
    src/ProxyManager.h
)

if(WIN32)
    list(APPEND SYSTEM_LIBS Iphlpapi Ws2_32 Advapi32)
endif()

# --- 修改点 3: 确保 add_executable 包含了资源文件 ---
add_executable(NetToolPro WIN32
    ${PROJECT_SOURCES}
    ${PROJECT_HEADERS}
)

target_link_libraries(NetToolPro PRIVATE
    Qt6::Widgets
    Qt6::Network
    Qt6::Core
    ${SYSTEM_LIBS}
)

set_property(TARGET NetToolPro PROPERTY CXX_STANDARD 17)
set_property(TARGET NetToolPro PROPERTY AUTOMOC ON)
set_property(TARGET NetToolPro PROPERTY AUTOUIC ON)
set_property(TARGET NetToolPro PROPERTY AUTORCC ON) # 必须开启 AUTORCC 以处理 .qrc
