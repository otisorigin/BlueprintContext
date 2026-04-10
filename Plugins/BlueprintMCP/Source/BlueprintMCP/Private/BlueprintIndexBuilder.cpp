#include "BlueprintIndexBuilder.h"
#include "MCPDatabase.h"
#include "AssetClassifier.h"
#include "DataAssetIndexer.h"
#include "Containers/Ticker.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Editor.h"
#include "Misc/Paths.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

DEFINE_LOG_CATEGORY_STATIC(LogBPIndexBuilder, Log, All);

// --- FBlueprintIndexBuilder ---

FBlueprintIndexBuilder::FBlueprintIndexBuilder()
{
}

void FBlueprintIndexBuilder::RegisterDelegates()
{
	if (GEditor)
	{
		CompileDelegate = GEditor->OnBlueprintCompiled().AddRaw(this, &FBlueprintIndexBuilder::OnBlueprintCompiled);
	}
}

FBlueprintIndexBuilder::~FBlueprintIndexBuilder()
{
	if (CompileDelegate.IsValid() && GEditor)
	{
		GEditor->OnBlueprintCompiled().Remove(CompileDelegate);
	}
	if (RebuildTickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(RebuildTickerHandle);
	}
}

void FBlueprintIndexBuilder::Build(FMCPDatabase& DB, const FAssetClassifier& Classifier, FDataAssetIndexer& DataIndexer)
{
	DatabasePtr = &DB;
	ClassifierPtr = &Classifier;
	DataIndexerPtr = &DataIndexer;

	IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();

	// Fetch ALL assets from /Game/ in one query — classifier will sort them
	FARFilter Filter;
	Filter.PackagePaths.Add(TEXT("/Game"));
	Filter.bRecursivePaths = true;

	TArray<FAssetData> AllAssets;
	AR.GetAssets(Filter, AllAssets);

	UE_LOG(LogBPIndexBuilder, Log, TEXT("IndexBuilder: found %d assets in /Game/"), AllAssets.Num());

	// Reset stats
	LastBuildStats = FIndexBuildStats();
	LastBuildStats.TotalScanned = AllAssets.Num();

	for (const FAssetData& Asset : AllAssets)
	{
		// Level 1: path filter
		if (Classifier.IsExcludedByPath(Asset))
		{
			LastBuildStats.CountSkippedPath++;
			continue;
		}

		// Level 2: classify by type
		switch (Classifier.Classify(Asset))
		{
		case EAssetCategory::Blueprint:
			IndexAsset(Asset, DB);
			LastBuildStats.CountBP++;
			break;

		case EAssetCategory::DataTable:
			DataIndexer.IndexDataTable(Asset, DB);
			LastBuildStats.CountDT++;
			break;

		case EAssetCategory::DataAsset:
			DataIndexer.IndexDataAsset(Asset, DB);
			LastBuildStats.CountDA++;
			break;

		case EAssetCategory::Generic:
			IndexGenericAsset(Asset, DB);
			LastBuildStats.CountGeneric++;
			break;

		case EAssetCategory::ContentRef:
			IndexContentAsset(Asset, DB);
			LastBuildStats.CountContentRef++;
			break;

		case EAssetCategory::Skip:
			LastBuildStats.CountSkippedType++;
			break;
		}
	}

	IndexBuildTime = FDateTime::UtcNow();
	LastBuildStats.BuildTime = IndexBuildTime;

	// Populate in-memory arrays for search engine
	PopulateEntriesFromDB(DB);

	UE_LOG(LogBPIndexBuilder, Log,
		TEXT("IndexBuilder done: BP=%d DT=%d DA=%d Generic=%d ContentRef=%d SkippedType=%d SkippedPath=%d"),
		LastBuildStats.CountBP, LastBuildStats.CountDT, LastBuildStats.CountDA,
		LastBuildStats.CountGeneric, LastBuildStats.CountContentRef, LastBuildStats.CountSkippedType, LastBuildStats.CountSkippedPath);
}

void FBlueprintIndexBuilder::Build(FMCPDatabase& DB)
{
	// Legacy overload — only indexes blueprints (no classifier)
	DatabasePtr = &DB;

	IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();

	TArray<FAssetData> Assets;
	AR.GetAssetsByClass(UBlueprint::StaticClass()->GetClassPathName(), Assets, /*bSearchSubClasses=*/true);

	for (const FAssetData& Asset : Assets)
	{
		FString AssetPath = Asset.GetObjectPathString();
		if (AssetPath.Contains(TEXT("/Engine/")) || AssetPath.Contains(TEXT("/Plugins/")))
		{
			continue;
		}
		IndexAsset(Asset, DB);
	}

	IndexBuildTime = FDateTime::UtcNow();
	PopulateEntriesFromDB(DB);

	UE_LOG(LogBPIndexBuilder, Log, TEXT("Index built (legacy): %d blueprints"), Entries.Num());
}

