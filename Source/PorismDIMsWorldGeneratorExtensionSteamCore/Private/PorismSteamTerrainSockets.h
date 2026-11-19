// Copyright 2026 Spotted Loaf Studio

#pragma once

#include "CoreMinimal.h"
#include "IPAddress.h"

namespace PorismSteamTerrainSockets
{
	/** Port zero identifies a client factory. Only a host may consume the project override. */
	inline int32 ListenPort(int32 ProviderPort, int32 HostOverride)
	{
		return ProviderPort != 0 && HostOverride != 0 ? HostOverride : ProviderPort;
	}

	/** Publish an endpoint only after the driver proves it owns the exact requested bind. */
	template <typename TBind>
	bool Listen(TBind&& Bind, int32 Port, const FString& Identity, FString& OutAddress)
	{
		OutAddress.Reset();
		if (Identity.IsEmpty() || Port < 1 || Port > MAX_uint16 || !Bind(Port)) return false;
		OutAddress = FString::Printf(TEXT("%s:%d"), *Identity, Port);
		return true;
	}

	/** A retry may carry a clone of this attempt's key. Reject foreign address types before
	 * equality: Steam and platform socket subsystems use different FInternetAddr subclasses. */
	inline bool MatchesReplyAttempt(const FInternetAddr* ActiveKey, const FInternetAddr* RequestedKey)
	{
		return ActiveKey && RequestedKey && ActiveKey->GetProtocolType() == RequestedKey->GetProtocolType()
			&& *ActiveKey == *RequestedKey;
	}

	/** The terrain endpoint must name the already-connected Steam host, on a different port.
	 * Use that trusted identity to build the URL; never parse an advertised URL with options. */
	inline bool PeerPort(const FString& Address, const FString& GameplayPeer, int32 GameplayPort, int32& Port)
	{
		FString Identity, PortText;
		if (GameplayPeer.IsEmpty() || !Address.Split(TEXT(":"), &Identity, &PortText)) return false;
		Identity.RemoveFromStart(TEXT("steam."), ESearchCase::CaseSensitive);
		if (Identity != GameplayPeer || PortText.IsEmpty() || PortText.Len() > 5) return false;
		for (TCHAR C : PortText) if (C < '0' || C > '9') return false;
		return LexTryParseString(Port, *PortText) && Port > 0 && Port <= MAX_uint16 && Port != GameplayPort;
	}
}
