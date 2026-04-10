#include "BlueprintMCPServer.h"
#include "BlueprintMCPModule.h"
#include "BlueprintIndexBuilder.h"
#include "BlueprintSearchEngine.h"
#include "BlueprintHeaderGen.h"
#include "MCPDatabase.h"
#include "HttpServerModule.h"
#include "HttpServerResponse.h"
#include "IHttpRouter.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/Guid.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/DataTable.h"
#include "Engine/DataAsset.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
#include "UObject/FieldIterator.h"

// Helper to serialize JSON to string
static FString JsonToString(TSharedPtr<FJsonObject> Obj)
{
	FString Out;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
	FJsonSerializer::Serialize(Obj.ToSharedRef(), Writer);
	return Out;
}

// Helper to create JSON array from string array
static TArray<TSharedPtr<FJsonValue>> StringArrayToJson(const TArray<FString>& Arr)
{
	TArray<TSharedPtr<FJsonValue>> Result;
	for (const FString& S : Arr)
	{
		Result.Add(MakeShared<FJsonValueString>(S));
	}
	return Result;
}

FBlueprintMCPServer::FBlueprintMCPServer(FBlueprintMCPModule* InOwner)
	: Owner(InOwner)
	, Port(8765)
{
	// Read port from config
	int32 ConfigPort;
	if (GConfig->GetInt(TEXT("BlueprintMCP"), TEXT("Port"), ConfigPort, GEditorIni))
	{
		Port = ConfigPort;
	}
}

void FBlueprintMCPServer::OpenLogFile()
{
	FString LogDir = FPaths::ProjectSavedDir() / TEXT("BlueprintMCP");
	IFileManager::Get().MakeDirectory(*LogDir, true);
	LogFilePath = LogDir / TEXT("mcp_requests.log");

	// Write session header
	FString Header = FString::Printf(
		TEXT("\n========== BlueprintMCP Session started: %s ==========\n"),
		*FDateTime::Now().ToString(TEXT("%Y-%m-%d %H:%M:%S"))
	);
	FFileHelper::SaveStringToFile(Header, *LogFilePath,
		FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM,
		&IFileManager::Get(), FILEWRITE_Append);
}

void FBlueprintMCPServer::WriteToLog(const FString& Entry)
{
	if (LogFilePath.IsEmpty()) return;

	FString Line = FString::Printf(TEXT("[%s] %s\n"),
		*FDateTime::Now().ToString(TEXT("%Y-%m-%d %H:%M:%S")),
		*Entry);

	FFileHelper::SaveStringToFile(Line, *LogFilePath,
		FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM,
		&IFileManager::Get(), FILEWRITE_Append);
}

FBlueprintMCPServer::~FBlueprintMCPServer()
{
	Stop();
}

void FBlueprintMCPServer::GenerateToken()
{
	AuthToken = FGuid::NewGuid().ToString();

	FString TokenDir = FPaths::ProjectSavedDir() / TEXT("BlueprintMCP");
	IFileManager::Get().MakeDirectory(*TokenDir, true);

	FString TokenPath = TokenDir / TEXT("token.txt");
	FFileHelper::SaveStringToFile(AuthToken, *TokenPath);

	UE_LOG(LogTemp, Log, TEXT("[BlueprintMCP] Auth token written to: %s"), *TokenPath);
}

FString FBlueprintMCPServer::LoadToken()
{
	FString TokenPath = FPaths::ProjectSavedDir() / TEXT("BlueprintMCP") / TEXT("token.txt");
	FString Token;
	FFileHelper::LoadFileToString(Token, *TokenPath);
	return Token.TrimStartAndEnd();
}

void FBlueprintMCPServer::Start()
{
	GenerateToken();
	OpenLogFile();

	FHttpServerModule& HttpModule = FModuleManager::LoadModuleChecked<FHttpServerModule>("HTTPServer");
	Router = HttpModule.GetHttpRouter(Port);

	if (!Router)
	{
		UE_LOG(LogTemp, Error, TEXT("[BlueprintMCP] Failed to get HTTP router on port %d"), Port);
		return;
	}

	// Bind routes - using POST for tool calls, GET for discovery
	auto BindRoute = [this](const FString& Path, bool(FBlueprintMCPServer::*Handler)(const FHttpServerRequest&, const FHttpResultCallback&))
	{
		FHttpRouteHandle Handle = Router->BindRoute(
			FHttpPath(Path),
			EHttpServerRequestVerbs::VERB_POST | EHttpServerRequestVerbs::VERB_GET,
			FHttpRequestHandler::CreateLambda([this, Path, Handler](const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete) -> bool
			{
				LastRequestPath = Path;
				return (this->*Handler)(Request, OnComplete);
			})
		);
		RouteHandles.Add(Handle);
	};

	BindRoute(TEXT("/mcp/tools"),        &FBlueprintMCPServer::HandleToolsList);
	BindRoute(TEXT("/mcp/bp_search"),     &FBlueprintMCPServer::HandleSearch);
	BindRoute(TEXT("/mcp/bp_architecture"), &FBlueprintMCPServer::HandleArchitecture);
	BindRoute(TEXT("/mcp/bp_list"),       &FBlueprintMCPServer::HandleList);
	BindRoute(TEXT("/mcp/bp_hierarchy"), &FBlueprintMCPServer::HandleHierarchy);
	BindRoute(TEXT("/mcp/bp_header"),    &FBlueprintMCPServer::HandleHeader);
	BindRoute(TEXT("/mcp/bp_vars"),        &FBlueprintMCPServer::HandleVars);
	BindRoute(TEXT("/mcp/bp_parent_vars"),  &FBlueprintMCPServer::HandleParentVars);
	BindRoute(TEXT("/mcp/bp_components"),   &FBlueprintMCPServer::HandleComponents);
	BindRoute(TEXT("/mcp/bp_funcs"),     &FBlueprintMCPServer::HandleFuncs);
	BindRoute(TEXT("/mcp/bp_refs"),      &FBlueprintMCPServer::HandleRefs);
	BindRoute(TEXT("/mcp/asset_related"), &FBlueprintMCPServer::HandleAssetRelated);
	BindRoute(TEXT("/mcp/dt_list"),       &FBlueprintMCPServer::HandleDtList);
	BindRoute(TEXT("/mcp/dt_schema"),     &FBlueprintMCPServer::HandleDtSchema);
	BindRoute(TEXT("/mcp/dt_rows"),       &FBlueprintMCPServer::HandleDtRows);
	BindRoute(TEXT("/mcp/da_values"),     &FBlueprintMCPServer::HandleDaValues);
	BindRoute(TEXT("/mcp/asset_properties"), &FBlueprintMCPServer::HandleAssetProperties);

	HttpModule.StartAllListeners();

	UE_LOG(LogTemp, Log, TEXT("[BlueprintMCP] MCP Server started on port %d"), Port);
}

void FBlueprintMCPServer::Stop()
{
	if (Router)
	{
		for (const FHttpRouteHandle& Handle : RouteHandles)
		{
			Router->UnbindRoute(Handle);
		}
		RouteHandles.Empty();
		Router.Reset();
	}
}

bool FBlueprintMCPServer::CheckAuth(const FHttpServerRequest& Request, TUniquePtr<FHttpServerResponse>& OutResponse)
{
	// Find Authorization header
	FString AuthHeader;
	for (const auto& Header : Request.Headers)
	{
		if (Header.Key.Equals(TEXT("Authorization"), ESearchCase::IgnoreCase))
		{
			if (Header.Value.Num() > 0)
			{
				AuthHeader = Header.Value[0];
			}
			break;
		}
	}

	if (AuthHeader.IsEmpty())
	{
		OutResponse = MakeErrorResponse(TEXT("Missing Authorization header"), 401);
		return false;
	}

	FString Token = AuthHeader;
	Token.RemoveFromStart(TEXT("Bearer "));
	Token.TrimStartAndEndInline();

	if (Token != AuthToken)
	{
		OutResponse = MakeErrorResponse(TEXT("Invalid token"), 403);
		return false;
	}

	return true;
}

TSharedPtr<FJsonObject> FBlueprintMCPServer::ParseBody(const FHttpServerRequest& Request)
{
	if (Request.Body.Num() == 0)
	{
		WriteToLog(FString::Printf(TEXT("REQUEST  %s  (no body)"), *LastRequestPath));
		return MakeShared<FJsonObject>();
	}

	// Null-terminate before converting to avoid reading past buffer
	TArray<uint8> SafeBody = Request.Body;
	SafeBody.Add(0);
	FString BodyStr = FString(UTF8_TO_TCHAR(reinterpret_cast<const char*>(SafeBody.GetData())));

	WriteToLog(FString::Printf(TEXT("REQUEST  %s  %s"), *LastRequestPath, *BodyStr));

	TSharedPtr<FJsonObject> Obj;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(BodyStr);
	if (!FJsonSerializer::Deserialize(Reader, Obj))
	{
		WriteToLog(FString::Printf(TEXT("PARSE_ERROR  %s  JSON deserialization failed"), *LastRequestPath));
	}

	return Obj ? Obj : MakeShared<FJsonObject>();
}

