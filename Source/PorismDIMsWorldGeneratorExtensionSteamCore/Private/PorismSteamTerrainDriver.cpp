// Copyright 2026 Spotted Loaf Studio

#include "PorismSteamTerrainDriver.h"

#include "PorismDIMsWorldGeneratorExtensionSteamCore.h"
#include "PorismSteamTerrainSockets.h"
#include "PorismSteamTerrainStream.h"
#include "Engine/Engine.h"
#include "Engine/NetConnection.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Misc/NetworkVersion.h"
#include "Net/DataChannel.h"
#include "PacketHandler.h"
#include "PacketHandlers/StatelessConnectHandlerComponent.h"
#include "SteamCoreSocket.h"
#include "SteamCoreSocketsSubsystem.h"

const FName UPorismSteamTerrainDriver::Definition(TEXT("PorismSteamTerrain"));

namespace
{
	bool Established(UNetConnection* Connection)
	{
		return IsValid(Connection) && Connection->GetConnectionState() == USOCK_Open
			&& Connection->Handler && Connection->Handler->IsFullyInitialized();
	}

	bool IsP2P(const TSharedPtr<const FInternetAddr>& Addr)
	{
		return Addr && Addr->IsValid() && Addr->GetProtocolType() == FNetworkProtocolTypes::SteamCoreSocketsP2P;
	}
}

UPorismSteamTerrainDriver* UPorismSteamTerrainDriver::Create(UNetDriver& Gameplay)
{
	UWorld* World = Gameplay.GetWorld();
	if (!GEngine || !World || World->GetNetDriver() != &Gameplay || World->bIsTearingDown
		|| !Gameplay.IsA<USteamCoreSocketsNetDriver>() || Gameplay.IsA<UPorismSteamTerrainDriver>()) return nullptr;

	auto* Defaults = GetDefault<UPorismSteamTerrainDriver>();
	if (!Defaults->IsAvailable() || Defaults->ArePacketHandlersDisabled()) return nullptr;
	const auto* Def = GEngine->NetDriverDefinitions.FindByPredicate([](const FNetDriverDefinition& D) { return D.DefName == Definition; });
	const FName ClassPath(*StaticClass()->GetPathName());
	if (!Def || Def->DriverClassName != ClassPath || Def->DriverClassNameFallback != ClassPath)
	{
		UE_LOG(LogPorismDIMsWorldGeneratorExtensionSteamCore, Error, TEXT("Missing exact Steam terrain driver definition; no fallback."));
		return nullptr;
	}

	auto* Driver = Cast<UPorismSteamTerrainDriver>(GEngine->CreateNetDriver(World, Definition));
	if (Driver)
	{
		Driver->GameplayDriver = &Gameplay;
		Driver->bSkipServerReplicateActors = true;
		Driver->bSkipClearVoicePackets = true;
		Driver->EnableExecuteRPCFunctions(false);
		Driver->EnablePreReplication(false);
		Driver->SetWorld(World);
	}
	return Driver;
}

bool UPorismSteamTerrainDriver::Listen(int32 Port, IChunkWorldNetTransportFactory::FOnConnectionAccepted Callback, FString& Endpoint)
{
	Endpoint.Reset();
	auto* Sub = static_cast<FSteamCoreSocketsSubsystem*>(GetSocketSubsystem());
	const auto GameAddr = GameplayDriver->GetLocalAddr();
	if (!Callback || !Sub || !Sub->IsLoggedInToSteam() || !ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)
		|| GameplayDriver->ServerConnection || !IsP2P(GameAddr) || Port == GameAddr->GetPort()) return false;

	const FString Identity = GameAddr->ToString(false);
	AcceptCallback = MoveTemp(Callback);
	return PorismSteamTerrainSockets::Listen([&](int32 ExactPort)
	{
		FURL Url;
		Url.Host = TEXT("steam.") + Identity;
		Url.Port = ExactPort;
		FString Error;
		if (!InitListen(this, Url, false, Error) || !m_Socket || m_bIsDelayedNetworkAccess
			|| !ConnectionlessHandler || !StatelessConnectComponent.IsValid() || !m_Socket->m_PollGroup
			|| !IsP2P(LocalAddr) || LocalAddr->GetPort() != ExactPort || LocalAddr->ToString(false) != Identity) return false;
		const auto* Registered = Sub->GetSocketInfo(m_Socket->m_InternalHandle);
		return Registered && !Registered->IsMarkedForDeletion() && Registered->m_NetDriver.Get() == this
			&& Registered->m_Socket == m_Socket && !Registered->m_Parent && m_Socket->m_bIsListenSocket
			&& !(m_Socket->m_SendMode & k_nSteamNetworkingSend_Reliable);
	}, Port, Identity, Endpoint);
}

