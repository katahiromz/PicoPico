// 「パソコン★ピコピコ化計画」 by 片山博文MZ.
// License: MIT
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <mmsystem.h>
#include <shlwapi.h>
#include "resource.h"
#include "SaveBitmapToFile.h"

// ピコピコ音を鳴らす対象。
enum PICO_TYPE
{
    PICO_KEY = 0,
    PICO_MOUSE = 1,
    PICO_WND = 2,
    MAX_PICO = 3 // 個数。
};

#define WM_PICO_KEY (WM_USER + 100)             // キーボード操作に対するメッセージ。
#define WM_PICO_MOUSE (WM_USER + 101)           // マウス操作に対するメッセージ。
#define WM_PICO_WND (WM_USER + 102)             // 画面表示内容変更に対するメッセージ。
#define WM_NOTIFY_ICON (WM_USER + 200)          // 通知アイコンのメッセージ。

#define TIMERID_WND_CHECK 999

static HINSTANCE s_hInst = NULL;                // アプリのインスタンスハンドル。
static HWND s_hMainWnd = NULL;                  // メインウィンドウのハンドル。
static LONG s_nLock = 0;                        // 同期処理用。
static TCHAR s_szWavFile[MAX_PICO][MAX_PATH];   // WAVファイルへのパス名。
static HHOOK s_hKeyHook = NULL;                 // 低レベル キーボード フック。
static HHOOK s_hMouseHook = NULL;               // 低レベル マウス フック。
static HWND s_hwndFore = NULL;                  // 前面ウィンドウ。
static HBITMAP s_hBitmap = NULL;                // 直前のスクリーンショットのビットマップ ハンドル。
static BOOL s_bKeep = FALSE;                    // タスク トレイに常駐するか？
static BOOL s_bEnabled = TRUE;                  // 有効化されているか？
static BOOL s_bFullScreen = FALSE;              // フルスクリーンをチェックするか？
static TCHAR s_szNone[MAX_PATH];                // 「(なし)」という文字列。
static INT s_nThreshold = 32;                   // ビットマップの変更内容を許容する閾値。
static UINT s_uTaskbarRestart = 0;              // タスクバーが再作成されたら届くメッセージ。
static BOOL s_bHideOnInit = FALSE;              // 初期化時にウィンドウを隠すフラグ。

// スクリーンをキャプチャする関数。
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

// ウィンドウをキャプチャする関数。
HBITMAP CaptureWindow(HWND hwnd OPTIONAL)
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

    // ビットマップの各ピクセルを比較する。。
    LPBYTE pb1 = (LPBYTE)bm1.bmBits, pb2 = (LPBYTE)bm2.bmBits;
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

    return count >= s_nThreshold; // 閾値より大きければ変更されたと見なす。
}

// 「更新」ボタンを有効化・無効化する。
void EnableUpdate(HWND hwnd, BOOL bEnable = TRUE)
{
    HWND hwndUpdate = GetDlgItem(hwnd, psh10);
    EnableWindow(hwndUpdate, bEnable);
}

