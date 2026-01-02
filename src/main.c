#include "network_tools.h" 
#include <windows.h>
#include <commctrl.h>
#include <stdio.h>
#include <shlobj.h>
#include <stdlib.h> 
#include <wchar.h>

#pragma comment(lib, "comctl32.lib")

#pragma comment(linker,"/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

// --- 控件 ID 定义 ---
#define ID_BTN_BROWSE       101
#define ID_RADIO_FILE       102
#define ID_RADIO_TEXT       103
#define ID_EDIT_FILE        104
#define ID_EDIT_TEXT        105
#define ID_EDIT_TIMEOUT     106
#define ID_EDIT_COUNT       107
#define ID_EDIT_PORTS       108

#define ID_EDIT_SINGLE_IP   120
#define ID_EDIT_SINGLE_PORT 121
#define ID_BTN_SINGLE_SCAN  122

#define ID_BTN_PING         109
#define ID_BTN_SCAN         110
#define ID_BTN_EXTRACT      111
#define ID_BTN_PROXY        112
#define ID_BTN_STOP         116 

#define ID_LIST_RESULT      113
#define ID_STATUS_BAR       114
#define ID_BTN_EXPORT       115

// [新增] IP归属地复选框
#define ID_CHECK_LOCATION   118

// 右键菜单 ID
#define IDM_COPY            201
#define IDM_SELECT_ALL      202
#define IDM_DEL_OFFLINE     203
#define IDM_DEL_SELECTED    204 

HINSTANCE hInst;
HWND hMainWnd, hList, hStatus;
HWND hEditFile, hEditText, hEditTimeout, hEditCount, hEditPorts;
HWND hEditSingleIp, hEditSinglePort;
HWND hBtnProxy;
int isProxySet = 0;
HFONT hSystemFont = NULL; 
TaskType g_currentTask = 0; 

int g_sortColumn = -1;      
BOOL g_sortAscending = TRUE; 

LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);

void EnableDPIAwareness() {
    HMODULE hUser32 = GetModuleHandleW(L"user32.dll");
    if (hUser32) {
        typedef BOOL (WINAPI * SetProcessDPIAwareFunc)();
        SetProcessDPIAwareFunc setDPI = (SetProcessDPIAwareFunc)GetProcAddress(hUser32, "SetProcessDPIAware");
        if (setDPI) setDPI();
    }
}

