// Copyright 1998-2019 Epic Games, Inc. All Rights Reserved.

/*=============================================================================
	Scene.cpp: Scene manager implementation.
=============================================================================*/

#include "CoreMinimal.h"
#include "HAL/ThreadSafeCounter.h"
#include "Stats/Stats.h"
#include "HAL/IConsoleManager.h"
#include "Misc/App.h"
#include "UObject/UObjectIterator.h"
#include "Misc/PackageName.h"
#include "EngineDefines.h"
#include "EngineGlobals.h"
#include "Components/ActorComponent.h"
#include "RHI.h"
#include "RenderingThread.h"
#include "RenderResource.h"
#include "UniformBuffer.h"
#include "SceneTypes.h"
#include "SceneInterface.h"
#include "Components/PrimitiveComponent.h"
#include "MaterialShared.h"
#include "SceneManagement.h"
#include "PrecomputedLightVolume.h"
#include "PrecomputedVolumetricLightmap.h"
#include "Components/LightComponent.h"
#include "GameFramework/WorldSettings.h"
#include "Components/DecalComponent.h"
#include "Components/ReflectionCaptureComponent.h"
#include "ScenePrivateBase.h"
#include "SceneCore.h"
#include "PrimitiveSceneInfo.h"
#include "LightSceneInfo.h"
#include "LightMapRendering.h"
#include "AtmosphereRendering.h"
#include "BasePassRendering.h"
#include "MobileBasePassRendering.h"
#include "LightPropagationVolume.h"
#include "ScenePrivate.h"
#include "RendererModule.h"
#include "StaticMeshResources.h"
#include "ParameterCollection.h"
#include "DistanceFieldAmbientOcclusion.h"
#include "EngineModule.h"
#include "FXSystem.h"
#include "DistanceFieldLightingShared.h"
#include "SpeedTreeWind.h"
#include "Components/WindDirectionalSourceComponent.h"
#include "PlanarReflectionSceneProxy.h"
#include "Engine/StaticMesh.h"
#include "GPUSkinCache.h"
#include "DynamicShadowMapChannelBindingHelper.h"
#include "GPUScene.h"
#if RHI_RAYTRACING
#include "RayTracing/RayTracingDynamicGeometryCollection.h"
#include "RHIGPUReadback.h"
#endif

// Enable this define to do slow checks for components being added to the wrong
// world's scene, when using PIE. This can happen if a PIE component is reattached
// while GWorld is the editor world, for example.
#define CHECK_FOR_PIE_PRIMITIVE_ATTACH_SCENE_MISMATCH	0


DECLARE_CYCLE_STAT(TEXT("DeferredShadingSceneRenderer MotionBlurStartFrame"), STAT_FDeferredShadingSceneRenderer_MotionBlurStartFrame, STATGROUP_SceneRendering);

IMPLEMENT_GLOBAL_SHADER_PARAMETER_STRUCT(FDistanceCullFadeUniformShaderParameters, "PrimitiveFade");

IMPLEMENT_GLOBAL_SHADER_PARAMETER_STRUCT(FDitherUniformShaderParameters, "PrimitiveDither");

/** Global primitive uniform buffer resource containing distance cull faded in */
TGlobalResource< FGlobalDistanceCullFadeUniformBuffer > GDistanceCullFadedInUniformBuffer;

/** Global primitive uniform buffer resource containing dither faded in */
TGlobalResource< FGlobalDitherUniformBuffer > GDitherFadedInUniformBuffer;

static FThreadSafeCounter FSceneViewState_UniqueID;

/**
 * Holds the info to update SpeedTree wind per unique tree object in the scene, instead of per instance
 */
struct FSpeedTreeWindComputation
{
	explicit FSpeedTreeWindComputation() :
		ReferenceCount(1)
	{
	}

	/** SpeedTree wind object */
	FSpeedTreeWind Wind;

	/** Uniform buffer shared between trees of the same type. */
	TUniformBufferRef<FSpeedTreeUniformParameters> UniformBuffer;

	int32 ReferenceCount;
};


/** Default constructor. */
FSceneViewState::FSceneViewState()
	: OcclusionQueryPool(RQT_Occlusion)
{
	UniqueID = FSceneViewState_UniqueID.Increment();
	OcclusionFrameCounter = 0;
	LastRenderTime = -FLT_MAX;
	LastRenderTimeDelta = 0.0f;
	MotionBlurTimeScale = 1.0f;
	PrevViewMatrixForOcclusionQuery.SetIdentity();
	PrevViewOriginForOcclusionQuery = FVector::ZeroVector;
#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
	bIsFreezing = false;
	bIsFrozen = false;
	bIsFrozenViewMatricesCached = false;
#endif
	// Register this object as a resource, so it will receive device reset notifications.
	if ( IsInGameThread() )
	{
		BeginInitResource(this);
	}
	else
	{
		InitResource();
	}
	CachedVisibilityChunk = NULL;
	CachedVisibilityHandlerId = INDEX_NONE;
	CachedVisibilityBucketIndex = INDEX_NONE;
	CachedVisibilityChunkIndex = INDEX_NONE;
	MIDUsedCount = 0;
	TemporalAASampleIndex = 0;
	TemporalAASampleCount = 1;
	FrameIndex = 0;
	DistanceFieldTemporalSampleIndex = 0;
	AOTileIntersectionResources = NULL;
	AOScreenGridResources = NULL;
	bDOFHistory = true;
	bDOFHistory2 = true;
	
	// Sets the mipbias to invalid large number.
	MaterialTextureCachedMipBias = BIG_NUMBER;

	bSequencerIsPaused = false;

	LightPropagationVolume = NULL; 

	bIsStereoView = false;

	bRoundRobinOcclusionEnabled = false;

	HeightfieldLightingAtlas = NULL;

	for (int32 CascadeIndex = 0; CascadeIndex < ARRAY_COUNT(TranslucencyLightingCacheAllocations); CascadeIndex++)
	{
		TranslucencyLightingCacheAllocations[CascadeIndex] = NULL;
	}

	bInitializedGlobalDistanceFieldOrigins = false;
	GlobalDistanceFieldUpdateIndex = 0;

	ShadowOcclusionQueryMaps.Empty(FOcclusionQueryHelpers::MaxBufferedOcclusionFrames);
	ShadowOcclusionQueryMaps.AddZeroed(FOcclusionQueryHelpers::MaxBufferedOcclusionFrames);	

	bValidEyeAdaptation = false;

	LastAutoDownsampleChangeTime = 0;
	SmoothedHalfResTranslucencyGPUDuration = 0;
	SmoothedFullResTranslucencyGPUDuration = 0;
	bShouldAutoDownsampleTranslucency = false;

	PreExposure = 1.f;
	bUpdateLastExposure = false;

#if RHI_RAYTRACING
	VarianceMipTreeDimensions = FIntVector(0);
	VarianceMipTree = new FRWBuffer;

	TotalRayCount = 0;
	TotalRayCountBuffer = new FRWBuffer;
	if (GetFeatureLevel() >= ERHIFeatureLevel::SM5)
	{
		ENQUEUE_RENDER_COMMAND(InitializeSceneViewStateRWBuffer)(
			[this](FRHICommandList&)
			{
				TotalRayCountBuffer->Initialize(sizeof(uint32), 1, PF_R32_UINT);
			});
	}
	bReadbackInitialized = false;
	RayCountGPUReadback = new FRHIGPUMemoryReadback(TEXT("Ray Count Readback"));
#endif
}

void DestroyRenderResource(FRenderResource* RenderResource)
{
	if (RenderResource) 
	{
		ENQUEUE_RENDER_COMMAND(DestroySceneViewStateRenderResource)(
			[RenderResource](FRHICommandList&)
			{
				RenderResource->ReleaseResource();
				delete RenderResource;
			});
	}
}

void DestroyRWBuffer(FRWBuffer* RWBuffer)
{
	ENQUEUE_RENDER_COMMAND(DestroyRWBuffer)(
		[RWBuffer](FRHICommandList&)
		{
			delete RWBuffer;
		});
}

FSceneViewState::~FSceneViewState()
{
	CachedVisibilityChunk = NULL;

	for (int32 CascadeIndex = 0; CascadeIndex < ARRAY_COUNT(TranslucencyLightingCacheAllocations); CascadeIndex++)
	{
		delete TranslucencyLightingCacheAllocations[CascadeIndex];
	}

	DestroyRenderResource(HeightfieldLightingAtlas);
	DestroyRenderResource(AOTileIntersectionResources);
	AOTileIntersectionResources = NULL;
	DestroyRenderResource(AOScreenGridResources);
	AOScreenGridResources = NULL;
	DestroyLightPropagationVolume();

#if RHI_RAYTRACING
	DestroyRWBuffer(VarianceMipTree);
	DestroyRWBuffer(TotalRayCountBuffer);

	delete RayCountGPUReadback;
#endif // RHI_RAYTRACING
}

#if WITH_EDITOR

FPixelInspectorData::FPixelInspectorData()
{
	for (int32 i = 0; i < 2; ++i)
	{
		RenderTargetBufferFinalColor[i] = nullptr;
		RenderTargetBufferDepth[i] = nullptr;
		RenderTargetBufferSceneColor[i] = nullptr;
		RenderTargetBufferHDR[i] = nullptr;
		RenderTargetBufferA[i] = nullptr;
		RenderTargetBufferBCDE[i] = nullptr;
	}
}

void FPixelInspectorData::InitializeBuffers(FRenderTarget* BufferFinalColor, FRenderTarget* BufferSceneColor, FRenderTarget* BufferDepth, FRenderTarget* BufferHDR, FRenderTarget* BufferA, FRenderTarget* BufferBCDE, int32 BufferIndex)
{
	RenderTargetBufferFinalColor[BufferIndex] = BufferFinalColor;
	RenderTargetBufferDepth[BufferIndex] = BufferDepth;
	RenderTargetBufferSceneColor[BufferIndex] = BufferSceneColor;
	RenderTargetBufferHDR[BufferIndex] = BufferHDR;
	RenderTargetBufferA[BufferIndex] = BufferA;
	RenderTargetBufferBCDE[BufferIndex] = BufferBCDE;

	check(RenderTargetBufferBCDE[BufferIndex] != nullptr);
	
	FIntPoint BufferSize = RenderTargetBufferBCDE[BufferIndex]->GetSizeXY();
	check(BufferSize.X == 4 && BufferSize.Y == 1);

	if (RenderTargetBufferA[BufferIndex] != nullptr)
	{
		BufferSize = RenderTargetBufferA[BufferIndex]->GetSizeXY();
		check(BufferSize.X == 1 && BufferSize.Y == 1);
	}
	
	if (RenderTargetBufferFinalColor[BufferIndex] != nullptr)
	{
		BufferSize = RenderTargetBufferFinalColor[BufferIndex]->GetSizeXY();
		//The Final color grab an area and can change depending on the setup
		//It should at least contain 1 pixel but can be 3x3 or more
		check(BufferSize.X > 0 && BufferSize.Y > 0);
	}

	if (RenderTargetBufferDepth[BufferIndex] != nullptr)
	{
		BufferSize = RenderTargetBufferDepth[BufferIndex]->GetSizeXY();
		check(BufferSize.X == 1 && BufferSize.Y == 1);
	}

	if (RenderTargetBufferSceneColor[BufferIndex] != nullptr)
	{
		BufferSize = RenderTargetBufferSceneColor[BufferIndex]->GetSizeXY();
		check(BufferSize.X == 1 && BufferSize.Y == 1);
	}

	if (RenderTargetBufferHDR[BufferIndex] != nullptr)
	{
		BufferSize = RenderTargetBufferHDR[BufferIndex]->GetSizeXY();
		check(BufferSize.X == 1 && BufferSize.Y == 1);
	}
}

bool FPixelInspectorData::AddPixelInspectorRequest(FPixelInspectorRequest *PixelInspectorRequest)
{
	if (PixelInspectorRequest == nullptr)
		return false;
	FVector2D ViewportUV = PixelInspectorRequest->SourceViewportUV;
	if (Requests.Contains(ViewportUV))
		return false;
	
	//Remove the oldest request since the new request use the buffer
	if (Requests.Num() > 1)
	{
		FVector2D FirstKey(-1, -1);
		for (auto kvp : Requests)
		{
			FirstKey = kvp.Key;
			break;
		}
		if (Requests.Contains(FirstKey))
		{
			Requests.Remove(FirstKey);
		}
	}
	Requests.Add(ViewportUV, PixelInspectorRequest);
	return true;
}

#endif //WITH_EDITOR

FDistanceFieldSceneData::FDistanceFieldSceneData(EShaderPlatform ShaderPlatform) 
	: NumObjectsInBuffer(0)
	, ObjectBuffers(NULL)
	, SurfelBuffers(NULL)
	, InstancedSurfelBuffers(NULL)
	, AtlasGeneration(0)
{
	static const auto CVar = IConsoleManager::Get().FindTConsoleVariableDataInt(TEXT("r.GenerateMeshDistanceFields"));

	bTrackAllPrimitives = (DoesPlatformSupportDistanceFieldAO(ShaderPlatform) || DoesPlatformSupportDistanceFieldShadowing(ShaderPlatform)) && CVar->GetValueOnGameThread() != 0;

	bCanUse16BitObjectIndices = RHISupportsBufferLoadTypeConversion(ShaderPlatform);
}

FDistanceFieldSceneData::~FDistanceFieldSceneData() 
{
	delete ObjectBuffers;
}

void FDistanceFieldSceneData::AddPrimitive(FPrimitiveSceneInfo* InPrimitive)
{
	const FPrimitiveSceneProxy* Proxy = InPrimitive->Proxy;

	if ((bTrackAllPrimitives || Proxy->CastsDynamicIndirectShadow())
		&& Proxy->CastsDynamicShadow()
		&& Proxy->AffectsDistanceFieldLighting())
	{
		if (Proxy->SupportsHeightfieldRepresentation())
		{
			HeightfieldPrimitives.Add(InPrimitive);
			FBoxSphereBounds PrimitiveBounds = Proxy->GetBounds();
			FGlobalDFCacheType CacheType = Proxy->IsOftenMoving() ? GDF_Full : GDF_MostlyStatic;
			PrimitiveModifiedBounds[CacheType].Add(FVector4(PrimitiveBounds.Origin, PrimitiveBounds.SphereRadius));
		}

		if (Proxy->SupportsDistanceFieldRepresentation())
		{
			checkSlow(!PendingAddOperations.Contains(InPrimitive));
			checkSlow(!PendingUpdateOperations.Contains(InPrimitive));
			PendingAddOperations.Add(InPrimitive);
		}
	}
}

void FDistanceFieldSceneData::UpdatePrimitive(FPrimitiveSceneInfo* InPrimitive)
{
	const FPrimitiveSceneProxy* Proxy = InPrimitive->Proxy;

	if ((bTrackAllPrimitives || Proxy->CastsDynamicIndirectShadow()) 
		&& Proxy->CastsDynamicShadow() 
		&& Proxy->AffectsDistanceFieldLighting()
		&& Proxy->SupportsDistanceFieldRepresentation() 
		&& !PendingAddOperations.Contains(InPrimitive)
		// This is needed to prevent infinite buildup when DF features are off such that the pending operations don't get consumed
		&& !PendingUpdateOperations.Contains(InPrimitive)
		// This can happen when the primitive fails to allocate from the SDF atlas
		&& InPrimitive->DistanceFieldInstanceIndices.Num() > 0)
	{
		PendingUpdateOperations.Add(InPrimitive);
	}
}

void FDistanceFieldSceneData::RemovePrimitive(FPrimitiveSceneInfo* InPrimitive)
{
	const FPrimitiveSceneProxy* Proxy = InPrimitive->Proxy;

	if ((bTrackAllPrimitives || Proxy->CastsDynamicIndirectShadow()) 
		&& Proxy->AffectsDistanceFieldLighting())
	{
		if (Proxy->SupportsDistanceFieldRepresentation())
		{
			PendingAddOperations.Remove(InPrimitive);
			PendingUpdateOperations.Remove(InPrimitive);

			if (InPrimitive->DistanceFieldInstanceIndices.Num() > 0)
			{
				PendingRemoveOperations.Add(FPrimitiveRemoveInfo(InPrimitive));
			}
			
			InPrimitive->DistanceFieldInstanceIndices.Empty();
		}

		if (Proxy->SupportsHeightfieldRepresentation())
		{
			HeightfieldPrimitives.Remove(InPrimitive);

			FBoxSphereBounds PrimitiveBounds = Proxy->GetBounds();
			FGlobalDFCacheType CacheType = Proxy->IsOftenMoving() ? GDF_Full : GDF_MostlyStatic;
			PrimitiveModifiedBounds[CacheType].Add(FVector4(PrimitiveBounds.Origin, PrimitiveBounds.SphereRadius));
		}
	}
}

void FDistanceFieldSceneData::Release()
{
	if (ObjectBuffers)
	{
		ObjectBuffers->Release();
	}
}

void FDistanceFieldSceneData::VerifyIntegrity()
{
#if DO_CHECK
	check(NumObjectsInBuffer == PrimitiveInstanceMapping.Num());

	for (int32 PrimitiveInstanceIndex = 0; PrimitiveInstanceIndex < PrimitiveInstanceMapping.Num(); PrimitiveInstanceIndex++)
	{
		const FPrimitiveAndInstance& PrimitiveAndInstance = PrimitiveInstanceMapping[PrimitiveInstanceIndex];

		check(PrimitiveAndInstance.Primitive && PrimitiveAndInstance.Primitive->DistanceFieldInstanceIndices.Num() > 0);
		check(PrimitiveAndInstance.Primitive->DistanceFieldInstanceIndices.IsValidIndex(PrimitiveAndInstance.InstanceIndex));

		const int32 InstanceIndex = PrimitiveAndInstance.Primitive->DistanceFieldInstanceIndices[PrimitiveAndInstance.InstanceIndex];
		check(InstanceIndex == PrimitiveInstanceIndex || InstanceIndex == -1);
	}
#endif
}

void FScene::UpdateSceneSettings(AWorldSettings* WorldSettings)
{
	FScene* Scene = this;
	float InDefaultMaxDistanceFieldOcclusionDistance = WorldSettings->DefaultMaxDistanceFieldOcclusionDistance;
	float InGlobalDistanceFieldViewDistance = WorldSettings->GlobalDistanceFieldViewDistance;
	float InDynamicIndirectShadowsSelfShadowingIntensity = FMath::Clamp(WorldSettings->DynamicIndirectShadowsSelfShadowingIntensity, 0.0f, 1.0f);
	ENQUEUE_RENDER_COMMAND(UpdateSceneSettings)(
		[Scene, InDefaultMaxDistanceFieldOcclusionDistance, InGlobalDistanceFieldViewDistance, InDynamicIndirectShadowsSelfShadowingIntensity](FRHICommandList& RHICmdList)
	{
		Scene->DefaultMaxDistanceFieldOcclusionDistance = InDefaultMaxDistanceFieldOcclusionDistance;
		Scene->GlobalDistanceFieldViewDistance = InGlobalDistanceFieldViewDistance;
		Scene->DynamicIndirectShadowsSelfShadowingIntensity = InDynamicIndirectShadowsSelfShadowingIntensity;
	});
}

