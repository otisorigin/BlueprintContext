#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"
#include "UObject/ObjectSaveContext.h"

class FBlueprintIndexBuilder;
class FBlueprintSearchEngine;
class FBlueprintHeaderGen;
class FBlueprintMCPServer;
class FMCPDatabase;
class FAssetRelationBuilder;
class FDataAssetIndexer;
class FAssetClassifier;

class FBlueprintMCPModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

	FBlueprintIndexBuilder* GetIndexBuilder() const { return IndexBuilder.Get(); }
	FBlueprintSearchEngine* GetSearchEngine() const { return SearchEngine.Get(); }
	FBlueprintHeaderGen* GetHeaderGen() const { return HeaderGen.Get(); }
	FMCPDatabase* GetDatabase() const { return Database.Get(); }

private:
	void OnAssetRegistryReady();
	void OnObjectPreSave(UObject* Object, FObjectPreSaveContext SaveContext);
	void RegisterConsoleCommands();

	TUniquePtr<FMCPDatabase> Database;
	TUniquePtr<FAssetClassifier> Classifier;
	TUniquePtr<FBlueprintIndexBuilder> IndexBuilder;
	TUniquePtr<FBlueprintSearchEngine> SearchEngine;
	TUniquePtr<FBlueprintHeaderGen> HeaderGen;
	TUniquePtr<FAssetRelationBuilder> RelationBuilder;
	TUniquePtr<FDataAssetIndexer> DataIndexer;
	TUniquePtr<FBlueprintMCPServer> MCPServer;

	TArray<IConsoleObject*> ConsoleCommands;
	FDelegateHandle OnObjectSavedHandle;
};
