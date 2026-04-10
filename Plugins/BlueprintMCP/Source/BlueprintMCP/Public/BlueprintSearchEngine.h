#pragma once

#include "CoreMinimal.h"

struct FBlueprintIndexEntry;

struct FSearchResult
{
	FString Name;
	FString Path;
	FString Parent;
	float Score;
	FString MatchReason;
};

struct FSearchFilter
{
	FString Parent;
	FString Folder;
	FString Interface;
	FString NameGlob;
};

class FBlueprintSearchEngine
{
public:
	TArray<FSearchResult> Search(const TArray<FBlueprintIndexEntry>& Entries, const FString& Query, int32 TopK, const FSearchFilter& Filter);

private:
	void Tokenize(const FString& Input, TArray<FString>& OutTokens);
	bool MatchField(const FString& Field, const FString& Token);
	bool MatchAny(const TArray<FString>& Fields, const FString& Token);
	FString BuildMatchReason(const FBlueprintIndexEntry& Entry, const TArray<FString>& Tokens);
	bool PassesFilter(const FBlueprintIndexEntry& Entry, const FSearchFilter& Filter);
	bool MatchGlob(const FString& Name, const FString& Pattern);

	static const TSet<FString>& GetStopWords();
};