void FBlueprintIndexBuilder::IndexAsset(const FAssetData& Asset, FMCPDatabase& DB)
{
	FMCPAssetRecord Record;
	Record.Name = Asset.AssetName.ToString();
	Record.Path = Asset.GetObjectPathString();
	Record.AssetType = TEXT("Blueprint");

	FString PackagePath = Asset.PackageName.ToString();
	Record.Folder = FPaths::GetPath(PackagePath);
	if (!Record.Folder.RemoveFromStart(TEXT("/Game/")))
	{
		// Root-level asset: PackagePath parent is exactly "/Game"
		Record.Folder = TEXT("");
	}

	// Parent class
	FString ParentClassPath;
	if (Asset.GetTagValue(FBlueprintTags::ParentClassPath, ParentClassPath))
	{
		Record.Parent = ExtractClassName(ParentClassPath);
	}

	Record.ModifiedAt = GetAssetTimestamp(Asset);

	// Header hash
	Record.HeaderHash = FString::Printf(TEXT("%08x"), GetTypeHash(Record.Name + Record.Parent));

	int64 AssetId = DB.UpsertAsset(Record);
	if (AssetId <= 0) return;

	// --- Build metadata ---
	TArray<TPair<FString, FString>> Meta;

	// Interfaces
	FString InterfacesRaw;
	if (Asset.GetTagValue(FBlueprintTags::ImplementedInterfaces, InterfacesRaw))
	{
		TArray<FString> Ifaces;
		ParseInterfaces(InterfacesRaw, Ifaces);
		for (const FString& Iface : Ifaces)
		{
			Meta.Emplace(TEXT("interface"), Iface);
		}
	}

	// Variables and functions from Blueprint object
	UBlueprint* BP = Cast<UBlueprint>(Asset.FastGetAsset(false));
	if (BP)
	{
		for (const FBPVariableDescription& Var : BP->NewVariables)
		{
			Meta.Emplace(TEXT("var"), Var.VarName.ToString());

			FString TypeStr = Var.VarType.PinCategory.ToString();
			if (Var.VarType.PinSubCategoryObject.IsValid())
			{
				TypeStr = Var.VarType.PinSubCategoryObject->GetName();
			}
			Meta.Emplace(TEXT("var_type"), TypeStr);

			if (!Var.Category.IsEmpty())
			{
				Meta.Emplace(TEXT("category"), Var.Category.ToString());
			}
		}

		// Clear and rebuild functions for this asset
		DB.ClearFunctions(AssetId);

		for (const UEdGraph* Graph : BP->FunctionGraphs)
		{
			if (Graph)
			{
				FString FuncName = Graph->GetName();
				if (!FuncName.StartsWith(TEXT("UserConstruction")) &&
					FuncName != TEXT("ReceiveBeginPlay") &&
					FuncName != TEXT("ReceiveTick"))
				{
					Meta.Emplace(TEXT("func"), FuncName);
					IndexFunctionSignature(AssetId, Graph, DB);
				}
			}
		}
	}

	DB.ClearMeta(AssetId);
	DB.SetMeta(AssetId, Meta);
}

void FBlueprintIndexBuilder::IndexGenericAsset(const FAssetData& Asset, FMCPDatabase& DB)
{
	FMCPAssetRecord Record;
	Record.Name = Asset.AssetName.ToString();
	Record.Path = Asset.GetObjectPathString();
	Record.AssetType = TEXT("Generic");

	FString PackagePath = Asset.PackageName.ToString();
	Record.Folder = FPaths::GetPath(PackagePath);
	if (!Record.Folder.RemoveFromStart(TEXT("/Game/")))
	{
		Record.Folder = TEXT("");
	}

	Record.ModifiedAt = GetAssetTimestamp(Asset);

	// Store the asset class path as parent — most useful metadata for generic assets
	Record.Parent = Asset.AssetClassPath.ToString();

	int64 AssetId = DB.UpsertAsset(Record);
	if (AssetId <= 0) return;

	// AssetRegistrySearchable tags — free, no LoadObject
	TArray<TPair<FString, FString>> Meta;
	for (const auto& TagPair : Asset.TagsAndValues)
	{
		Meta.Emplace(TEXT("tag"), TagPair.Key.ToString() + TEXT("=") + TagPair.Value.GetValue());
	}

	DB.ClearMeta(AssetId);
	DB.SetMeta(AssetId, Meta);
}

