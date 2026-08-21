#pragma once

#include <windows.h>

void GameNativeStartNotice();
void GameNativeStopNotice();
void GameNativeArmGameThreadPump();
BOOL GameNativeChatReady();
BOOL GameNativePostChatNotice(LPCWSTR text, INT color);
BOOL GameNativePostLoadNotice(LPCWSTR text, INT color);
INT GameNativeChatRgb(INT red, INT green, INT blue);
