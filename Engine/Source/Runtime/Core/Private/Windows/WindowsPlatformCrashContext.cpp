// Copyright 1998-2019 Epic Games, Inc. All Rights Reserved.

#include "Windows/WindowsPlatformCrashContext.h"
#include "HAL/PlatformMallocCrash.h"
#include "HAL/ExceptionHandling.h"
#include "Misc/EngineVersion.h"
#include "Misc/EngineBuildSettings.h"
#include "HAL/ExceptionHandling.h"
#include "HAL/ThreadHeartBeat.h"
#include "HAL/PlatformProcess.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformOutputDevices.h"
#include "Internationalization/Internationalization.h"
#include "Misc/App.h"
#include "Misc/Paths.h"
#include "Misc/FeedbackContext.h"
#include "Misc/MessageDialog.h"
#include "Misc/CoreDelegates.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/OutputDeviceRedirector.h"
#include "Misc/OutputDeviceFile.h"
#include "Templates/ScopedPointer.h"
#include "Windows/WindowsPlatformStackWalk.h"
#include "Windows/WindowsHWrapper.h"
#include "Windows/AllowWindowsPlatformTypes.h"
#include "Templates/UniquePtr.h"
#include "Misc/OutputDeviceArchiveWrapper.h"
#include "HAL/ThreadManager.h"
#include "BuildSettings.h"
#include <strsafe.h>
#include <dbghelp.h>
#include <Shlwapi.h>
#include <psapi.h>

#ifndef UE_LOG_CRASH_CALLSTACK
	#define UE_LOG_CRASH_CALLSTACK 1
#endif

#pragma comment( lib, "version.lib" )
#pragma comment( lib, "Shlwapi.lib" )

/**
 * Code for an assert exception
 */
const uint32 AssertExceptionCode = 0x4000;

/**
 * Stores information about an assert that can be unpacked in the exception handler.
 */
struct FAssertInfo
{
	const TCHAR* ErrorMessage;
	int32 NumStackFramesToIgnore;

	FAssertInfo(const TCHAR* InErrorMessage, int32 InNumStackFramesToIgnore)
		: ErrorMessage(InErrorMessage)
		, NumStackFramesToIgnore(InNumStackFramesToIgnore)
	{
	}
};

void FWindowsPlatformCrashContext::GetProcModuleHandles(FModuleHandleArray& OutHandles)
{
	// Get all the module handles for the current process. Each module handle is its base address.
	for (;;)
	{
		DWORD BufferSize = OutHandles.Num() * sizeof(HMODULE);
		DWORD RequiredBufferSize = 0;
		if (!EnumProcessModules(GetCurrentProcess(), (HMODULE*)OutHandles.GetData(), BufferSize, &RequiredBufferSize))
		{
			return;
		}
		if (RequiredBufferSize <= BufferSize)
		{
			break;
		}
		OutHandles.SetNum(RequiredBufferSize / sizeof(HMODULE));
	}
	// Sort the handles by address. This allows us to do a binary search for the module containing an address.
	Algo::Sort(OutHandles);
}

void FWindowsPlatformCrashContext::ConvertProgramCountersToStackFrames(
	const FModuleHandleArray& SortedModuleHandles,
	const uint64* ProgramCounters,
	int32 NumPCs,
	TArray<FCrashStackFrame>& OutStackFrames)
{
	// Prepare the callstack buffer
	OutStackFrames.Reset(NumPCs);

	// Create the crash context
	for (int32 Idx = 0; Idx < NumPCs; ++Idx)
	{
		int32 ModuleIdx = Algo::UpperBound(SortedModuleHandles, (void*)ProgramCounters[Idx]) - 1;
		if (ModuleIdx < 0 || ModuleIdx >= SortedModuleHandles.Num())
		{
			OutStackFrames.Add(FCrashStackFrame(TEXT("Unknown"), 0, ProgramCounters[Idx]));
		}
		else
		{
			TCHAR ModuleName[MAX_PATH];
			if (GetModuleFileNameW((HMODULE)SortedModuleHandles[ModuleIdx], ModuleName, MAX_PATH) != 0)
			{
				TCHAR* ModuleNameEnd = FCString::Strrchr(ModuleName, '\\');
				if (ModuleNameEnd != nullptr)
				{
					FMemory::Memmove(ModuleName, ModuleNameEnd + 1, (FCString::Strlen(ModuleNameEnd + 1) + 1) * sizeof(TCHAR));
				}

				TCHAR* ModuleNameExt = FCString::Strrchr(ModuleName, '.');
				if (ModuleNameExt != nullptr)
				{
					*ModuleNameExt = 0;
				}
			}
			else
			{
				FCString::Strcpy(ModuleName, TEXT("Unknown"));
			}

			uint64 BaseAddress = (uint64)SortedModuleHandles[ModuleIdx];
			uint64 Offset = ProgramCounters[Idx] - BaseAddress;
			OutStackFrames.Add(FCrashStackFrame(ModuleName, BaseAddress, Offset));
		}
	}
}