void FBlueprintIndexBuilder::IndexContentAsset(const FAssetData& Asset, FMCPDatabase& DB)
{
	FMCPAssetRecord Record;
	Record.Name = Asset.AssetName.ToString();
	Record.Path = Asset.GetObjectPathString();
	// Store the native class name as asset_type (e.g. "AnimMontage", "StaticMesh")
	Record.AssetType = Asset.AssetClassPath.GetAssetName().ToString();

	FString PackagePath = Asset.PackageName.ToString();
	Record.Folder = FPaths::GetPath(PackagePath);
	if (!Record.Folder.RemoveFromStart(TEXT("/Game/")))
	{
		Record.Folder = TEXT("");
	}

	Record.ModifiedAt = GetAssetTimestamp(Asset);

	// No parent, no meta, no LoadObject — just a lightweight target for edges
	DB.UpsertAsset(Record);
}

void FBlueprintIndexBuilder::IndexFunctionSignature(int64 AssetId, const UEdGraph* Graph, FMCPDatabase& DB)
{
	if (!Graph) return;

	FMCPFunctionRecord Func;
	Func.AssetId = AssetId;
	Func.Name = Graph->GetName();

	// Find entry node for inputs and result node for outputs
	TSharedPtr<FJsonObject> SigObj = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> Inputs;
	TArray<TSharedPtr<FJsonValue>> Outputs;
	TArray<FString> Flags;

	for (const UEdGraphNode* Node : Graph->Nodes)
	{
		if (const UK2Node_FunctionEntry* EntryNode = Cast<UK2Node_FunctionEntry>(Node))
		{
			// Extract input pins (skip exec pin)
			for (const UEdGraphPin* Pin : EntryNode->Pins)
			{
				if (Pin->Direction == EGPD_Output && Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
				{
					TSharedPtr<FJsonObject> Param = MakeShared<FJsonObject>();
					Param->SetStringField(TEXT("name"), Pin->PinName.ToString());
					Param->SetStringField(TEXT("type"), Pin->PinType.PinCategory.ToString());
					if (Pin->PinType.PinSubCategoryObject.IsValid())
					{
						Param->SetStringField(TEXT("type"), Pin->PinType.PinSubCategoryObject->GetName());
					}
					Inputs.Add(MakeShared<FJsonValueObject>(Param));
				}
			}

			// Extract function flags from metadata
			if (EntryNode->FindPin(UEdGraphSchema_K2::PN_Then))
			{
				// Has exec pin = not pure
			}
			else
			{
				Flags.Add(TEXT("BlueprintPure"));
			}
			Flags.Add(TEXT("BlueprintCallable"));
		}
		else if (const UK2Node_FunctionResult* ResultNode = Cast<UK2Node_FunctionResult>(Node))
		{
			for (const UEdGraphPin* Pin : ResultNode->Pins)
			{
				if (Pin->Direction == EGPD_Input && Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
				{
					TSharedPtr<FJsonObject> Param = MakeShared<FJsonObject>();
					Param->SetStringField(TEXT("name"), Pin->PinName.ToString());
					Param->SetStringField(TEXT("type"), Pin->PinType.PinCategory.ToString());
					if (Pin->PinType.PinSubCategoryObject.IsValid())
					{
						Param->SetStringField(TEXT("type"), Pin->PinType.PinSubCategoryObject->GetName());
					}
					Outputs.Add(MakeShared<FJsonValueObject>(Param));
				}
			}
		}
	}

	// Serialize signature to JSON
	SigObj->SetArrayField(TEXT("inputs"), Inputs);
	SigObj->SetArrayField(TEXT("outputs"), Outputs);

	FString SigStr;
	TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&SigStr);
	FJsonSerializer::Serialize(SigObj.ToSharedRef(), Writer);
	Func.SignatureJson = SigStr;

	// Serialize flags
	Func.FlagsJson = TEXT("[");
	for (int32 i = 0; i < Flags.Num(); ++i)
	{
		if (i > 0) Func.FlagsJson += TEXT(",");
		Func.FlagsJson += FString::Printf(TEXT("\"%s\""), *Flags[i]);
	}
	Func.FlagsJson += TEXT("]");

	DB.UpsertFunction(AssetId, Func);
}

void FBlueprintIndexBuilder::PopulateEntriesFromDB(FMCPDatabase& DB)
{
	Entries.Empty();
	PathToIndex.Empty();

	TArray<FMCPAssetRecord> Records = DB.QueryAssetsByType(TEXT("Blueprint"));

	for (const FMCPAssetRecord& Rec : Records)
	{
		FBlueprintIndexEntry Entry;
		Entry.Name = Rec.Name;
		Entry.Path = Rec.Path;
		Entry.Parent = Rec.Parent;
		Entry.Folder = Rec.Folder;
		Entry.HeaderHash = Rec.HeaderHash;
		Entry.ModifiedAt = FDateTime::FromUnixTimestamp(Rec.ModifiedAt);

		// Load meta arrays
		int64 AssetId = DB.GetAssetId(Rec.Path);
		if (AssetId > 0)
		{
			Entry.Interfaces = DB.GetMeta(AssetId, TEXT("interface"));
			Entry.VarNames = DB.GetMeta(AssetId, TEXT("var"));
			Entry.VarTypes = DB.GetMeta(AssetId, TEXT("var_type"));
			Entry.FuncNames = DB.GetMeta(AssetId, TEXT("func"));

			// Deduplicate categories
			TArray<FString> AllCats = DB.GetMeta(AssetId, TEXT("category"));
			for (const FString& Cat : AllCats)
			{
				Entry.Categories.AddUnique(Cat);
			}
		}

		int32 Idx = Entries.Add(MoveTemp(Entry));
		PathToIndex.Add(Rec.Path, Idx);
	}
}

void FBlueprintIndexBuilder::RebuildEntry(const FString& AssetPath, FMCPDatabase& DB)
{
	IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();
	FAssetData Asset = AR.GetAssetByObjectPath(FSoftObjectPath(AssetPath));

	if (!Asset.IsValid()) return;

	IndexAsset(Asset, DB);

	// Update the in-memory entry for the search engine
	FBlueprintIndexEntry NewEntry;
	PopulateEntryFromAsset(Asset, NewEntry);

	int32* ExistingIdx = PathToIndex.Find(AssetPath);
	if (ExistingIdx)
	{
		Entries[*ExistingIdx] = MoveTemp(NewEntry);
	}
	else
	{
		int32 Idx = Entries.Add(MoveTemp(NewEntry));
		PathToIndex.Add(AssetPath, Idx);
	}
}

void FBlueprintIndexBuilder::InvalidateHeader(const FString& AssetPath)
{
	FString HeaderFile = GetHeaderCacheDir() / FPaths::GetBaseFilename(AssetPath) + TEXT(".h");
	IFileManager::Get().Delete(*HeaderFile);
}

void FBlueprintIndexBuilder::OnBlueprintCompiled()
{
	// Debounce: ignore if a rebuild is already scheduled or running
	if (bIsBuilding || bRebuildPending) return;

	bRebuildPending = true;

	// Schedule one rebuild 3 seconds after the last compile event.
	// This coalesces bursts of compile events (e.g. opening an asset editor)
	// into a single background rebuild instead of an infinite loop.
	RebuildTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateRaw(this, &FBlueprintIndexBuilder::OnRebuildTick), 3.0f);
}

