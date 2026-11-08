// main.cpp - 程序入口（单文件 exe，Windows GUI 子系统）
#include "core.h"

int runGui(HINSTANCE hInst, int nCmdShow);

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR lpCmdLine, int nCmdShow) {
    (void)hPrev;
    (void)lpCmdLine;

    // 高 DPI 感知（动态解析，兼容各版本 Windows）
    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    if (u32) {
        typedef BOOL(WINAPI * PFN_SetCtx)(HANDLE);
        PFN_SetCtx sc = (PFN_SetCtx)GetProcAddress(u32, "SetProcessDpiAwarenessContext");
        if (sc) {
            sc((HANDLE)-4);  // PER_MONITOR_AWARE_V2
        } else {
            typedef BOOL(WINAPI * PFN_SetAware)();
            PFN_SetAware sd = (PFN_SetAware)GetProcAddress(u32, "SetProcessDPIAware");
            if (sd) sd();
        }
    }

    return runGui(hInst, nCmdShow);
}
