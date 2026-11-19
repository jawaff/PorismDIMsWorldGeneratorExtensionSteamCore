// Copyright 2026 Spotted Loaf Studio

#pragma once

#include "ChunkWorldExtended/ChunkWorldNetTransport.h"
#include "Engine/NetworkDelegates.h"
#include "SteamCoreSocketsNetDriver.h"
#include "PorismSteamTerrainDriver.generated.h"

class FPorismTerrainStream;

/** Separate Steam-owned listener/dialer. UE packets and the reliable byte stream have one reader.
 * No actor replication, world login or second gameplay auth session runs on this driver. */
UCLASS(Transient)
class UPorismSteamTerrainDriver final : public USteamCoreSocketsNetDriver, public FNetworkNotify
{
	GENERATED_BODY()
public:
	static const FName Definition;
	/** Creates a world-owned named driver, only for the actual Steam gameplay world. */
	static UPorismSteamTerrainDriver* Create(UNetDriver& Gameplay);
	/** Publishes only a registered listener on the requested virtual port. */
	bool Listen(int32 Port, IChunkWorldNetTransportFactory::FOnConnectionAccepted Callback, FString& Endpoint);
	/** Queue Porism's token until the auxiliary handshake finishes; do not block the game thread. */
	TUniquePtr<IChunkWorldNetTransport> Dial(const FString& Endpoint);
	/** Detach admission before destroying; safe if called while the driver is ticking. */
	void Stop();

	void TickDispatch(float DeltaSeconds) override;
	void LowLevelSend(TSharedPtr<const FInternetAddr> Address, void* Data, int32 CountBits, FOutPacketTraits& Traits) override;
	void Shutdown() override;
	bool ShouldReplicateActor(AActor*) const override { return false; }
	bool ShouldForwardFunction(AActor*, UFunction*, void*) const override { return false; }

private:
	EAcceptConnection::Type NotifyAcceptingConnection() override;
	void NotifyAcceptedConnection(UNetConnection* Connection) override;
	bool NotifyAcceptingChannel(UChannel* Channel) override;
	void NotifyControlMessage(UNetConnection* Connection, uint8 Type, FInBunch& Bunch) override;

	struct FPeer
	{
		TWeakObjectPtr<UNetConnection> GameplayConnection;
		TSharedPtr<FPorismTerrainStream> Stream;
		uint32 Handle = 0;
		bool bHandshakeStarted = false;
		bool bChallengePassed = false;
		bool bHelloSent = false;
		bool bAdmitted = false;
		double Deadline = 0;
	};
	bool OwnsHandle(uint32 Handle, UNetConnection* Connection);
	bool HasGameplayPeer(const FPeer& Peer) const;
	void Receive(class ISteamNetworkingSockets& Sockets);
	void Dispatch(struct SteamNetworkingMessage_t& Message);
	void PumpPeer(UNetConnection& Connection, FPeer& Peer);
	TMap<TWeakObjectPtr<UNetConnection>, FPeer> Peers;
	TWeakObjectPtr<UNetDriver> GameplayDriver;
	IChunkWorldNetTransportFactory::FOnConnectionAccepted AcceptCallback;
	UNetConnection* ReplyConnection = nullptr; // Valid only on the synchronous inbound packet stack.
	uint32 LastAttempt = 0;
	bool bDialer = false;
	bool bStopped = false;
};
