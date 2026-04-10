#pragma once

#include "CoreMinimal.h"

class FMCPDatabase;
class FAssetClassifier;
class FDataAssetIndexer;
struct FMCPFunctionRecord;
struct FAssetData;
class UEdGraph;

struct FBlueprintIndexEntry
{
	FString Name;
	FString Path;
	FString Parent;
	TArray<FString> Interfaces;
	FString Folder;
	TArray<FString> VarNames;
	TArray<FString> FuncNames;
	TArray<FString> VarTypes;
	TArray<FString> Categories;
	FDateTime ModifiedAt;
	FString HeaderHash;
};

/** Stats from the last Build() run — used by IndexStats console command */
struct FIndexBuildStats
{
	int32 TotalScanned = 0;
	int32 CountBP = 0;
	int32 CountDT = 0;
	int32 CountDA = 0;
	int32 CountGeneric = 0;
	int32 CountContentRef = 0;
	int32 CountSkippedType = 0;
	int32 CountSkippedPath = 0;
	FDateTime BuildTime;
};

class FBlueprintIndexBuilder
{
public:
	FBlueprintIndexBuilder();
	~FBlueprintIndexBuilder();

	void RegisterDelegates();

	/** Unified build: scans all assets in /Game/, classifies, routes to indexers */
	void Build(FMCPDatabase& DB, const FAssetClassifier& Classifier, FDataAssetIndexer& DataIndexer);

	/** Legacy overload — builds only blueprints (for backward compat) */
	void Build(FMCPDatabase& DB);

	void RebuildEntry(const FString& AssetPath, FMCPDatabase& DB);
	void InvalidateHeader(const FString& AssetPath);

	/** Index a single blueprint asset into the database */
	void IndexAsset(const FAssetData& Asset, FMCPDatabase& DB);

	/** Index a generic (unknown but potentially useful) asset — minimal metadata */
	void IndexGenericAsset(const FAssetData& Asset, FMCPDatabase& DB);

	/** Index a content asset (mesh/texture/sound/anim) — name+path+nativeClass only, no tags, no LoadObject */
	void IndexContentAsset(const FAssetData& Asset, FMCPDatabase& DB);

	/** Populate in-memory TArray from DB (for search engine compatibility) */
	void PopulateEntriesFromDB(FMCPDatabase& DB);

	const TArray<FBlueprintIndexEntry>& GetEntries() const { return Entries; }
	int32 GetEntryCount() const { return Entries.Num(); }
	double GetIndexAgeSeconds() const;
	const FIndexBuildStats& GetLastBuildStats() const { return LastBuildStats; }

	FString GetCacheDir() const;
	FString GetHeaderCacheDir() const;

private:
	void PopulateEntryFromAsset(const FAssetData& Asset, FBlueprintIndexEntry& Entry);
	void LoadVarFuncNames(const FAssetData& Asset, FBlueprintIndexEntry& Entry);
	void IndexFunctionSignature(int64 AssetId, const UEdGraph* Graph, FMCPDatabase& DB);
	void ParseInterfaces(const FString& InterfacesRaw, TArray<FString>& OutInterfaces);
	FString ExtractClassName(const FString& ClassPath);
	int64 GetAssetTimestamp(const FAssetData& Asset);

	void OnBlueprintCompiled();
	bool OnRebuildTick(float DeltaTime);
	FDelegateHandle CompileDelegate;
	FTSTicker::FDelegateHandle RebuildTickerHandle;

	TArray<FBlueprintIndexEntry> Entries;
	TMap<FString, int32> PathToIndex;
	FDateTime IndexBuildTime;

	FIndexBuildStats LastBuildStats;

	// Stored for use in OnBlueprintCompiled callback
	FMCPDatabase* DatabasePtr = nullptr;
	const FAssetClassifier* ClassifierPtr = nullptr;
	FDataAssetIndexer* DataIndexerPtr = nullptr;

	bool bIsBuilding = false;
	bool bRebuildPending = false;
};