void FWindowsPlatformCrashContext::SetPortableCallStack(const uint64* StackTrace, int32 StackTraceDepth)
{
	FModuleHandleArray ProcessModuleHandles;
	GetProcModuleHandles(ProcessModuleHandles);
	ConvertProgramCountersToStackFrames(ProcessModuleHandles, StackTrace, StackTraceDepth, CallStack);
}

void FWindowsPlatformCrashContext::AddPlatformSpecificProperties() const
{
	AddCrashProperty(TEXT("PlatformIsRunningWindows"), 1);
	// On windows track the crash type
	AddCrashProperty(TEXT("PlatformCallbackResult"), GetCrashType());
}

bool FWindowsPlatformCrashContext::GetPlatformAllThreadContextsString(FString& OutStr) const
{
	OutStr = AllThreadContexts;
	return !OutStr.IsEmpty();
}

void FWindowsPlatformCrashContext::AddIsCrashed(bool bIsCrashed, FString& OutStr)
{
	OutStr += FString::Printf(TEXT("<IsCrashed>%s</IsCrashed>"), bIsCrashed ? TEXT("true") : TEXT("false"));
	OutStr += LINE_TERMINATOR;
}

void FWindowsPlatformCrashContext::AddThreadId(uint32 ThreadId, FString& OutStr)
{
	OutStr += FString::Printf(TEXT("<ThreadID>%d</ThreadID>"), ThreadId);
	OutStr += LINE_TERMINATOR;
}

void FWindowsPlatformCrashContext::AddThreadName(const TCHAR* ThreadName, FString& OutStr)
{
	OutStr += FString::Printf(TEXT("<ThreadName>%s</ThreadName>"), ThreadName);
	OutStr += LINE_TERMINATOR;
}

void FWindowsPlatformCrashContext::AddThreadContext(
	const FModuleHandleArray& ProcModuleHandles,
	uint32 CrashedThreadId,
	uint32 ThreadId,
	const FString& ThreadName,
	const uint64* StackTrace,
	int32 Depth,
	FString& OutStr)
{
	OutStr += TEXT("<Thread>");
	{
		OutStr += TEXT("<CallStack>");

		TArray<FCrashStackFrame> CrashStackFrames;
		ConvertProgramCountersToStackFrames(ProcModuleHandles, StackTrace, Depth, CrashStackFrames);

		int32 MaxModuleNameLen = 0;
		for (const FCrashStackFrame& StFrame : CrashStackFrames)
		{
			MaxModuleNameLen = FMath::Max(MaxModuleNameLen, StFrame.ModuleName.Len());
		}

		FString CallstackStr;
		for (const FCrashStackFrame& StFrame : CrashStackFrames)
		{
			CallstackStr += FString::Printf(TEXT("%-*s 0x%016x + %-8x"), MaxModuleNameLen + 1, *StFrame.ModuleName, StFrame.BaseAddress, StFrame.Offset);
			CallstackStr += LINE_TERMINATOR;
		}
		AppendEscapedXMLString(OutStr, *CallstackStr);
		OutStr += TEXT("</CallStack>");
		OutStr += LINE_TERMINATOR;
	}
	AddIsCrashed(ThreadId == CrashedThreadId, OutStr);
	// TODO: do we need thread register states?
	OutStr += TEXT("<Registers></Registers>");
	OutStr += LINE_TERMINATOR;
	AddThreadId(ThreadId, OutStr);
	AddThreadName(*ThreadName, OutStr);
	OutStr += TEXT("</Thread>");
	OutStr += LINE_TERMINATOR;
}

void FWindowsPlatformCrashContext::AddAllThreadContexts(uint32 CrashedThreadId, FString& OutStr)
{
	TArray<typename FThreadManager::FThreadStackBackTrace> StackTraces;
	FThreadManager::Get().GetAllThreadStackBackTraces(StackTraces);

	FModuleHandleArray ProcModuleHandles;
	GetProcModuleHandles(ProcModuleHandles);

	for (int32 Idx = 0; Idx < StackTraces.Num(); ++Idx)
	{
		const auto& ThreadStTrace = StackTraces[Idx];
		const uint32 ThreadId = ThreadStTrace.ThreadId;
		const FString& ThreadName = ThreadStTrace.ThreadName;
		const auto& ThreadPCs = ThreadStTrace.ProgramCounters;
		AddThreadContext(ProcModuleHandles, CrashedThreadId, ThreadId, ThreadName, ThreadPCs.GetData(), ThreadPCs.Num(), OutStr);
	}
}

/** Platform specific constants. */
enum EConstants
{
	UE4_MINIDUMP_CRASHCONTEXT = LastReservedStream + 1,
};

