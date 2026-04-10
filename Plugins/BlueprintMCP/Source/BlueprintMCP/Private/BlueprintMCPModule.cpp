#include "BlueprintMCPModule.h"
#include "MCPDatabase.h"
#include "AssetClassifier.h"
#include "BlueprintIndexBuilder.h"
#include "BlueprintSearchEngine.h"
#include "BlueprintHeaderGen.h"
#include "BlueprintMCPServer.h"
#include "AssetRelationBuilder.h"
#include "DataAssetIndexer.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/DataTable.h"
#include "Engine/DataAsset.h"
#include "Misc/Paths.h"

DEFINE_LOG_CATEGORY_STATIC(LogBlueprintMCP, Log, All);

#define LOCTEXT_NAMESPACE "FBlueprintMCPModule"

void FBlueprintMCPModule::StartupModule()
{
	// 1. Open database
	Database = MakeUnique<FMCPDatabase>();
	FString DbPath;
	if (!GConfig->GetString(TEXT("BlueprintMCP"), TEXT("DbPath"), DbPath, GEditorIni))
	{
		DbPath = FPaths::ProjectSavedDir() / TEXT("BlueprintMCP") / TEXT("blueprints.db");
	}

	if (!Database->Open(DbPath))
	{
		UE_LOG(LogBlueprintMCP, Error, TEXT("Failed to open database at %s"), *DbPath);
		return;
	}

	// 2. Create schema (idempotent)
	Database->CreateSchema();

	// 3. Create all subsystems
	Classifier = MakeUnique<FAssetClassifier>();
	IndexBuilder = MakeUnique<FBlueprintIndexBuilder>();
	SearchEngine = MakeUnique<FBlueprintSearchEngine>();
	HeaderGen = MakeUnique<FBlueprintHeaderGen>();
	RelationBuilder = MakeUnique<FAssetRelationBuilder>();
	DataIndexer = MakeUnique<FDataAssetIndexer>();
	MCPServer = MakeUnique<FBlueprintMCPServer>(this);
	MCPServer->SetDatabase(Database.Get());

	RegisterConsoleCommands();

	// 4. Wait for AssetRegistry before indexing
	FAssetRegistryModule& ARModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	IAssetRegistry& AR = ARModule.Get();

	if (AR.IsLoadingAssets())
	{
		AR.OnFilesLoaded().AddRaw(this, &FBlueprintMCPModule::OnAssetRegistryReady);
	}
	else
	{
		OnAssetRegistryReady();
	}
}

void FBlueprintMCPModule::OnAssetRegistryReady()
{
	// Unified indexing: classifier routes each asset to the right indexer
	Database->BeginTransaction();
	IndexBuilder->Build(*Database, *Classifier, *DataIndexer);
	Database->CommitTransaction();

	// Build relationship graph AFTER all assets are in DB
	RelationBuilder->BuildAllEdges(*Database);

	// Register save delegate for DataTable/DataAsset invalidation
	OnObjectSavedHandle = FCoreUObjectDelegates::OnObjectPreSave.AddRaw(this, &FBlueprintMCPModule::OnObjectPreSave);

	// Start HTTP server
	MCPServer->Start();

	const FIndexBuildStats& Stats = IndexBuilder->GetLastBuildStats();
	int32 EdgeCount = Database->GetEdgeCount();

	UE_LOG(LogBlueprintMCP, Log,
		TEXT("Ready. BP=%d DT=%d DA=%d Generic=%d ContentRef=%d (skipped: type=%d path=%d). %d edges. Server started."),
		Stats.CountBP, Stats.CountDT, Stats.CountDA, Stats.CountGeneric, Stats.CountContentRef,
		Stats.CountSkippedType, Stats.CountSkippedPath, EdgeCount);
}

