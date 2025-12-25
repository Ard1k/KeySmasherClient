#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00 // Windows 10
#endif

#include <windows.h>
#include <shellapi.h>
#include <winhttp.h>
#include <string>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <atomic>
#include <memory>
#include <chrono>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "resource.h"

#ifndef WINHTTP_WEB_SOCKET_SUCCESS_CLOSE
#define WINHTTP_WEB_SOCKET_SUCCESS_CLOSE 1000
#endif

#pragma comment(lib, "winhttp.lib")
#pragma comment(linker, "/SUBSYSTEM:WINDOWS")

HHOOK keyboardHook;
HWND lastForeground = nullptr;
HWND g_hWnd = NULL;

HICON g_hIconRunning = NULL;
HICON g_hIconPaused = NULL;
HICON g_hIconConnecting = NULL; // red icon while connecting

// single-instance mutex
HANDLE g_singletonMutex = NULL;

const std::wstring TARGET_TITLE = L"Parsec";

// websocket endpoint (loaded from KeySmasherClient.ini next to the executable)
std::wstring WS_HOST = L"0.0.0.0";
INTERNET_PORT WS_PORT = 80;
const std::wstring WS_PATH = L"/ws";

// Load configuration from KeySmasherClient.ini in the executable directory
// If the INI does not exist, create it with default host 0.0.0.0 and default port 80.
// Returns true if a new INI file was created.
bool LoadConfig() {
    bool created = false;
    wchar_t exePath[MAX_PATH] = {};
    if (GetModuleFileNameW(NULL, exePath, MAX_PATH) == 0) return false;
    std::wstring p(exePath);
    size_t pos = p.find_last_of(L"\\/");
    std::wstring dir = (pos == std::wstring::npos) ? L"." : p.substr(0, pos);
    std::wstring iniPath = dir + L"\\KeySmasherClient.ini";

    // If file doesn't exist, create with defaults
    DWORD attrs = GetFileAttributesW(iniPath.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        // create default values
        WritePrivateProfileStringW(L"Network", L"WebSocketHost", L"0.0.0.0", iniPath.c_str());
        wchar_t portStr[16];
        _itow_s((int)WS_PORT, portStr, 10);
        WritePrivateProfileStringW(L"Network", L"WebSocketPort", portStr, iniPath.c_str());
        created = true;
    }

    // read host
    wchar_t hostBuf[256] = {};
    GetPrivateProfileStringW(L"Network", L"WebSocketHost", WS_HOST.c_str(), hostBuf, _countof(hostBuf), iniPath.c_str());
    if (hostBuf[0] != L'\0') WS_HOST = std::wstring(hostBuf);

    // read port
    int port = GetPrivateProfileIntW(L"Network", L"WebSocketPort", (int)WS_PORT, iniPath.c_str());
    if (port > 0 && port <= 65535) WS_PORT = (INTERNET_PORT)port;

    return created;
}

struct WSConnection {
    HINTERNET hSession = NULL;
    HINTERNET hConnect = NULL;
    HINTERNET hWebSocket = NULL;
};

// Threading / queue for messages
std::mutex queueMutex;
std::condition_variable queueCv;
std::queue<std::string> msgQueue;
std::atomic<bool> running{ true };
std::atomic<bool> paused{ false };
std::thread wsThread;

// connection state
std::atomic<bool> wsConnected{ false };
std::atomic<bool> wsConnecting{ false };
std::atomic<bool> noConnectionAlertShown{ false };

// state for pressed keys tracking
std::mutex stateMutex;
std::unordered_set<int> pressedKeys; // keys for which last sent prefix was 'd'

// Title animator
std::thread titleThread;
std::mutex titleMutex;
std::unordered_map<HWND, std::wstring> originalTitles;

// Tray
const UINT TRAY_ICON_ID = 1;
const UINT WM_TRAY_CALLBACK = WM_APP + 1;
// const int IDM_SHOW_CONSOLE = 1001; // removed - no console
const int IDM_TOGGLE_PAUSE = 1002;
const int IDM_EXIT = 1003;

