// Copyright 1998-2014 Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class jemalloc : ModuleRules
{
	public jemalloc(TargetInfo Target)
	{
		Type = ModuleType.External;

        if (Target.Platform == UnrealTargetPlatform.Linux)
        {
		// includes may differ depending on target platform
		// "separate" means that function names are mangled and libc malloc()/free() is still available
		// for use by other libraries
		// "drop-in" means that jemalloc replaces libc malloc()/free() and every allocation is made
		// through it - including libraries like libcurl
		PublicIncludePaths.Add(UEBuildConfiguration.UEThirdPartySourceDirectory + "jemalloc/include/Linux/" + Target.Architecture + "/separate");
		PublicLibraryPaths.Add(UEBuildConfiguration.UEThirdPartySourceDirectory + "jemalloc/lib/Linux/" + Target.Architecture + "/separate");
		//PublicIncludePaths.Add(UEBuildConfiguration.UEThirdPartySourceDirectory + "jemalloc/include/Linux/drop-in");
		//PublicLibraryPaths.Add(UEBuildConfiguration.UEThirdPartySourceDirectory + "jemalloc/lib/Linux/drop-in");
            PublicAdditionalLibraries.Add("jemalloc");
		Definitions.Add("PLATFORM_SUPPORTS_JEMALLOC=1");
        }
	}
}
