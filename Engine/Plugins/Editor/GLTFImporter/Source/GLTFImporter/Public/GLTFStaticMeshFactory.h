// Copyright 1998-2019 Epic Games, Inc. All Rights Reserved.

#pragma once

#include "GLTFLogger.h"

#include "CoreMinimal.h"
#include "Templates/UniquePtr.h"
#include "UObject/ObjectMacros.h"

class UStaticMesh;

namespace GLTF
{
	struct FAsset;
	class FStaticMeshFactoryImpl;

	class GLTFIMPORTER_API FStaticMeshFactory
	{
	public:
		FStaticMeshFactory();
		~FStaticMeshFactory();

		const TArray<UStaticMesh*>& CreateMeshes(const GLTF::FAsset& Asset, UObject* ParentPackage, EObjectFlags Flags, bool bApplyPostEditChange);

		const TArray<UStaticMesh*>& GetMeshes() const;
		const TArray<FLogMessage>&  GetLogMessages() const;

		float GetUniformScale() const;
		void  SetUniformScale(float Scale);

		bool GetGenerateLightmapUVs() const;
		void SetGenerateLightmapUVs(bool bGenerateLightmapUVs);

		void SetReserveSize(uint32 Size);

		void CleanUp();

	private:
		TUniquePtr<FStaticMeshFactoryImpl> Impl;
	};

}  // namespace GLTF