namespace
{
static int32 ReportCrashCallCount = 0;

/**
 * Write a Windows minidump to disk
 * @param The Crash context with its data already serialized into its buffer
 * @param Path Full path of file to write (normally a .dmp file)
 * @param ExceptionInfo Pointer to structure containing the exception information
 * @return Success or failure
 */

// #CrashReport: 2014-10-08 Move to FWindowsPlatformCrashContext
bool WriteMinidump(FWindowsPlatformCrashContext& InContext, const TCHAR* Path, LPEXCEPTION_POINTERS ExceptionInfo)
{
	// Try to create file for minidump.
	HANDLE FileHandle = CreateFileW(Path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	
	if (FileHandle == INVALID_HANDLE_VALUE)
	{
		return false;
	}

	// Initialise structure required by MiniDumpWriteDumps
	MINIDUMP_EXCEPTION_INFORMATION DumpExceptionInfo = {};

	DumpExceptionInfo.ThreadId			= GetCurrentThreadId();
	DumpExceptionInfo.ExceptionPointers	= ExceptionInfo;
	DumpExceptionInfo.ClientPointers	= FALSE;

	// CrashContext.runtime-xml is now a part of the minidump file.
	MINIDUMP_USER_STREAM CrashContextStream ={0};
	CrashContextStream.Type = UE4_MINIDUMP_CRASHCONTEXT;
	CrashContextStream.BufferSize = InContext.GetBuffer().GetAllocatedSize();
	CrashContextStream.Buffer = (void*)*InContext.GetBuffer();

	MINIDUMP_USER_STREAM_INFORMATION CrashContextStreamInformation = {0};
	CrashContextStreamInformation.UserStreamCount = 1;
	CrashContextStreamInformation.UserStreamArray = &CrashContextStream;

	MINIDUMP_TYPE MinidumpType = MiniDumpNormal;//(MINIDUMP_TYPE)(MiniDumpWithPrivateReadWriteMemory|MiniDumpWithDataSegs|MiniDumpWithHandleData|MiniDumpWithFullMemoryInfo|MiniDumpWithThreadInfo|MiniDumpWithUnloadedModules);

	// For ensures by default we use minidump to avoid severe hitches when writing 3GB+ files.
	// However the crash dump mode will remain the same.
	bool bShouldBeFullCrashDump = InContext.IsFullCrashDump();
	if (bShouldBeFullCrashDump)
	{
		MinidumpType = (MINIDUMP_TYPE)(MiniDumpWithFullMemory|MiniDumpWithFullMemoryInfo|MiniDumpWithHandleData|MiniDumpWithThreadInfo|MiniDumpWithUnloadedModules);
	}

	const BOOL Result = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), FileHandle, MinidumpType, &DumpExceptionInfo, &CrashContextStreamInformation, NULL);
	CloseHandle(FileHandle);

	return Result == TRUE;
}

/**
 * Enum indicating whether to run the crash reporter UI
 */
enum class EErrorReportUI
{
	/** Ask the user for a description */
	ShowDialog,

	/** Silently uploaded the report */
	ReportInUnattendedMode	
};

/** 
 * Create a crash report, add the user log and video, and save them a unique the crash folder
 * Launch CrashReportClient.exe to read the report and upload to our CR pipeline
 */