HFONT GetFixedSystemFont() {
    HDC hdc = GetDC(NULL);
    int logPixelsY = GetDeviceCaps(hdc, LOGPIXELSY);
    ReleaseDC(NULL, hdc);
    int height = -MulDiv(9, logPixelsY, 72); 
    return CreateFontW(height, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, 
                      DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, 
                      DEFAULT_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Microsoft YaHei UI");
}

BOOL CALLBACK EnumChildProcSetFont(HWND hWndChild, LPARAM lParam) {
    SendMessageW(hWndChild, WM_SETFONT, (WPARAM)lParam, TRUE);
    return TRUE;
}

wchar_t* get_alloc_text(HWND hwnd) {
    int len = GetWindowTextLengthW(hwnd);
    if (len == 0) return _wcsdup(L"");
    wchar_t* buf = (wchar_t*)malloc((len + 2) * sizeof(wchar_t));
    GetWindowTextW(hwnd, buf, len + 1);
    return buf;
}

// [修改] 解析更多列
void add_list_row(const wchar_t* pipedData) {
    wchar_t* copy = _wcsdup(pipedData);
    wchar_t* ctx;
    wchar_t* token = wcstok_s(copy, L"|", &ctx);
    
    LVITEMW lvItem = {0};
    lvItem.mask = LVIF_TEXT;
    lvItem.iItem = ListView_GetItemCount(hList);
    lvItem.iSubItem = 0;
    lvItem.pszText = token ? token : L"";
    
    ListView_InsertItem(hList, &lvItem);
    
    int col = 1;
    while ((token = wcstok_s(NULL, L"|", &ctx))) {
        // [修复] 增加花括号以避免宏展开导致的 if-else 错误
        if (wcscmp(token, L"-") == 0) {
            ListView_SetItemText(hList, lvItem.iItem, col++, L"");
        } else {
            ListView_SetItemText(hList, lvItem.iItem, col++, token);
        }
    }
    free(copy);
}

int CALLBACK CompareListViewItems(LPARAM lParam1, LPARAM lParam2, LPARAM lParamSort) {
    int col = (int)lParamSort; 
    
    wchar_t buf1[64] = {0}, buf2[64] = {0};
    ListView_GetItemText(hList, (int)lParam1, col, buf1, 63);
    ListView_GetItemText(hList, (int)lParam2, col, buf2, 63);

    int result = 0;
    int isNumeric = 0;

    if (g_currentTask == TASK_PING && (col == 2 || col == 3 || col == 4)) {
        isNumeric = 1;
    }
    else if ((g_currentTask == TASK_SCAN || g_currentTask == TASK_SINGLE_SCAN) && col == 1) {
        isNumeric = 1;
    }

    if (isNumeric) {
        if (g_currentTask == TASK_PING) {
             int isInvalid1 = (wcsstr(buf1, L"N/A") || wcsstr(buf1, L"超时") || wcslen(buf1)==0);
             int isInvalid2 = (wcsstr(buf2, L"N/A") || wcsstr(buf2, L"超时") || wcslen(buf2)==0);
             
             if (isInvalid1 && !isInvalid2) result = 1;
             else if (!isInvalid1 && isInvalid2) result = -1;
             else if (isInvalid1 && isInvalid2) result = 0;
             else {
                 double v1 = _wtof(buf1);
                 double v2 = _wtof(buf2);
                 if (v1 > v2) result = 1;
                 else if (v1 < v2) result = -1;
             }
        } else {
            int p1 = _wtoi(buf1);
            int p2 = _wtoi(buf2);
            if (p1 > p2) result = 1;
            else if (p1 < p2) result = -1;
        }
    } else {
        result = wcscmp(buf1, buf2);
    }
    return g_sortAscending ? result : -result;
}

void CopyListViewSelection() {
    int count = ListView_GetSelectedCount(hList);
    if (count == 0) return;
    int bufSize = count * 256; 
    wchar_t* buffer = (wchar_t*)malloc(bufSize * sizeof(wchar_t));
    buffer[0] = 0;
    int item = -1;
    while ((item = ListView_GetNextItem(hList, item, LVNI_SELECTED)) != -1) {
        wchar_t line[1024] = {0};
        wchar_t cell[256];
        HWND hHeader = ListView_GetHeader(hList);
        int cols = Header_GetItemCount(hHeader);
        for (int i = 0; i < cols; i++) {
            ListView_GetItemText(hList, item, i, cell, 256);
            wcscat_s(line, 1024, cell);
            if (i < cols - 1) wcscat_s(line, 1024, L"\t");
        }
        wcscat_s(line, 1024, L"\r\n");
        if (wcslen(buffer) + wcslen(line) >= bufSize) {
            bufSize *= 2;
            buffer = (wchar_t*)realloc(buffer, bufSize * sizeof(wchar_t));
        }
        wcscat_s(buffer, bufSize, line);
    }
    if (OpenClipboard(hMainWnd)) {
        EmptyClipboard();
        size_t size = (wcslen(buffer) + 1) * sizeof(wchar_t);
        HGLOBAL hGlob = GlobalAlloc(GMEM_MOVEABLE, size);
        memcpy(GlobalLock(hGlob), buffer, size);
        GlobalUnlock(hGlob);
        SetClipboardData(CF_UNICODETEXT, hGlob);
        CloseClipboard();
    }
    free(buffer);
}

void DeleteOfflineItems() {
    int count = ListView_GetItemCount(hList);
    SendMessage(hList, WM_SETREDRAW, FALSE, 0); 
    for (int i = count - 1; i >= 0; i--) {
        wchar_t status[64];
        ListView_GetItemText(hList, i, 1, status, 64); 
        if (wcsstr(status, L"超时") || wcsstr(status, L"无效")) {
            ListView_DeleteItem(hList, i);
        }
    }
    SendMessage(hList, WM_SETREDRAW, TRUE, 0);
}

void DeleteSelectedItems() {
    int count = ListView_GetItemCount(hList);
    if (count <= 0) return;
    SendMessage(hList, WM_SETREDRAW, FALSE, 0); 
    for (int i = count - 1; i >= 0; i--) {
        if (ListView_GetItemState(hList, i, LVIS_SELECTED) == LVIS_SELECTED) {
            ListView_DeleteItem(hList, i);
        }
    }
    SendMessage(hList, WM_SETREDRAW, TRUE, 0);
}

void export_csv() {
    wchar_t path[MAX_PATH] = {0};
    OPENFILENAMEW ofn = {0};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hMainWnd;
    ofn.lpstrFilter = L"CSV Files (*.csv)\0*.csv\0All Files\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_OVERWRITEPROMPT;
    
    if (GetSaveFileNameW(&ofn)) {
        FILE* fp;
        if (_wfopen_s(&fp, path, L"w, ccs=UTF-8") == 0) { 
            int rowCount = ListView_GetItemCount(hList);
            HWND hHeader = ListView_GetHeader(hList);
            int colCount = Header_GetItemCount(hHeader);
            wchar_t buf[256];
            for (int j = 0; j < colCount; j++) {
                HDITEMW hdi = { HDI_TEXT, 0, buf, NULL, 255, 0 };
                Header_GetItem(hHeader, j, &hdi);
                fwprintf(fp, L"%s%s", j == 0 ? L"" : L",", buf);
            }
            fwprintf(fp, L"\n");
            for (int i = 0; i < rowCount; i++) {
                for (int j = 0; j < colCount; j++) {
                    ListView_GetItemText(hList, i, j, buf, sizeof(buf)/sizeof(wchar_t));
                    fwprintf(fp, L"%s%s", j == 0 ? L"" : L",", buf);
                }
                fwprintf(fp, L"\n");
            }
            fclose(fp);
            MessageBoxW(hMainWnd, L"文件导出成功！", L"成功", MB_OK);
        }
    }
}

void start_task(TaskType type) {
    reset_stop_task();
    g_currentTask = type;
    g_sortColumn = -1;
    g_sortAscending = TRUE;

    ThreadParams* p = (ThreadParams*)malloc(sizeof(ThreadParams));
    if (!p) return;
    memset(p, 0, sizeof(ThreadParams));

    p->hwndNotify = hMainWnd;
    p->retryCount = GetDlgItemInt(hMainWnd, ID_EDIT_COUNT, NULL, FALSE);
    p->timeoutMs = GetDlgItemInt(hMainWnd, ID_EDIT_TIMEOUT, NULL, FALSE);
    
    // [New] 获取归属地复选框状态
    p->showLocation = (IsDlgButtonChecked(hMainWnd, ID_CHECK_LOCATION) == BST_CHECKED);

    if (type == TASK_SINGLE_SCAN) {
        p->targetInput = get_alloc_text(hEditSingleIp); 
        p->portsInput = get_alloc_text(hEditSinglePort);
    } else {
        p->portsInput = get_alloc_text(hEditPorts);
        if (IsDlgButtonChecked(hMainWnd, ID_RADIO_FILE)) {
            wchar_t* path = get_alloc_text(hEditFile);
            FILE* f;
            if (_wfopen_s(&f, path, L"rb") == 0) {
                fseek(f, 0, SEEK_END);
                long sz = ftell(f);
                fseek(f, 0, SEEK_SET);
                char* buffer = (char*)malloc(sz + 1);
                fread(buffer, 1, sz, f);
                buffer[sz] = 0;
                fclose(f);
                int wlen = MultiByteToWideChar(CP_UTF8, 0, buffer, -1, NULL, 0);
                if (wlen == 0) wlen = MultiByteToWideChar(CP_ACP, 0, buffer, -1, NULL, 0);
                p->targetInput = (wchar_t*)malloc((wlen + 1) * sizeof(wchar_t));
                MultiByteToWideChar(CP_UTF8, 0, buffer, -1, p->targetInput, wlen);
                p->targetInput[wlen] = 0;
                free(buffer);
            } else {
                p->targetInput = _wcsdup(L"");
                MessageBoxW(hMainWnd, L"无法读取文件，请检查路径。", L"错误", MB_ICONERROR);
            }
            free(path);
        } else {
            p->targetInput = get_alloc_text(hEditText);
        }
    }

    ListView_DeleteAllItems(hList);
    while(ListView_DeleteColumn(hList, 0));

    if (type == TASK_PING) {
        // [修改] 动态增加归属地列
        int colIdx = 0;
        wchar_t* cols[] = {L"目标地址", L"状态", L"平均延迟(ms)", L"丢包率(%)", L"TTL"};
        for(int i=0; i<5; i++) {
            LVCOLUMNW lvc = {0}; lvc.mask = LVCF_TEXT | LVCF_WIDTH; lvc.pszText = cols[i]; lvc.cx = (i==0?180:100);
            ListView_InsertColumn(hList, colIdx++, &lvc);
        }
        if (p->showLocation) {
            LVCOLUMNW lvc = {0}; lvc.mask = LVCF_TEXT | LVCF_WIDTH; lvc.pszText = L"归属地"; lvc.cx = 200;
            ListView_InsertColumn(hList, colIdx++, &lvc);
        }
        _beginthreadex(NULL, 0, thread_ping, p, 0, NULL);
    } 
    else if (type == TASK_SCAN) {
        int colIdx = 0;
        wchar_t* cols[] = {L"目标地址", L"端口", L"状态"};
        for(int i=0; i<3; i++) {
            LVCOLUMNW lvc = {0}; lvc.mask = LVCF_TEXT | LVCF_WIDTH; lvc.pszText = cols[i]; lvc.cx = (i==0?200:100);
            ListView_InsertColumn(hList, colIdx++, &lvc);
        }
        if (p->showLocation) {
            LVCOLUMNW lvc = {0}; lvc.mask = LVCF_TEXT | LVCF_WIDTH; lvc.pszText = L"归属地"; lvc.cx = 200;
            ListView_InsertColumn(hList, colIdx++, &lvc);
        }
        _beginthreadex(NULL, 0, thread_port_scan, p, 0, NULL);
    } 
    else if (type == TASK_EXTRACT) {
        LVCOLUMNW lvc = {0}; lvc.mask = LVCF_TEXT|LVCF_WIDTH; lvc.pszText = L"提取到的IP地址"; lvc.cx = 300;
        ListView_InsertColumn(hList, 0, &lvc);
        if (p->showLocation) {
            LVCOLUMNW lvc2 = {0}; lvc2.mask = LVCF_TEXT | LVCF_WIDTH; lvc2.pszText = L"归属地"; lvc2.cx = 200;
            ListView_InsertColumn(hList, 1, &lvc2);
        }
        _beginthreadex(NULL, 0, thread_extract_ip, p, 0, NULL);
    }
    else if (type == TASK_SINGLE_SCAN) {
        int colIdx = 0;
        wchar_t* cols[] = {L"目标地址", L"开放端口", L"服务/备注"};
        for(int i=0; i<3; i++) {
            LVCOLUMNW lvc = {0}; lvc.mask = LVCF_TEXT|LVCF_WIDTH; lvc.pszText = cols[i]; lvc.cx = (i==0?150:100);
            ListView_InsertColumn(hList, colIdx++, &lvc);
        }
        if (p->showLocation) {
            LVCOLUMNW lvc = {0}; lvc.mask = LVCF_TEXT | LVCF_WIDTH; lvc.pszText = L"归属地"; lvc.cx = 200;
            ListView_InsertColumn(hList, colIdx++, &lvc);
        }
        _beginthreadex(NULL, 0, thread_single_scan, p, 0, NULL);
    }
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPWSTR lpCmdLine, int nCmdShow) {
    hInst = hInstance;
    EnableDPIAwareness();

    WSADATA wsa;
    WSAStartup(MAKEWORD(2,2), &wsa);
    
    INITCOMMONCONTROLSEX ic = {sizeof(INITCOMMONCONTROLSEX), ICC_STANDARD_CLASSES | ICC_WIN95_CLASSES};
    InitCommonControlsEx(&ic); 

    WNDCLASSEXW wc = {0};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1); // 灰色背景
    wc.lpszClassName = L"NetToolProClass";
    wc.hIcon = LoadIcon(hInstance, L"IDI_MAIN_ICON"); 
    wc.hIconSm = wc.hIcon;
    
    RegisterClassExW(&wc);

    hMainWnd = CreateWindowExW(WS_EX_ACCEPTFILES, 
        L"NetToolProClass", L"多功能网络工具 (C语言重构版 - Unicode)", 
        WS_OVERLAPPEDWINDOW, 
        CW_USEDEFAULT, CW_USEDEFAULT, 920, 750, NULL, NULL, hInstance, NULL);

    ShowWindow(hMainWnd, nCmdShow);
    UpdateWindow(hMainWnd);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    if (hSystemFont) DeleteObject(hSystemFont);
    WSACleanup();
    return (int)msg.wParam;
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE:
        {
            hSystemFont = GetFixedSystemFont();

            int grp1Y = 10;
            CreateWindowW(L"BUTTON", L"批量任务设置", WS_CHILD|WS_VISIBLE|BS_GROUPBOX, 10, grp1Y, 880, 230, hWnd, NULL, hInst, NULL);
            
            CreateWindowW(L"BUTTON", L"从文件:", WS_CHILD|WS_VISIBLE|BS_AUTORADIOBUTTON, 30, grp1Y+25, 80, 20, hWnd, (HMENU)ID_RADIO_FILE, hInst, NULL);
            hEditFile = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD|WS_VISIBLE|ES_AUTOHSCROLL, 110, grp1Y+23, 300, 23, hWnd, (HMENU)ID_EDIT_FILE, hInst, NULL);
            CreateWindowW(L"BUTTON", L"浏览...", WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON, 420, grp1Y+23, 60, 23, hWnd, (HMENU)ID_BTN_BROWSE, hInst, NULL);
            CheckDlgButton(hWnd, ID_RADIO_FILE, BST_CHECKED);

            CreateWindowW(L"BUTTON", L"粘贴文本:", WS_CHILD|WS_VISIBLE|BS_AUTORADIOBUTTON, 30, grp1Y+55, 80, 20, hWnd, (HMENU)ID_RADIO_TEXT, hInst, NULL);
            hEditText = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD|WS_VISIBLE|WS_VSCROLL|ES_MULTILINE|ES_WANTRETURN, 110, grp1Y+55, 370, 90, hWnd, (HMENU)ID_EDIT_TEXT, hInst, NULL);

            CreateWindowW(L"STATIC", L"Ping超时(ms):", WS_CHILD|WS_VISIBLE, 500, grp1Y+55, 90, 20, hWnd, NULL, hInst, NULL);
            hEditTimeout = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"1000", WS_CHILD|WS_VISIBLE|ES_NUMBER, 600, grp1Y+53, 60, 23, hWnd, (HMENU)ID_EDIT_TIMEOUT, hInst, NULL);
            
            CreateWindowW(L"STATIC", L"Ping次数:", WS_CHILD|WS_VISIBLE, 500, grp1Y+85, 90, 20, hWnd, NULL, hInst, NULL);
            hEditCount = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"5", WS_CHILD|WS_VISIBLE|ES_NUMBER, 600, grp1Y+83, 60, 23, hWnd, (HMENU)ID_EDIT_COUNT, hInst, NULL);

            // [New] 增加 IP 归属地复选框
            CreateWindowW(L"BUTTON", L"显示 IP 归属地 (需 qqwry.dat)", WS_CHILD|WS_VISIBLE|BS_AUTOCHECKBOX, 500, grp1Y+120, 200, 20, hWnd, (HMENU)ID_CHECK_LOCATION, hInst, NULL);
            CheckDlgButton(hWnd, ID_CHECK_LOCATION, BST_CHECKED); // 默认勾选

            CreateWindowW(L"STATIC", L"批量扫描端口:", WS_CHILD|WS_VISIBLE, 30, grp1Y+160, 90, 20, hWnd, NULL, hInst, NULL);
            hEditPorts = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"80,443,8080,1433,3306,3389", WS_CHILD|WS_VISIBLE|ES_AUTOHSCROLL, 120, grp1Y+158, 750, 23, hWnd, (HMENU)ID_EDIT_PORTS, hInst, NULL);

            int btnY = grp1Y + 195;
            CreateWindowW(L"BUTTON", L"开始批量 Ping", WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON, 30, btnY, 120, 30, hWnd, (HMENU)ID_BTN_PING, hInst, NULL);
            CreateWindowW(L"BUTTON", L"批量端口扫描", WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON, 160, btnY, 120, 30, hWnd, (HMENU)ID_BTN_SCAN, hInst, NULL);
            CreateWindowW(L"BUTTON", L"从文本提取IP", WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON, 290, btnY, 120, 30, hWnd, (HMENU)ID_BTN_EXTRACT, hInst, NULL);
            CreateWindowW(L"BUTTON", L"中止任务", WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON, 420, btnY, 100, 30, hWnd, (HMENU)ID_BTN_STOP, hInst, NULL);
            
            hBtnProxy = CreateWindowW(L"BUTTON", L"设置系统代理", WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON, 530, btnY, 100, 30, hWnd, (HMENU)ID_BTN_PROXY, hInst, NULL);

            int grp2Y = 250;
            CreateWindowW(L"BUTTON", L"单个目标扫描", WS_CHILD|WS_VISIBLE|BS_GROUPBOX, 10, grp2Y, 880, 60, hWnd, NULL, hInst, NULL);
            
            CreateWindowW(L"STATIC", L"目标IP:", WS_CHILD|WS_VISIBLE, 30, grp2Y+25, 50, 20, hWnd, NULL, hInst, NULL);
            hEditSingleIp = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"127.0.0.1", WS_CHILD|WS_VISIBLE, 85, grp2Y+23, 120, 23, hWnd, (HMENU)ID_EDIT_SINGLE_IP, hInst, NULL);
            
            CreateWindowW(L"STATIC", L"端口范围:", WS_CHILD|WS_VISIBLE, 220, grp2Y+25, 60, 20, hWnd, NULL, hInst, NULL);
            hEditSinglePort = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"1-1024, 3306, 8080", WS_CHILD|WS_VISIBLE, 285, grp2Y+23, 300, 23, hWnd, (HMENU)ID_EDIT_SINGLE_PORT, hInst, NULL);
            
            CreateWindowW(L"BUTTON", L"扫描指定目标", WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON, 600, grp2Y+20, 120, 30, hWnd, (HMENU)ID_BTN_SINGLE_SCAN, hInst, NULL);

            CreateWindowW(L"STATIC", L"运行结果 (右键可复制/全选):", WS_CHILD|WS_VISIBLE, 10, 320, 200, 20, hWnd, NULL, hInst, NULL);
            
            // 列表
            hList = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"", 
                WS_CHILD|WS_VISIBLE|LVS_REPORT|LVS_SHOWSELALWAYS, 
                10, 340, 880, 320, hWnd, (HMENU)ID_LIST_RESULT, hInst, NULL);
            ListView_SetExtendedListViewStyle(hList, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);

            CreateWindowW(L"BUTTON", L"导出结果为 CSV", WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON, 10, 670, 120, 25, hWnd, (HMENU)ID_BTN_EXPORT, hInst, NULL);
            
            hStatus = CreateWindowExW(0, STATUSCLASSNAMEW, L"就绪 - 支持拖拽文件输入", WS_CHILD|WS_VISIBLE|SBARS_SIZEGRIP, 0, 0, 0, 0, hWnd, (HMENU)ID_STATUS_BAR, hInst, NULL);

            EnumChildWindows(hWnd, EnumChildProcSetFont, (LPARAM)hSystemFont);
        }
        break;

    case WM_NOTIFY:
        {
            LPNMHDR pnmh = (LPNMHDR)lParam;
            if (pnmh->idFrom == ID_LIST_RESULT && pnmh->code == LVN_COLUMNCLICK) {
                LPNMLISTVIEW pnmv = (LPNMLISTVIEW)lParam;
                int column = pnmv->iSubItem;
                if (column != g_sortColumn) {
                    g_sortColumn = column;
                    g_sortAscending = TRUE;
                } else {
                    g_sortAscending = !g_sortAscending;
                }
                ListView_SortItemsEx(hList, CompareListViewItems, (LPARAM)column);
            }
            else if (pnmh->idFrom == ID_LIST_RESULT && pnmh->code == NM_RCLICK) {
                POINT pt;
                GetCursorPos(&pt);
                HMENU hMenu = CreatePopupMenu();
                AppendMenuW(hMenu, MF_STRING, IDM_COPY, L"复制选中内容");
                AppendMenuW(hMenu, MF_STRING, IDM_SELECT_ALL, L"全选");
                AppendMenuW(hMenu, MF_STRING, IDM_DEL_SELECTED, L"删除选中项");
                if (g_currentTask == TASK_PING) {
                    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
                    AppendMenuW(hMenu, MF_STRING, IDM_DEL_OFFLINE, L"删除不在线/超时结果");
                }
                TrackPopupMenu(hMenu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hWnd, NULL);
                DestroyMenu(hMenu);
            }
        }
        break;

    case WM_DROPFILES:
        {
            HDROP hDrop = (HDROP)wParam;
            wchar_t filePath[MAX_PATH];
            if (DragQueryFileW(hDrop, 0, filePath, MAX_PATH)) {
                SetWindowTextW(hEditFile, filePath);
                CheckDlgButton(hWnd, ID_RADIO_FILE, BST_CHECKED);
                CheckDlgButton(hWnd, ID_RADIO_TEXT, BST_UNCHECKED);
                SendMessageW(hStatus, SB_SETTEXTW, 0, (LPARAM)L"文件已加载，请选择功能开始。");
            }
            DragFinish(hDrop);
        }
        break;

    case WM_COMMAND:
        switch(LOWORD(wParam)) {
        case IDM_COPY: CopyListViewSelection(); break;
        case IDM_SELECT_ALL: ListView_SetItemState(hList, -1, LVIS_SELECTED, LVIS_SELECTED); break;
        case IDM_DEL_OFFLINE: DeleteOfflineItems(); break;
        case IDM_DEL_SELECTED: DeleteSelectedItems(); break;

        case ID_BTN_STOP: 
            signal_stop_task(); 
            SendMessageW(hStatus, SB_SETTEXTW, 0, (LPARAM)L"正在尝试中止任务...");
            break;

        case ID_BTN_BROWSE:
            {
                wchar_t path[MAX_PATH] = {0};
                OPENFILENAMEW ofn = {0};
                ofn.lStructSize = sizeof(ofn);
                ofn.hwndOwner = hWnd;
                ofn.lpstrFilter = L"Text Files (*.txt)\0*.txt\0All Files\0*.*\0";
                ofn.lpstrFile = path;
                ofn.nMaxFile = MAX_PATH;
                ofn.Flags = OFN_FILEMUSTEXIST;
                if (GetOpenFileNameW(&ofn)) {
                    SetWindowTextW(hEditFile, path);
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
                    SetWindowTextW(hBtnProxy, L"设置系统代理");
                    MessageBoxW(hWnd, L"系统代理已取消。", L"提示", MB_OK);
                }
            } else {
                int idx = ListView_GetSelectionMark(hList);
                if (idx == -1) {
                    MessageBoxW(hWnd, L"请先在列表中选中包含 IP 和 端口 的一行。", L"提示", MB_OK);
                    break;
                }
                wchar_t ip[64] = {0}, portStr[16] = {0};
                ListView_GetItemText(hList, idx, 0, ip, sizeof(ip)/sizeof(wchar_t)); 
                ListView_GetItemText(hList, idx, 1, portStr, sizeof(portStr)/sizeof(wchar_t));
                
                int port = _wtoi(portStr);
                if (port > 0 && port < 65536) {
                    if (proxy_set_system(ip, port)) {
                        isProxySet = 1;
                        SetWindowTextW(hBtnProxy, L"取消系统代理");
                        wchar_t msg[128];
                        swprintf_s(msg, 128, L"代理已设置为 %s:%d", ip, port);
                        MessageBoxW(hWnd, msg, L"成功", MB_OK);
                    } else {
                        MessageBoxW(hWnd, L"设置代理失败，请检查注册表权限。", L"错误", MB_ICONERROR);
                    }
                } else {
                    MessageBoxW(hWnd, L"无效的端口格式，请确保选中行的第二列是端口号。", L"错误", MB_ICONERROR);
                }
            }
            break;
        }
        break;

    case WM_USER_LOG:
        {
            wchar_t* text = (wchar_t*)lParam;
            SendMessageW(hStatus, SB_SETTEXTW, 0, (LPARAM)text);
            free(text);
        }
        break;
    case WM_USER_RESULT:
        {
            wchar_t* row = (wchar_t*)lParam;
            add_list_row(row);
            free(row);
        }
        break;
    case WM_USER_FINISH:
        {
            wchar_t* msg = (wchar_t*)lParam;
            SendMessageW(hStatus, SB_SETTEXTW, 0, (LPARAM)msg);
            free(msg);
        }
        break;
        
    case WM_SIZE:
        SendMessage(hStatus, WM_SIZE, 0, 0);
        RECT rc;
        GetClientRect(hWnd, &rc);
        
        RECT rcStatus;
        GetWindowRect(hStatus, &rcStatus);
        int statusHeight = rcStatus.bottom - rcStatus.top;
        if (statusHeight == 0) statusHeight = 25; 

        HWND hBtnExport = GetDlgItem(hWnd, ID_BTN_EXPORT);
        if (hBtnExport) {
            int btnW = 120;
            int btnH = 25;
            int margin = 10;
            int x = rc.right - btnW - margin;
            int y = rc.bottom - statusHeight - btnH - 5; 
            SetWindowPos(hBtnExport, NULL, x, y, btnW, btnH, SWP_NOZORDER);
        }

        if (hList) {
            int listBottomMargin = statusHeight + 35; 
            int listH = rc.bottom - 340 - listBottomMargin;
            if (listH < 100) listH = 100; 
            SetWindowPos(hList, NULL, 0, 0, rc.right - 20, listH, SWP_NOMOVE | SWP_NOZORDER);
        }
        break;

    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
        {
            HDC hdc = (HDC)wParam;
            SetBkColor(hdc, GetSysColor(COLOR_BTNFACE));
            SetBkMode(hdc, TRANSPARENT);
            return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
        }

    case WM_DESTROY:
        if (isProxySet) proxy_unset_system();
        PostQuitMessage(0);
        break;

    default:
        return DefWindowProcW(hWnd, message, wParam, lParam);
    }
    return 0;
}
