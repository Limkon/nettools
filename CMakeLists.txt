
fullContent: "cmake_minimum_required(VERSION 3.16)

project(NetToolProC VERSION 3.7 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# 包含源文件
set(PROJECT_SOURCES
    src/main.c
    src/network_tools.c
)

set(PROJECT_HEADERS
    src/network_tools.h
)

# 资源文件 (如果有 .rc 文件可以添加)
if(EXISTS \"${CMAKE_SOURCE_DIR}/resources/NetToolPro.rc\")
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
    # 设置为 Windows GUI 子系统 (去除控制台窗口)
    set_target_properties(NetToolPro PROPERTIES WIN32_EXECUTABLE ON)
endif()"
