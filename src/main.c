#include <windows.h>
#include <commctrl.h>
#include <stdio.h>
#include <shlobj.h>
#include "network_tools.h"

#pragma comment(lib, "comctl32.lib")

// --- 控件 ID 定义 ---
#define ID_BTN_BROWSE       101
#define ID_RADIO_FILE       102
#define ID_RADIO_TEXT       103
#define ID_EDIT_FILE        104
#define ID_EDIT_TEXT        105
#define ID_EDIT_TIMEOUT     106
#define ID_EDIT_COUNT       107
#define ID_EDIT_PORTS       108

// 单个扫描区域 ID
#define ID_EDIT_SINGLE_IP   120
#define ID_EDIT_SINGLE_PORT 121
#define ID_BTN_SINGLE_SCAN  122

// 控制按钮 ID
#define ID_BTN_PING         109
#define ID_BTN_SCAN         110
#define ID_BTN_EXTRACT      111
#define ID_BTN_PROXY        112
#define ID_BTN_STOP         116 // 预留，本次重构暂未实现复杂的停止逻辑

// 结果与状态
#define ID_LIST_RESULT      113
#define ID_STATUS_BAR       114
#define ID_BTN_EXPORT       115

// 全局变量
HINSTANCE hInst;
HWND hMainWnd, hList, hStatus;
HWND hEditFile, hEditText, hEditTimeout, hEditCount, hEditPorts;
HWND hEditSingleIp, hEditSinglePort;
HWND hBtnProxy;
int isProxySet = 0;

// 声明
LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);

// --- 辅助函数 ---

// 获取控件文本（需要手动 free）
char* get_alloc_text(HWND hwnd) {
    int len = GetWindowTextLengthA(hwnd);
    if (len == 0) return _strdup("");
    char* buf = (char*)malloc(len + 2);
    GetWindowTextA(hwnd, buf, len + 1);
    return buf;
}

// 添加列表行 (格式: col1|col2|col3...)
void add_list_row(const char* pipedData) {
    char* copy = _strdup(pipedData);
    char* ctx;
    char* token = strtok_s(copy, "|", &ctx);
    
    LVITEMA lvItem = {0};
    lvItem.mask = LVIF_TEXT;
    lvItem.iItem = ListView_GetItemCount(hList);
    lvItem.iSubItem = 0;
    lvItem.pszText = token ? token : "";
    
    ListView_InsertItemA(hList, &lvItem);
    
    int col = 1;
    while ((token = strtok_s(NULL, "|", &ctx))) {
        ListView_SetItemTextA(hList, lvItem.iItem, col++, token);
    }
    free(copy);
}

// 导出 CSV
void export_csv() {
    char path[MAX_PATH] = {0};
    OPENFILENAMEA ofn = {0};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hMainWnd;
    ofn.lpstrFilter = "CSV Files (*.csv)\0*.csv\0All Files\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_OVERWRITEPROMPT;
    
    if (GetSaveFileNameA(&ofn)) {
        FILE* fp;
        if (fopen_s(&fp, path, "w") == 0) {
            int rowCount = ListView_GetItemCount(hList);
            // 获取列头 (简单处理，假设最多5列)
            HWND hHeader = ListView_GetHeader(hList);
            int colCount = Header_GetItemCount(hHeader);
            
            // 写入表头
            char buf[256];
            for (int j = 0; j < colCount; j++) {
                HDITEMA hdi = { HDI_TEXT, 0, buf, NULL, 255, 0 };
                Header_GetItemA(hHeader, j, &hdi);
                fprintf(fp, "%s%s", j == 0 ? "" : ",", buf);
            }
            fprintf(fp, "\n");

            // 写入数据
            for (int i = 0; i < rowCount; i++) {
                for (int j = 0; j < colCount; j++) {
                    ListView_GetItemTextA(hList, i, j, buf, sizeof(buf));
                    fprintf(fp, "%s%s", j == 0 ? "" : ",", buf);
                }
                fprintf(fp, "\n");
            }
            fclose(fp);
            MessageBoxA(hMainWnd, "文件导出成功！", "成功", MB_OK);
        }
    }
}

