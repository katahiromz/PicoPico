// 「パソコン★ピコピコ化計画」 by 片山博文MZ.
// License: MIT
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <mmsystem.h>
#include <shlwapi.h>
#include "resource.h"
#include "SaveBitmapToFile.h"

enum PICO_TYPE
{
    PICO_KEY = 0,
    PICO_MOUSE = 1,
    PICO_WND = 2,
    MAX_PICO = 3,
};

#define WM_PICO_KEY (WM_USER + 100)
#define WM_PICO_MOUSE (WM_USER + 101)
#define WM_PICO_WND (WM_USER + 102)
#define WM_NOTIFY_ICON (WM_USER + 200)

#define TIMERID_WND_CHECK 999

static HINSTANCE s_hInst = NULL;
static HWND s_hMainWnd = NULL;
static LONG s_nLock = 0;
static TCHAR s_szWavFile[MAX_PICO][MAX_PATH];
static HHOOK s_hKeyHook = NULL;
static HHOOK s_hMouseHook = NULL;
static HWND s_hwndFore = NULL;
static HBITMAP s_hBitmap = NULL;
static BOOL s_bKeep = FALSE;
static BOOL s_bEnabled = TRUE;
static BOOL s_bFullScreen = FALSE;
static TCHAR s_szNone[MAX_PATH];
static INT s_nThreshold = 32;
static UINT s_uTaskbarRestart = 0;
static BOOL s_bHideOnInit = FALSE;

HBITMAP CaptureScreen(const RECT& rc, BOOL bFullScreen)
{
    INT x = rc.left, y = rc.top;
    INT width = rc.right - rc.left, height = rc.bottom - rc.top;

    BITMAPINFO bmi { };
    bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = height;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 24;

    HDC hScreenDC = CreateDC(TEXT("DISPLAY"), NULL, NULL, NULL);
    HDC hDC = CreateCompatibleDC(hScreenDC);
    HBITMAP hBitmap = CreateDIBSection(hDC, &bmi, DIB_RGB_COLORS, NULL, NULL, NULL);
    if (hBitmap)
    {
        HGDIOBJ hbmOld = SelectObject(hDC, hBitmap);
        StretchBlt(hDC, 0, 0, width, height, hScreenDC, x, y, width, height, SRCCOPY | CAPTUREBLT);

        if (bFullScreen) // フルスクリーンの場合はタスクバーを無視するように灰色で塗りつぶす。
        {
            HWND hTrayWnd = FindWindowEx(NULL, NULL, TEXT("Shell_TrayWnd"), NULL);
            RECT rcTrayWnd;
            GetWindowRect(hTrayWnd, &rcTrayWnd);
            FillRect(hDC, &rcTrayWnd, GetStockBrush(DKGRAY_BRUSH));
        }

        SelectObject(hDC, hbmOld);
    }
    DeleteDC(hDC);
    DeleteDC(hScreenDC);

    return hBitmap;
}

HBITMAP CaptureWindow(HWND hwnd)
{
    RECT rc;
    if (!hwnd) // フルスクリーン。
    {
        INT x = GetSystemMetrics(SM_XVIRTUALSCREEN);
        INT y = GetSystemMetrics(SM_YVIRTUALSCREEN);
        INT width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
        INT height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
        rc = { x, y, x + width, y + height };
    }
    else // ウィンドウのみ。
    {
        GetWindowRect(hwnd, &rc);
    }

    return CaptureScreen(rc, !hwnd);
}

