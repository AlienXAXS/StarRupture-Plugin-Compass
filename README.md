# Compass — StarRupture Client Plugin

Adds a heads-up compass bar to the [StarRupture](https://store.steampowered.com/app/1631270/StarRupture/) game HUD showing nearby entities and points of interest in the direction you are facing.

**Target:** Game client only

---

## What It Shows

Each category can be toggled and distance-limited independently:

| Category | Description |
|---|---|
| Players | Other players in the session |
| Base Cores | Player base cores |
| Map Markers | Antennas, abandoned bases, caves, obelisks |
| Foundables | Dead bodies and drones |
| Enemies | Hostile entities |
| Custom Pins | Player-placed map pins |

---

## Configuration

Config is stored in `Plugins\config\Compass.ini` and is generated on first launch.

| Setting | Description |
|---|---|
| `Enabled` | `0` or `1` — enables the plugin |
| Position, width, scale | Controls the compass bar placement and size |
| Line colour | HEX colour for the compass bar line |
| Per-category render distance | Distance in Unreal Units (`0` = unlimited) |

---

## Installation

1. Download the latest release ZIP from the [Releases](../../releases) page:
   - `Compass_Plugin-Client-*.zip`

2. Extract into your game's `Binaries\Win64\` folder. The ZIP contains a `Plugins\` folder — it will sit alongside your existing `dwmapi.dll`.

3. After the first launch, edit `Plugins\config\Compass.ini` and set `Enabled=1`.

> **Requires [StarRupture-ModLoader](https://github.com/AlienXAXS/StarRupture-ModLoader)** to be installed first.

---

## Troubleshooting

| Problem | Solution |
|---|---|
| Compass not visible | Confirm `Enabled=1` is set in `Plugins\config\Compass.ini`. |
| Plugin not loading | Check `modloader.log` in `Binaries\Win64\` for errors. |

---

## Building from Source

Requires Visual Studio 2022 and the [StarRupture-Plugin-SDK](https://github.com/AlienXAXS/StarRupture-Plugin-SDK).

Clone the repo, open `Compass_Plugin.sln`, and build the `Client Release|x64` configuration. The output DLL will be placed in `build\Client Release\Plugins\`.

---

## Disclaimer

Use at your own risk. The authors are not responsible for any damage caused by using this software.