/**
 * Sets the FX system associated with the scene.
 */
void FScene::SetFXSystem( class FFXSystemInterface* InFXSystem )
{
	FXSystem = InFXSystem;
}

/**
 * Get the FX system associated with the scene.
 */
FFXSystemInterface* FScene::GetFXSystem()
{
	return FXSystem;
}

void FScene::UpdateParameterCollections(const TArray<FMaterialParameterCollectionInstanceResource*>& InParameterCollections)
{
	ENQUEUE_RENDER_COMMAND(UpdateParameterCollectionsCommand)(
		[this, InParameterCollections](FRHICommandList&)
	{
		// Empty the scene's map so any unused uniform buffers will be released
		ParameterCollections.Empty();

		// Add each existing parameter collection id and its uniform buffer
		for (int32 CollectionIndex = 0; CollectionIndex < InParameterCollections.Num(); CollectionIndex++)
		{
			FMaterialParameterCollectionInstanceResource* InstanceResource = InParameterCollections[CollectionIndex];
			ParameterCollections.Add(InstanceResource->GetId(), InstanceResource->GetUniformBuffer());
		}
	});
}

SIZE_T FScene::GetSizeBytes() const
{
	return sizeof(*this) 
		+ Primitives.GetAllocatedSize()
		+ Lights.GetAllocatedSize()
		+ StaticMeshes.GetAllocatedSize()
		+ ExponentialFogs.GetAllocatedSize()
		+ WindSources.GetAllocatedSize()
		+ SpeedTreeVertexFactoryMap.GetAllocatedSize()
		+ SpeedTreeWindComputationMap.GetAllocatedSize()
		+ LightOctree.GetSizeBytes()
		+ PrimitiveOctree.GetSizeBytes();
}

void FScene::CheckPrimitiveArrays()
{
	check(Primitives.Num() == PrimitiveTransforms.Num());
	check(Primitives.Num() == PrimitiveSceneProxies.Num());
	check(Primitives.Num() == PrimitiveBounds.Num());
	check(Primitives.Num() == PrimitiveFlagsCompact.Num());
	check(Primitives.Num() == PrimitiveVisibilityIds.Num());
	check(Primitives.Num() == PrimitiveOcclusionFlags.Num());
	check(Primitives.Num() == PrimitiveComponentIds.Num());
	check(Primitives.Num() == PrimitiveOcclusionBounds.Num());
}


static TAutoConsoleVariable<int32> CVarDoLazyStaticMeshUpdate(
	TEXT("r.DoLazyStaticMeshUpdate"),
	0,
	TEXT("If true, then do not add meshes to the static mesh draw lists until they are visible. Experiemental option. Incompatible with the Mesh Draw Command pipeline."));

static void DoLazyStaticMeshUpdateCVarSinkFunction()
{
	//@todo MeshCommandPipeline r.DoLazyStaticMeshUpdate isn't currently supported.
	/*
	static bool CachedDoLazyStaticMeshUpdate = CVarDoLazyStaticMeshUpdate.GetValueOnGameThread() && !WITH_EDITOR;
	bool DoLazyStaticMeshUpdate = CVarDoLazyStaticMeshUpdate.GetValueOnGameThread() && !WITH_EDITOR;

	if (DoLazyStaticMeshUpdate != CachedDoLazyStaticMeshUpdate)
	{
		CachedDoLazyStaticMeshUpdate = DoLazyStaticMeshUpdate;
		for (TObjectIterator<UWorld> It; It; ++It)
		{
			UWorld* World = *It;
			if (World && World->Scene)
			{
				ENQUEUE_UNIQUE_RENDER_COMMAND_ONEPARAMETER(
					UpdateDoLazyStaticMeshUpdate,
					FScene*, Scene, (FScene*)(World->Scene),
					{
						Scene->UpdateDoLazyStaticMeshUpdate(RHICmdList);
					});
			}
		}
	}
	*/
}

static FAutoConsoleVariableSink CVarDoLazyStaticMeshUpdateSink(FConsoleCommandDelegate::CreateStatic(&DoLazyStaticMeshUpdateCVarSinkFunction));

void FScene::UpdateDoLazyStaticMeshUpdate(FRHICommandListImmediate& CmdList)
{
	//@todo MeshCommandPipeline r.DoLazyStaticMeshUpdate isn't currently supported.
	/*
	bool DoLazyStaticMeshUpdate = CVarDoLazyStaticMeshUpdate.GetValueOnRenderThread() && !WITH_EDITOR;

	for (int32 PrimitiveIndex = 0; PrimitiveIndex < Primitives.Num(); PrimitiveIndex++)
	{
		Primitives[PrimitiveIndex]->UpdateStaticMeshes(CmdList, !DoLazyStaticMeshUpdate);
	}
	*/
}

template<typename T>
static void TArraySwapElements(TArray<T>& Array, int i1, int i2)
{
	T tmp = Array[i1];
	Array[i1] = Array[i2];
	Array[i2] = tmp;
}

void FScene::AddPrimitiveSceneInfo_RenderThread(FRHICommandListImmediate& RHICmdList, FPrimitiveSceneInfo* PrimitiveSceneInfo)
{
	SCOPE_CYCLE_COUNTER(STAT_AddScenePrimitiveRenderThreadTime);
	
	CheckPrimitiveArrays();

	Primitives.Add(PrimitiveSceneInfo);
	PrimitiveTransforms.Add(PrimitiveSceneInfo->Proxy->GetLocalToWorld());
	PrimitiveSceneProxies.Add(PrimitiveSceneInfo->Proxy);
	PrimitiveBounds.AddUninitialized();
	PrimitiveFlagsCompact.AddUninitialized();
	PrimitiveVisibilityIds.AddUninitialized();
	PrimitiveOcclusionFlags.AddUninitialized();
	PrimitiveComponentIds.AddUninitialized();
	PrimitiveOcclusionBounds.AddUninitialized();
	
	const int SourceIndex = PrimitiveSceneProxies.Num() - 1;
	PrimitiveSceneInfo->PackedIndex = SourceIndex;

	{
		bool EntryFound = false;
		int BroadIndex = - 1;
		SIZE_T InsertProxyHash = PrimitiveSceneInfo->Proxy->GetTypeHash();
		//broad phase search for a matching type
		for (BroadIndex = TypeOffsetTable.Num() - 1; BroadIndex >= 0; BroadIndex--)
		{
			// example how the prefix sum of the tails could look like
			// PrimitiveSceneProxies[0,0,0,6,6,6,6,6,2,2,2,2,1,1,1,7,4,8]
			// TypeOffsetTable[3,8,12,15,16,17,18]

			if (TypeOffsetTable[BroadIndex].PrimitiveSceneProxyType == InsertProxyHash)
			{
				EntryFound = true;
				break;
			}
		}

		//new type encountered
		if (EntryFound == false)
		{
			BroadIndex = TypeOffsetTable.Num();
			if (BroadIndex)
			{
				FTypeOffsetTableEntry Entry = TypeOffsetTable[BroadIndex - 1];
				//adding to the end of the list and offset of the tail (will will be incremented once during the while loop)
				TypeOffsetTable.Push(FTypeOffsetTableEntry(InsertProxyHash, Entry.Offset));
			}
			else
			{
				//starting with an empty list and offset zero (will will be incremented once during the while loop)
				TypeOffsetTable.Push(FTypeOffsetTableEntry(InsertProxyHash, 0));
			}
		}

		while (BroadIndex < TypeOffsetTable.Num())
		{
			FTypeOffsetTableEntry& NextEntry = TypeOffsetTable[BroadIndex++];
			int DestIndex = NextEntry.Offset++; //prepare swap and increment

			// example swap chain of inserting a type of 6 at the end
			// PrimitiveSceneProxies[0,0,0,6,6,6,6,6,2,2,2,2,1,1,1,7,4,8,6]
			// PrimitiveSceneProxies[0,0,0,6,6,6,6,6,6,2,2,2,1,1,1,7,4,8,2]
			// PrimitiveSceneProxies[0,0,0,6,6,6,6,6,6,2,2,2,2,1,1,7,4,8,1]
			// PrimitiveSceneProxies[0,0,0,6,6,6,6,6,6,2,2,2,2,1,1,1,4,8,7]
			// PrimitiveSceneProxies[0,0,0,6,6,6,6,6,6,2,2,2,2,1,1,1,7,8,4]
			// PrimitiveSceneProxies[0,0,0,6,6,6,6,6,6,2,2,2,2,1,1,1,7,4,8]

			if (DestIndex != SourceIndex)
			{
				checkfSlow(SourceIndex > DestIndex, TEXT("Corrupted Prefix Sum [%d, %d]"), SourceIndex, DestIndex);
				Primitives[DestIndex]->PackedIndex = SourceIndex;
				Primitives[SourceIndex]->PackedIndex = DestIndex;

				TArraySwapElements(Primitives, DestIndex, SourceIndex);
				TArraySwapElements(PrimitiveTransforms, DestIndex, SourceIndex);
				TArraySwapElements(PrimitiveSceneProxies, DestIndex, SourceIndex);
				TArraySwapElements(PrimitiveBounds, DestIndex, SourceIndex);
				TArraySwapElements(PrimitiveFlagsCompact, DestIndex, SourceIndex);
				TArraySwapElements(PrimitiveVisibilityIds, DestIndex, SourceIndex);
				TArraySwapElements(PrimitiveOcclusionFlags, DestIndex, SourceIndex);
				TArraySwapElements(PrimitiveComponentIds, DestIndex, SourceIndex);
				TArraySwapElements(PrimitiveOcclusionBounds, DestIndex, SourceIndex);

				AddPrimitiveToUpdateGPU(*this, DestIndex);
			}
		}
	}

	CheckPrimitiveArrays();

	// Add the primitive to its shadow parent's linked list of children.
	// Note: must happen before AddToScene because AddToScene depends on LightingAttachmentRoot
	PrimitiveSceneInfo->LinkAttachmentGroup();

	// Set lod Parent information if valid
	PrimitiveSceneInfo->LinkLODParentComponent();

	// Add the primitive to the scene.
	PrimitiveSceneInfo->AddToScene(RHICmdList, true);

	//@todo MeshCommandPipeline r.DoLazyStaticMeshUpdate isn't currently supported.
	/*
#if WITH_EDITOR
	PrimitiveSceneInfo->AddToScene(RHICmdList, true);
#else
	const bool bAddToDrawLists = !(CVarDoLazyStaticMeshUpdate.GetValueOnRenderThread());
	if (bAddToDrawLists)
	{
		PrimitiveSceneInfo->AddToScene(RHICmdList, true);
	}
	else
	{
		PrimitiveSceneInfo->AddToScene(RHICmdList, true, false);
		PrimitiveSceneInfo->BeginDeferredUpdateStaticMeshes();
	}
#endif
	*/

	AddPrimitiveToUpdateGPU(*this, SourceIndex);

	DistanceFieldSceneData.AddPrimitive(PrimitiveSceneInfo);

	// LOD Parent, if this is LOD parent, we should update Proxy Scene Info
	// LOD parent gets removed WHEN no children is accessing
	// LOD parent can be recreated as scene updates
	// I update if the parent component ID is still valid
	// @Todo : really remove it if you know this is being destroyed - should happen from game thread as streaming in/out
	SceneLODHierarchy.UpdateNodeSceneInfo(PrimitiveSceneInfo->PrimitiveComponentId, PrimitiveSceneInfo);
}

/**
 * Verifies that a component is added to the proper scene
 *
 * @param Component Component to verify
 * @param World World who's scene the primitive is being attached to
 */
FORCEINLINE static void VerifyProperPIEScene(UPrimitiveComponent* Component, UWorld* World)
{
#if CHECK_FOR_PIE_PRIMITIVE_ATTACH_SCENE_MISMATCH
	checkf(Component->GetOuter() == GetTransientPackage() || 
		(FPackageName::GetLongPackageAssetName(Component->GetOutermost()->GetName()).StartsWith(PLAYWORLD_PACKAGE_PREFIX) == 
		FPackageName::GetLongPackageAssetName(World->GetOutermost()->GetName()).StartsWith(PLAYWORLD_PACKAGE_PREFIX)),
		TEXT("The component %s was added to the wrong world's scene (due to PIE). The callstack should tell you why"), 
		*Component->GetFullName()
		);
#endif
}

void FPersistentUniformBuffers::Initialize()
{
	FViewUniformShaderParameters ViewUniformBufferParameters;
	ViewUniformBuffer = TUniformBufferRef<FViewUniformShaderParameters>::CreateUniformBufferImmediate(ViewUniformBufferParameters, UniformBuffer_MultiFrame, EUniformBufferValidation::None);

	FInstancedViewUniformShaderParameters InstancedViewUniformBufferParameters;
	InstancedViewUniformBuffer = TUniformBufferRef<FInstancedViewUniformShaderParameters>::CreateUniformBufferImmediate(InstancedViewUniformBufferParameters, UniformBuffer_MultiFrame, EUniformBufferValidation::None);

	FSceneTexturesUniformParameters DepthPassParameters;
	DepthPassUniformBuffer = TUniformBufferRef<FSceneTexturesUniformParameters>::CreateUniformBufferImmediate(DepthPassParameters, UniformBuffer_MultiFrame, EUniformBufferValidation::None);

	FOpaqueBasePassUniformParameters BasePassParameters;
	OpaqueBasePassUniformBuffer = TUniformBufferRef<FOpaqueBasePassUniformParameters>::CreateUniformBufferImmediate(BasePassParameters, UniformBuffer_MultiFrame, EUniformBufferValidation::None);

	FTranslucentBasePassUniformParameters TranslucentBasePassParameters;
	TranslucentBasePassUniformBuffer = TUniformBufferRef<FTranslucentBasePassUniformParameters>::CreateUniformBufferImmediate(TranslucentBasePassParameters, UniformBuffer_MultiFrame, EUniformBufferValidation::None);

	FReflectionCaptureShaderData ReflectionCaptureParameters;
	ReflectionCaptureUniformBuffer = TUniformBufferRef<FReflectionCaptureShaderData>::CreateUniformBufferImmediate(ReflectionCaptureParameters, UniformBuffer_MultiFrame);

	CSMShadowDepthViewUniformBuffer = TUniformBufferRef<FViewUniformShaderParameters>::CreateUniformBufferImmediate(ViewUniformBufferParameters, UniformBuffer_MultiFrame, EUniformBufferValidation::None);

	FShadowDepthPassUniformParameters CSMShadowDepthPassParameters;
	CSMShadowDepthPassUniformBuffer = TUniformBufferRef<FShadowDepthPassUniformParameters>::CreateUniformBufferImmediate(CSMShadowDepthPassParameters, UniformBuffer_MultiFrame, EUniformBufferValidation::None);

	FDistortionPassUniformParameters DistortionPassParameters;
	DistortionPassUniformBuffer = TUniformBufferRef<FDistortionPassUniformParameters>::CreateUniformBufferImmediate(DistortionPassParameters, UniformBuffer_MultiFrame, EUniformBufferValidation::None);

	FSceneTexturesUniformParameters VelocityPassParameters;
	VelocityPassUniformBuffer = TUniformBufferRef<FSceneTexturesUniformParameters>::CreateUniformBufferImmediate(VelocityPassParameters, UniformBuffer_MultiFrame, EUniformBufferValidation::None);

	FSceneTexturesUniformParameters HitProxyPassParameters;
	HitProxyPassUniformBuffer = TUniformBufferRef<FSceneTexturesUniformParameters>::CreateUniformBufferImmediate(HitProxyPassParameters, UniformBuffer_MultiFrame, EUniformBufferValidation::None);

	FSceneTexturesUniformParameters MeshDecalPassParameters;
	MeshDecalPassUniformBuffer = TUniformBufferRef<FSceneTexturesUniformParameters>::CreateUniformBufferImmediate(MeshDecalPassParameters, UniformBuffer_MultiFrame, EUniformBufferValidation::None);

	FLightmapDensityPassUniformParameters LightmapDensityPassParameters;
	LightmapDensityPassUniformBuffer = TUniformBufferRef<FLightmapDensityPassUniformParameters>::CreateUniformBufferImmediate(LightmapDensityPassParameters, UniformBuffer_MultiFrame, EUniformBufferValidation::None);

	FDebugViewModePassPassUniformParameters DebugViewModePassParameters;
	DebugViewModePassUniformBuffer = TUniformBufferRef<FDebugViewModePassPassUniformParameters>::CreateUniformBufferImmediate(DebugViewModePassParameters, UniformBuffer_MultiFrame, EUniformBufferValidation::None);

	VoxelizeVolumeViewUniformBuffer = TUniformBufferRef<FViewUniformShaderParameters>::CreateUniformBufferImmediate(ViewUniformBufferParameters, UniformBuffer_MultiFrame, EUniformBufferValidation::None);

	FVoxelizeVolumePassUniformParameters VoxelizeVolumePassParameters;
	VoxelizeVolumePassUniformBuffer = TUniformBufferRef<FVoxelizeVolumePassUniformParameters>::CreateUniformBufferImmediate(VoxelizeVolumePassParameters, UniformBuffer_MultiFrame, EUniformBufferValidation::None);

	FSceneTexturesUniformParameters ConvertToUniformMeshPassParameters;
	ConvertToUniformMeshPassUniformBuffer = TUniformBufferRef<FSceneTexturesUniformParameters>::CreateUniformBufferImmediate(ConvertToUniformMeshPassParameters, UniformBuffer_MultiFrame, EUniformBufferValidation::None);

	FSceneTexturesUniformParameters CustomDepthPassUniformBufferParameters;
	CustomDepthPassUniformBuffer = TUniformBufferRef<FSceneTexturesUniformParameters>::CreateUniformBufferImmediate(CustomDepthPassUniformBufferParameters, UniformBuffer_MultiFrame, EUniformBufferValidation::None);

	FMobileSceneTextureUniformParameters MobileCustomDepthPassUniformBufferParameters;
	MobileCustomDepthPassUniformBuffer = TUniformBufferRef<FMobileSceneTextureUniformParameters>::CreateUniformBufferImmediate(MobileCustomDepthPassUniformBufferParameters, UniformBuffer_MultiFrame, EUniformBufferValidation::None);

	CustomDepthViewUniformBuffer = TUniformBufferRef<FViewUniformShaderParameters>::CreateUniformBufferImmediate(ViewUniformBufferParameters, UniformBuffer_MultiFrame, EUniformBufferValidation::None);

	FMobileShadowDepthPassUniformParameters MobileCSMShadowDepthPassParameters;
	MobileCSMShadowDepthPassUniformBuffer = TUniformBufferRef<FMobileShadowDepthPassUniformParameters>::CreateUniformBufferImmediate(MobileCSMShadowDepthPassParameters, UniformBuffer_MultiFrame, EUniformBufferValidation::None);

	FMobileBasePassUniformParameters MobileBasePassUniformParameters;
	MobileOpaqueBasePassUniformBuffer = TUniformBufferRef<FMobileBasePassUniformParameters>::CreateUniformBufferImmediate(MobileBasePassUniformParameters, UniformBuffer_MultiFrame, EUniformBufferValidation::None);
	MobileTranslucentBasePassUniformBuffer = TUniformBufferRef<FMobileBasePassUniformParameters>::CreateUniformBufferImmediate(MobileBasePassUniformParameters, UniformBuffer_MultiFrame, EUniformBufferValidation::None);
	
	FMobileDistortionPassUniformParameters MobileDistortionPassUniformParameters;
	MobileDistortionPassUniformBuffer = TUniformBufferRef<FMobileDistortionPassUniformParameters>::CreateUniformBufferImmediate(MobileDistortionPassUniformParameters, UniformBuffer_MultiFrame, EUniformBufferValidation::None);
	
	FMobileDirectionalLightShaderParameters MobileDirectionalLightShaderParameters = {};
	for (int32 Index = 0; Index < ARRAY_COUNT(MobileDirectionalLightUniformBuffers); ++Index)
	{
		MobileDirectionalLightUniformBuffers[Index] = TUniformBufferRef<FMobileDirectionalLightShaderParameters>::CreateUniformBufferImmediate(MobileDirectionalLightShaderParameters, UniformBuffer_MultiFrame, EUniformBufferValidation::None);
	}
	

#if WITH_EDITOR
	FSceneTexturesUniformParameters EditorSelectionPassParameters;
	EditorSelectionPassUniformBuffer = TUniformBufferRef<FSceneTexturesUniformParameters>::CreateUniformBufferImmediate(EditorSelectionPassParameters, UniformBuffer_MultiFrame, EUniformBufferValidation::None);
#endif
}

