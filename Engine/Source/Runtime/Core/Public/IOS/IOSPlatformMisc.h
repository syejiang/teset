// Copyright 1998-2015 Epic Games, Inc. All Rights Reserved.

/*=============================================================================================
	IOSPlatformMisc.h: iOS platform misc functions
==============================================================================================*/

#pragma once
#include "GenericPlatform/GenericPlatformMisc.h"
#include "IOS/IOSSystemIncludes.h"

/**
* iOS implementation of the misc OS functions
**/
struct CORE_API FIOSPlatformMisc : public FGenericPlatformMisc
{
	static void PlatformInit();
    static void PlatformPostInit(bool ShowSplashScreen = false);
	static class GenericApplication* CreateApplication();
	static void GetEnvironmentVariable(const TCHAR* VariableName, TCHAR* Result, int32 ResultLength);
	static void* GetHardwareWindow();

#if !UE_BUILD_SHIPPING
	FORCEINLINE static bool IsDebuggerPresent()
	{
		// Based on http://developer.apple.com/library/mac/#qa/qa1361/_index.html

		struct kinfo_proc Info;
		int32 Mib[] = { CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid() };
		SIZE_T Size = sizeof(Info);

		sysctl( Mib, sizeof( Mib ) / sizeof( *Mib ), &Info, &Size, NULL, 0 );

		return ( Info.kp_proc.p_flag & P_TRACED ) != 0;
	}
	FORCEINLINE static void DebugBreak()
	{
		if( IsDebuggerPresent() )
		{
			//Signal interupt to our process, this is caught by the main thread, this is not immediate but you can continue
			//when triggered by check macros you will need to inspect other threads for the appFailAssert call.
			//kill( getpid(), SIGINT );

			//If you want an immediate break use the trap instruction, continued execuction is halted
#if WITH_SIMULATOR
            __asm__ ( "int $3" );
#elif PLATFORM_64BITS
			__asm__ ( "svc 0" );
#else
            __asm__ ( "trap" );
#endif
		}
	}
#endif

	/** Break into debugger. Returning false allows this function to be used in conditionals. */
	FORCEINLINE static bool DebugBreakReturningFalse()
	{
#if !UE_BUILD_SHIPPING
		DebugBreak();
#endif
		return false;
	}

	/** Prompts for remote debugging if debugger is not attached. Regardless of result, breaks into debugger afterwards. Returns false for use in conditionals. */
	static FORCEINLINE bool DebugBreakAndPromptForRemoteReturningFalse()
	{
#if !UE_BUILD_SHIPPING
		if (!IsDebuggerPresent())
		{
			PromptForRemoteDebugging(false);
		}

		DebugBreak();
#endif

		return false;
	}

	FORCEINLINE static void MemoryBarrier()
	{
		OSMemoryBarrier();
	}

	static void LowLevelOutputDebugString(const TCHAR *Message);
	static const TCHAR* GetSystemErrorMessage(TCHAR* OutBuffer, int32 BufferCount, int32 Error);
	static void ClipboardCopy(const TCHAR* Str);
	static void ClipboardPaste(class FString& Dest);
	static EAppReturnType::Type MessageBoxExt( EAppMsgType::Type MsgType, const TCHAR* Text, const TCHAR* Caption );
	static int32 NumberOfCores();
	static void LoadPreInitModules();
	static void SetMemoryWarningHandler(void (* Handler)(const FGenericMemoryWarningContext& Context));
	static bool HasPlatformFeature(const TCHAR* FeatureName);
	static FString GetDefaultLocale();
	static bool SetStoredValue(const FString& InStoreId, const FString& InSectionName, const FString& InKeyName, const FString& InValue);
	static bool GetStoredValue(const FString& InStoreId, const FString& InSectionName, const FString& InKeyName, FString& OutValue);
	static TArray<uint8> GetSystemFontBytes();

	static class IPlatformChunkInstall* GetPlatformChunkInstall();
    
	//////// Platform specific
	static void* CreateAutoreleasePool();
	static void ReleaseAutoreleasePool(void *Pool);
	static void HandleLowMemoryWarning();
	static bool IsPackagedForDistribution();
	static FString GetUniqueDeviceId();

	// Possible iOS devices
	enum EIOSDevice
	{
		// add new devices to the top, and add to IOSDeviceNames below!
		IOS_IPhone4,
		IOS_IPhone4S,
		IOS_IPhone5, // also the IPhone5C
		IOS_IPhone5S,
		IOS_IPodTouch5,
		IOS_IPad2,
		IOS_IPad3,
		IOS_IPad4,
		IOS_IPadMini,
		IOS_IPadMini2,
		IOS_IPadAir,
		IOS_IPadAir2,
		IOS_IPhone6,
		IOS_IPhone6Plus,
		IOS_Unknown,
	};

	static EIOSDevice GetIOSDeviceType();

	static FORCENOINLINE const TCHAR* GetDefaultDeviceProfileName()
	{
		static const TCHAR* IOSDeviceNames[] = 
		{
			L"IPhone4",
			L"IPhone4S",
			L"IPhone5",
			L"IPhone5S",
			L"IPodTouch5",
			L"IPad2",
			L"IPad3",
			L"IPad4",
			L"IPadMini",
			L"IPadMini2",
			L"IPadAir",
			L"IPadAir2",
			L"IPhone6",
			L"IPhone6Plus",
			L"Unknown",
		};
		static_assert((sizeof(IOSDeviceNames) / sizeof(IOSDeviceNames[0])) == ((int32)IOS_Unknown + 1), "Mismatched IOSDeviceNames and EIOSDevice.");
		
		// look up into the string array by the enum
		return IOSDeviceNames[(int32)GetIOSDeviceType()];
	}
};

typedef FIOSPlatformMisc FPlatformMisc;
