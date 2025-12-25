#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00 // Windows 10
#endif

#include <windows.h>
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

#ifndef WINHTTP_WEB_SOCKET_SUCCESS_CLOSE
#define WINHTTP_WEB_SOCKET_SUCCESS_CLOSE 1000
#endif

#pragma comment(lib, "winhttp.lib")

HHOOK keyboardHook;
HWND lastForeground = nullptr;

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
std::thread wsThread;

bool IsTargetWindowActive() {
    HWND hwnd = GetForegroundWindow();
    if (!hwnd) return false;

    wchar_t title[256];
    GetWindowTextW(hwnd, title, 256);

    return std::wstring(title).find(TARGET_TITLE) != std::wstring::npos;
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

        if (!msg.empty()) {
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

LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION) {
        if (wParam == WM_KEYDOWN || wParam == WM_KEYUP || wParam == WM_SYSKEYDOWN || wParam == WM_SYSKEYUP) {
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
                std::cout << msg + "\n";
            }
        }
    }

    return CallNextHookEx(NULL, nCode, wParam, lParam);
}

int main() {
    // start websocket worker thread
    wsThread = std::thread(WSWorker);

    keyboardHook = SetWindowsHookEx(WH_KEYBOARD_LL, LowLevelKeyboardProc, NULL, 0);

    if (!keyboardHook) {
        std::cout << "Failed to install hook\n";
        running = false;
        queueCv.notify_all();
        if (wsThread.joinable()) wsThread.join();
        return 1;
    }

    std::cout << "Listening for keys...\n";

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {}

    // signal thread to stop and clean up
    running = false;
    queueCv.notify_all();
    if (wsThread.joinable()) wsThread.join();

    UnhookWindowsHookEx(keyboardHook);
    return 0;
}