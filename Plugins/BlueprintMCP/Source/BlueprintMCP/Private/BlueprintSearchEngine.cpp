#include "BlueprintSearchEngine.h"
#include "BlueprintIndexBuilder.h"

const TSet<FString>& FBlueprintSearchEngine::GetStopWords()
{
	static TSet<FString> Words = {
		TEXT("a"), TEXT("an"), TEXT("the"), TEXT("is"), TEXT("are"), TEXT("was"), TEXT("were"),
		TEXT("be"), TEXT("been"), TEXT("being"), TEXT("have"), TEXT("has"), TEXT("had"),
		TEXT("do"), TEXT("does"), TEXT("did"), TEXT("will"), TEXT("would"), TEXT("could"),
		TEXT("should"), TEXT("may"), TEXT("might"), TEXT("can"), TEXT("shall"),
		TEXT("to"), TEXT("of"), TEXT("in"), TEXT("for"), TEXT("on"), TEXT("with"),
		TEXT("at"), TEXT("by"), TEXT("from"), TEXT("as"), TEXT("into"), TEXT("through"),
		TEXT("and"), TEXT("or"), TEXT("but"), TEXT("not"), TEXT("no"),
		TEXT("that"), TEXT("this"), TEXT("it"), TEXT("its"),
		TEXT("which"), TEXT("what"), TEXT("who"), TEXT("whom"),
		TEXT("all"), TEXT("each"), TEXT("every"), TEXT("both")
	};
	return Words;
}

void FBlueprintSearchEngine::Tokenize(const FString& Input, TArray<FString>& OutTokens)
{
	FString Lower = Input.ToLower();

	// Split by spaces and common separators
	TArray<FString> RawTokens;
	Lower.ParseIntoArray(RawTokens, TEXT(" "), true);

	const TSet<FString>& StopWords = GetStopWords();

	for (const FString& Token : RawTokens)
	{
		FString Clean = Token;
		// Remove punctuation
		Clean.RemoveFromStart(TEXT("\""));
		Clean.RemoveFromEnd(TEXT("\""));
		Clean.RemoveFromEnd(TEXT(","));
		Clean.RemoveFromEnd(TEXT("."));

		if (Clean.Len() > 1 && !StopWords.Contains(Clean))
		{
			OutTokens.Add(Clean);

			// Also split CamelCase tokens: "ApplyDamage" -> "apply", "damage"
			// This helps match query "damage" against func name "ApplyDamage"
		}
	}
}

bool FBlueprintSearchEngine::MatchField(const FString& Field, const FString& Token)
{
	return Field.ToLower().Contains(Token);
}

bool FBlueprintSearchEngine::MatchAny(const TArray<FString>& Fields, const FString& Token)
{
	for (const FString& F : Fields)
	{
		if (F.ToLower().Contains(Token))
		{
			return true;
		}
	}
	return false;
}

bool FBlueprintSearchEngine::MatchGlob(const FString& Name, const FString& Pattern)
{
	// Simple glob: only * supported
	if (Pattern.IsEmpty())
	{
		return true;
	}

	FString Pat = Pattern;
	FString Str = Name;

	// Case insensitive
	Pat = Pat.ToLower();
	Str = Str.ToLower();

	if (!Pat.Contains(TEXT("*")))
	{
		return Str.Contains(Pat);
	}

	// Split by * and check each part appears in order
	TArray<FString> Parts;
	Pat.ParseIntoArray(Parts, TEXT("*"), true);

	int32 SearchFrom = 0;
	for (int32 i = 0; i < Parts.Num(); ++i)
	{
		int32 Found = Str.Find(Parts[i], ESearchCase::CaseSensitive, ESearchDir::FromStart, SearchFrom);
		if (Found == INDEX_NONE)
		{
			return false;
		}
		// If first part, must be at start (unless pattern starts with *)
		if (i == 0 && !Pattern.StartsWith(TEXT("*")) && Found != 0)
		{
			return false;
		}
		SearchFrom = Found + Parts[i].Len();
	}

	// If pattern doesn't end with *, last part must be at end
	if (!Pattern.EndsWith(TEXT("*")) && SearchFrom != Str.Len())
	{
		return false;
	}

	return true;
}

bool FBlueprintSearchEngine::PassesFilter(const FBlueprintIndexEntry& Entry, const FSearchFilter& Filter)
{
	if (!Filter.Parent.IsEmpty() && !Entry.Parent.Contains(Filter.Parent))
	{
		return false;
	}

	if (!Filter.Folder.IsEmpty() && !Entry.Folder.Contains(Filter.Folder))
	{
		return false;
	}

	if (!Filter.Interface.IsEmpty())
	{
		bool bFound = false;
		for (const FString& Iface : Entry.Interfaces)
		{
			if (Iface.Contains(Filter.Interface))
			{
				bFound = true;
				break;
			}
		}
		if (!bFound)
		{
			return false;
		}
	}

	if (!Filter.NameGlob.IsEmpty() && !MatchGlob(Entry.Name, Filter.NameGlob))
	{
		return false;
	}

	return true;
}