// イメージの変更を検出する。
BOOL BitmapChanged(HBITMAP hbm1, HBITMAP hbm2)
{
    BITMAP bm1, bm2;
    GetObject(hbm1, sizeof(bm1), &bm1);
    GetObject(hbm2, sizeof(bm2), &bm2);

    // 画像の幅か高さが変わっていたら、変更されたとみなす。
    if (bm1.bmWidth != bm2.bmWidth || bm1.bmHeight != bm2.bmHeight)
    {
        return TRUE;
    }

    // ビットマップの各ピクセルを非アックスる。
    LPBYTE pb1 = (LPBYTE)bm1.bmBits;
    LPBYTE pb2 = (LPBYTE)bm2.bmBits;
    INT count = 0;
    for (INT y = 0; y < bm1.bmHeight; ++y)
    {
        LPBYTE line1 = &pb1[y * bm1.bmWidthBytes];
        LPBYTE line2 = &pb2[y * bm2.bmWidthBytes];
        for (INT x = 0; x < bm1.bmWidth; ++x)
        {
            if (line1[x * 3 + 0] != line2[x * 3 + 0] ||
                line1[x * 3 + 1] != line2[x * 3 + 1] ||
                line1[x * 3 + 2] != line2[x * 3 + 2])
            {
                // 変更あり。
                ++count;
            }
        }
    }

    return count >= s_nThreshold;
}

void EnableUpdate(HWND hwnd, BOOL bEnable = TRUE)
{
    HWND hwndUpdate = GetDlgItem(hwnd, psh10);
    EnableWindow(hwndUpdate, bEnable);
}

void OnTimer(HWND hwnd, UINT id)
{
    if (id == 888)
    {
        KillTimer(hwnd, id);
        PostMessage(hwnd, WM_COMMAND, ID_HIDE, 0);
        return;
    }

    if (id != TIMERID_WND_CHECK)
        return;

    KillTimer(hwnd, TIMERID_WND_CHECK);

    HWND hwndFore = GetForegroundWindow();
    HBITMAP hbm = CaptureWindow(s_bFullScreen ? NULL : hwndFore);

    //SaveBitmapToFile(TEXT("a.bmp"), hbm);

    if (!s_bFullScreen && s_hwndFore != hwndFore)
    {
        s_hwndFore = hwndFore;
        DeleteObject(s_hBitmap);
        s_hBitmap = hbm;
        PostMessage(s_hMainWnd, WM_PICO_WND, GetTickCount(), 0);
        SetTimer(hwnd, TIMERID_WND_CHECK, 500, NULL);
        return;
    }

    if (BitmapChanged(hbm, s_hBitmap))
    {
        DeleteObject(s_hBitmap);
        s_hBitmap = hbm;
        PostMessage(s_hMainWnd, WM_PICO_WND, GetTickCount(), 0);
        SetTimer(hwnd, TIMERID_WND_CHECK, 500, NULL);
        return;
    }

    DeleteObject(hbm);

    SetTimer(hwnd, TIMERID_WND_CHECK, 500, NULL);
}

void GetWavFile(HWND hwnd, PICO_TYPE type, INT iItem, LPTSTR pszFileName)
{
    TCHAR szText[MAX_PATH];
    szText[0] = 0;
    SendDlgItemMessage(hwnd, cmb1 + type, CB_GETLBTEXT, iItem, (LPARAM)szText);
    szText[_countof(szText) - 1] = 0;

    lstrcpyn(pszFileName, szText, MAX_PATH);
}

void SetWavFile(HWND hwnd, PICO_TYPE type, LPCTSTR pszFileName)
{
    if (!pszFileName || !*pszFileName)
    {
        s_szWavFile[type][0] = 0;
        return;
    }

    TCHAR szPath[MAX_PATH];
    GetModuleFileName(NULL, szPath, _countof(szPath));
    PathRemoveFileSpec(szPath);
    PathAppend(szPath, pszFileName);

    lstrcpyn(s_szWavFile[type], szPath, _countof(s_szWavFile[type]));
}

