#include "compass.h"
#include "compass_textures.h"
#include "plugin_helpers.h"
#include "plugin_config.h"
#include "../aob_patterns.h"
#include "../compass_fields.h"
#include "Engine_classes.hpp"
#include "ChimeraUI_classes.hpp"
#include "../layout/layout.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace Compass
{
	// ---------------------------------------------------------------------------
	// Hook state
	// ---------------------------------------------------------------------------

	static IPluginHooks* g_hooks = nullptr;
	static IPluginSelf*  g_self  = nullptr;

	// ---------------------------------------------------------------------------
	// Viewport draw hook (optional AOB path)
	// ---------------------------------------------------------------------------

	using ViewportDraw_t = void(__fastcall*)(void*, void*, void*);
	static ViewportDraw_t g_originalViewportDraw = nullptr;
	static HookHandle     g_viewportDrawHook     = nullptr;

	// Filled in by ResolvePatterns during OnPluginLoadHooks; 0 means the pattern
	// missed on this build and the compass simply will not draw.
	static uintptr_t      g_viewportDrawAddr     = 0;

	// ---------------------------------------------------------------------------
	// Cardinal direction table
	// ---------------------------------------------------------------------------

	struct Cardinal
	{
		const wchar_t* label;
		float worldYaw;
	};

	static constexpr Cardinal CARDINALS[] = {
		{L"N", 0.0f}, {L"NE", 45.0f}, {L"E", 90.0f},
		{L"SE", 135.0f}, {L"S", 180.0f}, {L"SW", 225.0f},
		{L"W", 270.0f}, {L"NW", 315.0f},
	};

	static SDK::UTexture* GetPoiTexture(Layout::PoiType type)
	{
		switch (type)
		{
		case Layout::PoiType::Antena: return s_tex.antena;
		case Layout::PoiType::AbandonedBase: return s_tex.abandonedBase;
		case Layout::PoiType::Cave: return s_tex.cave;
		case Layout::PoiType::Obelisk: return s_tex.obelisk;
		default: return nullptr;
		}
	}

	// ---------------------------------------------------------------------------
	// Cached draw config -- refreshed on OnConfigChanged to avoid per-frame INI reads.
	// IsEnabled() is the only value kept as a direct read (it's checked once per
	// frame in the hook and benefits from immediate response to disable).
	// ---------------------------------------------------------------------------

	struct DrawConfig
	{
		bool textOnly;
		float scale;
		float posY;
		float widthFraction;
		int entityScanInterval;
		int playerScanInterval;
		SDK::FLinearColor lineColor;
		CompassConfig::EntitySettings players;
		CompassConfig::EntitySettings cores;
		CompassConfig::MarkerSettings markers;
		CompassConfig::FoundableSettings foundables;
		CompassConfig::EntitySettings enemies;
		CompassConfig::EntitySettings customPins;
	};

	static DrawConfig s_cfg = {};

	static void RefreshConfig()
	{
		s_cfg.textOnly = CompassConfig::Config::IsTextOnly();
		s_cfg.scale = CompassConfig::Config::GetScale();
		s_cfg.posY = CompassConfig::Config::GetPosY();
		s_cfg.widthFraction = CompassConfig::Config::GetWidthFraction();
		s_cfg.entityScanInterval = CompassConfig::Config::GetEntityScanInterval();
		s_cfg.playerScanInterval = CompassConfig::Config::GetPlayerScanInterval();
		CompassConfig::Config::GetLineColor(
			s_cfg.lineColor.R, s_cfg.lineColor.G, s_cfg.lineColor.B, s_cfg.lineColor.A);
		// A feature that is off keeps its section unread and its markers hidden.
		s_cfg.players = CompassFields::PlayerMarkers.On()
			? CompassConfig::Config::GetPlayers() : CompassConfig::EntitySettings{};
		s_cfg.cores = CompassFields::BaseCoreMarkers.On()
			? CompassConfig::Config::GetBaseCores() : CompassConfig::EntitySettings{};
		s_cfg.markers = CompassConfig::Config::GetMarkers();
		s_cfg.foundables = CompassFields::FoundableMarkers.On()
			? CompassConfig::Config::GetFoundables() : CompassConfig::FoundableSettings{};
		s_cfg.enemies = CompassFields::EnemyMarkers.On()
			? CompassConfig::Config::GetEnemies() : CompassConfig::EntitySettings{};
		s_cfg.customPins = CompassFields::CustomPinMarkers.On()
			? CompassConfig::Config::GetCustomPins() : CompassConfig::EntitySettings{};
	}

	// ---------------------------------------------------------------------------
	// Throttled entity cache (time-based, driven by OnTick not frame count)
	// ---------------------------------------------------------------------------

	static float s_scanAccum = 0.0f;
	static float s_playerScanAccum = 0.0f;

	// ---------------------------------------------------------------------------
	// GatherPlayersData -- direct call to UCrMapManuSubsystem::GatherPlayersData
	// ---------------------------------------------------------------------------
	// Calling this before ScanPlayerMarkers forces the subsystem to refresh its
	// PlayersMarkerDataContainer immediately, rather than waiting for the game to
	// set bMapMenuOpen and process it on the next gameplay tick.
	// ---------------------------------------------------------------------------

	using GatherPlayersData_t = void(__fastcall*)(void* thisSubsystem);
	static GatherPlayersData_t g_gatherPlayersDataFn = nullptr;
	static void* g_mapManuSubsystem = nullptr;
	static SDK::UWorld* g_mapManuWorld = nullptr;

	// Filled in by ResolvePatterns; 0 means the pattern missed on this build and
	// player markers fall back to the game's own once-per-tick refresh.
	static uintptr_t g_gatherPlayersDataAddr = 0;

	static bool CallGatherPlayersData(GatherPlayersData_t fn, void* subsystem)
	{
		__try
		{
			fn(subsystem);
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	}

	static void* FindMapManuSubsystem(SDK::UWorld* world)
	{
		SDK::TUObjectArray* arr = SDK::UObject::GObjects.GetTypedPtr();
		if (!arr) return nullptr;

		try
		{
			for (int32_t i = 0; i < arr->NumElements; ++i)
			{
				SDK::UObject* obj = arr->GetByIndex(i);
				if (!obj || !obj->Class) continue;
				if (obj->Outer != static_cast<SDK::UObject*>(world)) continue;

				std::string className = obj->Class->GetName();
				if (className == "CrMapManuSubsystem")
					return obj;
			}
		}
		catch (...)
		{
		}

		return nullptr;
	}

	static std::vector<Layout::BaseCoreEntry> s_cores;
	static std::vector<Layout::MarkerEntry> s_markers; // all visible POIs (incl. caves)
	static std::vector<Layout::FoundableEntry> s_foundables;
	static std::vector<Layout::EnemyEntry> s_enemies;
	static std::vector<Layout::PlayerMarkerEntry> s_playerMarkers;
	static std::vector<Layout::CustomPinEntry> s_customPins;
	static bool s_isChimeraMain = false; // set by OnAnyWorldBeginPlay, cleared by OnBeforeWorldEndPlay

	static void RefreshEntities(SDK::UWorld* world)
	{
		LOG_TRACE("[Compass] RefreshEntities: world=%p", static_cast<void*>(world));

		LOG_TRACE("[Compass] >> ScanBaseCores...");
		if (CompassFields::BaseCoreMarkers.On())
		{
			try { s_cores = Layout::ScanBaseCores(world); }
			catch (...)
			{
				LOG_WARN("[Compass] Exception in ScanBaseCores -- cache cleared");
				s_cores.clear();
			}
		}
		LOG_TRACE("[Compass] >> ScanBaseCores done (%d)", static_cast<int>(s_cores.size()));

		LOG_TRACE("[Compass] >> ScanMarkers...");
		try { s_markers = Layout::ScanMarkers(world); }
		catch (...)
		{
			LOG_WARN("[Compass] Exception in ScanMarkers -- cache cleared");
			s_markers.clear();
		}
		LOG_TRACE("[Compass] >> ScanMarkers done (%d)", static_cast<int>(s_markers.size()));

		LOG_TRACE("[Compass] >> ScanFoundables...");
		if (CompassFields::FoundableMarkers.On())
		{
			try { s_foundables = Layout::ScanFoundables(world); }
			catch (...)
			{
				LOG_WARN("[Compass] Exception in ScanFoundables -- cache cleared");
				s_foundables.clear();
			}
		}
		LOG_TRACE("[Compass] >> ScanFoundables done (%d)", static_cast<int>(s_foundables.size()));

		LOG_TRACE("[Compass] >> ScanEnemies...");
		if (CompassFields::EnemyMarkers.On())
		{
			try { s_enemies = Layout::ScanEnemies(world); }
			catch (...)
			{
				LOG_WARN("[Compass] Exception in ScanEnemies -- cache cleared");
				s_enemies.clear();
			}
		}
		LOG_TRACE("[Compass] >> ScanEnemies done (%d)", static_cast<int>(s_enemies.size()));

		LOG_TRACE("[Compass] >> ScanCustomPins...");
		if (CompassFields::CustomPinMarkers.On())
		{
			try { s_customPins = Layout::ScanCustomPins(world); }
			catch (...)
			{
				LOG_WARN("[Compass] Exception in ScanCustomPins -- cache cleared");
				s_customPins.clear();
			}
		}
		LOG_TRACE("[Compass] >> ScanCustomPins done (%d)", static_cast<int>(s_customPins.size()));

		// HLOD fallback: appends distant markers/cores not yet in the streaming radius.
		// Deduplicates against already-found real actor entries by proximity.
		LOG_TRACE("[Compass] >> ScanHLOD...");
		try { Layout::ScanHLOD(world, s_markers, s_cores); }
		catch (...) { LOG_WARN("[Compass] Exception in ScanHLOD -- ignored"); }
		LOG_TRACE("[Compass] >> ScanHLOD done (markers=%d cores=%d after merge)",
		          static_cast<int>(s_markers.size()), static_cast<int>(s_cores.size()));

		int caveCount = 0;
		for (const auto& m : s_markers)
			if (m.type == Layout::PoiType::Cave) ++caveCount;

		int bodyCount = 0, droneCount = 0;
		for (const auto& f : s_foundables)
		{
			if (f.type == Layout::FoundableType::DeadBody) ++bodyCount;
			else if (f.type == Layout::FoundableType::Drone) ++droneCount;
		}
		LOG_TRACE(
			"[Compass] Scan complete: %d players, %d cores, %d POIs (%d caves), %d bodies, %d drones, %d enemies, %d custompins",
			static_cast<int>(s_playerMarkers.size()), static_cast<int>(s_cores.size()),
			static_cast<int>(s_markers.size()), caveCount, bodyCount, droneCount, static_cast<int>(s_enemies.size()),
			static_cast<int>(s_customPins.size()));

		// Signal the subsystem AFTER reading so the next gameplay tick (which runs
		// before our next PostRender) sees the flag and refreshes its data in time
		// for our next scan. Setting it before the reads would only affect the tick
		// AFTER this frame anyway, making it equivalent but less clear in intent.
		SDK::APlayerController* localPC = SDK::UGameplayStatics::GetPlayerController(world, 0);
		if (uint8_t* mapMenuOpen = CompassFields::MapMenuOpen.In(localPC))
			*mapMenuOpen = 1;
	}

	// ---------------------------------------------------------------------------
	// OnTick -- game-thread tick; drives all entity scanning so DrawCompass is
	// purely read + draw with no world traversal on the render path.
	// ---------------------------------------------------------------------------

	static void OnTick(float deltaSeconds)
	{
		if (!s_isChimeraMain) return;

		SDK::UWorld* world = nullptr;
		try { world = PluginSdkRuntime::GetWorld(); }
		catch (...) { return; }
		if (!world) return;

		// Player scan (fast interval).
		s_playerScanAccum += deltaSeconds;
		const float playerScanInterval = s_cfg.playerScanInterval / 60.0f;
		if (CompassFields::PlayerMarkers.On() && s_playerScanAccum >= playerScanInterval)
		{
			s_playerScanAccum = 0.0f;

			if (g_gatherPlayersDataFn)
			{
				if (world != g_mapManuWorld)
				{
					g_mapManuSubsystem = nullptr;
					g_mapManuWorld = world;
				}
				if (!g_mapManuSubsystem)
					g_mapManuSubsystem = FindMapManuSubsystem(world);

				if (g_mapManuSubsystem)
				{
					if (!CallGatherPlayersData(g_gatherPlayersDataFn, g_mapManuSubsystem))
					{
						LOG_WARN("[Compass] Exception in GatherPlayersData -- resetting subsystem cache");
						g_mapManuSubsystem = nullptr;
					}
				}
			}

			try { s_playerMarkers = Layout::ScanPlayerMarkers(world); }
			catch (...)
			{
				LOG_WARN("[Compass] Exception in ScanPlayerMarkers -- cache cleared");
				s_playerMarkers.clear();
			}
		}

		// Full entity scan (slow interval).
		s_scanAccum += deltaSeconds;
		const float scanInterval = s_cfg.entityScanInterval / 60.0f;
		if (s_scanAccum >= scanInterval)
		{
			s_scanAccum = 0.0f;
			RefreshEntities(world);
		}
	}

	// ---------------------------------------------------------------------------
	// Compass drawing
	// ---------------------------------------------------------------------------

	static void DrawCompass(SDK::AHUD* hud, SDK::UCanvas* canvas, SDK::UWorld* world)
	{
		const bool textOnly = s_cfg.textOnly;
		if (!textOnly)
			EnsureTextures(world);

		SDK::APlayerController* pc = hud->GetOwningPlayerController();
		if (!pc)
			return;
		
		SDK::APawn* localPawn = pc->K2_GetPawn();
		SDK::FVector playerLoc = localPawn ? localPawn->K2_GetActorLocation() : SDK::FVector{};

		SDK::FRotator controlRot = pc->GetControlRotation();
		// rawYaw: UE-space (0 = East). Used for entity bearing math.
		// yaw:    compass-convention (0 = North). Used for cardinal placement.
		const float rawYaw = static_cast<float>(controlRot.Yaw);
		const float yaw = rawYaw + 90.0f;

		const float scale = s_cfg.scale;
		const float posY = s_cfg.posY;
		const float halfWidth = canvas->SizeX * 0.5f * s_cfg.widthFraction * scale;
		const float centerX = canvas->SizeX * 0.5f;
		const float left = centerX - halfWidth;
		const float right = centerX + halfWidth;

		// One-time config dump to help diagnose sizing/position issues
		static bool s_firstDraw = true;
		if (s_firstDraw)
		{
			s_firstDraw = false;
			LOG_INFO("[Compass] First draw -- scale=%.2f posY=%.1f widthFraction=%.2f scanInterval=%d canvas=%dx%d",
			         scale, posY, s_cfg.widthFraction, s_cfg.entityScanInterval,
			         static_cast<int>(canvas->SizeX), static_cast<int>(canvas->SizeY));
		}

		SDK::UFont* labelFont = SDK::UEngine::GetEngine() ? SDK::UEngine::GetEngine()->LargeFont : nullptr;
		SDK::UFont* entityFont = labelFont; // same font as cardinals for bigger entity text

		// --- Helper: world position -> compass screenX ---
		auto ToScreenX = [&](const SDK::FVector& pos) -> float
		{
			const float dx = pos.X - playerLoc.X;
			const float dy = pos.Y - playerLoc.Y;
			float delta = atan2f(dy, dx) * (180.0f / 3.14159265358979f) - rawYaw;
			while (delta > 180.0f) delta -= 360.0f;
			while (delta < -180.0f) delta += 360.0f;
			return centerX + (delta / 90.0f) * halfWidth;
		};

		auto InBounds = [&](float x) { return x >= left && x <= right; };

		// --- Colors ---
		static constexpr SDK::FLinearColor white{1.0f, 1.0f, 1.0f, 0.9f};
		static constexpr SDK::FLinearColor dimWhite{1.0f, 1.0f, 1.0f, 0.4f};
		static constexpr SDK::FLinearColor yellow{1.0f, 0.85f, 0.0f, 1.0f};
		static constexpr SDK::FLinearColor colPlayer{0.3f, 0.8f, 1.0f, 1.0f}; // cyan
		static constexpr SDK::FLinearColor colCore{1.0f, 0.5f, 0.0f, 1.0f}; // orange
		static constexpr SDK::FLinearColor colMarker{1.0f, 1.0f, 0.0f, 1.0f}; // yellow
		static constexpr SDK::FLinearColor colBody{0.7f, 0.7f, 0.7f, 1.0f}; // grey
		static constexpr SDK::FLinearColor colDrone{0.4f, 0.9f, 0.5f, 1.0f}; // green
		static constexpr SDK::FLinearColor colEnemy{1.0f, 0.15f, 0.15f, 1.0f}; // red
		static constexpr SDK::FLinearColor black1{0.0f, 0.0f, 0.0f, 1.0f};

		// --- Horizontal compass line ---
		// DrawLine ignores alpha (uses FBatchedElements which has no translucent blend).
		// DrawRect uses FCanvasTileItem with SE_BLEND_Translucent, so alpha works.
		const float lineH = 1.5f * scale;
		hud->DrawRect(s_cfg.lineColor, left, posY - lineH * 0.5f, right - left, lineH);

		// --- Cardinal ticks + labels (above the line) ---
		for (const auto& c : CARDINALS)
		{
			float delta = c.worldYaw - yaw;
			while (delta > 180.0f) delta -= 360.0f;
			while (delta < -180.0f) delta += 360.0f;

			const float screenX = centerX + (delta / 90.0f) * halfWidth;
			if (!InBounds(screenX)) continue;

			// Tick crossing the line
			hud->DrawLine(screenX, posY - 6.0f * scale, screenX, posY + 4.0f * scale, white, 1.0f * scale);

			// Label above the tick
			SDK::FString label(c.label);
			SDK::FVector2D sz = canvas->K2_TextSize(labelFont, label, {scale, scale});
			canvas->K2_DrawText(labelFont, label,
			                    {screenX - sz.X * 0.5f, posY - 9.0f * scale - sz.Y},
			                    {scale, scale}, white, 0.0f,
			                    {0.0f, 0.0f, 0.0f, 0.0f}, {0.0f, 0.0f},
			                    false, false, true, black1);
		}

		// --- Center notch (current heading, above the line) ---
		hud->DrawLine(centerX, posY - 22.0f * scale, centerX, posY + 6.0f * scale, yellow, 2.5f * scale);

		// --- Config: per-entity settings (from cache) ---
		const auto& cfgPlayers = s_cfg.players;
		const auto& cfgCores = s_cfg.cores;
		const auto& cfgMarkers = s_cfg.markers;
		const auto& cfgFoundables = s_cfg.foundables;
		const auto& cfgEnemies = s_cfg.enemies;
		const auto& cfgCustomPins = s_cfg.customPins;

		// --- Helper: distance-based alpha (fade starts at 80% of max) ---
		auto DistAlpha = [](float dist, float maxDist) -> float
		{
			if (maxDist <= 0.0f) return 1.0f;
			const float fadeStart = maxDist * 0.8f;
			if (dist <= fadeStart) return 1.0f;
			if (dist >= maxDist) return 0.0f;
			return (maxDist - dist) / (maxDist - fadeStart);
		};

		// --- Entity markers (below the line) ---

		// Shared edge-fade calculation
		auto EdgeAlpha = [&](float screenX) -> float
		{
			const float edgeFadeZone = halfWidth * 0.12f;
			return fminf(
				fminf((screenX - left) / edgeFadeZone, 1.0f),
				fminf((right - screenX) / edgeFadeZone, 1.0f)
			);
		};

		// Draw icon texture (with text fallback). White tint preserves art colours.
		auto DrawEntityIcon = [&](float screenX, SDK::UTexture* tex,
		                          const wchar_t* fallbackSym, SDK::FLinearColor colour, float alpha)
		{
			if (!InBounds(screenX)) return;
			const float finalAlpha = alpha * EdgeAlpha(screenX);
			if (finalAlpha <= 0.0f) return;

			hud->DrawLine(screenX, posY + 4.0f * scale, screenX, posY + 10.0f * scale, colour, 1.5f * scale);

			if (!textOnly && tex && IsValidTexture(tex))
			{
				const float iconSize = 22.0f * scale;
				SDK::FLinearColor tint{1.0f, 1.0f, 1.0f, finalAlpha};
				hud->DrawTexture(tex,
				                 screenX - iconSize * 0.5f, posY + 10.0f * scale, iconSize, iconSize,
				                 0.0f, 0.0f, 1.0f, 1.0f,
				                 tint, SDK::EBlendMode::BLEND_Translucent,
				                 1.0f, false, 0.0f, {0.5f, 0.5f});
			}
			else
			{
				colour.A *= finalAlpha;
				const float es = scale * 1.25f;
				SDK::FString s(fallbackSym);
				SDK::FVector2D sz = canvas->K2_TextSize(entityFont, s, {es, es});
				SDK::FLinearColor outline{0.0f, 0.0f, 0.0f, finalAlpha};
				canvas->K2_DrawText(entityFont, s,
				                    {screenX - sz.X * 0.5f, posY + 16.0f * scale},
				                    {es, es}, colour, 0.0f,
				                    {0.0f, 0.0f, 0.0f, 0.0f}, {0.0f, 0.0f},
				                    false, false, true, outline);
			}
		};

		// Draw icon + name label below (used for base cores which have custom names).
		auto DrawEntityIconWithLabel = [&](float screenX, SDK::UTexture* tex,
		                                   const wchar_t* label, SDK::FLinearColor colour, float alpha)
		{
			if (!InBounds(screenX)) return;
			const float finalAlpha = alpha * EdgeAlpha(screenX);
			if (finalAlpha <= 0.0f) return;

			hud->DrawLine(screenX, posY + 4.0f * scale, screenX, posY + 10.0f * scale, colour, 1.5f * scale);

			const float iconSize = 22.0f * scale;

			const bool showIcon = !textOnly && tex && IsValidTexture(tex);
			if (showIcon)
			{
				SDK::FLinearColor tint{1.0f, 1.0f, 1.0f, finalAlpha};
				hud->DrawTexture(tex,
				                 screenX - iconSize * 0.5f, posY + 10.0f * scale, iconSize, iconSize,
				                 0.0f, 0.0f, 1.0f, 1.0f,
				                 tint, SDK::EBlendMode::BLEND_Translucent,
				                 1.0f, false, 0.0f, {0.5f, 0.5f});
			}

			// Name label below the icon (or below stem if no icon)
			colour.A *= finalAlpha;
			const float es = scale * 1.0f;
			SDK::FString s(label);
			SDK::FVector2D sz = canvas->K2_TextSize(entityFont, s, {es, es});
			SDK::FLinearColor outline{0.0f, 0.0f, 0.0f, finalAlpha};
			const float labelY = posY + 10.0f * scale + (showIcon ? iconSize : 6.0f * scale);
			canvas->K2_DrawText(entityFont, s,
			                    {screenX - sz.X * 0.5f, labelY},
			                    {es, es}, colour, 0.0f,
			                    {0.0f, 0.0f, 0.0f, 0.0f}, {0.0f, 0.0f},
			                    false, false, true, outline);
		};

		// --- POI helpers (used when building the draw queue below) ---
		auto PoiTypeEnabled = [&](Layout::PoiType t) -> bool
		{
			switch (t)
			{
			case Layout::PoiType::Antena: return cfgMarkers.showAntena;
			case Layout::PoiType::AbandonedBase: return cfgMarkers.showAbandonedBase;
			case Layout::PoiType::Cave: return cfgMarkers.showCave;
			case Layout::PoiType::Obelisk: return cfgMarkers.showObelisk;
			default: return false;
			}
		};
		auto PoiSymbol = [](Layout::PoiType t) -> const wchar_t*
		{
			switch (t)
			{
			case Layout::PoiType::Antena: return L"A";
			case Layout::PoiType::AbandonedBase: return L"Ab";
			case Layout::PoiType::Cave: return L"C";
			case Layout::PoiType::Obelisk: return L"Ob";
			default: return L"?";
			}
		};
		auto PoiDistance = [&](Layout::PoiType t) -> float
		{
			switch (t)
			{
			case Layout::PoiType::Antena: return cfgMarkers.antenaDistance;
			case Layout::PoiType::AbandonedBase: return cfgMarkers.abandonedBaseDistance;
			case Layout::PoiType::Cave: return cfgMarkers.caveDistance;
			case Layout::PoiType::Obelisk: return cfgMarkers.obeliskDistance;
			default: return 0.0f;
			}
		};

		// ---------------------------------------------------------------------------
		// Unified draw queue -- all visible entities sorted by distance descending
		// so that the closest entity is always drawn last (rendered on top).
		// ---------------------------------------------------------------------------
		enum class DrawKind : uint8_t { Icon, IconWithLabel };
		struct DrawCall
		{
			float distSq;
			float screenX;
			float alpha;
			SDK::UTexture* tex;
			SDK::FLinearColor colour;
			wchar_t label[64];
			DrawKind kind;
		};
		std::vector<DrawCall> drawQueue;
		drawQueue.reserve(
			s_playerMarkers.size() + s_cores.size() +
			s_markers.size() + s_foundables.size() + s_customPins.size());

		auto PushIcon = [&](float distSq, float sx, SDK::UTexture* tex,
		                    const wchar_t* sym, SDK::FLinearColor col, float alpha)
		{
			DrawCall dc{};
			dc.distSq = distSq; dc.screenX = sx; dc.alpha = alpha;
			dc.tex = tex; dc.colour = col; dc.kind = DrawKind::Icon;
			wcsncpy_s(dc.label, sym, _TRUNCATE);
			drawQueue.push_back(dc);
		};
		auto PushLabel = [&](float distSq, float sx, SDK::UTexture* tex,
		                     const wchar_t* lbl, SDK::FLinearColor col, float alpha)
		{
			DrawCall dc{};
			dc.distSq = distSq; dc.screenX = sx; dc.alpha = alpha;
			dc.tex = tex; dc.colour = col; dc.kind = DrawKind::IconWithLabel;
			wcsncpy_s(dc.label, lbl, _TRUNCATE);
			drawQueue.push_back(dc);
		};

		// Players -- sourced from PlayersMarkerDataContainer (map subsystem), not actor scan.
		// Covers all online players regardless of streaming range.
		if (cfgPlayers.enabled)
		{
			for (const auto& p : s_playerMarkers)
			{
				const float dx = p.location.X - playerLoc.X, dy = p.location.Y - playerLoc.Y;
				const float distSq = dx * dx + dy * dy;
				const float alpha = DistAlpha(sqrtf(distSq), cfgPlayers.distance);
				if (alpha <= 0.0f) continue;
				PushLabel(distSq, ToScreenX(p.location), s_tex.player, p.playerName.c_str(), colPlayer, alpha);
			}
		}
		if (cfgCores.enabled)
		{
			for (const auto& c : s_cores)
			{
				const float dx = c.location.X - playerLoc.X, dy = c.location.Y - playerLoc.Y;
				const float distSq = dx * dx + dy * dy;
				const float alpha = DistAlpha(sqrtf(distSq), cfgCores.distance);
				if (alpha <= 0.0f) continue;
				PushLabel(distSq, ToScreenX(c.location), s_tex.baseCore, c.name.c_str(), colCore, alpha);
			}
		}
		// ForgottenEngine and OrbitalLander are filtered out in ScanMarkers.
		if (cfgMarkers.enabled)
		{
			for (const auto& m : s_markers)
			{
				if (!PoiTypeEnabled(m.type)) continue;
				const float dx = m.location.X - playerLoc.X, dy = m.location.Y - playerLoc.Y;
				const float distSq = dx * dx + dy * dy;
				const float alpha = DistAlpha(sqrtf(distSq), PoiDistance(m.type));
				if (alpha <= 0.0f) continue;
				PushIcon(distSq, ToScreenX(m.location), GetPoiTexture(m.type), PoiSymbol(m.type), colMarker, alpha);
			}
		}
		if (cfgFoundables.enabled)
		{
			for (const auto& f : s_foundables)
			{
				const bool isBody = f.type == Layout::FoundableType::DeadBody;
				const bool isDrone = f.type == Layout::FoundableType::Drone;

				if (isBody && !cfgFoundables.showDeadBody) continue;
				if (isDrone && !cfgFoundables.showDrone) continue;

				const float maxDist = isBody ? cfgFoundables.deadBodyDistance : cfgFoundables.droneDistance;
				const float dx = f.location.X - playerLoc.X, dy = f.location.Y - playerLoc.Y;
				const float distSq = dx * dx + dy * dy;
				const float alpha = DistAlpha(sqrtf(distSq), maxDist);
				if (alpha <= 0.0f) continue;
				if (isBody)
					PushIcon(distSq, ToScreenX(f.location), s_tex.body, L"D", colBody, alpha);
				else
					PushIcon(distSq, ToScreenX(f.location), s_tex.drone, L"Dr", colDrone, alpha);
			}
		}
		// --- Personal map pins (ACrGameStateBase::PlayerPersonalMarkers) ---
		if (cfgCustomPins.enabled)
		{
			for (const auto& pin : s_customPins)
			{
				const float dx = pin.location.X - playerLoc.X, dy = pin.location.Y - playerLoc.Y;
				const float distSq = dx * dx + dy * dy;
				const float alpha = DistAlpha(sqrtf(distSq), cfgCustomPins.distance);
				if (alpha <= 0.0f) continue;
				const SDK::FLinearColor col = {
					pin.color.R, pin.color.G, pin.color.B,
					pin.color.A > 0.0f ? pin.color.A : 1.0f
				};
				const wchar_t* lbl = pin.playerName.empty() ? L"Pin" : pin.playerName.c_str();
				PushLabel(distSq, ToScreenX(pin.location), s_tex.customPin, lbl, col, alpha);
			}
		}

		// Sort furthest-first so closest entities paint on top.
		std::sort(drawQueue.begin(), drawQueue.end(),
		          [](const DrawCall& a, const DrawCall& b) { return a.distSq > b.distSq; });

		for (const auto& dc : drawQueue)
		{
			if (dc.kind == DrawKind::IconWithLabel)
				DrawEntityIconWithLabel(dc.screenX, dc.tex, dc.label, dc.colour, dc.alpha);
			else
				DrawEntityIcon(dc.screenX, dc.tex, dc.label, dc.colour, dc.alpha);
		}

		// --- Enemy dots (drawn on the compass line itself, on top of everything else) ---
		// Two-pass per dot: outer glow rect then bright core rect.
		if (cfgEnemies.enabled)
		{
			const float dotCore = 3.0f * scale;
			const float dotGlow = dotCore * 2.5f;
			const float halfCore = dotCore * 0.5f;
			const float halfGlow = dotGlow * 0.5f;

			for (const auto& e : s_enemies)
			{
				const float dx = e.location.X - playerLoc.X, dy = e.location.Y - playerLoc.Y;
				const float dist = sqrtf(dx * dx + dy * dy);
				const float alpha = DistAlpha(dist, cfgEnemies.distance);
				if (alpha <= 0.0f) continue;

				const float sx = ToScreenX(e.location);
				if (!InBounds(sx)) continue;

				const float edgeAlpha = EdgeAlpha(sx);
				const float finalAlpha = alpha * edgeAlpha;
				if (finalAlpha <= 0.0f) continue;

				// Glow halo
				SDK::FLinearColor glow{colEnemy.R, colEnemy.G, colEnemy.B, 0.25f * finalAlpha};
				hud->DrawRect(glow, sx - halfGlow, posY - halfGlow, dotGlow, dotGlow);

				// Bright core
				SDK::FLinearColor core{colEnemy.R, colEnemy.G, colEnemy.B, finalAlpha};
				hud->DrawRect(core, sx - halfCore, posY - halfCore, dotCore, dotCore);
			}
		}
	}

	// ---------------------------------------------------------------------------
	// World end play callback — clears all stale state before the world tears down.
	// Prevents crashes on save reload when the new UWorld reuses the same address.
	// ---------------------------------------------------------------------------

	// ---------------------------------------------------------------------------
	// OnExperienceLoadComplete -- fires when the gameplay experience finishes
	// loading (game framework fully ready).  Safe game-thread context for calling
	// StaticLoadObject without touching the streaming system mid-render.
	// ---------------------------------------------------------------------------
	static void OnExperienceLoadComplete()
	{
		LOG_INFO("[Compass] OnExperienceLoadComplete: loading textures");
		SDK::UWorld* world = PluginSdkRuntime::GetWorld();
		LoadAllTextures(world);
	}

	// ---------------------------------------------------------------------------
	// OnAnyWorldBeginPlay -- secondary trigger so textures are re-loaded if the
	// experience callback was missed (e.g. plugin loaded mid-session) or after a
	// seamless travel that fires BeginPlay without re-firing ExperienceLoadComplete.
	// ---------------------------------------------------------------------------
	static void OnAnyWorldBeginPlay(SDK::UWorld* world, const char* worldName)
	{
		if (!world) return;
		s_isChimeraMain = std::string(worldName) == "ChimeraMain";
		if (!s_isChimeraMain) return;

		LOG_INFO("[Compass] OnAnyWorldBeginPlay: '%s' -- loading textures", worldName);
		LoadAllTextures(world);
	}

	// Hot-load seed: the plugin installed after ChimeraMain began, so
	// OnAnyWorldBeginPlay will never fire for this world. The loader calls this on
	// the first tick after we register if ChimeraMain is already running.
	static void OnUIStateChanged(uint64_t /*oldState*/, uint64_t newState, void* /*userData*/)
	{
		if (!(newState & PLUGIN_UI_IN_GAMEPLAY) || s_isChimeraMain) return;

		try
		{
			SDK::UWorld* currentWorld = PluginSdkRuntime::GetWorld();
			if (!currentWorld) return;

			s_isChimeraMain = true;
			LoadAllTextures(currentWorld);
			LOG_INFO("[Compass] Install: already in ChimeraMain -- s_isChimeraMain set");
		}
		catch (...) {}
	}

	static void OnBeforeWorldEndPlay(SDK::UWorld* /*world*/, const char* worldName)
	{
		LOG_INFO("[Compass] OnBeforeWorldEndPlay: '%s' — clearing all caches and stale pointers", worldName);

		// Clear entity caches so they aren't accessed with stale data on the next world.
		s_cores.clear();
		s_markers.clear();
		s_foundables.clear();
		s_enemies.clear();
		s_playerMarkers.clear();
		s_customPins.clear();

		// Reset subsystem pointers — the CrMapManuSubsystem is being destroyed with the world.
		g_mapManuSubsystem = nullptr;
		g_mapManuWorld = nullptr;

		// Clear texture slots -- they will be reloaded fresh in OnExperienceLoadComplete
		// or OnAnyWorldBeginPlay for the new world.  Nulling them here prevents DrawCompass
		// from drawing with stale pointers during the world teardown/transition gap.
		ClearTextures();

		// Reset world tracking.
		s_isChimeraMain = false;

		// Reset scan throttle accumulators so scans fire immediately in the new world.
		s_scanAccum = 9999.0f;
		s_playerScanAccum = 9999.0f;

		// Notify layout scanners to force reset their internal statics on next call,
		// even if the new world reuses the same pointer address as this one.
		Layout::NotifyWorldEndPlay();

		LOG_INFO("[Compass] OnBeforeWorldEndPlay: cleanup complete");
	}

	// RefreshConfig() re-reads every category into s_cfg (General.TextOnly, Compass.*,
	// Players.*, BaseCores.*, Markers.*, Foundables.*, Enemies.*, CustomPins.*), so any
	// changed item -- regardless of which category it lives in -- needs a refresh.
	// This runs synchronously on the game thread, same as OnHUDPostRender, so the
	// updated value is already in s_cfg by the time the next frame is drawn -- no
	// extra signaling needed for the compass to look live while editing.
	static void OnConfigChanged(const char* category, const char* key, const char* newValue)
	{
		//LOG_INFO("[Compass] Config changed: %s.%s = %s -- refreshing", category, key, newValue);
		RefreshConfig();
	}

	// ---------------------------------------------------------------------------
	// UGameViewportClient::Draw detour + PostRender draw
	// ---------------------------------------------------------------------------

	// Draws the compass during AHUD::PostRender while the canvas is live.
	// This is the correct insertion point for FSR3 Frame Generation: PostRender fires
	// inside UGameViewportClient::Draw, before FSR3 FG captures the UI layer for
	// composition onto generated frames. Drawing here means the compass is included in
	// that UI capture and appears on every display frame (both real and interpolated).
	static void OnHUDPostRender(void* hudPtr)
	{
		if (!CompassConfig::Config::IsEnabled()) return;
		if (!s_isChimeraMain) return;

		SDK::AHUD* hud = static_cast<SDK::AHUD*>(hudPtr);
		if (!hud || !hud->Canvas) return;

		SDK::UWorld* world = nullptr;
		try { world = PluginSdkRuntime::GetWorld(); }
		catch (...) { return; }
		if (!world) return;

		try { DrawCompass(hud, hud->Canvas, world); }
		catch (...) { LOG_ERROR("[Compass] Exception in DrawCompass (PostRender) -- suppressed"); }
	}

	// The viewport detour is kept so we retain the AOB hook infrastructure, but
	// all drawing now happens in OnHUDPostRender above. The detour simply calls
	// through to the original.
	static void __fastcall ViewportDrawDetour(void* thisPtr, void* inViewport, void* sceneCanvas)
	{
		if (g_originalViewportDraw)
			g_originalViewportDraw(thisPtr, inViewport, sceneCanvas);
	}

	// ---------------------------------------------------------------------------
	// Install / Remove
	// ---------------------------------------------------------------------------

	static uintptr_t ResolveFunction(IPluginSelf* self, IPluginHookScanner* scanner,
		const char* hookName, const char* pattern)
	{
		PluginScanRequest req = PLUGIN_SCAN_REQUEST_INIT;
		req.hookName = hookName;
		req.pattern  = pattern;
		req.kind     = PLUGIN_SCAN_FUNCTION_START;
		req.flags    = PLUGIN_SCAN_FLAG_OPTIONAL;

		return scanner->Resolve(self, &req);
	}

	void ResolvePatterns(IPluginSelf* self, IPluginHookScanner* scanner)
	{
		if (!self || !scanner)
			return;

		// Optional: a miss leaves the plugin loaded but silent, which is what the
		// old scan-at-init path did. The loader still lists it for the user.
		//
		// FUNCTION_START on both: Draw is detoured and GatherPlayersData is
		// called, and either way the address has to be a function's primary
		// entry. The loader checks that against the exception directory.
		//
		// Draw is a key in the loader's map, so a game update that moves it is a
		// map row and not a new plugin build. The pattern stays as the fallback
		// for a loader whose map has no row for the running build.
		g_viewportDrawAddr = PluginMap::Function(scanner, self,
			"UGameViewportClient::Draw", AobPatterns::UGameViewportClient_Draw,
			PLUGIN_SCAN_FUNCTION_START, PLUGIN_SCAN_FLAG_OPTIONAL);

		// GatherPlayersData used to go through ResolveOptionalUnique, because its
		// prologue is shared with at least one unrelated function on some builds
		// and taking the first of several matches means calling into whichever
		// one sits lower in the image. That is the default rule now -- every
		// resolve demands exactly one match -- so the plain call says the same
		// thing, and FUNCTION_START adds the check the old path never made.
		g_gatherPlayersDataAddr = ResolveFunction(self, scanner,
			"UCrMapManuSubsystem::GatherPlayersData",
			AobPatterns::UCrMapManuSubsystem_GatherPlayersData);
	}

	bool Install(IPluginSelf* self)
	{
		if (!self)
		{
			LOG_ERROR("[Compass] Install called with null self");
			return false;
		}

		IPluginHooks* hooks = self->hooks;

		if (!hooks)
		{
			LOG_ERROR("[Compass] Install called with null hooks interface");
			return false;
		}

		if (!hooks->HUD)
		{
			LOG_ERROR("[Compass] hooks->HUD is null -- compass requires a client build");
			return false;
		}

		g_self  = self;
		g_hooks = hooks;

		// GatherPlayersData comes from our own scan back in OnPluginLoadHooks, not
		// from the modloader's cache -- see aob_patterns.h for why.
		if (g_gatherPlayersDataAddr)
		{
			g_gatherPlayersDataFn = reinterpret_cast<GatherPlayersData_t>(g_gatherPlayersDataAddr);
			LOG_INFO("[Compass] GatherPlayersData at 0x%llX", static_cast<unsigned long long>(g_gatherPlayersDataAddr));
		}
		else
		{
			LOG_WARN("[Compass] GatherPlayersData address not available -- player markers may not update in real time");
		}

		// Install the UGameViewportClient::Draw detour on the address resolved
		// back in OnPluginLoadHooks.
		uintptr_t addr = g_viewportDrawAddr;
		if (!addr)
		{
			LOG_ERROR("[Compass] UGameViewportClient::Draw unresolved -- compass will not render");
			return false;
		}

		g_viewportDrawHook = hooks->Hooks->Install(addr, (void*)ViewportDrawDetour, (void**)&g_originalViewportDraw);
		if (!g_viewportDrawHook)
		{
			LOG_ERROR("[Compass] Failed to install UGameViewportClient::Draw hook -- compass will not render");
			g_originalViewportDraw = nullptr;
			return false;
		}

		LOG_INFO("[Compass] UGameViewportClient::Draw hook installed at 0x%llX", static_cast<unsigned long long>(addr));

		// PostRender is where we draw the compass -- inside Draw, before FSR3 FG captures the UI layer.
		hooks->HUD->RegisterOnPostRender(OnHUDPostRender);

		// Engine tick drives all entity scanning off the render path.
		hooks->Engine->RegisterOnTick(OnTick);

		// Load textures when the gameplay experience is fully ready.
		hooks->World->RegisterOnExperienceLoadComplete(OnExperienceLoadComplete);

		// Secondary trigger: catches seamless travel and plugin-loaded-mid-session cases.
		hooks->World->RegisterOnAnyWorldBeginPlay(OnAnyWorldBeginPlay);

		hooks->UI->RegisterOnConfigChanged(self, OnConfigChanged);

		// Hot-load seed: if we're already in ChimeraMain when the plugin installs,
		// OnUIStateChanged sets the flag on the loader's next tick.
		if (hooks->GameMenu)
			hooks->GameMenu->RegisterOnUIStateChanged(PLUGIN_UI_IN_GAMEPLAY, OnUIStateChanged, nullptr);

		// Clear stale state before world teardown.
		hooks->World->RegisterOnBeforeWorldEndPlay(OnBeforeWorldEndPlay);

		// Populate s_cfg immediately so DrawCompass has valid values before
		// OnConfigChanged ever fires (it only fires on subsequent changes).
		RefreshConfig();

		return true;
	}

	void Remove(IPluginHooks* hooks)
	{
		if (g_viewportDrawHook && hooks && hooks->Hooks)
		{
			hooks->Hooks->Remove(g_viewportDrawHook);
			g_viewportDrawHook     = nullptr;
			g_originalViewportDraw = nullptr;
			LOG_INFO("[Compass] UGameViewportClient::Draw hook uninstalled");
		}

		if (g_hooks && g_hooks->HUD)
			g_hooks->HUD->UnregisterOnPostRender(OnHUDPostRender);

		if (g_hooks && g_hooks->Engine)
			g_hooks->Engine->UnregisterOnTick(OnTick);

		if (g_hooks && g_hooks->World)
		{
			g_hooks->World->UnregisterOnExperienceLoadComplete(OnExperienceLoadComplete);
			g_hooks->World->UnregisterOnAnyWorldBeginPlay(OnAnyWorldBeginPlay);
			g_hooks->World->UnregisterOnBeforeWorldEndPlay(OnBeforeWorldEndPlay);
			LOG_INFO("[Compass] World callbacks unregistered");
		}

		if (g_hooks && g_hooks->GameMenu)
			g_hooks->GameMenu->UnregisterOnUIStateChanged(OnUIStateChanged, nullptr);

		g_hooks        = nullptr;
		g_self         = nullptr;
		g_gatherPlayersDataFn = nullptr;
	}
}
