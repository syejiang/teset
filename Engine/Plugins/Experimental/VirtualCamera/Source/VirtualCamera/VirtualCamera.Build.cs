// Copyright 1998-2019 Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class VirtualCamera : ModuleRules
{
	public VirtualCamera(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;
			
		PublicDependencyModuleNames.AddRange(
			new string[]
			{
                "AugmentedReality",
                "CinematicCamera",
                "Core",
                "CoreUObject",
                "Engine",
                "HeadMountedDisplay",
                "InputCore",
                "LevelSequence",
                "LiveLinkInterface",
                "MovieScene",
                "RemoteSession",
                "SteamVR",
                "TimeManagement"
            }
            );

        if (Target.bBuildEditor == true)
        {
			PublicDependencyModuleNames.Add("LevelSequenceEditor");
			PublicDependencyModuleNames.Add("Sequencer");
			PublicDependencyModuleNames.Add("SlateCore");
			PublicDependencyModuleNames.Add("TakeRecorder");
			PrivateDependencyModuleNames.Add("UnrealEd");
        }
	}
}