BOOL LoadSettingsNoCheck(HWND hwnd)
{
    for (INT i = 0; i < MAX_PICO; ++i)
        s_szWavFile[i][0] = 0;

    s_bKeep = FALSE;
    s_bEnabled = TRUE;
    s_bFullScreen = FALSE;
    s_nThreshold = 32;

    HKEY hKey;
    RegOpenKeyEx(HKEY_CURRENT_USER, TEXT("Software\\Katayama Hirofumi MZ\\PicoPico"),
                 0, KEY_READ, &hKey);
    if (hKey == NULL)
        return FALSE;

    DWORD cbValue, dwValue;
    TCHAR szText[MAX_PATH];

    static const LPCTSTR apsz[] = { TEXT("WavFile0"), TEXT("WavFile1"), TEXT("WavFile2") };
    for (INT i = 0; i < 3; ++i)
    {
        cbValue = sizeof(szText);
        if (RegQueryValueEx(hKey, apsz[i], NULL, NULL, (BYTE*)szText, &cbValue) == 0)
        {
            szText[_countof(szText) - 1] = 0;
            lstrcpyn(s_szWavFile[i], szText, _countof(s_szWavFile[i]));
        }
    }

    cbValue = sizeof(dwValue);
    if (RegQueryValueEx(hKey, TEXT("Keep"), NULL, NULL, (BYTE*)&dwValue, &cbValue) == 0)
    {
        s_bKeep = !!dwValue;
    }

    cbValue = sizeof(dwValue);
    if (RegQueryValueEx(hKey, TEXT("Enabled"), NULL, NULL, (BYTE*)&dwValue, &cbValue) == 0)
    {
        s_bEnabled = !!dwValue;
    }

    cbValue = sizeof(dwValue);
    if (RegQueryValueEx(hKey, TEXT("FullScreen"), NULL, NULL, (BYTE*)&dwValue, &cbValue) == 0)
    {
        s_bFullScreen = !!dwValue;
    }

    cbValue = sizeof(dwValue);
    if (RegQueryValueEx(hKey, TEXT("Threshold"), NULL, NULL, (BYTE*)&dwValue, &cbValue) == 0)
    {
        s_nThreshold = dwValue;
    }

    RegCloseKey(hKey);
    return TRUE;
}

void ValidateSetting(HWND hwnd, PICO_TYPE type)
{
    INT iItem = 0;
    if (PathFileExists(s_szWavFile[type]))
    {
        LPCTSTR filename = PathFindFileName(s_szWavFile[type]);
        iItem = (INT)SendDlgItemMessage(hwnd, cmb1 + type, CB_FINDSTRINGEXACT, -1, (LPARAM)filename);
        if (iItem < 0)
            iItem = 0;
    }

    SendDlgItemMessage(hwnd, cmb1 + type, CB_SETCURSEL, iItem, 0);

    TCHAR szText[MAX_PATH];
    GetWavFile(hwnd, type, iItem, szText);
    SetWavFile(hwnd, type, szText);

    if (!PathFileExists(s_szWavFile[type]))
    {
        s_szWavFile[type][0] = 0;
        iItem = 0;
        SendDlgItemMessage(hwnd, cmb1 + type, CB_SETCURSEL, iItem, 0);
    }
}

void LoadSettings(HWND hwnd)
{
    if (LoadSettingsNoCheck(hwnd))
    {
        ValidateSetting(hwnd, PICO_KEY);
        ValidateSetting(hwnd, PICO_MOUSE);
        ValidateSetting(hwnd, PICO_WND);
    }
}

void SaveSettings(void)
{
    HKEY hKey;
    RegCreateKeyEx(HKEY_CURRENT_USER, TEXT("Software\\Katayama Hirofumi MZ\\PicoPico"),
                   0, NULL, 0, KEY_WRITE, NULL, &hKey, NULL);
    if (hKey == NULL)
        return;

    DWORD cbValue, dwValue;

    static const LPCTSTR apsz[] = { TEXT("WavFile0"), TEXT("WavFile1"), TEXT("WavFile2") };
    for (INT i = 0; i < MAX_PICO; ++i)
    {
        cbValue = (lstrlen(s_szWavFile[i]) + 1) * sizeof(TCHAR);
        RegSetValueEx(hKey, apsz[i], 0, REG_SZ, (BYTE*)s_szWavFile[i], cbValue);
    }

    dwValue = s_bKeep;
    cbValue = sizeof(dwValue);
    RegSetValueEx(hKey, TEXT("Keep"), 0, REG_DWORD, (BYTE*)&dwValue, cbValue);

    dwValue = s_bEnabled;
    cbValue = sizeof(dwValue);
    RegSetValueEx(hKey, TEXT("Enabled"), 0, REG_DWORD, (BYTE*)&dwValue, cbValue);

    dwValue = s_bFullScreen;
    cbValue = sizeof(dwValue);
    RegSetValueEx(hKey, TEXT("FullScreen"), 0, REG_DWORD, (BYTE*)&dwValue, cbValue);

    dwValue = s_nThreshold;
    cbValue = sizeof(dwValue);
    RegSetValueEx(hKey, TEXT("Threshold"), 0, REG_DWORD, (BYTE*)&dwValue, cbValue);

    RegCloseKey(hKey);
}

