#include "AssetRelationBuilder.h"
#include "MCPDatabase.h"
#include "SQLitePreparedStatement.h"
#include "AssetRegistry/AssetRegistryModule.h"

DEFINE_LOG_CATEGORY_STATIC(LogMCPRelations, Log, All);

void FAssetRelationBuilder::BuildAllEdges(FMCPDatabase& DB)
{
	TArray<FMCPAssetRecord> AllAssets = DB.QueryAllAssets();

	DB.BeginTransaction();

	for (const FMCPAssetRecord& Asset : AllAssets)
	{
		BuildEdgesForAsset(Asset.Id, Asset.Path, DB);
	}

	DB.CommitTransaction();

	UE_LOG(LogMCPRelations, Log, TEXT("Built edges for %d assets. Total edges: %d"), AllAssets.Num(), DB.GetEdgeCount());
}

void FAssetRelationBuilder::BuildEdgesForAsset(int64 AssetId, const FString& Path, FMCPDatabase& DB)
{
	if (AssetId <= 0) return;

	DB.ClearEdgesFrom(AssetId);

	TOptional<FMCPAssetRecord> AssetOpt = DB.GetAsset(Path);
	if (!AssetOpt.IsSet()) return;

	const FMCPAssetRecord& Asset = AssetOpt.GetValue();

	BuildInheritsEdge(AssetId, Asset.Parent, DB);
	BuildImplementsEdges(AssetId, DB);
	BuildReferenceEdges(AssetId, Path, DB);
	BuildVarTypeEdges(AssetId, DB);
}

void FAssetRelationBuilder::BuildInheritsEdge(int64 FromId, const FString& Parent, FMCPDatabase& DB)
{
	if (Parent.IsEmpty()) return;

	// Try to find parent in DB (only works for Blueprint parents, not C++ classes)
	// Search by name since Parent is a class name, not a path
	// We need to find an asset whose name matches the parent
	FString Sql = FString::Printf(TEXT("SELECT id FROM assets WHERE name = '%s' LIMIT 1"), *Parent.Replace(TEXT("'"), TEXT("''")));
	int64 ToId = -1;
	DB.ExecuteWithCallback(Sql, [&](const FSQLitePreparedStatement& Row)
	{
		Row.GetColumnValueByIndex(0, ToId);
	});

	if (ToId > 0 && ToId != FromId)
	{
		DB.UpsertEdge(FromId, ToId, TEXT("INHERITS"));
	}
}

void FAssetRelationBuilder::BuildImplementsEdges(int64 FromId, FMCPDatabase& DB)
{
	TArray<FString> Interfaces = DB.GetMeta(FromId, TEXT("interface"));
	for (const FString& Iface : Interfaces)
	{
		// Search for interface asset by name
		FString Sql = FString::Printf(TEXT("SELECT id FROM assets WHERE name = '%s' LIMIT 1"), *Iface.Replace(TEXT("'"), TEXT("''")));
		int64 ToId = -1;
		DB.ExecuteWithCallback(Sql, [&](const FSQLitePreparedStatement& Row)
		{
			Row.GetColumnValueByIndex(0, ToId);
		});

		if (ToId > 0 && ToId != FromId)
		{
			DB.UpsertEdge(FromId, ToId, TEXT("IMPLEMENTS"));
		}
	}
}

void FAssetRelationBuilder::BuildReferenceEdges(int64 FromId, const FString& Path, FMCPDatabase& DB)
{
	IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();

	FString PackageName = FPackageName::ObjectPathToPackageName(Path);
	FName PackageFName(*PackageName);

	// Query hard and soft deps separately for classification
	auto ProcessDeps = [&](const TArray<FName>& Deps, const FString& Hardness)
	{
		for (const FName& DepName : Deps)
		{
			FString DepPackage = DepName.ToString();
			if (DepPackage.IsEmpty()) continue;

			// Skip /Script/ packages (C++ modules — not indexed assets)
			if (DepPackage.StartsWith(TEXT("/Script/"))) continue;

			TArray<FAssetData> AssetsInPackage;
			AR.GetAssetsByPackageName(DepName, AssetsInPackage);

			for (const FAssetData& DepAsset : AssetsInPackage)
			{
				FString DepPath = DepAsset.GetObjectPathString();
				int64 ToId = DB.GetAssetId(DepPath);
				if (ToId <= 0 || ToId == FromId) continue;

				FString TargetType = DB.GetAssetType(ToId);
				FString Rel = TEXT("REFERENCES");
				if (TargetType == TEXT("DataTable"))
				{
					Rel = TEXT("USES_DT");
				}
				else if (TargetType == TEXT("DataAsset"))
				{
					Rel = TEXT("USES_DA");
				}

				DB.UpsertEdge(FromId, ToId, Rel, TEXT(""), Hardness);
			}
		}
	};

	// Hard dependencies
	TArray<FName> HardDeps;
	UE::AssetRegistry::FDependencyQuery HardQuery;
	HardQuery.Required = UE::AssetRegistry::EDependencyProperty::Hard;
	AR.GetDependencies(PackageFName, HardDeps, UE::AssetRegistry::EDependencyCategory::Package, HardQuery);
	ProcessDeps(HardDeps, TEXT("Hard"));

	// Soft dependencies
	TArray<FName> SoftDeps;
	UE::AssetRegistry::FDependencyQuery SoftQuery;
	SoftQuery.Required = UE::AssetRegistry::EDependencyProperty::None;
	SoftQuery.Excluded = UE::AssetRegistry::EDependencyProperty::Hard;
	AR.GetDependencies(PackageFName, SoftDeps, UE::AssetRegistry::EDependencyCategory::Package, SoftQuery);
	ProcessDeps(SoftDeps, TEXT("Soft"));
}

void FAssetRelationBuilder::BuildVarTypeEdges(int64 FromId, FMCPDatabase& DB)
{
	TArray<FString> VarTypes = DB.GetMeta(FromId, TEXT("var_type"));
	for (const FString& VarType : VarTypes)
	{
		// Check if the var type matches an asset name in our DB
		FString Sql = FString::Printf(TEXT("SELECT id FROM assets WHERE name = '%s' LIMIT 1"), *VarType.Replace(TEXT("'"), TEXT("''")));
		int64 ToId = -1;
		DB.ExecuteWithCallback(Sql, [&](const FSQLitePreparedStatement& Row)
		{
			Row.GetColumnValueByIndex(0, ToId);
		});

		if (ToId > 0 && ToId != FromId)
		{
			DB.UpsertEdge(FromId, ToId, TEXT("HAS_VAR_OF_TYPE"));
		}
	}
}
