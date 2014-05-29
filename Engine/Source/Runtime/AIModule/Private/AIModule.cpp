// Copyright 1998-2014 Epic Games, Inc. All Rights Reserved.

#include "AIModulePrivate.h"

class FAIModule : public IAIModule
{
	// Begin IModuleInterface
	virtual void StartupModule() OVERRIDE;
	virtual void ShutdownModule() OVERRIDE;
	// End IModuleInterface
};

IMPLEMENT_MODULE(FAIModule, AIModule)

void FAIModule::StartupModule()
{ 
	// This code will execute after your module is loaded into memory (but after global variables are initialized, of course.)
}

void FAIModule::ShutdownModule()
{
	// This function may be called during shutdown to clean up your module.  For modules that support dynamic reloading,
	// we call this function before unloading the module.
}
