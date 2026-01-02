cmake_minimum_required(VERSION 3.16)

project(NetToolProC VERSION 3.7 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# === 核心修复：解决乱码的关键 ===
# 1. 定义 Unicode 宏，启用 Windows 宽字符 API
add_compile_definitions(UNICODE _UNICODE)

# 2. 强制 MSVC 使用 UTF-8 读取源码并生成执行字符集
if(MSVC)
    add_compile_options(/utf-8)
endif()

# 包含源文件
set(PROJECT_SOURCES
    src/main.c
    src/network_tools.c
)

set(PROJECT_HEADERS
    src/network_tools.h
)

# 资源文件
if(EXISTS "${CMAKE_SOURCE_DIR}/resources/NetToolPro.rc")
    list(APPEND PROJECT_SOURCES resources/NetToolPro.rc)
endif()

add_executable(NetToolPro ${PROJECT_SOURCES} ${PROJECT_HEADERS})

# 链接 Windows 系统库
if(WIN32)
    target_link_libraries(NetToolPro PRIVATE
        ws2_32
        iphlpapi
        comctl32
        gdi32
        user32
        kernel32
        advapi32
        wininet
    )
    set_target_properties(NetToolPro PROPERTIES WIN32_EXECUTABLE ON)
endif()