void UpdateTrayIcon() {
    if (!g_hWnd) return;
    NOTIFYICONDATA nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_hWnd;
    nid.uID = TRAY_ICON_ID;
    nid.uFlags = NIF_ICON;

    // Priority: connecting (red) -> paused -> connected (running) -> default running
    if (wsConnecting.load()) {
        nid.hIcon = g_hIconConnecting ? g_hIconConnecting : LoadIcon(NULL, IDI_ERROR);
    } else if (paused.load()) {
        nid.hIcon = g_hIconPaused ? g_hIconPaused : LoadIcon(NULL, IDI_APPLICATION);
    } else if (wsConnected.load()) {
        nid.hIcon = g_hIconRunning ? g_hIconRunning : LoadIcon(NULL, IDI_APPLICATION);
    } else {
        // not connected and not currently trying -> show error icon
        nid.hIcon = g_hIconConnecting ? g_hIconConnecting : LoadIcon(NULL, IDI_ERROR);
    }

    Shell_NotifyIcon(NIM_MODIFY, &nid);
}

bool IsTargetWindowActive() {
    HWND hwnd = GetForegroundWindow();
    if (!hwnd) return false;

    wchar_t title[256];
    GetWindowTextW(hwnd, title, 256);

    std::wstring s(title);
    // check starts with TARGET_TITLE
    if (s.size() < TARGET_TITLE.size()) return false;
    return s.compare(0, TARGET_TITLE.size(), TARGET_TITLE) == 0;
}

void CloseWSConnection(std::unique_ptr<WSConnection>& conn) {
    // mark disconnected and allow future alerts
    wsConnected = false;
    noConnectionAlertShown = false;

    if (!conn) {
        UpdateTrayIcon();
        return;
    }
    if (conn->hWebSocket) {
        WinHttpWebSocketClose(conn->hWebSocket, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE, NULL, 0);
        WinHttpCloseHandle(conn->hWebSocket);
        conn->hWebSocket = NULL;
    }
    if (conn->hConnect) {
        WinHttpCloseHandle(conn->hConnect);
        conn->hConnect = NULL;
    }
    if (conn->hSession) {
        WinHttpCloseHandle(conn->hSession);
        conn->hSession = NULL;
    }
    conn.reset();
    UpdateTrayIcon();
}

