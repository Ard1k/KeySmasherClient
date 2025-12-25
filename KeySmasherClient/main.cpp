#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00 // Windows 10
#endif

#include <windows.h>
#include <winhttp.h>
#include <string>
#include <iostream>

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

bool IsTargetWindowActive() {
    HWND hwnd = GetForegroundWindow();
    if (!hwnd) return false;

    wchar_t title[256];
    GetWindowTextW(hwnd, title, 256);

    return std::wstring(title).find(TARGET_TITLE) != std::wstring::npos;
}

void SendWebSocketMessage(const std::string& msg) {
    HINTERNET hSession = WinHttpOpen(L"KeySmasherClient/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS, 0);

    if (!hSession) return;

    HINTERNET hConnect = WinHttpConnect(hSession, WS_HOST.c_str(), WS_PORT, 0);
    if (!hConnect) {
        WinHttpCloseHandle(hSession);
        return;
    }

    HINTERNET hRequest = WinHttpOpenRequest(
        hConnect, L"GET", WS_PATH.c_str(),
        NULL, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        0);

    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return;
    }

    BOOL result = WinHttpSetOption(hRequest,
        WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET,
        NULL, 0);

    if (!result) {
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return;
    }

    result = WinHttpSendRequest(hRequest,
        WINHTTP_NO_ADDITIONAL_HEADERS, 0,
        WINHTTP_NO_REQUEST_DATA, 0,
        0, 0);

    if (!result || !WinHttpReceiveResponse(hRequest, NULL)) {
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return;
    }

    HINTERNET hWebSocket = WinHttpWebSocketCompleteUpgrade(hRequest, NULL);
    if (!hWebSocket) {
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return;
    }

    WinHttpCloseHandle(hRequest);

    WinHttpWebSocketSend(hWebSocket,
        WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
        (BYTE*)msg.c_str(),
        (DWORD)msg.size());

    WinHttpWebSocketClose(hWebSocket, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE, NULL, 0);

    WinHttpCloseHandle(hWebSocket);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
}

LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION && wParam == WM_KEYDOWN) {
        KBDLLHOOKSTRUCT* kb = (KBDLLHOOKSTRUCT*)lParam;

        if (IsTargetWindowActive()) {
            std::string msg = std::to_string(kb->vkCode);
            SendWebSocketMessage(msg);
            std::cout << msg + "\n";
        }
    }

    return CallNextHookEx(NULL, nCode, wParam, lParam);
}

int main() {
    keyboardHook = SetWindowsHookEx(WH_KEYBOARD_LL, LowLevelKeyboardProc, NULL, 0);

    if (!keyboardHook) {
        std::cout << "Failed to install hook\n";
        return 1;
    }

    std::cout << "Listening for keys...\n";

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {}

    UnhookWindowsHookEx(keyboardHook);
    return 0;
}