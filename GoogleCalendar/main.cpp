#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <shellscalingapi.h>
#include <commctrl.h>
#include <string>
#include <thread>
#include <vector>
#include <mutex>
#include <atomic>
#include <chrono>
#include <sstream>
#include "resource.h"
#include "overlay.h"
#include "GoogleCalendar.h"

using namespace std;
using namespace std::chrono;

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shcore.lib")
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "msimg32.lib")
#pragma comment(lib, "ws2_32.lib")

#define WM_TRAYICON (WM_USER + 1)
#define ID_TRAY_EXIT 1001
#define ID_TRAY_REFRESH 1002
#define ID_TRAY_SETTINGS 1003
#define TIMER_FETCH 2001
#define TIMER_ALERT 2002
#define FETCH_INTERVAL 60000 // download meetings every 60 seconds
#define ALERT_INTERVAL 1000 // check plane launches every second

static HWND g_hwndMain = nullptr;
static NOTIFYICONDATA g_nid = {};
static mutex g_meetingsMutex;
static vector<Meeting> g_meetings;
static vector<string> g_firedAlerts; // keep track of which alerts have already been fired, to avoid duplicates
static bool g_running = true;

static mutex g_authMutex;
string g_clientId = "";
string g_clientSecret = "";
string g_calendarId = "";
string g_refreshToken = "";
string g_accessToken = "";
time_t g_accessTokenExpiry = 0;

static atomic<bool> g_loginInProgress{ false };

LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM); // main window procedure
void ShowSettingsDialog(HWND hwndParent);
void ShowTrayMenu(HWND hwnd);
void InitTrayIcon(HWND hwnd);
void RemoveTrayIcon();
void FetchMeetingsTask();
void CheckAlertsTask();
void LoadSettings();
void SaveAppSettings(const string& clientId, const string& clientSecret, const string& calendarId);
void SaveRefreshToken(const string& refreshToken);

static void LogDebug(const string& msg) {
    char path[MAX_PATH];
    GetTempPathA(MAX_PATH, path);
    string fullPath = string(path) + "GoogleCalendarNotifier_debug.log";
    FILE* f = nullptr;
    fopen_s(&f, fullPath.c_str(), "a");

    if (f) {
        time_t now = time(nullptr);
        char buf[32];
        struct tm tmv;
        localtime_s(&tmv, &now);
        strftime(buf, sizeof(buf), "%H:%M:%S", &tmv);
        fprintf(f, "[%s] %s\n", buf, msg.c_str());
        fclose(f);
    }
}

