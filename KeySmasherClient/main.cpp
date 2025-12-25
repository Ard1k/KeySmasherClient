#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00 // Windows 10
#endif

#include <windows.h>
#include <shellapi.h>
#include <winhttp.h>
#include <string>
#include <iostream>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <atomic>
#include <memory>
#include <chrono>
#include <unordered_map>

#ifndef WINHTTP_WEB_SOCKET_SUCCESS_CLOSE
#define WINHTTP_WEB_SOCKET_SUCCESS_CLOSE 1000
#endif

#pragma comment(lib, "winhttp.lib")

HHOOK keyboardHook;
HWND lastForeground = nullptr;
HWND g_hWnd = NULL;

const std::wstring TARGET_TITLE = L"Parsec";
const std::wstring WS_HOST = L"192.168.40.70";
const INTERNET_PORT WS_PORT = 80;
const std::wstring WS_PATH = L"/ws";

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

// Title animator
std::thread titleThread;
std::mutex titleMutex;
std::unordered_map<HWND, std::wstring> originalTitles;

// Tray
const UINT TRAY_ICON_ID = 1;
const UINT WM_TRAY_CALLBACK = WM_APP + 1;
const int IDM_SHOW_CONSOLE = 1001;
const int IDM_TOGGLE_PAUSE = 1002;
const int IDM_EXIT = 1003;

bool consoleVisible = false;

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
    if (!conn) return;
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
}

std::unique_ptr<WSConnection> ConnectWebSocket() {
    auto conn = std::make_unique<WSConnection>();

    conn->hSession = WinHttpOpen(L"KeySmasherClient/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS, 0);

    if (!conn->hSession) {
        return nullptr;
    }

    conn->hConnect = WinHttpConnect(conn->hSession, WS_HOST.c_str(), WS_PORT, 0);
    if (!conn->hConnect) {
        CloseWSConnection(conn);
        return nullptr;
    }

    HINTERNET hRequest = WinHttpOpenRequest(
        conn->hConnect, L"GET", WS_PATH.c_str(),
        NULL, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        0);

    if (!hRequest) {
        CloseWSConnection(conn);
        return nullptr;
    }

    BOOL result = WinHttpSetOption(hRequest,
        WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET,
        NULL, 0);

    if (!result) {
        WinHttpCloseHandle(hRequest);
        CloseWSConnection(conn);
        return nullptr;
    }

    result = WinHttpSendRequest(hRequest,
        WINHTTP_NO_ADDITIONAL_HEADERS, 0,
        WINHTTP_NO_REQUEST_DATA, 0,
        0, 0);

    if (!result || !WinHttpReceiveResponse(hRequest, NULL)) {
        WinHttpCloseHandle(hRequest);
        CloseWSConnection(conn);
        return nullptr;
    }

    conn->hWebSocket = WinHttpWebSocketCompleteUpgrade(hRequest, NULL);
    WinHttpCloseHandle(hRequest);

    if (!conn->hWebSocket) {
        CloseWSConnection(conn);
        return nullptr;
    }

    return conn;
}

void WSWorker() {
    std::unique_ptr<WSConnection> conn;

    while (running) {
        if (!conn) {
            conn = ConnectWebSocket();
            if (!conn) {
                // failed to connect, wait and retry
                std::this_thread::sleep_for(std::chrono::seconds(2));
                continue;
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
    nid.hIcon = LoadIcon(NULL, IDI_APPLICATION);
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
    std::wstring showLabel = consoleVisible ? L"Hide Console" : L"Show Console";
    AppendMenu(hMenu, MF_STRING, IDM_SHOW_CONSOLE, showLabel.c_str());
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
            // toggle console on double-click
            if (consoleVisible) {
                ShowWindow(GetConsoleWindow(), SW_HIDE);
                consoleVisible = false;
            } else {
                ShowWindow(GetConsoleWindow(), SW_SHOW);
                consoleVisible = true;
            }
        }
        break;
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDM_SHOW_CONSOLE:
            if (consoleVisible) {
                ShowWindow(GetConsoleWindow(), SW_HIDE);
                consoleVisible = false;
            } else {
                ShowWindow(GetConsoleWindow(), SW_SHOW);
                consoleVisible = true;
            }
            break;
        case IDM_TOGGLE_PAUSE:
            paused = !paused.load();
            break;
        case IDM_EXIT:
            // signal shutdown
            running = false;
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
                char prefix = keyDown ? 'd' : 'u';
                std::string msg = std::string(1, prefix) + std::to_string(kb->vkCode);
                {
                    std::lock_guard<std::mutex> lock(queueMutex);
                    msgQueue.push(msg);
                }
                queueCv.notify_one();
                if (consoleVisible) std::cout << msg + "\n";
            }
        }
    }

    return CallNextHookEx(NULL, nCode, wParam, lParam);
}

int main() {
    // create hidden window to receive tray messages
    HINSTANCE hInstance = GetModuleHandle(NULL);
    WNDCLASSEX wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = TrayWndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = L"KeySmasherTrayClass";
    RegisterClassEx(&wc);

    g_hWnd = CreateWindowEx(0, wc.lpszClassName, L"KeySmasherTrayWindow", 0,
        0, 0, 0, 0, HWND_MESSAGE, NULL, hInstance, NULL);

    if (!g_hWnd) {
        std::cout << "Failed to create tray window\n";
        return 1;
    }

    AddTrayIcon(g_hWnd);

    // hide console by default
    HWND console = GetConsoleWindow();
    if (console) {
        ShowWindow(console, SW_HIDE);
        consoleVisible = false;
    }

    // start websocket worker thread
    wsThread = std::thread(WSWorker);

    keyboardHook = SetWindowsHookEx(WH_KEYBOARD_LL, LowLevelKeyboardProc, NULL, 0);

    if (!keyboardHook) {
        std::cout << "Failed to install hook\n";
        running = false;
        queueCv.notify_all();
        if (wsThread.joinable()) wsThread.join();
        RemoveTrayIcon(g_hWnd);
        DestroyWindow(g_hWnd);
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
    DestroyWindow(g_hWnd);
    UnregisterClass(wc.lpszClassName, hInstance);

    return 0;
}