LRESULT CALLBACK
LowLevelKeyboardProc(
    INT nCode,
    WPARAM wParam,
    LPARAM lParam)
{
    if (nCode < 0 || nCode != HC_ACTION)
        return CallNextHookEx(s_hKeyHook, nCode, wParam, lParam);

    if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN)
    {
        KBDLLHOOKSTRUCT *pData = (KBDLLHOOKSTRUCT*)lParam;
        if (!(pData->flags & (1 << 7)))
        {
            switch (pData->vkCode)
            {
            case VK_LMENU: case VK_RMENU: case VK_MENU:
            case VK_LSHIFT: case VK_RSHIFT: case VK_SHIFT:
            case VK_LCONTROL: case VK_RCONTROL: case VK_CONTROL:
                break;
            default:
                if (InterlockedIncrement(&s_nLock) == 1)
                {
                    PostMessage(s_hMainWnd, WM_PICO_KEY, GetTickCount(), 0);
                    InterlockedDecrement(&s_nLock);
                }
                break;
            }
        }
    }

    return CallNextHookEx(s_hKeyHook, nCode, wParam, lParam);
}

LRESULT CALLBACK
LowLevelMouseProc(
    INT nCode,
    WPARAM wParam,
    LPARAM lParam)
{
    if (nCode < 0 || nCode != HC_ACTION)
        return CallNextHookEx(s_hMouseHook, nCode, wParam, lParam);

    switch (wParam)
    {
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
        {
            MSLLHOOKSTRUCT *pData = (MSLLHOOKSTRUCT*)lParam;
            if (InterlockedIncrement(&s_nLock) == 1)
            {
                PostMessage(s_hMainWnd, WM_PICO_MOUSE, GetTickCount(), 0);
                InterlockedDecrement(&s_nLock);
            }
        }
        break;
    }

    return CallNextHookEx(s_hMouseHook, nCode, wParam, lParam);
}

void PopulateWavFiles(HWND hwnd)
{
    LoadString(s_hInst, IDS_NONE, s_szNone, _countof(s_szNone));

    for (INT i = 0; i < MAX_PICO; ++i)
    {
        SendDlgItemMessage(hwnd, cmb1 + i, CB_RESETCONTENT, 0, 0);
        SendDlgItemMessage(hwnd, cmb1 + i, CB_ADDSTRING, 0, (LPARAM)s_szNone);
    }

    TCHAR szPath[MAX_PATH];
    GetModuleFileName(NULL, szPath, _countof(szPath));
    PathRemoveFileSpec(szPath);
    PathAppend(szPath, TEXT("*.wav"));

    WIN32_FIND_DATA find;
    HANDLE hFind = FindFirstFile(szPath, &find);
    if (hFind != INVALID_HANDLE_VALUE)
    {
        do
        {
            for (INT i = 0; i < MAX_PICO; ++i)
            {
                SendDlgItemMessage(hwnd, cmb1 + i, CB_ADDSTRING, 0, (LPARAM)find.cFileName);
            }
        } while (FindNextFile(hFind, &find));
        FindClose(hFind);
    }

    for (INT i = 0; i < MAX_PICO; ++i)
    {
        SendDlgItemMessage(hwnd, cmb1 + i, CB_SETCURSEL, 0, 0);
    }
}