TUniquePtr<FHttpServerResponse> FBlueprintMCPServer::MakeJsonResponse(TSharedPtr<FJsonObject> Result, int32 TokensEst, bool bTruncated)
{
	TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
	Response->SetStringField(TEXT("jsonrpc"), TEXT("2.0"));
	Response->SetNumberField(TEXT("id"), 1);
	Response->SetObjectField(TEXT("result"), Result);

	// Serialize first to measure actual size, then compute token estimate from real string length
	TSharedPtr<FJsonObject> Meta = MakeShared<FJsonObject>();
	Meta->SetNumberField(TEXT("tokens_est"), 0); // placeholder, updated below
	Meta->SetBoolField(TEXT("truncated"), bTruncated);
	Response->SetObjectField(TEXT("meta"), Meta);

	FString JsonStr = JsonToString(Response);
	int32 ActualTokensEst = FMath::Max(1, JsonStr.Len() / 4);
	Meta->SetNumberField(TEXT("tokens_est"), ActualTokensEst);
	JsonStr = JsonToString(Response); // re-serialize with corrected value

	WriteToLog(FString::Printf(TEXT("RESPONSE %s  tokens_est=%d truncated=%s  %s"),
		*LastRequestPath, ActualTokensEst, bTruncated ? TEXT("true") : TEXT("false"), *JsonStr));

	auto HttpResponse = FHttpServerResponse::Create(JsonStr, TEXT("application/json"));
	return HttpResponse;
}

TUniquePtr<FHttpServerResponse> FBlueprintMCPServer::MakeErrorResponse(const FString& Message, int32 Code)
{
	UE_LOG(LogTemp, Warning, TEXT("[BlueprintMCP] Request error (code %d): %s"), Code, *Message);

	TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
	Response->SetStringField(TEXT("jsonrpc"), TEXT("2.0"));
	Response->SetNumberField(TEXT("id"), 1);

	TSharedPtr<FJsonObject> Error = MakeShared<FJsonObject>();
	Error->SetNumberField(TEXT("code"), Code);
	Error->SetStringField(TEXT("message"), Message);
	Response->SetObjectField(TEXT("error"), Error);

	FString JsonStr = JsonToString(Response);

	WriteToLog(FString::Printf(TEXT("ERROR    %s  code=%d  %s"),
		*LastRequestPath, Code, *Message));

	return FHttpServerResponse::Create(JsonStr, TEXT("application/json"));
}

// ==================== ROUTE HANDLERS ====================

bool FBlueprintMCPServer::HandleToolsList(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	// No auth required for tool discovery
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

	TArray<TSharedPtr<FJsonValue>> Tools;

	auto AddTool = [&](const FString& Name, const FString& Desc)
	{
		TSharedPtr<FJsonObject> Tool = MakeShared<FJsonObject>();
		Tool->SetStringField(TEXT("name"), Name);
		Tool->SetStringField(TEXT("description"), Desc);
		Tools.Add(MakeShared<FJsonValueObject>(Tool));
	};

	AddTool(TEXT("bp_search"), TEXT("Semantic search across all project blueprints by query string"));
	AddTool(TEXT("bp_architecture"), TEXT("Aggregated project architecture map: class families, interfaces, folders"));
	AddTool(TEXT("bp_list"), TEXT("List blueprints with required filters (name_glob, parent, interface)"));
	AddTool(TEXT("bp_hierarchy"), TEXT("Inheritance tree for a specific blueprint class"));
	AddTool(TEXT("bp_header"), TEXT("Full C++-style header declaration for a blueprint"));
	AddTool(TEXT("bp_vars"), TEXT("Variables declared directly in this blueprint class (NewVariables only)"));
	AddTool(TEXT("bp_parent_vars"), TEXT("Editor-visible properties from parent project classes (C++ and Blueprint) up to first engine class. Shows current CDO value and flags overridden=true when this BP changed the default."));
	AddTool(TEXT("bp_components"), TEXT("Lists all components attached to a Blueprint: own (added in this BP) and inherited from parent Blueprint classes. Each entry includes component name, class, source (blueprint/native/engine), and inherited_from when from a parent BP."));
	AddTool(TEXT("bp_funcs"), TEXT("Functions only for a blueprint class"));
	AddTool(TEXT("bp_refs"), TEXT("Reverse references: children or interface implementors (via SQL graph)"));
	AddTool(TEXT("asset_related"), TEXT("Universal graph traversal: find related assets by relationship type, direction, with optional recursion"));
	AddTool(TEXT("dt_list"), TEXT("List DataTables with optional row_struct and folder filters"));
	AddTool(TEXT("dt_schema"), TEXT("Column names and types for a DataTable (no LoadObject)"));
	AddTool(TEXT("dt_rows"), TEXT("Fetch specific rows from a DataTable (lazy loaded, cached, max 20 default)"));
	AddTool(TEXT("da_values"), TEXT("All UPROPERTY field values for a DataAsset"));
	AddTool(TEXT("asset_properties"), TEXT("All UPROPERTY field values for ANY asset type (Blueprint, InputMappingContext, InputAction, etc). Use when da_values doesn't apply."));

	Result->SetArrayField(TEXT("tools"), Tools);

	OnComplete(MakeJsonResponse(Result, 200, false));
	return true;
}

bool FBlueprintMCPServer::HandleSearch(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	TUniquePtr<FHttpServerResponse> AuthErr;
	if (!CheckAuth(Request, AuthErr))
	{
		OnComplete(MoveTemp(AuthErr));
		return true;
	}

	TSharedPtr<FJsonObject> Body = ParseBody(Request);

	FString Query = Body->GetStringField(TEXT("query"));
	int32 TopK = 10;
	Body->TryGetNumberField(TEXT("top_k"), TopK);
	TopK = FMath::Clamp(TopK, 1, 20);

	if (Query.IsEmpty())
	{
		OnComplete(MakeErrorResponse(TEXT("'query' field is required"), -1));
		return true;
	}

	FSearchFilter Filter;
	const TSharedPtr<FJsonObject>* FilterObj;
	if (Body->TryGetObjectField(TEXT("filter"), FilterObj))
	{
		(*FilterObj)->TryGetStringField(TEXT("parent"), Filter.Parent);
		(*FilterObj)->TryGetStringField(TEXT("folder"), Filter.Folder);
		(*FilterObj)->TryGetStringField(TEXT("interface"), Filter.Interface);
		(*FilterObj)->TryGetStringField(TEXT("name_glob"), Filter.NameGlob);
	}

	const auto& Entries = Owner->GetIndexBuilder()->GetEntries();
	TArray<FSearchResult> Results = Owner->GetSearchEngine()->Search(Entries, Query, TopK, Filter);

	// Build response
	TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();

	TArray<TSharedPtr<FJsonValue>> ResultsArr;
	for (const FSearchResult& R : Results)
	{
		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("name"), R.Name);
		Item->SetStringField(TEXT("path"), R.Path);
		Item->SetStringField(TEXT("parent"), R.Parent);
		Item->SetNumberField(TEXT("score"), R.Score);
		Item->SetStringField(TEXT("match_reason"), R.MatchReason);
		ResultsArr.Add(MakeShared<FJsonValueObject>(Item));
	}
	ResultObj->SetArrayField(TEXT("results"), ResultsArr);

	TSharedPtr<FJsonObject> Meta = MakeShared<FJsonObject>();
	Meta->SetNumberField(TEXT("searched"), Entries.Num());
	Meta->SetNumberField(TEXT("returned"), Results.Num());
	Meta->SetNumberField(TEXT("index_age_sec"), Owner->GetIndexBuilder()->GetIndexAgeSeconds());
	ResultObj->SetObjectField(TEXT("meta"), Meta);

	int32 TokensEst = Results.Num() * 40;
	OnComplete(MakeJsonResponse(ResultObj, TokensEst, false));
	return true;
}

