#pragma once

#include <windows.h>

void GameNativeStartClipSupport();
void GameNativeStopClipSupport();
void GameNativeRefreshPluginClips();
void GameNativeNotePluginWindow(void* window, int windowId);
void GameNativeNoteTileOffset(int x, int y);
void GameNativeExtractClippedLayout(const char* text);
bool GameNativeExtractTileOffset(const char* text, int* x, int* y);
