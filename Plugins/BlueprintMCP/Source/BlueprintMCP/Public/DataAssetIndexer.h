#pragma once

#include "CoreMinimal.h"

class FMCPDatabase;
struct FAssetData;

class FDataAssetIndexer
{
public:
	void IndexAllDataTables(FMCPDatabase& DB);
	void IndexAllDataAssets(FMCPDatabase& DB);

	void IndexDataTable(const FAssetData& Asset, FMCPDatabase& DB);
	void IndexDataAsset(const FAssetData& Asset, FMCPDatabase& DB);

private:
	int64 GetAssetTimestamp(const FAssetData& Asset);
	bool ShouldExcludePath(const FString& AssetPath) const;
};
