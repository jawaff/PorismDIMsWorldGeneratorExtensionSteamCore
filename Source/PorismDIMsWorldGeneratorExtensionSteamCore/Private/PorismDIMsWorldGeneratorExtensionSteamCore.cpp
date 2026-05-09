// Copyright 2026 Spotted Loaf Studio

#include "PorismDIMsWorldGeneratorExtensionSteamCore.h"

#include "ChunkWorldExtended/ChunkWorldNetManager.h"
#include "ChunkWorldExtended/ChunkWorldNetTransport.h"
#include "Engine/Engine.h"
#include "Misc/ConfigCacheIni.h"
#include "Engine/World.h"
#include "HAL/Event.h"
#include "HAL/PlatformProcess.h"
#include "Misc/ScopeLock.h"
#include "SteamCorePro/Steam.h"
#include "SteamCoreSharedTypes.h"
#include "SteamCoreSocketsSubsystem.h"
#include "SteamCoreSocketsTypes.h"

DEFINE_LOG_CATEGORY(LogPorismDIMsWorldGeneratorExtensionSteamCore);

namespace
{
	static constexpr int32 DefaultSteamVirtualPort = 7878;
	static constexpr float ConnectTimeoutSeconds = 10.0f;
	static constexpr float PollSleepSeconds = 0.01f;
	static bool bInstalledSteamTransportFactory = false;

	class FPorismSteamSocketsTransportFactory;

	struct FParsedSteamConnectToken
	{
		uint64 SteamId = 0;
		int32 VirtualPort = 0;

		bool IsValid() const
		{
			return SteamId != 0 && VirtualPort >= 0;
		}
	};

	static bool IsAllDigits(const FString& Value)
	{
		if (Value.IsEmpty())
		{
			return false;
		}

		for (const TCHAR Character : Value)
		{
			if (!FChar::IsDigit(Character))
			{
				return false;
			}
		}

		return true;
	}

	static bool ParseSteamIdentitySegment(const FString& Segment, uint64& OutSteamId)
	{
		FString Normalized = Segment;
		Normalized.RemoveFromStart(TEXT("steam."));
		if (!IsAllDigits(Normalized))
		{
			return false;
		}

		OutSteamId = FCString::Strtoui64(*Normalized, nullptr, 10);
		return OutSteamId != 0;
	}

	static FString ExtractSteamConnectToken(const FString& RawAddress)
	{
		TArray<FString> Segments;
		RawAddress.ParseIntoArray(Segments, TEXT(":"), false);
		for (int32 Index = Segments.Num() - 2; Index >= 0; --Index)
		{
			uint64 SteamId = 0;
			if (ParseSteamIdentitySegment(Segments[Index], SteamId) && IsAllDigits(Segments[Index + 1]))
			{
				return FString::Printf(TEXT("%llu:%s"), SteamId, *Segments[Index + 1]);
			}
		}

		uint64 SteamId = 0;
		if (ParseSteamIdentitySegment(RawAddress, SteamId))
		{
			return FString::Printf(TEXT("%llu"), SteamId);
		}

		return RawAddress;
	}

	static bool ParseSteamConnectToken(const FString& RawAddress, FParsedSteamConnectToken& OutToken)
	{
		const FString ConnectToken = ExtractSteamConnectToken(RawAddress);
		FString SteamIdString;
		FString VirtualPortString;
		if (!ConnectToken.Split(TEXT(":"), &SteamIdString, &VirtualPortString, ESearchCase::CaseSensitive, ESearchDir::FromEnd))
		{
			return false;
		}

		uint64 SteamId = 0;
		if (!ParseSteamIdentitySegment(SteamIdString, SteamId) || !IsAllDigits(VirtualPortString))
		{
			return false;
		}

		OutToken.SteamId = SteamId;
		OutToken.VirtualPort = FCString::Atoi(*VirtualPortString);
		return OutToken.IsValid();
	}

