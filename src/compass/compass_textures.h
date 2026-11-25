#pragma once

#include "Engine_classes.hpp"
#include "object_ref.h"
#include "plugin_helpers.h"

#include <algorithm>
#include <vector>

// ---------------------------------------------------------------------------
// compass_textures.h -- icon texture cache and asset loading.
//
// Intended to be included only from compass.cpp.
// All symbols are in namespace Compass and have internal linkage (static).
// ---------------------------------------------------------------------------

namespace Compass
{
	// ---------------------------------------------------------------------------
	// POI icon texture cache
	//
	// Each slot is an ObjectRef, read with Get() every time it is used: Get()
	// hands the texture back only while it is still the live object that was
	// loaded. The textures are kept alive with the engine's own strong
	// reference (UObject::AddRef), see RetainTexture below.
	// ---------------------------------------------------------------------------

	struct CompassTextures
	{
		// Entity types
		ObjectRef<SDK::UTexture> player;
		ObjectRef<SDK::UTexture> baseCore;
		ObjectRef<SDK::UTexture> body;
		ObjectRef<SDK::UTexture> drone;
		// POI marker types
		ObjectRef<SDK::UTexture> antena;
		ObjectRef<SDK::UTexture> abandonedBase;
		ObjectRef<SDK::UTexture> cave;
		ObjectRef<SDK::UTexture> obelisk;
		ObjectRef<SDK::UTexture> customPin;
	};

	static CompassTextures s_tex;

	// ---------------------------------------------------------------------------
	// StaticLoadObject -- resolved via pattern scan at plugin init.
	// Signature matches CoreUObject's free function:
	//   UObject* StaticLoadObject(UClass*, UObject* Outer, const wchar_t* Name,
	//                             const wchar_t* Filename, uint32 LoadFlags,
	//                             UPackageMap*, bool bAllowReconciliation,
	//                             const FLinkerInstancingContext*)
	// ---------------------------------------------------------------------------
	using StaticLoadObject_fn = SDK::UObject* (*)(
		SDK::UClass*, // Class  (may be nullptr -- engine will resolve)
		SDK::UObject*, // Outer  (nullptr = global)
		const wchar_t*, // Name   (full asset path)
		const wchar_t*, // Filename (nullptr)
		uint32_t, // LoadFlags (0 = LOAD_None)
		void*, // Sandbox UPackageMap* (nullptr)
		bool, // bAllowObjectReconciliation
		const void* // FLinkerInstancingContext* (nullptr)
	);

	static StaticLoadObject_fn g_StaticLoadObject = nullptr;

	void SetStaticLoadObject(uintptr_t addr)
	{
		g_StaticLoadObject = reinterpret_cast<StaticLoadObject_fn>(addr);
		LOG_INFO("[Compass] StaticLoadObject registered at 0x%llX", static_cast<unsigned long long>(addr));
	}

	// ---------------------------------------------------------------------------
	// Strong references -- UObject::AddRef / UObject::ReleaseRef.
	//
	// The engine's own reference count (the one TStrongObjectPtr uses): the
	// garbage collector treats an object with a count above zero as a root, so a
	// texture holding one is not collected. Both take the object in rcx.
	// Game thread only.
	//
	// No AddRef: the textures are not loaded and the compass draws text markers.
	// No ReleaseRef: the references are kept until the process exits.
	// ---------------------------------------------------------------------------
	using RefFn = void(__fastcall*)(const SDK::UObject*);

	static RefFn g_addRef = nullptr;
	static RefFn g_releaseRef = nullptr;

	// Every object we hold one reference on, once each.
	static std::vector<ObjectRef<SDK::UTexture>> s_retained;

	// The "N/9 loaded" line is written once per world: set again at each world
	// begin, cleared when the line is written.
	static bool s_summaryPending = true;

	void SetStrongRefFunctions(uintptr_t addRef, uintptr_t releaseRef)
	{
		g_addRef = reinterpret_cast<RefFn>(addRef);
		g_releaseRef = reinterpret_cast<RefFn>(releaseRef);

		if (g_addRef)
			LOG_INFO("[Compass] Strong ref: AddRef at 0x%llX", static_cast<unsigned long long>(addRef));
		else
			LOG_WARN("[Compass] AddRef unavailable -- icons off");

		if (g_releaseRef)
			LOG_INFO("[Compass] Strong ref: ReleaseRef at 0x%llX", static_cast<unsigned long long>(releaseRef));
		else
			LOG_WARN("[Compass] ReleaseRef unavailable -- refs held until exit");
	}

	// Takes one strong reference on tex unless one is already held. Returns true
	// when a new reference was taken.
	static bool RetainTexture(SDK::UTexture* tex)
	{
		for (const auto& held : s_retained)
			if (held.Get() == tex)
				return false;

		g_addRef(tex);
		s_retained.emplace_back(tex);
		return true;
	}

	// ---------------------------------------------------------------------------
	// Asset path table.
	// ---------------------------------------------------------------------------
	struct TexEntry
	{
		ObjectRef<SDK::UTexture>& slot;
		const wchar_t* fullPath;
	};

