#include "AssetClassifier.h"
#include "AssetRegistry/AssetData.h"
#include "Engine/Blueprint.h"
#include "Engine/DataTable.h"
#include "Engine/DataAsset.h"
#include "Engine/CompositeDataTable.h"
#include "Engine/StaticMesh.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/Texture.h"
#include "Engine/Font.h"
#include "Engine/FontFace.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialFunction.h"
#include "Sound/SoundBase.h"
#include "Sound/SoundCue.h"
#include "Sound/SoundWave.h"
#include "Animation/AnimSequenceBase.h"
#include "Animation/BlendSpace.h"
#include "Animation/PoseAsset.h"
#include "Animation/Skeleton.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "Particles/ParticleSystem.h"
#include "Engine/World.h"
#include "Curves/CurveBase.h"
#include "Internationalization/StringTable.h"

DEFINE_LOG_CATEGORY_STATIC(LogMCPClassifier, Log, All);

// Cached class pointers for types from optional modules (Niagara)
// Resolved once at first Classify() call to avoid hard module dependency
namespace
{
	bool bNiagaraResolved = false;
	UClass* NiagaraSystemClass = nullptr;
	UClass* NiagaraEmitterClass = nullptr;

	void ResolveNiagaraClasses()
	{
		if (bNiagaraResolved) return;
		bNiagaraResolved = true;
		NiagaraSystemClass = FindObject<UClass>(nullptr, TEXT("/Script/Niagara.NiagaraSystem"));
		NiagaraEmitterClass = FindObject<UClass>(nullptr, TEXT("/Script/Niagara.NiagaraEmitter"));
	}
}

FAssetClassifier::FAssetClassifier()
{
	LoadConfig();
}

void FAssetClassifier::LoadConfig()
{
	// ExtraIncludePaths
	FString IncludeStr;
	if (GConfig->GetString(TEXT("BlueprintMCP"), TEXT("ExtraIncludePaths"), IncludeStr, GEditorIni))
	{
		IncludeStr.ParseIntoArray(Config.ExtraIncludePaths, TEXT(","), true);
		for (FString& P : Config.ExtraIncludePaths) P.TrimStartAndEndInline();
	}

	// ExcludeFolders
	FString ExcludeStr;
	if (GConfig->GetString(TEXT("BlueprintMCP"), TEXT("ExcludeFolders"), ExcludeStr, GEditorIni))
	{
		ExcludeStr.ParseIntoArray(Config.ExcludeFolders, TEXT(","), true);
		for (FString& P : Config.ExcludeFolders) P.TrimStartAndEndInline();
	}

	// ExtraSkipClasses
	FString SkipStr;
	if (GConfig->GetString(TEXT("BlueprintMCP"), TEXT("ExtraSkipClasses"), SkipStr, GEditorIni))
	{
		SkipStr.ParseIntoArray(Config.ExtraSkipClasses, TEXT(","), true);
		for (FString& P : Config.ExtraSkipClasses) P.TrimStartAndEndInline();
	}
}

bool FAssetClassifier::IsExcludedByPath(const FAssetData& Asset) const
{
	const FString Path = Asset.PackagePath.ToString();

	// /Game/ is always included — also match exact "/Game" for root-level assets
	if (Path == TEXT("/Game") || Path.StartsWith(TEXT("/Game/")))
	{
		// Check ExcludeFolders blacklist within /Game/
		for (const FString& Exclude : Config.ExcludeFolders)
		{
			if (Path.StartsWith(Exclude))
			{
				return true;
			}
		}
		return false;
	}

	// Extra include paths from config
	for (const FString& Include : Config.ExtraIncludePaths)
	{
		if (Path.StartsWith(Include))
		{
			return false;
		}
	}

	// Everything else: /Engine/, /ControlRigModules/, /NNEDenoiser/, /Plugins/ etc.
	return true;
}

