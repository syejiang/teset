// Copyright 1998-2019 Epic Games, Inc. All Rights Reserved.

#include "Analytics/CrashReportsPrivacySettings.h"
#include "UObject/UnrealType.h"
#include "Interfaces/IAnalyticsProvider.h"
#include "EngineAnalytics.h"

#define LOCTEXT_NAMESPACE "CrashReportsPrivacySettings"

UCrashReportsPrivacySettings::UCrashReportsPrivacySettings(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
	, bSendUnattendedBugReports(true)
{
}

void UCrashReportsPrivacySettings::GetToogleCategoryAndPropertyNames(FName& OutCategory, FName& OutProperty) const
{
	OutCategory = FName("Options");
	OutProperty = FName("bSendUnattendedBugReports");
};

FText UCrashReportsPrivacySettings::GetFalseStateLabel() const
{
	return LOCTEXT("FalseStateLabel", "Don't send");
};

FText UCrashReportsPrivacySettings::GetFalseStateTooltip() const
{
	return LOCTEXT("FalseStateTooltip", "Don't send unattended bug reports to Epic Games.");
};

FText UCrashReportsPrivacySettings::GetFalseStateDescription() const
{
	return LOCTEXT("FalseStateDescription", "Turn on this setting to allow the editor send non-critical bug reports by default to Epic Games to help us improve Unreal Engine. Please note that if you turn off this setting, we may ask you to share information about critical bugs, but sharing such information is optional. You can find out more at our Privacy Policy.");
};

FText UCrashReportsPrivacySettings::GetTrueStateLabel() const
{
	return LOCTEXT("TrueStateLabel", "Send unattended bug reports");
};

FText UCrashReportsPrivacySettings::GetTrueStateTooltip() const
{
	return LOCTEXT("TrueStateTooltip", "Automatically send bug reports that don't require user attention to Epic Games .");
};

FText UCrashReportsPrivacySettings::GetTrueStateDescription() const
{
	return LOCTEXT("TrueStateDescription", "Turn off this setting if you prefer to not have the editor send non-critical bug reports by default to Epic Games to help us improve Unreal Engine. Please note that if you turn off this setting, we may ask you to share information about critical bugs, but sharing such information is optional. You can find out more at our Privacy Policy.");
};

FString UCrashReportsPrivacySettings::GetAdditionalInfoUrl() const
{
	return FString(TEXT("http://epicgames.com/privacynotice"));
};

FText UCrashReportsPrivacySettings::GetAdditionalInfoUrlLabel() const
{
	return LOCTEXT("HyperlinkLabel", "Epic Games Privacy Notice");
};

#undef LOCTEXT_NAMESPACE
