// Copyright 1998-2019 Epic Games, Inc. All Rights Reserved.

#include "FileMediaOutput.h"

#include "FileMediaCapture.h"
#include "Misc/Paths.h"
#include "UnrealEngine.h"



UFileMediaOutput::UFileMediaOutput()
	: Super()
{
	FilePath.Path = FPaths::Combine(*FPaths::ProjectSavedDir(), *FString("MediaOutput"));
}


bool UFileMediaOutput::Validate(FString& OutFailureReason) const
{
	if (!Super::Validate(OutFailureReason))
	{
		return false;
	}

	if (FilePath.Path.IsEmpty())
	{
		OutFailureReason = FString::Printf(TEXT("Can't validate MediaOutput '%s'. The file path is null."), *GetName());
		return false;
	}

	if (GetRequestedPixelFormat() == PF_A2B10G10R10)
	{
		OutFailureReason = FString::Printf(TEXT("Can't validate MediaOutput '%s'. File media output doesn't support 10bits format."), *GetName());
		return false;
	}

	if (GetRequestedPixelFormat() != PF_B8G8R8A8 && WriteOptions.Format != EDesiredImageFormat::EXR)
	{
		OutFailureReason = FString::Printf(TEXT("Can't validate MediaOutput '%s'. Only EXR export is currently supported for PF_FloatRGBA and PF_A32B32G32R32F formats."), *GetName());
		return false;
	}

	return true;
}


FIntPoint UFileMediaOutput::GetRequestedSize() const
{
	if (bOverrideDesiredSize)
	{
		return DesiredSize;
	}

	return FIntPoint(GSystemResolution.ResX, GSystemResolution.ResY);
}


EPixelFormat UFileMediaOutput::GetRequestedPixelFormat() const
{
	if (bOverridePixelFormat)
	{
		return DesiredPixelFormat == EFileMediaOutputPixelFormat::FloatRGBA ? PF_FloatRGBA : PF_B8G8R8A8;
	}

	static const auto CVarDefaultBackBufferPixelFormat = IConsoleManager::Get().FindTConsoleVariableDataInt(TEXT("r.DefaultBackBufferPixelFormat"));
	EPixelFormat SceneTargetFormat = EDefaultBackBufferPixelFormat::Convert2PixelFormat(EDefaultBackBufferPixelFormat::FromInt(CVarDefaultBackBufferPixelFormat->GetValueOnAnyThread()));

	if (WriteOptions.Format == EDesiredImageFormat::EXR)
	{
		return SceneTargetFormat == EPixelFormat::PF_A2B10G10R10 ? PF_B8G8R8A8 : SceneTargetFormat;
	}
	else
	{
		return PF_B8G8R8A8;
	}
}


UMediaCapture* UFileMediaOutput::CreateMediaCaptureImpl()
{
	UMediaCapture* Result = NewObject<UFileMediaCapture>();
	if (Result)
	{
		Result->SetMediaOutput(this);
	}
	return Result;
}
