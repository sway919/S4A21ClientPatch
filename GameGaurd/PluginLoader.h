#pragma once

#include <windows.h>

// GameGaurd.ini [Plugins] -> LoadLibrary, then ClientPatchPluginInit.
namespace plugin_loader
{
void LoadConfiguredPlugins(HMODULE module);
}
