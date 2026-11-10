#pragma once
// Routes window-geometry calls in this translation unit through the battle
// window scale (see RuntimeWindowScaleWin32.h). Include after <windows.h> in
// sources that size or hit-test against the game window.
#include "runtime/window/win32/RuntimeWindowScaleWin32.h"

#define GetClientRect concord::window::scaledGetClientRect
#define ScreenToClient concord::window::scaledScreenToClient
#define ClientToScreen concord::window::scaledClientToScreen
#define MapWindowPoints concord::window::scaledMapWindowPoints
#define InvalidateRect concord::window::scaledInvalidateRect