void LoadSettings() { // load saved settings from registry
    HKEY hKey;
    if (RegOpenKeyExA(HKEY_CURRENT_USER,
        "Software\\GoogleCalendarNotifier", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        char buf[4096];
        DWORD sz;

        sz = sizeof(buf);
        if (RegQueryValueExA(hKey, "ClientId", nullptr, nullptr, (LPBYTE)buf, &sz) == ERROR_SUCCESS)
            g_clientId = string(buf, sz - 1);

        sz = sizeof(buf);
        if (RegQueryValueExA(hKey, "ClientSecret", nullptr, nullptr, (LPBYTE)buf, &sz) == ERROR_SUCCESS)
            g_clientSecret = string(buf, sz - 1);

        sz = sizeof(buf);
        if (RegQueryValueExA(hKey, "CalendarId", nullptr, nullptr, (LPBYTE)buf, &sz) == ERROR_SUCCESS)
            g_calendarId = string(buf, sz - 1);

        sz = sizeof(buf);
        if (RegQueryValueExA(hKey, "RefreshToken", nullptr, nullptr, (LPBYTE)buf, &sz) == ERROR_SUCCESS)
            g_refreshToken = string(buf, sz - 1);

        RegCloseKey(hKey);
    }
}

void SaveAppSettings(const string& clientId, const string& clientSecret, const string& calendarId) {
    HKEY hKey;
    RegCreateKeyExA(HKEY_CURRENT_USER, "Software\\GoogleCalendarNotifier", 0, nullptr, 0, KEY_WRITE, nullptr, &hKey, nullptr);
    RegSetValueExA(hKey, "ClientId", 0, REG_SZ, (LPBYTE)clientId.c_str(), (DWORD)clientId.size() + 1);
    RegSetValueExA(hKey, "ClientSecret", 0, REG_SZ, (LPBYTE)clientSecret.c_str(), (DWORD)clientSecret.size() + 1);
    RegSetValueExA(hKey, "CalendarId", 0, REG_SZ, (LPBYTE)calendarId.c_str(), (DWORD)calendarId.size() + 1);
    RegCloseKey(hKey);

    lock_guard<mutex> lk(g_authMutex);
    g_clientId = clientId;
    g_clientSecret = clientSecret;
    g_calendarId = calendarId;
}

void SaveRefreshToken(const string& refreshToken) {
    HKEY hKey;
    RegCreateKeyExA(HKEY_CURRENT_USER, "Software\\GoogleCalendarNotifier", 0, nullptr, 0, KEY_WRITE, nullptr, &hKey, nullptr);
    RegSetValueExA(hKey, "RefreshToken", 0, REG_SZ, (LPBYTE)refreshToken.c_str(), (DWORD)refreshToken.size() + 1);
    RegCloseKey(hKey);

    lock_guard<mutex> lk(g_authMutex);
    g_refreshToken = refreshToken;
}

#define WM_LOGIN_DONE (WM_USER + 200) // custom message sent to the settings dialog when the login thread finishes

struct LoginThreadCtx {
    HWND hDlg;
    string clientId;
    string clientSecret;
};

static void RunLoginThread(LoginThreadCtx ctx) {
    OAuthTokenResult r = RunOAuthLoginFlow(ctx.clientId, ctx.clientSecret); // starts the OAuth login flow, returns access token and refresh token if successful

    if (r.success) {
        SaveRefreshToken(r.refreshToken);
        {
            lock_guard<mutex> lk(g_authMutex);
            g_accessToken = r.accessToken;
            g_accessTokenExpiry = time(nullptr) + r.expiresInSec;
        }
        LogDebug("Login OAuth successful, refresh_token saved.");
    } else {
        LogDebug("Login OAuth failed: " + r.errorMessage);
    }

    string* pMsg;
    WPARAM status;

    if (r.success) {
        pMsg = new string("");
        status = 1;
    } else {
        pMsg = new string(r.errorMessage);
        status = 0;
    }

    PostMessageA(ctx.hDlg, WM_LOGIN_DONE, status, (LPARAM)pMsg);
}

// ----- visual styling resources for the Settings dialog -----
static HFONT g_hFontTitle = nullptr;
static HFONT g_hFontLabel = nullptr;
static HFONT g_hFontEdit = nullptr;
static HFONT g_hFontButton = nullptr;
static HBRUSH g_hbrPurpleBg = nullptr;
static HBRUSH g_hbrWhite = nullptr;

static void EnsureSettingsUIResources() {
    if (!g_hbrPurpleBg) g_hbrPurpleBg = CreateSolidBrush(RGB(0xC2, 0x8A, 0xFF)); // light purple background
    if (!g_hbrWhite)    g_hbrWhite = CreateSolidBrush(RGB(255, 255, 255));
    
    // Fonturi și mai mari conform cerinței
    if (!g_hFontTitle)  g_hFontTitle  = CreateFontA(34, 0, 0, 0, FW_BOLD,   FALSE, FALSE, FALSE, ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, VARIABLE_PITCH, "Segoe UI");
    if (!g_hFontLabel)  g_hFontLabel  = CreateFontA(26, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, VARIABLE_PITCH, "Segoe UI");
    if (!g_hFontEdit)   g_hFontEdit   = CreateFontA(28, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, VARIABLE_PITCH, "Segoe UI");
    if (!g_hFontButton) g_hFontButton = CreateFontA(26, 0, 0, 0, FW_BOLD,   FALSE, FALSE, FALSE, ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, VARIABLE_PITCH, "Segoe UI");
}

// Paints an owner-drawn button with a soft drop shadow and rounded corners.
static void DrawStyledButton(LPDRAWITEMSTRUCT dis, COLORREF fillColor, COLORREF textColor) {
    HDC hdc = dis->hDC;
    RECT rc = dis->rcItem;
    bool pressed = (dis->itemState & ODS_SELECTED) != 0;
    bool disabled = (dis->itemState & ODS_DISABLED) != 0;

    // Fill the background behind the button with the dialog's purple color 
    // to fix the white corners artifact on rounded rectangles.
    FillRect(hdc, &rc, g_hbrPurpleBg);

    // shadow (offset rounded rect, drawn first/underneath)
    RECT shadowRc = rc;
    OffsetRect(&shadowRc, 3, 4);
    HBRUSH hShadowBrush = CreateSolidBrush(RGB(110, 70, 160));
    HRGN hShadowRgn = CreateRoundRectRgn(shadowRc.left, shadowRc.top, shadowRc.right, shadowRc.bottom, 14, 14);
    FillRgn(hdc, hShadowRgn, hShadowBrush);
    DeleteObject(hShadowRgn);
    DeleteObject(hShadowBrush);

    // button face
    RECT btnRc = rc;
    btnRc.right -= 3;
    btnRc.bottom -= 4;

    COLORREF fc = fillColor;
    if (disabled) {
        fc = RGB(190, 190, 190);
    } else if (pressed) {
        fc = RGB((BYTE)(GetRValue(fc) * 0.82), (BYTE)(GetGValue(fc) * 0.82), (BYTE)(GetBValue(fc) * 0.82));
    }
    COLORREF borderColor = RGB((BYTE)(GetRValue(fc) * 0.7), (BYTE)(GetGValue(fc) * 0.7), (BYTE)(GetBValue(fc) * 0.7));

    HBRUSH hBtnBrush = CreateSolidBrush(fc);
    HPEN hBtnPen = CreatePen(PS_SOLID, 1, borderColor);
    HGDIOBJ oldBrush = SelectObject(hdc, hBtnBrush);
    HGDIOBJ oldPen = SelectObject(hdc, hBtnPen);
    RoundRect(hdc, btnRc.left, btnRc.top, btnRc.right, btnRc.bottom, 14, 14);
    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
    DeleteObject(hBtnBrush);
    DeleteObject(hBtnPen);

    char text[256];
    GetWindowTextA(dis->hwndItem, text, sizeof(text));
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, disabled ? RGB(235, 235, 235) : textColor);
    HFONT hOldFont = (HFONT)SelectObject(hdc, g_hFontButton);
    DrawTextA(hdc, text, -1, &btnRc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(hdc, hOldFont);
}

static void ApplyFontToChildren(HWND hDlg) {
    EnumChildWindows(hDlg, [](HWND hChild, LPARAM) -> BOOL {
        char cls[64];
        GetClassNameA(hChild, cls, sizeof(cls));
        if (_stricmp(cls, "EDIT") == 0) {
            SendMessageA(hChild, WM_SETFONT, (WPARAM)g_hFontEdit, TRUE);
        } else if (_stricmp(cls, "STATIC") == 0) {
            SendMessageA(hChild, WM_SETFONT, (WPARAM)g_hFontLabel, TRUE);
        }
        return TRUE;
    }, 0);
}

void ShowSettingsDialog(HWND hwndParent) {
    EnsureSettingsUIResources();

    static ATOM settingsClassAtom = 0;
    if (!settingsClassAtom) {
        WNDCLASSEXA wcx = { sizeof(WNDCLASSEXA) };
        wcx.lpfnWndProc = [](HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) -> LRESULT {
            if (msg == WM_COMMAND) { // handle button clicks
                if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL || LOWORD(wParam) == ID_BTN_LOGIN) { // OK, Cancel, or Login button clicked
                    PostMessageA(hWnd, WM_USER + 100, wParam, 0);
                    return 0;
                }
            }
            if (msg == WM_LOGIN_DONE) { // custom message sent from the login thread when it finishes
                PostMessageA(hWnd, WM_USER + 100, (WPARAM)(0x10000 | wParam), lParam);
                return 0;
            }
            if (msg == WM_CLOSE) { // close button clicked
                PostMessageA(hWnd, WM_USER + 100, IDCANCEL, 0);
                return 0;
            }
            if (msg == WM_CTLCOLORSTATIC) { // labels blend into the purple background
                HDC hdcStatic = (HDC)wParam;
                SetBkMode(hdcStatic, TRANSPARENT);
                SetTextColor(hdcStatic, RGB(35, 0, 75));
                return (LRESULT)g_hbrPurpleBg;
            }
            if (msg == WM_CTLCOLOREDIT) { // edit boxes stay white for readability
                HDC hdcEdit = (HDC)wParam;
                SetBkMode(hdcEdit, OPAQUE);
                SetBkColor(hdcEdit, RGB(255, 255, 255));
                SetTextColor(hdcEdit, RGB(20, 20, 20));
                return (LRESULT)g_hbrWhite;
            }
            if (msg == WM_DRAWITEM) { // custom-painted buttons (bigger, rounded, with shadow)
                LPDRAWITEMSTRUCT dis = (LPDRAWITEMSTRUCT)lParam;
                if (dis->CtlID == ID_BTN_LOGIN) {
                    DrawStyledButton(dis, RGB(0x6A, 0x1B, 0xB8), RGB(255, 255, 255));
                    return TRUE;
                } else if (dis->CtlID == IDOK) {
                    DrawStyledButton(dis, RGB(0x4B, 0x8B, 0x3B), RGB(255, 255, 255));
                    return TRUE;
                } else if (dis->CtlID == IDCANCEL) {
                    DrawStyledButton(dis, RGB(0x8A, 0x4F, 0xD6), RGB(255, 255, 255));
                    return TRUE;
                }
            }
            return DefWindowProcA(hWnd, msg, wParam, lParam);
        };
        wcx.hInstance = GetModuleHandle(nullptr);
        wcx.lpszClassName = "GoogleCalendarSettingsDlgClass";
        wcx.hbrBackground = g_hbrPurpleBg; // light purple background (#C28AFF)
        wcx.hCursor = LoadCursor(nullptr, IDC_ARROW);
        settingsClassAtom = RegisterClassExA(&wcx);
    }

    if (hwndParent)
        EnableWindow(hwndParent, FALSE);

    HWND hDlg = CreateWindowExA(WS_EX_DLGMODALFRAME | WS_EX_TOPMOST, "GoogleCalendarSettingsDlgClass", "AIRPING - CONNECT TO GOOGLE CALENDAR", WS_VISIBLE | WS_CAPTION | WS_SYSMENU | WS_DLGFRAME, 100, 100, 800, 550, hwndParent, nullptr, GetModuleHandle(nullptr), nullptr);

    if (!hDlg) {
        if (hwndParent)
            EnableWindow(hwndParent, TRUE);
        return;
    }

    string clientId, clientSecret, calendarId;
    bool hasRefreshToken;
    {
        lock_guard<mutex> lk(g_authMutex); // mutex lock to safely read the global variables
        clientId = g_clientId;
        clientSecret = g_clientSecret;
        calendarId = g_calendarId;
        hasRefreshToken = !g_refreshToken.empty();
    }

    CreateWindowExA(0, "STATIC", "CLIENT ID:", WS_CHILD | WS_VISIBLE, 30, 20, 740, 35, hDlg, nullptr, nullptr, nullptr);
    HWND hClientId = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", clientId.c_str(), WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 30, 55, 740, 45, hDlg, (HMENU)ID_EDIT_CLIENT_ID, nullptr, nullptr);
    SendMessageA(hClientId, EM_SETLIMITTEXT, 4000, 0);

    CreateWindowExA(0, "STATIC", "CLIENT SECRET:", WS_CHILD | WS_VISIBLE, 30, 115, 740, 35, hDlg, nullptr, nullptr, nullptr);
    HWND hClientSecret = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", clientSecret.c_str(), WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_PASSWORD, 30, 150, 740, 45, hDlg, (HMENU)ID_EDIT_CLIENT_SECRET, nullptr, nullptr);
    SendMessageA(hClientSecret, EM_SETLIMITTEXT, 4000, 0);

    CreateWindowExA(0, "STATIC", "CALENDAR ID:", WS_CHILD | WS_VISIBLE, 30, 210, 740, 35, hDlg, nullptr, nullptr, nullptr);
    HWND hCalendarId = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", calendarId.c_str(), WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 30, 245, 740, 45, hDlg, (HMENU)ID_EDIT_CALENDAR_ID, nullptr, nullptr);
    SendMessageA(hCalendarId, EM_SETLIMITTEXT, 4000, 0);

    const char* btnText;
    if (hasRefreshToken) {
        btnText = "RE-LOGIN WITH GOOGLE";
    } else {
        btnText = "LOGIN WITH GOOGLE";
    }
    HWND hLoginBtn = CreateWindowExA(0, "BUTTON", btnText, WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 30, 320, 320, 65, hDlg, (HMENU)ID_BTN_LOGIN, nullptr, nullptr);

    string statusText;
    if (hasRefreshToken) {
        statusText = "STATUS: CONNECTED.";
    } else {
        statusText = "STATUS: NOT CONNECTED.\nSAVE CLIENT ID/ SECRET, THEN LOGIN.";
    }
    HWND hStatus = CreateWindowExA(0, "STATIC", statusText.c_str(), WS_CHILD | WS_VISIBLE, 370, 320, 410, 80, hDlg, (HMENU)ID_STATIC_LOGIN_STATUS, nullptr, nullptr);

    HWND hOk = CreateWindowExA(0, "BUTTON", "SAVE", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 510, 420, 120, 50, hDlg, (HMENU)IDOK, nullptr, nullptr);
    HWND hCancel = CreateWindowExA(0, "BUTTON", "CLOSE", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 650, 420, 120, 50, hDlg, (HMENU)IDCANCEL, nullptr, nullptr);

    ApplyFontToChildren(hDlg);
    SendMessageA(hStatus, WM_SETFONT, (WPARAM)g_hFontLabel, TRUE);

    MSG msg;
    bool done = false;
    while (!done && GetMessage(&msg, nullptr, 0, 0)) {
        if (msg.message == WM_USER + 100) {
            WPARAM w = msg.wParam;

            if (w & 0x10000) {
                bool success = (w & 0xFFFF) != 0;
                string* pMsg = reinterpret_cast<string*>(msg.lParam); // the message from the login thread (allocated on heap)
                string errMsg;
                if (pMsg) {
                    errMsg = *pMsg;
                } else {
                    errMsg = "";
                }
                delete pMsg;

                g_loginInProgress = false;
                EnableWindow(hLoginBtn, TRUE);
                SetWindowTextA(hLoginBtn, "RE-LOGIN WITH GOOGLE");

                if (success) {
                    SetWindowTextA(hStatus, "STATUS: CONNECTED SUCCESSFULLY!");
                    MessageBoxA(hDlg, "Authentication successful! The application can now read the calendar.", "OAuth - Success", MB_OK | MB_ICONINFORMATION);
                } else {
                    SetWindowTextA(hStatus, "STATUS: LOGIN FAILED.");
                    string full = "Authentication failed:\n\n" + errMsg;
                    MessageBoxA(hDlg, full.c_str(), "OAuth - Error", MB_OK | MB_ICONERROR);
                }
            }
            else if (LOWORD(w) == IDOK) {
                char buf[4096];
                GetWindowTextA(hClientId, buf, sizeof(buf));
                string cid = buf;
                GetWindowTextA(hClientSecret, buf, sizeof(buf));
                string csec = buf;
                GetWindowTextA(hCalendarId, buf, sizeof(buf));
                string calId = buf;
                SaveAppSettings(cid, csec, calId);
                MessageBoxA(hDlg, "Settings saved.", "Info", MB_OK | MB_ICONINFORMATION);
            }
            else if (LOWORD(w) == IDCANCEL) {
                done = true;
            }
            else if (LOWORD(w) == ID_BTN_LOGIN) {
                if (!g_loginInProgress) {
                    char buf[4096];
                    GetWindowTextA(hClientId, buf, sizeof(buf));
                    string cid = buf;
                    GetWindowTextA(hClientSecret, buf, sizeof(buf));
                    string csec = buf;
                    GetWindowTextA(hCalendarId, buf, sizeof(buf));
                    string calId = buf;

                    if (cid.empty() || csec.empty()) {
                        MessageBoxA(hDlg, "Please fill in Client ID and Client Secret before logging in.", "Missing Information", MB_OK | MB_ICONWARNING);
                    } else {
                        SaveAppSettings(cid, csec, calId);

                        g_loginInProgress = true;
                        EnableWindow(hLoginBtn, FALSE);
                        SetWindowTextA(hLoginBtn, "CONNECTING...");
                        SetWindowTextA(hStatus, "STATUS: BROWSER OPENED, WAITING FOR CONSENT...");

                        LoginThreadCtx ctx;
                        ctx.hDlg = hDlg;
                        ctx.clientId = cid;
                        ctx.clientSecret = csec;
                        thread(RunLoginThread, ctx).detach();
                    }
                }
            }
        }
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    DestroyWindow(hDlg);

    if (hwndParent) {
        EnableWindow(hwndParent, TRUE);
        SetForegroundWindow(hwndParent);
    }
}

void FetchMeetingsTask() { // runs in a separate thread, fetches meetings from Google Calendar every 60 seconds
    string clientId, clientSecret, calendarId, refreshToken;
    {
        lock_guard<mutex> lk(g_authMutex);
        clientId = g_clientId;
        clientSecret = g_clientSecret;
        calendarId = g_calendarId;
        refreshToken = g_refreshToken;
    }

    if (clientId.empty() || clientSecret.empty() || calendarId.empty() || refreshToken.empty()) {
        return;
    }

    string accessToken;
    time_t expiry;
    {
        lock_guard<mutex> lk(g_authMutex);
        accessToken = g_accessToken;
        expiry = g_accessTokenExpiry;
    }

    bool tokenOk = EnsureValidAccessToken(clientId, clientSecret, refreshToken, accessToken, expiry);
    if (!tokenOk) {
        return;
    }

    {
        lock_guard<mutex> lk(g_authMutex);
        g_accessToken = accessToken;
        g_accessTokenExpiry = expiry;
    }

    vector<Meeting> fresh;
    if (!FetchUpcomingMeetings(accessToken, calendarId, fresh)) {
        return;
    }

    {
        lock_guard<mutex> lk(g_meetingsMutex);
        g_meetings = fresh;
    }
}

void CheckAlertsTask() { // runs in a separate thread, checks if any meetings are starting soon and shows overlay banners
    auto now = system_clock::now();
    const int alertOffsets[] = { 0, 5*60, 10*60, 15*60, 20*60, 30*60, 60*60 };

    vector<string> pendingBanners;

    lock_guard<mutex> lk(g_meetingsMutex);
    for (auto& m : g_meetings) {
        auto meetingTime = system_clock::from_time_t(m.startTimestamp);
        auto diff = duration_cast<seconds>(meetingTime - now).count();

        for (int offset : alertOffsets) { // check if we are within the alert window for this offset
            if (diff <= offset && diff > offset - 60) {
                string alertKey = m.uuid + "_" + to_string(offset);
    
                bool alreadyFired = false;
                for (auto& k : g_firedAlerts)
                    if (k == alertKey) { 
                        alreadyFired = true; 
                        break; 
                    }

                if (!alreadyFired) {
                    g_firedAlerts.push_back(alertKey);

                    string label = m.name;
                    if (!label.empty() && (unsigned char)label[0] < 128 && islower((unsigned char)label[0])) // capitalize the first letter of the meeting name for better display
                        label[0] = (char)toupper((unsigned char)label[0]);

                    string bannerText;
                    if (offset == 0) {
                        if (m.isBooking && !m.bookedBy.empty())
                            bannerText = label + " with " + m.bookedBy + " starts now";
                        else
                            bannerText = label + " starts now";
                    } else {
                        int minutesBefore = offset / 60;
                        string timeStr;
                        if (minutesBefore == 60) {
                            timeStr = "in 1 hour";
                        } else {
                            timeStr = " starts in " + to_string(minutesBefore) + " minutes";
                        }

                        if (m.isBooking && !m.bookedBy.empty())
                            bannerText = label + " with " + m.bookedBy + " " + timeStr;
                        else
                            bannerText = label + " " + timeStr;
                    }

                    pendingBanners.push_back(bannerText);
                }
            }
        }
    }

    if (!pendingBanners.empty()) {
        thread([pendingBanners]() {
            ShowMultiplePlaneOverlays(pendingBanners);
        }).detach();
    }
}

void InitTrayIcon(HWND hwnd) {
    g_nid.cbSize = sizeof(NOTIFYICONDATA);
    g_nid.hWnd = hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon = LoadIcon(GetModuleHandle(nullptr), MAKEINTRESOURCE(IDI_APP_ICON));
    strcpy_s(g_nid.szTip, "AirPing");
    Shell_NotifyIcon(NIM_ADD, &g_nid);
}

void RemoveTrayIcon() {
    Shell_NotifyIcon(NIM_DELETE, &g_nid);
}

void ShowTrayMenu(HWND hwnd) {
    HMENU hMenu = CreatePopupMenu();
    AppendMenuA(hMenu, MF_STRING, ID_TRAY_REFRESH, "Refresh meetings");
    AppendMenuA(hMenu, MF_STRING, ID_TRAY_SETTINGS,"Settings...");
    AppendMenuA(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuA(hMenu, MF_STRING, ID_TRAY_EXIT, "Exit");

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd);
    TrackPopupMenu(hMenu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
    DestroyMenu(hMenu);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) { // main window procedure
    
    if (msg == WM_CREATE) { // create the tray icon and start the periodic tasks
        InitTrayIcon(hwnd);
        SetTimer(hwnd, TIMER_FETCH, FETCH_INTERVAL, nullptr); // fetch meetings every 60 seconds
        SetTimer(hwnd, TIMER_ALERT, ALERT_INTERVAL, nullptr); // check for alerts every second
        
        thread([]() { FetchMeetingsTask(); }).detach();
        return 0;
    }
    else if (msg == WM_TIMER) { // handle timer events for fetching meetings and checking alerts
        if (wParam == TIMER_FETCH) { // fetch meetings in a separate thread to avoid blocking the main UI thread
            thread([]() { FetchMeetingsTask(); }).detach();
        } else if (wParam == TIMER_ALERT) { // check alerts in a separate thread to avoid blocking the main UI thread
            CheckAlertsTask(); 
        }
        return 0;
    }
    else if (msg == WM_COMMAND) { // handle menu commands from the tray icon
        WORD commandId = LOWORD(wParam);

        if (commandId == ID_TRAY_EXIT) { // exit the application
            g_running = false;
            RemoveTrayIcon();
            PostQuitMessage(0);
        } 
        else if (commandId == ID_TRAY_REFRESH) { // refresh meetings in a separate thread to avoid blocking the main UI thread
            thread([]() { FetchMeetingsTask(); }).detach();
        } 
        else if (commandId == ID_TRAY_SETTINGS) { // show the settings dialog
            ShowSettingsDialog(hwnd);
        }
        return 0;
    }
    else if (msg == WM_DESTROY) { // clean up and exit the application
        RemoveTrayIcon();
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProc(hwnd, msg, wParam, lParam);
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int) { // entry point of the application
    SetProcessDpiAwareness(PROCESS_PER_MONITOR_DPI_AWARE);

    HANDLE hMutex = CreateMutexA(nullptr, TRUE, "GoogleCalendarNotifier_SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxA(nullptr, "GoogleCalendarNotifier is already running!", "Info", MB_OK | MB_ICONINFORMATION);
        return 0;
    }

    LoadSettings();

    WNDCLASSEXA wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = "GoogleCalendarNotifierHidden";
    wc.hIcon = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_APP_ICON));
    RegisterClassExA(&wc);

    g_hwndMain = CreateWindowExA(0, "GoogleCalendarNotifierHidden", "GoogleCalendarNotifier", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, hInstance, nullptr);

    if (g_refreshToken.empty()) {
        ShowSettingsDialog(g_hwndMain);
    }

    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    ReleaseMutex(hMutex);
    CloseHandle(hMutex);
    return 0;
}

