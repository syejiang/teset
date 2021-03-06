// Copyright 1998-2019 Epic Games, Inc. All Rights Reserved.

#pragma once

#include "EditorSubsystem.h"

#include "UObject/ObjectMacros.h"
#include "UObject/UObjectGlobals.h"
#include "Templates/SubclassOf.h"
#include "Containers/Queue.h"

#include "ImportSubsystem.generated.h"

/**
 * Interface for tasks that need delayed execution
 */
class IImportSubsystemTask
{
public:
	virtual ~IImportSubsystemTask() { }

	virtual void Run() = 0;
};

/**
 * UImportSubsystem
 * Subsystem for importing assets in the editor, 
 * Contains utility functions and callbacks for hooking into importing.
 */
UCLASS()
class UNREALED_API UImportSubsystem : public UEditorSubsystem
{
	GENERATED_BODY()

public:
	UImportSubsystem();

	virtual void Initialize(FSubsystemCollectionBase& Collection);
	virtual void Deinitialize();

	/* Import files next tick */
	void ImportNextTick(const TArray<FString>& Files, const FString& DestinationPath);

	/** delegate type fired when new assets are being (re-)imported. Params: UFactory* InFactory, UClass* InClass, UObject* InParent, const FName& Name, const TCHAR* Type */
	DECLARE_MULTICAST_DELEGATE_FiveParams(FOnAssetPreImport, UFactory*, UClass*, UObject*, const FName&, const TCHAR*);
	/** delegate type fired when new assets have been (re-)imported. Note: InCreatedObject can be NULL if import failed. Params: UFactory* InFactory, UObject* InCreatedObject */
	DECLARE_MULTICAST_DELEGATE_TwoParams(FOnAssetPostImport, UFactory*, UObject*);
	/** delegate type fired when new assets have been reimported. Note: InCreatedObject can be NULL if import failed. Params: UObject* InCreatedObject */
	DECLARE_MULTICAST_DELEGATE_OneParam(FOnAssetReimport, UObject*);

	// Broadcast AssetPreImport, do not broadcast with OnAssetPostImport directly.
	void BroadcastAssetPreImport(UFactory* InFactory, UClass* InClass, UObject* InParent, const FName& Name, const TCHAR* Type);
	// Broadcast AssetPostImport, do not broadcast with OnAssetPostImport directly.
	void BroadcastAssetPostImport(UFactory* InFactory, UObject* InCreatedObject);
	// Broadcast AssetReimport, do not broadcast with OnAssetReimport directly.
	void BroadcastAssetReimport(UObject* InCreatedObject);

	// Used to register and unregister ONLY use Broadcast functions to execute the delegate
	FOnAssetPreImport OnAssetPreImport;
	FOnAssetPostImport OnAssetPostImport;
	FOnAssetReimport OnAssetReimport;

private:

	/* Run deferred logic waiting to be run next tick */
	void HandleNextTick();

	/** delegate type fired when new assets are being (re-)imported. Params: UFactory* InFactory, UClass* InClass, UObject* InParent, const FName& Name, const TCHAR* Type */
	DECLARE_DYNAMIC_MULTICAST_DELEGATE_FiveParams(FOnAssetPreImport_Dyn, UFactory*, InFactory, UClass*, InClass, UObject*, InParent, const FName&, Name, const FString, Type);
	/** delegate type fired when new assets have been (re-)imported. Note: InCreatedObject can be NULL if import failed. Params: UFactory* InFactory, UObject* InCreatedObject */
	DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnAssetPostImport_Dyn, UFactory*, InFactory, UObject*, InCreatedObject);
	/** delegate type fired when new assets have been reimported. Note: InCreatedObject can be NULL if import failed. Params: UObject* InCreatedObject */
	DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnAssetReimport_Dyn, UObject*, InCreatedObject);

	UPROPERTY(BlueprintAssignable, DisplayName = "OnAssetPreImport", meta = (ScriptName = "OnAssetPreImport"))
	FOnAssetPreImport_Dyn OnAssetPreImport_BP;
	UPROPERTY(BlueprintAssignable, DisplayName = "OnAssetPostImport", meta = (ScriptName = "OnAssetPostImport"))
	FOnAssetPostImport_Dyn OnAssetPostImport_BP;
	UPROPERTY(BlueprintAssignable, DisplayName = "OnAssetReimport", meta = (ScriptName = "OnAssetReimport"))
	FOnAssetReimport_Dyn OnAssetReimport_BP;

	/* Tasks waiting to be run next tick */
	TQueue<TSharedPtr<IImportSubsystemTask>> PendingTasks;
};