bool FBlueprintMCPServer::HandleArchitecture(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	TUniquePtr<FHttpServerResponse> AuthErr;
	if (!CheckAuth(Request, AuthErr))
	{
		OnComplete(MoveTemp(AuthErr));
		return true;
	}

	TSharedPtr<FJsonObject> Body = ParseBody(Request);

	FString FolderFilter;
	Body->TryGetStringField(TEXT("folder"), FolderFilter);
	int32 MinChildren = 2;
	Body->TryGetNumberField(TEXT("min_children"), MinChildren);
	FString Group;
	Body->TryGetStringField(TEXT("group"), Group);
	int32 Limit = 20;
	int32 Offset = 0;
	Body->TryGetNumberField(TEXT("limit"), Limit);
	Body->TryGetNumberField(TEXT("offset"), Offset);
	Limit = FMath::Clamp(Limit, 1, 100);

	const auto& Entries = Owner->GetIndexBuilder()->GetEntries();

	// Build family map: parent -> children
	TMap<FString, TArray<int32>> FamilyMap;
	TMap<FString, TSet<FString>> FamilyInterfaces;
	TMap<FString, int32> InterfaceCounts;
	TMap<FString, int32> FolderCounts;

	int32 TotalBlueprints = 0;

	for (int32 i = 0; i < Entries.Num(); ++i)
	{
		const FBlueprintIndexEntry& E = Entries[i];

		if (!FolderFilter.IsEmpty() && !E.Folder.Contains(FolderFilter))
		{
			continue;
		}

		TotalBlueprints++;

		// Group by root parent
		FamilyMap.FindOrAdd(E.Parent).Add(i);

		for (const FString& Iface : E.Interfaces)
		{
			FamilyInterfaces.FindOrAdd(E.Parent).Add(Iface);
			InterfaceCounts.FindOrAdd(Iface)++;
		}

		// Count by folder (first two levels)
		FString FolderKey = E.Folder;
		TArray<FString> FolderParts;
		FolderKey.ParseIntoArray(FolderParts, TEXT("/"), true);
		if (FolderParts.Num() >= 1)
		{
			FString TopFolder = FolderParts[0] + TEXT("/");
			if (FolderParts.Num() >= 2)
			{
				TopFolder = FolderParts[0] + TEXT("/") + FolderParts[1] + TEXT("/");
			}
			FolderCounts.FindOrAdd(TopFolder)++;
		}
	}

	// Sort families by count descending, apply min_children filter
	TArray<TPair<FString, TArray<int32>>> SortedFamilies;
	for (auto& Pair : FamilyMap)
	{
		if (Pair.Value.Num() >= MinChildren)
		{
			SortedFamilies.Add({Pair.Key, Pair.Value});
		}
	}
	SortedFamilies.Sort([](const auto& A, const auto& B) { return A.Value.Num() > B.Value.Num(); });

	// Sort interfaces by count descending
	TArray<TPair<FString, int32>> SortedInterfaces;
	for (auto& Pair : InterfaceCounts) SortedInterfaces.Add({Pair.Key, Pair.Value});
	SortedInterfaces.Sort([](const auto& A, const auto& B) { return A.Value > B.Value; });

	// Sort folders by count descending
	TArray<TPair<FString, int32>> SortedFolders;
	for (auto& Pair : FolderCounts) SortedFolders.Add({Pair.Key, Pair.Value});
	SortedFolders.Sort([](const auto& A, const auto& B) { return A.Value > B.Value; });

	int32 FamiliesTotal = SortedFamilies.Num();
	int32 InterfacesTotal = SortedInterfaces.Num();
	int32 FoldersTotal = SortedFolders.Num();

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	int32 TokensEst = 50;

	if (Group.IsEmpty())
	{
		// Summary only — no items, just counts
		Result->SetStringField(TEXT("summary"),
			FString::Printf(TEXT("%d blueprints, %d families, %d interfaces, %d folders"),
				TotalBlueprints, FamiliesTotal, InterfacesTotal, FoldersTotal));
		Result->SetNumberField(TEXT("total_blueprints"), TotalBlueprints);
		Result->SetNumberField(TEXT("families_count"), FamiliesTotal);
		Result->SetNumberField(TEXT("interfaces_count"), InterfacesTotal);
		Result->SetNumberField(TEXT("folders_count"), FoldersTotal);
		TokensEst = 80;
	}
	else if (Group == TEXT("families"))
	{
		int32 Start = FMath::Min(Offset, FamiliesTotal);
		int32 End = FMath::Min(Start + Limit, FamiliesTotal);

		TArray<TSharedPtr<FJsonValue>> FamiliesArr;
		for (int32 fi = Start; fi < End; ++fi)
		{
			const auto& Pair = SortedFamilies[fi];
			int32 Count = Pair.Value.Num();

			TSharedPtr<FJsonObject> Family = MakeShared<FJsonObject>();
			Family->SetStringField(TEXT("root"), Pair.Key);
			Family->SetNumberField(TEXT("count"), Count);

			// Top children (first 5)
			TArray<TSharedPtr<FJsonValue>> TopChildren;
			int32 ChildLimit = FMath::Min(5, Count);
			for (int32 j = 0; j < ChildLimit; ++j)
			{
				TopChildren.Add(MakeShared<FJsonValueString>(Entries[Pair.Value[j]].Name));
			}
			Family->SetArrayField(TEXT("top_children"), TopChildren);

			// Interfaces used by this family
			if (const TSet<FString>* Ifaces = FamilyInterfaces.Find(Pair.Key))
			{
				TArray<TSharedPtr<FJsonValue>> IfaceArr;
				for (const FString& I : *Ifaces)
				{
					IfaceArr.Add(MakeShared<FJsonValueString>(I));
				}
				Family->SetArrayField(TEXT("interfaces_used"), IfaceArr);
			}
			else
			{
				Family->SetArrayField(TEXT("interfaces_used"), {});
			}

			FamiliesArr.Add(MakeShared<FJsonValueObject>(Family));
		}

		Result->SetNumberField(TEXT("total"), FamiliesTotal);
		Result->SetNumberField(TEXT("returned"), FamiliesArr.Num());
		Result->SetBoolField(TEXT("has_more"), End < FamiliesTotal);
		Result->SetArrayField(TEXT("families"), FamiliesArr);
		TokensEst = FamiliesArr.Num() * 50 + 50;
	}
	else if (Group == TEXT("interfaces"))
	{
		int32 Start = FMath::Min(Offset, InterfacesTotal);
		int32 End = FMath::Min(Start + Limit, InterfacesTotal);

		TArray<TSharedPtr<FJsonValue>> InterfacesArr;
		for (int32 ii = Start; ii < End; ++ii)
		{
			const auto& Pair = SortedInterfaces[ii];
			TSharedPtr<FJsonObject> Iface = MakeShared<FJsonObject>();
			Iface->SetStringField(TEXT("name"), Pair.Key);
			Iface->SetNumberField(TEXT("implementors_count"), Pair.Value);
			InterfacesArr.Add(MakeShared<FJsonValueObject>(Iface));
		}

		Result->SetNumberField(TEXT("total"), InterfacesTotal);
		Result->SetNumberField(TEXT("returned"), InterfacesArr.Num());
		Result->SetBoolField(TEXT("has_more"), End < InterfacesTotal);
		Result->SetArrayField(TEXT("interfaces"), InterfacesArr);
		TokensEst = InterfacesArr.Num() * 20 + 50;
	}
	else if (Group == TEXT("folders"))
	{
		int32 Start = FMath::Min(Offset, FoldersTotal);
		int32 End = FMath::Min(Start + Limit, FoldersTotal);

		TArray<TSharedPtr<FJsonValue>> FoldersArr;
		for (int32 di = Start; di < End; ++di)
		{
			const auto& Pair = SortedFolders[di];
			TSharedPtr<FJsonObject> Folder = MakeShared<FJsonObject>();
			Folder->SetStringField(TEXT("path"), Pair.Key);
			Folder->SetNumberField(TEXT("count"), Pair.Value);
			FoldersArr.Add(MakeShared<FJsonValueObject>(Folder));
		}

		Result->SetNumberField(TEXT("total"), FoldersTotal);
		Result->SetNumberField(TEXT("returned"), FoldersArr.Num());
		Result->SetBoolField(TEXT("has_more"), End < FoldersTotal);
		Result->SetArrayField(TEXT("folders"), FoldersArr);
		TokensEst = FoldersArr.Num() * 15 + 50;
	}
	else
	{
		OnComplete(MakeErrorResponse(FString::Printf(TEXT("Unknown group: '%s'. Use 'families', 'interfaces', or 'folders'."), *Group), -1));
		return true;
	}

	bool bTruncated = Result->HasField(TEXT("has_more")) && Result->GetBoolField(TEXT("has_more"));
	OnComplete(MakeJsonResponse(Result, TokensEst, bTruncated));
	return true;
}