	// ---------------------------------------------------------------------------
	// LoadAllTextures -- loads every texture slot that has no live texture, via
	// StaticLoadObject, and holds a strong reference on each distinct one.
	//
	// Must be called from a game-thread event (OnExperienceLoadComplete,
	// OnAnyWorldBeginPlay, OnTick), NOT from inside a PostRender callback.
	// Calling StaticLoadObject from a render-adjacent context can touch the
	// streaming system while it is in an undefined state and produce corrupted
	// GPU resources (the "random coloured squares" symptom).
	//
	// The references persist across worlds, so a slot that still resolves is left
	// alone; only a slot whose texture is gone is loaded again.
	//
	// Returns true when every slot holds a live texture. logSummary: write the
	// "N/9 loaded" line (once per world).
	// ---------------------------------------------------------------------------
	static bool LoadAllTextures(bool logSummary)
	{
		if (!g_StaticLoadObject)
		{
			LOG_WARN("[Compass] LoadAllTextures: StaticLoadObject not registered -- skipping");
			return false;
		}
		if (!g_addRef)
			return false;

		// Drop retained entries whose texture is gone, before anything is loaded:
		// a new texture can land on the same address and slot, and a stale entry
		// would then match it and suppress its AddRef. Nothing is released here:
		// a dead object has no reference of ours to give back.
		s_retained.erase(std::remove_if(s_retained.begin(), s_retained.end(),
		                                [](const ObjectRef<SDK::UTexture>& r) { return !r.Get(); }),
		                 s_retained.end());

		TexEntry entries[] = {
			{s_tex.player,        L"/Game/Chimera/UI/Map/Markers/T_UI_player_mapIcon.T_UI_player_mapIcon"},
			{s_tex.baseCore,      L"/Game/Chimera/UI/Map/Markers/T_UI_baseCore_mapIcon.T_UI_baseCore_mapIcon"},
			{s_tex.body,          L"/Game/Chimera/UI/Map/Markers/T_UI_deadBody_mapIcon.T_UI_deadBody_mapIcon"},
			{s_tex.drone,         L"/Game/Chimera/UI/Map/Markers/T_UI_drone_mapIcon.T_UI_drone_mapIcon"},
			{s_tex.antena,        L"/Game/Chimera/UI/Map/Markers/T_UI_antenna_mapIcon.T_UI_antenna_mapIcon"},
			{s_tex.abandonedBase, L"/Game/Chimera/UI/Map/Markers/T_UI_abandonedBase_mapIcon.T_UI_abandonedBase_mapIcon"},
			{s_tex.cave,          L"/Game/Chimera/UI/Map/Markers/T_UI_cave_mapIcon.T_UI_cave_mapIcon"},
			{s_tex.obelisk,       L"/Game/Chimera/UI/Map/Markers/T_UI_obelisk_mapIcon.T_UI_obelisk_mapIcon"},
			{s_tex.customPin,     L"/Game/Chimera/UI/Map/Markers/T_UI_marker_mapIcon.T_UI_marker_mapIcon"},
		};

		int present = 0;
		int newRefs = 0;
		for (auto& e : entries)
		{
			if (e.slot.Get())
			{
				++present;
				continue;
			}

			auto* obj = static_cast<SDK::UTexture*>(
				g_StaticLoadObject(nullptr, nullptr, e.fullPath, nullptr, 0, nullptr, true, nullptr));
			if (!obj)
			{
				LOG_WARN("[Compass] LoadAllTextures: StaticLoadObject returned null for %ls", e.fullPath);
				continue;
			}

			if (RetainTexture(obj))
				++newRefs;
			e.slot.Set(obj);
			++present;
		}

		if ((logSummary && s_summaryPending) || newRefs > 0)
		{
			s_summaryPending = false;
			LOG_INFO(
				"[Compass] LoadAllTextures: %d/9 loaded, %d new refs -- player=%p core=%p body=%p drone=%p antenna=%p abandonedBase=%p cave=%p obelisk=%p customPin=%p",
				present, newRefs,
				static_cast<void*>(s_tex.player.Get()), static_cast<void*>(s_tex.baseCore.Get()),
				static_cast<void*>(s_tex.body.Get()), static_cast<void*>(s_tex.drone.Get()),
				static_cast<void*>(s_tex.antena.Get()), static_cast<void*>(s_tex.abandonedBase.Get()),
				static_cast<void*>(s_tex.cave.Get()), static_cast<void*>(s_tex.obelisk.Get()),
				static_cast<void*>(s_tex.customPin.Get()));
		}

		return present == 9;
	}

	// True when a slot has no live texture (not loaded yet, or collected).
	static bool AnyTextureMissing()
	{
		const ObjectRef<SDK::UTexture>* slots[] = {
			&s_tex.player, &s_tex.baseCore, &s_tex.body, &s_tex.drone, &s_tex.antena,
			&s_tex.abandonedBase, &s_tex.cave, &s_tex.obelisk, &s_tex.customPin,
		};
		for (const auto* s : slots)
			if (!s->Get())
				return true;
		return false;
	}

	// ---------------------------------------------------------------------------
	// ReleaseTextures -- gives back every reference taken, at plugin shutdown.
	// Game thread only: off it (process exit) the references are left alone, as
	// they are when the texture-release feature is off.
	// ---------------------------------------------------------------------------
	static void ReleaseTextures(bool onGameThread, bool featureOn)
	{
		if (s_retained.empty())
			return;

		if (!g_releaseRef || !featureOn || !onGameThread)
		{
			LOG_INFO("[Compass] Texture refs not released (%s)",
			         !g_releaseRef ? "ReleaseRef unavailable"
			         : !featureOn  ? "texture-release is off"
			                       : "not on the game thread");
			s_retained.clear();
			s_tex = {};
			return;
		}

		int released = 0;
		for (const auto& held : s_retained)
		{
			if (auto* tex = held.Get())
			{
				g_releaseRef(tex);
				++released;
			}
		}
		LOG_INFO("[Compass] Released %d texture refs", released);
		s_retained.clear();
		s_tex = {};
	}
} // namespace Compass
