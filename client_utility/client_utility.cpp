// MIT License
//
// Copyright(c) 2025 Matthieu Bucchianeri
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#define WIN32_LEAN_AND_MEAN // Exclude rarely-used stuff from Windows headers
#include <windows.h>
#include <openvr.h>
#pragma comment(lib, "openvr_api.lib")

#include <chrono>
#include <string>
#include <thread>

#include "../driver_shim/SharedMemory.h"

static inline bool startsWith(const std::string& str, const std::string& substr) {
    return str.find(substr) == 0;
}

static shared::SharedMemory* g_sharedMemory = nullptr;

LRESULT __stdcall CallbackProc(int nCode, WPARAM wParam, LPARAM lParam) {
    PKBDLLHOOKSTRUCT key = (PKBDLLHOOKSTRUCT)lParam;
    if (wParam == WM_KEYDOWN && nCode == HC_ACTION) {
        switch (key->vkCode) {
        case VK_LWIN:
        case VK_RWIN:
            if (!vr::VROverlay()->IsDashboardVisible()) {
                vr::VROverlay()->ShowDashboard("system.desktop.1");
            } else {
                g_sharedMemory->sendClickEvent = 1;
            }
            break;
        }
    }

    return CallNextHookEx(NULL, nCode, wParam, lParam);
}

void BackgroundThread(HHOOK hook) {
    auto lastMessage = std::chrono::steady_clock::now();

    bool exiting = false;
    while (!exiting) {
        // Process vrserver events to detect exit.
        vr::VREvent_t event{};
        while (vr::VRSystem()->PollNextEvent(&event, sizeof(event))) {
            if (event.eventType == vr::VREvent_Quit) {
                exiting = true;
            }
        }

        // 400ms is reactive enough.
        Sleep(400);
    }

    if (hook) {
        // This will break the GetMessage() loop.
        UnhookWindowsHookEx(hook);
    }
}

int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPWSTR lpCmdLine, int nCmdShow) {
    vr::IVRSystem* system = nullptr;
    {
        vr::EVRInitError error;
        system = vr::VR_Init(&error, vr::VRApplication_Background);
    }
    if (!system) {
        return 1;
    }

    HANDLE sharedFileHandle = OpenFileMapping(FILE_MAP_READ | FILE_MAP_WRITE, false, L"KeyboardNav.SharedMemory");
    g_sharedMemory = (shared::SharedMemory*)MapViewOfFile(
        sharedFileHandle, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(*g_sharedMemory));
    if (!g_sharedMemory) {
        return 1;
    }

    HMODULE user32 = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, L"user32.dll", &user32);

    const std::string part1 = "SetWindow";
    const std::string part2 = "sHookExW";
    const std::string full = part1 + part2;
    using pfnSetWindowsHookEx = decltype(&SetWindowsHookEx);
    pfnSetWindowsHookEx SetWindowsHookEx = (pfnSetWindowsHookEx)GetProcAddress(user32, full.c_str());

    HHOOK hook = nullptr;
    if (SetWindowsHookEx) {
        // Hook messages and be responsive.
        hook = SetWindowsHookEx(WH_KEYBOARD_LL, CallbackProc, nullptr, 0);
    }
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

    std::thread backgroundWorker = std::thread([&] { BackgroundThread(hook); });

    // Process hook events to detect Win key.
    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (backgroundWorker.joinable()) {
        backgroundWorker.join();
    }

    if (g_sharedMemory) {
        UnmapViewOfFile(g_sharedMemory);
    }

    if (sharedFileHandle) {
        CloseHandle(sharedFileHandle);
    }

    return 0;
}