bool FPersistentUniformBuffers::UpdateViewUniformBuffer(const FViewInfo& View)
{
	// ViewUniformBuffer can be cached by mesh commands, so we need to update it every time we change current view.
	if (CachedView != &View)
	{
		ViewUniformBuffer.UpdateUniformBufferImmediate(*View.CachedViewUniformShaderParameters);

		if (View.IsInstancedStereoPass() && View.Family->Views.Num() > 0)
		{
			// When drawing the left eye in a stereo scene, copy the right eye view values into the instanced view uniform buffer.
			const EStereoscopicPass StereoPassIndex = (View.StereoPass != eSSP_FULL) ? eSSP_RIGHT_EYE : eSSP_FULL;

			const FViewInfo& InstancedView = static_cast<const FViewInfo&>(View.Family->GetStereoEyeView(StereoPassIndex));
			InstancedViewUniformBuffer.UpdateUniformBufferImmediate(reinterpret_cast<FInstancedViewUniformShaderParameters&>(*InstancedView.CachedViewUniformShaderParameters));
		}

		CachedView = &View;
		return true;
	}
	return false;
}

FScene::FScene(UWorld* InWorld, bool bInRequiresHitProxies, bool bInIsEditorScene, bool bCreateFXSystem, ERHIFeatureLevel::Type InFeatureLevel)
:	FSceneInterface(InFeatureLevel)
,	World(InWorld)
,	FXSystem(NULL)
,	bStaticDrawListsMobileHDR(false)
,	bStaticDrawListsMobileHDR32bpp(false)
,	StaticDrawListsEarlyZPassMode(0)
,	StaticDrawShaderPipelines(0)
,	bScenesPrimitivesNeedStaticMeshElementUpdate(false)
,	SkyLight(NULL)
,	SimpleDirectionalLight(NULL)
,	SunLight(NULL)
,	ReflectionSceneData(InFeatureLevel)
,	IndirectLightingCache(InFeatureLevel)
,	DistanceFieldSceneData(GShaderPlatformForFeatureLevel[InFeatureLevel])
,	PreshadowCacheLayout(0, 0, 0, 0, false, false)
,	AtmosphericFog(NULL)
,	PrecomputedVisibilityHandler(NULL)
,	LightOctree(FVector::ZeroVector,HALF_WORLD_MAX)
,	PrimitiveOctree(FVector::ZeroVector,HALF_WORLD_MAX)
,	bRequiresHitProxies(bInRequiresHitProxies)
,	bIsEditorScene(bInIsEditorScene)
,	NumUncachedStaticLightingInteractions(0)
,	NumUnbuiltReflectionCaptures(0)
,	NumMobileStaticAndCSMLights_RenderThread(0)
,	NumMobileMovableDirectionalLights_RenderThread(0)
,	GPUSkinCache(nullptr)
,	SceneLODHierarchy(this)
,	DefaultMaxDistanceFieldOcclusionDistance(InWorld->GetWorldSettings()->DefaultMaxDistanceFieldOcclusionDistance)
,	GlobalDistanceFieldViewDistance(InWorld->GetWorldSettings()->GlobalDistanceFieldViewDistance)
,	DynamicIndirectShadowsSelfShadowingIntensity(FMath::Clamp(InWorld->GetWorldSettings()->DynamicIndirectShadowsSelfShadowingIntensity, 0.0f, 1.0f))
,	ReadOnlyCVARCache(FReadOnlyCVARCache::Get())
#if RHI_RAYTRACING
, RayTracingDynamicGeometryCollection(nullptr)
#endif
,	NumVisibleLights_GameThread(0)
,	NumEnabledSkylights_GameThread(0)
,	SceneFrameNumber(0)
{
	FMemory::Memzero(MobileDirectionalLights);

	check(World);
	World->Scene = this;

	FeatureLevel = World->FeatureLevel;

	static auto* MobileHDRCvar = IConsoleManager::Get().FindTConsoleVariableDataInt(TEXT("r.MobileHDR"));
	static auto* MobileHDR32bppModeCvar = IConsoleManager::Get().FindTConsoleVariableDataInt(TEXT("r.MobileHDR32bppMode"));
	bStaticDrawListsMobileHDR = MobileHDRCvar->GetValueOnAnyThread() == 1;
	bStaticDrawListsMobileHDR32bpp = bStaticDrawListsMobileHDR && (GSupportsRenderTargetFormat_PF_FloatRGBA == false || MobileHDR32bppModeCvar->GetValueOnAnyThread() != 0);

	static auto* EarlyZPassCvar = IConsoleManager::Get().FindTConsoleVariableDataInt(TEXT("r.EarlyZPass"));
	StaticDrawListsEarlyZPassMode = EarlyZPassCvar->GetValueOnAnyThread();

	static auto* ShaderPipelinesCvar = IConsoleManager::Get().FindTConsoleVariableDataInt(TEXT("r.ShaderPipelines"));
	StaticDrawShaderPipelines = ShaderPipelinesCvar->GetValueOnAnyThread();

	if (World->FXSystem)
	{
		FFXSystemInterface::Destroy(World->FXSystem);
	}

	if (bCreateFXSystem)
	{
		World->CreateFXSystem();
	}
	else
	{
		World->FXSystem = NULL;
		SetFXSystem(NULL);
	}

	if (IsGPUSkinCacheAvailable())
	{
		const bool bRequiresMemoryLimit = !bInIsEditorScene;
		GPUSkinCache = new FGPUSkinCache(bRequiresMemoryLimit);
	}

#if RHI_RAYTRACING
	if (IsRayTracingEnabled())
	{
		RayTracingDynamicGeometryCollection = new FRayTracingDynamicGeometryCollection();
	}
#endif

	World->UpdateParameterCollectionInstances(false);

	FPersistentUniformBuffers* PersistentUniformBuffers = &UniformBuffers;
	ENQUEUE_RENDER_COMMAND(InitializeUniformBuffers)(
		[PersistentUniformBuffers](FRHICommandListImmediate& RHICmdList)
	{
		PersistentUniformBuffers->Initialize();
	});
}

FScene::~FScene()
{
#if 0 // if you have component that has invalid scene, try this code to see this is reason. 
	for (FObjectIterator Iter(UActorComponent::StaticClass()); Iter; ++Iter)
	{
		UActorComponent * ActorComp = CastChecked<UActorComponent>(*Iter);
		if (ActorComp->GetScene() == this)
		{
			UE_LOG(LogRenderer, Log, TEXT("%s's scene is going to get invalidated"), *ActorComp->GetName());
		}
	}
#endif

	ReflectionSceneData.CubemapArray.ReleaseResource();
	IndirectLightingCache.ReleaseResource();
	DistanceFieldSceneData.Release();

	if (AtmosphericFog)
	{
		delete AtmosphericFog;
		AtmosphericFog = nullptr;
	}

	if (GPUSkinCache)
	{
		delete GPUSkinCache;
		GPUSkinCache = nullptr;
	}

#if RHI_RAYTRACING
	if (RayTracingDynamicGeometryCollection)
	{
		delete RayTracingDynamicGeometryCollection;
		RayTracingDynamicGeometryCollection = nullptr;
	}
#endif
}

void FScene::AddPrimitive(UPrimitiveComponent* Primitive)
{
	SCOPE_CYCLE_COUNTER(STAT_AddScenePrimitiveGT);

	checkf(!Primitive->IsUnreachable(), TEXT("%s"), *Primitive->GetFullName());

	const float WorldTime = GetWorld()->GetTimeSeconds();
	// Save the world transform for next time the primitive is added to the scene
	float DeltaTime = WorldTime - Primitive->LastSubmitTime;
	if ( DeltaTime < -0.0001f || Primitive->LastSubmitTime < 0.0001f )
	{
		// Time was reset?
		Primitive->LastSubmitTime = WorldTime;
	}
	else if ( DeltaTime > 0.0001f )
	{
		// First call for the new frame?
		Primitive->LastSubmitTime = WorldTime;
	}

	// Create the primitive's scene proxy.
	FPrimitiveSceneProxy* PrimitiveSceneProxy = Primitive->CreateSceneProxy();
	Primitive->SceneProxy = PrimitiveSceneProxy;
	if(!PrimitiveSceneProxy)
	{
		// Primitives which don't have a proxy are irrelevant to the scene manager.
		return;
	}

	// Create the primitive scene info.
	FPrimitiveSceneInfo* PrimitiveSceneInfo = new FPrimitiveSceneInfo(Primitive, this);
	PrimitiveSceneProxy->PrimitiveSceneInfo = PrimitiveSceneInfo;

	// Cache the primitive's initial transform.
	FMatrix RenderMatrix = Primitive->GetRenderMatrix();
	FVector AttachmentRootPosition(0);

	AActor* AttachmentRoot = Primitive->GetAttachmentRootActor();
	if (AttachmentRoot)
	{
		AttachmentRootPosition = AttachmentRoot->GetActorLocation();
	}

	struct FCreateRenderThreadParameters
	{
		FPrimitiveSceneProxy* PrimitiveSceneProxy;
		FMatrix RenderMatrix;
		FBoxSphereBounds WorldBounds;
		FVector AttachmentRootPosition;
		FBoxSphereBounds LocalBounds;
	};
	FCreateRenderThreadParameters Params =
	{
		PrimitiveSceneProxy,
		RenderMatrix,
		Primitive->Bounds,
		AttachmentRootPosition,
		Primitive->CalcBounds(FTransform::Identity)
	};

	// Help track down primitive with bad bounds way before the it gets to the Renderer
	ensureMsgf(!Primitive->Bounds.BoxExtent.ContainsNaN() && !Primitive->Bounds.Origin.ContainsNaN() && !FMath::IsNaN(Primitive->Bounds.SphereRadius) && FMath::IsFinite(Primitive->Bounds.SphereRadius),
			TEXT("Nans found on Bounds for Primitive %s: Origin %s, BoxExtent %s, SphereRadius %f"), *Primitive->GetName(), *Primitive->Bounds.Origin.ToString(), *Primitive->Bounds.BoxExtent.ToString(), Primitive->Bounds.SphereRadius);

	// Create any RenderThreadResources required.
	ENQUEUE_RENDER_COMMAND(CreateRenderThreadResourcesCommand)(
		[Params](FRHICommandListImmediate& RHICmdList)
	{
		FPrimitiveSceneProxy* SceneProxy = Params.PrimitiveSceneProxy;
		FScopeCycleCounter Context(SceneProxy->GetStatId());
		SceneProxy->SetTransform(Params.RenderMatrix, Params.WorldBounds, Params.LocalBounds, Params.AttachmentRootPosition);

		// Create any RenderThreadResources required.
		SceneProxy->CreateRenderThreadResources();
	});

	INC_DWORD_STAT_BY( STAT_GameToRendererMallocTotal, PrimitiveSceneProxy->GetMemoryFootprint() + PrimitiveSceneInfo->GetMemoryFootprint() );

	// Verify the primitive is valid (this will compile away to a nop without CHECK_FOR_PIE_PRIMITIVE_ATTACH_SCENE_MISMATCH)
	VerifyProperPIEScene(Primitive, World);

	// Increment the attachment counter, the primitive is about to be attached to the scene.
	Primitive->AttachmentCounter.Increment();

	// Send a command to the rendering thread to add the primitive to the scene.
	FScene* Scene = this;
	ENQUEUE_RENDER_COMMAND(AddPrimitiveCommand)(
		[Scene, PrimitiveSceneInfo](FRHICommandListImmediate& RHICmdList)
		{
			FScopeCycleCounter Context(PrimitiveSceneInfo->Proxy->GetStatId());
			Scene->AddPrimitiveSceneInfo_RenderThread(RHICmdList, PrimitiveSceneInfo);
		});

}

static int32 GWarningOnRedundantTransformUpdate = 0;
static FAutoConsoleVariableRef CVarWarningOnRedundantTransformUpdate(
	TEXT("r.WarningOnRedundantTransformUpdate"),
	GWarningOnRedundantTransformUpdate,
	TEXT("Produce a warning when UpdatePrimitiveTransform_RenderThread is called redundantly."),
	ECVF_Default
);


void FScene::UpdatePrimitiveTransform_RenderThread(FRHICommandListImmediate& RHICmdList, FPrimitiveSceneProxy* PrimitiveSceneProxy, const FBoxSphereBounds& WorldBounds, const FBoxSphereBounds& LocalBounds, const FMatrix& LocalToWorld, const FVector& AttachmentRootPosition)
{
	SCOPE_CYCLE_COUNTER(STAT_UpdatePrimitiveTransformRenderThreadTime);

	FPrimitiveSceneInfo* PrimitiveSceneInfo = PrimitiveSceneProxy->GetPrimitiveSceneInfo();

	const bool bUpdateStaticDrawLists = !PrimitiveSceneProxy->StaticElementsAlwaysUseProxyPrimitiveUniformBuffer() 
		|| !UseGPUScene(GMaxRHIShaderPlatform, GetFeatureLevel());

	// Remove the primitive from the scene at its old location
	// (note that the octree update relies on the bounds not being modified yet).
	PrimitiveSceneInfo->RemoveFromScene(bUpdateStaticDrawLists);

	VelocityData.UpdateTransform(PrimitiveSceneInfo->PrimitiveComponentId, LocalToWorld, PrimitiveSceneProxy->GetLocalToWorld());

	if (GWarningOnRedundantTransformUpdate && PrimitiveSceneProxy->WouldSetTransformBeRedundant(LocalToWorld, WorldBounds, LocalBounds, AttachmentRootPosition))
	{
		UE_LOG(LogRenderer, Warning, TEXT("Redundant UpdatePrimitiveTransform_RenderThread Owner: %s, Resource: %s, Level: %s"), *PrimitiveSceneProxy->GetOwnerName().ToString(), *PrimitiveSceneProxy->GetResourceName().ToString(), *PrimitiveSceneProxy->GetLevelName().ToString());
	}
	// Update the primitive transform.
	PrimitiveSceneProxy->SetTransform(LocalToWorld, WorldBounds, LocalBounds, AttachmentRootPosition);
	PrimitiveTransforms[PrimitiveSceneInfo->PackedIndex] = LocalToWorld;

	if (!RHISupportsVolumeTextures(GetFeatureLevel())
		&& (PrimitiveSceneProxy->IsMovable() || PrimitiveSceneProxy->NeedsUnbuiltPreviewLighting() || PrimitiveSceneProxy->GetLightmapType() == ELightmapType::ForceVolumetric))
	{
		PrimitiveSceneInfo->MarkIndirectLightingCacheBufferDirty();
	}

	AddPrimitiveToUpdateGPU(*this, PrimitiveSceneInfo->PackedIndex);

	DistanceFieldSceneData.UpdatePrimitive(PrimitiveSceneInfo);

	// If the primitive has static mesh elements, it should have returned true from ShouldRecreateProxyOnUpdateTransform!
	check(!(bUpdateStaticDrawLists && PrimitiveSceneInfo->StaticMeshes.Num()));

	// Re-add the primitive to the scene with the new transform.
	PrimitiveSceneInfo->AddToScene(RHICmdList, bUpdateStaticDrawLists);
}

void FScene::UpdatePrimitiveTransform(UPrimitiveComponent* Primitive)
{
	SCOPE_CYCLE_COUNTER(STAT_UpdatePrimitiveTransformGT);

	// Save the world transform for next time the primitive is added to the scene
	const float WorldTime = GetWorld()->GetTimeSeconds();
	float DeltaTime = WorldTime - Primitive->LastSubmitTime;
	if ( DeltaTime < -0.0001f || Primitive->LastSubmitTime < 0.0001f )
	{
		// Time was reset?
		Primitive->LastSubmitTime = WorldTime;
	}
	else if ( DeltaTime > 0.0001f )
	{
		// First call for the new frame?
		Primitive->LastSubmitTime = WorldTime;
	}

	if(Primitive->SceneProxy)
	{
		// Check if the primitive needs to recreate its proxy for the transform update.
		if(Primitive->ShouldRecreateProxyOnUpdateTransform())
		{
			// Re-add the primitive from scratch to recreate the primitive's proxy.
			RemovePrimitive(Primitive);
			AddPrimitive(Primitive);
		}
		else
		{
			FVector AttachmentRootPosition(0);

			AActor* Actor = Primitive->GetAttachmentRootActor();
			if (Actor != NULL)
			{
				AttachmentRootPosition = Actor->GetActorLocation();
			}

			struct FPrimitiveUpdateParams
			{
				FScene* Scene;
				FPrimitiveSceneProxy* PrimitiveSceneProxy;
				FBoxSphereBounds WorldBounds;
				FBoxSphereBounds LocalBounds;
				FMatrix LocalToWorld;
				FVector AttachmentRootPosition;
			};

			FPrimitiveUpdateParams UpdateParams;
			UpdateParams.Scene = this;
			UpdateParams.PrimitiveSceneProxy = Primitive->SceneProxy;
			UpdateParams.WorldBounds = Primitive->Bounds;
			UpdateParams.LocalToWorld = Primitive->GetRenderMatrix();
			UpdateParams.AttachmentRootPosition = AttachmentRootPosition;
			UpdateParams.LocalBounds = Primitive->CalcBounds(FTransform::Identity);

			// Help track down primitive with bad bounds way before the it gets to the Renderer
			ensureMsgf(!Primitive->Bounds.BoxExtent.ContainsNaN() && !Primitive->Bounds.Origin.ContainsNaN() && !FMath::IsNaN(Primitive->Bounds.SphereRadius) && FMath::IsFinite(Primitive->Bounds.SphereRadius),
				TEXT("Nans found on Bounds for Primitive %s: Origin %s, BoxExtent %s, SphereRadius %f"), *Primitive->GetName(), *Primitive->Bounds.Origin.ToString(), *Primitive->Bounds.BoxExtent.ToString(), Primitive->Bounds.SphereRadius);

			ENQUEUE_RENDER_COMMAND(UpdateTransformCommand)(
				[UpdateParams](FRHICommandListImmediate& RHICmdList)
				{
					FScopeCycleCounter Context(UpdateParams.PrimitiveSceneProxy->GetStatId());
					UpdateParams.Scene->UpdatePrimitiveTransform_RenderThread(RHICmdList, UpdateParams.PrimitiveSceneProxy, UpdateParams.WorldBounds, UpdateParams.LocalBounds, UpdateParams.LocalToWorld, UpdateParams.AttachmentRootPosition);
				});
		}
	}
	else
	{
		// If the primitive doesn't have a scene info object yet, it must be added from scratch.
		AddPrimitive(Primitive);
	}
}