bool FBlueprintIndexBuilder::OnRebuildTick(float /*DeltaTime*/)
{
	bRebuildPending = false;

	if (!bIsBuilding)
	{
		bIsBuilding = true;
		if (DatabasePtr && ClassifierPtr && DataIndexerPtr)
		{
			Build(*DatabasePtr, *ClassifierPtr, *DataIndexerPtr);
		}
		else if (DatabasePtr)
		{
			Build(*DatabasePtr);
		}
		bIsBuilding = false;
		UE_LOG(LogBPIndexBuilder, Verbose, TEXT("Reindexed after blueprint compilation"));
	}

	return false; // one-shot: do not repeat
}

double FBlueprintIndexBuilder::GetIndexAgeSeconds() const
{
	return (FDateTime::UtcNow() - IndexBuildTime).GetTotalSeconds();
}

FString FBlueprintIndexBuilder::GetCacheDir() const
{
	FString Dir;
	if (!GConfig->GetString(TEXT("BlueprintMCP"), TEXT("CacheDir"), Dir, GEditorIni))
	{
		Dir = FPaths::ProjectSavedDir() / TEXT("BlueprintMCP");
	}
	return Dir;
}

FString FBlueprintIndexBuilder::GetHeaderCacheDir() const
{
	return GetCacheDir() / TEXT("headers");
}