void FBlueprintMCPModule::OnObjectPreSave(UObject* Object, FObjectPreSaveContext SaveContext)
{
	if (!Database || !Database->IsOpen()) return;

	if (UBlueprint* BP = Cast<UBlueprint>(Object))
	{
		FString Path = BP->GetPathName();
		IndexBuilder->RebuildEntry(Path, *Database);
		UE_LOG(LogBlueprintMCP, Log, TEXT("Re-indexed Blueprint %s"), *Path);
	}

	if (UDataTable* DT = Cast<UDataTable>(Object))
	{
		FString Path = DT->GetPathName();
		int64 AssetId = Database->GetAssetId(Path);
		if (AssetId > 0)
		{
			Database->ClearDTRows(AssetId);
			Database->UpdateRowCount(AssetId, DT->GetRowMap().Num());
			UE_LOG(LogBlueprintMCP, Verbose, TEXT("Invalidated dt_rows cache for %s"), *Path);
		}
	}

	if (UDataAsset* DA = Cast<UDataAsset>(Object))
	{
		FAssetData AssetData(DA);
		DataIndexer->IndexDataAsset(AssetData, *Database);
		UE_LOG(LogBlueprintMCP, Verbose, TEXT("Re-indexed DataAsset %s"), *DA->GetPathName());
	}
}

void FBlueprintMCPModule::ShutdownModule()
{
	// Remove delegates
	if (OnObjectSavedHandle.IsValid())
	{
		FCoreUObjectDelegates::OnObjectPreSave.Remove(OnObjectSavedHandle);
	}

	// Unregister console commands
	for (IConsoleObject* Cmd : ConsoleCommands)
	{
		IConsoleManager::Get().UnregisterConsoleObject(Cmd);
	}
	ConsoleCommands.Empty();

	// Shutdown in reverse order
	if (MCPServer) MCPServer->Stop();
	MCPServer.Reset();
	RelationBuilder.Reset();
	DataIndexer.Reset();
	HeaderGen.Reset();
	SearchEngine.Reset();
	IndexBuilder.Reset();
	Classifier.Reset();

	if (Database)
	{
		Database->Close();
		Database.Reset();
	}
}