TUniquePtr<IChunkWorldNetTransport> UPorismSteamTerrainDriver::Dial(const FString& Endpoint)
{
	UNetConnection* Game = GameplayDriver->ServerConnection;
	if (!Established(Game) || !IsP2P(Game->GetRemoteAddr())) return nullptr;
	const auto GameAddr = Game->GetRemoteAddr();
	const FString Identity = GameAddr->ToString(false);
	int32 Port = 0;
	if (!PorismSteamTerrainSockets::PeerPort(Endpoint, Identity, GameAddr->GetPort(), Port)) return nullptr;

	FURL Url;
	Url.Host = TEXT("steam.") + Identity;
	Url.Port = Port;
	bDialer = true;
	FString Error;
	if (!InitConnect(this, Url, Error) || !ServerConnection || !m_Socket || !ServerConnection->Handler
		|| !ServerConnection->StatelessConnectComponent.IsValid() || !OwnsHandle(m_Socket->m_InternalHandle, ServerConnection)
		|| (m_Socket->m_SendMode & k_nSteamNetworkingSend_Reliable)) return nullptr;

	auto& Peer = Peers.Add(ServerConnection.Get());
	Peer.GameplayConnection = Game;
	Peer.Handle = m_Socket->m_InternalHandle;
	Peer.Stream = MakeShared<FPorismTerrainStream>();
	Peer.Deadline = FPlatformTime::Seconds() + InitialConnectTimeout;
	return MakeUnique<FPorismTerrainTransport>(Peer.Stream.ToSharedRef());
}

EAcceptConnection::Type UPorismSteamTerrainDriver::NotifyAcceptingConnection()
{
	return !bStopped && AcceptCallback && GameplayDriver.IsValid() && LastAttempt != MAX_uint32
		&& Peers.Num() < 64 + GameplayDriver->ClientConnections.Num() ? EAcceptConnection::Accept : EAcceptConnection::Reject;
}

void UPorismSteamTerrainDriver::NotifyAcceptedConnection(UNetConnection* Connection)
{
	const auto SteamAddr = Connection->GetRemoteAddr();
	auto& Peer = Peers.Add(Connection);
	Peer.Stream = MakeShared<FPorismTerrainStream>();
	Peer.Deadline = FPlatformTime::Seconds() + InitialConnectTimeout;
	// SteamCore calls this before AddClientConnection. Give Unreal's bookkeeping and cookie a key
	// unique to this attempt: Steam endpoints compare equal across ports and overlapping reconnects.
	// This key never leaves the packet handler as a network destination.
	auto Key = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->CreateInternetAddr();
	Key->SetIp(++LastAttempt);
	Key->SetPort(LocalAddr->GetPort());
	Connection->RemoteAddr = Key;

	if (!IsP2P(SteamAddr) || !GameplayDriver.IsValid()) return;
	for (UNetConnection* Game : GameplayDriver->ClientConnections)
	{
		if (Established(Game) && Game->PlayerController && IsP2P(Game->GetRemoteAddr())
			&& Game->GetRemoteAddr()->ToString(false) == SteamAddr->ToString(false))
		{
			if (Peer.GameplayConnection.IsValid()) { Peer.GameplayConnection.Reset(); return; }
			Peer.GameplayConnection = Game;
		}
	}
}