// タイマー処理。
void OnTimer(HWND hwnd, UINT id)
{
    if (id == 888) // 「/Hide」オプションで隠すときに発火する。。
    {
        KillTimer(hwnd, id);
        PostMessage(hwnd, WM_COMMAND, ID_HIDE, 0);
        return;
    }

    // ウィンドウチェック用のタイマーIDかどうか確認する。
    if (id != TIMERID_WND_CHECK)
        return;

    // いったんタイマーを消す。
    KillTimer(hwnd, TIMERID_WND_CHECK);

    // 前面ウィンドウを取得し、そのイメージをキャプチャする。
    HWND hwndFore = GetForegroundWindow();
    HBITMAP hbm = CaptureWindow(s_bFullScreen ? NULL : hwndFore);

    //SaveBitmapToFile(TEXT("a.bmp"), hbm);

    // フルスクリーンが対象でなければ、前面ウィンドウを比べる。
    if (!s_bFullScreen && s_hwndFore != hwndFore)
    {
        // 前面ウィンドウを保存する。
        s_hwndFore = hwndFore;

        // 古いビットマップを破棄し、新しいビットマップをセットする。
        DeleteObject(s_hBitmap);
        s_hBitmap = hbm;

        // WM_PICO_WNDメッセージを発火する。
        PostMessage(s_hMainWnd, WM_PICO_WND, GetTickCount(), 0);

        // タイマーを再稼働。
        SetTimer(hwnd, TIMERID_WND_CHECK, 500, NULL);
        return;
    }

    // ビットマップ イメージが変更されていれば、WM_PICO_WNDメッセージを発火する。
    if (BitmapChanged(hbm, s_hBitmap))
    {
        // 古いビットマップを破棄し、新しいビットマップをセットする。
        DeleteObject(s_hBitmap);
        s_hBitmap = hbm;

        // WM_PICO_WNDメッセージを発火する。
        PostMessage(s_hMainWnd, WM_PICO_WND, GetTickCount(), 0);

        // タイマーを再稼働。
        SetTimer(hwnd, TIMERID_WND_CHECK, 500, NULL);
        return;
    }

    // キャプチャしたビットマップを破棄する。
    DeleteObject(hbm);

    // タイマーを再稼働。
    SetTimer(hwnd, TIMERID_WND_CHECK, 500, NULL);
}

// コンボボックスからWAVファイル名を取得する。
void GetWavFile(HWND hwnd, PICO_TYPE type, INT iItem, LPTSTR pszFileName)
{
    TCHAR szText[MAX_PATH];
    szText[0] = 0;
    SendDlgItemMessage(hwnd, cmb1 + type, CB_GETLBTEXT, iItem, (LPARAM)szText);
    szText[_countof(szText) - 1] = 0;

    lstrcpyn(pszFileName, szText, MAX_PATH);
}

// WAVファイル名を使ってs_szWavFile[type]にフルパス名を格納する。
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

// アプリの設定を読み込む。
BOOL LoadSettingsNoCheck(HWND hwnd)
{
    // 設定を初期化する。
    s_bKeep = FALSE;
    s_bEnabled = TRUE;
    s_bFullScreen = FALSE;
    s_nThreshold = 32;
    for (INT i = 0; i < MAX_PICO; ++i)
        s_szWavFile[i][0] = 0;

    // レジストリを開く。
    HKEY hKey;
    RegOpenKeyEx(HKEY_CURRENT_USER, TEXT("Software\\Katayama Hirofumi MZ\\PicoPico"),
                 0, KEY_READ, &hKey);
    if (hKey == NULL)
        return FALSE; // レジストリキーが開けなければ失敗。

    DWORD cbValue, dwValue;
    TCHAR szText[MAX_PATH];

    // WAVファイル名。
    static const LPCTSTR apsz[] = { TEXT("WavFile0"), TEXT("WavFile1"), TEXT("WavFile2") };
    for (INT i = 0; i < MAX_PICO; ++i)
    {
        cbValue = sizeof(szText);
        if (RegQueryValueEx(hKey, apsz[i], NULL, NULL, (BYTE*)szText, &cbValue) == 0)
        {
            szText[_countof(szText) - 1] = 0;
            lstrcpyn(s_szWavFile[i], szText, _countof(s_szWavFile[i]));
        }
    }

    // 常駐するか？
    cbValue = sizeof(dwValue);
    if (RegQueryValueEx(hKey, TEXT("Keep"), NULL, NULL, (BYTE*)&dwValue, &cbValue) == 0)
    {
        s_bKeep = !!dwValue;
    }

    // 有効か？
    cbValue = sizeof(dwValue);
    if (RegQueryValueEx(hKey, TEXT("Enabled"), NULL, NULL, (BYTE*)&dwValue, &cbValue) == 0)
    {
        s_bEnabled = !!dwValue;
    }

    // フルスクリーンか？
    cbValue = sizeof(dwValue);
    if (RegQueryValueEx(hKey, TEXT("FullScreen"), NULL, NULL, (BYTE*)&dwValue, &cbValue) == 0)
    {
        s_bFullScreen = !!dwValue;
    }

    // 閾値。
    cbValue = sizeof(dwValue);
    if (RegQueryValueEx(hKey, TEXT("Threshold"), NULL, NULL, (BYTE*)&dwValue, &cbValue) == 0)
    {
        s_nThreshold = dwValue;
    }

    // レジストリ キーを閉じる。
    RegCloseKey(hKey);
    return TRUE; // 成功。
}