// 启动任务辅助
void start_task(TaskType type) {
    ThreadParams* p = (ThreadParams*)malloc(sizeof(ThreadParams));
    if (!p) return;
    memset(p, 0, sizeof(ThreadParams));

    p->hwndNotify = hMainWnd;
    p->retryCount = GetDlgItemInt(hMainWnd, ID_EDIT_COUNT, NULL, FALSE);
    p->timeoutMs = GetDlgItemInt(hMainWnd, ID_EDIT_TIMEOUT, NULL, FALSE);
    
    // 根据任务类型获取不同的输入
    if (type == TASK_SINGLE_SCAN) {
        // 单个扫描：获取单独的输入框
        p->targetInput = get_alloc_text(hEditSingleIp); // 这里只存一个IP
        p->portsInput = get_alloc_text(hEditSinglePort);
    } else {
        // 批量任务：获取通用端口和目标源
        p->portsInput = get_alloc_text(hEditPorts);

        if (IsDlgButtonChecked(hMainWnd, ID_RADIO_FILE)) {
            char* path = get_alloc_text(hEditFile);
            FILE* f;
            if (fopen_s(&f, path, "rb") == 0) {
                fseek(f, 0, SEEK_END);
                long sz = ftell(f);
                fseek(f, 0, SEEK_SET);
                p->targetInput = (char*)malloc(sz + 1);
                if (p->targetInput) {
                    fread(p->targetInput, 1, sz, f);
                    p->targetInput[sz] = 0;
                }
                fclose(f);
            } else {
                p->targetInput = _strdup("");
                MessageBoxA(hMainWnd, "无法读取文件，请检查路径。", "错误", MB_ICONERROR);
            }
            free(path);
        } else {
            p->targetInput = get_alloc_text(hEditText);
        }
    }

    // 重置列表并设置表头
    ListView_DeleteAllItems(hList);
    while(ListView_DeleteColumn(hList, 0));

    if (type == TASK_PING) {
        char* cols[] = {"目标地址", "状态", "平均延迟(ms)", "丢包率(%)", "TTL"};
        for(int i=0; i<5; i++) {
            LVCOLUMNA lvc = {0}; lvc.mask = LVCF_TEXT | LVCF_WIDTH; lvc.pszText = cols[i]; lvc.cx = (i==0?180:100);
            ListView_InsertColumnA(hList, i, &lvc);
        }
        _beginthreadex(NULL, 0, thread_ping, p, 0, NULL);
    } 
    else if (type == TASK_SCAN) {
        char* cols[] = {"目标地址", "端口", "状态"};
        for(int i=0; i<3; i++) {
            LVCOLUMNA lvc = {0}; lvc.mask = LVCF_TEXT | LVCF_WIDTH; lvc.pszText = cols[i]; lvc.cx = (i==0?200:100);
            ListView_InsertColumnA(hList, i, &lvc);
        }
        _beginthreadex(NULL, 0, thread_port_scan, p, 0, NULL);
    } 
    else if (type == TASK_EXTRACT) {
        char* cols[] = {"提取到的IP地址"};
        LVCOLUMNA lvc = {0}; lvc.mask = LVCF_TEXT|LVCF_WIDTH; lvc.pszText = cols[0]; lvc.cx = 300;
        ListView_InsertColumnA(hList, 0, &lvc);
        _beginthreadex(NULL, 0, thread_extract_ip, p, 0, NULL);
    }
    else if (type == TASK_SINGLE_SCAN) {
        char* cols[] = {"目标地址", "开放端口", "服务/备注"};
        for(int i=0; i<3; i++) {
            LVCOLUMNA lvc = {0}; lvc.mask = LVCF_TEXT|LVCF_WIDTH; lvc.pszText = cols[i]; lvc.cx = (i==0?150:100);
            ListView_InsertColumnA(hList, i, &lvc);
        }
        _beginthreadex(NULL, 0, thread_single_scan, p, 0, NULL);
    }
}

