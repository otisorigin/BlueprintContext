#pragma once

#include "CoreMinimal.h"

struct FBlueprintVarInfo
{
	FString Name;
	FString Type;
	FString DefaultValue;
	FString Category;
	TArray<FString> Flags;

	TSharedPtr<FJsonObject> ToJson() const;
};

struct FBlueprintFuncInfo
{
	FString Name;
	FString Category;
	TArray<FString> Flags;
	TArray<TPair<FString, FString>> Inputs;
	TArray<TPair<FString, FString>> Outputs;

	TSharedPtr<FJsonObject> ToJson() const;
};

struct FBlueprintHeaderData
{
	FString Path;
	FString HeaderText;
	TArray<FBlueprintVarInfo> Variables;
	TArray<FBlueprintFuncInfo> Functions;
	int32 TokensEst;
	bool bCached;
};

class FBlueprintHeaderGen
{
public:
	FBlueprintHeaderGen();

	FBlueprintHeaderData Generate(const FString& AssetPath);
	TArray<FBlueprintVarInfo> GenerateVars(const FString& AssetPath);
	TArray<FBlueprintFuncInfo> GenerateFuncs(const FString& AssetPath);

	void InvalidateCache(const FString& AssetPath);
	FString GetCacheDir() const;

	class UBlueprint* LoadBlueprint(const FString& AssetPath);

private:
	FString GenerateHeaderText(UBlueprint* BP, const TArray<FBlueprintVarInfo>& Vars, const TArray<FBlueprintFuncInfo>& Funcs);
	FString PropertyTypeToString(const class FProperty* Prop);
	FString GetPropertyFlags(const FProperty* Prop);
	int32 EstimateTokens(const FString& Text);

	TMap<FString, FBlueprintHeaderData> Cache;
};
