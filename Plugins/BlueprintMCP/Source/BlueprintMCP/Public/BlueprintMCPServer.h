#pragma once

#include "CoreMinimal.h"
#include "HttpServerModule.h"
#include "HttpRouteHandle.h"
#include "IHttpRouter.h"

class FBlueprintMCPModule;
class FMCPDatabase;

class FBlueprintMCPServer
{
public:
	FBlueprintMCPServer(FBlueprintMCPModule* InOwner);
	~FBlueprintMCPServer();

	void SetDatabase(FMCPDatabase* InDB) { Database = InDB; }
	void Start();
	void Stop();

private:
	bool CheckAuth(const FHttpServerRequest& Request, TUniquePtr<FHttpServerResponse>& OutResponse);
	TSharedPtr<FJsonObject> ParseBody(const FHttpServerRequest& Request);

	// JSON-RPC helpers
	TUniquePtr<FHttpServerResponse> MakeJsonResponse(TSharedPtr<FJsonObject> Result, int32 TokensEst = 0, bool bTruncated = false);
	TUniquePtr<FHttpServerResponse> MakeErrorResponse(const FString& Message, int32 Code = -1);

	// Existing route handlers
	bool HandleToolsList(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
	bool HandleSearch(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
	bool HandleArchitecture(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
	bool HandleList(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
	bool HandleHierarchy(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
	bool HandleHeader(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
	bool HandleVars(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
	bool HandleParentVars(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
	bool HandleComponents(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
	bool HandleFuncs(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
	bool HandleRefs(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);

	// New v2 route handlers
	bool HandleAssetRelated(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
	bool HandleDtList(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
	bool HandleDtSchema(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
	bool HandleDtRows(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
	bool HandleDaValues(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
	bool HandleAssetProperties(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);

	void GenerateToken();
	FString LoadToken();

	void OpenLogFile();
	void WriteToLog(const FString& Entry);

	FBlueprintMCPModule* Owner;
	FMCPDatabase* Database = nullptr;
	TSharedPtr<IHttpRouter> Router;
	TArray<FHttpRouteHandle> RouteHandles;
	FString AuthToken;
	int32 Port;
	FString LogFilePath;
	FString LastRequestPath;
};
