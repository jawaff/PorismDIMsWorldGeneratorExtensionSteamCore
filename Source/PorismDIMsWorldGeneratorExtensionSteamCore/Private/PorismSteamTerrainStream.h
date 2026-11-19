// Copyright 2026 Spotted Loaf Studio

#pragma once

#include "ChunkWorldExtended/ChunkWorldNetTransport.h"
#include "Containers/Deque.h"
#include "HAL/Event.h"

/** Porism's blocking reader and game-thread Steam pump share bytes, never sockets or UObjects. */
class FPorismTerrainStream
{
public:
	static constexpr int32 BlockSize = 32 * 1024;
	// A complete legal terrain frame can be submitted from the game thread before the next network tick.
	static constexpr int32 Capacity = 64 * 1024 * 1024 + 3 * sizeof(int32) + sizeof(FGuid);

	void Close();
	bool IsOpen() const;
	bool Write(const uint8* Data, int32 Count);
	bool ReceiveBlock(const uint8* Data, int32 Count);
	bool Read(uint8* Data, int32 MaxBytes, int32& ReadBytes);
	bool Wait(float Seconds);

	enum class ESend { Sent, Busy, Failed };
	/** A full Steam queue retains the unsent block. Never sleep waiting for the next game-thread tick. */
	void Flush(TFunctionRef<ESend(TConstArrayView<uint8>)> SendBlock);

private:
	struct FBytes
	{
		bool Append(const uint8* Data, int32 Count);
		TConstArrayView<uint8> Front() const;
		void Consume(int32 Count);
		TDeque<TArray<uint8>> Blocks;
		int32 Size = 0;
		int32 Offset = 0;
	};
	mutable FCriticalSection Mutex;
	FEventRef Readable{EEventMode::ManualReset};
	FBytes Incoming, Outgoing;
	bool bOpen = true;
};

/** Destruction by Porism retires exactly one stream, even if its original peer has reconnected. */
class FPorismTerrainTransport final : public IChunkWorldNetTransport
{
public:
	explicit FPorismTerrainTransport(TSharedRef<FPorismTerrainStream> InStream) : Stream(MoveTemp(InStream)) {}
	~FPorismTerrainTransport() override { Close(); }
	void Close() override { Stream->Close(); }
	bool IsConnected() const override { return Stream->IsOpen(); }
	bool WaitForData(float Seconds) override { return Stream->Wait(Seconds); }
	bool Recv(uint8* Data, int32 Count, int32& ReadBytes) override { return Stream->Read(Data, Count, ReadBytes); }
	bool Send(const uint8* Data, int32 Count, int32& Sent) override
	{
		Sent = Stream->Write(Data, Count) ? Count : 0;
		return Sent > 0;
	}
private:
	TSharedRef<FPorismTerrainStream> Stream;
};