int32 ReportCrashUsingCrashReportClient(FWindowsPlatformCrashContext& InContext, EXCEPTION_POINTERS* ExceptionInfo, EErrorReportUI ReportUI)
{
	// Prevent CrashReportClient from spawning another CrashReportClient.
	const TCHAR* ExecutableName = FPlatformProcess::ExecutableName();
	bool bCanRunCrashReportClient = FCString::Stristr( ExecutableName, TEXT( "CrashReportClient" ) ) == nullptr;

	// Suppress the user input dialog if we're running in unattended mode
	bool bNoDialog = FApp::IsUnattended() || ReportUI == EErrorReportUI::ReportInUnattendedMode || IsRunningDedicatedServer();

	bool bSendUnattendedBugReports = true;
	GConfig->GetBool(TEXT("/Script/UnrealEd.CrashReportsPrivacySettings"), TEXT("bSendUnattendedBugReports"), bSendUnattendedBugReports, GEditorSettingsIni);

	if (BuildSettings::IsLicenseeVersion() && !UE_EDITOR)
	{
		// do not send unattended reports in licensees' builds except for the editor, where it is governed by the above setting
		bSendUnattendedBugReports = false;
	}

	if (bNoDialog && !bSendUnattendedBugReports)
	{
		bCanRunCrashReportClient = false;
	}

	if( bCanRunCrashReportClient )
	{
		static const TCHAR CrashReportClientExeName[] = TEXT("CrashReportClient.exe");
		bool bCrashReporterRan = false;

		// Generate Crash GUID
		TCHAR CrashGUID[FGenericCrashContext::CrashGUIDLength];
		InContext.GetUniqueCrashName(CrashGUID, FGenericCrashContext::CrashGUIDLength);
		const FString AppName = InContext.GetCrashGameName();

		FString CrashFolder = FPaths::Combine(*FPaths::ProjectSavedDir(), TEXT("Crashes"), CrashGUID);
		FString CrashFolderAbsolute = IFileManager::Get().ConvertToAbsolutePathForExternalAppForWrite(*CrashFolder);
		if (IFileManager::Get().MakeDirectory(*CrashFolderAbsolute, true))
		{
			// Save crash context
			const FString CrashContextXMLPath = FPaths::Combine(*CrashFolderAbsolute, FPlatformCrashContext::CrashContextRuntimeXMLNameW);
			InContext.SerializeAsXML(*CrashContextXMLPath);

			// Save mindump
			if (ExceptionInfo != nullptr)
			{
				const FString MinidumpFileName = FPaths::Combine(*CrashFolderAbsolute, *FGenericCrashContext::UE4MinidumpName);
				WriteMinidump(InContext, *MinidumpFileName, ExceptionInfo);
			}

			// Copy log
			const FString LogSrcAbsolute = FPlatformOutputDevices::GetAbsoluteLogFilename();
			FString LogFilename = FPaths::GetCleanFilename(LogSrcAbsolute);
			const FString LogDstAbsolute = FPaths::Combine(*CrashFolderAbsolute, *LogFilename);

			// Flush out the log
			GLog->Flush();

			// If we have a memory only log, make sure it's dumped to file before we attach it to the report
#if !NO_LOGGING
			bool bMemoryOnly = FPlatformOutputDevices::GetLog()->IsMemoryOnly();
			bool bBacklogEnabled = FOutputDeviceRedirector::Get()->IsBacklogEnabled();

			if (bMemoryOnly || bBacklogEnabled)
			{
				FArchive* LogFile = IFileManager::Get().CreateFileWriter(*LogDstAbsolute, FILEWRITE_AllowRead);
				if (LogFile)
				{
					if (bMemoryOnly)
					{
						FPlatformOutputDevices::GetLog()->Dump(*LogFile);
					}
					else
					{
						FOutputDeviceArchiveWrapper Wrapper(LogFile);
						GLog->SerializeBacklog(&Wrapper);
					}

					LogFile->Flush();
					delete LogFile;
				}
			}
			else
			{
				const bool bReplace = true;
				const bool bEvenIfReadOnly = false;
				const bool bAttributes = false;
				FCopyProgress* const CopyProgress = nullptr;
				static_cast<void>(IFileManager::Get().Copy(*LogDstAbsolute, *LogSrcAbsolute, bReplace, bEvenIfReadOnly, bAttributes, CopyProgress, FILEREAD_AllowWrite, FILEWRITE_AllowRead));	// best effort, so don't care about result: couldn't copy -> tough, no log
			}
#endif // !NO_LOGGING

			// If present, include the crash report config file to pass config values to the CRC
			const TCHAR* CrashConfigSrcPath = FWindowsPlatformCrashContext::GetCrashConfigFilePath();
			if (IFileManager::Get().FileExists(CrashConfigSrcPath))
			{
				FString CrashConfigFilename = FPaths::GetCleanFilename(CrashConfigSrcPath);
				const FString CrashConfigDstAbsolute = FPaths::Combine(*CrashFolderAbsolute, *CrashConfigFilename);
				static_cast<void>(IFileManager::Get().Copy(*CrashConfigDstAbsolute, CrashConfigSrcPath));	// best effort, so don't care about result: couldn't copy -> tough, no config
			}

			// If present, include the crash video
			const FString CrashVideoPath = FPaths::ProjectLogDir() / TEXT("CrashVideo.avi");
			if (IFileManager::Get().FileExists(*CrashVideoPath))
			{
				FString CrashVideoFilename = FPaths::GetCleanFilename(CrashVideoPath);
				const FString CrashVideoDstAbsolute = FPaths::Combine(*CrashFolderAbsolute, *CrashVideoFilename);
				static_cast<void>(IFileManager::Get().Copy(*CrashVideoDstAbsolute, *CrashVideoPath));	// best effort, so don't care about result: couldn't copy -> tough, no video
			}

			// Build machines do not upload these automatically since it is not okay to have lingering processes after the build completes.
			if (GIsBuildMachine)
			{
				return EXCEPTION_CONTINUE_EXECUTION;
			}

			// Run Crash Report Client
			FString CrashReportClientArguments = FString::Printf(TEXT("\"%s\""), *CrashFolderAbsolute);

			// Pass nullrhi to CRC when the engine is in this mode to stop the CRC attempting to initialize RHI when the capability isn't available
			bool bNullRHI = !FApp::CanEverRender();

			if (bNoDialog || bNullRHI)
			{
				CrashReportClientArguments += TEXT(" -Unattended");
			}

			if (bNullRHI)
			{
				CrashReportClientArguments += TEXT(" -nullrhi");
			}

			CrashReportClientArguments += FString(TEXT(" -AppName=")) + AppName;
			CrashReportClientArguments += FString(TEXT(" -CrashGUID=")) + CrashGUID;

			const FString DownstreamStorage = FWindowsPlatformStackWalk::GetDownstreamStorage();
			if (!DownstreamStorage.IsEmpty())
			{
				CrashReportClientArguments += FString(TEXT(" -DebugSymbols=")) + DownstreamStorage;
			}

			// CrashReportClient.exe should run without dragging in binaries from an inherited dll directory
			// So, get the current dll directory for restore and clear before creating process
			TCHAR* CurrentDllDirectory = nullptr;
			DWORD BufferSize = (GetDllDirectory(0, nullptr) + 1) * sizeof(TCHAR);
			
			if (BufferSize > 0)
			{
				CurrentDllDirectory = (TCHAR*) FMemory::Malloc(BufferSize);
				if (CurrentDllDirectory)
				{
					FMemory::Memset(CurrentDllDirectory, 0, BufferSize);
					GetDllDirectory(BufferSize, CurrentDllDirectory);
					SetDllDirectory(nullptr);
				}
			}

			FString AbsCrashReportClientLog;
			if (FParse::Value(FCommandLine::Get(), TEXT("AbsCrashReportClientLog="), AbsCrashReportClientLog))
			{
				CrashReportClientArguments += FString::Format(TEXT(" -abslog=\"{0}\""), { AbsCrashReportClientLog });
			}

			FString CrashClientPath = FPaths::Combine(*FPaths::EngineDir(), TEXT("Binaries"), FPlatformProcess::GetBinariesSubdirectory(), CrashReportClientExeName);
			bCrashReporterRan = FPlatformProcess::CreateProc(*CrashClientPath, *CrashReportClientArguments, true, false, false, NULL, 0, NULL, NULL).IsValid();

			// Restore the dll directory
			if (CurrentDllDirectory)
			{
				SetDllDirectory(CurrentDllDirectory);
				FMemory::Free(CurrentDllDirectory);
			}
		}

		if (!bCrashReporterRan && !bNoDialog)
		{
			UE_LOG(LogWindows, Log, TEXT("Could not start %s"), CrashReportClientExeName);
			FPlatformMemory::DumpStats(*GWarn);
			FText MessageTitle(FText::Format(
				NSLOCTEXT("MessageDialog", "AppHasCrashed", "The {0} {1} has crashed and will close"),
				FText::FromString(AppName),
				FText::FromString(FPlatformMisc::GetEngineMode())
				));
			FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(GErrorHist), &MessageTitle);
		}
	}

	// Let the system take back over (return value only used by ReportEnsure)
	return EXCEPTION_CONTINUE_EXECUTION;
}

} // end anonymous namespace