FString FBlueprintSearchEngine::BuildMatchReason(const FBlueprintIndexEntry& Entry, const TArray<FString>& Tokens)
{
	TArray<FString> Reasons;

	TArray<FString> MatchedFuncs;
	TArray<FString> MatchedVars;
	TArray<FString> MatchedInterfaces;

	for (const FString& Token : Tokens)
	{
		if (MatchField(Entry.Name, Token))
		{
			// Name match is implicit
		}

		for (const FString& F : Entry.FuncNames)
		{
			if (F.ToLower().Contains(Token) && !MatchedFuncs.Contains(F))
			{
				MatchedFuncs.Add(F);
			}
		}

		for (const FString& V : Entry.VarNames)
		{
			if (V.ToLower().Contains(Token) && !MatchedVars.Contains(V))
			{
				MatchedVars.Add(V);
			}
		}

		for (const FString& I : Entry.Interfaces)
		{
			if (I.ToLower().Contains(Token) && !MatchedInterfaces.Contains(I))
			{
				MatchedInterfaces.Add(I);
			}
		}
	}

	if (MatchedFuncs.Num() > 0)
	{
		Reasons.Add(TEXT("func: ") + FString::Join(MatchedFuncs, TEXT(", ")));
	}
	if (MatchedVars.Num() > 0)
	{
		Reasons.Add(TEXT("var: ") + FString::Join(MatchedVars, TEXT(", ")));
	}
	if (MatchedInterfaces.Num() > 0)
	{
		Reasons.Add(TEXT("interface: ") + FString::Join(MatchedInterfaces, TEXT(", ")));
	}

	return FString::Join(Reasons, TEXT("; "));
}

TArray<FSearchResult> FBlueprintSearchEngine::Search(
	const TArray<FBlueprintIndexEntry>& Entries,
	const FString& Query,
	int32 TopK,
	const FSearchFilter& Filter)
{
	TopK = FMath::Clamp(TopK, 1, 20);

	TArray<FString> Tokens;
	Tokenize(Query, Tokens);

	if (Tokens.Num() == 0)
	{
		return {};
	}

	TArray<TPair<float, int32>> Scores;

	for (int32 i = 0; i < Entries.Num(); ++i)
	{
		const FBlueprintIndexEntry& E = Entries[i];

		if (!PassesFilter(E, Filter))
		{
			continue;
		}

		float Score = 0.f;
		for (const FString& T : Tokens)
		{
			if (MatchField(E.Name, T))        Score += 3.0f;
			if (MatchAny(E.FuncNames, T))      Score += 2.0f;
			if (MatchAny(E.VarNames, T))       Score += 2.0f;
			if (MatchAny(E.Categories, T))     Score += 1.5f;
			if (MatchField(E.Parent, T))       Score += 1.0f;
			for (const FString& Iface : E.Interfaces)
			{
				if (MatchField(Iface, T))
				{
					Score += 1.0f;
					break;
				}
			}
		}

		if (Score > 0.f)
		{
			Scores.Add({Score, i});
		}
	}

	// Sort descending
	Scores.Sort([](const TPair<float, int32>& A, const TPair<float, int32>& B)
	{
		return A.Key > B.Key;
	});

	// Bonus: +0.2 if parent matches one of the other top results (cluster bonus)
	if (Scores.Num() > 1)
	{
		TSet<FString> TopParents;
		for (int32 j = 0; j < FMath::Min(TopK, Scores.Num()); ++j)
		{
			TopParents.Add(Entries[Scores[j].Value].Parent);
		}
		for (auto& S : Scores)
		{
			if (TopParents.Contains(Entries[S.Value].Parent))
			{
				S.Key += 0.2f;
			}
		}
		// Re-sort after bonus
		Scores.Sort([](const TPair<float, int32>& A, const TPair<float, int32>& B)
		{
			return A.Key > B.Key;
		});
	}

	float MaxScore = Scores.Num() > 0 ? Scores[0].Key : 1.f;

	TArray<FSearchResult> Results;
	for (int32 j = 0; j < FMath::Min(TopK, Scores.Num()); ++j)
	{
		const FBlueprintIndexEntry& E = Entries[Scores[j].Value];

		FSearchResult R;
		R.Name = E.Name;
		R.Path = E.Path;
		R.Parent = E.Parent;
		R.Score = Scores[j].Key / MaxScore;
		R.MatchReason = BuildMatchReason(E, Tokens);
		Results.Add(R);
	}

	return Results;
}