BOOL AddNotifyIcon(HWND hwnd)
{
    NOTIFYICONDATA NotifyIcon = { NOTIFYICONDATA_V1_SIZE };
    NotifyIcon.hWnd = hwnd;
    NotifyIcon.uID = 1;
    NotifyIcon.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    NotifyIcon.uCallbackMessage = WM_NOTIFY_ICON;
    NotifyIcon.hIcon = (HICON)LoadImage(s_hInst, MAKEINTRESOURCE(IDI_MAIN), IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON),
        0);

    TCHAR szTip[MAX_PATH];
    LoadString(s_hInst, IDS_TIP, szTip, _countof(szTip));
    lstrcpyn(NotifyIcon.szTip, szTip, _countof(NotifyIcon.szTip));

    return Shell_NotifyIcon(NIM_ADD, &NotifyIcon);
}

void RemoveNotifyIcon(HWND hwnd)
{
    NOTIFYICONDATA NotifyIcon = { NOTIFYICONDATA_V1_SIZE };
    NotifyIcon.hWnd = hwnd;
    NotifyIcon.uID = 1;
    Shell_NotifyIcon(NIM_DELETE, &NotifyIcon);
}

BOOL OnInitDialog(HWND hwnd, HWND hwndFocus, LPARAM lParam)
{
    s_hMainWnd = hwnd;

    PopulateWavFiles(hwnd);

    LoadSettings(hwnd);

    if (s_bHideOnInit)
    {
        SetTimer(hwnd, 888, 500, NULL);
        s_bKeep = TRUE;
    }

    HICON hIcon = LoadIcon(s_hInst, MAKEINTRESOURCE(IDI_MAIN));
    SendMessage(hwnd, WM_SETICON, ICON_BIG, (LPARAM)hIcon);

    HICON hSmallIcon = (HICON)LoadImage(s_hInst, MAKEINTRESOURCE(IDI_MAIN), IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON),
        0);
    SendMessage(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)hSmallIcon);

    s_hKeyHook = SetWindowsHookEx(WH_KEYBOARD_LL, LowLevelKeyboardProc, NULL, 0);
    s_hMouseHook = SetWindowsHookEx(WH_MOUSE_LL, LowLevelMouseProc, NULL, 0);

    CheckDlgButton(hwnd, chx1, (s_bKeep ? BST_CHECKED : BST_UNCHECKED));
    if (s_bKeep)
        AddNotifyIcon(hwnd);

    CheckDlgButton(hwnd, chx2, (s_bFullScreen ? BST_CHECKED : BST_UNCHECKED));
    CheckDlgButton(hwnd, chx3, (s_bEnabled ? BST_CHECKED : BST_UNCHECKED));

    EnableWindow(GetDlgItem(hwnd, psh10), FALSE);

    SetTimer(hwnd, TIMERID_WND_CHECK, 500, 0);

    s_uTaskbarRestart = RegisterWindowMessage(TEXT("TaskbarCreated"));

    return TRUE;
}

void OnDestroy(HWND hwnd)
{
    KillTimer(hwnd, TIMERID_WND_CHECK);

    if (s_hKeyHook)
    {
        UnhookWindowsHookEx(s_hKeyHook);
        s_hKeyHook = NULL;
    }

    if (s_hMouseHook)
    {
        UnhookWindowsHookEx(s_hMouseHook);
        s_hMouseHook = NULL;
    }

    if (s_hBitmap)
    {
        DeleteObject(s_hBitmap);
        s_hBitmap = NULL;
    }

    RemoveNotifyIcon(hwnd);

    PostQuitMessage(0);
}

void OnCmbSelect(HWND hwnd, INT id)
{
    PICO_TYPE type = (PICO_TYPE)(id - cmb1);

    INT iItem = (INT)SendDlgItemMessage(hwnd, id, CB_GETCURSEL, 0, 0);
    if (iItem == -1)
    {
        SetWavFile(hwnd, type, NULL);
        return;
    }

    TCHAR szText[MAX_PATH];
    GetWavFile(hwnd, type, iItem, szText);
    if (lstrcmpi(szText, s_szNone) == 0)
        SetWavFile(hwnd, type, NULL);
    else
        SetWavFile(hwnd, type, szText);
}

void OnKeep(HWND hwnd)
{
    s_bKeep = (IsDlgButtonChecked(hwnd, chx1) == BST_CHECKED);
    if (s_bKeep)
    {
        AddNotifyIcon(hwnd);
    }
    else
    {
        RemoveNotifyIcon(hwnd);
    }
}