	static int32 ResolveSteamVirtualPort()
	{
		if (GEngine)
		{
			for (const FWorldContext& WorldContext : GEngine->GetWorldContexts())
			{
				if (WorldContext.WorldType != EWorldType::Game && WorldContext.WorldType != EWorldType::PIE)
				{
					continue;
				}

				if (UWorld* World = WorldContext.World())
				{
					const int32 BasePort = World->URL.Port + 100;
					if (BasePort > 0)
					{
						return BasePort;
					}
				}
			}
		}

		return DefaultSteamVirtualPort;
	}

	class FPorismSteamSocketsTransport final : public IChunkWorldNetTransport
	{
	public:
		explicit FPorismSteamSocketsTransport(const HSteamNetConnection InConnectionHandle)
			: ConnectionHandle(InConnectionHandle)
		{
		}

		virtual ~FPorismSteamSocketsTransport() override
		{
			Close();
		}

		void Register();
		void MarkDisconnected();

		bool WaitUntilConnected(const float TimeoutSeconds)
		{
			const double Deadline = FPlatformTime::Seconds() + TimeoutSeconds;
			while (FPlatformTime::Seconds() < Deadline)
			{
				const ESteamNetworkingConnectionState State = QueryConnectionState();
				if (State == k_ESteamNetworkingConnectionState_Connected)
				{
					bConnected = true;
					return true;
				}

				if (State == k_ESteamNetworkingConnectionState_ClosedByPeer ||
					State == k_ESteamNetworkingConnectionState_ProblemDetectedLocally ||
					State == k_ESteamNetworkingConnectionState_None)
				{
					MarkDisconnected();
					return false;
				}

				FPlatformProcess::SleepNoStats(PollSleepSeconds);
			}

			return false;
		}

		virtual void Close() override
		{
			FScopeLock Lock(&ReceiveMutex);
			if (ConnectionHandle == k_HSteamNetConnection_Invalid)
			{
				return;
			}

			Unregister();

			if (ISteamNetworkingSockets* SocketInterface = FSteamCoreSocketsSubsystem::GetSteamSocketsInterface())
			{
				SocketInterface->CloseConnection(ConnectionHandle, k_ESteamNetConnectionEnd_App_Generic, "Closed by Porism bridge", false);
			}

			ConnectionHandle = k_HSteamNetConnection_Invalid;
			bConnected = false;
			PendingBytes.Reset();
			PendingReadOffset = 0;
		}

		virtual bool Send(const uint8* Data, const int32 NumBytes, int32& OutBytesSent) override
		{
			OutBytesSent = 0;
			if (!IsConnected() || !WaitUntilConnected(ConnectTimeoutSeconds))
			{
				return false;
			}

			ISteamNetworkingSockets* SocketInterface = FSteamCoreSocketsSubsystem::GetSteamSocketsInterface();
			if (!SocketInterface)
			{
				return false;
			}

			const EResult SendResult = SocketInterface->SendMessageToConnection(
				ConnectionHandle,
				Data,
				NumBytes,
				k_nSteamNetworkingSend_Reliable,
				nullptr);

			if (SendResult == k_EResultOK)
			{
				OutBytesSent = NumBytes;
				return true;
			}

			if (SendResult == k_EResultNoConnection)
			{
				MarkDisconnected();
			}

			return false;
		}

		virtual bool Recv(uint8* Buffer, const int32 BufferSize, int32& OutBytesRead) override
		{
			OutBytesRead = 0;
			if (BufferSize <= 0)
			{
				return false;
			}

			while (IsConnected())
			{
				{
					FScopeLock Lock(&ReceiveMutex);
					if (HasQueuedBytes())
					{
						const int32 AvailableBytes = PendingBytes.Num() - PendingReadOffset;
						OutBytesRead = FMath::Min(BufferSize, AvailableBytes);
						FMemory::Memcpy(Buffer, PendingBytes.GetData() + PendingReadOffset, OutBytesRead);
						PendingReadOffset += OutBytesRead;
						if (PendingReadOffset >= PendingBytes.Num())
						{
							PendingBytes.Reset();
							PendingReadOffset = 0;
						}
						return OutBytesRead > 0;
					}
				}

				if (!PumpReceivedMessages())
				{
					FPlatformProcess::SleepNoStats(PollSleepSeconds);
				}
			}

			return false;
		}

