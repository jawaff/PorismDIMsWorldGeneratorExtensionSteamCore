// Copyright 2026 Spotted Loaf Studio

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

DECLARE_LOG_CATEGORY_EXTERN(LogPorismDIMsWorldGeneratorExtensionSteamCore, Log, All);

/**
 * Optional runtime bridge that routes Porism's auxiliary transport over Steam relay-backed sockets.
 */
class FPorismDIMsWorldGeneratorExtensionSteamCoreModule : public IModuleInterface
{
public:
	/**
	 * Registers the Steam transport factory once Porism and SteamCore are available.
	 */
	virtual void StartupModule() override;

	/**
	 * Releases the project-owned Steam transport factory on shutdown.
	 */
	virtual void ShutdownModule() override;

	/** Host policy for newly created Steam listeners, set on the game thread before hosting.
	 * Zero keeps the provider port; values outside 1..65535 fail the bind. Never overrides a joining client's endpoint. */
	void SetHostVirtualPortOverride(int32 Port)
	{
		check(IsInGameThread());
		HostVirtualPortOverride = Port;
	}

private:
	TWeakObjectPtr<class UNetDriver> GameplayForFactory;
	int32 HostVirtualPortOverride = 0;
	bool bInstalledSteamTransportFactory = false;
};
