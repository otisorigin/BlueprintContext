#pragma once

#include "CoreMinimal.h"

class FSQLiteDatabase;
class FSQLitePreparedStatement;

// ============================================================
// Data structures for DB records
// ============================================================

struct FMCPAssetRecord
{
	int64 Id = -1;
	FString Name;
	FString Path;
	FString AssetType;   // "Blueprint", "DataTable", "DataAsset", "Interface", "Struct"
	FString Parent;
	FString Folder;
	int64 ModifiedAt = 0; // Unix timestamp
	FString HeaderHash;
	// DataTable-specific
	FString RowStruct;
	int32 RowCount = 0;
	// DataAsset-specific
	FString DaClass;
};

struct FMCPFunctionRecord
{
	int64 Id = -1;
	int64 AssetId = -1;
	FString Name;
	FString Category;
	FString FlagsJson;     // JSON array: ["BlueprintCallable","BlueprintPure"]
	FString SignatureJson; // JSON: {"inputs":[...],"outputs":[...]}
	FString GraphHash;
};

struct FMCPRelatedResult
{
	FString Name;
	FString Path;
	FString AssetType;
	FString Rel;
	int32 Depth = 1;
	FString PropertyPath;
	FString DepHardness;
};

// ============================================================
// FMCPDatabase — central SQLite wrapper
// ============================================================

class FMCPDatabase
{
public:
	FMCPDatabase();
	~FMCPDatabase();

	// Non-copyable
	FMCPDatabase(const FMCPDatabase&) = delete;
	FMCPDatabase& operator=(const FMCPDatabase&) = delete;

	bool Open(const FString& DbPath);
	void Close();
	bool IsOpen() const;

	// DDL
	bool CreateSchema();

	// Transactions (critical for bulk insert performance)
	void BeginTransaction();
	void CommitTransaction();
	void RollbackTransaction();

	// ---- Assets ----
	int64 UpsertAsset(const FMCPAssetRecord& Record);
	void DeleteAsset(const FString& Path);
	TOptional<FMCPAssetRecord> GetAsset(const FString& Path);
	int64 GetAssetId(const FString& Path);
	int64 GetAssetIdFresh(const FString& Path); // bypasses cache — use after long-running loads
	FString GetAssetType(int64 AssetId);

	// ---- Meta ----
	void SetMeta(int64 AssetId, const TArray<TPair<FString, FString>>& KeyValues);
	void ClearMeta(int64 AssetId);
	TArray<FString> GetMeta(int64 AssetId, const FString& Key);

	// ---- Edges ----
	void UpsertEdge(int64 FromId, int64 ToId, const FString& Rel, const FString& PropertyPath = TEXT(""), const FString& DepHardness = TEXT(""));
	void ClearEdgesFrom(int64 AssetId);

	// ---- Functions ----
	int64 UpsertFunction(int64 AssetId, const FMCPFunctionRecord& Func);
	void ClearFunctions(int64 AssetId);

	// ---- DataTable rows ----
	int32 UpsertDTRows(int64 AssetId, const TArray<TPair<FString, FString>>& Rows);
	void ClearDTRows(int64 AssetId);
	bool HasDTRows(int64 AssetId);
	void UpdateRowCount(int64 AssetId, int32 NewCount);

	// ---- Queries ----
	TArray<FMCPAssetRecord> QueryAssetsByType(const FString& AssetType);
	TArray<FMCPAssetRecord> QueryAllAssets();
	TArray<FMCPRelatedResult> GetRelated(int64 AssetId, const FString& Rel, bool bIncoming, bool bRecursive, int32 MaxDepth = 10, int32 Limit = 50, int32 Offset = 0);

	// ---- Raw SQL ----
	bool Execute(const FString& Sql);
	int64 ExecuteWithCallback(const FString& Sql, TFunction<void(const FSQLitePreparedStatement&)> RowCallback);

	// ---- Stats ----
	int32 GetAssetCount();
	int32 GetAssetCountByType(const FString& AssetType);
	int32 GetEdgeCount();

	// ---- DT Row queries ----
	TArray<TPair<FString, FString>> GetDTRows(int64 AssetId, const TArray<FString>& RowNames, int32 Limit = 20);
	int32 GetDTRowCount(int64 AssetId);

private:
	void InvalidatePathCache(const FString& Path);
	void RebuildPathCache();
	FMCPAssetRecord RowToAssetRecord(const FSQLitePreparedStatement& Stmt) const;

	TUniquePtr<FSQLiteDatabase> Db;
	TMap<FString, int64> PathToIdCache;
	bool bCacheBuilt = false;
};