		virtual bool WaitForData(const float TimeoutSeconds) override
		{
			if (!IsConnected())
			{
				return false;
			}

			const double Deadline = FPlatformTime::Seconds() + TimeoutSeconds;
			while (FPlatformTime::Seconds() < Deadline && IsConnected())
			{
				{
					FScopeLock Lock(&ReceiveMutex);
					if (HasQueuedBytes())
					{
						return true;
					}
				}

				if (!PumpReceivedMessages())
				{
					FPlatformProcess::SleepNoStats(PollSleepSeconds);
				}
			}

			return false;
		}

		virtual bool IsConnected() const override
		{
			return bConnected && ConnectionHandle != k_HSteamNetConnection_Invalid;
		}

	private:
		void Unregister();

		bool PumpReceivedMessages()
		{
			ISteamNetworkingSockets* SocketInterface = FSteamCoreSocketsSubsystem::GetSteamSocketsInterface();
			if (!SocketInterface || ConnectionHandle == k_HSteamNetConnection_Invalid)
			{
				MarkDisconnected();
				return false;
			}

			SteamNetworkingMessage_t* Message = nullptr;
			const int32 MessageCount = SocketInterface->ReceiveMessagesOnConnection(ConnectionHandle, &Message, 1);
			if (MessageCount > 0 && Message)
			{
				FScopeLock Lock(&ReceiveMutex);
				const int32 PreviousNum = PendingBytes.Num();
				PendingBytes.SetNumUninitialized(PreviousNum + static_cast<int32>(Message->GetSize()));
				FMemory::Memcpy(PendingBytes.GetData() + PreviousNum, Message->GetData(), Message->GetSize());
				Message->Release();
				return true;
			}

			if (MessageCount < 0)
			{
				MarkDisconnected();
			}

			return false;
		}

		bool HasQueuedBytes() const
		{
			return PendingReadOffset < PendingBytes.Num();
		}

		ESteamNetworkingConnectionState QueryConnectionState() const
		{
			ISteamNetworkingSockets* SocketInterface = FSteamCoreSocketsSubsystem::GetSteamSocketsInterface();
			if (!SocketInterface || ConnectionHandle == k_HSteamNetConnection_Invalid)
			{
				return k_ESteamNetworkingConnectionState_None;
			}

			SteamNetConnectionRealTimeStatus_t Status;
			int32 DummyLanes = 0;
			SteamNetConnectionRealTimeLaneStatus_t* DummyLaneData = nullptr;
			if (!SocketInterface->GetConnectionRealTimeStatus(ConnectionHandle, &Status, DummyLanes, DummyLaneData))
			{
				return k_ESteamNetworkingConnectionState_None;
			}

			return Status.m_eState;
		}

		mutable FCriticalSection ReceiveMutex;
		HSteamNetConnection ConnectionHandle = k_HSteamNetConnection_Invalid;
		TArray<uint8> PendingBytes;
		int32 PendingReadOffset = 0;
		std::atomic<bool> bConnected { true };
	};

	class FPorismSteamSocketsEventRouter final
	{
	public:
		static FPorismSteamSocketsEventRouter& Get()
		{
			static FPorismSteamSocketsEventRouter Router;
			return Router;
		}