void FBlueprintIndexBuilder::PopulateEntryFromAsset(const FAssetData& Asset, FBlueprintIndexEntry& Entry)
{
	Entry.Name = Asset.AssetName.ToString();
	Entry.Path = Asset.GetObjectPathString();

	FString PackagePath = Asset.PackageName.ToString();
	Entry.Folder = FPaths::GetPath(PackagePath);
	if (!Entry.Folder.RemoveFromStart(TEXT("/Game/")))
	{
		Entry.Folder = TEXT("");
	}

	FString ParentClassPath;
	if (Asset.GetTagValue(FBlueprintTags::ParentClassPath, ParentClassPath))
	{
		Entry.Parent = ExtractClassName(ParentClassPath);
	}

	FString InterfacesRaw;
	if (Asset.GetTagValue(FBlueprintTags::ImplementedInterfaces, InterfacesRaw))
	{
		ParseInterfaces(InterfacesRaw, Entry.Interfaces);
	}

	const FString AssetFilePath = FPackageName::LongPackageNameToFilename(PackagePath, FPackageName::GetAssetPackageExtension());
	FDateTime FileTime = IFileManager::Get().GetTimeStamp(*AssetFilePath);
	Entry.ModifiedAt = (FileTime != FDateTime::MinValue()) ? FileTime : FDateTime::UtcNow();

	LoadVarFuncNames(Asset, Entry);

	Entry.HeaderHash = FString::Printf(TEXT("%08x"), GetTypeHash(Entry.Name + Entry.Parent + FString::FromInt(Entry.VarNames.Num())));
}

void FBlueprintIndexBuilder::LoadVarFuncNames(const FAssetData& Asset, FBlueprintIndexEntry& Entry)
{
	UBlueprint* BP = Cast<UBlueprint>(Asset.FastGetAsset(false));
	if (!BP) return;

	for (const FBPVariableDescription& Var : BP->NewVariables)
	{
		Entry.VarNames.Add(Var.VarName.ToString());

		FString TypeStr = Var.VarType.PinCategory.ToString();
		if (Var.VarType.PinSubCategoryObject.IsValid())
		{
			TypeStr = Var.VarType.PinSubCategoryObject->GetName();
		}
		Entry.VarTypes.Add(TypeStr);

		if (!Var.Category.IsEmpty())
		{
			FString Cat = Var.Category.ToString();
			if (!Entry.Categories.Contains(Cat))
			{
				Entry.Categories.Add(Cat);
			}
		}
	}

	for (const UEdGraph* Graph : BP->FunctionGraphs)
	{
		if (Graph)
		{
			FString FuncName = Graph->GetName();
			if (!FuncName.StartsWith(TEXT("UserConstruction")) &&
				FuncName != TEXT("ReceiveBeginPlay") &&
				FuncName != TEXT("ReceiveTick"))
			{
				Entry.FuncNames.Add(FuncName);
			}
		}
	}
}

void FBlueprintIndexBuilder::ParseInterfaces(const FString& InterfacesRaw, TArray<FString>& OutInterfaces)
{
	TArray<FString> Parts;
	InterfacesRaw.ParseIntoArray(Parts, TEXT(","), true);
	for (FString& Part : Parts)
	{
		Part.TrimStartAndEndInline();
		Part.RemoveFromStart(TEXT("("));
		Part.RemoveFromEnd(TEXT(")"));

		FString ClassName = ExtractClassName(Part);
		if (!ClassName.IsEmpty())
		{
			OutInterfaces.Add(ClassName);
		}
	}
}

FString FBlueprintIndexBuilder::ExtractClassName(const FString& ClassPath)
{
	FString Result = ClassPath;

	int32 DotIdx;
	if (Result.FindLastChar('.', DotIdx))
	{
		Result = Result.Mid(DotIdx + 1);
	}

	Result.RemoveFromEnd(TEXT("'"));

	if (Result.EndsWith(TEXT("_C")))
	{
		Result = Result.LeftChop(2);
	}

	if (Result.StartsWith(TEXT("Class'")))
	{
		Result = Result.Mid(6);
	}

	return Result;
}

int64 FBlueprintIndexBuilder::GetAssetTimestamp(const FAssetData& Asset)
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