static FCriticalSection EnsureLock;
static bool bReentranceGuard = false;

#if WINVER > 0x502	// Windows Error Reporting is not supported on Windows XP
/**
 * A wrapper for ReportCrashUsingCrashReportClient that creates a new ensure crash context
 */
int32 ReportEnsureUsingCrashReportClient(HANDLE Thread, EXCEPTION_POINTERS* ExceptionInfo, int IgnoreCount, const TCHAR* ErrorMessage, EErrorReportUI ReportUI)
{
	FWindowsPlatformCrashContext CrashContext(ECrashContextType::Ensure, ErrorMessage);

	void* ContextWrapper = FWindowsPlatformStackWalk::MakeThreadContextWrapper(ExceptionInfo->ContextRecord, Thread);
	CrashContext.CapturePortableCallStack(IgnoreCount, ContextWrapper);

	return ReportCrashUsingCrashReportClient(CrashContext, ExceptionInfo, ReportUI);
}
#endif

FORCENOINLINE void ReportEnsureInner( const TCHAR* ErrorMessage, int NumStackFramesToIgnore )
{
	// Skip this frame and the ::RaiseException call itself
	NumStackFramesToIgnore += 2;

	/** This is the last place to gather memory stats before exception. */
	FGenericCrashContext::CrashMemoryStats = FPlatformMemory::GetStats();

#if WINVER > 0x502	// Windows Error Reporting is not supported on Windows XP
#if !PLATFORM_SEH_EXCEPTIONS_DISABLED
	__try
#endif
	{
		::RaiseException(1, 0, 0, nullptr);
	}
#if !PLATFORM_SEH_EXCEPTIONS_DISABLED
	__except(ReportEnsureUsingCrashReportClient(GetCurrentThread(), GetExceptionInformation(), NumStackFramesToIgnore, ErrorMessage, IsInteractiveEnsureMode() ? EErrorReportUI::ShowDialog : EErrorReportUI::ReportInUnattendedMode))
	CA_SUPPRESS(6322)
	{
	}
#endif
#endif	// WINVER
}