// 設定をバリデートする。
void ValidateSetting(HWND hwnd, PICO_TYPE type)
{
    // コンボボックスから項目のインデックスを取得する。無効ならゼロをセット。
    INT iItem = 0;
    if (PathFileExists(s_szWavFile[type]))
    {
        LPCTSTR filename = PathFindFileName(s_szWavFile[type]);
        iItem = (INT)SendDlgItemMessage(hwnd, cmb1 + type, CB_FINDSTRINGEXACT, -1, (LPARAM)filename);
        if (iItem < 0)
            iItem = 0;
    }

    // コンボボックスの選択を変える。
    SendDlgItemMessage(hwnd, cmb1 + type, CB_SETCURSEL, iItem, 0);

    // WAVファイルをセットする。
    TCHAR szText[MAX_PATH];
    GetWavFile(hwnd, type, iItem, szText);
    SetWavFile(hwnd, type, szText);

    if (!PathFileExists(s_szWavFile[type])) // WAVパス名が無効なら
    {
        // 初期化する。
        s_szWavFile[type][0] = 0;
        iItem = 0;
        SendDlgItemMessage(hwnd, cmb1 + type, CB_SETCURSEL, iItem, 0);
    }
}

// アプリ設定を読み込む。
void LoadSettings(HWND hwnd)
{
    if (LoadSettingsNoCheck(hwnd))
    {
        ValidateSetting(hwnd, PICO_KEY);
        ValidateSetting(hwnd, PICO_MOUSE);
        ValidateSetting(hwnd, PICO_WND);
    }
}

// アプリ設定を保存する。
void SaveSettings(void)
{
    // レジストリキーを作成または開く。
    HKEY hKey;
    RegCreateKeyEx(HKEY_CURRENT_USER, TEXT("Software\\Katayama Hirofumi MZ\\PicoPico"),
                   0, NULL, 0, KEY_WRITE, NULL, &hKey, NULL);
    if (hKey == NULL)
        return;

    DWORD cbValue, dwValue;

    // WAVファイル名。
    static const LPCTSTR apsz[] = { TEXT("WavFile0"), TEXT("WavFile1"), TEXT("WavFile2") };
    for (INT i = 0; i < MAX_PICO; ++i)
    {
        cbValue = (lstrlen(s_szWavFile[i]) + 1) * sizeof(TCHAR);
        RegSetValueEx(hKey, apsz[i], 0, REG_SZ, (BYTE*)s_szWavFile[i], cbValue);
    }

    // 常駐するか？
    dwValue = s_bKeep;
    cbValue = sizeof(dwValue);
    RegSetValueEx(hKey, TEXT("Keep"), 0, REG_DWORD, (BYTE*)&dwValue, cbValue);

    // 有効か？
    dwValue = s_bEnabled;
    cbValue = sizeof(dwValue);
    RegSetValueEx(hKey, TEXT("Enabled"), 0, REG_DWORD, (BYTE*)&dwValue, cbValue);

    // フルスクリーンか？
    dwValue = s_bFullScreen;
    cbValue = sizeof(dwValue);
    RegSetValueEx(hKey, TEXT("FullScreen"), 0, REG_DWORD, (BYTE*)&dwValue, cbValue);

    // 閾値。
    dwValue = s_nThreshold;
    cbValue = sizeof(dwValue);
    RegSetValueEx(hKey, TEXT("Threshold"), 0, REG_DWORD, (BYTE*)&dwValue, cbValue);

    // レジストリ キーを閉じる。
    RegCloseKey(hKey);
}

