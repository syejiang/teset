// Copyright 1998-2019 Epic Games, Inc. All Rights Reserved.

#include "AdditiveAnimationInstance.h"
#include "AnimationSharingInstances.h"
#include "AnimationSharingManager.h"

FAdditiveAnimationInstance::FAdditiveAnimationInstance() : SkeletalMeshComponent(nullptr), AdditiveInstance(nullptr), AdditiveAnimationSequence(nullptr), BaseComponent(nullptr), bLoopingState(false)
{
}

void FAdditiveAnimationInstance::Initialise(USkeletalMeshComponent* InSkeletalMeshComponent, UClass* InAnimationBPClass)
{
	if (InSkeletalMeshComponent && InAnimationBPClass)
	{
		SkeletalMeshComponent = InSkeletalMeshComponent;
		SkeletalMeshComponent->SetAnimInstanceClass(InAnimationBPClass);
		SkeletalMeshComponent->SetComponentTickEnabled(false);	
		SkeletalMeshComponent->SetForcedLOD(1);
		SkeletalMeshComponent->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;
		AdditiveInstance = Cast<UAnimSharingAdditiveInstance>(SkeletalMeshComponent->GetAnimInstance());
	}
}

void FAdditiveAnimationInstance::Setup(USkeletalMeshComponent* InBaseComponent, UAnimSequence* InAnimSequence)
{
	UAnimationSharingManager::SetDebugMaterial(SkeletalMeshComponent, 1);
	SkeletalMeshComponent->SetComponentTickEnabled(true);
	if (AdditiveInstance)
	{
		AdditiveInstance->BaseComponent = BaseComponent = InBaseComponent;
		AdditiveInstance->AdditiveAnimation = AdditiveAnimationSequence = InAnimSequence;
		AdditiveInstance->Alpha = 1.0f;
		AdditiveInstance->bStateBool = bLoopingState = true;

		SkeletalMeshComponent->AddTickPrerequisiteComponent(BaseComponent);
	}
}

void FAdditiveAnimationInstance::UpdateBaseComponent(USkeletalMeshComponent* InBaseComponent)
{
	if (AdditiveInstance)
	{
		SkeletalMeshComponent->RemoveTickPrerequisiteComponent(BaseComponent);
		AdditiveInstance->BaseComponent = BaseComponent = InBaseComponent;
		SkeletalMeshComponent->AddTickPrerequisiteComponent(BaseComponent);
	}
}

void FAdditiveAnimationInstance::Stop()
{
	if (AdditiveInstance)
	{
		UAnimationSharingManager::SetDebugMaterial(SkeletalMeshComponent, 0);
		SkeletalMeshComponent->SetComponentTickEnabled(false);
		SkeletalMeshComponent->RemoveTickPrerequisiteComponent(BaseComponent);
	}
}

void FAdditiveAnimationInstance::Start()
{
	if (AdditiveInstance)
	{
		AdditiveInstance->bStateBool = bLoopingState = false;
	}
}

USkeletalMeshComponent* FAdditiveAnimationInstance::GetBaseComponent() const
{
	return BaseComponent;
}

USkeletalMeshComponent* FAdditiveAnimationInstance::GetComponent() const
{
	return SkeletalMeshComponent;
}

