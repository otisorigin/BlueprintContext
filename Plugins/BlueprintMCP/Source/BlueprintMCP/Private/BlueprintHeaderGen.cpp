#include "BlueprintHeaderGen.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "EdGraph/EdGraph.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/Paths.h"

// --- FBlueprintVarInfo ---

TSharedPtr<FJsonObject> FBlueprintVarInfo::ToJson() const
{
	TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetStringField(TEXT("name"), Name);
	Obj->SetStringField(TEXT("type"), Type);
	if (!DefaultValue.IsEmpty())
	{
		Obj->SetStringField(TEXT("default"), DefaultValue);
	}
	Obj->SetStringField(TEXT("category"), Category);

	TArray<TSharedPtr<FJsonValue>> FlagsArr;
	for (const FString& F : Flags)
	{
		FlagsArr.Add(MakeShared<FJsonValueString>(F));
	}
	Obj->SetArrayField(TEXT("flags"), FlagsArr);

	return Obj;
}

// --- FBlueprintFuncInfo ---

TSharedPtr<FJsonObject> FBlueprintFuncInfo::ToJson() const
{
	TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetStringField(TEXT("name"), Name);
	Obj->SetStringField(TEXT("category"), Category);

	TArray<TSharedPtr<FJsonValue>> FlagsArr;
	for (const FString& F : Flags)
	{
		FlagsArr.Add(MakeShared<FJsonValueString>(F));
	}
	Obj->SetArrayField(TEXT("flags"), FlagsArr);

	TArray<TSharedPtr<FJsonValue>> InputsArr;
	for (const auto& P : Inputs)
	{
		TSharedPtr<FJsonObject> PObj = MakeShared<FJsonObject>();
		PObj->SetStringField(TEXT("name"), P.Key);
		PObj->SetStringField(TEXT("type"), P.Value);
		InputsArr.Add(MakeShared<FJsonValueObject>(PObj));
	}
	Obj->SetArrayField(TEXT("inputs"), InputsArr);

	TArray<TSharedPtr<FJsonValue>> OutputsArr;
	for (const auto& P : Outputs)
	{
		TSharedPtr<FJsonObject> PObj = MakeShared<FJsonObject>();
		PObj->SetStringField(TEXT("name"), P.Key);
		PObj->SetStringField(TEXT("type"), P.Value);
		OutputsArr.Add(MakeShared<FJsonValueObject>(PObj));
	}
	Obj->SetArrayField(TEXT("outputs"), OutputsArr);

	return Obj;
}

// --- FBlueprintHeaderGen ---

FBlueprintHeaderGen::FBlueprintHeaderGen()
{
}

UBlueprint* FBlueprintHeaderGen::LoadBlueprint(const FString& AssetPath)
{
	// AssetPath may be like "/Game/Characters/BP_Player.BP_Player"
	// or just "/Game/Characters/BP_Player"
	FString CleanPath = AssetPath;

	// Ensure it has the object name suffix
	if (!CleanPath.Contains(TEXT(".")))
	{
		FString BaseName = FPaths::GetBaseFilename(CleanPath);
		CleanPath = CleanPath + TEXT(".") + BaseName;
	}

	UBlueprint* BP = LoadObject<UBlueprint>(nullptr, *CleanPath);
	return BP;
}