// 低レベル キーボード操作のフック プロシージャ。SetWindowsHookExを参照。
LRESULT CALLBACK
LowLevelKeyboardProc(
    INT nCode,
    WPARAM wParam,
    LPARAM lParam)
{
    if (nCode < 0 || nCode != HC_ACTION)
        return CallNextHookEx(s_hKeyHook, nCode, wParam, lParam);

    if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) // キーが押された。
    {
        KBDLLHOOKSTRUCT *pData = (KBDLLHOOKSTRUCT*)lParam;
        if (!(pData->flags & (1 << 7))) // 確かに押された。
        {
            switch (pData->vkCode)
            {
            case VK_LMENU: case VK_RMENU: case VK_MENU:
            case VK_LSHIFT: case VK_RSHIFT: case VK_SHIFT:
            case VK_LCONTROL: case VK_RCONTROL: case VK_CONTROL:
                // 興味がない仮想キーを無視する。
                break;

            default:
                // マルチスレッドを考慮して処理する。
                if (InterlockedIncrement(&s_nLock) == 1)
                {
                    // WM_PICO_KEYメッセージを投函する。
                    PostMessage(s_hMainWnd, WM_PICO_KEY, GetTickCount(), 0);
                    InterlockedDecrement(&s_nLock);
                }
                break;
            }
        }
    }

    return CallNextHookEx(s_hKeyHook, nCode, wParam, lParam);
}

// 低レベル マウス操作のフック プロシージャ。SetWindowsHookExを参照。
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
            // マルチスレッドを考慮して処理する。
            if (InterlockedIncrement(&s_nLock) == 1)
            {
                // WM_PICO_MOUSEメッセージを投函する。
                PostMessage(s_hMainWnd, WM_PICO_MOUSE, GetTickCount(), 0);
                InterlockedDecrement(&s_nLock);
            }
        }
        break;
    }

    return CallNextHookEx(s_hMouseHook, nCode, wParam, lParam);
}

// WAVファイルのコンボボックスを埋める。
void PopulateWavFiles(HWND hwnd)
{
    // 「(なし)」という文字列を読み込む。
    LoadString(s_hInst, IDS_NONE, s_szNone, _countof(s_szNone));

    // コンボボックスを空にして「(なし)」という文字列を追加。
    for (INT i = 0; i < MAX_PICO; ++i)
    {
        SendDlgItemMessage(hwnd, cmb1 + i, CB_RESETCONTENT, 0, 0);
        SendDlgItemMessage(hwnd, cmb1 + i, CB_ADDSTRING, 0, (LPARAM)s_szNone);
    }

    // 現在のEXEファイルのパス名から読み込むべきWAVファイルの位置を決める。
    TCHAR szPath[MAX_PATH];
    GetModuleFileName(NULL, szPath, _countof(szPath));
    PathRemoveFileSpec(szPath);
    PathAppend(szPath, TEXT("*.wav"));

    // WAVファイルを列挙する。
    WIN32_FIND_DATA find;
    HANDLE hFind = FindFirstFile(szPath, &find);
    if (hFind != INVALID_HANDLE_VALUE)
    {
        do
        {
            // 各コンボボックスに項目を追加していく。
            for (INT i = 0; i < MAX_PICO; ++i)
            {
                SendDlgItemMessage(hwnd, cmb1 + i, CB_ADDSTRING, 0, (LPARAM)find.cFileName);
            }
        } while (FindNextFile(hFind, &find));
        FindClose(hFind);
    }

    // 現在の選択を最初の項目にする。
    for (INT i = 0; i < MAX_PICO; ++i)
    {
        SendDlgItemMessage(hwnd, cmb1 + i, CB_SETCURSEL, 0, 0);
    }
}

// 通知アイコンを追加する。
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

// 通知アイコンを削除する。
void RemoveNotifyIcon(HWND hwnd)
{
    NOTIFYICONDATA NotifyIcon = { NOTIFYICONDATA_V1_SIZE };
    NotifyIcon.hWnd = hwnd;
    NotifyIcon.uID = 1;
    Shell_NotifyIcon(NIM_DELETE, &NotifyIcon);
}

