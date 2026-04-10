#pragma once

#include "CoreMinimal.h"

class FMCPDatabase;

class FAssetRelationBuilder
{
public:
	/** Build all edges for all assets in the database. Call AFTER all assets are indexed. */
	void BuildAllEdges(FMCPDatabase& DB);

	/** Build edges for a single asset (used during incremental re-indexation). */
	void BuildEdgesForAsset(int64 AssetId, const FString& Path, FMCPDatabase& DB);

private:
	void BuildInheritsEdge(int64 FromId, const FString& Parent, FMCPDatabase& DB);
	void BuildImplementsEdges(int64 FromId, FMCPDatabase& DB);
	void BuildReferenceEdges(int64 FromId, const FString& Path, FMCPDatabase& DB);
	void BuildVarTypeEdges(int64 FromId, FMCPDatabase& DB);
};
