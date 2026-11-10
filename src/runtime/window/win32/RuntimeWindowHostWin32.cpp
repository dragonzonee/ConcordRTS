#include "runtime/window/win32/RuntimeWindowHostWin32.h"
#include "runtime/window/win32/RuntimeWindowScaleWin32.h"
#include "render/hd/RuntimeHdSprites.h"
#include <chrono>
#include <thread>

namespace {
constexpr wchar_t hostProperty[] = L"ConcordRTS.RuntimeWindowHost.W013";
// CreateWindow dispatches synchronously before its return and before session
// attachment. Preserve that null-session path, including native defaults.
thread_local RuntimeWindowHostWin32* creatingHost = nullptr;
}
RuntimeWindowHostWin32::RuntimeWindowHostWin32(WNDPROC dispatch) noexcept : dispatch_(dispatch) {}
RuntimeWindowHostWin32::~RuntimeWindowHostWin32() {
    if (window_ != nullptr && IsWindow(window_)) DestroyWindow(window_);
}
RuntimeWindowCreateResult RuntimeWindowHostWin32::create(int width, int height) {
    HINSTANCE instance = GetModuleHandleW(nullptr);
    const wchar_t* className = L"ConcordRTSRuntimeWindow";
    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = procedure;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = className;
    windowClass.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    if (RegisterClassW(&windowClass) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return RuntimeWindowCreateResult::RegistrationFailed;
    // HD render scale: the real client is S times the logical game size. When
    // that fills (or exceeds) the monitor, use a borderless full-screen window.
    const int scale = concord::render::hd::hdRenderScale();
    const int realWidth = width * scale, realHeight = height * scale;
    MONITORINFO monitor{sizeof(MONITORINFO)};
    GetMonitorInfoW(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), &monitor);
    const int monitorWidth = monitor.rcMonitor.right - monitor.rcMonitor.left;
    const int monitorHeight = monitor.rcMonitor.bottom - monitor.rcMonitor.top;
    const bool fullScreen = scale > 1 && realWidth >= monitorWidth && realHeight >= monitorHeight;
    const DWORD style = fullScreen ? WS_POPUP : WS_OVERLAPPEDWINDOW;
    RECT windowRect{0, 0, realWidth, realHeight};
    if (!fullScreen) AdjustWindowRect(&windowRect, style, FALSE);
    auto* previous = creatingHost;
    creatingHost = this;
    window_ = CreateWindowExW(0, className, L"ConcordRTS", style,
        fullScreen ? monitor.rcMonitor.left : CW_USEDEFAULT, fullScreen ? monitor.rcMonitor.top : CW_USEDEFAULT,
        windowRect.right - windowRect.left, windowRect.bottom - windowRect.top, nullptr, nullptr, instance, nullptr);
    creatingHost = previous;
    if (window_ != nullptr) concord::window::setRuntimeWindowScale(window_, scale);
    return window_ != nullptr ? RuntimeWindowCreateResult::Created
        : RuntimeWindowCreateResult::CreationFailed;
}
void RuntimeWindowHostWin32::attachSession(LONG_PTR session) noexcept {
    SetWindowLongPtrW(window_, GWLP_USERDATA, session);
}
HWND RuntimeWindowHostWin32::window() const noexcept { return window_; }
bool RuntimeWindowHostWin32::drainMessages() {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        if (message.message == WM_QUIT) return false;
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return true;
}
void RuntimeWindowHostWin32::sleepForIdleSlice() {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
}
LRESULT CALLBACK RuntimeWindowHostWin32::procedure(HWND window, UINT message,
    WPARAM wParam, LPARAM lParam) {
    auto* host = reinterpret_cast<RuntimeWindowHostWin32*>(GetPropW(window, hostProperty));
    if (message == WM_NCCREATE && creatingHost != nullptr) {
        host = creatingHost;
        if (!SetPropW(window, hostProperty, host)) return FALSE;
    }
    lParam = concord::window::scaleRuntimeMouseLParam(window, message, lParam);
    const LRESULT result = host != nullptr
        ? host->dispatch_(window, message, wParam, lParam)
        : DefWindowProcW(window, message, wParam, lParam);
    if (message == WM_NCDESTROY && host != nullptr) {
        RemovePropW(window, hostProperty);
        host->window_ = nullptr;
    }
    return result;
}