// WM_INITDIALOG
BOOL OnInitDialog(HWND hwnd, HWND hwndFocus, LPARAM lParam)
{
    // ハンドルを保存。
    s_hMainWnd = hwnd;

    // WAVファイルのコンボボックスを埋めていく。
    PopulateWavFiles(hwnd);

    // アプリ設定を読み込む。
    LoadSettings(hwnd);

    // 必要なら隠す。
    if (s_bHideOnInit)
    {
        SetTimer(hwnd, 888, 500, NULL);
        s_bKeep = TRUE;
    }

    // ダイアログにアイコンをセット。
    HICON hIcon = LoadIcon(s_hInst, MAKEINTRESOURCE(IDI_MAIN));
    SendMessage(hwnd, WM_SETICON, ICON_BIG, (LPARAM)hIcon);
    HICON hSmallIcon = (HICON)LoadImage(s_hInst, MAKEINTRESOURCE(IDI_MAIN), IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    SendMessage(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)hSmallIcon);

    // 低レベルなフックを行う。
    s_hKeyHook = SetWindowsHookEx(WH_KEYBOARD_LL, LowLevelKeyboardProc, NULL, 0);
    s_hMouseHook = SetWindowsHookEx(WH_MOUSE_LL, LowLevelMouseProc, NULL, 0);

    // 必要ならチェックボックスをチェックする。
    CheckDlgButton(hwnd, chx1, (s_bKeep ? BST_CHECKED : BST_UNCHECKED));
    CheckDlgButton(hwnd, chx2, (s_bFullScreen ? BST_CHECKED : BST_UNCHECKED));
    CheckDlgButton(hwnd, chx3, (s_bEnabled ? BST_CHECKED : BST_UNCHECKED));

    // 必要なら、通知アイコンを追加する。
    if (s_bKeep)
        AddNotifyIcon(hwnd);

    // 「更新」ボタンを無効化する。
    EnableWindow(GetDlgItem(hwnd, psh10), FALSE);

    // ウィンドウを監視するために、タイマーを導入する。
    SetTimer(hwnd, TIMERID_WND_CHECK, 500, 0);

    // タスクバーが再作成されたときのために、TaskbarCreatedメッセージを取得する。
    s_uTaskbarRestart = RegisterWindowMessage(TEXT("TaskbarCreated"));

    return TRUE; // 自動フォーカス。
}

// WM_DESTROY
void OnDestroy(HWND hwnd)
{
    // タイマーを消す。
    KillTimer(hwnd, TIMERID_WND_CHECK);

    // フックを解除する。
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

    // ビットマップを破棄する。
    if (s_hBitmap)
    {
        DeleteObject(s_hBitmap);
        s_hBitmap = NULL;
    }

    // 通知アイコンを削除する。
    RemoveNotifyIcon(hwnd);

    // 終了メッセージ。
    PostQuitMessage(0);
}

// コンボボックスの更新。
void OnCmbSelect(HWND hwnd, INT id)
{
    // コンボボックスのIDからタイプを取得。
    PICO_TYPE type = (PICO_TYPE)(id - cmb1);

    // 現在の選択を取得。
    INT iItem = (INT)SendDlgItemMessage(hwnd, id, CB_GETCURSEL, 0, 0);
    if (iItem == -1)
    {
        SetWavFile(hwnd, type, NULL);
        return;
    }

    // WAVファイルのパス名を更新。
    TCHAR szText[MAX_PATH];
    GetWavFile(hwnd, type, iItem, szText);
    if (lstrcmpi(szText, s_szNone) == 0)
        SetWavFile(hwnd, type, NULL);
    else
        SetWavFile(hwnd, type, szText);
}

// 常駐フラグを更新し、通知アイコンを追加または削除する。
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

// フルスクリーンのフラグを更新する。
void OnFullScreen(HWND hwnd)
{
    s_bFullScreen = (IsDlgButtonChecked(hwnd, chx2) == BST_CHECKED);
}

