#pragma once

// ===========================================================================
// aob_patterns.h -- every game-build-dependent constant the Compass uses.
//
// A game update is the only thing that breaks anything in this file. When one
// lands, this is the ONLY file that should need editing: byte patterns first,
// then the struct offsets below them.
//
// Re-deriving a pattern (IDA, on the shipping exe):
//   1. Locate the function, take the bytes from its first instruction.
//   2. Wildcard every stack displacement, immediate and rel32 call/jmp target
//      (?? for each byte) -- those move between builds even when the code does
//      not.
//   3. Extend the pattern until it matches EXACTLY ONE address. The scanner
//      takes the first (lowest) match and does not warn on ambiguity, so a
//      pattern matching two functions silently resolves to the wrong one.
//      Update 2 broke GatherPlayersData exactly this way: its prologue became
//      byte-identical to UBiomesRuntimeSpawningSubsystem::Initialize, which
//      sits at a lower address and got called instead.
// ===========================================================================

namespace AobPatterns
{
	// -----------------------------------------------------------------------
	// Patterns scanned by this plugin
	//
	// Resolved in Compass::ResolvePatterns during OnPluginLoadHooks, the only
	// event in which the loader permits a plugin to pattern scan.
	// -----------------------------------------------------------------------

	// void UGameViewportClient::Draw(FViewport* Viewport, FCanvas* SceneCanvas)
	//
	// The compass draws from a detour on this rather than from AHUD::PostRender
	// so the overlay survives the HUD being hidden. Optional: a miss leaves the
	// plugin loaded but silent.
	//
	// Verified unique in CL-125218 (Update 2).
	inline constexpr const char* UGameViewportClient_Draw =
		"48 89 5C 24 ?? 55 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 ?? ?? ?? ?? "
		"48 81 EC ?? ?? ?? ?? 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 85 ?? ?? ?? ?? "
		"45 33 FF 48 89 55";

	// void UCrMapManuSubsystem::GatherPlayersData(UCrMapManuSubsystem* this)
	//
	// Native, not a UFUNCTION, so it can only be reached by AOB. Calling it
	// directly refreshes the subsystem's PlayersMarkerDataContainer immediately
	// instead of waiting a gameplay tick.
	//
	// The prologue up to the first call is NOT unique: as of Update 2,
	// UBiomesRuntimeSpawningSubsystem::Initialize compiles to an identical frame
	// setup and call to UAnimationSharingManager::GetWorld, and sits at a LOWER
	// address. The trailing bytes here are the instruction after that call --
	// mov rsi, [rax+1B0h], reading World->GameState -- where Initialize instead
	// has mov rsi, rax. The +1B0h displacement is beyond the end of the pattern,
	// so a UWorld layout change cannot break it.
	//
	// Resolved with ResolveOptionalUnique, which refuses anything but exactly one
	// match, so a future build that reintroduces the ambiguity fails loudly in
	// the loader's report rather than silently calling the wrong function.
	//
	// Verified unique in CL-125218 (Update 2).
	inline constexpr const char* UCrMapManuSubsystem_GatherPlayersData =
		"40 55 53 56 48 8D AC 24 ?? ?? ?? ?? 48 81 EC ?? ?? ?? ?? 48 8B D9 E8 ?? ?? ?? ?? 48 8B B0";

	// void UObject::AddRef(const UObject* this)
	//
	// The engine's strong reference (the count TStrongObjectPtr takes): the
	// garbage collector does not collect an object while it holds one. Same
	// function MiniMap resolves; also a global key in the loader's map, so this
	// pattern is only the fallback for a build the map has no row for.
	// Ends at the function's own return; the prologue alone is shared with
	// ReleaseRef and one more function.
	inline constexpr const char* UObject_AddRef =
		"40 53 48 83 EC 30 8B 59 0C 85 DB 79 ?? 4C 8D 0D ?? ?? ?? ?? 41 B8 0D 04 "
		"00 00 48 8D 15 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 84 C0 74 "
		"?? 90 CC 48 8B 05 ?? ?? ?? ?? 3B D8 0F 8D ?? ?? ?? ?? 48 89 74 24 40 48 "
		"89 7C 24 48 8B FB C1 EF 10 0F B7 F3 3B D8 7D ?? 85 DB 79 ?? 4C 8D 0D ?? "
		"?? ?? ?? 89 5C 24 20 41 B8 2C 03 00 00 48 8D 15 ?? ?? ?? ?? 48 8D 0D ?? "
		"?? ?? ?? E8 ?? ?? ?? ?? 84 C0 74 ?? 90 CC 8B 05 ?? ?? ?? ?? 3B F8 72 ?? "
		"89 44 24 28 4C 8D 0D ?? ?? ?? ?? 41 B8 2D 03 00 00 89 7C 24 20 48 8D 15 "
		"?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 84 C0 74 ?? 90 CC 8B 05 "
		"?? ?? ?? ?? 3B D8 7C ?? 89 44 24 28 4C 8D 0D ?? ?? ?? ?? 41 B8 2E 03 00 "
		"00 89 5C 24 20 48 8D 15 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? "
		"84 C0 74 ?? 90 CC 48 8B 05 ?? ?? ?? ?? 48 8B 1C F8 48 8B 7C 24 48 48 85 "
		"DB 75 ?? 4C 8D 0D ?? ?? ?? ?? 41 B8 30 03 00 00 48 8D 15 ?? ?? ?? ?? 48 "
		"8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 84 C0 74 ?? 90 CC 48 8D 0C 76 48 8B 74 "
		"24 40 48 8D 1C CB 48 85 DB 75 ?? 4C 8D 0D ?? ?? ?? ?? 41 B8 4B 03 00 00 "
		"48 8D 15 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 84 C0 74 ?? 90 "
		"CC EB ?? 33 DB F0 FF 43 14 8B 43 08 0F BA E0 1D 72 ?? BA 00 00 00 20 48 "
		"8B CB 48 83 C4 30 5B E9 ?? ?? ?? ?? 48 83 C4 30 5B C3";