std::unique_ptr<WSConnection> ConnectWebSocket() {
    auto conn = std::make_unique<WSConnection>();

    conn->hSession = WinHttpOpen(L"KeySmasherClient/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS, 0);

    if (!conn->hSession) {
        wsConnected = false;
        return nullptr;
    }

    // set reasonable timeouts so blocking calls return in a timely manner
    // resolveTimeout, connectTimeout, sendTimeout, receiveTimeout in milliseconds
    WinHttpSetTimeouts(conn->hSession, 3000, 3000, 3000, 3000);

    if (!running.load()) { CloseWSConnection(conn); wsConnected = false; return nullptr; }

    conn->hConnect = WinHttpConnect(conn->hSession, WS_HOST.c_str(), WS_PORT, 0);
    if (!conn->hConnect) {
        CloseWSConnection(conn);
        wsConnected = false;
        return nullptr;
    }

    if (!running.load()) { CloseWSConnection(conn); wsConnected = false; return nullptr; }

    HINTERNET hRequest = WinHttpOpenRequest(
        conn->hConnect, L"GET", WS_PATH.c_str(),
        NULL, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        0);

    if (!hRequest) {
        CloseWSConnection(conn);
        wsConnected = false;
        return nullptr;
    }

    if (!running.load()) { WinHttpCloseHandle(hRequest); CloseWSConnection(conn); wsConnected = false; return nullptr; }

    BOOL result = WinHttpSetOption(hRequest,
        WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET,
        NULL, 0);

    if (!result) {
        WinHttpCloseHandle(hRequest);
        CloseWSConnection(conn);
        wsConnected = false;
        return nullptr;
    }

    if (!running.load()) { WinHttpCloseHandle(hRequest); CloseWSConnection(conn); wsConnected = false; return nullptr; }

    result = WinHttpSendRequest(hRequest,
        WINHTTP_NO_ADDITIONAL_HEADERS, 0,
        WINHTTP_NO_REQUEST_DATA, 0,
        0, 0);

    if (!result || !WinHttpReceiveResponse(hRequest, NULL)) {
        WinHttpCloseHandle(hRequest);
        CloseWSConnection(conn);
        wsConnected = false;
        return nullptr;
    }

    if (!running.load()) { WinHttpCloseHandle(hRequest); CloseWSConnection(conn); wsConnected = false; return nullptr; }

    conn->hWebSocket = WinHttpWebSocketCompleteUpgrade(hRequest, NULL);
    WinHttpCloseHandle(hRequest);

    if (!conn->hWebSocket) {
        CloseWSConnection(conn);
        wsConnected = false;
        return nullptr;
    }

    // mark connected
    wsConnected = true;
    noConnectionAlertShown = false;

    UpdateTrayIcon();

    return conn;
}

void ReleaseAllPressedKeys() {
    std::vector<std::string> outs;
    {
        std::lock_guard<std::mutex> lock(stateMutex);
        for (int vk : pressedKeys) {
            std::string msg = std::string(1, 'u') + std::to_string(vk);
            outs.push_back(msg);
        }
        pressedKeys.clear();
    }

    if (!outs.empty()) {
        std::lock_guard<std::mutex> qlock(queueMutex);
        for (auto &m : outs) msgQueue.push(m);
        queueCv.notify_all();
    }
}

void WSWorker() {
    std::unique_ptr<WSConnection> conn;

    while (running) {
        if (!conn) {
            // indicate connecting state and update tray
            wsConnecting = true;
            UpdateTrayIcon();

            conn = ConnectWebSocket();

            // finished attempt
            wsConnecting = false;
            UpdateTrayIcon();

            if (!conn) {
                // failed to connect - always show error and exit
                MessageBoxW(NULL, L"Couldn't establish WebSocket connection. Exiting application...", L"KeySmasherClient", MB_OK | MB_ICONERROR);

                // request application to exit immediately
                if (g_hWnd) {
                    PostMessage(g_hWnd, WM_COMMAND, MAKEWPARAM(IDM_EXIT, 0), 0);
                } else {
                    // fallback: signal quit
                    PostQuitMessage(0);
                }
                break; // break worker loop and exit thread
            }
        }

        std::string msg;
        {
            std::unique_lock<std::mutex> lock(queueMutex);
            if (msgQueue.empty()) {
                // wait until a message arrives or running becomes false
                queueCv.wait_for(lock, std::chrono::seconds(1));
            }
            if (!msgQueue.empty()) {
                msg = std::move(msgQueue.front());
                msgQueue.pop();
            }
        }

        if (!msg.empty() && !paused.load()) {
            if (!conn || !conn->hWebSocket) {
                // no active connection while trying to process a message -> notify user (only once until reconnect)
                if (!noConnectionAlertShown.exchange(true)) {
                    MessageBoxW(NULL, L"Nelze zpracovat zpravu: neni aktivni WebSocket spojeni.", L"KeySmasherClient - Chyba", MB_OK | MB_ICONERROR);
                }
                continue;
            }

            DWORD sendResult = WinHttpWebSocketSend(conn->hWebSocket,
                WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
                (BYTE*)msg.c_str(),
                (DWORD)msg.size());

            if (sendResult != ERROR_SUCCESS) {
                // treat as connection loss and attempt reconnect
                CloseWSConnection(conn);
            }
        }
    }

    // Clean up on exit
    CloseWSConnection(conn);
}

void TitleAnimator() {
    const wchar_t* indicators[] = {L"-", L"\\", L"|", L"/"};
    size_t idx = 0;
    bool prevActive = false;

    while (running) {
        if (paused.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            continue;
        }

        HWND hwnd = GetForegroundWindow();
        bool active = false;
        if (hwnd) {
            wchar_t title[256];
            GetWindowTextW(hwnd, title, 256);
            std::wstring s(title);
            if (s.size() >= TARGET_TITLE.size() && s.compare(0, TARGET_TITLE.size(), TARGET_TITLE) == 0) {
                active = true;
            }
        }

        if (!active && prevActive) {
            // Parsec lost focus -> release pressed keys
            ReleaseAllPressedKeys();
        }
        prevActive = active;

        if (active) {
            // animate
            std::wstring newTitle = TARGET_TITLE + L" [" + indicators[idx % 4] + L"]";
            idx++;

            // store original title once and set new title
            {
                std::lock_guard<std::mutex> lock(titleMutex);
                if (originalTitles.find(hwnd) == originalTitles.end()) {
                    wchar_t orig[256];
                    GetWindowTextW(hwnd, orig, 256);
                    originalTitles[hwnd] = std::wstring(orig);
                }
            }

            SetWindowTextW(hwnd, newTitle.c_str());
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        } else {
            // restore any titles we changed for windows that are no longer active
            {
                std::lock_guard<std::mutex> lock(titleMutex);
                for (auto it = originalTitles.begin(); it != originalTitles.end();) {
                    HWND h = it->first;
                    if (h && IsWindow(h)) {
                        SetWindowTextW(h, it->second.c_str());
                    }
                    it = originalTitles.erase(it);
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
    }

    // restore on exit
    {
        std::lock_guard<std::mutex> lock(titleMutex);
        for (auto &p : originalTitles) {
            if (p.first && IsWindow(p.first)) SetWindowTextW(p.first, p.second.c_str());
        }
        originalTitles.clear();
    }
}

void AddTrayIcon(HWND hwnd) {
    NOTIFYICONDATA nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd;
    nid.uID = TRAY_ICON_ID;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = WM_TRAY_CALLBACK;
    nid.hIcon = g_hIconRunning ? g_hIconRunning : LoadIcon(NULL, IDI_APPLICATION);
    wcscpy_s(nid.szTip, L"KeySmasherClient");
    Shell_NotifyIcon(NIM_ADD, &nid);
}

void RemoveTrayIcon(HWND hwnd) {
    NOTIFYICONDATA nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd;
    nid.uID = TRAY_ICON_ID;
    Shell_NotifyIcon(NIM_DELETE, &nid);
}

void ShowTrayMenu(HWND hwnd) {
    POINT pt;
    GetCursorPos(&pt);

    HMENU hMenu = CreatePopupMenu();
    // no console menu when running as GUI
    std::wstring pauseLabel = paused.load() ? L"Unpause" : L"Pause";
    AppendMenu(hMenu, MF_STRING, IDM_TOGGLE_PAUSE, pauseLabel.c_str());
    AppendMenu(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenu(hMenu, MF_STRING, IDM_EXIT, L"Exit");

    // Use TPM_RETURNCMD so TrackPopupMenu returns the selected command instead of sending messages
    int cmd = TrackPopupMenu(hMenu, TPM_RIGHTBUTTON | TPM_RETURNCMD, pt.x, pt.y, 0, hwnd, NULL);
    DestroyMenu(hMenu);

    if (cmd != 0) {
        // Post WM_COMMAND to be handled in the window procedure
        PostMessage(hwnd, WM_COMMAND, MAKEWPARAM(cmd, 0), 0);
    }
}

LRESULT CALLBACK TrayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_TRAY_CALLBACK:
        if (lParam == WM_RBUTTONUP) {
            ShowTrayMenu(hwnd);
        } else if (lParam == WM_LBUTTONDBLCLK) {
            // toggle pause on double-click
            paused = !paused.load();
            if (paused.load()) {
                ReleaseAllPressedKeys();
            }
            UpdateTrayIcon();
        }
        break;
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDM_TOGGLE_PAUSE:
            paused = !paused.load();
            if (paused.load()) {
                ReleaseAllPressedKeys();
            }
            UpdateTrayIcon();
            break;
        case IDM_EXIT:
            // signal shutdown
            running = false;
            queueCv.notify_all();
            PostQuitMessage(0);
            break;
        }
        break;
    case WM_DESTROY:
        RemoveTrayIcon(hwnd);
        PostQuitMessage(0);
        break;
    default:
        return DefWindowProc(hwnd, msg, wParam, lParam);
    }
    return 0;
}

LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION) {
        if (wParam == WM_KEYDOWN || wParam == WM_KEYUP || wParam == WM_SYSKEYDOWN || wParam == WM_SYSKEYUP) {
            if (paused.load()) return CallNextHookEx(NULL, nCode, wParam, lParam);

            KBDLLHOOKSTRUCT* kb = (KBDLLHOOKSTRUCT*)lParam;

            bool keyDown = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);
            bool keyUp = (wParam == WM_KEYUP || wParam == WM_SYSKEYUP);

            if (IsTargetWindowActive()) {
                // if no websocket connection, show an error (only once until reconnect)
                if (!wsConnected.load()) {
                    if (!noConnectionAlertShown.exchange(true)) {
                        MessageBoxW(NULL, L"Nelze zpracovat zpravu: neni aktivni WebSocket spojeni.", L"KeySmasherClient - Chyba", MB_OK | MB_ICONERROR);
                    }
                    return CallNextHookEx(NULL, nCode, wParam, lParam);
                }

                char prefix = keyDown ? 'd' : 'u';
                std::string msg = std::string(1, prefix) + std::to_string(kb->vkCode);
                {
                    std::lock_guard<std::mutex> lock(queueMutex);
                    msgQueue.push(msg);
                }
                queueCv.notify_one();

                // update pressedKeys state
                {
                    std::lock_guard<std::mutex> slock(stateMutex);
                    if (prefix == 'd') pressedKeys.insert((int)kb->vkCode);
                    else pressedKeys.erase((int)kb->vkCode);
                }
                //std::cout << msg + "\n"; no console available to print
            }
        }
    }

    return CallNextHookEx(NULL, nCode, wParam, lParam);
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    // enforce single instance using a named mutex
    g_singletonMutex = CreateMutexW(NULL, FALSE, L"Global\\KeySmasherClient_Mutex");
    if (!g_singletonMutex) {
        MessageBoxW(NULL, L"Failed to create instance mutex", L"KeySmasherClient", MB_OK | MB_ICONERROR);
        return 1;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(NULL, L"Another instance of KeySmasherClient is already running.", L"KeySmasherClient", MB_OK | MB_ICONINFORMATION);
        CloseHandle(g_singletonMutex);
        g_singletonMutex = NULL;
        return 1;
    }

    bool iniCreated = LoadConfig();
    if (iniCreated) {
        MessageBoxW(NULL, L"A configuration file 'KeySmasherClient.ini' was created next to the executable. Please set 'WebSocketHost' and 'WebSocketPort' in the [Network] section and restart the application.", L"KeySmasherClient - Configuration", MB_OK | MB_ICONINFORMATION);
        if (g_singletonMutex) { CloseHandle(g_singletonMutex); g_singletonMutex = NULL; }
        return 0;
    }

    // create hidden window to receive tray messages
    // HINSTANCE hInstance = GetModuleHandle(NULL); // use provided hInstance
    WNDCLASSEX wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = TrayWndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = L"KeySmasherTrayClass";
    wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    wc.hIconSm = LoadIcon(NULL, IDI_APPLICATION);
    RegisterClassEx(&wc);

    g_hWnd = CreateWindowEx(0, wc.lpszClassName, L"KeySmasherTrayWindow", 0,
        0, 0, 0, 0, HWND_MESSAGE, NULL, hInstance, NULL);

    if (!g_hWnd) {
        MessageBoxW(NULL, L"Failed to create tray window", L"KeySmasherClient", MB_OK | MB_ICONERROR);
        if (g_singletonMutex) { CloseHandle(g_singletonMutex); g_singletonMutex = NULL; }
        return 1;
    }

    // load icons from resources (resources\*.ico expected in project)
    g_hIconRunning = (HICON)LoadImage(hInstance, MAKEINTRESOURCE(IDI_TRAY_RUNNING), IMAGE_ICON, 0, 0, LR_DEFAULTCOLOR);
    g_hIconPaused = (HICON)LoadImage(hInstance, MAKEINTRESOURCE(IDI_TRAY_PAUSED), IMAGE_ICON, 0, 0, LR_DEFAULTCOLOR);
    // use system error icon as red/connecting icon
    g_hIconConnecting = LoadIcon(NULL, IDI_ERROR);

    // set window icons
    if (g_hIconRunning) {
        SendMessage(g_hWnd, WM_SETICON, ICON_BIG, (LPARAM)g_hIconRunning);
        SendMessage(g_hWnd, WM_SETICON, ICON_SMALL, (LPARAM)g_hIconRunning);
    }

    AddTrayIcon(g_hWnd);
    UpdateTrayIcon();

    // start websocket worker thread (will try to connect immediately and set connecting icon)
    wsThread = std::thread(WSWorker);

    keyboardHook = SetWindowsHookEx(WH_KEYBOARD_LL, LowLevelKeyboardProc, NULL, 0);

    if (!keyboardHook) {
        MessageBoxW(NULL, L"Failed to install hook", L"KeySmasherClient", MB_OK | MB_ICONERROR);
        running = false;
        queueCv.notify_all();
        if (wsThread.joinable()) wsThread.join();
        RemoveTrayIcon(g_hWnd);
        DestroyWindow(g_hWnd);
        if (g_singletonMutex) { CloseHandle(g_singletonMutex); g_singletonMutex = NULL; }
        return 1;
    }

    // start title animator
    titleThread = std::thread(TitleAnimator);

    // message loop for tray window and commands
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    // signal thread to stop and clean up
    running = false;
    queueCv.notify_all();
    if (wsThread.joinable()) wsThread.join();
    if (titleThread.joinable()) titleThread.join();

    UnhookWindowsHookEx(keyboardHook);

    RemoveTrayIcon(g_hWnd);
    if (g_hIconRunning) DestroyIcon(g_hIconRunning);
    if (g_hIconPaused) DestroyIcon(g_hIconPaused);
    if (g_hIconConnecting) DestroyIcon(g_hIconConnecting);
    DestroyWindow(g_hWnd);
    UnregisterClass(wc.lpszClassName, hInstance);

    if (g_singletonMutex) { CloseHandle(g_singletonMutex); g_singletonMutex = NULL; }

    return 0;
}