#pragma once
// Window scale for the battle window (HdScale > 1).
//
// The battle window's real client area is S times the game's logical size
// (e.g. 3840x2160 for a 1280x720 game at S=3). The game keeps working in
// logical pixels: client-size and screen<->client queries for that window are
// divided by S here, and mouse messages are divided by S in the window host.
// Frames reach the window through the D3D11 presenter, whose back buffer is
// either logical (stretched by DXGI) or S times logical (the HD map raster).
//
// Include after <windows.h>; RuntimeWindowScaleMacrosWin32.h routes the Win32
// calls in a translation unit through these functions.

#include <windows.h>

namespace concord::window {

struct RuntimeWindowScaleState {
    HWND window = nullptr;
    int scale = 1;
};

inline RuntimeWindowScaleState& runtimeWindowScaleState() {
    static RuntimeWindowScaleState state;
    return state;
}

inline void setRuntimeWindowScale(HWND window, int scale) {
    auto& s = runtimeWindowScaleState();
    s.window = window;
    s.scale = scale < 1 ? 1 : scale;
}

inline int runtimeWindowScaleFor(HWND window) {
    const auto& s = runtimeWindowScaleState();
    return window != nullptr && window == s.window ? s.scale : 1;
}

inline int runtimeWindowScale() { return runtimeWindowScaleState().scale; }

inline BOOL WINAPI scaledGetClientRect(HWND window, LPRECT rect) {
    const BOOL ok = ::GetClientRect(window, rect);
    const int scale = runtimeWindowScaleFor(window);
    if (ok && rect && scale > 1) {
        rect->left /= scale; rect->top /= scale;
        rect->right /= scale; rect->bottom /= scale;
    }
    return ok;
}

inline BOOL WINAPI scaledScreenToClient(HWND window, LPPOINT point) {
    const BOOL ok = ::ScreenToClient(window, point);
    const int scale = runtimeWindowScaleFor(window);
    if (ok && point && scale > 1) {
        // floor division keeps points left/above the client negative
        point->x = point->x >= 0 ? point->x / scale : -((-point->x + scale - 1) / scale);
        point->y = point->y >= 0 ? point->y / scale : -((-point->y + scale - 1) / scale);
    }
    return ok;
}

inline BOOL WINAPI scaledClientToScreen(HWND window, LPPOINT point) {
    const int scale = runtimeWindowScaleFor(window);
    if (point && scale > 1) { point->x *= scale; point->y *= scale; }
    return ::ClientToScreen(window, point);
}

inline int WINAPI scaledMapWindowPoints(HWND from, HWND to, LPPOINT points, UINT count) {
    const int fromScale = runtimeWindowScaleFor(from), toScale = runtimeWindowScaleFor(to);
    if (points && fromScale > 1) for (UINT i = 0; i < count; ++i) { points[i].x *= fromScale; points[i].y *= fromScale; }
    const int result = ::MapWindowPoints(from, to, points, count);
    if (points && toScale > 1) for (UINT i = 0; i < count; ++i) { points[i].x /= toScale; points[i].y /= toScale; }
    return result;
}

inline BOOL WINAPI scaledInvalidateRect(HWND window, const RECT* rect, BOOL erase) {
    const int scale = runtimeWindowScaleFor(window);
    if (rect && scale > 1) {
        const RECT scaled{rect->left * scale, rect->top * scale, rect->right * scale, rect->bottom * scale};
        return ::InvalidateRect(window, &scaled, erase);
    }
    return ::InvalidateRect(window, rect, erase);
}

// Mouse messages carry client coordinates: divide them for the scaled window.
inline LPARAM scaleRuntimeMouseLParam(HWND window, UINT message, LPARAM lParam) {
    const int scale = runtimeWindowScaleFor(window);
    if (scale <= 1) return lParam;
    switch (message) {
    case WM_MOUSEMOVE: case WM_MOUSEHOVER:
    case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
    case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK:
    case WM_XBUTTONDOWN: case WM_XBUTTONUP: case WM_XBUTTONDBLCLK: {
        const int x = static_cast<short>(LOWORD(lParam)), y = static_cast<short>(HIWORD(lParam));
        return MAKELPARAM(static_cast<short>(x / scale), static_cast<short>(y / scale));
    }
    case WM_SIZE:
        return MAKELPARAM(LOWORD(lParam) / scale, HIWORD(lParam) / scale);
    default:
        return lParam;
    }
}

}  // namespace concord::window