bool FBlueprintMCPServer::HandleList(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	TUniquePtr<FHttpServerResponse> AuthErr;
	if (!CheckAuth(Request, AuthErr))
	{
		OnComplete(MoveTemp(AuthErr));
		return true;
	}

	TSharedPtr<FJsonObject> Body = ParseBody(Request);

	// Parse filter — accept both nested {"filter": {...}} and flat params
	FSearchFilter Filter;
	bool bHasFilter = false;
	int32 Limit = 20;
	int32 Offset = 0;

	const TSharedPtr<FJsonObject>* FilterObj;
	TSharedPtr<FJsonObject> FilterSource;
	if (Body->TryGetObjectField(TEXT("filter"), FilterObj))
	{
		FilterSource = *FilterObj;
	}
	else
	{
		FilterSource = Body;
	}

	if (FilterSource->TryGetStringField(TEXT("name_glob"), Filter.NameGlob) && !Filter.NameGlob.IsEmpty()) bHasFilter = true;
	if (FilterSource->TryGetStringField(TEXT("parent"), Filter.Parent) && !Filter.Parent.IsEmpty()) bHasFilter = true;
	if (FilterSource->TryGetStringField(TEXT("interface"), Filter.Interface) && !Filter.Interface.IsEmpty()) bHasFilter = true;
	if (FilterSource->TryGetStringField(TEXT("folder"), Filter.Folder) && !Filter.Folder.IsEmpty()) bHasFilter = true;

	Body->TryGetNumberField(TEXT("limit"), Limit);
	Body->TryGetNumberField(TEXT("offset"), Offset);

	if (!bHasFilter && Limit > 100)
	{
		OnComplete(MakeErrorResponse(TEXT("bp_list requires at least one filter or explicit limit<=100. Use bp_search for discovery."), -1));
		return true;
	}

	Limit = FMath::Clamp(Limit, 1, 200);

	// Parse requested fields
	TSet<FString> Fields;
	const TArray<TSharedPtr<FJsonValue>>* FieldsArr;
	if (Body->TryGetArrayField(TEXT("fields"), FieldsArr))
	{
		for (const auto& V : *FieldsArr)
		{
			Fields.Add(V->AsString());
		}
	}
	bool bAllFields = Fields.Num() == 0;

	// Build the search engine to apply filters
	FBlueprintSearchEngine TempEngine;
	const auto& Entries = Owner->GetIndexBuilder()->GetEntries();

	TArray<TSharedPtr<FJsonValue>> Items;
	int32 Total = 0;

	for (const FBlueprintIndexEntry& E : Entries)
	{
		// Apply filters manually
		if (!Filter.NameGlob.IsEmpty() && !TempEngine.Search(Entries, E.Name, 1, Filter).Num()) // Use filter directly
		{
			// Actually let's do manual filtering here
		}

		bool bPass = true;
		if (!Filter.Parent.IsEmpty() && !E.Parent.Contains(Filter.Parent)) bPass = false;
		if (!Filter.Folder.IsEmpty() && !E.Folder.Contains(Filter.Folder)) bPass = false;
		if (!Filter.Interface.IsEmpty())
		{
			bool bFoundIface = false;
			for (const FString& I : E.Interfaces) { if (I.Contains(Filter.Interface)) { bFoundIface = true; break; } }
			if (!bFoundIface) bPass = false;
		}
		if (!Filter.NameGlob.IsEmpty())
		{
			// Simple glob match
			FString Pat = Filter.NameGlob.ToLower();
			FString Name = E.Name.ToLower();
			if (Pat.Contains(TEXT("*")))
			{
				FString Prefix = Pat.Replace(TEXT("*"), TEXT(""));
				if (!Name.Contains(Prefix)) bPass = false;
			}
			else
			{
				if (!Name.Contains(Pat)) bPass = false;
			}
		}

		if (!bPass) continue;

		Total++;

		if (Total <= Offset) continue;
		if (Items.Num() >= Limit) continue;

		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		if (bAllFields || Fields.Contains(TEXT("name")))   Item->SetStringField(TEXT("name"), E.Name);
		if (bAllFields || Fields.Contains(TEXT("path")))   Item->SetStringField(TEXT("path"), E.Path);
		if (bAllFields || Fields.Contains(TEXT("parent"))) Item->SetStringField(TEXT("parent"), E.Parent);
		if (bAllFields || Fields.Contains(TEXT("folder"))) Item->SetStringField(TEXT("folder"), E.Folder);
		if (bAllFields || Fields.Contains(TEXT("interfaces"))) Item->SetArrayField(TEXT("interfaces"), StringArrayToJson(E.Interfaces));
		if (bAllFields || Fields.Contains(TEXT("var_names")))  Item->SetArrayField(TEXT("var_names"), StringArrayToJson(E.VarNames));
		if (bAllFields || Fields.Contains(TEXT("func_names"))) Item->SetArrayField(TEXT("func_names"), StringArrayToJson(E.FuncNames));

		Items.Add(MakeShared<FJsonValueObject>(Item));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("total"), Total);
	Result->SetNumberField(TEXT("returned"), Items.Num());
	Result->SetArrayField(TEXT("items"), Items);

	TSharedPtr<FJsonObject> Meta = MakeShared<FJsonObject>();
	Meta->SetBoolField(TEXT("truncated"), Items.Num() < (Total - Offset));
	Result->SetObjectField(TEXT("meta"), Meta);

	int32 TokensEst = Items.Num() * 25;
	OnComplete(MakeJsonResponse(Result, TokensEst, Items.Num() < (Total - Offset)));
	return true;
}

bool FBlueprintMCPServer::HandleHierarchy(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	TUniquePtr<FHttpServerResponse> AuthErr;
	if (!CheckAuth(Request, AuthErr))
	{
		OnComplete(MoveTemp(AuthErr));
		return true;
	}

	TSharedPtr<FJsonObject> Body = ParseBody(Request);

	FString Path = Body->GetStringField(TEXT("path"));
	int32 DepthUp = 3;
	int32 DepthDown = 2;
	Body->TryGetNumberField(TEXT("depth_up"), DepthUp);
	Body->TryGetNumberField(TEXT("depth_down"), DepthDown);
	DepthUp = FMath::Clamp(DepthUp, 0, 5);
	DepthDown = FMath::Clamp(DepthDown, 0, 5);

	if (Path.IsEmpty())
	{
		OnComplete(MakeErrorResponse(TEXT("'path' field is required"), -1));
		return true;
	}

	const auto& Entries = Owner->GetIndexBuilder()->GetEntries();

	// Find the target entry
	const FBlueprintIndexEntry* Target = nullptr;
	for (const auto& E : Entries)
	{
		if (E.Path == Path || E.Name == Path)
		{
			Target = &E;
			break;
		}
	}

	if (!Target)
	{
		OnComplete(MakeErrorResponse(FString::Printf(TEXT("Blueprint not found: %s"), *Path), -1));
		return true;
	}

	// Build ancestors chain (from index - look up parent names)
	TArray<TSharedPtr<FJsonValue>> Ancestors;
	FString CurrentParent = Target->Parent;
	TSet<FString> Visited;
	int32 Depth = 0;

	while (!CurrentParent.IsEmpty() && Depth < DepthUp && !Visited.Contains(CurrentParent))
	{
		Visited.Add(CurrentParent);

		TSharedPtr<FJsonObject> AncObj = MakeShared<FJsonObject>();
		AncObj->SetStringField(TEXT("name"), CurrentParent);

		// Check if it's a BP or engine class
		bool bFoundInIndex = false;
		for (const auto& E : Entries)
		{
			if (E.Name == CurrentParent)
			{
				AncObj->SetStringField(TEXT("source"), TEXT("blueprint"));
				AncObj->SetStringField(TEXT("path"), E.Path);
				CurrentParent = E.Parent;
				bFoundInIndex = true;
				break;
			}
		}
		if (!bFoundInIndex)
		{
			// Distinguish project native C++ classes from engine classes via UClass registry.
			// Project classes live under /Script/<ProjectName>.*, engine classes under /Script/Engine.* etc.
			FString SourceLabel = TEXT("engine");
			UClass* NativeClass = FindFirstObject<UClass>(*CurrentParent, EFindFirstObjectOptions::None);
			if (NativeClass)
			{
				FString ClassPath = NativeClass->GetPathName();
				FString ProjectPrefix = FString::Printf(TEXT("/Script/%s."), FApp::GetProjectName());
				if (ClassPath.StartsWith(ProjectPrefix))
				{
					SourceLabel = TEXT("native");
				}
			}
			AncObj->SetStringField(TEXT("source"), SourceLabel);
			CurrentParent.Empty(); // Stop traversing — C++ class has no further Blueprint ancestors
		}

		Ancestors.Add(MakeShared<FJsonValueObject>(AncObj));
		Depth++;
	}

	// Build children tree recursively
	TFunction<TSharedPtr<FJsonObject>(const FString&, int32)> BuildChildTree;
	BuildChildTree = [&](const FString& ParentName, int32 RemainingDepth) -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> Node = MakeShared<FJsonObject>();
		// Unused standalone - children are built inline
		return Node;
	};

	// Find direct children
	TArray<TSharedPtr<FJsonValue>> ChildrenArr;
	int32 SiblingsCount = 0;

	for (const auto& E : Entries)
	{
		if (E.Parent == Target->Name)
		{
			TSharedPtr<FJsonObject> ChildObj = MakeShared<FJsonObject>();
			ChildObj->SetStringField(TEXT("name"), E.Name);
			ChildObj->SetStringField(TEXT("path"), E.Path);

			// Find grandchildren if depth allows
			if (DepthDown > 1)
			{
				TArray<TSharedPtr<FJsonValue>> GrandChildren;
				for (const auto& GC : Entries)
				{
					if (GC.Parent == E.Name)
					{
						TSharedPtr<FJsonObject> GCObj = MakeShared<FJsonObject>();
						GCObj->SetStringField(TEXT("name"), GC.Name);
						GCObj->SetStringField(TEXT("path"), GC.Path);
						GrandChildren.Add(MakeShared<FJsonValueObject>(GCObj));
					}
				}
				if (GrandChildren.Num() > 0)
				{
					ChildObj->SetArrayField(TEXT("children"), GrandChildren);
				}
			}

			ChildrenArr.Add(MakeShared<FJsonValueObject>(ChildObj));
		}

		// Count siblings (same parent)
		if (E.Parent == Target->Parent && E.Name != Target->Name)
		{
			SiblingsCount++;
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("ancestors"), Ancestors);
	Result->SetArrayField(TEXT("children"), ChildrenArr);
	Result->SetArrayField(TEXT("interfaces"), StringArrayToJson(Target->Interfaces));
	Result->SetNumberField(TEXT("siblings_count"), SiblingsCount);

	int32 TokensEst = 150 + Ancestors.Num() * 30 + ChildrenArr.Num() * 40;
	OnComplete(MakeJsonResponse(Result, TokensEst, false));
	return true;
}

bool FBlueprintMCPServer::HandleHeader(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	TUniquePtr<FHttpServerResponse> AuthErr;
	if (!CheckAuth(Request, AuthErr))
	{
		OnComplete(MoveTemp(AuthErr));
		return true;
	}

	TSharedPtr<FJsonObject> Body = ParseBody(Request);
	FString Path = Body->GetStringField(TEXT("path"));

	if (Path.IsEmpty())
	{
		OnComplete(MakeErrorResponse(TEXT("'path' field is required"), -1));
		return true;
	}

	FBlueprintHeaderData Data = Owner->GetHeaderGen()->Generate(Path);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("path"), Data.Path);
	Result->SetStringField(TEXT("header"), Data.HeaderText);

	TSharedPtr<FJsonObject> Meta = MakeShared<FJsonObject>();
	Meta->SetNumberField(TEXT("var_count"), Data.Variables.Num());
	Meta->SetNumberField(TEXT("func_count"), Data.Functions.Num());
	Meta->SetBoolField(TEXT("cached"), Data.bCached);
	Meta->SetNumberField(TEXT("tokens_est"), Data.TokensEst);

	int32 LargeWarningTokens = 1500;
	GConfig->GetInt(TEXT("BlueprintMCP"), TEXT("LargeHeaderWarningTokens"), LargeWarningTokens, GEditorIni);

	if (Data.TokensEst > LargeWarningTokens)
	{
		Result->SetStringField(TEXT("warning"),
			FString::Printf(TEXT("Large blueprint (%d tokens). Consider bp_vars or bp_funcs first."), Data.TokensEst));
	}

	Result->SetObjectField(TEXT("meta"), Meta);

	OnComplete(MakeJsonResponse(Result, Data.TokensEst, false));
	return true;
}

