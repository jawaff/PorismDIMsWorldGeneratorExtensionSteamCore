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
};