void OnFullScreen(HWND hwnd)
{
    s_bFullScreen = (IsDlgButtonChecked(hwnd, chx2) == BST_CHECKED);
}

void OnEnableSound(HWND hwnd)
{
    s_bEnabled = (IsDlgButtonChecked(hwnd, chx3) == BST_CHECKED);
}

void DoPlay(HWND hwnd, PICO_TYPE type)
{
    if (s_bEnabled)
        PlaySound(s_szWavFile[type], NULL, SND_ASYNC | SND_FILENAME | SND_NOWAIT);
}

BOOL OnOK(HWND hwnd)
{
    OnCmbSelect(hwnd, cmb1);
    OnCmbSelect(hwnd, cmb2);
    OnCmbSelect(hwnd, cmb3);
    OnKeep(hwnd);
    OnFullScreen(hwnd);
    OnEnableSound(hwnd);
    return TRUE;
}

void OnCommand(HWND hwnd, int id, HWND hwndCtl, UINT codeNotify)
{
    switch (id)
    {
    case cmb1:
        switch (codeNotify)
        {
        case CBN_SELCHANGE:
        case CBN_SELENDOK:
            EnableUpdate(hwnd);
            break;
        }
        break;

    case cmb2:
        switch (codeNotify)
        {
        case CBN_SELCHANGE:
        case CBN_SELENDOK:
            EnableUpdate(hwnd);
            break;
        }
        break;

    case cmb3:
        switch (codeNotify)
        {
        case CBN_SELCHANGE:
        case CBN_SELENDOK:
            EnableUpdate(hwnd);
            break;
        }
        break;

    case chx1:
    case chx2:
    case chx3:
        EnableUpdate(hwnd);
        break;

    case IDOK:
        if (OnOK(hwnd))
        {
            EnableUpdate(hwnd, FALSE);

            if (s_bKeep)
                ShowWindow(hwnd, SW_HIDE);
            else
                EndDialog(hwnd, id);
        }
        break;

    case psh10:
        if (OnOK(hwnd))
            EnableUpdate(hwnd, FALSE);
        break;

    case IDCANCEL:
        if (s_bKeep)
            ShowWindow(hwnd, SW_HIDE);
        else
            EndDialog(hwnd, id);
        break;

    case psh1:
        EndDialog(hwnd, IDCANCEL);
        break;

    case psh2:
    case psh3:
    case psh4:
        {
            BOOL bEnabled = s_bEnabled;
            s_bEnabled = FALSE;
            PICO_TYPE type = (PICO_TYPE)(id - psh2);
            INT iItem = (INT)SendDlgItemMessage(hwnd, cmb1 + type, CB_GETCURSEL, 0, 0);
            TCHAR szText[MAX_PATH];
            GetWavFile(hwnd, type, iItem, szText);
            PlaySound(szText, NULL, SND_ASYNC | SND_FILENAME | SND_NOWAIT | SND_NODEFAULT);
            s_bEnabled = bEnabled;
        }
        break;

    case ID_EXIT:
        EndDialog(hwnd, id);
        break;

    case ID_CONFIG:
        ShowWindow(hwnd, SW_SHOWNORMAL);
        break;

    case ID_ENABLE:
        s_bEnabled = TRUE;
        break;

    case ID_DISABLE:
        s_bEnabled = FALSE;
        break;

    case ID_HIDE:
        ShowWindow(hwnd, SW_HIDE);
        break;
    }
}

HWND FindTrayNotifyWnd(void)
{
    HWND hTrayWnd = FindWindowEx(NULL, NULL, TEXT("Shell_TrayWnd"), NULL);
    return FindWindowEx(hTrayWnd, NULL, TEXT("TrayNotifyWnd"), NULL);
}