		void Startup()
		{
			FScopeLock Lock(&Mutex);
			if (bRegistered)
			{
				return;
			}

			if (IsRunningDedicatedServer())
			{
				ServerConnectionStatusCallback.Register(this, &FPorismSteamSocketsEventRouter::OnConnectionStatusChanged);
			}
			else
			{
				ClientConnectionStatusCallback.Register(this, &FPorismSteamSocketsEventRouter::OnConnectionStatusChanged);
			}

			bRegistered = true;
		}

		void Shutdown()
		{
			FScopeLock Lock(&Mutex);
			if (!bRegistered)
			{
				return;
			}

			if (IsRunningDedicatedServer())
			{
				ServerConnectionStatusCallback.Unregister();
			}
			else
			{
				ClientConnectionStatusCallback.Unregister();
			}

			Factories.Reset();
			Transports.Reset();
			bRegistered = false;
		}

		void RegisterFactory(const HSteamListenSocket ListenSocket, FPorismSteamSocketsTransportFactory* Factory)
		{
			FScopeLock Lock(&Mutex);
			Factories.Add(ListenSocket, Factory);
		}

		void UnregisterFactory(const HSteamListenSocket ListenSocket)
		{
			FScopeLock Lock(&Mutex);
			Factories.Remove(ListenSocket);
		}

		void RegisterTransport(const HSteamNetConnection ConnectionHandle, FPorismSteamSocketsTransport* Transport)
		{
			FScopeLock Lock(&Mutex);
			Transports.Add(ConnectionHandle, Transport);
		}

		void UnregisterTransport(const HSteamNetConnection ConnectionHandle)
		{
			FScopeLock Lock(&Mutex);
			Transports.Remove(ConnectionHandle);
		}

	private:
		void OnConnectionStatusChanged(SteamNetConnectionStatusChangedCallback_t* Message);

		FCriticalSection Mutex;
		TMap<HSteamListenSocket, FPorismSteamSocketsTransportFactory*> Factories;
		TMap<HSteamNetConnection, FPorismSteamSocketsTransport*> Transports;
		CCallbackManual<FPorismSteamSocketsEventRouter, SteamNetConnectionStatusChangedCallback_t> ClientConnectionStatusCallback;
		CCallbackManual<FPorismSteamSocketsEventRouter, SteamNetConnectionStatusChangedCallback_t, true> ServerConnectionStatusCallback;
		bool bRegistered = false;
	};