void FBlueprintMCPModule::RegisterConsoleCommands()
{
	ConsoleCommands.Add(IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("BlueprintMCP.RebuildIndex"),
		TEXT("Rebuilds the Blueprint MCP index from scratch"),
		FConsoleCommandDelegate::CreateLambda([this]()
		{
			if (IndexBuilder && Database && Classifier && DataIndexer)
			{
				Database->BeginTransaction();
				IndexBuilder->Build(*Database, *Classifier, *DataIndexer);
				Database->CommitTransaction();
				RelationBuilder->BuildAllEdges(*Database);
				const FIndexBuildStats& S = IndexBuilder->GetLastBuildStats();
				UE_LOG(LogBlueprintMCP, Log, TEXT("Index rebuilt. BP=%d DT=%d DA=%d Generic=%d ContentRef=%d. %d edges."),
					S.CountBP, S.CountDT, S.CountDA, S.CountGeneric, S.CountContentRef, Database->GetEdgeCount());
			}
		}),
		ECVF_Default
	));

	ConsoleCommands.Add(IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("BlueprintMCP.PrintIndex"),
		TEXT("Prints the Blueprint MCP index summary"),
		FConsoleCommandDelegate::CreateLambda([this]()
		{
			if (IndexBuilder)
			{
				UE_LOG(LogBlueprintMCP, Log, TEXT("Index: %d blueprints, age %.1fs"),
					IndexBuilder->GetEntryCount(), IndexBuilder->GetIndexAgeSeconds());
				for (const FBlueprintIndexEntry& E : IndexBuilder->GetEntries())
				{
					UE_LOG(LogBlueprintMCP, Log, TEXT("  %s (%s) parent=%s vars=%d funcs=%d"),
						*E.Name, *E.Path, *E.Parent, E.VarNames.Num(), E.FuncNames.Num());
				}
			}
		}),
		ECVF_Default
	));

	ConsoleCommands.Add(IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("BlueprintMCP.Search"),
		TEXT("Search blueprints: BlueprintMCP.Search <query>"),
		FConsoleCommandWithArgsDelegate::CreateLambda([this](const TArray<FString>& Args)
		{
			if (Args.Num() == 0)
			{
				UE_LOG(LogBlueprintMCP, Warning, TEXT("Usage: BlueprintMCP.Search <query>"));
				return;
			}
			FString Query = FString::Join(Args, TEXT(" "));
			FSearchFilter Filter;
			TArray<FSearchResult> Results = SearchEngine->Search(IndexBuilder->GetEntries(), Query, 10, Filter);
			UE_LOG(LogBlueprintMCP, Log, TEXT("Search '%s': %d results"), *Query, Results.Num());
			for (const FSearchResult& R : Results)
			{
				UE_LOG(LogBlueprintMCP, Log, TEXT("  [%.2f] %s (%s) - %s"), R.Score, *R.Name, *R.Parent, *R.MatchReason);
			}
		}),
		ECVF_Default
	));

	ConsoleCommands.Add(IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("BlueprintMCP.DbInfo"),
		TEXT("Prints database statistics"),
		FConsoleCommandDelegate::CreateLambda([this]()
		{
			if (Database && Database->IsOpen())
			{
				UE_LOG(LogBlueprintMCP, Log, TEXT("DB Stats:"));
				UE_LOG(LogBlueprintMCP, Log, TEXT("  Total assets: %d"), Database->GetAssetCount());
				UE_LOG(LogBlueprintMCP, Log, TEXT("  Blueprints:   %d"), Database->GetAssetCountByType(TEXT("Blueprint")));
				UE_LOG(LogBlueprintMCP, Log, TEXT("  DataTables:   %d"), Database->GetAssetCountByType(TEXT("DataTable")));
				UE_LOG(LogBlueprintMCP, Log, TEXT("  DataAssets:   %d"), Database->GetAssetCountByType(TEXT("DataAsset")));
				UE_LOG(LogBlueprintMCP, Log, TEXT("  Generic:      %d"), Database->GetAssetCountByType(TEXT("Generic")));
				UE_LOG(LogBlueprintMCP, Log, TEXT("  Edges:        %d"), Database->GetEdgeCount());
			}
			else
			{
				UE_LOG(LogBlueprintMCP, Warning, TEXT("Database not open"));
			}
		}),
		ECVF_Default
	));

	ConsoleCommands.Add(IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("BlueprintMCP.PrintEdges"),
		TEXT("Print edges for an asset: BlueprintMCP.PrintEdges <path>"),
		FConsoleCommandWithArgsDelegate::CreateLambda([this](const TArray<FString>& Args)
		{
			if (Args.Num() == 0 || !Database)
			{
				UE_LOG(LogBlueprintMCP, Warning, TEXT("Usage: BlueprintMCP.PrintEdges <path>"));
				return;
			}
			FString Path = Args[0];
			int64 AssetId = Database->GetAssetId(Path);
			if (AssetId <= 0)
			{
				UE_LOG(LogBlueprintMCP, Warning, TEXT("Asset not found: %s"), *Path);
				return;
			}

			UE_LOG(LogBlueprintMCP, Log, TEXT("Outgoing edges for %s:"), *Path);
			TArray<FMCPRelatedResult> Out = Database->GetRelated(AssetId, TEXT("all"), false, false, 1, 100);
			for (const FMCPRelatedResult& R : Out)
			{
				UE_LOG(LogBlueprintMCP, Log, TEXT("  -> %s [%s] %s"), *R.Name, *R.Rel, *R.Path);
			}

			UE_LOG(LogBlueprintMCP, Log, TEXT("Incoming edges for %s:"), *Path);
			TArray<FMCPRelatedResult> In = Database->GetRelated(AssetId, TEXT("all"), true, false, 1, 100);
			for (const FMCPRelatedResult& R : In)
			{
				UE_LOG(LogBlueprintMCP, Log, TEXT("  <- %s [%s] %s"), *R.Name, *R.Rel, *R.Path);
			}
		}),
		ECVF_Default
	));

	ConsoleCommands.Add(IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("BlueprintMCP.RebuildEdges"),
		TEXT("Rebuilds all relationship edges"),
		FConsoleCommandDelegate::CreateLambda([this]()
		{
			if (RelationBuilder && Database)
			{
				RelationBuilder->BuildAllEdges(*Database);
				UE_LOG(LogBlueprintMCP, Log, TEXT("Edges rebuilt. Total: %d"), Database->GetEdgeCount());
			}
		}),
		ECVF_Default
	));

	ConsoleCommands.Add(IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("BlueprintMCP.IndexStats"),
		TEXT("Prints index classification stats. Add 'verbose' for Generic breakdown."),
		FConsoleCommandWithArgsDelegate::CreateLambda([this](const TArray<FString>& Args)
		{
			if (!IndexBuilder || !Database || !Database->IsOpen())
			{
				UE_LOG(LogBlueprintMCP, Warning, TEXT("Index not ready"));
				return;
			}

			const FIndexBuildStats& S = IndexBuilder->GetLastBuildStats();
			int32 InSQLite = Database->GetAssetCount();

			UE_LOG(LogBlueprintMCP, Log, TEXT("Index stats:"));
			UE_LOG(LogBlueprintMCP, Log, TEXT("  Total scanned:     %d"), S.TotalScanned);
			UE_LOG(LogBlueprintMCP, Log, TEXT("  Blueprint:         %d  (indexed)"), S.CountBP);
			UE_LOG(LogBlueprintMCP, Log, TEXT("  DataTable:         %d  (indexed)"), S.CountDT);
			UE_LOG(LogBlueprintMCP, Log, TEXT("  DataAsset:         %d  (indexed)"), S.CountDA);
			UE_LOG(LogBlueprintMCP, Log, TEXT("  Generic:           %d  (indexed, minimal metadata)"), S.CountGeneric);
			UE_LOG(LogBlueprintMCP, Log, TEXT("  ContentRef:        %d  (meshes/textures/audio/vfx — edge targets)"), S.CountContentRef);
			UE_LOG(LogBlueprintMCP, Log, TEXT("  Skipped (type):    %d  levels/fonts"), S.CountSkippedType);
			UE_LOG(LogBlueprintMCP, Log, TEXT("  Skipped (path):    %d  ExcludeFolders matches"), S.CountSkippedPath);
			UE_LOG(LogBlueprintMCP, Log, TEXT("  ─────────────────────────"));
			UE_LOG(LogBlueprintMCP, Log, TEXT("  In SQLite:         %d"), InSQLite);
			UE_LOG(LogBlueprintMCP, Log, TEXT("  Last rebuild:      %s"), *S.BuildTime.ToString());

			// Verbose: breakdown of Generic asset classes
			bool bVerbose = Args.Num() > 0 && Args[0].ToLower() == TEXT("verbose");
			if (bVerbose)
			{
				TMap<FString, int32> GenericClasses;
				TArray<FMCPAssetRecord> Generics = Database->QueryAssetsByType(TEXT("Generic"));
				for (const FMCPAssetRecord& G : Generics)
				{
					GenericClasses.FindOrAdd(G.Parent, 0)++;
				}

				// Sort by count descending
				GenericClasses.ValueSort([](int32 A, int32 B) { return A > B; });

				UE_LOG(LogBlueprintMCP, Log, TEXT("  Generic breakdown (top classes):"));
				int32 Shown = 0;
				for (const auto& Pair : GenericClasses)
				{
					UE_LOG(LogBlueprintMCP, Log, TEXT("    %-50s x %d"), *Pair.Key, Pair.Value);
					if (++Shown >= 10) break;
				}
			}
		}),
		ECVF_Default
	));
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FBlueprintMCPModule, BlueprintMCP)