// 有効フラグを更新する。
void OnEnableSound(HWND hwnd)
{
    s_bEnabled = (IsDlgButtonChecked(hwnd, chx3) == BST_CHECKED);
}

// 必要ならWAVファイルを使って音声を再生する。
void DoPlay(HWND hwnd, PICO_TYPE type)
{
    if (s_bEnabled)
        PlaySound(s_szWavFile[type], NULL, SND_ASYNC | SND_FILENAME | SND_NOWAIT);
}

// OKボタンか更新ボタンが押された。
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

// WM_COMMAND
void OnCommand(HWND hwnd, int id, HWND hwndCtl, UINT codeNotify)
{
    switch (id)
    {
    case cmb1: // 「キーボードのタイプ音」のコンボボックス。
        switch (codeNotify)
        {
        case CBN_SELCHANGE:
        case CBN_SELENDOK:
            EnableUpdate(hwnd);
            break;
        }
        break;

    case cmb2: // 「マウスのクリック音」のコンボボックス。
        switch (codeNotify)
        {
        case CBN_SELCHANGE:
        case CBN_SELENDOK:
            EnableUpdate(hwnd);
            break;
        }
        break;

    case cmb3: // 「画面表示変更時の音」のコンボボックス。
        switch (codeNotify)
        {
        case CBN_SELCHANGE:
        case CBN_SELENDOK:
            EnableUpdate(hwnd);
            break;
        }
        break;

    case chx1: case chx2: case chx3: // チェックボックス。
        EnableUpdate(hwnd);
        break;

    case IDOK: // 「OK」ボタン。
        if (OnOK(hwnd))
        {
            EnableUpdate(hwnd, FALSE);

            if (s_bKeep)
                ShowWindow(hwnd, SW_HIDE);
            else
                EndDialog(hwnd, id);
        }
        break;

    case psh10: // 「更新」ボタン。
        if (OnOK(hwnd))
            EnableUpdate(hwnd, FALSE);
        break;

    case IDCANCEL: // 「キャンセル」ボタン。
        if (s_bKeep)
            ShowWindow(hwnd, SW_HIDE);
        else
            EndDialog(hwnd, id);
        break;

    case psh1: // 「終了」ボタン。
        EndDialog(hwnd, IDCANCEL);
        break;

    case psh2: case psh3: case psh4: // 再生ボタン。
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

    case ID_EXIT: // 「終了」コマンド。
        EndDialog(hwnd, id);
        break;

    case ID_CONFIG: // 「設定」コマンド。
        ShowWindow(hwnd, SW_SHOWNORMAL);
        break;

    case ID_ENABLE: // 「有効化」コマンド。
        s_bEnabled = TRUE;
        CheckDlgButton(hwnd, chx3, BST_CHECKED);
        break;

    case ID_DISABLE: // 「無効化」コマンド。
        s_bEnabled = FALSE;
        CheckDlgButton(hwnd, chx3, BST_UNCHECKED);
        break;

    case ID_HIDE: // 「隠す」コマンド。
        ShowWindow(hwnd, SW_HIDE);
        break;
    }
}

// 通知領域のウィンドウを取得する。
HWND FindTrayNotifyWnd(void)
{
    HWND hTrayWnd = FindWindowEx(NULL, NULL, TEXT("Shell_TrayWnd"), NULL);
    return FindWindowEx(hTrayWnd, NULL, TEXT("TrayNotifyWnd"), NULL);
}