	class FPorismSteamSocketsTransportFactory final
		: public IChunkWorldNetTransportFactory
		, public TSharedFromThis<FPorismSteamSocketsTransportFactory>
	{
	public:
		virtual ~FPorismSteamSocketsTransportFactory() override
		{
			StopListening();
		}

		virtual bool StartListening(FOnConnectionAccepted Callback) override
		{
			StopListening();
			AcceptCallback = MoveTemp(Callback);

			FSteamCoreSocketsSubsystem* SocketSubsystem = static_cast<FSteamCoreSocketsSubsystem*>(ISocketSubsystem::Get(STEAMCORE_SOCKETS_SUBSYSTEM));
			ISteamNetworkingSockets* SocketInterface = FSteamCoreSocketsSubsystem::GetSteamSocketsInterface();
			if (!SocketSubsystem || !SocketInterface)
			{
				UE_LOG(LogPorismDIMsWorldGeneratorExtensionSteamCore, Error, TEXT("Porism Steam bridge could not access Steam networking sockets."));
				return false;
			}

			const TSharedPtr<FInternetAddr> IdentityAddress = SocketSubsystem->GetIdentityAddress();
			if (!IdentityAddress.IsValid())
			{
				UE_LOG(LogPorismDIMsWorldGeneratorExtensionSteamCore, Error, TEXT("Porism Steam bridge could not resolve a local Steam identity."));
				return false;
			}

			VirtualPort = ResolveSteamVirtualPort();
			ListenSocket = SocketInterface->CreateListenSocketP2P(VirtualPort, 0, nullptr);
			if (ListenSocket == k_HSteamListenSocket_Invalid)
			{
				UE_LOG(LogPorismDIMsWorldGeneratorExtensionSteamCore, Error, TEXT("Porism Steam bridge failed to create a P2P listen socket on virtual port %d."), VirtualPort);
				return false;
			}

			ConnectionAddress = FString::Printf(TEXT("%s:%d"), *IdentityAddress->ToString(false), VirtualPort);
			FPorismSteamSocketsEventRouter::Get().RegisterFactory(ListenSocket, this);
			UE_LOG(LogPorismDIMsWorldGeneratorExtensionSteamCore, Log, TEXT("Porism Steam bridge listening on %s."), *ConnectionAddress);
			return true;
		}

		virtual void StopListening() override
		{
			if (ListenSocket == k_HSteamListenSocket_Invalid)
			{
				return;
			}

			FPorismSteamSocketsEventRouter::Get().UnregisterFactory(ListenSocket);
			if (ISteamNetworkingSockets* SocketInterface = FSteamCoreSocketsSubsystem::GetSteamSocketsInterface())
			{
				SocketInterface->CloseListenSocket(ListenSocket);
			}

			ListenSocket = k_HSteamListenSocket_Invalid;
			ConnectionAddress.Empty();
			VirtualPort = 0;
		}

		virtual FString GetConnectionAddress() const override
		{
			return ConnectionAddress;
		}

		virtual TUniquePtr<IChunkWorldNetTransport> CreateConnection(const FString& Address) override
		{
			FParsedSteamConnectToken ParsedToken;
			if (!ParseSteamConnectToken(Address, ParsedToken))
			{
				UE_LOG(LogPorismDIMsWorldGeneratorExtensionSteamCore, Error, TEXT("Porism Steam bridge could not parse Steam connect token from '%s'."), *Address);
				return nullptr;
			}

			ISteamNetworkingSockets* SocketInterface = FSteamCoreSocketsSubsystem::GetSteamSocketsInterface();
			if (!SocketInterface)
			{
				return nullptr;
			}

			SteamNetworkingIdentity Identity;
			Identity.Clear();
			Identity.SetSteamID64(ParsedToken.SteamId);

			const HSteamNetConnection ConnectionHandle = SocketInterface->ConnectP2P(Identity, ParsedToken.VirtualPort, 0, nullptr);
			if (ConnectionHandle == k_HSteamNetConnection_Invalid)
			{
				UE_LOG(LogPorismDIMsWorldGeneratorExtensionSteamCore, Error, TEXT("Porism Steam bridge failed to open P2P connection to %llu:%d."), ParsedToken.SteamId, ParsedToken.VirtualPort);
				return nullptr;
			}

			TUniquePtr<FPorismSteamSocketsTransport> Transport = MakeUnique<FPorismSteamSocketsTransport>(ConnectionHandle);
			Transport->Register();
			if (!Transport->WaitUntilConnected(ConnectTimeoutSeconds))
			{
				UE_LOG(LogPorismDIMsWorldGeneratorExtensionSteamCore, Error, TEXT("Porism Steam bridge timed out connecting to %llu:%d."), ParsedToken.SteamId, ParsedToken.VirtualPort);
				Transport->Close();
				return nullptr;
			}

			return Transport;
		}

		void HandleIncomingConnection(const SteamNetConnectionStatusChangedCallback_t& Message)
		{
			ISteamNetworkingSockets* SocketInterface = FSteamCoreSocketsSubsystem::GetSteamSocketsInterface();
			if (!SocketInterface)
			{
				return;
			}

			const EResult AcceptResult = SocketInterface->AcceptConnection(Message.m_hConn);
			if (AcceptResult != k_EResultOK)
			{
				SocketInterface->CloseConnection(Message.m_hConn, static_cast<int32>(AcceptResult), "Porism bridge rejected connection", false);
				return;
			}

			TUniquePtr<FPorismSteamSocketsTransport> Transport = MakeUnique<FPorismSteamSocketsTransport>(Message.m_hConn);
			Transport->Register();

			const bool bKeepListening = AcceptCallback ? AcceptCallback(MoveTemp(Transport)) : true;
			if (!bKeepListening)
			{
				StopListening();
			}
		}

	private:
		FOnConnectionAccepted AcceptCallback;
		HSteamListenSocket ListenSocket = k_HSteamListenSocket_Invalid;
		FString ConnectionAddress;
		int32 VirtualPort = 0;
	};

