# Porism DIMs World Generator Extension SteamCore

Send Porism terrain data over Steam networking on a separate connection from gameplay.

[SteamCore PRO on Fab](https://www.fab.com/listings/d101542d-2534-4b2c-be16-e9b3e5cd4d04)

## Usage

Put this plugin in your project's `Plugins/` folder. Enable it, base Porism DIMs World Generator, and SteamCore PRO on the host and client. You don't need the separate `PorismDIMsWorldGeneratorExtension` plugin.

### Set up Steam networking

Merge this into your project's `Config/DefaultEngine.ini`. Use your Steam App ID in place of `480`.

```ini
[OnlineSubsystem]
DefaultPlatformService=SteamCore

[OnlineSubsystemSteamCore]
bEnabled=True
SteamDevAppId=480
SteamAppId=480
bUseSteamNetworking=True
```

Porism uses Steam for terrain when the session uses `USteamCoreSocketsNetDriver` for gameplay. If your game already uses that driver, keep your setup. Otherwise, choose one option below. A separate `SteamGameNetDriver` entry is optional.

### Option A: Make Steam the default

Use SteamCore Sockets for `GameNetDriver` in `Config/DefaultEngine.ini`:

```ini
[/Script/Engine.GameEngine]
+NetDriverDefinitions=(DefName="GameNetDriver",DriverClassName="/Script/SteamCoreSockets.SteamCoreSocketsNetDriver",DriverClassNameFallback="/Script/SteamCoreSockets.SteamCoreSocketsNetDriver")
```

Replace the existing `GameNetDriver` entry instead of adding a duplicate. If the old entry is inherited, remove it with `-NetDriverDefinitions=` followed by its exact value before adding the new one. Leave other driver definitions in place.

- **Pro:** Keep your usual `?listen` host code. No `NetDriverDef=` option needed.
- **Con:** Sessions that use the default now use Steam. LAN or direct-IP sessions need a separate IP driver if you still support them.

### Option B: Pick Steam per session

Keep your IP `GameNetDriver` and add a second entry in `Config/DefaultEngine.ini`:

```ini
[/Script/Engine.GameEngine]
+NetDriverDefinitions=(DefName="SteamGameNetDriver",DriverClassName="/Script/SteamCoreSockets.SteamCoreSocketsNetDriver",DriverClassNameFallback="/Script/SteamCoreSockets.SteamCoreSocketsNetDriver")
```

Select it when hosting a Steam listen server:

```cpp
#include "Kismet/GameplayStatics.h"

// In your game's host code. Replace YourMap with your map name.
UGameplayStatics::OpenLevel(this, FName(TEXT("YourMap")), true,
    TEXT("listen?NetDriverDef=SteamGameNetDriver"));
```

- **Pro:** Keep LAN/IP as the default and let the same map host Steam or LAN sessions.
- **Con:** Your host code must select `SteamGameNetDriver` for Steam sessions.

### Test your setup

Have clients join through your game's Steam session flow. Look for `ChunkWorld: Steam terrain listener` in the host log. The bridge calls Porism's `RegisterProvider` at startup and supplies its own `PorismSteamTerrain` driver definition. You don't need to select that driver, edit Porism source, or add registration code to your game.