FString FBlueprintHeaderGen::PropertyTypeToString(const FProperty* Prop)
{
	if (!Prop)
	{
		return TEXT("void");
	}

	if (const FBoolProperty* BoolProp = CastField<FBoolProperty>(Prop))
	{
		return TEXT("bool");
	}
	if (const FIntProperty* IntProp = CastField<FIntProperty>(Prop))
	{
		return TEXT("int32");
	}
	if (const FInt64Property* Int64Prop = CastField<FInt64Property>(Prop))
	{
		return TEXT("int64");
	}
	if (const FFloatProperty* FloatProp = CastField<FFloatProperty>(Prop))
	{
		return TEXT("float");
	}
	if (const FDoubleProperty* DoubleProp = CastField<FDoubleProperty>(Prop))
	{
		return TEXT("double");
	}
	if (const FStrProperty* StrProp = CastField<FStrProperty>(Prop))
	{
		return TEXT("FString");
	}
	if (const FNameProperty* NameProp = CastField<FNameProperty>(Prop))
	{
		return TEXT("FName");
	}
	if (const FTextProperty* TextProp = CastField<FTextProperty>(Prop))
	{
		return TEXT("FText");
	}
	if (const FObjectProperty* ObjProp = CastField<FObjectProperty>(Prop))
	{
		if (ObjProp->PropertyClass)
		{
			return FString::Printf(TEXT("%s*"), *ObjProp->PropertyClass->GetName());
		}
		return TEXT("UObject*");
	}
	if (const FSoftObjectProperty* SoftProp = CastField<FSoftObjectProperty>(Prop))
	{
		if (SoftProp->PropertyClass)
		{
			return FString::Printf(TEXT("TSoftObjectPtr<%s>"), *SoftProp->PropertyClass->GetName());
		}
		return TEXT("TSoftObjectPtr<UObject>");
	}
	if (const FClassProperty* ClassProp = CastField<FClassProperty>(Prop))
	{
		if (ClassProp->MetaClass)
		{
			return FString::Printf(TEXT("TSubclassOf<%s>"), *ClassProp->MetaClass->GetName());
		}
		return TEXT("TSubclassOf<UObject>");
	}
	if (const FArrayProperty* ArrProp = CastField<FArrayProperty>(Prop))
	{
		return FString::Printf(TEXT("TArray<%s>"), *PropertyTypeToString(ArrProp->Inner));
	}
	if (const FSetProperty* SetProp = CastField<FSetProperty>(Prop))
	{
		return FString::Printf(TEXT("TSet<%s>"), *PropertyTypeToString(SetProp->ElementProp));
	}
	if (const FMapProperty* MapProp = CastField<FMapProperty>(Prop))
	{
		return FString::Printf(TEXT("TMap<%s, %s>"), *PropertyTypeToString(MapProp->KeyProp), *PropertyTypeToString(MapProp->ValueProp));
	}
	if (const FStructProperty* StructProp = CastField<FStructProperty>(Prop))
	{
		if (StructProp->Struct)
		{
			return StructProp->Struct->GetName();
		}
		return TEXT("FStruct");
	}
	if (const FEnumProperty* EnumProp = CastField<FEnumProperty>(Prop))
	{
		if (EnumProp->GetEnum())
		{
			return EnumProp->GetEnum()->GetName();
		}
		return TEXT("enum");
	}
	if (const FByteProperty* ByteProp = CastField<FByteProperty>(Prop))
	{
		if (ByteProp->Enum)
		{
			return ByteProp->Enum->GetName();
		}
		return TEXT("uint8");
	}

	return Prop->GetCPPType();
}

FString FBlueprintHeaderGen::GetPropertyFlags(const FProperty* Prop)
{
	TArray<FString> Flags;

	if (Prop->HasAnyPropertyFlags(CPF_Edit))
	{
		if (Prop->HasAnyPropertyFlags(CPF_DisableEditOnInstance))
		{
			Flags.Add(TEXT("EditDefaultsOnly"));
		}
		else
		{
			Flags.Add(TEXT("EditAnywhere"));
		}
	}

	if (Prop->HasAnyPropertyFlags(CPF_BlueprintVisible))
	{
		if (Prop->HasAnyPropertyFlags(CPF_BlueprintReadOnly))
		{
			Flags.Add(TEXT("BlueprintReadOnly"));
		}
		else
		{
			Flags.Add(TEXT("BlueprintReadWrite"));
		}
	}

	if (Prop->HasAnyPropertyFlags(CPF_Net))
	{
		Flags.Add(TEXT("Replicated"));
	}

	if (Prop->HasAnyPropertyFlags(CPF_ExposeOnSpawn))
	{
		Flags.Add(TEXT("ExposeOnSpawn"));
	}

	return FString::Join(Flags, TEXT(", "));
}

int32 FBlueprintHeaderGen::EstimateTokens(const FString& Text)
{
	// Rough estimate: ~4 chars per token for code
	return Text.Len() / 4;
}

