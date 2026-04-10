#include "DataAssetIndexer.h"
#include "MCPDatabase.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/DataTable.h"
#include "Engine/DataAsset.h"
#include "Misc/Paths.h"
#include "UObject/FieldIterator.h"

DEFINE_LOG_CATEGORY_STATIC(LogMCPDataIndexer, Log, All);

void FDataAssetIndexer::IndexAllDataTables(FMCPDatabase& DB)
{
	bool bIndex = true;
	GConfig->GetBool(TEXT("BlueprintMCP"), TEXT("IndexDataTables"), bIndex, GEditorIni);
	if (!bIndex) return;

	IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();

	TArray<FAssetData> Assets;
	AR.GetAssetsByClass(UDataTable::StaticClass()->GetClassPathName(), Assets, /*bSearchSubClasses=*/true);

	int32 Count = 0;
	for (const FAssetData& Asset : Assets)
	{
		if (ShouldExcludePath(Asset.GetObjectPathString())) continue;
		IndexDataTable(Asset, DB);
		++Count;
	}

	UE_LOG(LogMCPDataIndexer, Log, TEXT("Indexed %d DataTables"), Count);
}

void FDataAssetIndexer::IndexAllDataAssets(FMCPDatabase& DB)
{
	bool bIndex = true;
	GConfig->GetBool(TEXT("BlueprintMCP"), TEXT("IndexDataAssets"), bIndex, GEditorIni);
	if (!bIndex) return;

	IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();

	TArray<FAssetData> Assets;
	AR.GetAssetsByClass(UDataAsset::StaticClass()->GetClassPathName(), Assets, /*bSearchSubClasses=*/true);

	int32 Count = 0;
	for (const FAssetData& Asset : Assets)
	{
		if (ShouldExcludePath(Asset.GetObjectPathString())) continue;
		IndexDataAsset(Asset, DB);
		++Count;
	}

	UE_LOG(LogMCPDataIndexer, Log, TEXT("Indexed %d DataAssets"), Count);
}

void FDataAssetIndexer::IndexDataTable(const FAssetData& Asset, FMCPDatabase& DB)
{
	FMCPAssetRecord Record;
	Record.Name = Asset.AssetName.ToString();
	Record.Path = Asset.GetObjectPathString();
	Record.AssetType = TEXT("DataTable");

	FString PackagePath = Asset.PackageName.ToString();
	Record.Folder = FPaths::GetPath(PackagePath);
	Record.Folder.RemoveFromStart(TEXT("/Game/"));

	Record.ModifiedAt = GetAssetTimestamp(Asset);

	// Row struct and row count from AssetRegistry tags (no LoadObject needed)
	Asset.GetTagValue(TEXT("RowStructure"), Record.RowStruct);

	// UE5 DataTable exports tag "RowCount" (not "NumRows")
	FString RowCountStr;
	if (Asset.GetTagValue(TEXT("RowCount"), RowCountStr) && !RowCountStr.IsEmpty())
	{
		Record.RowCount = FCString::Atoi(*RowCountStr);
	}

	int64 AssetId = DB.UpsertAsset(Record);
	if (AssetId <= 0) return;

	// Column schema from the row struct via reflection
	TArray<TPair<FString, FString>> Meta;

	if (!Record.RowStruct.IsEmpty())
	{
		UScriptStruct* RowStruct = FindObject<UScriptStruct>(nullptr, *Record.RowStruct);
		if (!RowStruct)
		{
			// Try with /Script/ prefix variations
			RowStruct = FindObject<UScriptStruct>(nullptr, *FString::Printf(TEXT("/Script/Engine.%s"), *Record.RowStruct));
		}

		if (RowStruct)
		{
			for (TFieldIterator<FProperty> It(RowStruct); It; ++It)
			{
				Meta.Emplace(TEXT("dt_column"), It->GetName());
				Meta.Emplace(TEXT("dt_column_type"), It->GetCPPType());
			}
		}
	}

	DB.ClearMeta(AssetId);
	DB.SetMeta(AssetId, Meta);
}

void FDataAssetIndexer::IndexDataAsset(const FAssetData& Asset, FMCPDatabase& DB)
{
	FMCPAssetRecord Record;
	Record.Name = Asset.AssetName.ToString();
	Record.Path = Asset.GetObjectPathString();
	Record.AssetType = TEXT("DataAsset");

	FString PackagePath = Asset.PackageName.ToString();
	Record.Folder = FPaths::GetPath(PackagePath);
	Record.Folder.RemoveFromStart(TEXT("/Game/"));

	Record.ModifiedAt = GetAssetTimestamp(Asset);

	FString ClassPath;
	if (Asset.GetTagValue(TEXT("NativeClass"), ClassPath))
	{
		Record.DaClass = ClassPath;
	}

	int64 AssetId = DB.UpsertAsset(Record);
	if (AssetId <= 0) return;

	// Index fields — only AssetRegistrySearchable by default
	bool bLoadFull = false;
	GConfig->GetBool(TEXT("BlueprintMCP"), TEXT("IndexDataAssetLoadFull"), bLoadFull, GEditorIni);

	TArray<TPair<FString, FString>> Meta;

	UObject* Obj = Asset.FastGetAsset(false);
	if (UDataAsset* DA = Cast<UDataAsset>(Obj))
	{
		for (TFieldIterator<FProperty> It(DA->GetClass()); It; ++It)
		{
			FProperty* Prop = *It;

			// Skip if not marked searchable (unless full load is enabled)
			if (!bLoadFull && !Prop->HasMetaData(TEXT("AssetRegistrySearchable")))
			{
				continue;
			}

			FString ValueStr;
			const void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(DA);
			Prop->ExportTextItem_Direct(ValueStr, ValuePtr, nullptr, DA, PPF_None);

			Meta.Emplace(TEXT("da_field"), Prop->GetName());
			Meta.Emplace(TEXT("da_field_value"), ValueStr);
		}
	}

	DB.ClearMeta(AssetId);
	DB.SetMeta(AssetId, Meta);
}

int64 FDataAssetIndexer::GetAssetTimestamp(const FAssetData& Asset)
{
	FString PackagePath = Asset.PackageName.ToString();
	const FString AssetFilePath = FPackageName::LongPackageNameToFilename(PackagePath, FPackageName::GetAssetPackageExtension());
	FDateTime FileTime = IFileManager::Get().GetTimeStamp(*AssetFilePath);
	if (FileTime == FDateTime::MinValue())
	{
		FileTime = FDateTime::UtcNow();
	}
	return FileTime.ToUnixTimestamp();
}

bool FDataAssetIndexer::ShouldExcludePath(const FString& AssetPath) const
{
	return AssetPath.Contains(TEXT("/Engine/")) || AssetPath.Contains(TEXT("/Plugins/"));
}