FORCENOINLINE void ReportAssert(const TCHAR* ErrorMessage, int NumStackFramesToIgnore)
{
	/** This is the last place to gather memory stats before exception. */
	FGenericCrashContext::CrashMemoryStats = FPlatformMemory::GetStats();

	FAssertInfo Info(ErrorMessage, NumStackFramesToIgnore + 2); // +2 for this function and RaiseException()

	ULONG_PTR Arguments[] = { (ULONG_PTR)&Info };
	::RaiseException( AssertExceptionCode, 0, ARRAY_COUNT(Arguments), Arguments );
}

void ReportHang(const TCHAR* ErrorMessage, const uint64* StackFrames, int32 NumStackFrames, uint32 HungThreadId)
{
	if (ReportCrashCallCount > 0 || FDebug::HasAsserted())
	{
		// Don't report ensures after we've crashed/asserted, they simply may be a result of the crash as
		// the engine is already in a bad state.
		return;
	}

	FWindowsPlatformCrashContext CrashContext(ECrashContextType::Ensure, ErrorMessage);
	CrashContext.SetPortableCallStack(StackFrames, NumStackFrames);
	CrashContext.SetCrashedThreadId(HungThreadId);
	CrashContext.CaptureAllThreadContexts();

	EErrorReportUI ReportUI = IsInteractiveEnsureMode() ? EErrorReportUI::ShowDialog : EErrorReportUI::ReportInUnattendedMode;
	ReportCrashUsingCrashReportClient(CrashContext, nullptr, ReportUI);
}

// #CrashReport: 2015-05-28 This should be named EngineEnsureHandler
/** 
 * Report an ensure to the crash reporting system
 */
FORCENOINLINE void ReportEnsure( const TCHAR* ErrorMessage, int NumStackFramesToIgnore )
{
	if (ReportCrashCallCount > 0 || FDebug::HasAsserted())
	{
		// Don't report ensures after we've crashed/asserted, they simply may be a result of the crash as
		// the engine is already in a bad state.
		return;
	}

	// Simple re-entrance guard.
	EnsureLock.Lock();

	if( bReentranceGuard )
	{
		EnsureLock.Unlock();
		return;
	}

	// Stop checking heartbeat for this thread (and stop the gamethread hitch detector if we're the game thread).
	// Ensure can take a lot of time (when stackwalking), so we don't want hitches/hangs firing.
	// These are no-ops on threads that didn't already have a heartbeat etc.
	FSlowHeartBeatScope SuspendHeartBeat(true);
	FDisableHitchDetectorScope SuspendGameThreadHitch;

	bReentranceGuard = true;

	ReportEnsureInner(ErrorMessage, NumStackFramesToIgnore + 1);

	bReentranceGuard = false;
	EnsureLock.Unlock();
}

#include "Windows/HideWindowsPlatformTypes.h"

// Original code below

#include "Windows/AllowWindowsPlatformTypes.h"
	#include <ErrorRep.h>
	#include <DbgHelp.h>
#include "Windows/HideWindowsPlatformTypes.h"

#pragma comment(lib, "Faultrep.lib")

/** 
 * Creates an info string describing the given exception record.
 * See MSDN docs on EXCEPTION_RECORD.
 */
#include "Windows/AllowWindowsPlatformTypes.h"
void CreateExceptionInfoString(EXCEPTION_RECORD* ExceptionRecord)
{
	// #CrashReport: 2014-08-18 Fix FString usage?
	FString ErrorString = TEXT("Unhandled Exception: ");

#define HANDLE_CASE(x) case x: ErrorString += TEXT(#x); break;

	switch (ExceptionRecord->ExceptionCode)
	{
	case EXCEPTION_ACCESS_VIOLATION:
		ErrorString += TEXT("EXCEPTION_ACCESS_VIOLATION ");
		if (ExceptionRecord->ExceptionInformation[0] == 0)
		{
			ErrorString += TEXT("reading address ");
		}
		else if (ExceptionRecord->ExceptionInformation[0] == 1)
		{
			ErrorString += TEXT("writing address ");
		}
		ErrorString += FString::Printf(TEXT("0x%08x"), (uint32)ExceptionRecord->ExceptionInformation[1]);
		break;
	HANDLE_CASE(EXCEPTION_ARRAY_BOUNDS_EXCEEDED)
	HANDLE_CASE(EXCEPTION_DATATYPE_MISALIGNMENT)
	HANDLE_CASE(EXCEPTION_FLT_DENORMAL_OPERAND)
	HANDLE_CASE(EXCEPTION_FLT_DIVIDE_BY_ZERO)
	HANDLE_CASE(EXCEPTION_FLT_INVALID_OPERATION)
	HANDLE_CASE(EXCEPTION_ILLEGAL_INSTRUCTION)
	HANDLE_CASE(EXCEPTION_INT_DIVIDE_BY_ZERO)
	HANDLE_CASE(EXCEPTION_PRIV_INSTRUCTION)
	HANDLE_CASE(EXCEPTION_STACK_OVERFLOW)
	default:
		ErrorString += FString::Printf(TEXT("0x%08x"), (uint32)ExceptionRecord->ExceptionCode);
	}

	FCString::Strncpy(GErrorExceptionDescription, *ErrorString, ARRAY_COUNT(GErrorExceptionDescription));

#undef HANDLE_CASE
}