void FScene::UpdatePrimitiveLightingAttachmentRoot(UPrimitiveComponent* Primitive)
{
	const UPrimitiveComponent* NewLightingAttachmentRoot = Cast<UPrimitiveComponent>(Primitive->GetAttachmentRoot());

	if (NewLightingAttachmentRoot == Primitive)
	{
		NewLightingAttachmentRoot = NULL;
	}

	FPrimitiveComponentId NewComponentId = NewLightingAttachmentRoot ? NewLightingAttachmentRoot->ComponentId : FPrimitiveComponentId();

	if (Primitive->SceneProxy)
	{
		FPrimitiveSceneProxy* Proxy = Primitive->SceneProxy;
		ENQUEUE_RENDER_COMMAND(UpdatePrimitiveAttachment)(
			[Proxy,NewComponentId](FRHICommandList&)
			{
				FPrimitiveSceneInfo* PrimitiveInfo = Proxy->GetPrimitiveSceneInfo();
				PrimitiveInfo->UnlinkAttachmentGroup();
				PrimitiveInfo->LightingAttachmentRoot = NewComponentId;
				PrimitiveInfo->LinkAttachmentGroup();
			});
	}
}

void FScene::UpdatePrimitiveAttachment(UPrimitiveComponent* Primitive)
{
	TArray<USceneComponent*, TInlineAllocator<1> > ProcessStack;
	ProcessStack.Push(Primitive);

	// Walk down the tree updating, because the scene's attachment data structures must be updated if the root of the attachment tree changes
	while (ProcessStack.Num() > 0)
	{
		USceneComponent* Current = ProcessStack.Pop(/*bAllowShrinking=*/ false);
		if (Current)
		{
			UPrimitiveComponent* CurrentPrimitive = Cast<UPrimitiveComponent>(Current);

			if (CurrentPrimitive
				&& CurrentPrimitive->GetWorld() 
				&& CurrentPrimitive->GetWorld()->Scene == this
				&& CurrentPrimitive->ShouldComponentAddToScene())
			{
				UpdatePrimitiveLightingAttachmentRoot(CurrentPrimitive);
			}

			ProcessStack.Append(Current->GetAttachChildren());
		}
	}
}

void FScene::UpdatePrimitiveDistanceFieldSceneData_GameThread(UPrimitiveComponent* Primitive)
{
	check(IsInGameThread());

	if (Primitive->SceneProxy)
	{
		Primitive->LastSubmitTime = GetWorld()->GetTimeSeconds();

		ENQUEUE_RENDER_COMMAND(UpdatePrimDFSceneDataCmd)(
			[this, PrimitiveSceneProxy = Primitive->SceneProxy](FRHICommandList&)
			{
				if (PrimitiveSceneProxy && PrimitiveSceneProxy->GetPrimitiveSceneInfo())
				{
					FPrimitiveSceneInfo* Info = PrimitiveSceneProxy->GetPrimitiveSceneInfo();
					DistanceFieldSceneData.UpdatePrimitive(Info);
				}
			});
	}
}

FPrimitiveSceneInfo* FScene::GetPrimitiveSceneInfo(int32 PrimitiveIndex)
{
	if(Primitives.IsValidIndex(PrimitiveIndex))
	{
		return Primitives[PrimitiveIndex];
	}
	return NULL;
}

void FScene::RemovePrimitiveSceneInfo_RenderThread(FPrimitiveSceneInfo* PrimitiveSceneInfo)
{
	SCOPE_CYCLE_COUNTER(STAT_RemoveScenePrimitiveTime);

	// clear it up, parent is getting removed
	SceneLODHierarchy.UpdateNodeSceneInfo(PrimitiveSceneInfo->PrimitiveComponentId, nullptr);

	CheckPrimitiveArrays();

	int32 PrimitiveIndex = PrimitiveSceneInfo->PackedIndex;
	{
		int BroadIndex = -1;
		SIZE_T InsertProxyHash = PrimitiveSceneInfo->Proxy->GetTypeHash();
		//broad phase search for a matching type
		for (BroadIndex = TypeOffsetTable.Num() - 1; BroadIndex >= 0; BroadIndex--)
		{
			// example how the prefix sum of the tails could look like
			// PrimitiveSceneProxies[0,0,0,6,6,6,6,6,2,2,2,2,1,1,1,7,4,8]
			// TypeOffsetTable[3,8,12,15,16,17,18]

			if (TypeOffsetTable[BroadIndex].PrimitiveSceneProxyType == InsertProxyHash)
			{
				const int InsertionOffset = TypeOffsetTable[BroadIndex].Offset;
				const int PrevOffset = BroadIndex > 0 ? TypeOffsetTable[BroadIndex - 1].Offset : 0;
				checkfSlow(PrimitiveIndex >= PrevOffset && PrimitiveIndex < InsertionOffset, TEXT("PrimitiveIndex %d not in Bucket Range [%d, %d]"), PrimitiveIndex, PrevOffset, InsertionOffset);
				break;
			}
		}

		int SourceIndex = PrimitiveIndex;
		const int SavedBroadIndex = BroadIndex;
		while ( BroadIndex < TypeOffsetTable.Num() )
		{
			FTypeOffsetTableEntry& NextEntry = TypeOffsetTable[BroadIndex++];
			int DestIndex = --NextEntry.Offset; //decrement and prepare swap 

			// example swap chain of removing X 
			// PrimitiveSceneProxies[0,0,0,6,X,6,6,6,2,2,2,2,1,1,1,7,4,8]
			// PrimitiveSceneProxies[0,0,0,6,6,6,6,6,X,2,2,2,1,1,1,7,4,8]
			// PrimitiveSceneProxies[0,0,0,6,6,6,6,6,2,2,2,X,1,1,1,7,4,8]
			// PrimitiveSceneProxies[0,0,0,6,6,6,6,6,2,2,2,1,1,1,X,7,4,8]
			// PrimitiveSceneProxies[0,0,0,6,6,6,6,6,2,2,2,1,1,1,7,X,4,8]
			// PrimitiveSceneProxies[0,0,0,6,6,6,6,6,2,2,2,1,1,1,7,4,X,8]
			// PrimitiveSceneProxies[0,0,0,6,6,6,6,6,2,2,2,1,1,1,7,4,8,X]

			if (DestIndex != SourceIndex)
			{
				checkfSlow(DestIndex > SourceIndex, TEXT("Corrupted Prefix Sum [%d, %d]"), DestIndex, SourceIndex);
				Primitives[DestIndex]->PackedIndex = SourceIndex;
				Primitives[SourceIndex]->PackedIndex = DestIndex;

				TArraySwapElements(Primitives, DestIndex, SourceIndex);
				TArraySwapElements(PrimitiveTransforms, DestIndex, SourceIndex);
				TArraySwapElements(PrimitiveSceneProxies, DestIndex, SourceIndex);
				TArraySwapElements(PrimitiveBounds, DestIndex, SourceIndex);
				TArraySwapElements(PrimitiveFlagsCompact, DestIndex, SourceIndex);
				TArraySwapElements(PrimitiveVisibilityIds, DestIndex, SourceIndex);
				TArraySwapElements(PrimitiveOcclusionFlags, DestIndex, SourceIndex);
				TArraySwapElements(PrimitiveComponentIds, DestIndex, SourceIndex);
				TArraySwapElements(PrimitiveOcclusionBounds, DestIndex, SourceIndex);
				SourceIndex = DestIndex;

				AddPrimitiveToUpdateGPU(*this, DestIndex);
			}
		}

		const int PreviousOffset = SavedBroadIndex > 0 ? TypeOffsetTable[SavedBroadIndex - 1].Offset : 0;
		const int CurrentOffset = TypeOffsetTable[SavedBroadIndex].Offset;

		checkfSlow(PreviousOffset <= CurrentOffset, TEXT("Corrupted Bucket [%d, %d]"), PreviousOffset, CurrentOffset);
		if (CurrentOffset - PreviousOffset == 0)
		{
			// remove empty OffsetTable entries e.g.
			// TypeOffsetTable[3,8,12,15,15,17,18]
			// TypeOffsetTable[3,8,12,15,17,18]
			TypeOffsetTable.RemoveAt(SavedBroadIndex);
		}

		checkfSlow((TypeOffsetTable.Num() == 0 && Primitives.Num() == 1) || TypeOffsetTable[TypeOffsetTable.Num() - 1].Offset == Primitives.Num() - 1, TEXT("Corrupted Tail Offset [%d, %d]"), TypeOffsetTable[TypeOffsetTable.Num() - 1].Offset, Primitives.Num() - 1);
		checkfSlow(Primitives[Primitives.Num() - 1] == PrimitiveSceneInfo, TEXT("Removed item should be at the end"));

		Primitives.Pop();
		PrimitiveTransforms.Pop();
		PrimitiveSceneProxies.Pop();
		PrimitiveBounds.Pop();
		PrimitiveFlagsCompact.Pop();
		PrimitiveVisibilityIds.Pop();
		PrimitiveOcclusionFlags.Pop();
		PrimitiveComponentIds.Pop();
		PrimitiveOcclusionBounds.Pop();
	}

	PrimitiveSceneInfo->PackedIndex = MAX_int32;
	
	CheckPrimitiveArrays();

	// Remove primitive's motion blur information.
	VelocityData.RemoveTransform(PrimitiveSceneInfo->PrimitiveComponentId);

	// Unlink the primitive from its shadow parent.
	PrimitiveSceneInfo->UnlinkAttachmentGroup();

	// Unlink the LOD parent info if valid
	PrimitiveSceneInfo->UnlinkLODParentComponent();

	// Remove the primitive from the scene.
	PrimitiveSceneInfo->RemoveFromScene(true);

	// Update the primitive that was swapped to this index
	AddPrimitiveToUpdateGPU(*this, PrimitiveIndex);

	DistanceFieldSceneData.RemovePrimitive(PrimitiveSceneInfo);

	// free the primitive scene proxy.
	delete PrimitiveSceneInfo->Proxy;
}

void FScene::RemovePrimitive( UPrimitiveComponent* Primitive )
{
	SCOPE_CYCLE_COUNTER(STAT_RemoveScenePrimitiveGT);

	FPrimitiveSceneProxy* PrimitiveSceneProxy = Primitive->SceneProxy;

	if(PrimitiveSceneProxy)
	{
		FPrimitiveSceneInfo* PrimitiveSceneInfo = PrimitiveSceneProxy->GetPrimitiveSceneInfo();

		// Disassociate the primitive's scene proxy.
		Primitive->SceneProxy = NULL;

		// Send a command to the rendering thread to remove the primitive from the scene.
		FScene* Scene = this;
		FThreadSafeCounter* AttachmentCounter = &Primitive->AttachmentCounter;
		ENQUEUE_RENDER_COMMAND(FRemovePrimitiveCommand)(
			[Scene, PrimitiveSceneInfo, AttachmentCounter](FRHICommandList&)
			{
				FScopeCycleCounter Context(PrimitiveSceneInfo->Proxy->GetStatId());
				Scene->RemovePrimitiveSceneInfo_RenderThread(PrimitiveSceneInfo);
				AttachmentCounter->Decrement();
			});

		// Delete the PrimitiveSceneInfo on the game thread after the rendering thread has processed its removal.
		// This must be done on the game thread because the hit proxy references (and possibly other members) need to be freed on the game thread.
		BeginCleanup(PrimitiveSceneInfo);
	}
}

void FScene::ReleasePrimitive( UPrimitiveComponent* PrimitiveComponent )
{
	// Send a command to the rendering thread to clean up any state dependent on this primitive
	FScene* Scene = this;
	FPrimitiveComponentId PrimitiveComponentId = PrimitiveComponent->ComponentId;
	ENQUEUE_RENDER_COMMAND(FReleasePrimitiveCommand)(
		[Scene, PrimitiveComponentId](FRHICommandList&)
		{
			// Free the space in the indirect lighting cache
			Scene->IndirectLightingCache.ReleasePrimitive(PrimitiveComponentId);
		});
}

void FScene::AssignAvailableShadowMapChannelForLight(FLightSceneInfo* LightSceneInfo)
{
	FDynamicShadowMapChannelBindingHelper Helper;
	check(LightSceneInfo && LightSceneInfo->Proxy);

	// For lights with static shadowing, only check for lights intersecting the preview channel if any.
	if (LightSceneInfo->Proxy->HasStaticShadowing())
	{
		Helper.DisableAllOtherChannels(LightSceneInfo->GetDynamicShadowMapChannel());

		// If this static shadowing light does not need a (preview) channel, skip it.
		if (!Helper.HasAnyChannelEnabled())
		{
			return;
		}
	}
	else if (LightSceneInfo->Proxy->GetLightType() == LightType_Directional)
	{
		// The implementation of forward lighting in ShadowProjectionPixelShader.usf does not support binding the directional light to channel 3.
		// This is related to the USE_FADE_PLANE feature that encodes the CSM blend factor the alpha channel.
		Helper.DisableChannel(3);
	}

	Helper.UpdateAvailableChannels(Lights, LightSceneInfo);

	const int32 NewChannelIndex = Helper.GetBestAvailableChannel();
	if (NewChannelIndex != INDEX_NONE)
	{
		// Unbind the channels previously allocated to lower priority lights.
		for (FLightSceneInfo* OtherLight : Helper.GetLights(NewChannelIndex))
		{
			OtherLight->SetDynamicShadowMapChannel(INDEX_NONE);
		}

		LightSceneInfo->SetDynamicShadowMapChannel(NewChannelIndex);

		// Try to assign new channels to lights that were just unbound.
		// Sort the lights so that they only get inserted once (prevents recursion).
		Helper.SortLightByPriority(NewChannelIndex);
		for (FLightSceneInfo* OtherLight : Helper.GetLights(NewChannelIndex))
		{
			AssignAvailableShadowMapChannelForLight(OtherLight);
		}
	}
	else
	{
		LightSceneInfo->SetDynamicShadowMapChannel(INDEX_NONE);
		OverflowingDynamicShadowedLights.AddUnique(LightSceneInfo->Proxy->GetComponentName());
	}
}

void FScene::AddLightSceneInfo_RenderThread(FLightSceneInfo* LightSceneInfo)
{
	SCOPE_CYCLE_COUNTER(STAT_AddSceneLightTime);

	check(LightSceneInfo->bVisible);

	// Add the light to the light list.
	LightSceneInfo->Id = Lights.Add(FLightSceneInfoCompact(LightSceneInfo));
	const FLightSceneInfoCompact& LightSceneInfoCompact = Lights[LightSceneInfo->Id];

	if (LightSceneInfo->Proxy->GetLightType() == LightType_Directional &&
		// Only use a stationary or movable light
		!LightSceneInfo->Proxy->HasStaticLighting())
	{
		// Set SimpleDirectionalLight
		if(!SimpleDirectionalLight)
		{
			SimpleDirectionalLight = LightSceneInfo;
		}

		if(GetShadingPath() == EShadingPath::Mobile)
		{
			const bool bUseCSMForDynamicObjects = LightSceneInfo->Proxy->UseCSMForDynamicObjects();
#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
			// these are tracked for disabled shader permutation warnings
			if (LightSceneInfo->Proxy->IsMovable())
			{
				NumMobileMovableDirectionalLights_RenderThread++;
			}
			if (bUseCSMForDynamicObjects)
			{
				NumMobileStaticAndCSMLights_RenderThread++;
			}
#endif
		    // Set MobileDirectionalLights entry
		    int32 FirstLightingChannel = GetFirstLightingChannelFromMask(LightSceneInfo->Proxy->GetLightingChannelMask());
		    if (FirstLightingChannel >= 0 && MobileDirectionalLights[FirstLightingChannel] == nullptr)
		    {
			    MobileDirectionalLights[FirstLightingChannel] = LightSceneInfo;
    
			    // if this light is a dynamic shadowcast then we need to update the static draw lists to pick a new lighting policy:
			    if (!LightSceneInfo->Proxy->HasStaticShadowing() || bUseCSMForDynamicObjects)
				{
		    		bScenesPrimitivesNeedStaticMeshElementUpdate = true;
				}
		    }
		}
	}

	const bool bForwardShading = IsForwardShadingEnabled(GetShaderPlatform());

	if (bForwardShading && (LightSceneInfo->Proxy->CastsDynamicShadow() || LightSceneInfo->Proxy->GetLightFunctionMaterial()))
	{
		AssignAvailableShadowMapChannelForLight(LightSceneInfo);
	}

	if (LightSceneInfo->Proxy->IsUsedAsAtmosphereSunLight() &&
		(!SunLight || LightSceneInfo->Proxy->GetColor().ComputeLuminance() > SunLight->Proxy->GetColor().ComputeLuminance()) ) // choose brightest sun light...
	{
		SunLight = LightSceneInfo;
	}

	// Add the light to the scene.
	LightSceneInfo->AddToScene();
}

void FScene::AddLight(ULightComponent* Light)
{
	// Create the light's scene proxy.
	FLightSceneProxy* Proxy = Light->CreateSceneProxy();
	if(Proxy)
	{
		// Associate the proxy with the light.
		Light->SceneProxy = Proxy;

		// Update the light's transform and position.
		Proxy->SetTransform(Light->GetComponentTransform().ToMatrixNoScale(),Light->GetLightPosition());

		// Create the light scene info.
		Proxy->LightSceneInfo = new FLightSceneInfo(Proxy, true);

		INC_DWORD_STAT(STAT_SceneLights);

		// Adding a new light
		++NumVisibleLights_GameThread;

		// Send a command to the rendering thread to add the light to the scene.
		ENQUEUE_UNIQUE_RENDER_COMMAND_TWOPARAMETER(
			FAddLightCommand,
			FScene*,Scene,this,
			FLightSceneInfo*,LightSceneInfo,Proxy->LightSceneInfo,
			{
				FScopeCycleCounter Context(LightSceneInfo->Proxy->GetStatId());
				Scene->AddLightSceneInfo_RenderThread(LightSceneInfo);
			});
	}
}

void FScene::AddInvisibleLight(ULightComponent* Light)
{
	// Create the light's scene proxy.
	FLightSceneProxy* Proxy = Light->CreateSceneProxy();

	if(Proxy)
	{
		// Associate the proxy with the light.
		Light->SceneProxy = Proxy;

		// Update the light's transform and position.
		Proxy->SetTransform(Light->GetComponentTransform().ToMatrixNoScale(),Light->GetLightPosition());

		// Create the light scene info.
		Proxy->LightSceneInfo = new FLightSceneInfo(Proxy, false);

		INC_DWORD_STAT(STAT_SceneLights);

		// Send a command to the rendering thread to add the light to the scene.
		ENQUEUE_UNIQUE_RENDER_COMMAND_TWOPARAMETER(
			FAddLightCommand,
			FScene*,Scene,this,
			FLightSceneInfo*,LightSceneInfo,Proxy->LightSceneInfo,
		{
			FScopeCycleCounter Context(LightSceneInfo->Proxy->GetStatId());
			LightSceneInfo->Id = Scene->InvisibleLights.Add(FLightSceneInfoCompact(LightSceneInfo));
		});
	}
}