bool FBlueprintMCPServer::HandleVars(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	TUniquePtr<FHttpServerResponse> AuthErr;
	if (!CheckAuth(Request, AuthErr))
	{
		OnComplete(MoveTemp(AuthErr));
		return true;
	}

	TSharedPtr<FJsonObject> Body = ParseBody(Request);
	FString Path = Body->GetStringField(TEXT("path"));

	if (Path.IsEmpty())
	{
		OnComplete(MakeErrorResponse(TEXT("'path' field is required"), -1));
		return true;
	}

	UBlueprint* BP = Owner->GetHeaderGen()->LoadBlueprint(Path);
	if (!BP)
	{
		OnComplete(MakeErrorResponse(FString::Printf(TEXT("Blueprint not found: %s"), *Path), -1));
		return true;
	}

	int32 Limit = 20;
	int32 Offset = 0;
	Body->TryGetNumberField(TEXT("limit"), Limit);
	Body->TryGetNumberField(TEXT("offset"), Offset);
	Limit = FMath::Clamp(Limit, 1, 100);

	TArray<FBlueprintVarInfo> Vars = Owner->GetHeaderGen()->GenerateVars(Path);

	int32 Total = Vars.Num();
	int32 Start = FMath::Min(Offset, Total);
	int32 End = FMath::Min(Start + Limit, Total);

	TArray<TSharedPtr<FJsonValue>> VarsArr;
	for (int32 i = Start; i < End; ++i)
	{
		VarsArr.Add(MakeShared<FJsonValueObject>(Vars[i].ToJson()));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("total"), Total);
	Result->SetNumberField(TEXT("returned"), VarsArr.Num());
	Result->SetBoolField(TEXT("has_more"), End < Total);
	Result->SetArrayField(TEXT("variables"), VarsArr);

	int32 TokensEst = VarsArr.Num() * 30;
	OnComplete(MakeJsonResponse(Result, TokensEst, End < Total));
	return true;
}

bool FBlueprintMCPServer::HandleParentVars(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	TUniquePtr<FHttpServerResponse> AuthErr;
	if (!CheckAuth(Request, AuthErr))
	{
		OnComplete(MoveTemp(AuthErr));
		return true;
	}

	TSharedPtr<FJsonObject> Body = ParseBody(Request);
	FString Path = Body->GetStringField(TEXT("path"));
	if (Path.IsEmpty())
	{
		OnComplete(MakeErrorResponse(TEXT("'path' field is required"), -1));
		return true;
	}

	int32 Limit = 20;
	int32 Offset = 0;
	bool bOnlyOverridden = false;
	FString CategoryFilter;
	Body->TryGetNumberField(TEXT("limit"), Limit);
	Body->TryGetNumberField(TEXT("offset"), Offset);
	Body->TryGetBoolField(TEXT("only_overridden"), bOnlyOverridden);
	Body->TryGetStringField(TEXT("category"), CategoryFilter);
	Limit = FMath::Clamp(Limit, 1, 100);

	UBlueprint* BP = Owner->GetHeaderGen()->LoadBlueprint(Path);
	if (!BP)
	{
		OnComplete(MakeErrorResponse(FString::Printf(TEXT("Blueprint not found: %s"), *Path), -1));
		return true;
	}

	UClass* BPClass = BP->GeneratedClass;
	if (!BPClass)
	{
		OnComplete(MakeErrorResponse(TEXT("Blueprint not compiled"), -1));
		return true;
	}

	UObject* BPCDO = BPClass->GetDefaultObject();
	FString ProjectPrefix = FString::Printf(TEXT("/Script/%s."), FApp::GetProjectName());

	TArray<TSharedPtr<FJsonValue>> ClassesArr;
	int32 Total = 0;
	int32 Returned = 0;

	UClass* SuperClass = BPClass->GetSuperClass();
	while (SuperClass)
	{
		FString ClassPath = SuperClass->GetPathName();

		// Determine source — stop at first engine class
		FString SourceLabel;
		bool bIsBlueprint = Cast<UBlueprintGeneratedClass>(SuperClass) != nullptr;
		if (bIsBlueprint)
		{
			SourceLabel = TEXT("blueprint");
		}
		else if (ClassPath.StartsWith(ProjectPrefix))
		{
			SourceLabel = TEXT("native");
		}
		else
		{
			break; // engine class — stop traversal
		}

		UObject* SuperCDO = SuperClass->GetDefaultObject();

		TArray<TSharedPtr<FJsonValue>> VarsArr;
		for (TFieldIterator<FProperty> It(SuperClass, EFieldIteratorFlags::ExcludeSuper); It; ++It)
		{
			FProperty* Prop = *It;

			// Only editor-visible / Blueprint-accessible properties
			if (!Prop->HasAnyPropertyFlags(CPF_Edit | CPF_BlueprintVisible)) continue;
			if (Prop->HasAnyPropertyFlags(CPF_Deprecated)) continue;

			FString BPValue, SuperValue;
			Prop->ExportTextItem_Direct(BPValue,    Prop->ContainerPtrToValuePtr<void>(BPCDO),    nullptr, nullptr, PPF_None);
			Prop->ExportTextItem_Direct(SuperValue, Prop->ContainerPtrToValuePtr<void>(SuperCDO), nullptr, nullptr, PPF_None);

			// Apply filters
			if (bOnlyOverridden && BPValue == SuperValue) continue;
			if (!CategoryFilter.IsEmpty())
			{
				const FString* PropCategory = Prop->FindMetaData(TEXT("Category"));
				if (!PropCategory || !PropCategory->Contains(CategoryFilter)) continue;
			}

			Total++;
			if (Total <= Offset) continue;
			if (Returned >= Limit) continue;

			TSharedPtr<FJsonObject> VarObj = MakeShared<FJsonObject>();
			VarObj->SetStringField(TEXT("name"),  Prop->GetName());
			VarObj->SetStringField(TEXT("type"),  Prop->GetCPPType());
			VarObj->SetStringField(TEXT("value"), BPValue);

			if (BPValue != SuperValue)
			{
				VarObj->SetBoolField(TEXT("overridden"), true);
				VarObj->SetStringField(TEXT("parent_value"), SuperValue);
			}

			VarsArr.Add(MakeShared<FJsonValueObject>(VarObj));
			Returned++;
		}

		if (VarsArr.Num() > 0)
		{
			TSharedPtr<FJsonObject> ClassObj = MakeShared<FJsonObject>();
			ClassObj->SetStringField(TEXT("name"),   SuperClass->GetName());
			ClassObj->SetStringField(TEXT("source"), SourceLabel);
			if (bIsBlueprint)
			{
				FString AssetPath = ClassPath;
				AssetPath.RemoveFromEnd(TEXT("_C"));
				ClassObj->SetStringField(TEXT("path"), AssetPath);
			}
			ClassObj->SetArrayField(TEXT("variables"), VarsArr);
			ClassesArr.Add(MakeShared<FJsonValueObject>(ClassObj));
		}

		SuperClass = SuperClass->GetSuperClass();
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("path"), Path);
	Result->SetNumberField(TEXT("total"), Total);
	Result->SetNumberField(TEXT("returned"), Returned);
	Result->SetBoolField(TEXT("has_more"), Returned < (Total - Offset));
	Result->SetArrayField(TEXT("classes"), ClassesArr);

	int32 TokensEst = Returned * 40;
	OnComplete(MakeJsonResponse(Result, TokensEst, Returned < (Total - Offset)));
	return true;
}