bool UPorismSteamTerrainDriver::HasGameplayPeer(const FPeer& Peer) const
{
	const auto* Game = GameplayDriver.Get();
	auto* Connection = Peer.GameplayConnection.Get();
	return Game && GetWorld() && GetWorld()->GetNetDriver() == Game && Established(Connection) && Connection->GetDriver() == Game;
}

bool UPorismSteamTerrainDriver::OwnsHandle(uint32 Handle, UNetConnection* Connection)
{
	if (!Handle || !m_Socket || !Connection || Connection->GetDriver() != this) return false;
	auto* Sub = static_cast<FSteamCoreSocketsSubsystem*>(GetSocketSubsystem());
	const auto* R = Sub ? Sub->GetSocketInfo(Handle) : nullptr;
	if (!R || R->IsMarkedForDeletion() || R->m_NetDriver.Get() != this || !R->m_Socket
		|| R->m_Socket->m_InternalHandle != Handle || R->m_Socket->m_bIsListenSocket) return false;
	return bDialer ? ServerConnection == Connection && R->m_Socket == m_Socket && !R->m_Parent
		: R->m_Parent == m_Socket && R->m_Socket != m_Socket && FindClientConnectionForHandle(Handle) == Connection;
}

void UPorismSteamTerrainDriver::LowLevelSend(TSharedPtr<const FInternetAddr> Address, void* Data, int32 CountBits, FOutPacketTraits&)
{
	// Only a synchronous, already-framed engine reply is routable. An address alone has no authority.
	// Do not call Super (shared address lookup) or the connection's packet handler (double framing).
	auto* Peer = Peers.Find(ReplyConnection);
	if (bStopped || bDialer || !Peer || !ReplyConnection || !Data || CountBits <= 0 || CountBits > MAX_PACKET_SIZE * 8
		|| !PorismSteamTerrainSockets::MatchesReplyAttempt(ReplyConnection->GetRemoteAddr().Get(), Address.Get())
		|| !ConnectionlessHandler || !ConnectionlessHandler->GetRawSend() || !HasGameplayPeer(*Peer)
		|| !OwnsHandle(Peer->Handle, ReplyConnection)) return;
	if (auto* Net = FSteamCoreSocketsSubsystem::GetSteamSocketsInterface())
	{
		Net->SendMessageToConnection(Peer->Handle, Data, (CountBits + 7) / 8, k_nSteamNetworkingSend_UnreliableNoNagle, nullptr);
	}
}

void UPorismSteamTerrainDriver::Receive(ISteamNetworkingSockets& Net)
{
	// Sole consumer of the terrain poll group (or outgoing connection). Do not call SteamCore's
	// TickDispatch as well: it discards the source handle before invoking the connectionless handler.
	SteamNetworkingMessage_t* Batch[32];
	for (int32 I = 0; I < 4; ++I)
	{
		const int32 Count = bDialer ? Net.ReceiveMessagesOnConnection(m_Socket->m_InternalHandle, Batch, UE_ARRAY_COUNT(Batch))
			: Net.ReceiveMessagesOnPollGroup(m_Socket->m_PollGroup, Batch, UE_ARRAY_COUNT(Batch));
		if (Count < 0) { Stop(); return; }
		for (int32 N = 0; N < Count; ++N)
		{
			if (!GetPendingDestruction()) Dispatch(*Batch[N]);
			Batch[N]->Release();
		}
		if (Count < UE_ARRAY_COUNT(Batch) || GetPendingDestruction()) return;
	}
}