void FScene::SetSkyLight(FSkyLightSceneProxy* LightProxy)
{
	check(LightProxy);
	NumEnabledSkylights_GameThread++;

	FScene* Scene = this;

	ENQUEUE_RENDER_COMMAND(FSetSkyLightCommand)
		([Scene, LightProxy](FRHICommandListImmediate& RHICmdList)
	{
		check(!Scene->SkyLightStack.Contains(LightProxy));
		Scene->SkyLightStack.Push(LightProxy);
		const bool bOriginalHadSkylight = Scene->ShouldRenderSkylightInBasePass(BLEND_Opaque);

		// Use the most recently enabled skylight
		Scene->SkyLight = LightProxy;

		const bool bNewHasSkylight = Scene->ShouldRenderSkylightInBasePass(BLEND_Opaque);

		if (bOriginalHadSkylight != bNewHasSkylight)
		{
			// Mark the scene as needing static draw lists to be recreated if needed
			// The base pass chooses shaders based on whether there's a skylight in the scene, and that is cached in static draw lists
			Scene->bScenesPrimitivesNeedStaticMeshElementUpdate = true;
		}
	});
}

void FScene::DisableSkyLight(FSkyLightSceneProxy* LightProxy)
{
	check(LightProxy);
	NumEnabledSkylights_GameThread--;

	FScene* Scene = this;

	ENQUEUE_RENDER_COMMAND(FDisableSkyLightCommand)
		([Scene, LightProxy](FRHICommandListImmediate& RHICmdList)
	{
		const bool bOriginalHadSkylight = Scene->ShouldRenderSkylightInBasePass(BLEND_Opaque);

		Scene->SkyLightStack.RemoveSingle(LightProxy);

		if (Scene->SkyLightStack.Num() > 0)
		{
			// Use the most recently enabled skylight
			Scene->SkyLight = Scene->SkyLightStack.Last();
		}
		else
		{
			Scene->SkyLight = NULL;
		}

		const bool bNewHasSkylight = Scene->ShouldRenderSkylightInBasePass(BLEND_Opaque);

		// Update the scene if we switched skylight enabled states
		if (bOriginalHadSkylight != bNewHasSkylight)
		{
			Scene->bScenesPrimitivesNeedStaticMeshElementUpdate = true;
		}
	});
}

void FScene::AddOrRemoveDecal_RenderThread(FDeferredDecalProxy* Proxy, bool bAdd)
{
	if(bAdd)
	{
		Decals.Add(Proxy);
	}
	else
	{
		// can be optimized
		for(TSparseArray<FDeferredDecalProxy*>::TIterator It(Decals); It; ++It)
		{
			FDeferredDecalProxy* CurrentProxy = *It;

			if (CurrentProxy == Proxy)
			{
				It.RemoveCurrent();
				delete CurrentProxy;
				break;
			}
		}
	}
}

void FScene::AddDecal(UDecalComponent* Component)
{
	if(!Component->SceneProxy)
	{
		// Create the decals's scene proxy.
		Component->SceneProxy = Component->CreateSceneProxy();

		INC_DWORD_STAT(STAT_SceneDecals);

		// Send a command to the rendering thread to add the light to the scene.
		ENQUEUE_UNIQUE_RENDER_COMMAND_TWOPARAMETER(
			FAddDecalCommand,
			FScene*,Scene,this,
			FDeferredDecalProxy*,Proxy,Component->SceneProxy,
		{
			Scene->AddOrRemoveDecal_RenderThread(Proxy, true);
		});
	}
}

void FScene::RemoveDecal(UDecalComponent* Component)
{
	if(Component->SceneProxy)
	{
		DEC_DWORD_STAT(STAT_SceneDecals);

		// Send a command to the rendering thread to remove the light from the scene.
		ENQUEUE_UNIQUE_RENDER_COMMAND_TWOPARAMETER(
			FRemoveDecalCommand,
			FScene*,Scene,this,
			FDeferredDecalProxy*,Proxy,Component->SceneProxy,
		{
			Scene->AddOrRemoveDecal_RenderThread(Proxy, false);
		});

		// Disassociate the primitive's scene proxy.
		Component->SceneProxy = NULL;
	}
}

void FScene::UpdateDecalTransform(UDecalComponent* Decal)
{
	if(Decal->SceneProxy)
	{
		//Send command to the rendering thread to update the decal's transform.
		ENQUEUE_UNIQUE_RENDER_COMMAND_TWOPARAMETER(
			UpdateTransformCommand,
			FDeferredDecalProxy*,DecalSceneProxy,Decal->SceneProxy,
			FTransform,ComponentToWorldIncludingDecalSize,Decal->GetTransformIncludingDecalSize(),
		{
			// Update the primitive's transform.
			DecalSceneProxy->SetTransformIncludingDecalSize(ComponentToWorldIncludingDecalSize);
		});
	}
}

void FScene::UpdateDecalFadeOutTime(UDecalComponent* Decal)
{
	FDeferredDecalProxy* Proxy = Decal->SceneProxy;
	if(Proxy)
	{
		float CurrentTime = GetWorld()->GetTimeSeconds();
		float DecalFadeStartDelay = Decal->FadeStartDelay;
		float DecalFadeDuration = Decal->FadeDuration;

		ENQUEUE_RENDER_COMMAND(FUpdateDecalFadeInTimeCommand)(
			[Proxy, CurrentTime, DecalFadeStartDelay, DecalFadeDuration](FRHICommandListImmediate& RHICmdList)
		{
			if (DecalFadeDuration > 0.0f)
			{
				Proxy->InvFadeDuration = 1.0f / DecalFadeDuration;
				Proxy->FadeStartDelayNormalized = (CurrentTime + DecalFadeStartDelay + DecalFadeDuration) * Proxy->InvFadeDuration;
			}
			else
			{
				Proxy->InvFadeInDuration = -1.0f;
				Proxy->FadeInStartDelayNormalized = 1.0f;
			}
		});
	}
}

void FScene::UpdateDecalFadeInTime(UDecalComponent* Decal)
{
	FDeferredDecalProxy* Proxy = Decal->SceneProxy;
	if (Proxy)
	{
		float CurrentTime = GetWorld()->GetTimeSeconds();
		float DecalFadeStartDelay = Decal->FadeInStartDelay;
		float DecalFadeDuration = Decal->FadeInDuration;

		ENQUEUE_RENDER_COMMAND(FUpdateDecalFadeInTimeCommand)(
			[Proxy, CurrentTime, DecalFadeStartDelay, DecalFadeDuration](FRHICommandListImmediate& RHICmdList)
		{
			if (DecalFadeDuration > 0.0f)
			{
				Proxy->InvFadeInDuration = 1.0f / DecalFadeDuration;
				Proxy->FadeInStartDelayNormalized = (CurrentTime + DecalFadeDuration) * -Proxy->InvFadeInDuration;
			}
			else
			{
				Proxy->InvFadeInDuration = 1.0f;
				Proxy->FadeInStartDelayNormalized = 0.0f;
			}
		});
	}
}


void FScene::AddReflectionCapture(UReflectionCaptureComponent* Component)
{
	if (!Component->SceneProxy)
	{
		Component->SceneProxy = Component->CreateSceneProxy();

		FScene* Scene = this;
		FReflectionCaptureProxy* Proxy = Component->SceneProxy;

		ENQUEUE_RENDER_COMMAND(FAddCaptureCommand)
			([Scene, Proxy](FRHICommandListImmediate& RHICmdList)
		{
			if (Proxy->bUsingPreviewCaptureData)
			{
				FPlatformAtomics::InterlockedIncrement(&Scene->NumUnbuiltReflectionCaptures);
			}

			Scene->ReflectionSceneData.bRegisteredReflectionCapturesHasChanged = true;
			const int32 PackedIndex = Scene->ReflectionSceneData.RegisteredReflectionCaptures.Add(Proxy);

			Proxy->PackedIndex = PackedIndex;
			Scene->ReflectionSceneData.RegisteredReflectionCapturePositions.Add(Proxy->Position);

			checkSlow(Scene->ReflectionSceneData.RegisteredReflectionCaptures.Num() == Scene->ReflectionSceneData.RegisteredReflectionCapturePositions.Num());
		});
	}
}

void FScene::RemoveReflectionCapture(UReflectionCaptureComponent* Component)
{
	if (Component->SceneProxy)
	{
		FScene* Scene = this;
		FReflectionCaptureProxy* Proxy = Component->SceneProxy;

		ENQUEUE_RENDER_COMMAND(FRemoveCaptureCommand)
			([Scene, Proxy](FRHICommandListImmediate& RHICmdList)
		{
			if (Proxy->bUsingPreviewCaptureData)
			{
				FPlatformAtomics::InterlockedDecrement(&Scene->NumUnbuiltReflectionCaptures);
			}

			Scene->ReflectionSceneData.bRegisteredReflectionCapturesHasChanged = true;

			int32 CaptureIndex = Proxy->PackedIndex;
			Scene->ReflectionSceneData.RegisteredReflectionCaptures.RemoveAtSwap(CaptureIndex);
			Scene->ReflectionSceneData.RegisteredReflectionCapturePositions.RemoveAtSwap(CaptureIndex);

			if (Scene->ReflectionSceneData.RegisteredReflectionCaptures.IsValidIndex(CaptureIndex))
			{
				FReflectionCaptureProxy* OtherCapture = Scene->ReflectionSceneData.RegisteredReflectionCaptures[CaptureIndex];
				OtherCapture->PackedIndex = CaptureIndex;
			}

			delete Proxy;

			checkSlow(Scene->ReflectionSceneData.RegisteredReflectionCaptures.Num() == Scene->ReflectionSceneData.RegisteredReflectionCapturePositions.Num());
		});

		// Disassociate the primitive's scene proxy.
		Component->SceneProxy = NULL;
	}
}

void FScene::UpdateReflectionCaptureTransform(UReflectionCaptureComponent* Component)
{
	if (Component->SceneProxy)
	{
		const FReflectionCaptureMapBuildData* MapBuildData = Component->GetMapBuildData();
		bool bUsingPreviewCaptureData = MapBuildData == NULL;

		FScene* Scene = this;
		FReflectionCaptureProxy* Proxy = Component->SceneProxy;
		FMatrix Transform = Component->GetComponentTransform().ToMatrixWithScale();

		ENQUEUE_RENDER_COMMAND(FUpdateTransformCommand)
			([Scene, Proxy, Transform, bUsingPreviewCaptureData](FRHICommandListImmediate& RHICmdList)
		{
			if (Proxy->bUsingPreviewCaptureData)
			{
				FPlatformAtomics::InterlockedDecrement(&Scene->NumUnbuiltReflectionCaptures);
			}

			Proxy->bUsingPreviewCaptureData = bUsingPreviewCaptureData;

			if (Proxy->bUsingPreviewCaptureData)
			{
				FPlatformAtomics::InterlockedIncrement(&Scene->NumUnbuiltReflectionCaptures);
			}

			Scene->ReflectionSceneData.bRegisteredReflectionCapturesHasChanged = true;
			Proxy->SetTransform(Transform);
		});
	}
}

void FScene::ReleaseReflectionCubemap(UReflectionCaptureComponent* CaptureComponent)
{
	bool bRemoved = false;
	for (TSparseArray<UReflectionCaptureComponent*>::TIterator It(ReflectionSceneData.AllocatedReflectionCapturesGameThread); It; ++It)
	{
		UReflectionCaptureComponent* CurrentCapture = *It;

		if (CurrentCapture == CaptureComponent)
		{
			It.RemoveCurrent();
			bRemoved = true;
			break;
		}
	}

	if (bRemoved)
	{
	ENQUEUE_UNIQUE_RENDER_COMMAND_TWOPARAMETER(
		RemoveCaptureCommand,
			UReflectionCaptureComponent*, Component, CaptureComponent,
			FScene*, Scene, this,
		{
			const FCaptureComponentSceneState* ComponentStatePtr = Scene->ReflectionSceneData.AllocatedReflectionCaptureState.Find(Component);
			if (ComponentStatePtr)
			{
				// We track removed captures so we can remap them when reallocating the cubemap array
				check(ComponentStatePtr->CubemapIndex != -1);
				Scene->ReflectionSceneData.CubemapArraySlotsUsed[ComponentStatePtr->CubemapIndex] = false;
		}
		Scene->ReflectionSceneData.AllocatedReflectionCaptureState.Remove(Component);
	});
	}
}

const FReflectionCaptureProxy* FScene::FindClosestReflectionCapture(FVector Position) const
{
	checkSlow(IsInParallelRenderingThread());
	int32 ClosestCaptureIndex = INDEX_NONE;
	float ClosestDistanceSquared = FLT_MAX;

	// Linear search through the scene's reflection captures
	// ReflectionSceneData.RegisteredReflectionCapturePositions has been packed densely to make this coherent in memory
	for (int32 CaptureIndex = 0; CaptureIndex < ReflectionSceneData.RegisteredReflectionCapturePositions.Num(); CaptureIndex++)
	{
		const float DistanceSquared = (ReflectionSceneData.RegisteredReflectionCapturePositions[CaptureIndex] - Position).SizeSquared();

		if (DistanceSquared < ClosestDistanceSquared)
		{
			ClosestDistanceSquared = DistanceSquared;
			ClosestCaptureIndex = CaptureIndex;
		}
	}

	return ClosestCaptureIndex != INDEX_NONE ? ReflectionSceneData.RegisteredReflectionCaptures[ClosestCaptureIndex] : NULL;
}

const FPlanarReflectionSceneProxy* FScene::FindClosestPlanarReflection(const FBoxSphereBounds& Bounds) const
{
	checkSlow(IsInParallelRenderingThread());
	const FPlanarReflectionSceneProxy* ClosestPlanarReflection = NULL;
	float ClosestDistance = FLT_MAX;
	FBox PrimitiveBoundingBox(Bounds.Origin - Bounds.BoxExtent, Bounds.Origin + Bounds.BoxExtent);

	// Linear search through the scene's planar reflections
	for (int32 CaptureIndex = 0; CaptureIndex < PlanarReflections.Num(); CaptureIndex++)
	{
		FPlanarReflectionSceneProxy* CurrentPlanarReflection = PlanarReflections[CaptureIndex];
		const FBox ReflectionBounds = CurrentPlanarReflection->WorldBounds;

		if (PrimitiveBoundingBox.Intersect(ReflectionBounds))
		{
			const float Distance = FMath::Abs(CurrentPlanarReflection->ReflectionPlane.PlaneDot(Bounds.Origin));

			if (Distance < ClosestDistance)
			{
				ClosestDistance = Distance;
				ClosestPlanarReflection = CurrentPlanarReflection;
			}
		}
	}

	return ClosestPlanarReflection;
}

const FPlanarReflectionSceneProxy* FScene::GetForwardPassGlobalPlanarReflection() const
{
	// For the forward pass just pick first planar reflection.

	if (PlanarReflections.Num() > 0)
	{
		return PlanarReflections[0];
	}

	return nullptr;
}

void FScene::FindClosestReflectionCaptures(FVector Position, const FReflectionCaptureProxy* (&SortedByDistanceOUT)[FPrimitiveSceneInfo::MaxCachedReflectionCaptureProxies]) const
{
	checkSlow(IsInParallelRenderingThread());
	static const int32 ArraySize = FPrimitiveSceneInfo::MaxCachedReflectionCaptureProxies;

	struct FReflectionCaptureDistIndex
	{
		int32 CaptureIndex;
		float CaptureDistance;
		const FReflectionCaptureProxy* CaptureProxy;
	};

	// Find the nearest n captures to this primitive. 
	const int32 NumRegisteredReflectionCaptures = ReflectionSceneData.RegisteredReflectionCapturePositions.Num();
	const int32 PopulateCaptureCount = FMath::Min(ArraySize, NumRegisteredReflectionCaptures);

	TArray<FReflectionCaptureDistIndex, TFixedAllocator<ArraySize>> ClosestCaptureIndices;
	ClosestCaptureIndices.AddUninitialized(PopulateCaptureCount);

	for (int32 CaptureIndex = 0; CaptureIndex < PopulateCaptureCount; CaptureIndex++)
	{
		ClosestCaptureIndices[CaptureIndex].CaptureIndex = CaptureIndex;
		ClosestCaptureIndices[CaptureIndex].CaptureDistance = (ReflectionSceneData.RegisteredReflectionCapturePositions[CaptureIndex] - Position).SizeSquared();
	}
	
	for (int32 CaptureIndex = PopulateCaptureCount; CaptureIndex < NumRegisteredReflectionCaptures; CaptureIndex++)
	{
		const float DistanceSquared = (ReflectionSceneData.RegisteredReflectionCapturePositions[CaptureIndex] - Position).SizeSquared();
		for (int32 i = 0; i < ArraySize; i++)
		{
			if (DistanceSquared<ClosestCaptureIndices[i].CaptureDistance)
			{
				ClosestCaptureIndices[i].CaptureDistance = DistanceSquared;
				ClosestCaptureIndices[i].CaptureIndex = CaptureIndex;
				break;
			}
		}
	}

	for (int32 CaptureIndex = 0; CaptureIndex < PopulateCaptureCount; CaptureIndex++)
	{
		FReflectionCaptureProxy* CaptureProxy = ReflectionSceneData.RegisteredReflectionCaptures[ClosestCaptureIndices[CaptureIndex].CaptureIndex];		
		ClosestCaptureIndices[CaptureIndex].CaptureProxy = CaptureProxy;
	}
	// Sort by influence radius.
	ClosestCaptureIndices.Sort([](const FReflectionCaptureDistIndex& A, const FReflectionCaptureDistIndex& B)
		{
			if (A.CaptureProxy->InfluenceRadius != B.CaptureProxy->InfluenceRadius)
			{
				return (A.CaptureProxy->InfluenceRadius < B.CaptureProxy->InfluenceRadius);
			}
			return A.CaptureProxy->Guid < B.CaptureProxy->Guid;
		});

	FMemory::Memzero(SortedByDistanceOUT);

	for (int32 CaptureIndex = 0; CaptureIndex < PopulateCaptureCount; CaptureIndex++)
	{
		SortedByDistanceOUT[CaptureIndex] = ClosestCaptureIndices[CaptureIndex].CaptureProxy;
	}
}

int64 FScene::GetCachedWholeSceneShadowMapsSize() const
{
	int64 CachedShadowmapMemory = 0;

	for (TMap<int32, FCachedShadowMapData>::TConstIterator CachedShadowMapIt(CachedShadowMaps); CachedShadowMapIt; ++CachedShadowMapIt)
	{
		const FCachedShadowMapData& ShadowMapData = CachedShadowMapIt.Value();

		if (ShadowMapData.ShadowMap.IsValid())
		{
			CachedShadowmapMemory += ShadowMapData.ShadowMap.ComputeMemorySize();
		}
	}

	return CachedShadowmapMemory;
}