bool FBlueprintMCPServer::HandleComponents(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	TUniquePtr<FHttpServerResponse> AuthErr;
	if (!CheckAuth(Request, AuthErr))
	{
		OnComplete(MoveTemp(AuthErr));
		return true;
	}

	TSharedPtr<FJsonObject> Body = ParseBody(Request);
	FString Path = Body->GetStringField(TEXT("path"));
	if (Path.IsEmpty())
	{
		OnComplete(MakeErrorResponse(TEXT("'path' field is required"), -1));
		return true;
	}

	int32 Limit = 20;
	int32 Offset = 0;
	Body->TryGetNumberField(TEXT("limit"), Limit);
	Body->TryGetNumberField(TEXT("offset"), Offset);
	Limit = FMath::Clamp(Limit, 1, 100);

	UBlueprint* BP = Owner->GetHeaderGen()->LoadBlueprint(Path);
	if (!BP)
	{
		OnComplete(MakeErrorResponse(FString::Printf(TEXT("Blueprint not found: %s"), *Path), -1));
		return true;
	}

	FString ProjectPrefix = FString::Printf(TEXT("/Script/%s."), FApp::GetProjectName());

	// Helper: classify a component class
	auto ClassifyComponent = [&](UClass* CompClass, TSharedPtr<FJsonObject>& Out)
	{
		if (!CompClass) return;
		FString CompClassPath = CompClass->GetPathName();
		bool bIsBlueprint = Cast<UBlueprintGeneratedClass>(CompClass) != nullptr;

		Out->SetStringField(TEXT("class"), CompClass->GetName());
		if (bIsBlueprint)
		{
			Out->SetStringField(TEXT("source"), TEXT("blueprint"));
			FString AssetPath = CompClassPath;
			AssetPath.RemoveFromEnd(TEXT("_C"));
			Out->SetStringField(TEXT("class_path"), AssetPath);
		}
		else if (CompClassPath.StartsWith(ProjectPrefix))
		{
			Out->SetStringField(TEXT("source"), TEXT("native"));
		}
		else
		{
			Out->SetStringField(TEXT("source"), TEXT("engine"));
		}
	};

	// Helper: collect SCS nodes from one Blueprint
	auto CollectFromBP = [&](UBlueprint* SourceBP, const FString& InheritedFrom, TArray<TSharedPtr<FJsonValue>>& Out)
	{
		if (!SourceBP->SimpleConstructionScript) return;
		for (USCS_Node* Node : SourceBP->SimpleConstructionScript->GetAllNodes())
		{
			if (!Node || !Node->ComponentClass) continue;
			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("name"), Node->GetVariableName().ToString());
			ClassifyComponent(Node->ComponentClass, Obj);
			if (!InheritedFrom.IsEmpty())
			{
				Obj->SetStringField(TEXT("inherited_from"), InheritedFrom);
			}
			Out.Add(MakeShared<FJsonValueObject>(Obj));
		}
	};

	TArray<TSharedPtr<FJsonValue>> AllComponents;

	// Own components (added in this BP)
	CollectFromBP(BP, TEXT(""), AllComponents);

	// Inherited components from parent Blueprint classes
	UClass* SuperClass = BP->GeneratedClass ? BP->GeneratedClass->GetSuperClass() : nullptr;
	while (SuperClass)
	{
		UBlueprintGeneratedClass* SuperBPClass = Cast<UBlueprintGeneratedClass>(SuperClass);
		if (!SuperBPClass) break;

		UBlueprint* ParentBP = Cast<UBlueprint>(SuperBPClass->ClassGeneratedBy);
		if (ParentBP)
		{
			CollectFromBP(ParentBP, ParentBP->GetName(), AllComponents);
		}
		SuperClass = SuperClass->GetSuperClass();
	}

	int32 Total = AllComponents.Num();
	int32 Start = FMath::Min(Offset, Total);
	int32 End = FMath::Min(Start + Limit, Total);

	TArray<TSharedPtr<FJsonValue>> ComponentsArr;
	for (int32 i = Start; i < End; ++i)
	{
		ComponentsArr.Add(AllComponents[i]);
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("path"), Path);
	Result->SetNumberField(TEXT("total"), Total);
	Result->SetNumberField(TEXT("returned"), ComponentsArr.Num());
	Result->SetBoolField(TEXT("has_more"), End < Total);
	Result->SetArrayField(TEXT("components"), ComponentsArr);

	int32 TokensEst = ComponentsArr.Num() * 30;
	OnComplete(MakeJsonResponse(Result, TokensEst, End < Total));
	return true;
}

bool FBlueprintMCPServer::HandleFuncs(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	TUniquePtr<FHttpServerResponse> AuthErr;
	if (!CheckAuth(Request, AuthErr))
	{
		OnComplete(MoveTemp(AuthErr));
		return true;
	}

	TSharedPtr<FJsonObject> Body = ParseBody(Request);
	FString Path = Body->GetStringField(TEXT("path"));

	if (Path.IsEmpty())
	{
		OnComplete(MakeErrorResponse(TEXT("'path' field is required"), -1));
		return true;
	}

	UBlueprint* BP = Owner->GetHeaderGen()->LoadBlueprint(Path);
	if (!BP)
	{
		OnComplete(MakeErrorResponse(FString::Printf(TEXT("Blueprint not found: %s"), *Path), -1));
		return true;
	}

	int32 Limit = 20;
	int32 Offset = 0;
	Body->TryGetNumberField(TEXT("limit"), Limit);
	Body->TryGetNumberField(TEXT("offset"), Offset);
	Limit = FMath::Clamp(Limit, 1, 100);

	TArray<FBlueprintFuncInfo> Funcs = Owner->GetHeaderGen()->GenerateFuncs(Path);

	int32 Total = Funcs.Num();
	int32 Start = FMath::Min(Offset, Total);
	int32 End = FMath::Min(Start + Limit, Total);

	TArray<TSharedPtr<FJsonValue>> FuncsArr;
	for (int32 i = Start; i < End; ++i)
	{
		FuncsArr.Add(MakeShared<FJsonValueObject>(Funcs[i].ToJson()));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("total"), Total);
	Result->SetNumberField(TEXT("returned"), FuncsArr.Num());
	Result->SetBoolField(TEXT("has_more"), End < Total);
	Result->SetArrayField(TEXT("functions"), FuncsArr);

	int32 TokensEst = FuncsArr.Num() * 40;
	OnComplete(MakeJsonResponse(Result, TokensEst, End < Total));
	return true;
}

bool FBlueprintMCPServer::HandleRefs(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	TUniquePtr<FHttpServerResponse> AuthErr;
	if (!CheckAuth(Request, AuthErr))
	{
		OnComplete(MoveTemp(AuthErr));
		return true;
	}

	TSharedPtr<FJsonObject> Body = ParseBody(Request);
	FString Path = Body->GetStringField(TEXT("path"));
	FString RefType = TEXT("child");
	Body->TryGetStringField(TEXT("ref_type"), RefType);
	bool bRecursive = false;
	Body->TryGetBoolField(TEXT("recursive"), bRecursive);
	int32 Limit = 50;
	Body->TryGetNumberField(TEXT("limit"), Limit);
	Limit = FMath::Clamp(Limit, 1, 50);

	if (Path.IsEmpty())
	{
		OnComplete(MakeErrorResponse(TEXT("'path' field is required"), -1));
		return true;
	}

	if (!Database)
	{
		OnComplete(MakeErrorResponse(TEXT("Database not initialized"), -1));
		return true;
	}

	int64 AssetId = Database->GetAssetId(Path);
	if (AssetId <= 0)
	{
		OnComplete(MakeErrorResponse(FString::Printf(TEXT("Asset not found: %s"), *Path), -1));
		return true;
	}

	// Map old ref_type to edge rel
	FString Rel;
	if (RefType == TEXT("child")) Rel = TEXT("INHERITS");
	else if (RefType == TEXT("interface_impl")) Rel = TEXT("IMPLEMENTS");
	else Rel = RefType; // Allow new rel types directly

	TArray<FMCPRelatedResult> Results = Database->GetRelated(AssetId, Rel, /*bIncoming=*/true, bRecursive, 10, Limit);

	TArray<TSharedPtr<FJsonValue>> RefsArr;
	for (const FMCPRelatedResult& R : Results)
	{
		TSharedPtr<FJsonObject> Ref = MakeShared<FJsonObject>();
		Ref->SetStringField(TEXT("name"), R.Name);
		Ref->SetStringField(TEXT("path"), R.Path);
		Ref->SetStringField(TEXT("rel"), R.Rel);
		Ref->SetNumberField(TEXT("depth"), R.Depth);
		RefsArr.Add(MakeShared<FJsonValueObject>(Ref));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("ref_type"), RefType);
	Result->SetStringField(TEXT("path"), Path);
	Result->SetNumberField(TEXT("total"), RefsArr.Num());
	Result->SetArrayField(TEXT("refs"), RefsArr);

	int32 TokensEst = RefsArr.Num() * 40;
	OnComplete(MakeJsonResponse(Result, TokensEst, false));
	return true;
}

// ==================== NEW V2 HANDLERS ====================