	// void UObject::ReleaseRef(const UObject* this)
	//
	// Gives back what AddRef took. Identical to AddRef up to the last index
	// check, then its own tail (two tail calls into the engine's decrement), so
	// the pattern runs that far to match exactly one address.
	//
	// Verified unique in CL 127004, 128786, 129402 and 130045.
	inline constexpr const char* UObject_ReleaseRef =
		"40 53 48 83 EC 30 8B 59 0C 85 DB 79 ?? 4C 8D 0D ?? ?? ?? ?? 41 B8 0D 04 "
		"00 00 48 8D 15 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 84 C0 74 "
		"?? 90 CC 48 8B 05 ?? ?? ?? ?? 3B D8 0F 8D ?? ?? ?? ?? 48 89 74 24 40 48 "
		"89 7C 24 48 8B FB C1 EF 10 0F B7 F3 3B D8 7D ?? 85 DB 79 ?? 4C 8D 0D ?? "
		"?? ?? ?? 89 5C 24 20 41 B8 2C 03 00 00 48 8D 15 ?? ?? ?? ?? 48 8D 0D ?? "
		"?? ?? ?? E8 ?? ?? ?? ?? 84 C0 74 ?? 90 CC 8B 05 ?? ?? ?? ?? 3B F8 72 ?? "
		"89 44 24 28 4C 8D 0D ?? ?? ?? ?? 41 B8 2D 03 00 00 89 7C 24 20 48 8D 15 "
		"?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 84 C0 74 ?? 90 CC 8B 05 "
		"?? ?? ?? ?? 3B D8 7C ?? 89 44 24 28 4C 8D 0D ?? ?? ?? ?? 41 B8 2E 03 00 "
		"00 89 5C 24 20 48 8D 15 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? "
		"84 C0 74 ?? 90 CC 48 8B 05 ?? ?? ?? ?? 48 8B 1C F8 48 8B 7C 24 48 48 85 "
		"DB 75 ?? 4C 8D 0D ?? ?? ?? ?? 41 B8 30 03 00 00 48 8D 15 ?? ?? ?? ?? 48 "
		"8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 84 C0 74 ?? 90 CC 48 8D 0C 76 48 8B 74 "
		"24 40 48 8D 1C CB 48 85 DB 75 ?? 4C 8D 0D ?? ?? ?? ?? 41 B8 4B 03 00 00 "
		"48 8D 15 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 84 C0 74 ?? 90 "
		"CC 48 8B CB 48 83 C4 30 5B E9 ?? ?? ?? ?? 33 DB 48 8B CB 48 83 C4 30 5B "
		"E9 ?? ?? ?? ??";

	// -----------------------------------------------------------------------
	// Struct offsets
	//
	// Raw byte offsets into game objects, for fields the SDK dump does not
	// expose (native members with no UPROPERTY show up as Pad_xxxx). These
	// shift whenever a class gains or loses a member, and a stale one writes
	// into whatever now occupies that slot -- silent memory corruption, not a
	// crash. Re-check every one of them after a game update.
	// -----------------------------------------------------------------------

	namespace Offsets
	{
		// ACrPlayerControllerBase::bMapMenuOpen (native bool, no UPROPERTY).
		//
		// UCrMapManuSubsystem::UpdateMapMenuData only calls GatherBuildingsData /
		// GatherPlayersData / GatherBaseAttackData when at least one player
		// controller has this set, so the compass sets it to keep the subsystem's
		// marker containers refreshing while the map is closed.
		//
		// To re-derive: decompile UCrMapManuSubsystem::UpdateMapMenuData and read
		// the flag it tests on each PC before incrementing its counter. IDA shows
		// it as PC[98].NamePrivate.ComparisonIndex.Value -- 98 * sizeof(UObject)
		// (40) + 0x18 = 0xF68.
		//
		// CL-125218 (Update 2): 0xF68. Was 0xEE8 pre-Update-2; that slot is now
		// inside OnContextInteractionStart (a TMulticastDelegate at 0xED0).
		inline constexpr unsigned int ACrPlayerControllerBase_bMapMenuOpen = 0xF68;
	}

	// -----------------------------------------------------------------------
	// Addresses supplied by the modloader
	//
	// Not scanned here -- the loader resolves these at startup and hands them
	// over through hooks->*. Listed so that when one of them regresses it is
	// clear the fix belongs in the modloader's scan_patterns.h, not this file.
	//
	//   StaticLoadObject   hooks->Engine->GetStaticLoadObjectAddress()
	//
	// GatherPlayersData used to come from hooks->HUD->GetGatherPlayersDataAddress()
	// and no longer does: the loader's pattern for it went ambiguous in Update 2
	// and resolved to UBiomesRuntimeSpawningSubsystem::Initialize, which Compass
	// then called once per player scan. Scanning it here keeps the pattern next
	// to the code that depends on it and puts a uniqueness check on it.
	// -----------------------------------------------------------------------
}