void FScene::AddPrecomputedLightVolume(const FPrecomputedLightVolume* Volume)
{
	FScene* Scene = this;

	ENQUEUE_RENDER_COMMAND(AddVolumeCommand)
		([Scene, Volume](FRHICommandListImmediate& RHICmdList) 
		{
			Scene->PrecomputedLightVolumes.Add(Volume);
			Scene->IndirectLightingCache.SetLightingCacheDirty(Scene, Volume);
		});
}

void FScene::RemovePrecomputedLightVolume(const FPrecomputedLightVolume* Volume)
{
	FScene* Scene = this; 

	ENQUEUE_RENDER_COMMAND(RemoveVolumeCommand)
		([Scene, Volume](FRHICommandListImmediate& RHICmdList) 
		{
			Scene->PrecomputedLightVolumes.Remove(Volume);
			Scene->IndirectLightingCache.SetLightingCacheDirty(Scene, Volume);
		});
}

void FVolumetricLightmapSceneData::AddLevelVolume(const FPrecomputedVolumetricLightmap* InVolume, EShadingPath ShadingPath)
{
	LevelVolumetricLightmaps.Add(InVolume);
}

void FVolumetricLightmapSceneData::RemoveLevelVolume(const FPrecomputedVolumetricLightmap* InVolume)
{
	LevelVolumetricLightmaps.Remove(InVolume);
}

bool FScene::HasPrecomputedVolumetricLightmap_RenderThread() const
{
	return VolumetricLightmapSceneData.HasData();
}

void FScene::AddPrecomputedVolumetricLightmap(const FPrecomputedVolumetricLightmap* Volume)
{
	FScene* Scene = this;

	ENQUEUE_RENDER_COMMAND(AddVolumeCommand)
		([Scene, Volume](FRHICommandListImmediate& RHICmdList) 
		{
#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
			if (Volume && Scene->GetShadingPath() == EShadingPath::Mobile)
			{
				const FPrecomputedVolumetricLightmapData* VolumeData = Volume->Data;
				if (VolumeData && VolumeData->BrickData.LQLightDirection.Data.Num() == 0)
				{
					FPlatformAtomics::InterlockedIncrement(&Scene->NumUncachedStaticLightingInteractions);
				}
			}
#endif
		Scene->VolumetricLightmapSceneData.AddLevelVolume(Volume, Scene->GetShadingPath());
		});
}

void FScene::RemovePrecomputedVolumetricLightmap(const FPrecomputedVolumetricLightmap* Volume)
{
	FScene* Scene = this; 

	ENQUEUE_RENDER_COMMAND(RemoveVolumeCommand)
		([Scene, Volume](FRHICommandListImmediate& RHICmdList) 
		{
#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
			if (Volume && Scene->GetShadingPath() == EShadingPath::Mobile)
			{
				const FPrecomputedVolumetricLightmapData* VolumeData = Volume->Data;
				if (VolumeData && VolumeData->BrickData.LQLightDirection.Data.Num() == 0)
				{
					FPlatformAtomics::InterlockedDecrement(&Scene->NumUncachedStaticLightingInteractions);
				}
			}
#endif
		Scene->VolumetricLightmapSceneData.RemoveLevelVolume(Volume);
		});
}

bool FScene::GetPreviousLocalToWorld(const FPrimitiveSceneInfo* PrimitiveSceneInfo, FMatrix& OutPreviousLocalToWorld) const
{
	return VelocityData.GetComponentPreviousLocalToWorld(PrimitiveSceneInfo->PrimitiveComponentId, OutPreviousLocalToWorld);
}

void FScene::GetPrimitiveUniformShaderParameters_RenderThread(const FPrimitiveSceneInfo* PrimitiveSceneInfo, bool& bHasPrecomputedVolumetricLightmap, FMatrix& PreviousLocalToWorld, int32& SingleCaptureIndex) const 
{
	bHasPrecomputedVolumetricLightmap = VolumetricLightmapSceneData.HasData();
	
	const bool bVelocityDataFound = VelocityData.GetComponentPreviousLocalToWorld(PrimitiveSceneInfo->PrimitiveComponentId, PreviousLocalToWorld);

	if (!bVelocityDataFound)
	{
		PreviousLocalToWorld = PrimitiveSceneInfo->Proxy->GetLocalToWorld();
	}

	SingleCaptureIndex = PrimitiveSceneInfo->CachedReflectionCaptureProxy ? PrimitiveSceneInfo->CachedReflectionCaptureProxy->SortedCaptureIndex : -1;
}

struct FUpdateLightTransformParameters
{
	FMatrix LightToWorld;
	FVector4 Position;
};

void FScene::UpdateLightTransform_RenderThread(FLightSceneInfo* LightSceneInfo, const FUpdateLightTransformParameters& Parameters)
{
	SCOPE_CYCLE_COUNTER(STAT_UpdateSceneLightTime);
	if( LightSceneInfo && LightSceneInfo->bVisible )
	{
		// Don't remove directional lights when their transform changes as nothing in RemoveFromScene() depends on their transform
		if (!(LightSceneInfo->Proxy->GetLightType() == LightType_Directional))
		{
			// Remove the light from the scene.
			LightSceneInfo->RemoveFromScene();
		}

		// Update the light's transform and position.
		LightSceneInfo->Proxy->SetTransform(Parameters.LightToWorld,Parameters.Position);

		// Also update the LightSceneInfoCompact
		if( LightSceneInfo->Id != INDEX_NONE )
		{
			LightSceneInfo->Scene->Lights[LightSceneInfo->Id].Init(LightSceneInfo);

			// Don't re-add directional lights when their transform changes as nothing in AddToScene() depends on their transform
			if (!(LightSceneInfo->Proxy->GetLightType() == LightType_Directional))
			{
				// Add the light to the scene at its new location.
				LightSceneInfo->AddToScene();
			}
		}
	}
}

void FScene::UpdateLightTransform(ULightComponent* Light)
{
	if(Light->SceneProxy)
	{
		FUpdateLightTransformParameters Parameters;
		Parameters.LightToWorld = Light->GetComponentTransform().ToMatrixNoScale();
		Parameters.Position = Light->GetLightPosition();
		FScene* Scene = this;
		FLightSceneInfo* LightSceneInfo = Light->SceneProxy->GetLightSceneInfo();
		ENQUEUE_RENDER_COMMAND(UpdateLightTransform)(
			[Scene, LightSceneInfo, Parameters](FRHICommandListImmediate& RHICmdList)
			{
				FScopeCycleCounter Context(LightSceneInfo->Proxy->GetStatId());
				Scene->UpdateLightTransform_RenderThread(LightSceneInfo, Parameters);
			});
	}
}

/** 
 * Updates the color and brightness of a light which has already been added to the scene. 
 *
 * @param Light - light component to update
 */
void FScene::UpdateLightColorAndBrightness(ULightComponent* Light)
{
	if(Light->SceneProxy)
	{
		struct FUpdateLightColorParameters
		{
			FLinearColor NewColor;
			float NewIndirectLightingScale;
			float NewVolumetricScatteringIntensity;
		};

		FUpdateLightColorParameters NewParameters;
		NewParameters.NewColor = FLinearColor(Light->LightColor) * Light->ComputeLightBrightness();
		NewParameters.NewIndirectLightingScale = Light->IndirectLightingIntensity;
		NewParameters.NewVolumetricScatteringIntensity = Light->VolumetricScatteringIntensity;

		if( Light->bUseTemperature )
		{
			 NewParameters.NewColor *= FLinearColor::MakeFromColorTemperature(Light->Temperature);
		}
	
		FScene* Scene = this;
		FLightSceneInfo* LightSceneInfo = Light->SceneProxy->GetLightSceneInfo();
		ENQUEUE_RENDER_COMMAND(UpdateLightColorAndBrightness)(
			[LightSceneInfo, Scene, NewParameters](FRHICommandListImmediate& RHICmdList)
			{
				if( LightSceneInfo && LightSceneInfo->bVisible )
				{
					// Mobile renderer:
					// a light with no color/intensity can cause the light to be ignored when rendering.
					// thus, lights that change state in this way must update the draw lists.
					Scene->bScenesPrimitivesNeedStaticMeshElementUpdate =
						Scene->bScenesPrimitivesNeedStaticMeshElementUpdate ||
						( Scene->GetShadingPath() == EShadingPath::Mobile 
						&& NewParameters.NewColor.IsAlmostBlack() != LightSceneInfo->Proxy->GetColor().IsAlmostBlack() );

					LightSceneInfo->Proxy->SetColor(NewParameters.NewColor);
					LightSceneInfo->Proxy->IndirectLightingScale = NewParameters.NewIndirectLightingScale;
					LightSceneInfo->Proxy->VolumetricScatteringIntensity = NewParameters.NewVolumetricScatteringIntensity;

					// Also update the LightSceneInfoCompact
					if( LightSceneInfo->Id != INDEX_NONE )
					{
						Scene->Lights[ LightSceneInfo->Id ].Color = NewParameters.NewColor;
					}
				}
			});
	}
}

void FScene::RemoveLightSceneInfo_RenderThread(FLightSceneInfo* LightSceneInfo)
{
	SCOPE_CYCLE_COUNTER(STAT_RemoveSceneLightTime);

	if (LightSceneInfo->bVisible)
	{
		// check SimpleDirectionalLight
		if (LightSceneInfo == SimpleDirectionalLight)
		{
			SimpleDirectionalLight = nullptr;
		}

		if(GetShadingPath() == EShadingPath::Mobile)
		{
			const bool bUseCSMForDynamicObjects = LightSceneInfo->Proxy->UseCSMForDynamicObjects();

#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
			// Tracked for disabled shader permutation warnings.
			// Condition must match that in AddLightSceneInfo_RenderThread
			if (LightSceneInfo->Proxy->GetLightType() == LightType_Directional && !LightSceneInfo->Proxy->HasStaticLighting())
			{
				if (LightSceneInfo->Proxy->IsMovable())
				{
					NumMobileMovableDirectionalLights_RenderThread--;
				}
				if (bUseCSMForDynamicObjects)
				{
					NumMobileStaticAndCSMLights_RenderThread--;
				}
			}
#endif

		    // check MobileDirectionalLights
		    for (int32 LightChannelIdx = 0; LightChannelIdx < ARRAY_COUNT(MobileDirectionalLights); LightChannelIdx++)
		    {
			    if (LightSceneInfo == MobileDirectionalLights[LightChannelIdx])
			    {
				    MobileDirectionalLights[LightChannelIdx] = nullptr;

					// find another light that could be the new MobileDirectionalLight for this channel
					for (const FLightSceneInfoCompact& OtherLight : Lights)
					{
						if (OtherLight.LightSceneInfo != LightSceneInfo &&
							OtherLight.LightType == LightType_Directional &&
							!OtherLight.bStaticLighting &&
							GetFirstLightingChannelFromMask(OtherLight.LightSceneInfo->Proxy->GetLightingChannelMask()) == LightChannelIdx)
						{
							MobileDirectionalLights[LightChannelIdx] = OtherLight.LightSceneInfo;
							break;
						}
					}

				    // if this light is a dynamic shadowcast then we need to update the static draw lists to pick a new lightingpolicy
					if (!LightSceneInfo->Proxy->HasStaticShadowing() || bUseCSMForDynamicObjects)
					{
						bScenesPrimitivesNeedStaticMeshElementUpdate = true;
					}
				    break;
			    }
		    }
		}

		if (LightSceneInfo == SunLight)
		{
			SunLight = NULL;
			// Search for new sun light...
			for (TSparseArray<FLightSceneInfoCompact>::TConstIterator It(Lights); It; ++It)
			{
				const FLightSceneInfoCompact& LightInfo = *It;
				if (LightInfo.LightSceneInfo != LightSceneInfo
					&& LightInfo.LightSceneInfo->Proxy->bUsedAsAtmosphereSunLight
					&& (!SunLight || SunLight->Proxy->GetColor().ComputeLuminance() < LightInfo.LightSceneInfo->Proxy->GetColor().ComputeLuminance()) )
				{
					SunLight = LightInfo.LightSceneInfo;
				}
			}
		}

		// Remove the light from the scene.
		LightSceneInfo->RemoveFromScene();

		// Remove the light from the lights list.
		Lights.RemoveAt(LightSceneInfo->Id);

		if (!LightSceneInfo->Proxy->HasStaticShadowing()
			&& LightSceneInfo->Proxy->CastsDynamicShadow()
			&& LightSceneInfo->GetDynamicShadowMapChannel() == -1)
		{
			OverflowingDynamicShadowedLights.Remove(LightSceneInfo->Proxy->GetComponentName());
		}
	}
	else
	{
		InvisibleLights.RemoveAt(LightSceneInfo->Id);
	}

	// Free the light scene info and proxy.
	delete LightSceneInfo->Proxy;
	delete LightSceneInfo;
}

void FScene::RemoveLight(ULightComponent* Light)
{
	if(Light->SceneProxy)
	{
		FLightSceneInfo* LightSceneInfo = Light->SceneProxy->GetLightSceneInfo();

		DEC_DWORD_STAT(STAT_SceneLights);

		// Removing one visible light
		--NumVisibleLights_GameThread;

		// Disassociate the primitive's render info.
		Light->SceneProxy = NULL;

		// Send a command to the rendering thread to remove the light from the scene.
		ENQUEUE_UNIQUE_RENDER_COMMAND_TWOPARAMETER(
			FRemoveLightCommand,
			FScene*,Scene,this,
			FLightSceneInfo*,LightSceneInfo,LightSceneInfo,
			{
				FScopeCycleCounter Context(LightSceneInfo->Proxy->GetStatId());
				Scene->RemoveLightSceneInfo_RenderThread(LightSceneInfo);
			});
	}
}

void FScene::AddExponentialHeightFog(UExponentialHeightFogComponent* FogComponent)
{
	ENQUEUE_UNIQUE_RENDER_COMMAND_TWOPARAMETER(
		FAddFogCommand,
		FScene*,Scene,this,
		FExponentialHeightFogSceneInfo,HeightFogSceneInfo,FExponentialHeightFogSceneInfo(FogComponent),
		{
			// Create a FExponentialHeightFogSceneInfo for the component in the scene's fog array.
			new(Scene->ExponentialFogs) FExponentialHeightFogSceneInfo(HeightFogSceneInfo);
		});
}

void FScene::RemoveExponentialHeightFog(UExponentialHeightFogComponent* FogComponent)
{
	ENQUEUE_UNIQUE_RENDER_COMMAND_TWOPARAMETER(
		FRemoveFogCommand,
		FScene*,Scene,this,
		UExponentialHeightFogComponent*,FogComponent,FogComponent,
		{
			// Remove the given component's FExponentialHeightFogSceneInfo from the scene's fog array.
			for(int32 FogIndex = 0;FogIndex < Scene->ExponentialFogs.Num();FogIndex++)
			{
				if(Scene->ExponentialFogs[FogIndex].Component == FogComponent)
				{
					Scene->ExponentialFogs.RemoveAt(FogIndex);
					break;
				}
			}
		});
}

void FScene::AddWindSource(UWindDirectionalSourceComponent* WindComponent)
{
	// if this wind component is not activated (or Auto Active is set to false), then don't add to WindSources
	if(!WindComponent->IsActive())
	{
		return;
	}

	WindComponents_GameThread.Add(WindComponent);

	FWindSourceSceneProxy* SceneProxy = WindComponent->CreateSceneProxy();
	WindComponent->SceneProxy = SceneProxy;

	ENQUEUE_UNIQUE_RENDER_COMMAND_TWOPARAMETER(
		FAddWindSourceCommand,
		FScene*,Scene,this,
		FWindSourceSceneProxy*,SceneProxy,SceneProxy,
		{
			Scene->WindSources.Add(SceneProxy);
		});
}

void FScene::RemoveWindSource(UWindDirectionalSourceComponent* WindComponent)
{
	WindComponents_GameThread.Remove(WindComponent);

	FWindSourceSceneProxy* SceneProxy = WindComponent->SceneProxy;
	WindComponent->SceneProxy = NULL;

	if(SceneProxy)
	{
		ENQUEUE_UNIQUE_RENDER_COMMAND_TWOPARAMETER(
			FRemoveWindSourceCommand,
			FScene*,Scene,this,
			FWindSourceSceneProxy*,SceneProxy,SceneProxy,
			{
				Scene->WindSources.Remove(SceneProxy);

				delete SceneProxy;
			});
	}
}

const TArray<FWindSourceSceneProxy*>& FScene::GetWindSources_RenderThread() const
{
	checkSlow(IsInRenderingThread());
	return WindSources;
}

void FScene::GetWindParameters(const FVector& Position, FVector& OutDirection, float& OutSpeed, float& OutMinGustAmt, float& OutMaxGustAmt) const
{
	FWindData AccumWindData;
	AccumWindData.PrepareForAccumulate();

	int32 NumActiveWindSources = 0;
	FVector4 AccumulatedDirectionAndSpeed(0,0,0,0);
	float TotalWeight = 0.0f;
	for (int32 i = 0; i < WindSources.Num(); i++)
	{
		
		FVector4 CurrentDirectionAndSpeed;
		float Weight;
		const FWindSourceSceneProxy* CurrentSource = WindSources[i];
		FWindData CurrentSourceData;
		if (CurrentSource->GetWindParameters(Position, CurrentSourceData, Weight))
		{
			AccumWindData.AddWeighted(CurrentSourceData, Weight);
			TotalWeight += Weight;
			NumActiveWindSources++;
		}
	}

	AccumWindData.NormalizeByTotalWeight(TotalWeight);

	if (NumActiveWindSources == 0)
	{
		AccumWindData.Direction = FVector(1.0f, 0.0f, 0.0f);
	}
	OutDirection	= AccumWindData.Direction;
	OutSpeed		= AccumWindData.Speed;
	OutMinGustAmt	= AccumWindData.MinGustAmt;
	OutMaxGustAmt	= AccumWindData.MaxGustAmt;
}

void FScene::GetWindParameters_GameThread(const FVector& Position, FVector& OutDirection, float& OutSpeed, float& OutMinGustAmt, float& OutMaxGustAmt) const
{
	FWindData AccumWindData;
	AccumWindData.PrepareForAccumulate();

	const int32 NumSources = WindComponents_GameThread.Num();
	int32 NumActiveSources = 0;
	float TotalWeight = 0.0f;

	// read the wind component array, this is safe for the game thread
	for(UWindDirectionalSourceComponent* Component : WindComponents_GameThread)
	{
		float Weight = 0.0f;
		FWindData CurrentComponentData;
		if(Component->GetWindParameters(Position, CurrentComponentData, Weight))
		{
			AccumWindData.AddWeighted(CurrentComponentData, Weight);
			TotalWeight += Weight;
			++NumActiveSources;
		}
	}

	AccumWindData.NormalizeByTotalWeight(TotalWeight);

	if(NumActiveSources == 0)
	{
		AccumWindData.Direction = FVector(1.0f, 0.0f, 0.0f);
	}

	OutDirection = AccumWindData.Direction;
	OutSpeed = AccumWindData.Speed;
	OutMinGustAmt = AccumWindData.MinGustAmt;
	OutMaxGustAmt = AccumWindData.MaxGustAmt;
}

