// Copyright 2026 Spotted Loaf Studio

using UnrealBuildTool;

public class PorismDIMsWorldGeneratorExtensionSteamCore : ModuleRules
{
	/// <summary>
	/// Registers the optional Steam relay-backed Porism transport bridge.
	/// </summary>
	public PorismDIMsWorldGeneratorExtensionSteamCore(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"PorismDIMsWorldGenerator",
			"Sockets",
			"SteamCorePro",
			"SteamCoreShared",
			"SteamCoreSockets"
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"NetCommon",
			"NetCore",
			"PacketHandler",
			"SteamLibrary"
		});

		// The driver exposes its Steam socket but not its handle or receive poll group.
		PrivateIncludePaths.Add(System.IO.Path.Combine(GetModuleDirectory("SteamCoreSockets"), "Private"));
	}
}