TArray<FBlueprintVarInfo> FBlueprintHeaderGen::GenerateVars(const FString& AssetPath)
{
	TArray<FBlueprintVarInfo> Result;

	UBlueprint* BP = LoadBlueprint(AssetPath);
	if (!BP)
	{
		return Result;
	}

	UClass* GenClass = BP->GeneratedClass;
	if (!GenClass)
	{
		return Result;
	}

	for (const FBPVariableDescription& VarDesc : BP->NewVariables)
	{
		FBlueprintVarInfo Info;
		Info.Name = VarDesc.VarName.ToString();
		Info.Category = VarDesc.Category.ToString();

		// Get type from generated class property
		FProperty* Prop = GenClass->FindPropertyByName(VarDesc.VarName);
		if (Prop)
		{
			Info.Type = PropertyTypeToString(Prop);

			// Get default value
			if (GenClass->GetDefaultObject())
			{
				FString DefaultStr;
				Prop->ExportTextItem_Direct(DefaultStr, Prop->ContainerPtrToValuePtr<void>(GenClass->GetDefaultObject()), nullptr, nullptr, PPF_None);
				if (!DefaultStr.IsEmpty())
				{
					Info.DefaultValue = DefaultStr;
				}
			}

			// Flags
			FString FlagStr = GetPropertyFlags(Prop);
			if (!FlagStr.IsEmpty())
			{
				FlagStr.ParseIntoArray(Info.Flags, TEXT(", "), true);
			}
		}
		else
		{
			// Fallback type from pin type
			Info.Type = VarDesc.VarType.PinCategory.ToString();
		}

		Result.Add(Info);
	}

	return Result;
}

TArray<FBlueprintFuncInfo> FBlueprintHeaderGen::GenerateFuncs(const FString& AssetPath)
{
	TArray<FBlueprintFuncInfo> Result;

	UBlueprint* BP = LoadBlueprint(AssetPath);
	if (!BP)
	{
		return Result;
	}

	UClass* GenClass = BP->GeneratedClass;

	for (const UEdGraph* Graph : BP->FunctionGraphs)
	{
		if (!Graph)
		{
			continue;
		}

		FBlueprintFuncInfo Info;
		Info.Name = Graph->GetName();

		// Try to get function metadata from generated class
		if (GenClass)
		{
			UFunction* Func = GenClass->FindFunctionByName(*Info.Name);
			if (Func)
			{
				// Category
				Info.Category = Func->GetMetaData(TEXT("Category"));

				// Flags
				if (Func->HasAnyFunctionFlags(FUNC_BlueprintCallable))
				{
					Info.Flags.Add(TEXT("BlueprintCallable"));
				}
				if (Func->HasAnyFunctionFlags(FUNC_BlueprintPure))
				{
					Info.Flags.Add(TEXT("BlueprintPure"));
				}
				if (Func->HasAnyFunctionFlags(FUNC_BlueprintEvent))
				{
					Info.Flags.Add(TEXT("BlueprintImplementableEvent"));
				}
				if (Func->HasAnyFunctionFlags(FUNC_Net))
				{
					Info.Flags.Add(TEXT("Replicated"));
				}
				if (Func->HasAnyFunctionFlags(FUNC_NetServer))
				{
					Info.Flags.Add(TEXT("Server"));
				}
				if (Func->HasAnyFunctionFlags(FUNC_NetClient))
				{
					Info.Flags.Add(TEXT("Client"));
				}

				// Parameters
				for (TFieldIterator<FProperty> It(Func); It; ++It)
				{
					FProperty* Param = *It;
					FString ParamType = PropertyTypeToString(Param);
					FString ParamName = Param->GetName();

					if (Param->HasAnyPropertyFlags(CPF_ReturnParm) || Param->HasAnyPropertyFlags(CPF_OutParm))
					{
						Info.Outputs.Add({ParamName, ParamType});
					}
					else
					{
						Info.Inputs.Add({ParamName, ParamType});
					}
				}
			}
		}

		Result.Add(Info);
	}

	return Result;
}