/** 
 * Crash reporting thread. 
 * We process all the crashes on a separate thread in case the original thread's stack is corrupted (stack overflow etc).
 * We're using low level API functions here because at the time we initialize the thread, nothing in the engine exists yet.
 **/
class FCrashReportingThread
{
	/** Thread Id */
	DWORD ThreadId;
	/** Thread handle */
	HANDLE Thread;

	/** Stops this thread */
	FThreadSafeCounter StopTaskCounter;
	/** Signals that the game has crashed */
	HANDLE CrashEvent;
	/** Exception information */
	LPEXCEPTION_POINTERS ExceptionInfo;
	DWORD CrashingThreadId;
	HANDLE CrashingThreadHandle;
	/** Event that signals the crash reporting thread has finished processing the crash */
	HANDLE CrashHandledEvent;

	/** Thread main proc */
	static DWORD STDCALL CrashReportingThreadProc(LPVOID pThis)
	{
		FCrashReportingThread* This = (FCrashReportingThread*)pThis;
		return This->Run();
	}

	/** Main loop that waits for a crash to trigger the report generation */
	FORCENOINLINE uint32 Run()
	{
		while (StopTaskCounter.GetValue() == 0)
		{
			if (WaitForSingleObject(CrashEvent, 500) == WAIT_OBJECT_0)
			{
				ResetEvent(CrashHandledEvent);
				HandleCrashInternal();
				ResetEvent(CrashEvent);
				// Let the thread that crashed know we're done.				
				SetEvent(CrashHandledEvent);
				break;
			}
		}
		return 0;
	}

	/** Called by the destructor to terminate the thread */
	void Stop()
	{
		StopTaskCounter.Increment();
	}

public:
		
	FCrashReportingThread()
		: Thread(nullptr)
		, CrashEvent(nullptr)
		, ExceptionInfo(nullptr)
		, CrashHandledEvent(nullptr)
	{
		// Create a background thread that will process the crash and generate crash reports
		Thread = CreateThread(NULL, 0, CrashReportingThreadProc, this, 0, &ThreadId);
		if (Thread)
		{
			SetThreadPriority(Thread, THREAD_PRIORITY_BELOW_NORMAL);
			// Synchronization objects
			CrashEvent = CreateEvent(nullptr, true, 0, nullptr);
			CrashHandledEvent = CreateEvent(nullptr, true, 0, nullptr);
		}
	}

	FORCENOINLINE ~FCrashReportingThread()
	{
		if (Thread)
		{
			// Stop the crash reporting thread
			Stop();
			// 1s should be enough for the thread to exit, otherwise don't bother with cleanup
			if (WaitForSingleObject(Thread, 1000) == WAIT_OBJECT_0)
			{
				CloseHandle(Thread);
				CloseHandle(CrashEvent);
				CloseHandle(CrashHandledEvent);
			}
			Thread = nullptr;
			CrashEvent = nullptr;
			CrashHandledEvent = nullptr;
		}
	}

	/** The thread that crashed calls this function which will trigger the CR thread to report the crash */
	FORCEINLINE void OnCrashed(LPEXCEPTION_POINTERS InExceptionInfo)
	{
		ExceptionInfo = InExceptionInfo;
		CrashingThreadId = GetCurrentThreadId();
		CrashingThreadHandle = GetCurrentThread();
		SetEvent(CrashEvent);
	}

	/** The thread that crashed calls this function to wait for the report to be generated */
	FORCEINLINE bool WaitUntilCrashIsHandled()
	{
		// Wait 60s, it's more than enough to generate crash report. We don't want to stall forever otherwise.
		return WaitForSingleObject(CrashHandledEvent, 60000) == WAIT_OBJECT_0;
	}

private:

