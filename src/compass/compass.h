#pragma once

#include "plugin_interface.h"

namespace Compass
{
	// Install the UGameViewportClient::Draw AOB hook.
	// Returns false if the pattern scan fails or the HUD interface is unavailable.
	bool Install(IPluginSelf* self);

	// Remove the hook and clean up all state.
	void Remove(IPluginHooks* hooks);

	// Register the StaticLoadObject function pointer found via pattern scan.
	// Must be called before the first draw frame for textures to load without
	// requiring the player to open their map first.
	void SetStaticLoadObject(uintptr_t addr);
}