	void FPorismSteamSocketsEventRouter::OnConnectionStatusChanged(SteamNetConnectionStatusChangedCallback_t* Message)
	{
		if (!Message)
		{
			return;
		}

		FPorismSteamSocketsTransportFactory* Factory = nullptr;
		FPorismSteamSocketsTransport* Transport = nullptr;
		{
			FScopeLock Lock(&Mutex);
			Factory = Factories.FindRef(Message->m_info.m_hListenSocket);
			Transport = Transports.FindRef(Message->m_hConn);
		}

		if (Message->m_eOldState == k_ESteamNetworkingConnectionState_None &&
			Message->m_info.m_eState == k_ESteamNetworkingConnectionState_Connecting &&
			Factory)
		{
			Factory->HandleIncomingConnection(*Message);
			return;
		}

		if (Transport &&
			(Message->m_info.m_eState == k_ESteamNetworkingConnectionState_ClosedByPeer ||
				Message->m_info.m_eState == k_ESteamNetworkingConnectionState_ProblemDetectedLocally))
		{
			Transport->MarkDisconnected();
		}
	}

	void FPorismSteamSocketsTransport::Register()
	{
		FPorismSteamSocketsEventRouter::Get().RegisterTransport(ConnectionHandle, this);
	}

	void FPorismSteamSocketsTransport::Unregister()
	{
		FPorismSteamSocketsEventRouter::Get().UnregisterTransport(ConnectionHandle);
	}

	void FPorismSteamSocketsTransport::MarkDisconnected()
	{
		bConnected = false;
	}
}

void FPorismDIMsWorldGeneratorExtensionSteamCoreModule::StartupModule()
{
	bool bUseSteamNetworking = false;
	if (GConfig)
	{
		GConfig->GetBool(TEXT("OnlineSubsystemSteamCore"), TEXT("bUseSteamNetworking"), bUseSteamNetworking, GEngineIni);
	}

	if (!bUseSteamNetworking)
	{
		UE_LOG(LogPorismDIMsWorldGeneratorExtensionSteamCore, Log, TEXT("Porism Steam transport bridge skipped because Steam networking is disabled in config."));
		return;
	}

	FModuleManager::LoadModuleChecked<IModuleInterface>("PorismDIMsWorldGenerator");
	FModuleManager::LoadModuleChecked<IModuleInterface>("SteamCoreSockets");

	FPorismSteamSocketsEventRouter::Get().Startup();
	AChunkWorldNetManager::SetTransportFactory(MakeShared<FPorismSteamSocketsTransportFactory>());
	bInstalledSteamTransportFactory = true;

	UE_LOG(LogPorismDIMsWorldGeneratorExtensionSteamCore, Log, TEXT("Porism Steam transport bridge started."));
}

void FPorismDIMsWorldGeneratorExtensionSteamCoreModule::ShutdownModule()
{
	if (bInstalledSteamTransportFactory)
	{
		AChunkWorldNetManager::SetTransportFactory(nullptr);
		FPorismSteamSocketsEventRouter::Get().Shutdown();
		bInstalledSteamTransportFactory = false;
	}

	UE_LOG(LogPorismDIMsWorldGeneratorExtensionSteamCore, Log, TEXT("Porism Steam transport bridge shutting down."));
}

IMPLEMENT_MODULE(FPorismDIMsWorldGeneratorExtensionSteamCoreModule, PorismDIMsWorldGeneratorExtensionSteamCore)