FString FBlueprintHeaderGen::GenerateHeaderText(UBlueprint* BP, const TArray<FBlueprintVarInfo>& Vars, const TArray<FBlueprintFuncInfo>& Funcs)
{
	FString Header;

	// Class declaration
	FString ClassName = BP->GetName();
	FString ParentName = BP->ParentClass ? BP->ParentClass->GetName() : TEXT("UObject");

	Header += TEXT("#pragma once\n\n");

	// Interfaces
	TArray<FString> InterfaceNames;
	for (const FBPInterfaceDescription& Iface : BP->ImplementedInterfaces)
	{
		if (Iface.Interface)
		{
			InterfaceNames.Add(Iface.Interface->GetName());
		}
	}

	Header += FString::Printf(TEXT("// Blueprint: %s\n"), *BP->GetPathName());
	Header += FString::Printf(TEXT("// Parent: %s\n"), *ParentName);
	if (InterfaceNames.Num() > 0)
	{
		Header += FString::Printf(TEXT("// Interfaces: %s\n"), *FString::Join(InterfaceNames, TEXT(", ")));
	}
	Header += TEXT("\n");

	Header += FString::Printf(TEXT("UCLASS()\nclass %s : public %s"), *ClassName, *ParentName);
	if (InterfaceNames.Num() > 0)
	{
		for (const FString& Iface : InterfaceNames)
		{
			Header += FString::Printf(TEXT(", public I%s"), *Iface.Mid(Iface.StartsWith(TEXT("BPI_")) ? 4 : 0));
		}
	}
	Header += TEXT("\n{\n\tGENERATED_BODY()\n\npublic:\n");

	// Variables
	if (Vars.Num() > 0)
	{
		Header += TEXT("\t// --- Variables ---\n\n");
		for (const FBlueprintVarInfo& V : Vars)
		{
			if (V.Flags.Num() > 0)
			{
				Header += FString::Printf(TEXT("\tUPROPERTY(%s)\n"), *FString::Join(V.Flags, TEXT(", ")));
			}
			else
			{
				Header += TEXT("\tUPROPERTY()\n");
			}
			Header += FString::Printf(TEXT("\t%s %s"), *V.Type, *V.Name);
			if (!V.DefaultValue.IsEmpty())
			{
				Header += FString::Printf(TEXT(" = %s"), *V.DefaultValue);
			}
			Header += TEXT(";\n\n");
		}
	}

	// Functions
	if (Funcs.Num() > 0)
	{
		Header += TEXT("\t// --- Functions ---\n\n");
		for (const FBlueprintFuncInfo& F : Funcs)
		{
			if (F.Flags.Num() > 0)
			{
				Header += FString::Printf(TEXT("\tUFUNCTION(%s)\n"), *FString::Join(F.Flags, TEXT(", ")));
			}
			else
			{
				Header += TEXT("\tUFUNCTION()\n");
			}

			// Return type
			FString ReturnType = TEXT("void");
			if (F.Outputs.Num() == 1)
			{
				ReturnType = F.Outputs[0].Value;
			}

			// Parameters
			TArray<FString> Params;
			for (const auto& In : F.Inputs)
			{
				Params.Add(FString::Printf(TEXT("%s %s"), *In.Value, *In.Key));
			}
			// Multiple outputs as out params
			if (F.Outputs.Num() > 1)
			{
				for (const auto& Out : F.Outputs)
				{
					Params.Add(FString::Printf(TEXT("%s& %s"), *Out.Value, *Out.Key));
				}
				ReturnType = TEXT("void");
			}

			Header += FString::Printf(TEXT("\t%s %s(%s);\n\n"), *ReturnType, *F.Name, *FString::Join(Params, TEXT(", ")));
		}
	}

	Header += TEXT("};\n");

	return Header;
}

FBlueprintHeaderData FBlueprintHeaderGen::Generate(const FString& AssetPath)
{
	// Check cache
	if (FBlueprintHeaderData* Cached = Cache.Find(AssetPath))
	{
		Cached->bCached = true;
		return *Cached;
	}

	FBlueprintHeaderData Data;
	Data.Path = AssetPath;
	Data.bCached = false;

	UBlueprint* BP = LoadBlueprint(AssetPath);
	if (!BP)
	{
		Data.HeaderText = TEXT("// Error: Could not load blueprint");
		Data.TokensEst = 10;
		return Data;
	}

	Data.Variables = GenerateVars(AssetPath);
	Data.Functions = GenerateFuncs(AssetPath);
	Data.HeaderText = GenerateHeaderText(BP, Data.Variables, Data.Functions);
	Data.TokensEst = EstimateTokens(Data.HeaderText);

	// Cache it
	Cache.Add(AssetPath, Data);

	return Data;
}

void FBlueprintHeaderGen::InvalidateCache(const FString& AssetPath)
{
	Cache.Remove(AssetPath);
}

FString FBlueprintHeaderGen::GetCacheDir() const
{
	return FPaths::ProjectSavedDir() / TEXT("BlueprintMCP") / TEXT("headers");
}