// --- 主程序入口 ---
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    hInst = hInstance;
    WSADATA wsa;
    WSAStartup(MAKEWORD(2,2), &wsa);
    InitCommonControls(); 

    WNDCLASSEXA wc = {0};
    wc.cbSize = sizeof(WNDCLASSEX);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW+1);
    wc.lpszClassName = "NetToolProClass";
    // 加载图标 (假设资源中有 IDI_ICON1)
    wc.hIcon = LoadIcon(hInstance, "IDI_MAIN_ICON"); 
    RegisterClassExA(&wc);

    hMainWnd = CreateWindowExA(WS_EX_ACCEPTFILES, // 启用拖拽支持
        "NetToolProClass", "多功能网络工具 (C语言 Win32版)", 
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, 
        CW_USEDEFAULT, CW_USEDEFAULT, 920, 750, NULL, NULL, hInstance, NULL);

    ShowWindow(hMainWnd, nCmdShow);
    UpdateWindow(hMainWnd);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    WSACleanup();
    return (int)msg.wParam;
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE:
        {
            HFONT hFont = CreateFontA(14, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH, "Microsoft YaHei");

            // === 1. 批量任务输入区 (Group Box 模拟) ===
            int grp1Y = 10;
            CreateWindowA("BUTTON", "批量任务设置", WS_CHILD|WS_VISIBLE|BS_GROUPBOX, 10, grp1Y, 880, 230, hWnd, NULL, hInst, NULL);
            
            // 输入源选择
            CreateWindowA("BUTTON", "从文件:", WS_CHILD|WS_VISIBLE|BS_AUTORADIOBUTTON, 30, grp1Y+25, 80, 20, hWnd, (HMENU)ID_RADIO_FILE, hInst, NULL);
            hEditFile = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "", WS_CHILD|WS_VISIBLE|ES_AUTOHSCROLL, 110, grp1Y+23, 300, 23, hWnd, (HMENU)ID_EDIT_FILE, hInst, NULL);
            CreateWindowA("BUTTON", "浏览...", WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON, 420, grp1Y+23, 60, 23, hWnd, (HMENU)ID_BTN_BROWSE, hInst, NULL);
            CheckDlgButton(hWnd, ID_RADIO_FILE, BST_CHECKED);

            CreateWindowA("BUTTON", "粘贴文本:", WS_CHILD|WS_VISIBLE|BS_AUTORADIOBUTTON, 30, grp1Y+55, 80, 20, hWnd, (HMENU)ID_RADIO_TEXT, hInst, NULL);
            hEditText = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "", WS_CHILD|WS_VISIBLE|WS_VSCROLL|ES_MULTILINE|ES_WANTRETURN, 110, grp1Y+55, 370, 90, hWnd, (HMENU)ID_EDIT_TEXT, hInst, NULL);

            // 文本框右侧：Ping 参数
            CreateWindowA("STATIC", "Ping超时(ms):", WS_CHILD|WS_VISIBLE, 500, grp1Y+55, 90, 20, hWnd, NULL, hInst, NULL);
            hEditTimeout = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "1000", WS_CHILD|WS_VISIBLE|ES_NUMBER, 600, grp1Y+53, 60, 23, hWnd, (HMENU)ID_EDIT_TIMEOUT, hInst, NULL);
            
            CreateWindowA("STATIC", "Ping次数:", WS_CHILD|WS_VISIBLE, 500, grp1Y+85, 90, 20, hWnd, NULL, hInst, NULL);
            hEditCount = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "5", WS_CHILD|WS_VISIBLE|ES_NUMBER, 600, grp1Y+83, 60, 23, hWnd, (HMENU)ID_EDIT_COUNT, hInst, NULL);

            // 批量端口
            CreateWindowA("STATIC", "批量扫描端口:", WS_CHILD|WS_VISIBLE, 30, grp1Y+160, 90, 20, hWnd, NULL, hInst, NULL);
            hEditPorts = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "80,443,8080,1433,3306,3389", WS_CHILD|WS_VISIBLE|ES_AUTOHSCROLL, 120, grp1Y+158, 750, 23, hWnd, (HMENU)ID_EDIT_PORTS, hInst, NULL);

            // 功能按钮
            int btnY = grp1Y + 195;
            CreateWindowA("BUTTON", "开始批量 Ping", WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON, 30, btnY, 120, 30, hWnd, (HMENU)ID_BTN_PING, hInst, NULL);
            CreateWindowA("BUTTON", "批量端口扫描", WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON, 160, btnY, 120, 30, hWnd, (HMENU)ID_BTN_SCAN, hInst, NULL);
            CreateWindowA("BUTTON", "从文本提取IP", WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON, 290, btnY, 120, 30, hWnd, (HMENU)ID_BTN_EXTRACT, hInst, NULL);
            hBtnProxy = CreateWindowA("BUTTON", "设置系统代理", WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON, 420, btnY, 120, 30, hWnd, (HMENU)ID_BTN_PROXY, hInst, NULL);

            // === 2. 单个目标扫描区 (补全遗漏功能) ===
            int grp2Y = 250;
            CreateWindowA("BUTTON", "单个目标扫描", WS_CHILD|WS_VISIBLE|BS_GROUPBOX, 10, grp2Y, 880, 60, hWnd, NULL, hInst, NULL);
            
            CreateWindowA("STATIC", "目标IP:", WS_CHILD|WS_VISIBLE, 30, grp2Y+25, 50, 20, hWnd, NULL, hInst, NULL);
            hEditSingleIp = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "127.0.0.1", WS_CHILD|WS_VISIBLE, 85, grp2Y+23, 120, 23, hWnd, (HMENU)ID_EDIT_SINGLE_IP, hInst, NULL);
            
            CreateWindowA("STATIC", "端口范围:", WS_CHILD|WS_VISIBLE, 220, grp2Y+25, 60, 20, hWnd, NULL, hInst, NULL);
            hEditSinglePort = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "1-1024, 3306, 8080", WS_CHILD|WS_VISIBLE, 285, grp2Y+23, 300, 23, hWnd, (HMENU)ID_EDIT_SINGLE_PORT, hInst, NULL);
            
            CreateWindowA("BUTTON", "扫描指定目标", WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON, 600, grp2Y+20, 120, 30, hWnd, (HMENU)ID_BTN_SINGLE_SCAN, hInst, NULL);

            // === 3. 结果列表与底部 ===
            CreateWindowA("STATIC", "运行结果:", WS_CHILD|WS_VISIBLE, 10, 320, 80, 20, hWnd, NULL, hInst, NULL);
            
            hList = CreateWindowExA(WS_EX_CLIENTEDGE, WC_LISTVIEWA, "", 
                WS_CHILD|WS_VISIBLE|LVS_REPORT|LVS_SHOWSELALWAYS, 
                10, 340, 880, 320, hWnd, (HMENU)ID_LIST_RESULT, hInst, NULL);
            ListView_SetExtendedListViewStyle(hList, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);

            CreateWindowA("BUTTON", "导出结果为 CSV", WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON, 10, 670, 120, 25, hWnd, (HMENU)ID_BTN_EXPORT, hInst, NULL);
            
            hStatus = CreateWindowExA(0, STATUSCLASSNAMEA, "就绪 - 支持拖拽文件输入", WS_CHILD|WS_VISIBLE|SBARS_SIZEGRIP, 0, 0, 0, 0, hWnd, (HMENU)ID_STATUS_BAR, hInst, NULL);

            // 统一设置字体
            EnumChildWindows(hWnd, (WNDENUMPROC)(void(*)(HWND,LPARAM))SendMessageA, (LPARAM)hFont);
        }
        break;

    case WM_DROPFILES:
        {
            HDROP hDrop = (HDROP)wParam;
            char filePath[MAX_PATH];
            if (DragQueryFileA(hDrop, 0, filePath, MAX_PATH)) {
                SetWindowTextA(hEditFile, filePath);
                CheckDlgButton(hWnd, ID_RADIO_FILE, BST_CHECKED);
                CheckDlgButton(hWnd, ID_RADIO_TEXT, BST_UNCHECKED);
                SendMessageA(hStatus, SB_SETTEXTA, 0, (LPARAM)"文件已加载，请选择功能开始。");
            }
            DragFinish(hDrop);
        }
        break;

    case WM_COMMAND:
        switch(LOWORD(wParam)) {
        case ID_BTN_BROWSE:
            {
                char path[MAX_PATH] = {0};
                OPENFILENAMEA ofn = {0};
                ofn.lStructSize = sizeof(ofn);
                ofn.hwndOwner = hWnd;
                ofn.lpstrFilter = "Text Files (*.txt)\0*.txt\0All Files\0*.*\0";
                ofn.lpstrFile = path;
                ofn.nMaxFile = MAX_PATH;
                ofn.Flags = OFN_FILEMUSTEXIST;
                if (GetOpenFileNameA(&ofn)) {
                    SetWindowTextA(hEditFile, path);
                    CheckDlgButton(hWnd, ID_RADIO_FILE, BST_CHECKED);
                    CheckDlgButton(hWnd, ID_RADIO_TEXT, BST_UNCHECKED);
                }
            }
            break;
        case ID_RADIO_FILE:
        case ID_RADIO_TEXT:
            CheckDlgButton(hWnd, ID_RADIO_FILE, LOWORD(wParam)==ID_RADIO_FILE ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(hWnd, ID_RADIO_TEXT, LOWORD(wParam)==ID_RADIO_TEXT ? BST_CHECKED : BST_UNCHECKED);
            break;
        case ID_BTN_PING: start_task(TASK_PING); break;
        case ID_BTN_SCAN: start_task(TASK_SCAN); break;
        case ID_BTN_EXTRACT: start_task(TASK_EXTRACT); break;
        case ID_BTN_SINGLE_SCAN: start_task(TASK_SINGLE_SCAN); break;
        case ID_BTN_EXPORT: export_csv(); break;
        case ID_BTN_PROXY:
            if (isProxySet) {
                if (proxy_unset_system()) {
                    isProxySet = 0;
                    SetWindowTextA(hBtnProxy, "设置系统代理");
                    MessageBoxA(hWnd, "系统代理已取消。", "提示", MB_OK);
                }
            } else {
                int idx = ListView_GetSelectionMark(hList);
                if (idx == -1) {
                    MessageBoxA(hWnd, "请先在列表中选中包含 IP 和 端口 的一行。", "提示", MB_OK);
                    break;
                }
                char ip[64] = {0}, portStr[16] = {0};
                ListView_GetItemTextA(hList, idx, 0, ip, sizeof(ip)); 
                ListView_GetItemTextA(hList, idx, 1, portStr, sizeof(portStr));
                
                int port = atoi(portStr);
                if (port > 0 && port < 65536) {
                    if (proxy_set_system(ip, port)) {
                        isProxySet = 1;
                        SetWindowTextA(hBtnProxy, "取消系统代理");
                        char msg[128];
                        snprintf(msg, sizeof(msg), "代理已设置为 %s:%d", ip, port);
                        MessageBoxA(hWnd, msg, "成功", MB_OK);
                    } else {
                        MessageBoxA(hWnd, "设置代理失败，请检查注册表权限。", "错误", MB_ICONERROR);
                    }
                } else {
                    MessageBoxA(hWnd, "无效的端口格式，请确保选中行的第二列是端口号。", "错误", MB_ICONERROR);
                }
            }
            break;
        }
        break;

    case WM_USER_LOG:
        {
            char* text = (char*)lParam;
            SendMessageA(hStatus, SB_SETTEXTA, 0, (LPARAM)text);
            free(text);
        }
        break;
    case WM_USER_RESULT:
        {
            char* row = (char*)lParam;
            add_list_row(row);
            free(row);
        }
        break;
    case WM_USER_FINISH:
        {
            char* msg = (char*)lParam;
            SendMessageA(hStatus, SB_SETTEXTA, 0, (LPARAM)msg);
            MessageBoxA(hWnd, msg, "任务完成", MB_OK);
            free(msg);
        }
        break;
        
    case WM_SIZE:
        SendMessage(hStatus, WM_SIZE, 0, 0);
        if (hList) {
            RECT rc;
            GetClientRect(hWnd, &rc);
            // 简单调整列表大小
            SetWindowPos(hList, NULL, 0, 0, rc.right - 20, rc.bottom - 340 - 30, SWP_NOMOVE | SWP_NOZORDER);
        }
        break;

    case WM_DESTROY:
        if (isProxySet) proxy_unset_system();
        PostQuitMessage(0);
        break;

    default:
        return DefWindowProc(hWnd, message, wParam, lParam);
    }
    return 0;
}