bool FBlueprintMCPServer::HandleAssetRelated(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	TUniquePtr<FHttpServerResponse> AuthErr;
	if (!CheckAuth(Request, AuthErr))
	{
		OnComplete(MoveTemp(AuthErr));
		return true;
	}

	TSharedPtr<FJsonObject> Body = ParseBody(Request);
	FString Path = Body->GetStringField(TEXT("path"));
	FString Rel = TEXT("all");
	Body->TryGetStringField(TEXT("rel"), Rel);
	FString Direction = TEXT("in");
	Body->TryGetStringField(TEXT("direction"), Direction);
	bool bRecursive = false;
	Body->TryGetBoolField(TEXT("recursive"), bRecursive);
	int32 Limit = 50;
	Body->TryGetNumberField(TEXT("limit"), Limit);
	Limit = FMath::Clamp(Limit, 1, 50);
	int32 Offset = 0;
	Body->TryGetNumberField(TEXT("offset"), Offset);
	Offset = FMath::Max(0, Offset);

	if (Path.IsEmpty())
	{
		OnComplete(MakeErrorResponse(TEXT("'path' field is required"), -1));
		return true;
	}
	if (!Database)
	{
		OnComplete(MakeErrorResponse(TEXT("Database not initialized"), -1));
		return true;
	}

	int64 AssetId = Database->GetAssetId(Path);
	if (AssetId <= 0)
	{
		OnComplete(MakeErrorResponse(FString::Printf(TEXT("Asset not found: %s"), *Path), -1));
		return true;
	}

	TArray<FMCPRelatedResult> AllResults;

	if (Direction == TEXT("both"))
	{
		// For "both", fetch from each side with offset applied to the combined result.
		// Fetch up to Offset+Limit from each side, combine, then slice.
		int32 FetchCount = Offset + Limit;
		TArray<FMCPRelatedResult> InResults = Database->GetRelated(AssetId, Rel, true, bRecursive, 10, FetchCount, 0);
		TArray<FMCPRelatedResult> OutResults = Database->GetRelated(AssetId, Rel, false, bRecursive, 10, FetchCount, 0);
		AllResults.Append(InResults);
		AllResults.Append(OutResults);
		// Apply offset+limit slice
		if (Offset < AllResults.Num())
		{
			AllResults.RemoveAt(0, FMath::Min(Offset, AllResults.Num()));
		}
		else
		{
			AllResults.Empty();
		}
		if (AllResults.Num() > Limit)
		{
			AllResults.SetNum(Limit);
		}
	}
	else
	{
		bool bIncoming = (Direction == TEXT("in"));
		AllResults = Database->GetRelated(AssetId, Rel, bIncoming, bRecursive, 10, Limit, Offset);
	}

	// Build token-optimized response
	// Short keys: n=name, p=path, t=type, r=rel, d=depth, pp=property_path, h=hardness
	// Strip /Game/ prefix from paths, omit empty/default fields
	auto CompactPath = [](const FString& InPath) -> FString
	{
		FString P = InPath;
		P.RemoveFromStart(TEXT("/Game/"));
		return P;
	};

	// Resolve C++ class paths: "/Script/Engine.Actor" -> "Actor"
	auto ResolveAssetType = [](const FString& InType) -> FString
	{
		if (InType.Contains(TEXT(".")))
		{
			int32 DotIdx;
			if (InType.FindLastChar('.', DotIdx))
			{
				return InType.Mid(DotIdx + 1);
			}
		}
		return InType;
	};

	TArray<TSharedPtr<FJsonValue>> ResultsArr;
	for (const FMCPRelatedResult& R : AllResults)
	{
		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("n"), R.Name);
		Item->SetStringField(TEXT("p"), CompactPath(R.Path));
		Item->SetStringField(TEXT("t"), ResolveAssetType(R.AssetType));
		Item->SetStringField(TEXT("r"), R.Rel);
		// Omit depth if 1 (default)
		if (R.Depth > 1)
		{
			Item->SetNumberField(TEXT("d"), R.Depth);
		}
		// Omit property_path and dep_hardness if empty
		if (!R.PropertyPath.IsEmpty())
		{
			Item->SetStringField(TEXT("pp"), R.PropertyPath);
		}
		if (!R.DepHardness.IsEmpty())
		{
			Item->SetStringField(TEXT("h"), R.DepHardness);
		}
		ResultsArr.Add(MakeShared<FJsonValueObject>(Item));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("results"), ResultsArr);

	TSharedPtr<FJsonObject> Meta = MakeShared<FJsonObject>();
	Meta->SetNumberField(TEXT("returned"), ResultsArr.Num());
	if (Offset > 0)
	{
		Meta->SetNumberField(TEXT("offset"), Offset);
	}
	bool bHasMore = ResultsArr.Num() >= Limit;
	if (bHasMore)
	{
		Meta->SetBoolField(TEXT("has_more"), true);
	}
	Meta->SetNumberField(TEXT("tokens_est"), ResultsArr.Num() * 30);
	Result->SetObjectField(TEXT("meta"), Meta);

	OnComplete(MakeJsonResponse(Result, ResultsArr.Num() * 30, bHasMore));
	return true;
}

bool FBlueprintMCPServer::HandleDtList(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	TUniquePtr<FHttpServerResponse> AuthErr;
	if (!CheckAuth(Request, AuthErr))
	{
		OnComplete(MoveTemp(AuthErr));
		return true;
	}

	if (!Database)
	{
		OnComplete(MakeErrorResponse(TEXT("Database not initialized"), -1));
		return true;
	}

	TSharedPtr<FJsonObject> Body = ParseBody(Request);
	FString RowStructFilter;
	Body->TryGetStringField(TEXT("row_struct"), RowStructFilter);
	FString FolderFilter;
	Body->TryGetStringField(TEXT("folder"), FolderFilter);

	int32 Limit = 20;
	int32 Offset = 0;
	Body->TryGetNumberField(TEXT("limit"), Limit);
	Body->TryGetNumberField(TEXT("offset"), Offset);
	Limit = FMath::Clamp(Limit, 1, 100);

	TArray<FMCPAssetRecord> DataTables = Database->QueryAssetsByType(TEXT("DataTable"));

	TArray<TSharedPtr<FJsonValue>> TablesArr;
	int32 Total = 0;
	for (const FMCPAssetRecord& DT : DataTables)
	{
		if (!RowStructFilter.IsEmpty() && DT.RowStruct != RowStructFilter) continue;
		if (!FolderFilter.IsEmpty() && !DT.Folder.Contains(FolderFilter)) continue;

		Total++;
		if (Total <= Offset) continue;
		if (TablesArr.Num() >= Limit) continue;

		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("name"), DT.Name);
		Item->SetStringField(TEXT("path"), DT.Path);
		Item->SetStringField(TEXT("row_struct"), DT.RowStruct);
		Item->SetNumberField(TEXT("row_count"), DT.RowCount);
		TablesArr.Add(MakeShared<FJsonValueObject>(Item));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("total"), Total);
	Result->SetNumberField(TEXT("returned"), TablesArr.Num());
	Result->SetBoolField(TEXT("has_more"), TablesArr.Num() < (Total - Offset));
	Result->SetArrayField(TEXT("tables"), TablesArr);

	int32 TokensEst = TablesArr.Num() * 30;
	OnComplete(MakeJsonResponse(Result, TokensEst, TablesArr.Num() < (Total - Offset)));
	return true;
}

bool FBlueprintMCPServer::HandleDtSchema(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	TUniquePtr<FHttpServerResponse> AuthErr;
	if (!CheckAuth(Request, AuthErr))
	{
		OnComplete(MoveTemp(AuthErr));
		return true;
	}

	if (!Database)
	{
		OnComplete(MakeErrorResponse(TEXT("Database not initialized"), -1));
		return true;
	}

	TSharedPtr<FJsonObject> Body = ParseBody(Request);
	FString Path = Body->GetStringField(TEXT("path"));

	if (Path.IsEmpty())
	{
		OnComplete(MakeErrorResponse(TEXT("'path' field is required"), -1));
		return true;
	}

	TOptional<FMCPAssetRecord> AssetOpt = Database->GetAsset(Path);
	if (!AssetOpt.IsSet() || AssetOpt->AssetType != TEXT("DataTable"))
	{
		OnComplete(MakeErrorResponse(FString::Printf(TEXT("DataTable not found: %s"), *Path), -1));
		return true;
	}

	const FMCPAssetRecord& Asset = AssetOpt.GetValue();
	int64 AssetId = Database->GetAssetId(Path);

	TArray<FString> ColNames = Database->GetMeta(AssetId, TEXT("dt_column"));
	TArray<FString> ColTypes = Database->GetMeta(AssetId, TEXT("dt_column_type"));

	TArray<TSharedPtr<FJsonValue>> ColumnsArr;
	int32 Count = FMath::Min(ColNames.Num(), ColTypes.Num());
	for (int32 i = 0; i < Count; ++i)
	{
		TSharedPtr<FJsonObject> Col = MakeShared<FJsonObject>();
		Col->SetStringField(TEXT("name"), ColNames[i]);
		Col->SetStringField(TEXT("type"), ColTypes[i]);
		ColumnsArr.Add(MakeShared<FJsonValueObject>(Col));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("path"), Path);
	Result->SetStringField(TEXT("row_struct"), Asset.RowStruct);
	Result->SetNumberField(TEXT("row_count"), Asset.RowCount);
	Result->SetArrayField(TEXT("columns"), ColumnsArr);

	int32 TokensEst = 50 + Count * 15;
	OnComplete(MakeJsonResponse(Result, TokensEst, false));
	return true;
}

