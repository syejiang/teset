// Copyright 1998-2019 Epic Games, Inc. All Rights Reserved.

#pragma once

#include "TakeRecorderSource.h"
#include "TakeRecorderSources.h"
#include "Templates/SubclassOf.h"
#include "TakeRecorderPlayerSource.generated.h"

class UTakeRecorderActorSource;

/** A recording source that records the current player */
UCLASS(DisplayName="Player", Category="Actors")
class UTakeRecorderPlayerSource : public UTakeRecorderSource
{
public:
	GENERATED_BODY()

	UTakeRecorderPlayerSource(const FObjectInitializer& ObjInit);

private:

	// UTakeRecorderSource
	virtual TArray<UTakeRecorderSource*> PreRecording(class ULevelSequence* InSequence, class ULevelSequence* InMasterSequence, FManifestSerializer* InManifestSerializer) override;
	virtual TArray<UTakeRecorderSource*> PostRecording(class ULevelSequence* InSequence, class ULevelSequence* InMasterSequence) override;
	virtual FText GetDisplayTextImpl() const override;
	virtual bool CanAddSource(UTakeRecorderSources* InSources) const override;

	// This source does not support subscenes (ie. "Player subscene"), but the player would be placed in subscenes if the option is enabled
	virtual bool SupportsSubscenes() const override { return false; }

	TWeakObjectPtr<UTakeRecorderActorSource> PlayerActorSource;
};