void UPorismSteamTerrainDriver::Dispatch(SteamNetworkingMessage_t& Msg)
{
	UNetConnection* Connection = bDialer ? ServerConnection.Get() : FindClientConnectionForHandle(Msg.m_conn);
	FPeer* Peer = Peers.Find(Connection);
	if (!Peer || !Peer->Stream->IsOpen() || !HasGameplayPeer(*Peer) || !OwnsHandle(Msg.m_conn, Connection)
		|| (Peer->Handle && Peer->Handle != Msg.m_conn) || Connection->GetConnectionState() == USOCK_Closed) return;
	Peer->Handle = Msg.m_conn;
	if (Msg.m_nFlags & k_nSteamNetworkingSend_Reliable)
	{
		// This driver reserves reliable Steam messages for the Porism byte stream; UE sends unreliably.
		if (!Peer->bAdmitted || !Peer->Stream->ReceiveBlock(static_cast<const uint8*>(Msg.m_pData), Msg.m_cbSize))
			Peer->Stream->Close();
		return;
	}
	if (!Msg.m_pData || Msg.m_cbSize <= 0 || Msg.m_cbSize > Connection->MaxPacket) return;

	TGuardValue<UNetConnection*> ReplyScope(ReplyConnection, bDialer ? nullptr : Connection);
	if (!bDialer && !Peer->bChallengePassed)
	{
		auto Stateless = StatelessConnectComponent.Pin();
		auto Bound = Connection->StatelessConnectComponent.Pin();
		if (!Stateless || !Bound || !ConnectionlessHandler || !Connection->Handler) return;
		FReceivedPacketView Packet;
		Packet.Address = Connection->GetRemoteAddr();
		Packet.DataView = {static_cast<uint8*>(Msg.m_pData), Msg.m_cbSize, ECountUnits::Bytes};
		bool bRestarted = false;
		if (ConnectionlessHandler->IncomingConnectionless(Packet) == EIncomingResult::Success
			&& Stateless->HasPassedChallenge(Packet.Address, bRestarted) && !bRestarted)
		{
			int32 ServerSequence, ClientSequence;
			Stateless->GetChallengeSequence(ServerSequence, ClientSequence);
			Connection->InitSequence(ClientSequence, ServerSequence);
			Bound->InitFromConnectionless(Stateless.Get()); // Retransmitted response must ACK this attempt, not the last peer.
			Peer->bChallengePassed = true;
			Connection->Handler->BeginHandshaking();
		}
		Stateless->ResetChallengeData();
		return; // A pending peer may supply handshake packets only.
	}
	Connection->ReceivedRawPacket(Msg.m_pData, Msg.m_cbSize);
}

bool UPorismSteamTerrainDriver::NotifyAcceptingChannel(UChannel* Channel)
{
	return Channel && Channel->ChIndex == 0 && Channel->ChName == NAME_Control;
}

void UPorismSteamTerrainDriver::NotifyControlMessage(UNetConnection* Connection, uint8 Type, FInBunch& Bunch)
{
	auto* Peer = Peers.Find(Connection);
	if (!Peer || Peer->bAdmitted || !Established(Connection) || !HasGameplayPeer(*Peer)) return;
	if (bDialer && Peer->bHelloSent && Type == NMT_BeaconWelcome)
	{
		Peer->bAdmitted = true;
	}
	else if (!bDialer && Peer->bChallengePassed && Type == NMT_Hello && AcceptCallback)
	{
		uint8 Endian = 0;
		uint32 Version = 0;
		FString Token;
		EEngineNetworkRuntimeFeatures Features{};
		if (!FNetControlMessage<NMT_Hello>::Receive(Bunch, Endian, Version, Token, Features)
			|| !FNetworkVersion::IsNetworkCompatible(FNetworkVersion::GetLocalNetworkVersion(), Version)
			|| !FNetworkVersion::AreNetworkRuntimeFeaturesCompatible(GetNetworkRuntimeFeatures(), Features)) return;
		Peer->bAdmitted = true;
		Connection->SetClientLoginState(EClientLoginState::Welcomed);
		FNetControlMessage<NMT_BeaconWelcome>::Send(Connection);
		Connection->FlushNet();
		if (!AcceptCallback(MakeUnique<FPorismTerrainTransport>(Peer->Stream.ToSharedRef()))) Stop();
	}
}

