// Copyright 1998-2019 Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class GeometryCache : ModuleRules
{
	public GeometryCache(ReadOnlyTargetRules Target) : base(Target)
	{
        PublicDependencyModuleNames.AddRange(
			new string[] {
				"Core",
				"CoreUObject",
				"Engine",
				"Slate",
				"SlateCore",
                "InputCore",
                "RenderCore",
                "RHI"
			}
		);

        PublicIncludePathModuleNames.Add("TargetPlatform");

        if (Target.bBuildEditor)
        {
            PublicIncludePathModuleNames.Add("GeometryCacheEd");
            DynamicallyLoadedModuleNames.Add("GeometryCacheEd");
            PrivateDependencyModuleNames.Add("MeshUtilitiesCommon");
        }        
	}
}