// 通知アイコンのメニューを表示し、メニューのコマンドを処理する。
void ShowNotifyMenu(HWND hwnd, BOOL bLeftButton)
{
    // カーソル位置を取得する。
    POINT pt;
    GetCursorPos(&pt);

    // メニューをリソースから読み込む。
    HMENU hMenu = LoadMenu(s_hInst, MAKEINTRESOURCE(IDR_MAINMENU));
    HMENU hSubMenu = GetSubMenu(hMenu, 0);

    // メニュー項目を変更する。
    if (s_bEnabled)
        CheckMenuRadioItem(hSubMenu, ID_ENABLE, ID_DISABLE, ID_ENABLE, MF_BYCOMMAND);
    else
        CheckMenuRadioItem(hSubMenu, ID_ENABLE, ID_DISABLE, ID_DISABLE, MF_BYCOMMAND);
    if (!IsWindowVisible(hwnd))
        EnableMenuItem(hSubMenu, ID_HIDE, MF_GRAYED);

    // TrackPopupMenuExに対する準備。
    SetForegroundWindow(hwnd);

    // TrackPopupMenuEx用の除外長方形を取得する。
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

    // TrackPopupMenuEx用のパラメータ。
    TPMPARAMS params = { sizeof(params), rc };

    // TrackPopupMenuEx用のフラグ。
    UINT fuFlags;
    if (bLeftButton)
        fuFlags = TPM_LEFTBUTTON | TPM_RETURNCMD | TPM_VERTICAL;
    else
        fuFlags = TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_VERTICAL;

    // 実際にメニューを表示する。
    INT nCmd = TrackPopupMenuEx(hSubMenu, fuFlags, pt.x, pt.y, hwnd, &params);

    // メニューが閉じられたら、WM_NULLメッセージとコマンドを投函する。
    PostMessage(hwnd, WM_NULL, 0, 0);
    PostMessage(hwnd, WM_COMMAND, nCmd, 0);

    // メニューを破棄する。
    DestroyMenu(hMenu);
}

// 通知アイコンに対する処理を行う。
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

// 「最小化」コマンドに処理を追加する。
void OnSysCommand(HWND hwnd, UINT cmd, int x, int y)
{
    if (cmd == SC_MINIMIZE)
    {
        if (s_bKeep)
            PostMessage(hwnd, WM_COMMAND, IDCANCEL, 0);
    }
}

// ダイアログ プロシージャ。
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

    case WM_PICO_KEY: case WM_PICO_MOUSE: case WM_PICO_WND:
        // メッセージが来たので必要なら音を鳴らす。
        if (GetTickCount() - wParam < 600) // 遅すぎない？
        {
            DoPlay(hwnd, (PICO_TYPE)(uMsg - WM_PICO_KEY));
        }
        break;

    case WM_NOTIFY_ICON: // 通知アイコンのメッセージ。
        OnNotifyIcon(hwnd, wParam, lParam);
        break;

    default: // さもなければ
        if (uMsg == s_uTaskbarRestart) // タスクバーが再作成された。
        {
            // 必要なら通知アイコンを追加する。
            if (s_bKeep)
                AddNotifyIcon(hwnd);
        }
        break;
    }

    return 0;
}

// Win32アプリのメイン関数。
INT WINAPI
WinMain(HINSTANCE   hInstance,
        HINSTANCE   hPrevInstance,
        LPSTR       lpCmdLine,
        INT         nCmdShow)
{
    // 「隠す」オプションを確認する。
    s_bHideOnInit = (lstrcmpi(lpCmdLine, TEXT("/Hide")) == 0);

    // タイトル文字列を読み込む。
    TCHAR szText[MAX_PATH];
    LoadString(hInstance, IDS_TITLE, szText, _countof(szText));

    // すでにメイン ウィンドウがあれば、アクティブにして終了する。
    HWND hwnd = FindWindow(TEXT("#32770"), szText);
    if (hwnd)
    {
        ShowWindow(hwnd, (s_bHideOnInit ? SW_HIDE : SW_SHOWNORMAL));
        SwitchToThisWindow(hwnd, TRUE);
        return 0;
    }

    // アプリのインスタンス ハンドルを保存。
    s_hInst = hInstance;

    // コモン コントロールを初期化。
    InitCommonControls();

    // ダイアログを表示して、閉じるまで待つ。
    DialogBox(hInstance, MAKEINTRESOURCE(IDD_MAIN), NULL, DialogProc);

    // アプリの設定を保存する。
    SaveSettings();

    return 0;
}
