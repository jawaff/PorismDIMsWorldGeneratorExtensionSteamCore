// Copyright 2026 Spotted Loaf Studio

#include "PorismDIMsWorldGeneratorExtensionSteamCore.h"

#include "ChunkWorldExtended/ChunkWorldNetworkSubsystem.h"
#include "Engine/World.h"
#include "Misc/ConfigCacheIni.h"
#include "PorismSteamTerrainDriver.h"
#include "PorismSteamTerrainSockets.h"

DEFINE_LOG_CATEGORY(LogPorismDIMsWorldGeneratorExtensionSteamCore);

namespace
{
	class FPorismSteamSocketsTransportFactory final : public IChunkWorldNetTransportFactory
	{
	public:
		FPorismSteamSocketsTransportFactory(UNetDriver* Driver, int32 Port) : Gameplay(Driver), VirtualPort(Port) {}
		~FPorismSteamSocketsTransportFactory() override { StopListening(); }

		bool StartListening(FOnConnectionAccepted Callback) override
		{
			if (Listener.IsValid() || !Gameplay.IsValid() || VirtualPort <= 0 || VirtualPort > MAX_uint16) return false;
			auto* Driver = UPorismSteamTerrainDriver::Create(*Gameplay.Get());
			if (!Driver) return false;
			if (!Driver->Listen(VirtualPort, MoveTemp(Callback), Address))
			{
				Driver->Stop();
				return false;
			}
			Listener = Driver;
			return true;
		}

		void StopListening() override
		{
			if (auto* Driver = Listener.Get()) Driver->Stop();
			Listener.Reset();
			Address.Reset();
		}

		FString GetConnectionAddress() const override { return Address; }

		TUniquePtr<IChunkWorldNetTransport> CreateConnection(const FString& Endpoint) override
		{
			if (VirtualPort != 0 || !Gameplay.IsValid()) return nullptr;
			auto* Driver = UPorismSteamTerrainDriver::Create(*Gameplay.Get());
			if (!Driver) return nullptr;
			auto Stream = Driver->Dial(Endpoint);
			if (!Stream) Driver->Stop();
			return Stream; // Porism discards the client factory immediately. The world owns the driver.
		}
	private:
		TWeakObjectPtr<UNetDriver> Gameplay;
		TWeakObjectPtr<UPorismSteamTerrainDriver> Listener;
		FString Address;
		int32 VirtualPort;
	};
}

void FPorismDIMsWorldGeneratorExtensionSteamCoreModule::StartupModule()
{
	bool bUseSteamNetworking = false;
	if (!GConfig || !GConfig->GetBool(TEXT("OnlineSubsystemSteamCore"), TEXT("bUseSteamNetworking"), bUseSteamNetworking, GEngineIni)
		|| !bUseSteamNetworking) return;

	FModuleManager::LoadModuleChecked<IModuleInterface>("PorismDIMsWorldGenerator");
	FModuleManager::LoadModuleChecked<IModuleInterface>("SteamCoreSockets");
	UChunkWorldNetworkSubsystem::RegisterProvider(TEXT("Steam"),
		[this](const UNetDriver& Driver)
		{
			// Porism resolves the active gameplay driver immediately before creating its factory, on the game thread.
			GameplayForFactory.Reset();
			if (!Driver.IsA<USteamCoreSocketsNetDriver>() || Driver.IsA<UPorismSteamTerrainDriver>()) return false;
			GameplayForFactory = const_cast<UNetDriver*>(&Driver);
			return true;
		},
		[this](int32 Port) -> TSharedPtr<IChunkWorldNetTransportFactory>
		{
			UNetDriver* Gameplay = GameplayForFactory.Get();
			GameplayForFactory.Reset();
			if (!Gameplay || !Gameplay->GetWorld() || Gameplay->GetWorld()->GetNetDriver() != Gameplay) return nullptr;
			return MakeShared<FPorismSteamSocketsTransportFactory>(Gameplay, PorismSteamTerrainSockets::ListenPort(Port, HostVirtualPortOverride));
		});
	bInstalledSteamTransportFactory = true;
}

void FPorismDIMsWorldGeneratorExtensionSteamCoreModule::ShutdownModule()
{
	if (bInstalledSteamTransportFactory)
	{
		UChunkWorldNetworkSubsystem::UnregisterProvider(TEXT("Steam"));
		GameplayForFactory.Reset();
		bInstalledSteamTransportFactory = false;
	}
}

IMPLEMENT_MODULE(FPorismDIMsWorldGeneratorExtensionSteamCoreModule, PorismDIMsWorldGeneratorExtensionSteamCore)