bool FBlueprintMCPServer::HandleDtRows(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	TUniquePtr<FHttpServerResponse> AuthErr;
	if (!CheckAuth(Request, AuthErr))
	{
		OnComplete(MoveTemp(AuthErr));
		return true;
	}

	if (!Database)
	{
		OnComplete(MakeErrorResponse(TEXT("Database not initialized"), -1));
		return true;
	}

	TSharedPtr<FJsonObject> Body = ParseBody(Request);
	FString Path = Body->GetStringField(TEXT("path"));

	if (Path.IsEmpty())
	{
		OnComplete(MakeErrorResponse(TEXT("'path' field is required"), -1));
		return true;
	}

	int64 AssetId = Database->GetAssetId(Path);
	if (AssetId <= 0)
	{
		OnComplete(MakeErrorResponse(FString::Printf(TEXT("Asset not found: %s"), *Path), -1));
		return true;
	}

	// Parse row names filter
	TArray<FString> RowNames;
	const TArray<TSharedPtr<FJsonValue>>* RowsArr;
	if (Body->TryGetArrayField(TEXT("rows"), RowsArr))
	{
		for (const auto& V : *RowsArr)
		{
			RowNames.Add(V->AsString());
		}
	}

	// Parse column filter
	TArray<FString> ColumnFilter;
	const TArray<TSharedPtr<FJsonValue>>* ColsArr;
	if (Body->TryGetArrayField(TEXT("columns"), ColsArr))
	{
		for (const auto& V : *ColsArr)
		{
			ColumnFilter.Add(V->AsString());
		}
	}

	int32 MaxRows = 20;
	GConfig->GetInt(TEXT("BlueprintMCP"), TEXT("MaxDtRowsPerRequest"), MaxRows, GEditorIni);

	// Lazy load: if rows not cached, load the DataTable
	bool bCached = Database->HasDTRows(AssetId);
	WriteToLog(FString::Printf(TEXT("DT_ROWS  %s  asset_id=%lld  cached=%s"), *Path, AssetId, bCached ? TEXT("true") : TEXT("false")));

	if (!bCached)
	{
		// FSoftObjectPath::TryLoad() is more reliable in editor than StaticLoadObject
		// as it ensures the package is fully deserialized before returning
		UDataTable* DT = Cast<UDataTable>(FSoftObjectPath(Path).TryLoad());
		if (!DT)
		{
			DT = Cast<UDataTable>(StaticLoadObject(UDataTable::StaticClass(), nullptr, *Path));
		}

		WriteToLog(FString::Printf(TEXT("DT_LOAD_RESULT  %s  valid=%s  row_map=%d  row_struct=%s"),
			*Path,
			DT ? TEXT("true") : TEXT("false"),
			DT ? (int32)DT->GetRowMap().Num() : -1,
			(DT && DT->RowStruct) ? *DT->RowStruct->GetName() : TEXT("null")));

		if (!DT)
		{
			OnComplete(MakeErrorResponse(TEXT("DataTable not found or failed to load"), -1));
			return true;
		}

		// TryLoad() can pump the game loop, triggering a reindex that reassigns asset IDs.
		// Re-fetch the live ID by path to avoid FK failures from a stale AssetId.
		AssetId = Database->GetAssetIdFresh(Path);
		if (AssetId <= 0)
		{
			OnComplete(MakeErrorResponse(FString::Printf(TEXT("Asset lost after load: %s"), *Path), -1));
			return true;
		}

		Database->ClearDTRows(AssetId);

		TArray<TPair<FString, FString>> RowsToInsert;
		for (const auto& Pair : DT->GetRowMap())
		{
			FString RowJson;
			if (DT->RowStruct)
			{
				TSharedPtr<FJsonObject> RowObj = MakeShared<FJsonObject>();
				for (TFieldIterator<FProperty> It(DT->RowStruct); It; ++It)
				{
					FProperty* Prop = *It;
					FString ValueStr;
					const void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Pair.Value);
					Prop->ExportTextItem_Direct(ValueStr, ValuePtr, nullptr, nullptr, PPF_None);
					RowObj->SetStringField(Prop->GetName(), ValueStr);
				}
				RowJson = JsonToString(RowObj);
			}
			RowsToInsert.Emplace(Pair.Key.ToString(), RowJson);
		}

		WriteToLog(FString::Printf(TEXT("DT_INSERT  %s  rows=%d"), *Path, RowsToInsert.Num()));

		int32 Inserted = 0;
		if (RowsToInsert.Num() > 0)
		{
			Inserted = Database->UpsertDTRows(AssetId, RowsToInsert);
		}

		WriteToLog(FString::Printf(TEXT("DT_VERIFY  %s  inserted=%d  rows_in_db=%d"),
			*Path, Inserted, Database->GetDTRowCount(AssetId)));
	}

	// Query rows from DB
	int32 Limit = RowNames.Num() > 0 ? RowNames.Num() : MaxRows;
	TArray<TPair<FString, FString>> Rows = Database->GetDTRows(AssetId, RowNames, Limit);
	int32 TotalRows = Database->GetDTRowCount(AssetId);

	// Build response
	TSharedPtr<FJsonObject> RowsObj = MakeShared<FJsonObject>();

	for (const auto& Row : Rows)
	{
		// Parse the stored JSON
		TSharedPtr<FJsonObject> RowData;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Row.Value);
		FJsonSerializer::Deserialize(Reader, RowData);

		if (RowData.IsValid())
		{
			// Apply column filter if specified
			if (ColumnFilter.Num() > 0)
			{
				TSharedPtr<FJsonObject> Filtered = MakeShared<FJsonObject>();
				for (const FString& Col : ColumnFilter)
				{
					if (RowData->HasField(Col))
					{
						FString Val;
						if (RowData->TryGetStringField(Col, Val))
						{
							Filtered->SetStringField(Col, Val);
						}
					}
				}
				RowsObj->SetObjectField(Row.Key, Filtered);
			}
			else
			{
				RowsObj->SetObjectField(Row.Key, RowData);
			}
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("path"), Path);
	Result->SetObjectField(TEXT("rows"), RowsObj);

	TSharedPtr<FJsonObject> Meta = MakeShared<FJsonObject>();
	Meta->SetNumberField(TEXT("total_rows"), TotalRows);
	Meta->SetNumberField(TEXT("returned"), Rows.Num());
	Meta->SetBoolField(TEXT("cached"), bCached);
	Meta->SetBoolField(TEXT("truncated"), RowNames.Num() == 0 && Rows.Num() < TotalRows);
	Result->SetObjectField(TEXT("meta"), Meta);

	int32 TokensEst = Rows.Num() * 50;
	OnComplete(MakeJsonResponse(Result, TokensEst, RowNames.Num() == 0 && Rows.Num() < TotalRows));
	return true;
}

bool FBlueprintMCPServer::HandleDaValues(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	TUniquePtr<FHttpServerResponse> AuthErr;
	if (!CheckAuth(Request, AuthErr))
	{
		OnComplete(MoveTemp(AuthErr));
		return true;
	}

	if (!Database)
	{
		OnComplete(MakeErrorResponse(TEXT("Database not initialized"), -1));
		return true;
	}

	TSharedPtr<FJsonObject> Body = ParseBody(Request);
	FString Path = Body->GetStringField(TEXT("path"));

	if (Path.IsEmpty())
	{
		OnComplete(MakeErrorResponse(TEXT("'path' field is required"), -1));
		return true;
	}

	// Load the DataAsset
	UObject* Obj = StaticLoadObject(UDataAsset::StaticClass(), nullptr, *Path);
	UDataAsset* DA = Cast<UDataAsset>(Obj);
	if (!DA)
	{
		OnComplete(MakeErrorResponse(FString::Printf(TEXT("DataAsset not found or failed to load: %s"), *Path), -1));
		return true;
	}

	TSharedPtr<FJsonObject> ValuesObj = MakeShared<FJsonObject>();
	FString ClassName = DA->GetClass()->GetName();

	for (TFieldIterator<FProperty> It(DA->GetClass()); It; ++It)
	{
		FProperty* Prop = *It;
		// Skip UObject base properties
		if (Prop->GetOwnerClass() == UObject::StaticClass() || Prop->GetOwnerClass() == UDataAsset::StaticClass())
		{
			continue;
		}

		FString ValueStr;
		const void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(DA);
		Prop->ExportTextItem_Direct(ValueStr, ValuePtr, nullptr, DA, PPF_None);
		ValuesObj->SetStringField(Prop->GetName(), ValueStr);
	}

	// Cache values in asset_meta
	int64 AssetId = Database->GetAssetId(Path);
	if (AssetId > 0)
	{
		TArray<TPair<FString, FString>> Meta;
		for (const auto& Field : ValuesObj->Values)
		{
			Meta.Emplace(TEXT("da_field"), Field.Key);
			Meta.Emplace(TEXT("da_field_value"), Field.Value->AsString());
		}
		Database->ClearMeta(AssetId);
		Database->SetMeta(AssetId, Meta);
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("path"), Path);
	Result->SetStringField(TEXT("class"), ClassName);
	Result->SetObjectField(TEXT("values"), ValuesObj);

	int32 TokensEst = 100 + ValuesObj->Values.Num() * 20;
	OnComplete(MakeJsonResponse(Result, TokensEst, false));
	return true;
}

bool FBlueprintMCPServer::HandleAssetProperties(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	TUniquePtr<FHttpServerResponse> AuthErr;
	if (!CheckAuth(Request, AuthErr)) { OnComplete(MoveTemp(AuthErr)); return true; }

	TSharedPtr<FJsonObject> Body = ParseBody(Request);
	FString Path = Body->GetStringField(TEXT("path"));
	if (Path.IsEmpty())
	{
		OnComplete(MakeErrorResponse(TEXT("'path' field is required"), -1));
		return true;
	}

	// Load any UObject — works for InputMappingContext, InputAction, etc.
	UObject* Obj = StaticLoadObject(UObject::StaticClass(), nullptr, *Path);
	if (!Obj)
	{
		OnComplete(MakeErrorResponse(FString::Printf(TEXT("Asset not found or failed to load: %s"), *Path), -1));
		return true;
	}

	UClass* ObjClass = Obj->GetClass();
	TSharedPtr<FJsonObject> ValuesObj = MakeShared<FJsonObject>();

	for (TFieldIterator<FProperty> It(ObjClass); It; ++It)
	{
		FProperty* Prop = *It;

		// Skip bare UObject internals
		if (Prop->GetOwnerClass() == UObject::StaticClass())
			continue;

		FString ValueStr;
		const void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Obj);
		Prop->ExportTextItem_Direct(ValueStr, ValuePtr, nullptr, Obj, PPF_None);
		ValuesObj->SetStringField(Prop->GetName(), ValueStr);
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("path"),  Path);
	Result->SetStringField(TEXT("class"), ObjClass->GetName());
	Result->SetObjectField(TEXT("properties"), ValuesObj);

	int32 TokensEst = 100 + ValuesObj->Values.Num() * 25;
	OnComplete(MakeJsonResponse(Result, TokensEst, false));
	return true;
}