void ShowNotifyMenu(HWND hwnd, BOOL bLeftButton)
{
    POINT pt;
    GetCursorPos(&pt);

    HMENU hMenu = LoadMenu(s_hInst, MAKEINTRESOURCE(IDR_MAINMENU));
    HMENU hSubMenu = GetSubMenu(hMenu, 0);

    if (s_bEnabled)
        CheckMenuRadioItem(hSubMenu, ID_ENABLE, ID_DISABLE, ID_ENABLE, MF_BYCOMMAND);
    else
        CheckMenuRadioItem(hSubMenu, ID_ENABLE, ID_DISABLE, ID_DISABLE, MF_BYCOMMAND);

    if (!IsWindowVisible(hwnd))
        EnableMenuItem(hSubMenu, ID_HIDE, MF_GRAYED);

    SetForegroundWindow(hwnd);

    RECT rc;
    HWND hTrayNotifyWnd = FindTrayNotifyWnd();
    if (hTrayNotifyWnd)
    {
        GetWindowRect(hTrayNotifyWnd, &rc);
    }
    else
    {
        SetRectEmpty(&rc);
    }

    TPMPARAMS params = { sizeof(params), rc };

    UINT fuFlags;
    if (bLeftButton)
        fuFlags = TPM_LEFTBUTTON | TPM_RETURNCMD | TPM_VERTICAL;
    else
        fuFlags = TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_VERTICAL;

    INT nCmd = TrackPopupMenuEx(hSubMenu, fuFlags, pt.x, pt.y, hwnd, &params);

    PostMessage(hwnd, WM_NULL, 0, 0);
    PostMessage(hwnd, WM_COMMAND, nCmd, 0);
    DestroyMenu(hMenu);
}

void OnNotifyIcon(HWND hwnd, WPARAM wParam, LPARAM lParam)
{
    switch (lParam)
    {
    case WM_LBUTTONDOWN:
        ShowWindow(hwnd, SW_SHOWNORMAL);
        SwitchToThisWindow(hwnd, TRUE);
        break;
    case WM_RBUTTONDOWN:
        ShowNotifyMenu(hwnd, lParam == WM_LBUTTONDOWN);
        break;
    }
}

void OnSysCommand(HWND hwnd, UINT cmd, int x, int y)
{
    if (cmd == SC_MINIMIZE)
    {
        if (s_bKeep)
            PostMessage(hwnd, WM_COMMAND, IDCANCEL, 0);
    }
}

INT_PTR CALLBACK
DialogProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    switch (uMsg)
    {
        HANDLE_MSG(hwnd, WM_INITDIALOG, OnInitDialog);
        HANDLE_MSG(hwnd, WM_DESTROY, OnDestroy);
        HANDLE_MSG(hwnd, WM_COMMAND, OnCommand);
        HANDLE_MSG(hwnd, WM_TIMER, OnTimer);
        HANDLE_MSG(hwnd, WM_SYSCOMMAND, OnSysCommand);

    case WM_PICO_KEY:
    case WM_PICO_MOUSE:
    case WM_PICO_WND:
        if (GetTickCount() - wParam < 600)
        {
            DoPlay(hwnd, (PICO_TYPE)(uMsg - WM_PICO_KEY));
        }
        break;

    case WM_NOTIFY_ICON:
        OnNotifyIcon(hwnd, wParam, lParam);
        break;

    default:
        if (uMsg == s_uTaskbarRestart)
        {
            if (s_bKeep)
                AddNotifyIcon(hwnd);
        }
        break;
    }
    return 0;
}

INT WINAPI
WinMain(HINSTANCE   hInstance,
        HINSTANCE   hPrevInstance,
        LPSTR       lpCmdLine,
        INT         nCmdShow)
{
    s_bHideOnInit = (lstrcmpi(lpCmdLine, TEXT("/Hide")) == 0);

    TCHAR szText[MAX_PATH];
    LoadString(hInstance, IDS_TITLE, szText, _countof(szText));
    HWND hwnd = FindWindow(TEXT("#32770"), szText);
    if (hwnd)
    {
        ShowWindow(hwnd, (s_bHideOnInit ? SW_HIDE : SW_SHOWNORMAL));
        SwitchToThisWindow(hwnd, TRUE);
        return 0;
    }

    s_hInst = hInstance;
    InitCommonControls();

    DialogBox(hInstance, MAKEINTRESOURCE(IDD_MAIN), NULL, DialogProc);

    SaveSettings();
    return 0;
}
