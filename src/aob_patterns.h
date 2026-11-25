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