void UPorismSteamTerrainDriver::PumpPeer(UNetConnection& Connection, FPeer& Peer)
{
	if (!Peer.Stream->IsOpen() || !HasGameplayPeer(Peer) || (!Peer.bAdmitted && FPlatformTime::Seconds() > Peer.Deadline)
		|| Connection.GetConnectionState() == USOCK_Closed || (Peer.Handle && !OwnsHandle(Peer.Handle, &Connection)))
	{
		Peer.Stream->Close();
		if (bDialer) Stop();
		else Connection.Close();
		return;
	}
	if (Connection.GetConnectionState() != USOCK_Open) return;
	if (bDialer && !Peer.bHandshakeStarted)
	{
		Peer.bHandshakeStarted = true;
		Connection.Handler->BeginHandshaking();
	}
	if (bDialer && !Peer.bHelloSent && Established(&Connection))
	{
		Peer.bHelloSent = true;
		uint8 Endian = uint8(PLATFORM_LITTLE_ENDIAN);
		uint32 Version = FNetworkVersion::GetLocalNetworkVersion();
		FString Token;
		auto Features = GetNetworkRuntimeFeatures();
		FNetControlMessage<NMT_Hello>::Send(&Connection, Endian, Version, Token, Features);
		Connection.FlushNet();
	}
	if (!Peer.bAdmitted || !Peer.Handle) return;
	auto* Net = FSteamCoreSocketsSubsystem::GetSteamSocketsInterface();
	Peer.Stream->Flush([Net, Handle = Peer.Handle](TConstArrayView<uint8> Bytes)
	{
		using ESend = FPorismTerrainStream::ESend;
		if (!Net) return ESend::Failed;
		const EResult Result = Net->SendMessageToConnection(Handle, Bytes.GetData(), Bytes.Num(), k_nSteamNetworkingSend_Reliable, nullptr);
		return Result == k_EResultOK ? ESend::Sent : Result == k_EResultLimitExceeded ? ESend::Busy : ESend::Failed;
	});
}

void UPorismSteamTerrainDriver::TickDispatch(float DeltaSeconds)
{
	if (bStopped) { Stop(); return; }
	UNetDriver::TickDispatch(DeltaSeconds);
	auto* Net = FSteamCoreSocketsSubsystem::GetSteamSocketsInterface();
	if (!Net || !m_Socket || !GameplayDriver.IsValid() || (bDialer && !ServerConnection)) { Stop(); return; }
	Receive(*Net);
	if (GetPendingDestruction()) return;
	for (auto It = Peers.CreateIterator(); It; ++It)
	{
		auto* Connection = It.Key().Get();
		if (!Connection || Connection->GetDriver() != this)
		{
			It.Value().Stream->Close();
			It.RemoveCurrent();
		}
		else PumpPeer(*Connection, It.Value());
	}
}

void UPorismSteamTerrainDriver::Stop()
{
	AcceptCallback = nullptr;
	if (GetWorld() && GEngine && GEngine->GetWorldContextFromWorld(GetWorld()))
		GEngine->DestroyNamedNetDriver(GetWorld(), NetDriverName);
}

void UPorismSteamTerrainDriver::Shutdown()
{
	if (bStopped) return;
	bStopped = true;
	AcceptCallback = nullptr;
	for (auto& Pair : Peers) Pair.Value.Stream->Close();
	Peers.Empty();
	// Named driver destruction can follow a rejected endpoint, before InitBase created metrics or sockets.
	if (!GetMetrics()) return;
	// Retire the client connection while its SteamCore socket record is still present.
	if (ServerConnection) ServerConnection->CleanUp();
	if (m_Socket && !m_Socket->m_InternalHandle)
	{
		if (auto* Sub = GetSocketSubsystem()) Sub->DestroySocket(m_Socket);
		m_Socket = nullptr;
	}
	Super::Shutdown();
}
