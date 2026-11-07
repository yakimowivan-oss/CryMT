// Borderless fullscreen (CRYMT_BORDERLESS, Window.cpp): when the game asks for fullscreen its swap chain stays
// windowed and the window becomes a borderless popup over its monitor, so Alt+Tab switches at once without a display
// mode change. The swap chain's fullscreen calls report what the game asked for.
#pragma once
#include <windows.h>

bool BorderlessOn();
// The game's fullscreen request for its window; BorderlessApply carries it out.
void BorderlessSetFullscreen(HWND window, bool fullscreen);
bool BorderlessFullscreen();
// Restyles the window when a request is pending. Runs on CryMT's helper thread: the style and position calls send
// messages to the window's thread, which must not be the render thread or a thread main waits for.
void BorderlessApply();