	/** Handles the crash */
	FORCENOINLINE void HandleCrashInternal()
	{
		// Stop the heartbeat thread so that it doesn't interfere with crashreporting
		FThreadHeartBeat::Get().Stop();

		GLog->PanicFlushThreadedLogs();

		// Get the default settings for the crash context
		ECrashContextType Type = ECrashContextType::Crash;
		const TCHAR* ErrorMessage = TEXT("Unhandled exception");
		int NumStackFramesToIgnore = 0;

		// If it was an assert, allow overriding the info from the exception parameters
		if (ExceptionInfo->ExceptionRecord->ExceptionCode == AssertExceptionCode && ExceptionInfo->ExceptionRecord->NumberParameters == 1)
		{
			const FAssertInfo& Info = *(const FAssertInfo*)ExceptionInfo->ExceptionRecord->ExceptionInformation[0];
			Type = ECrashContextType::Assert;
			ErrorMessage = Info.ErrorMessage;
			NumStackFramesToIgnore = Info.NumStackFramesToIgnore;
		}

		// Not super safe due to dynamic memory allocations, but at least enables new functionality.
		// Introduces a new runtime crash context. Will replace all Windows related crash reporting.
		FWindowsPlatformCrashContext CrashContext(Type, ErrorMessage);

		// Thread context wrapper for stack operations
		void* ContextWrapper = FWindowsPlatformStackWalk::MakeThreadContextWrapper(ExceptionInfo->ContextRecord, CrashingThreadHandle);
		CrashContext.CapturePortableCallStack(NumStackFramesToIgnore, ContextWrapper);
		CrashContext.SetCrashedThreadId(CrashingThreadId);
		CrashContext.CaptureAllThreadContexts();

		// Also mark the same number of frames to be ignored if we symbolicate from the minidump
		CrashContext.SetNumMinidumpFramesToIgnore(NumStackFramesToIgnore);

		// First launch the crash reporter client.
#if WINVER > 0x502	// Windows Error Reporting is not supported on Windows XP
		if (GUseCrashReportClient)
		{
			ReportCrashUsingCrashReportClient(CrashContext, ExceptionInfo, EErrorReportUI::ShowDialog);
		}
		else
#endif		// WINVER
		{
			CrashContext.SerializeContentToBuffer();
			WriteMinidump(CrashContext, MiniDumpFilenameW, ExceptionInfo);
		}

		// Then try run time crash processing and broadcast information about a crash.
		FCoreDelegates::OnHandleSystemError.Broadcast();

		const bool bGenerateRuntimeCallstack =
#if UE_LOG_CRASH_CALLSTACK
			true;
#else
			FParse::Param(FCommandLine::Get(), TEXT("ForceLogCallstacks")) || FEngineBuildSettings::IsInternalBuild() || FEngineBuildSettings::IsPerforceBuild() || FEngineBuildSettings::IsSourceDistribution();
#endif // UE_LOG_CRASH_CALLSTACK
		if (bGenerateRuntimeCallstack)
		{
			const SIZE_T StackTraceSize = 65535;
			ANSICHAR* StackTrace = (ANSICHAR*)GMalloc->Malloc(StackTraceSize);
			StackTrace[0] = 0;
			// Walk the stack and dump it to the allocated memory. This process usually allocates a lot of memory.
			if (!ContextWrapper)
			{
				ContextWrapper = FWindowsPlatformStackWalk::MakeThreadContextWrapper(ExceptionInfo->ContextRecord, CrashingThreadHandle);
			}
			
			FPlatformStackWalk::StackWalkAndDump(StackTrace, StackTraceSize, 0, ContextWrapper);
			
			if (ExceptionInfo->ExceptionRecord->ExceptionCode != 1 && ExceptionInfo->ExceptionRecord->ExceptionCode != AssertExceptionCode)
			{
				CreateExceptionInfoString(ExceptionInfo->ExceptionRecord);
				FCString::Strncat(GErrorHist, GErrorExceptionDescription, ARRAY_COUNT(GErrorHist));
				FCString::Strncat(GErrorHist, TEXT("\r\n\r\n"), ARRAY_COUNT(GErrorHist));
			}

			FCString::Strncat(GErrorHist, ANSI_TO_TCHAR(StackTrace), ARRAY_COUNT(GErrorHist));

			GMalloc->Free(StackTrace);
		}

		// Make sure any thread context wrapper is released
		if (ContextWrapper)
		{
			FWindowsPlatformStackWalk::ReleaseThreadContextWrapper(ContextWrapper);
		}

#if !UE_BUILD_SHIPPING
		FPlatformStackWalk::UploadLocalSymbols();
#endif
	}

};

#include "Windows/HideWindowsPlatformTypes.h"

TUniquePtr<FCrashReportingThread> GCrashReportingThread = MakeUnique<FCrashReportingThread>();

// #CrashReport: 2015-05-28 This should be named EngineCrashHandler
int32 ReportCrash( LPEXCEPTION_POINTERS ExceptionInfo )
{
	// Only create a minidump the first time this function is called.
	// (Can be called the first time from the RenderThread, then a second time from the MainThread.)
	if (GCrashReportingThread)
	{
		if (FPlatformAtomics::InterlockedIncrement(&ReportCrashCallCount) == 1)
		{
			GCrashReportingThread->OnCrashed(ExceptionInfo);
		}

		// Wait 60s for the crash reporting thread to process the message
		GCrashReportingThread->WaitUntilCrashIsHandled();
	}

	return EXCEPTION_EXECUTE_HANDLER;
}