void FScene::GetDirectionalWindParameters(FVector& OutDirection, float& OutSpeed, float& OutMinGustAmt, float& OutMaxGustAmt) const
{
	FWindData AccumWindData;
	AccumWindData.PrepareForAccumulate();

	int32 NumActiveWindSources = 0;
	FVector4 AccumulatedDirectionAndSpeed(0,0,0,0);
	float TotalWeight = 0.0f;
	for (int32 i = 0; i < WindSources.Num(); i++)
	{
		FVector4 CurrentDirectionAndSpeed;
		float Weight;
		const FWindSourceSceneProxy* CurrentSource = WindSources[i];
		FWindData CurrentSourceData;
		if (CurrentSource->GetDirectionalWindParameters(CurrentSourceData, Weight))
		{
			AccumWindData.AddWeighted(CurrentSourceData, Weight);			
			TotalWeight += Weight;
			NumActiveWindSources++;
		}
	}

	AccumWindData.NormalizeByTotalWeight(TotalWeight);	

	if (NumActiveWindSources == 0)
	{
		AccumWindData.Direction = FVector(1.0f, 0.0f, 0.0f);
	}
	OutDirection = AccumWindData.Direction;
	OutSpeed = AccumWindData.Speed;
	OutMinGustAmt = AccumWindData.MinGustAmt;
	OutMaxGustAmt = AccumWindData.MaxGustAmt;
}

void FScene::AddSpeedTreeWind(FVertexFactory* VertexFactory, const UStaticMesh* StaticMesh)
{
	if (StaticMesh != NULL && StaticMesh->SpeedTreeWind.IsValid() && StaticMesh->RenderData.IsValid())
	{
		FScene* Scene = this;
		ENQUEUE_RENDER_COMMAND(FAddSpeedTreeWindCommand)(
			[Scene, StaticMesh, VertexFactory](FRHICommandListImmediate& RHICmdList)
			{
				Scene->SpeedTreeVertexFactoryMap.Add(VertexFactory, StaticMesh);

				if (Scene->SpeedTreeWindComputationMap.Contains(StaticMesh))
				{
					(*(Scene->SpeedTreeWindComputationMap.Find(StaticMesh)))->ReferenceCount++;
				}
				else
				{
					FSpeedTreeWindComputation* WindComputation = new FSpeedTreeWindComputation;
					WindComputation->Wind = *(StaticMesh->SpeedTreeWind.Get( ));

					FSpeedTreeUniformParameters UniformParameters;
					FPlatformMemory::Memzero(&UniformParameters, sizeof(UniformParameters));
					WindComputation->UniformBuffer = TUniformBufferRef<FSpeedTreeUniformParameters>::CreateUniformBufferImmediate(UniformParameters, UniformBuffer_MultiFrame);
					Scene->SpeedTreeWindComputationMap.Add(StaticMesh, WindComputation);
				}
			});
	}
}

void FScene::RemoveSpeedTreeWind_RenderThread(class FVertexFactory* VertexFactory, const class UStaticMesh* StaticMesh)
{
	FSpeedTreeWindComputation** WindComputationRef = SpeedTreeWindComputationMap.Find(StaticMesh);
	if (WindComputationRef != NULL)
	{
		FSpeedTreeWindComputation* WindComputation = *WindComputationRef;

		WindComputation->ReferenceCount--;
		if (WindComputation->ReferenceCount < 1)
		{
			for (auto Iter = SpeedTreeVertexFactoryMap.CreateIterator(); Iter; ++Iter )
			{
				if (Iter.Value() == StaticMesh)
				{
					Iter.RemoveCurrent();
				}
			}

			SpeedTreeWindComputationMap.Remove(StaticMesh);
			delete WindComputation;
		}
	}
}

void FScene::UpdateSpeedTreeWind(double CurrentTime)
{
#define SET_SPEEDTREE_TABLE_FLOAT4V(name, offset) \
	UniformParameters.name = *(FVector4*)(WindShaderValues + FSpeedTreeWind::offset); \
	UniformParameters.Prev##name = *(FVector4*)(WindShaderValues + FSpeedTreeWind::offset + FSpeedTreeWind::NUM_SHADER_VALUES);

	FScene* Scene = this;
	ENQUEUE_RENDER_COMMAND(FUpdateSpeedTreeWindCommand)(
		[Scene, CurrentTime](FRHICommandListImmediate& RHICmdList)
		{
			FVector WindDirection;
			float WindSpeed;
			float WindMinGustAmt;
			float WindMaxGustAmt;
			Scene->GetDirectionalWindParameters(WindDirection, WindSpeed, WindMinGustAmt, WindMaxGustAmt);

			for (TMap<const UStaticMesh*, FSpeedTreeWindComputation*>::TIterator It(Scene->SpeedTreeWindComputationMap); It; ++It )
			{
				const UStaticMesh* StaticMesh = It.Key();
				FSpeedTreeWindComputation* WindComputation = It.Value();

				if( !(StaticMesh->RenderData.IsValid() && StaticMesh->SpeedTreeWind.IsValid()) )
				{
					It.RemoveCurrent();
					continue;
				}

				if (GIsEditor && StaticMesh->SpeedTreeWind->NeedsReload( ))
				{
					// reload the wind since it may have changed or been scaled differently during reimport
					StaticMesh->SpeedTreeWind->SetNeedsReload(false);
					WindComputation->Wind = *(StaticMesh->SpeedTreeWind.Get( ));
				}

				// advance the wind object
				WindComputation->Wind.SetDirection(WindDirection);
				WindComputation->Wind.SetStrength(WindSpeed);
				WindComputation->Wind.SetGustMin(WindMinGustAmt);
				WindComputation->Wind.SetGustMax(WindMaxGustAmt);
				WindComputation->Wind.Advance(true, CurrentTime);

				// copy data into uniform buffer
				const float* WindShaderValues = WindComputation->Wind.GetShaderTable();

				FSpeedTreeUniformParameters UniformParameters;
				UniformParameters.WindAnimation.Set(CurrentTime, 0.0f, 0.0f, 0.0f);
			
				SET_SPEEDTREE_TABLE_FLOAT4V(WindVector, SH_WIND_DIR_X);
				SET_SPEEDTREE_TABLE_FLOAT4V(WindGlobal, SH_GLOBAL_TIME);
				SET_SPEEDTREE_TABLE_FLOAT4V(WindBranch, SH_BRANCH_1_TIME);
				SET_SPEEDTREE_TABLE_FLOAT4V(WindBranchTwitch, SH_BRANCH_1_TWITCH);
				SET_SPEEDTREE_TABLE_FLOAT4V(WindBranchWhip, SH_BRANCH_1_WHIP);
				SET_SPEEDTREE_TABLE_FLOAT4V(WindBranchAnchor, SH_WIND_ANCHOR_X);
				SET_SPEEDTREE_TABLE_FLOAT4V(WindBranchAdherences, SH_GLOBAL_DIRECTION_ADHERENCE);
				SET_SPEEDTREE_TABLE_FLOAT4V(WindTurbulences, SH_BRANCH_1_TURBULENCE);
				SET_SPEEDTREE_TABLE_FLOAT4V(WindLeaf1Ripple, SH_LEAF_1_RIPPLE_TIME);
				SET_SPEEDTREE_TABLE_FLOAT4V(WindLeaf1Tumble, SH_LEAF_1_TUMBLE_TIME);
				SET_SPEEDTREE_TABLE_FLOAT4V(WindLeaf1Twitch, SH_LEAF_1_TWITCH_THROW);
				SET_SPEEDTREE_TABLE_FLOAT4V(WindLeaf2Ripple, SH_LEAF_2_RIPPLE_TIME);
				SET_SPEEDTREE_TABLE_FLOAT4V(WindLeaf2Tumble, SH_LEAF_2_TUMBLE_TIME);
				SET_SPEEDTREE_TABLE_FLOAT4V(WindLeaf2Twitch, SH_LEAF_2_TWITCH_THROW);
				SET_SPEEDTREE_TABLE_FLOAT4V(WindFrondRipple, SH_FROND_RIPPLE_TIME);
				SET_SPEEDTREE_TABLE_FLOAT4V(WindRollingBranch, SH_ROLLING_BRANCH_FIELD_MIN);
				SET_SPEEDTREE_TABLE_FLOAT4V(WindRollingLeafAndDirection, SH_ROLLING_LEAF_RIPPLE_MIN);
				SET_SPEEDTREE_TABLE_FLOAT4V(WindRollingNoise, SH_ROLLING_NOISE_PERIOD);

				WindComputation->UniformBuffer.UpdateUniformBufferImmediate(UniformParameters);
			}
		});
	#undef SET_SPEEDTREE_TABLE_FLOAT4V
}

FUniformBufferRHIParamRef FScene::GetSpeedTreeUniformBuffer(const FVertexFactory* VertexFactory) const
{
	if (VertexFactory != NULL)
	{
		const UStaticMesh* const* StaticMesh = SpeedTreeVertexFactoryMap.Find(VertexFactory);
		if (StaticMesh != NULL)
		{
			const FSpeedTreeWindComputation* const * WindComputation = SpeedTreeWindComputationMap.Find(*StaticMesh);
			if (WindComputation != NULL)
			{
				return (*WindComputation)->UniformBuffer;
			}
		}
	}

	return FUniformBufferRHIParamRef();
}

/**
 * Retrieves the lights interacting with the passed in primitive and adds them to the out array.
 *
 * Render thread version of function.
 * 
 * @param	Primitive				Primitive to retrieve interacting lights for
 * @param	RelevantLights	[out]	Array of lights interacting with primitive
 */
void FScene::GetRelevantLights_RenderThread( UPrimitiveComponent* Primitive, TArray<const ULightComponent*>* RelevantLights ) const
{
	check( Primitive );
	check( RelevantLights );
	if( Primitive->SceneProxy )
	{
		for( const FLightPrimitiveInteraction* Interaction=Primitive->SceneProxy->GetPrimitiveSceneInfo()->LightList; Interaction; Interaction=Interaction->GetNextLight() )
		{
			RelevantLights->Add( Interaction->GetLight()->Proxy->GetLightComponent() );
		}
	}
}

/**
 * Retrieves the lights interacting with the passed in primitive and adds them to the out array.
 *
 * @param	Primitive				Primitive to retrieve interacting lights for
 * @param	RelevantLights	[out]	Array of lights interacting with primitive
 */
void FScene::GetRelevantLights( UPrimitiveComponent* Primitive, TArray<const ULightComponent*>* RelevantLights ) const
{
	if( Primitive && RelevantLights )
	{
		// Add interacting lights to the array.
		const FScene* Scene = this;
		ENQUEUE_RENDER_COMMAND(FGetRelevantLightsCommand)(
			[Scene, Primitive, RelevantLights](FRHICommandListImmediate& RHICmdList)
			{
				Scene->GetRelevantLights_RenderThread( Primitive, RelevantLights );
			});

		// We need to block the main thread as the rendering thread needs to finish modifying the array before we can continue.
		FlushRenderingCommands();
	}
}

/** Sets the precomputed visibility handler for the scene, or NULL to clear the current one. */
void FScene::SetPrecomputedVisibility(const FPrecomputedVisibilityHandler* NewPrecomputedVisibilityHandler)
{
	ENQUEUE_UNIQUE_RENDER_COMMAND_TWOPARAMETER(
		UpdatePrecomputedVisibility,
		FScene*,Scene,this,
		const FPrecomputedVisibilityHandler*,PrecomputedVisibilityHandler,NewPrecomputedVisibilityHandler,
	{
		Scene->PrecomputedVisibilityHandler = PrecomputedVisibilityHandler;
	});
}

void FScene::UpdateStaticDrawLists_RenderThread(FRHICommandListImmediate& RHICmdList)
{
	SCOPE_CYCLE_COUNTER(STAT_Scene_UpdateStaticDrawLists_RT);

	const int32 NumPrimitives = Primitives.Num();

	for (int32 PrimitiveIndex = 0; PrimitiveIndex < NumPrimitives; ++PrimitiveIndex)
	{
		FPrimitiveSceneInfo* Primitive = Primitives[PrimitiveIndex];

		Primitive->RemoveStaticMeshes();
		Primitive->AddStaticMeshes(RHICmdList);
	}
}

void FScene::UpdateStaticDrawLists()
{
	ENQUEUE_UNIQUE_RENDER_COMMAND_ONEPARAMETER(
		FUpdateDrawLists,
		FScene*, Scene, this,
		{
			Scene->UpdateStaticDrawLists_RenderThread(RHICmdList);
		});
}

/**
 * @return		true if hit proxies should be rendered in this scene.
 */
bool FScene::RequiresHitProxies() const
{
	return (GIsEditor && bRequiresHitProxies);
}

void FScene::Release()
{
#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
	// Verify that no components reference this scene being destroyed
	static bool bTriggeredOnce = false;

	if (!bTriggeredOnce)
	{
		for (auto* ActorComponent : TObjectRange<UActorComponent>())
		{
			if ( !ensureMsgf(!ActorComponent->IsRegistered() || ActorComponent->GetScene() != this,
					TEXT("Component Name: %s World Name: %s Component Asset: %s"),
										*ActorComponent->GetFullName(),
										*GetWorld()->GetFullName(),
										*ActorComponent->AdditionalStatObject()->GetPathName()) )
			{
				bTriggeredOnce = true;
				break;
			}
		}
	}
#endif

	GetRendererModule().RemoveScene(this);

	// Send a command to the rendering thread to release the scene.
	ENQUEUE_UNIQUE_RENDER_COMMAND_ONEPARAMETER(
		FReleaseCommand,
		FScene*,Scene,this,
		{
			delete Scene;
		});
}

void FScene::ConditionalMarkStaticMeshElementsForUpdate()
{
	static auto* EarlyZPassCvar = IConsoleManager::Get().FindTConsoleVariableDataInt(TEXT("r.EarlyZPass"));
	static auto* ShaderPipelinesCvar = IConsoleManager::Get().FindTConsoleVariableDataInt(TEXT("r.ShaderPipelines"));

	bool bMobileHDR = IsMobileHDR();
	bool bMobileHDR32bpp = IsMobileHDR32bpp();
	int32 DesiredStaticDrawListsEarlyZPassMode = EarlyZPassCvar->GetValueOnRenderThread();
	int32 DesiredStaticDrawShaderPipelines = ShaderPipelinesCvar->GetValueOnRenderThread();

	if (bScenesPrimitivesNeedStaticMeshElementUpdate
		|| bStaticDrawListsMobileHDR != bMobileHDR
		|| bStaticDrawListsMobileHDR32bpp != bMobileHDR32bpp
		|| StaticDrawShaderPipelines != DesiredStaticDrawShaderPipelines
		|| StaticDrawListsEarlyZPassMode != DesiredStaticDrawListsEarlyZPassMode)
	{
		// Mark all primitives as needing an update
		// Note: Only visible primitives will actually update their static mesh elements
		for (int32 PrimitiveIndex = 0; PrimitiveIndex < Primitives.Num(); PrimitiveIndex++)
		{
			Primitives[PrimitiveIndex]->BeginDeferredUpdateStaticMeshes();
		}

		bScenesPrimitivesNeedStaticMeshElementUpdate = false;
		bStaticDrawListsMobileHDR = bMobileHDR;
		bStaticDrawListsMobileHDR32bpp = bMobileHDR32bpp;
		StaticDrawListsEarlyZPassMode = DesiredStaticDrawListsEarlyZPassMode;
		StaticDrawShaderPipelines = DesiredStaticDrawShaderPipelines;
	}
}

void FScene::DumpUnbuiltLightInteractions( FOutputDevice& Ar ) const
{
	FlushRenderingCommands();

	TSet<FString> LightsWithUnbuiltInteractions;
	TSet<FString> PrimitivesWithUnbuiltInteractions;

	// if want to print out all of the lights
	for( TSparseArray<FLightSceneInfoCompact>::TConstIterator It(Lights); It; ++It )
	{
		const FLightSceneInfoCompact& LightCompactInfo = *It;
		FLightSceneInfo* LightSceneInfo = LightCompactInfo.LightSceneInfo;

		bool bLightHasUnbuiltInteractions = false;

		for(FLightPrimitiveInteraction* Interaction = LightSceneInfo->DynamicInteractionOftenMovingPrimitiveList;
			Interaction;
			Interaction = Interaction->GetNextPrimitive())
		{
			if (Interaction->IsUncachedStaticLighting())
			{
				bLightHasUnbuiltInteractions = true;
				PrimitivesWithUnbuiltInteractions.Add(Interaction->GetPrimitiveSceneInfo()->ComponentForDebuggingOnly->GetFullName());
			}
		}

		for(FLightPrimitiveInteraction* Interaction = LightSceneInfo->DynamicInteractionStaticPrimitiveList;
			Interaction;
			Interaction = Interaction->GetNextPrimitive())
		{
			if (Interaction->IsUncachedStaticLighting())
			{
				bLightHasUnbuiltInteractions = true;
				PrimitivesWithUnbuiltInteractions.Add(Interaction->GetPrimitiveSceneInfo()->ComponentForDebuggingOnly->GetFullName());
			}
		}

		if (bLightHasUnbuiltInteractions)
		{
			LightsWithUnbuiltInteractions.Add(LightSceneInfo->Proxy->GetComponentName().ToString());
		}
	}

	Ar.Logf( TEXT( "DumpUnbuiltLightIteractions" ) );
	Ar.Logf( TEXT( "Lights with unbuilt interactions: %d" ), LightsWithUnbuiltInteractions.Num() );
	for (auto &LightName : LightsWithUnbuiltInteractions)
	{
		Ar.Logf(TEXT("    Light %s"), *LightName);
	}

	Ar.Logf( TEXT( "" ) );
	Ar.Logf( TEXT( "Primitives with unbuilt interactions: %d" ), PrimitivesWithUnbuiltInteractions.Num() );
	for (auto &PrimitiveName : PrimitivesWithUnbuiltInteractions)
	{
		Ar.Logf(TEXT("    Primitive %s"), *PrimitiveName);
	}
}

/**
 * Exports the scene.
 *
 * @param	Ar		The Archive used for exporting.
 **/
void FScene::Export( FArchive& Ar ) const
{
	
}

void FScene::ApplyWorldOffset(FVector InOffset)
{
	// Send a command to the rendering thread to shift scene data
	ENQUEUE_UNIQUE_RENDER_COMMAND_TWOPARAMETER(
		FApplyWorldOffset,
		FScene*,Scene,this,
		FVector,InOffset,InOffset,
	{
		Scene->ApplyWorldOffset_RenderThread(InOffset);
	});
}

