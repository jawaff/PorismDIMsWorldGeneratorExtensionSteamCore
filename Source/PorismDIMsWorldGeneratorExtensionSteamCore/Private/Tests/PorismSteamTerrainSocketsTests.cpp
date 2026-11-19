// Copyright 2026 Spotted Loaf Studio

#if WITH_DEV_AUTOMATION_TESTS
#include "PorismSteamTerrainDriver.h"
#include "PorismSteamTerrainSockets.h"
#include "PorismSteamTerrainStream.h"
#include "Async/Async.h"
#include "ChunkWorldExtended/ChunkWorldNetworkSubsystem.h"
#include "Engine/Engine.h"
#include "HAL/Event.h"
#include "Misc/AutomationTest.h"
#include "SocketSubsystem.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPorismSteamTerrainPortTest, "PorismExtension.Networking.SteamTerrain.HostVirtualPort",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FPorismSteamTerrainPortTest::RunTest(const FString&)
{
	using namespace PorismSteamTerrainSockets;
	int32 Bound = 0, Calls = 0;
	bool bCanBind = true;
	auto Bind = [&](int32 Port) { Bound = Port; ++Calls; return bCanBind; };
	const FString Identity(TEXT("76561198000000001"));
	FString Address;
	for (const int32 Override : {0, 1, 19077, 65535})
	{
		const int32 ExpectedPort = Override == 0 ? 17882 : Override;
		TestTrue(TEXT("Host can bind"), Listen(Bind, ListenPort(17882, Override), Identity, Address));
		TestEqual(TEXT("Exact virtual port reaches Steam"), Bound, ExpectedPort);
		TestEqual(TEXT("Advertised endpoint matches bind"), Address, FString::Printf(TEXT("%s:%d"), *Identity, ExpectedPort));
		int32 Parsed = 0;
		TestTrue(TEXT("Client follows advertised port"), PeerPort(Address, Identity, 7777, Parsed) && Parsed == ExpectedPort);
	}
	TestEqual(TEXT("Client factory ignores its local host override"), ListenPort(0, 20123), 0);
	int32 Port = 0;
	TestFalse(TEXT("Never dial gameplay port"), PeerPort(Identity + TEXT(":7777"), Identity, 7777, Port));
	TestFalse(TEXT("Never switch Steam identity"), PeerPort(TEXT("123:7877"), Identity, 7777, Port));

	const int32 PreviousCalls = Calls;
	for (const int32 Invalid : {-1, 65536})
	{
		TestFalse(TEXT("Invalid override rejected"), Listen(Bind, ListenPort(7877, Invalid), Identity, Address));
		TestTrue(TEXT("No stale endpoint"), Address.IsEmpty());
	}
	TestEqual(TEXT("Invalid port never reaches Steam"), Calls, PreviousCalls);
	bCanBind = false;
	Address = TEXT("previous:7877");
	Listen(Bind, 19077, Identity, Address);
	TestTrue(TEXT("Refused bind never remaps or advertises"), Address.IsEmpty() && Calls == PreviousCalls + 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPorismTerrainHandshakeRoute, "PorismExtension.Networking.SteamTerrain.HandshakeAttempt",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FPorismTerrainHandshakeRoute::RunTest(const FString&)
{
	auto* Platform = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	if (!TestNotNull(TEXT("Platform address keys available"), Platform)) return false;
	using PorismSteamTerrainSockets::MatchesReplyAttempt;
	auto First = Platform->CreateInternetAddr();
	First->SetIp(1);
	First->SetPort(7877);
	auto Retry = First->Clone();
	auto Rejoin = First->Clone();
	Rejoin->SetIp(2);
	TestTrue(TEXT("ACK retry on original attempt"), MatchesReplyAttempt(&First.Get(), &Retry.Get()));
	TestFalse(TEXT("Late reply without receive scope"), MatchesReplyAttempt(nullptr, &Retry.Get()));
	TestFalse(TEXT("No destination is not a route"), MatchesReplyAttempt(&First.Get(), nullptr));
	TestFalse(TEXT("Reconnect cannot inherit old attempt's reply"), MatchesReplyAttempt(&Rejoin.Get(), &Retry.Get()));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPorismTerrainGameplayChoice, "PorismExtension.Networking.SteamTerrain.GameplayChoice",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FPorismTerrainGameplayChoice::RunTest(const FString&)
{
	auto* Ip = LoadClass<UNetDriver>(nullptr, TEXT("/Script/OnlineSubsystemUtils.IpNetDriver"));
	if (!TestNotNull(TEXT("IP net driver available offline"), Ip)) return false;
	const FName Lan = UChunkWorldNetworkSubsystem::ProtocolForDriver(Ip->GetDefaultObject<UNetDriver>());
	TestEqual(TEXT("LAN gameplay selects native TCP even with the Steam bridge installed"), Lan, FName(TEXT("Tcp")));
	TestEqual(TEXT("Steam gameplay selects Steam"), UChunkWorldNetworkSubsystem::ProtocolForDriver(GetDefault<USteamCoreSocketsNetDriver>()), FName(TEXT("Steam")));
	TestNull(TEXT("Steam cannot guess a gameplay world"), UChunkWorldNetworkSubsystem::CreateFactory(TEXT("Steam"), 7877, true).Get());
	TestTrue(TEXT("Auxiliary driver is not a gameplay provider"), UChunkWorldNetworkSubsystem::ProtocolForDriver(GetDefault<UPorismSteamTerrainDriver>()).IsNone());
	const auto* GameDef = GEngine->NetDriverDefinitions.FindByPredicate([](const FNetDriverDefinition& D) { return D.DefName == NAME_GameNetDriver; });
	TestTrue(TEXT("Registering an auxiliary driver preserves the editor's gameplay driver"),
		GameDef && !GameDef->DriverClassName.IsNone() && GameDef->DriverClassName != FName(*UPorismSteamTerrainDriver::StaticClass()->GetPathName()));
	auto* Def = GEngine->NetDriverDefinitions.FindByPredicate([](const FNetDriverDefinition& D) { return D.DefName == UPorismSteamTerrainDriver::Definition; });
	if (TestNotNull(TEXT("Optional terrain driver registered"), Def))
	{
		TestEqual(TEXT("Steam class only"), Def->DriverClassName, FName(*UPorismSteamTerrainDriver::StaticClass()->GetPathName()));
		TestEqual(TEXT("No IP fallback"), Def->DriverClassNameFallback, Def->DriverClassName);
	}

	// Exercise the real LAN factory, not a fake socket or an installed Steam session.
	auto Host = UChunkWorldNetworkSubsystem::CreateFactory(Lan, 29777, false);
	auto Client = UChunkWorldNetworkSubsystem::CreateFactory(Lan, 0, true);
	if (!TestTrue(TEXT("Native LAN factories available"), Host && Client)) return false;
	FEventRef Accepted;
	TUniquePtr<IChunkWorldNetTransport> In;
	if (!TestTrue(TEXT("Listen on independent TCP port"), Host->StartListening([&](TUniquePtr<IChunkWorldNetTransport> Transport)
		{ In = MoveTemp(Transport); Accepted->Trigger(); return true; }))) return false;
	auto Out = Client->CreateConnection(TEXT("127.0.0.1:") + Host->GetConnectionAddress());
	const bool bAccepted = Out && Accepted->Wait(5000);
	Host->StopListening();
	if (!TestTrue(TEXT("Offline LAN connect and accept"), bAccepted && In)) return false;
	uint8 Byte = 97, Result = 0;
	int32 Read = 0;
	TestTrue(TEXT("Terrain byte arrives on the LAN listener"), Out->SendAll(&Byte, 1) && In->WaitForData(2)
		&& In->Recv(&Result, 1, Read) && Read == 1 && Result == Byte);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPorismTerrainStreamTest, "PorismExtension.Networking.SteamTerrain.Stream",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FPorismTerrainStreamTest::RunTest(const FString&)
{
	FPorismTerrainStream Stream;
	using ESend = FPorismTerrainStream::ESend;
	TArray<uint8> Source;
	Source.SetNumUninitialized(FPorismTerrainStream::BlockSize * 5 + 7);
	for (int32 I = 0; I < Source.Num(); ++I) Source[I] = static_cast<uint8>(I * 31);
	TestTrue(TEXT("Queue data while handshake is pending"), Stream.Write(Source.GetData(), Source.Num()));
	TestFalse(TEXT("Reject beyond bounded queue without copying"), Stream.Write(Source.GetData(), FPorismTerrainStream::Capacity));
	Stream.Flush([](TConstArrayView<uint8>) { return ESend::Busy; });
	TArray<uint8> Received;
	auto Capture = [&](TConstArrayView<uint8> Block) { Received.Append(Block.GetData(), Block.Num()); return ESend::Sent; };
	Stream.Flush(Capture);
	TestEqual(TEXT("Bound per-tick send work"), Received.Num(), FPorismTerrainStream::BlockSize * 4);
	Stream.Flush(Capture);
	TestTrue(TEXT("No lost or duplicated bytes after backpressure"), Source == Received);
	TestFalse(TEXT("Nonblocking poll before delivery"), Stream.Wait(0));
	TestTrue(TEXT("Receive one block"), Stream.ReceiveBlock(Source.GetData(), 47));
	uint8 Buffer[64];
	int32 Read = 0;
	TestTrue(TEXT("Partial read preserves remainder"), Stream.Read(Buffer, 7, Read) && Read == 7 && Stream.Wait(0)
		&& Stream.Read(Buffer + 7, 57, Read) && Read == 40 && FMemory::Memcmp(Buffer, Source.GetData(), 47) == 0 && !Stream.Wait(0));
	auto Reader = Async(EAsyncExecution::Thread, [&] { int32 Count; return Stream.Read(Buffer, 1, Count); });
	Stream.Close();
	Stream.Close();
	TestTrue(TEXT("Close wakes reader and retires writes"), Reader.WaitFor(FTimespan::FromSeconds(2)) && !Reader.Get() && !Stream.Write(Buffer, 1));
	return true;
}
#endif
