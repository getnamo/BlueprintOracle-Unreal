// Copyright 2026-current Getnamo. All Rights Reserved.

using UnrealBuildTool;

public class BlueprintOracle : ModuleRules
{
	public BlueprintOracle(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"CoreUObject",
				"Engine"
			}
		);

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"UnrealEd",          // commandlet base, linker access
				"Kismet",            // blueprint editor utilities
				"BlueprintGraph",    // K2Node types
				"ScriptDisassembler",// FKismetBytecodeDisassembler
				"AssetRegistry",     // asset discovery
				"Json",
				"JsonUtilities"
			}
		);
	}
}