void FScene::ApplyWorldOffset_RenderThread(FVector InOffset)
{
	QUICK_SCOPE_CYCLE_COUNTER(STAT_SceneApplyWorldOffset);
	
	// Primitives
	for (auto It = Primitives.CreateIterator(); It; ++It)
	{
		(*It)->ApplyWorldOffset(InOffset);
	}

	// Precomputed light volumes
	for (const FPrecomputedLightVolume* It : PrecomputedLightVolumes)
	{
		const_cast<FPrecomputedLightVolume*>(It)->ApplyWorldOffset(InOffset);
	}

	// Precomputed visibility
	if (PrecomputedVisibilityHandler)
	{
		const_cast<FPrecomputedVisibilityHandler*>(PrecomputedVisibilityHandler)->ApplyWorldOffset(InOffset);
	}
	
	// Invalidate indirect lighting cache
	IndirectLightingCache.SetLightingCacheDirty(this, NULL);

	// Primitives octree
	PrimitiveOctree.ApplyOffset(InOffset, /*bGlobalOctee*/ true);

	// Primitive bounds
	for (auto It = PrimitiveBounds.CreateIterator(); It; ++It)
	{
		(*It).BoxSphereBounds.Origin+= InOffset;
	}

	// Primitive occlusion bounds
	for (auto It = PrimitiveOcclusionBounds.CreateIterator(); It; ++It)
	{
		(*It).Origin+= InOffset;
	}

	// Lights
	VectorRegister OffsetReg = VectorLoadFloat3_W0(&InOffset);
	for (auto It = Lights.CreateIterator(); It; ++It)
	{
		(*It).BoundingSphereVector = VectorAdd((*It).BoundingSphereVector, OffsetReg);
		(*It).LightSceneInfo->Proxy->ApplyWorldOffset(InOffset);
	}

	// Lights octree
	LightOctree.ApplyOffset(InOffset, /*bGlobalOctee*/ true);

	// Cached preshadows
	for (auto It = CachedPreshadows.CreateIterator(); It; ++It)
	{
		(*It)->PreShadowTranslation-= InOffset;
		(*It)->ShadowBounds.Center+= InOffset;
	}

	// Decals
	for (auto It = Decals.CreateIterator(); It; ++It)
	{
		(*It)->ComponentTrans.AddToTranslation(InOffset);
	}

	// Wind sources
	for (auto It = WindSources.CreateIterator(); It; ++It)
	{
		(*It)->ApplyWorldOffset(InOffset);
	}

	// Reflection captures
	for (auto It = ReflectionSceneData.RegisteredReflectionCaptures.CreateIterator(); It; ++It)
	{
		FMatrix NewTransform = (*It)->BoxTransform.Inverse().ConcatTranslation(InOffset);
		(*It)->SetTransform(NewTransform);
	}

	// Planar reflections
	for (auto It = PlanarReflections.CreateIterator(); It; ++It)
	{
		(*It)->ApplyWorldOffset(InOffset);
	}
	
	// Exponential Fog
	for (FExponentialHeightFogSceneInfo& FogInfo : ExponentialFogs)
	{
		for (FExponentialHeightFogSceneInfo::FExponentialHeightFogSceneData& FogData : FogInfo.FogData)
		{
			FogData.Height += InOffset.Z;
		}
	}
	
	VelocityData.ApplyOffset(InOffset);
}

void FScene::OnLevelAddedToWorld(FName LevelAddedName, UWorld* InWorld, bool bIsLightingScenario)
{
	if (bIsLightingScenario)
	{
		InWorld->PropagateLightingScenarioChange();
	}

	ENQUEUE_UNIQUE_RENDER_COMMAND_TWOPARAMETER(
		FLevelAddedToWorld,
		class FScene*, Scene, this,
		FName, LevelName, LevelAddedName,
	{
		Scene->OnLevelAddedToWorld_RenderThread(LevelName);
	});
}

void FScene::OnLevelAddedToWorld_RenderThread(FName InLevelName)
{
	// Mark level primitives
	for (auto It = Primitives.CreateIterator(); It; ++It)
	{
		FPrimitiveSceneProxy* Proxy = (*It)->Proxy;
		if (Proxy->LevelName == InLevelName)
		{
			Proxy->bIsComponentLevelVisible = true;
			if (Proxy->NeedsLevelAddedToWorldNotification())
			{
				Proxy->OnLevelAddedToWorld();
			}
		}
	}
}

void FScene::OnLevelRemovedFromWorld(UWorld* InWorld, bool bIsLightingScenario)
{
	if (bIsLightingScenario)
	{
		InWorld->PropagateLightingScenarioChange();
	}
}

#if WITH_EDITOR
bool FScene::InitializePixelInspector(FRenderTarget* BufferFinalColor, FRenderTarget* BufferSceneColor, FRenderTarget* BufferDepth, FRenderTarget* BufferHDR, FRenderTarget* BufferA, FRenderTarget* BufferBCDE, int32 BufferIndex)
{
	//Initialize the buffers
	PixelInspectorData.InitializeBuffers(BufferFinalColor, BufferSceneColor, BufferDepth, BufferHDR, BufferA, BufferBCDE, BufferIndex);
	//return true when the interface is implemented
	return true;
}

bool FScene::AddPixelInspectorRequest(FPixelInspectorRequest *PixelInspectorRequest)
{
	return PixelInspectorData.AddPixelInspectorRequest(PixelInspectorRequest);
}
#endif //WITH_EDITOR

/**
 * Dummy NULL scene interface used by dedicated servers.
 */
class FNULLSceneInterface : public FSceneInterface
{
public:
	FNULLSceneInterface(UWorld* InWorld, bool bCreateFXSystem )
		:	FSceneInterface(GMaxRHIFeatureLevel)
		,	World( InWorld )
		,	FXSystem( NULL )
	{
		World->Scene = this;

		if (bCreateFXSystem)
		{
			World->CreateFXSystem();
		}
		else
		{
			World->FXSystem = NULL;
			SetFXSystem(NULL);
		}
	}

	virtual void AddPrimitive(UPrimitiveComponent* Primitive) override {}
	virtual void RemovePrimitive(UPrimitiveComponent* Primitive) override {}
	virtual void ReleasePrimitive(UPrimitiveComponent* Primitive) override {}
	virtual FPrimitiveSceneInfo* GetPrimitiveSceneInfo(int32 PrimiteIndex) override { return NULL; }

	/** Updates the transform of a primitive which has already been added to the scene. */
	virtual void UpdatePrimitiveTransform(UPrimitiveComponent* Primitive) override {}
	virtual void UpdatePrimitiveAttachment(UPrimitiveComponent* Primitive) override {}

	virtual void AddLight(ULightComponent* Light) override {}
	virtual void RemoveLight(ULightComponent* Light) override {}
	virtual void AddInvisibleLight(ULightComponent* Light) override {}
	virtual void SetSkyLight(FSkyLightSceneProxy* Light) override {}
	virtual void DisableSkyLight(FSkyLightSceneProxy* Light) override {}

	virtual void AddDecal(UDecalComponent*) override {}
	virtual void RemoveDecal(UDecalComponent*) override {}
	virtual void UpdateDecalTransform(UDecalComponent* Decal) override {}
	virtual void UpdateDecalFadeOutTime(UDecalComponent* Decal) override {};
	virtual void UpdateDecalFadeInTime(UDecalComponent* Decal) override {};

	/** Updates the transform of a light which has already been added to the scene. */
	virtual void UpdateLightTransform(ULightComponent* Light) override {}
	virtual void UpdateLightColorAndBrightness(ULightComponent* Light) override {}

	virtual void AddExponentialHeightFog(class UExponentialHeightFogComponent* FogComponent) override {}
	virtual void RemoveExponentialHeightFog(class UExponentialHeightFogComponent* FogComponent) override {}
	virtual void AddAtmosphericFog(class UAtmosphericFogComponent* FogComponent) override {}
	virtual void RemoveAtmosphericFog(class UAtmosphericFogComponent* FogComponent) override {}
	virtual void RemoveAtmosphericFogResource_RenderThread(FRenderResource* FogResource) override {}
	virtual FAtmosphericFogSceneInfo* GetAtmosphericFogSceneInfo() override { return NULL; }
	virtual void AddWindSource(class UWindDirectionalSourceComponent* WindComponent) override {}
	virtual void RemoveWindSource(class UWindDirectionalSourceComponent* WindComponent) override {}
	virtual const TArray<class FWindSourceSceneProxy*>& GetWindSources_RenderThread() const override
	{
		static TArray<class FWindSourceSceneProxy*> NullWindSources;
		return NullWindSources;
	}
	virtual void GetWindParameters(const FVector& Position, FVector& OutDirection, float& OutSpeed, float& OutMinGustAmt, float& OutMaxGustAmt) const override { OutDirection = FVector(1.0f, 0.0f, 0.0f); OutSpeed = 0.0f; OutMinGustAmt = 0.0f; OutMaxGustAmt = 0.0f; }
	virtual void GetWindParameters_GameThread(const FVector& Position, FVector& OutDirection, float& OutSpeed, float& OutMinGustAmt, float& OutMaxGustAmt) const override { OutDirection = FVector(1.0f, 0.0f, 0.0f); OutSpeed = 0.0f; OutMinGustAmt = 0.0f; OutMaxGustAmt = 0.0f; }
	virtual void GetDirectionalWindParameters(FVector& OutDirection, float& OutSpeed, float& OutMinGustAmt, float& OutMaxGustAmt) const override { OutDirection = FVector(1.0f, 0.0f, 0.0f); OutSpeed = 0.0f; OutMinGustAmt = 0.0f; OutMaxGustAmt = 0.0f; }
	virtual void AddSpeedTreeWind(class FVertexFactory* VertexFactory, const class UStaticMesh* StaticMesh) override {}
	virtual void RemoveSpeedTreeWind_RenderThread(class FVertexFactory* VertexFactory, const class UStaticMesh* StaticMesh) override {}
	virtual void UpdateSpeedTreeWind(double CurrentTime) override {}
	virtual FUniformBufferRHIParamRef GetSpeedTreeUniformBuffer(const FVertexFactory* VertexFactory) const override { return FUniformBufferRHIParamRef(); }

	virtual void Release() override {}

	/**
	 * Retrieves the lights interacting with the passed in primitive and adds them to the out array.
	 *
	 * @param	Primitive				Primitive to retrieve interacting lights for
	 * @param	RelevantLights	[out]	Array of lights interacting with primitive
	 */
	virtual void GetRelevantLights( UPrimitiveComponent* Primitive, TArray<const ULightComponent*>* RelevantLights ) const override {}

	/**
	 * @return		true if hit proxies should be rendered in this scene.
	 */
	virtual bool RequiresHitProxies() const override
	{
		return false;
	}

	// Accessors.
	virtual class UWorld* GetWorld() const override
	{
		return World;
	}

	/**
	* Return the scene to be used for rendering
	*/
	virtual class FScene* GetRenderScene() override
	{
		return NULL;
	}

	/**
	 * Sets the FX system associated with the scene.
	 */
	virtual void SetFXSystem( class FFXSystemInterface* InFXSystem ) override
	{
		FXSystem = InFXSystem;
	}

	/**
	 * Get the FX system associated with the scene.
	 */
	virtual class FFXSystemInterface* GetFXSystem() override
	{
		return FXSystem;
	}

	virtual bool HasAnyLights() const override { return false; }
private:
	UWorld* World;
	class FFXSystemInterface* FXSystem;
};

FSceneInterface* FRendererModule::AllocateScene(UWorld* World, bool bInRequiresHitProxies, bool bCreateFXSystem, ERHIFeatureLevel::Type InFeatureLevel)
{
	check(IsInGameThread());

	// Create a full fledged scene if we have something to render.
	if (GIsClient && FApp::CanEverRender() && !GUsingNullRHI)
	{
		FScene* NewScene = new FScene(World, bInRequiresHitProxies, GIsEditor && (!World || !World->IsGameWorld()), bCreateFXSystem, InFeatureLevel);
		AllocatedScenes.Add(NewScene);
		return NewScene;
	}
	// And fall back to a dummy/ NULL implementation for commandlets and dedicated server.
	else
	{
		return new FNULLSceneInterface(World, bCreateFXSystem);
	}
}

void FRendererModule::RemoveScene(FSceneInterface* Scene)
{
	check(IsInGameThread());
	AllocatedScenes.Remove(Scene);
}

void FRendererModule::UpdateStaticDrawLists()
{
	// Update all static meshes in order to recache cached mesh draw commands.
	for (TSet<FSceneInterface*>::TConstIterator SceneIt(AllocatedScenes); SceneIt; ++SceneIt)
	{
		(*SceneIt)->UpdateStaticDrawLists();
	}
}

void UpdateStaticMeshesForMaterials(const TArray<const FMaterial*>& MaterialResourcesToUpdate)
{
	TArray<UMaterialInterface*> UsedMaterials;

	for (TObjectIterator<UPrimitiveComponent> PrimitiveIt; PrimitiveIt; ++PrimitiveIt)
	{
		UPrimitiveComponent* PrimitiveComponent = *PrimitiveIt;

		if (PrimitiveComponent->IsRenderStateCreated() && PrimitiveComponent->SceneProxy)
		{
			UsedMaterials.Reset();
			bool bPrimitiveIsDependentOnMaterial = false;

			// Note: relying on GetUsedMaterials to be accurate, or else we won't propagate to the right primitives and the renderer will crash later
			// FPrimitiveSceneProxy::VerifyUsedMaterial is used to make sure that all materials used for rendering are reported in GetUsedMaterials
			PrimitiveComponent->GetUsedMaterials(UsedMaterials);

			if (UsedMaterials.Num() > 0)
			{
				for (TArray<const FMaterial*>::TConstIterator MaterialIt(MaterialResourcesToUpdate); MaterialIt; ++MaterialIt)
				{
					UMaterialInterface* UpdatedMaterialInterface = (*MaterialIt)->GetMaterialInterface();

					if (UpdatedMaterialInterface)
					{
						for (int32 MaterialIndex = 0; MaterialIndex < UsedMaterials.Num(); MaterialIndex++)
						{
							UMaterialInterface* TestMaterial = UsedMaterials[MaterialIndex];

							if (TestMaterial && (TestMaterial == UpdatedMaterialInterface || TestMaterial->IsDependent(UpdatedMaterialInterface)))
							{
								bPrimitiveIsDependentOnMaterial = true;
								break;
							}
						}
					}
				}

				if (bPrimitiveIsDependentOnMaterial)
				{
					ENQUEUE_UNIQUE_RENDER_COMMAND_ONEPARAMETER(
						FUpdateStaticMeshesForMaterials,
						FPrimitiveSceneProxy*, SceneProxy, PrimitiveComponent->SceneProxy,
						{
							SceneProxy->GetPrimitiveSceneInfo()->UpdateStaticMeshes(RHICmdList);
						});
				}
			}
		}
	}
}

void FRendererModule::UpdateStaticDrawListsForMaterials(const TArray<const FMaterial*>& Materials)
{
	// Update static meshes for a given set of materials in order to recache cached mesh draw commands.
	UpdateStaticMeshesForMaterials(Materials);
}

FSceneViewStateInterface* FRendererModule::AllocateViewState()
{
	return new FSceneViewState();
}

//////////////////////////////////////////////////////////////////////////

FLatentGPUTimer::FLatentGPUTimer(int32 InAvgSamples)
	: AvgSamples(InAvgSamples)
	, TotalTime(0.0f)
	, SampleIndex(0)
	, QueryIndex(0)
{
	TimeSamples.AddZeroed(AvgSamples);
}

bool FLatentGPUTimer::Tick(FRHICommandListImmediate& RHICmdList)
{
	if (GSupportsTimestampRenderQueries == false)
	{
		return false;
	}

	QueryIndex = (QueryIndex + 1) % NumBufferedFrames;

	if (StartQueries[QueryIndex] && EndQueries[QueryIndex])
	{
		if (IsRunningRHIInSeparateThread())
		{
			// Block until the RHI thread has processed the previous query commands, if necessary
			// Stat disabled since we buffer 2 frames minimum, it won't actually block
			//SCOPE_CYCLE_COUNTER(STAT_TranslucencyTimestampQueryFence_Wait);
			int32 BlockFrame = NumBufferedFrames - 1;
			FRHICommandListExecutor::WaitOnRHIThreadFence(QuerySubmittedFences[BlockFrame]);
			QuerySubmittedFences[BlockFrame] = nullptr;
		}

		uint64 StartMicroseconds;
		uint64 EndMicroseconds;
		bool bStartSuccess;
		bool bEndSuccess;

		{
			// Block on the GPU until we have the timestamp query results, if necessary
			// Stat disabled since we buffer 2 frames minimum, it won't actually block
			//SCOPE_CYCLE_COUNTER(STAT_TranslucencyTimestampQuery_Wait);
			bStartSuccess = RHICmdList.GetRenderQueryResult(StartQueries[QueryIndex], StartMicroseconds, true);
			bEndSuccess = RHICmdList.GetRenderQueryResult(EndQueries[QueryIndex], EndMicroseconds, true);
		}

		TotalTime -= TimeSamples[SampleIndex];
		float LastFrameTranslucencyDurationMS = TimeSamples[SampleIndex];
		if (bStartSuccess && bEndSuccess)
		{
			LastFrameTranslucencyDurationMS = (EndMicroseconds - StartMicroseconds) / 1000.0f;
		}

		TimeSamples[SampleIndex] = LastFrameTranslucencyDurationMS;
		TotalTime += LastFrameTranslucencyDurationMS;
		SampleIndex = (SampleIndex + 1) % AvgSamples;

		return bStartSuccess && bEndSuccess;
	}

	return false;
}

void FLatentGPUTimer::Begin(FRHICommandListImmediate& RHICmdList)
{
	if (GSupportsTimestampRenderQueries == false)
	{
		return;
	}
	
	if (!StartQueries[QueryIndex])
	{		
		StartQueries[QueryIndex] = RHICmdList.CreateRenderQuery(RQT_AbsoluteTime);
	}

	RHICmdList.EndRenderQuery(StartQueries[QueryIndex]);
}

void FLatentGPUTimer::End(FRHICommandListImmediate& RHICmdList)
{
	if (GSupportsTimestampRenderQueries == false)
	{
		return;
	}
	
	if (!EndQueries[QueryIndex])
	{
		EndQueries[QueryIndex] = RHICmdList.CreateRenderQuery(RQT_AbsoluteTime);
	}

	RHICmdList.EndRenderQuery(EndQueries[QueryIndex]);
	// Hint to the RHI to submit commands up to this point to the GPU if possible.  Can help avoid CPU stalls next frame waiting
	// for these query results on some platforms.
	RHICmdList.SubmitCommandsHint();

	if (IsRunningRHIInSeparateThread())
	{
		int32 NumFrames = NumBufferedFrames;
		for (int32 Dest = 1; Dest < NumFrames; Dest++)
		{
			QuerySubmittedFences[Dest] = QuerySubmittedFences[Dest - 1];
		}
		// Start an RHI thread fence so we can be sure the RHI thread has processed the EndRenderQuery before we ask for results
		QuerySubmittedFences[0] = RHICmdList.RHIThreadFence();
		RHICmdList.ImmediateFlush(EImmediateFlushType::DispatchToRHIThread);
	}
}

void FLatentGPUTimer::Release()
{
	for (int32 i = 0; i < NumBufferedFrames; ++i)
	{
		StartQueries[i].SafeRelease();
		EndQueries[i].SafeRelease();
	}
}

float FLatentGPUTimer::GetTimeMS()
{
	return TimeSamples[SampleIndex];
}

float FLatentGPUTimer::GetAverageTimeMS()
{
	return TotalTime / AvgSamples;
}

