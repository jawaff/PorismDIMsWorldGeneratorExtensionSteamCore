// Copyright 2026 Spotted Loaf Studio

#include "PorismSteamTerrainStream.h"
#include "Misc/ScopeLock.h"

bool FPorismTerrainStream::FBytes::Append(const uint8* Data, int32 Count)
{
	if (!Data || Count <= 0 || Count > Capacity - Size) return false;
	Size += Count;
	while (Count > 0)
	{
		if (Blocks.IsEmpty() || Blocks.Last().Num() == BlockSize) Blocks.EmplaceLast();
		TArray<uint8>& Tail = Blocks.Last();
		const int32 Take = FMath::Min(Count, BlockSize - Tail.Num());
		Tail.Append(Data, Take);
		Data += Take;
		Count -= Take;
	}
	return true;
}

TConstArrayView<uint8> FPorismTerrainStream::FBytes::Front() const
{
	return MakeArrayView(Blocks.First()).RightChop(Offset);
}

void FPorismTerrainStream::FBytes::Consume(int32 Count)
{
	Size -= Count;
	Offset += Count;
	if (Offset == Blocks.First().Num())
	{
		Blocks.PopFirst();
		Offset = 0;
	}
}

void FPorismTerrainStream::Close()
{
	FScopeLock Lock(&Mutex);
	bOpen = false;
	Incoming.Blocks.Empty();
	Outgoing.Blocks.Empty();
	Incoming.Size = Outgoing.Size = 0;
	Readable->Trigger();
}

bool FPorismTerrainStream::IsOpen() const
{
	FScopeLock Lock(&Mutex);
	return bOpen;
}

bool FPorismTerrainStream::Write(const uint8* Data, int32 Count)
{
	FScopeLock Lock(&Mutex);
	return bOpen && Outgoing.Append(Data, Count);
}

bool FPorismTerrainStream::ReceiveBlock(const uint8* Data, int32 Count)
{
	FScopeLock Lock(&Mutex);
	if (!bOpen || Count > BlockSize || !Incoming.Append(Data, Count)) return false;
	Readable->Trigger();
	return true;
}

bool FPorismTerrainStream::Wait(float Seconds)
{
	{
		FScopeLock Lock(&Mutex);
		if (!bOpen || Incoming.Size || Seconds <= 0) return bOpen && Incoming.Size > 0;
	}
	Readable->Wait(FTimespan::FromSeconds(Seconds));
	FScopeLock Lock(&Mutex);
	return bOpen && Incoming.Size > 0;
}

bool FPorismTerrainStream::Read(uint8* Data, int32 MaxBytes, int32& ReadBytes)
{
	ReadBytes = 0;
	if (!Data || MaxBytes <= 0) return false;
	for (;;)
	{
		{
			FScopeLock Lock(&Mutex);
			if (!bOpen) return false;
			if (Incoming.Size)
			{
				const auto Bytes = Incoming.Front();
				ReadBytes = FMath::Min(MaxBytes, Bytes.Num());
				FMemory::Memcpy(Data, Bytes.GetData(), ReadBytes);
				Incoming.Consume(ReadBytes);
				if (!Incoming.Size) Readable->Reset();
				return true;
			}
		}
		Readable->Wait();
	}
}

void FPorismTerrainStream::Flush(TFunctionRef<ESend(TConstArrayView<uint8>)> SendBlock)
{
	FScopeLock Lock(&Mutex);
	for (int32 Block = 0; bOpen && Outgoing.Size && Block < 4; ++Block)
	{
		const auto Bytes = Outgoing.Front();
		const ESend Result = SendBlock(Bytes);
		if (Result == ESend::Failed)
		{
			bOpen = false;
			Readable->Trigger();
		}
		if (Result != ESend::Sent) break;
		Outgoing.Consume(Bytes.Num());
	}
}