EAssetCategory FAssetClassifier::Classify(const FAssetData& Asset) const
{
	const FTopLevelAssetPath ClassPath = Asset.AssetClassPath;

	// Extra skip classes from config
	FString ClassPathStr = ClassPath.ToString();
	for (const FString& SkipClass : Config.ExtraSkipClasses)
	{
		if (ClassPathStr == SkipClass)
		{
			return EAssetCategory::Skip;
		}
	}

	// ── ContentRef: visual/audio resources — indexed minimally as edge targets ──
	// Meshes
	if (IsChildOf(ClassPath, UStaticMesh::StaticClass()))          return EAssetCategory::ContentRef;
	if (IsChildOf(ClassPath, USkeletalMesh::StaticClass()))        return EAssetCategory::ContentRef;
	// Textures
	if (IsChildOf(ClassPath, UTexture::StaticClass()))             return EAssetCategory::ContentRef;
	// Materials
	if (IsChildOf(ClassPath, UMaterialInterface::StaticClass()))   return EAssetCategory::ContentRef;
	if (IsChildOf(ClassPath, UMaterialFunction::StaticClass()))    return EAssetCategory::ContentRef;
	// Sound
	if (IsChildOf(ClassPath, USoundBase::StaticClass()))           return EAssetCategory::ContentRef;
	// Animation
	if (IsChildOf(ClassPath, UAnimSequenceBase::StaticClass()))    return EAssetCategory::ContentRef;
	if (IsChildOf(ClassPath, UBlendSpace::StaticClass()))          return EAssetCategory::ContentRef;
	if (IsChildOf(ClassPath, UPoseAsset::StaticClass()))           return EAssetCategory::ContentRef;
	if (IsChildOf(ClassPath, USkeleton::StaticClass()))            return EAssetCategory::ContentRef;
	if (IsChildOf(ClassPath, UPhysicsAsset::StaticClass()))        return EAssetCategory::ContentRef;
	// VFX
	if (IsChildOf(ClassPath, UParticleSystem::StaticClass()))      return EAssetCategory::ContentRef;
	// Niagara — resolved at runtime to avoid hard module dependency
	ResolveNiagaraClasses();
	if (NiagaraSystemClass && IsChildOf(ClassPath, NiagaraSystemClass))   return EAssetCategory::ContentRef;
	if (NiagaraEmitterClass && IsChildOf(ClassPath, NiagaraEmitterClass)) return EAssetCategory::ContentRef;
	// Levels — still Skip (not useful as edge targets, large count)
	if (IsChildOf(ClassPath, UWorld::StaticClass()))               return EAssetCategory::Skip;
	// Fonts — still Skip
	if (IsChildOf(ClassPath, UFont::StaticClass()))                return EAssetCategory::Skip;
	if (IsChildOf(ClassPath, UFontFace::StaticClass()))            return EAssetCategory::Skip;

	// ── BLUEPRINT: all BP logic ──
	if (IsChildOf(ClassPath, UBlueprint::StaticClass()))           return EAssetCategory::Blueprint;

	// ── DATATABLE ──
	if (IsChildOf(ClassPath, UDataTable::StaticClass()))           return EAssetCategory::DataTable;

	// ── DATAASSET: UDataAsset and all descendants ──
	if (IsChildOf(ClassPath, UDataAsset::StaticClass()))           return EAssetCategory::DataAsset;

	// ── Useful types that are not DA ──
	if (IsChildOf(ClassPath, UCurveBase::StaticClass()))           return EAssetCategory::Generic;
	if (IsChildOf(ClassPath, UStringTable::StaticClass()))         return EAssetCategory::Generic;

	// ── Everything else inside /Game/ — index minimally ──
	return EAssetCategory::Generic;
}

bool FAssetClassifier::IsChildOf(const FTopLevelAssetPath& ClassPath, UClass* ParentClass) const
{
	UClass* AssetClass = FindObject<UClass>(nullptr, *ClassPath.ToString());
	if (!AssetClass) return false;
	return AssetClass->IsChildOf(ParentClass);
}
