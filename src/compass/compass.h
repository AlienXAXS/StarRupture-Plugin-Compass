#pragma once

#include "plugin_interface.h"

namespace Compass
{
	// Resolve every AOB the compass needs. Callable only from the plugin's
	// OnPluginLoadHooks export — the loader refuses scans made anywhere else.
	void ResolvePatterns(IPluginSelf* self, IPluginHookScanner* scanner);

	// Install the UGameViewportClient::Draw hook against the address ResolvePatterns
	// found. Returns false if it never resolved, or the HUD interface is unavailable.
	bool Install(IPluginSelf* self);

	// Remove the hook and clean up all state.
	void Remove(IPluginHooks* hooks);

	// Register the StaticLoadObject function pointer found via pattern scan.
	// Must be called before the first draw frame for textures to load without
	// requiring the player to open their map first.
	void SetStaticLoadObject(uintptr_t addr);
}
