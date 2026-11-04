#pragma once

#include <vector>
#include <string>
#include "Engine_classes.hpp"
#include "Chimera_classes.hpp"
#include "../plugin_helpers.h"
#include "../compass_fields.h"

namespace Layout
{
	struct CustomPinEntry
	{
		SDK::FVector location; // X,Y from FVector2f; Z set to 0
		std::wstring playerName;
		SDK::FLinearColor color;
	};

	// Returns all personal map markers from ACrGameStateBase::PlayerPersonalMarkers.
	// These are custom pins placed by any player -- replicated to all clients via GameState.
	inline std::vector<CustomPinEntry> ScanCustomPins(SDK::UWorld* world)
	{
		std::vector<CustomPinEntry> result;
		if (!world)
			return result;

		auto* gameState = world->GameState
			                  ? static_cast<SDK::ACrGameStateBase*>(world->GameState)
			                  : nullptr;
		if (!gameState)
			return result;

		const auto* pinsArray = CompassFields::PlayerPersonalMarkers.In(gameState);
		if (!pinsArray)
			return result;
		const auto& pins = *pinsArray;
		LOG_TRACE("[ScanCustomPins] gameState=%p pins.Num()=%d", static_cast<void*>(gameState), pins.Num());

		for (int i = 0; i < pins.Num(); i++)
		{
			const auto& item = pins[i];

			const SDK::FVector2f* position = CompassFields::PinPosition.In(&item);
			const SDK::FString* playerName = CompassFields::PinPlayerName.In(&item);
			const SDK::FLinearColor* color = CompassFields::PinColor.In(&item);
			if (!position || !playerName || !color)
				continue;

			CustomPinEntry e;
			e.location = {position->X, position->Y, 0.0f};
			const wchar_t* p = playerName->CStr();
			e.playerName = (p && playerName->Num() > 0)
				               ? (std::wstring(p) + L"'s Marker")
				               : L"'s Marker";
			e.color = *color;

			LOG_TRACE("[ScanCustomPins] [%d] player='%ls' loc=(%.0f,%.0f)",
			          i, e.playerName.c_str(), e.location.X, e.location.Y);

			result.push_back(e);
		}

		return result;
	}
} // namespace Layout
