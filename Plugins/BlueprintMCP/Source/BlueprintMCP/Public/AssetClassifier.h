#pragma once

#include "CoreMinimal.h"

struct FAssetData;

enum class EAssetCategory : uint8
{
	Blueprint,
	DataTable,
	DataAsset,
	Generic,
	ContentRef,  // Meshes, textures, sounds, animations — indexed as edge targets only (name+path+nativeClass)
	Skip
};

struct FAssetClassifierConfig
{
	TArray<FString> ExtraIncludePaths;
	TArray<FString> ExcludeFolders;
	TArray<FString> ExtraSkipClasses;
};

class FAssetClassifier
{
public:
	FAssetClassifier();

	/** Load config from GEditorIni [BlueprintMCP] section */
	void LoadConfig();

	/** Level 1: filter by mount point / path */
	bool IsExcludedByPath(const FAssetData& Asset) const;

	/** Level 2: classify asset by its native class */
	EAssetCategory Classify(const FAssetData& Asset) const;

	const FAssetClassifierConfig& GetConfig() const { return Config; }

private:
	bool IsChildOf(const FTopLevelAssetPath& ClassPath, UClass* ParentClass) const;

	FAssetClassifierConfig Config;
};
