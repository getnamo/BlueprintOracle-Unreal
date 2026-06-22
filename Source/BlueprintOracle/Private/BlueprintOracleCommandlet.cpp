// Copyright 2026-current Getnamo. All Rights Reserved.

#include "BlueprintOracleCommandlet.h"

#include "Engine/Blueprint.h"
#include "Engine/DataTable.h"
#include "StructUtils/UserDefinedStruct.h"
#include "Engine/UserDefinedEnum.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphUtilities.h"

#include "K2Node.h"
#include "K2Node_CallFunction.h"
#include "K2Node_Variable.h"
#include "K2Node_VariableGet.h"
#include "K2Node_Event.h"
#include "K2Node_BreakStruct.h"
#include "K2Node_MakeStruct.h"
#include "K2Node_CallDelegate.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_VariableGet.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_EditablePinBase.h"

// Write-side (programmatic blueprint editing) APIs.
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/CompilerResultsLog.h"
#include "EdGraphSchema_K2.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Kismet/KismetMathLibrary.h"
#include "GameFramework/Pawn.h"
#include "UObject/SavePackage.h"

#include "ScriptDisassembler.h"

#include "UObject/Linker.h"
#include "UObject/LinkerLoad.h"
#include "UObject/UObjectHash.h"
#include "UObject/UnrealType.h"

#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/PackageName.h"
#include "Misc/StringOutputDevice.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "GenericPlatform/GenericPlatformFile.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetRegistry/ARFilter.h"

#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"

DEFINE_LOG_CATEGORY_STATIC(LogBlueprintOracle, Log, All);

namespace
{
	FString GuidStr(const FGuid& Guid)
	{
		return Guid.ToString(EGuidFormats::Digits);
	}

	int32 PackageIndexToInt(FPackageIndex Index)
	{
		// Positive = export (1-based), negative = import (1-based), 0 = null. Mirrors FPackageIndex.
		if (Index.IsExport())
		{
			return Index.ToExport() + 1;
		}
		if (Index.IsImport())
		{
			return -(Index.ToImport() + 1);
		}
		return 0;
	}

	TSharedRef<FJsonObject> PinTypeToJson(const FEdGraphPinType& Type)
	{
		TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetStringField(TEXT("category"), Type.PinCategory.ToString());
		O->SetStringField(TEXT("subCategory"), Type.PinSubCategory.ToString());
		if (UObject* SubObj = Type.PinSubCategoryObject.Get())
		{
			O->SetStringField(TEXT("subCategoryObject"), SubObj->GetPathName());
		}
		FString Container = TEXT("None");
		switch (Type.ContainerType)
		{
		case EPinContainerType::Array: Container = TEXT("Array"); break;
		case EPinContainerType::Set:   Container = TEXT("Set");   break;
		case EPinContainerType::Map:   Container = TEXT("Map");   break;
		default: break;
		}
		O->SetStringField(TEXT("container"), Container);
		O->SetBoolField(TEXT("isReference"), Type.bIsReference);
		O->SetBoolField(TEXT("isConst"), Type.bIsConst);
		return O;
	}

	// Function/variable/event name a node refers to, if any.
	FString NodeMemberName(UEdGraphNode* Node)
	{
		if (UK2Node_CallFunction* CF = Cast<UK2Node_CallFunction>(Node))
		{
			return CF->FunctionReference.GetMemberName().ToString();
		}
		if (UK2Node_Variable* V = Cast<UK2Node_Variable>(Node))
		{
			return V->VariableReference.GetMemberName().ToString();
		}
		if (UK2Node_Event* E = Cast<UK2Node_Event>(Node))
		{
			return E->EventReference.GetMemberName().ToString();
		}
		return FString();
	}

	void SaveJson(const TSharedRef<FJsonObject>& Root, const FString& Path)
	{
		FString Out;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
		FJsonSerializer::Serialize(Root, Writer);
		if (!FFileHelper::SaveStringToFile(Out, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			UE_LOG(LogBlueprintOracle, Error, TEXT("Failed to write %s"), *Path);
		}
	}
}

UBlueprintOracleCommandlet::UBlueprintOracleCommandlet()
{
	IsClient = false;
	IsServer = false;
	IsEditor = true;
	LogToConsole = true;
}

int32 UBlueprintOracleCommandlet::Main(const FString& Params)
{
	// -selftest : prove the programmatic-edit loop end-to-end on a disposable
	// in-memory blueprint (no live assets touched). Returns non-zero on failure.
	if (FParse::Param(*Params, TEXT("selftest")))
	{
		FString OutDirSelf;
		FParse::Value(*Params, TEXT("out="), OutDirSelf);
		if (OutDirSelf.IsEmpty())
		{
			OutDirSelf = FPaths::ProjectSavedDir() / TEXT("BlueprintOracle");
		}
		IFileManager::Get().MakeDirectory(*OutDirSelf, /*Tree*/ true);
		return RunSelfTest(OutDirSelf);
	}

	// -dumptable -asset=A,B : dump UDataTable(s) as JSON (row struct + row names + rows).
	if (FParse::Param(*Params, TEXT("dumptable")))
	{
		FString TableArg;
		FParse::Value(*Params, TEXT("asset="), TableArg, /*bShouldStopOnSeparator*/ false);
		FString DumpOut;
		FParse::Value(*Params, TEXT("out="), DumpOut);
		if (DumpOut.IsEmpty())
		{
			DumpOut = FPaths::ProjectSavedDir() / TEXT("BlueprintOracle");
		}
		IFileManager::Get().MakeDirectory(*DumpOut, /*Tree*/ true);
		TArray<FString> Tables;
		TableArg.ParseIntoArray(Tables, TEXT(","), /*CullEmpty*/ true);
		for (const FString& TablePkg : Tables)
		{
			UPackage* Pkg = LoadPackage(nullptr, *TablePkg.TrimStartAndEnd(), LOAD_None);
			if (!Pkg) { UE_LOG(LogBlueprintOracle, Error, TEXT("Cannot load %s"), *TablePkg); continue; }
			Pkg->FullyLoad();
			TArray<UObject*> Objects;
			GetObjectsWithOuter(Pkg, Objects, false);
			FString Name = TablePkg; Name.RemoveFromStart(TEXT("/")); Name.ReplaceInline(TEXT("/"), TEXT("_"));
			for (UObject* Obj : Objects)
			{
				if (UDataTable* Table = Cast<UDataTable>(Obj))
				{
					TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
					Root->SetStringField(TEXT("table"), Table->GetName());
					Root->SetStringField(TEXT("rowStruct"), Table->GetRowStructPathName().ToString());
					TArray<TSharedPtr<FJsonValue>> RowNames;
					for (const FName& RowName : Table->GetRowNames())
					{
						RowNames.Add(MakeShared<FJsonValueString>(RowName.ToString()));
					}
					Root->SetArrayField(TEXT("rowNames"), RowNames);
					Root->SetStringField(TEXT("rowsJson"), Table->GetTableAsJSON(EDataTableExportFlags::None));
					SaveJson(Root, DumpOut / Name + TEXT(".table.json"));
					UE_LOG(LogBlueprintOracle, Display, TEXT("  wrote table %s (%d rows)"),
						*Table->GetName(), Table->GetRowNames().Num());
					break;
				}
			}
		}
		return 0;
	}

	// -migrate -spec=<file.json> : apply a structural edit spec, compile-gated.
	// Dry run by default (no .uasset is written); pass -commit to save on clean compile.
	if (FParse::Param(*Params, TEXT("migrate")))
	{
		FString SpecPath;
		if (!FParse::Value(*Params, TEXT("spec="), SpecPath) || SpecPath.IsEmpty())
		{
			UE_LOG(LogBlueprintOracle, Error, TEXT("-migrate requires -spec=<path to migration json>."));
			return 1;
		}
		const bool bCommit = FParse::Param(*Params, TEXT("commit"));
		return RunMigration(SpecPath, bCommit);
	}

	// -settablefield -table=<path> -row=<name> -field=<authoredName> -value=<int> [-commit]
	// Set a byte/enum/int field on a DataTable row (top-level or one level of nested struct,
	// matched by authored name). For correcting content like an item's EquippedSlot. Dry-run
	// by default (reports without saving); -commit writes the .uasset.
	if (FParse::Param(*Params, TEXT("settablefield")))
	{
		FString TablePath, RowName, FieldName, ValueStr;
		FParse::Value(*Params, TEXT("table="), TablePath);
		FParse::Value(*Params, TEXT("row="), RowName);
		FParse::Value(*Params, TEXT("field="), FieldName);
		FParse::Value(*Params, TEXT("value="), ValueStr);
		const bool bCommit = FParse::Param(*Params, TEXT("commit"));
		if (TablePath.IsEmpty() || RowName.IsEmpty() || FieldName.IsEmpty() || ValueStr.IsEmpty())
		{
			UE_LOG(LogBlueprintOracle, Error, TEXT("-settablefield needs -table -row -field -value."));
			return 1;
		}
		UDataTable* Table = LoadObject<UDataTable>(nullptr, *TablePath);
		const UScriptStruct* RowStruct = Table ? Table->GetRowStruct() : nullptr;
		uint8* Row = RowStruct ? Table->FindRowUnchecked(FName(*RowName)) : nullptr;
		if (!Row)
		{
			UE_LOG(LogBlueprintOracle, Error, TEXT("-settablefield: row '%s' not found in '%s'"), *RowName, *TablePath);
			return 1;
		}
		const int64 NewValue = FCString::Atoi64(*ValueStr);
		auto SetField = [](UStruct* S, void* Container, const FString& N, int64 V) -> bool
		{
			for (TFieldIterator<FProperty> It(S); It; ++It)
			{
				if (It->GetAuthoredName() != N) { continue; }
				if (FByteProperty* BP = CastField<FByteProperty>(*It)) { BP->SetPropertyValue_InContainer(Container, (uint8)V); return true; }
				if (FEnumProperty* EP = CastField<FEnumProperty>(*It)) { EP->GetUnderlyingProperty()->SetIntPropertyValue(EP->ContainerPtrToValuePtr<void>(Container), V); return true; }
				if (FIntProperty* IP = CastField<FIntProperty>(*It)) { IP->SetPropertyValue_InContainer(Container, (int32)V); return true; }
			}
			return false;
		};
		bool bSet = SetField(const_cast<UScriptStruct*>(RowStruct), Row, FieldName, NewValue);
		for (TFieldIterator<FProperty> It(RowStruct); It && !bSet; ++It)
		{
			if (FStructProperty* SP = CastField<FStructProperty>(*It))
			{
				bSet = SetField(SP->Struct, SP->ContainerPtrToValuePtr<void>(Row), FieldName, NewValue);
			}
		}
		if (!bSet)
		{
			UE_LOG(LogBlueprintOracle, Error, TEXT("-settablefield: field '%s' not found on row '%s'"), *FieldName, *RowName);
			return 1;
		}
		UE_LOG(LogBlueprintOracle, Display, TEXT("-settablefield: %s[%s].%s = %lld (%s)"),
			*TablePath, *RowName, *FieldName, NewValue, bCommit ? TEXT("COMMIT") : TEXT("DRY-RUN"));
		if (bCommit)
		{
			UPackage* Package = Table->GetOutermost();
			Package->MarkPackageDirty();
			const FString FileName = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
			FPlatformFileManager::Get().GetPlatformFile().SetReadOnly(*FileName, false);
			FSavePackageArgs SaveArgs;
			SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
			SaveArgs.SaveFlags = SAVE_NoError;
			const FSavePackageResultStruct Result = UPackage::Save(Package, nullptr, *FileName, SaveArgs);
			UE_LOG(LogBlueprintOracle, Display, TEXT("  %s %s"), Result.IsSuccessful() ? TEXT("SAVED") : TEXT("SAVE FAILED"), *FileName);
			return Result.IsSuccessful() ? 0 : 1;
		}
		return 0;
	}

	TArray<FString> AssetPaths;

	// -dir=/MountPoint : enumerate every Blueprint under a content path via the
	// asset registry (filters out textures/meshes/data assets automatically).
	FString DirArg;
	if (FParse::Value(*Params, TEXT("dir="), DirArg))
	{
		FAssetRegistryModule& Module = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
		IAssetRegistry& Registry = Module.Get();
		UE_LOG(LogBlueprintOracle, Display, TEXT("Scanning %s for blueprints..."), *DirArg);
		Registry.ScanPathsSynchronous({DirArg}, /*bForceRescan*/ true);

		FARFilter Filter;
		Filter.bRecursivePaths = true;
		Filter.PackagePaths.Add(FName(*DirArg));
		Filter.bRecursiveClasses = true;
		Filter.ClassPaths.Add(UBlueprint::StaticClass()->GetClassPathName());

		TArray<FAssetData> Found;
		Registry.GetAssets(Filter, Found);
		for (const FAssetData& Asset : Found)
		{
			AssetPaths.Add(Asset.PackageName.ToString());
		}
		UE_LOG(LogBlueprintOracle, Display, TEXT("Found %d blueprint(s) under %s"), Found.Num(), *DirArg);
	}

	FString AssetArg;
	if (FParse::Value(*Params, TEXT("asset="), AssetArg, /*bShouldStopOnSeparator*/ false))
	{
		TArray<FString> Listed;
		AssetArg.ParseIntoArray(Listed, TEXT(","), /*CullEmpty*/ true);
		AssetPaths.Append(Listed);
	}

	if (AssetPaths.Num() == 0)
	{
		UE_LOG(LogBlueprintOracle, Error,
			TEXT("No assets specified. Pass -dir=/MountPoint to enumerate all blueprints under a content "
			     "path, and/or -asset=/Path/A,/Path/B for specific blueprints."));
		return 1;
	}

	FString OutDir;
	FParse::Value(*Params, TEXT("out="), OutDir);
	if (OutDir.IsEmpty())
	{
		OutDir = FPaths::ProjectSavedDir() / TEXT("BlueprintOracle");
	}
	IFileManager::Get().MakeDirectory(*OutDir, /*Tree*/ true);

	UE_LOG(LogBlueprintOracle, Display, TEXT("BlueprintOracle: %d asset(s) -> %s"), AssetPaths.Num(), *OutDir);

	for (const FString& PackageName : AssetPaths)
	{
		ProcessAsset(PackageName.TrimStartAndEnd(), OutDir);
	}

	UE_LOG(LogBlueprintOracle, Display, TEXT("BlueprintOracle: done."));
	return 0;
}

void UBlueprintOracleCommandlet::ProcessAsset(const FString& PackageName, const FString& OutDir)
{
	UE_LOG(LogBlueprintOracle, Display, TEXT("Processing %s"), *PackageName);

	UPackage* Package = LoadPackage(nullptr, *PackageName, LOAD_None);
	if (!Package)
	{
		UE_LOG(LogBlueprintOracle, Error, TEXT("Could not load package %s"), *PackageName);
		return;
	}
	Package->FullyLoad();

	// Use the full package path (with '/' -> '_') as the output filename so assets
	// that share a short name (e.g. multiple "OrkKnight") don't overwrite each other.
	FString AssetName = PackageName;
	AssetName.RemoveFromStart(TEXT("/"));
	AssetName.ReplaceInline(TEXT("/"), TEXT("_"));

	// Layout works on any package; do it first while the linker is fresh.
	WriteLayout(Package, OutDir / AssetName + TEXT(".layout.json"));

	UBlueprint* Blueprint = nullptr;
	{
		TArray<UObject*> Objects;
		GetObjectsWithOuter(Package, Objects, /*bIncludeNestedObjects*/ false);
		for (UObject* Obj : Objects)
		{
			Blueprint = Cast<UBlueprint>(Obj);
			if (Blueprint)
			{
				break;
			}
		}
	}

	if (!Blueprint)
	{
		// Not a Blueprint - try UserDefinedStruct / UserDefinedEnum (item data models).
		TArray<UObject*> Objects;
		GetObjectsWithOuter(Package, Objects, /*bIncludeNestedObjects*/ false);
		for (UObject* Obj : Objects)
		{
			if (UUserDefinedStruct* Struct = Cast<UUserDefinedStruct>(Obj))
			{
				WriteStruct(Struct, OutDir / AssetName + TEXT(".struct.json"));
				return;
			}
			if (UUserDefinedEnum* Enum = Cast<UUserDefinedEnum>(Obj))
			{
				WriteEnum(Enum, OutDir / AssetName + TEXT(".enum.json"));
				return;
			}
		}
		UE_LOG(LogBlueprintOracle, Warning, TEXT("%s is not a Blueprint/Struct/Enum; wrote layout only."), *PackageName);
		return;
	}

	WriteGraphs(Blueprint, AssetName, OutDir);
	WriteDisasm(Blueprint, OutDir / AssetName + TEXT(".disasm.txt"));
}

void UBlueprintOracleCommandlet::WriteLayout(UPackage* Package, const FString& OutPath)
{
	FLinkerLoad* Linker = Package->GetLinker();
	if (!Linker)
	{
		Linker = FLinkerLoad::FindExistingLinkerForPackage(Package);
	}
	if (!Linker)
	{
		UE_LOG(LogBlueprintOracle, Warning, TEXT("No linker for %s; skipping layout."), *Package->GetName());
		return;
	}

	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("package"), Package->GetName());

	// Summary cross-check fields.
	const FPackageFileSummary& Summary = Linker->Summary;
	TSharedRef<FJsonObject> Sum = MakeShared<FJsonObject>();
	Sum->SetNumberField(TEXT("totalHeaderSize"), Summary.TotalHeaderSize);
	Sum->SetNumberField(TEXT("nameCount"), Summary.NameCount);
	Sum->SetNumberField(TEXT("nameOffset"), Summary.NameOffset);
	Sum->SetNumberField(TEXT("exportCount"), Summary.ExportCount);
	Sum->SetNumberField(TEXT("exportOffset"), Summary.ExportOffset);
	Sum->SetNumberField(TEXT("importCount"), Summary.ImportCount);
	Sum->SetNumberField(TEXT("importOffset"), Summary.ImportOffset);
	Sum->SetNumberField(TEXT("fileVersionUE4"), Summary.GetFileVersionUE().FileVersionUE4);
	Sum->SetNumberField(TEXT("fileVersionUE5"), Summary.GetFileVersionUE().FileVersionUE5);
	Sum->SetNumberField(TEXT("fileVersionLicensee"), Summary.GetFileVersionLicenseeUE());
	Root->SetObjectField(TEXT("summary"), Sum);

	// Exports with byte offsets - the key alignment data for the Python reader.
	TArray<TSharedPtr<FJsonValue>> Exports;
	for (int32 i = 0; i < Linker->ExportMap.Num(); ++i)
	{
		const FObjectExport& Export = Linker->ExportMap[i];
		TSharedRef<FJsonObject> E = MakeShared<FJsonObject>();
		E->SetNumberField(TEXT("index"), i);
		E->SetStringField(TEXT("name"), Export.ObjectName.ToString());
		E->SetStringField(TEXT("class"), Linker->GetExportClassName(i).ToString());
		E->SetNumberField(TEXT("classIndex"), PackageIndexToInt(Export.ClassIndex));
		E->SetNumberField(TEXT("superIndex"), PackageIndexToInt(Export.SuperIndex));
		E->SetNumberField(TEXT("templateIndex"), PackageIndexToInt(Export.TemplateIndex));
		E->SetNumberField(TEXT("outerIndex"), PackageIndexToInt(Export.OuterIndex));
		E->SetNumberField(TEXT("serialOffset"), (double)Export.SerialOffset);
		E->SetNumberField(TEXT("serialSize"), (double)Export.SerialSize);
		E->SetNumberField(TEXT("objectFlags"), (double)(uint32)Export.ObjectFlags);
		Exports.Add(MakeShared<FJsonValueObject>(E));
	}
	Root->SetArrayField(TEXT("exports"), Exports);

	// Imports.
	TArray<TSharedPtr<FJsonValue>> Imports;
	for (int32 i = 0; i < Linker->ImportMap.Num(); ++i)
	{
		const FObjectImport& Import = Linker->ImportMap[i];
		TSharedRef<FJsonObject> I = MakeShared<FJsonObject>();
		I->SetNumberField(TEXT("index"), i);
		I->SetStringField(TEXT("name"), Import.ObjectName.ToString());
		I->SetStringField(TEXT("class"), Import.ClassName.ToString());
		I->SetStringField(TEXT("classPackage"), Import.ClassPackage.ToString());
		I->SetNumberField(TEXT("outerIndex"), PackageIndexToInt(Import.OuterIndex));
		Imports.Add(MakeShared<FJsonValueObject>(I));
	}
	Root->SetArrayField(TEXT("imports"), Imports);

	SaveJson(Root, OutPath);
	UE_LOG(LogBlueprintOracle, Display, TEXT("  wrote layout (%d exports, %d imports)"),
		Linker->ExportMap.Num(), Linker->ImportMap.Num());
}

void UBlueprintOracleCommandlet::WriteGraphs(UBlueprint* Blueprint, const FString& AssetName, const FString& OutDir)
{
	// Gather every authored graph.
	TArray<UEdGraph*> Graphs;
	Graphs.Append(Blueprint->UbergraphPages);
	Graphs.Append(Blueprint->FunctionGraphs);
	Graphs.Append(Blueprint->MacroGraphs);
	Graphs.Append(Blueprint->DelegateSignatureGraphs);

	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset"), AssetName);
	Root->SetStringField(TEXT("parentClass"), Blueprint->ParentClass ? Blueprint->ParentClass->GetPathName() : TEXT(""));
	Root->SetStringField(TEXT("generatedClass"), Blueprint->GeneratedClass ? Blueprint->GeneratedClass->GetName() : TEXT(""));

	// Implemented interfaces.
	TArray<TSharedPtr<FJsonValue>> Interfaces;
	for (const FBPInterfaceDescription& Iface : Blueprint->ImplementedInterfaces)
	{
		if (Iface.Interface)
		{
			Interfaces.Add(MakeShared<FJsonValueString>(Iface.Interface->GetPathName()));
		}
	}
	Root->SetArrayField(TEXT("interfaces"), Interfaces);

	// Variables.
	TArray<TSharedPtr<FJsonValue>> Variables;
	for (const FBPVariableDescription& Var : Blueprint->NewVariables)
	{
		TSharedRef<FJsonObject> V = MakeShared<FJsonObject>();
		V->SetStringField(TEXT("name"), Var.VarName.ToString());
		V->SetStringField(TEXT("category"), Var.Category.ToString());
		V->SetObjectField(TEXT("type"), PinTypeToJson(Var.VarType));
		V->SetStringField(TEXT("defaultValue"), Var.DefaultValue);
		Variables.Add(MakeShared<FJsonValueObject>(V));
	}
	Root->SetArrayField(TEXT("variables"), Variables);

	// Components from the construction script.
	TArray<TSharedPtr<FJsonValue>> Components;
	if (Blueprint->SimpleConstructionScript)
	{
		for (USCS_Node* Node : Blueprint->SimpleConstructionScript->GetAllNodes())
		{
			if (!Node)
			{
				continue;
			}
			TSharedRef<FJsonObject> C = MakeShared<FJsonObject>();
			C->SetStringField(TEXT("variableName"), Node->GetVariableName().ToString());
			C->SetStringField(TEXT("componentClass"), Node->ComponentClass ? Node->ComponentClass->GetPathName() : TEXT(""));
			C->SetStringField(TEXT("parentAttachment"), Node->ParentComponentOrVariableName.ToString());
			Components.Add(MakeShared<FJsonValueObject>(C));
		}
	}
	Root->SetArrayField(TEXT("components"), Components);

	// Per-graph node + pin walk, and accumulate ExportNodesToText.
	FString NodesText;
	TArray<TSharedPtr<FJsonValue>> GraphsJson;
	for (UEdGraph* Graph : Graphs)
	{
		if (!Graph)
		{
			continue;
		}

		TSharedRef<FJsonObject> G = MakeShared<FJsonObject>();
		G->SetStringField(TEXT("name"), Graph->GetName());

		TArray<TSharedPtr<FJsonValue>> NodesJson;
		TSet<UObject*> NodeSet;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node)
			{
				continue;
			}
			NodeSet.Add(Node);

			TSharedRef<FJsonObject> N = MakeShared<FJsonObject>();
			N->SetStringField(TEXT("guid"), GuidStr(Node->NodeGuid));
			N->SetStringField(TEXT("class"), Node->GetClass()->GetName());
			N->SetStringField(TEXT("title"), Node->GetNodeTitle(ENodeTitleType::ListView).ToString());
			N->SetStringField(TEXT("comment"), Node->NodeComment);
			N->SetNumberField(TEXT("posX"), Node->NodePosX);
			N->SetNumberField(TEXT("posY"), Node->NodePosY);
			const FString Member = NodeMemberName(Node);
			if (!Member.IsEmpty())
			{
				N->SetStringField(TEXT("memberName"), Member);
			}

			TArray<TSharedPtr<FJsonValue>> PinsJson;
			for (UEdGraphPin* Pin : Node->Pins)
			{
				if (!Pin)
				{
					continue;
				}
				TSharedRef<FJsonObject> P = MakeShared<FJsonObject>();
				P->SetStringField(TEXT("id"), GuidStr(Pin->PinId));
				P->SetStringField(TEXT("name"), Pin->PinName.ToString());
				P->SetStringField(TEXT("direction"), Pin->Direction == EGPD_Input ? TEXT("Input") : TEXT("Output"));
				P->SetObjectField(TEXT("type"), PinTypeToJson(Pin->PinType));
				P->SetStringField(TEXT("defaultValue"), Pin->DefaultValue);
				P->SetStringField(TEXT("defaultObject"), Pin->DefaultObject ? Pin->DefaultObject->GetPathName() : TEXT(""));
				P->SetStringField(TEXT("defaultText"), Pin->DefaultTextValue.ToString());

				TArray<TSharedPtr<FJsonValue>> LinkedTo;
				for (UEdGraphPin* Linked : Pin->LinkedTo)
				{
					if (!Linked)
					{
						continue;
					}
					TSharedRef<FJsonObject> L = MakeShared<FJsonObject>();
					UEdGraphNode* OwningNode = Linked->GetOwningNodeUnchecked();
					L->SetStringField(TEXT("node"), OwningNode ? GuidStr(OwningNode->NodeGuid) : TEXT(""));
					L->SetStringField(TEXT("pin"), GuidStr(Linked->PinId));
					LinkedTo.Add(MakeShared<FJsonValueObject>(L));
				}
				P->SetArrayField(TEXT("linkedTo"), LinkedTo);

				PinsJson.Add(MakeShared<FJsonValueObject>(P));
			}
			N->SetArrayField(TEXT("pins"), PinsJson);
			NodesJson.Add(MakeShared<FJsonValueObject>(N));
		}
		G->SetArrayField(TEXT("nodes"), NodesJson);
		GraphsJson.Add(MakeShared<FJsonValueObject>(G));

		// ExportNodesToText ground truth for this graph.
		FString GraphText;
		FEdGraphUtilities::ExportNodesToText(NodeSet, GraphText);
		NodesText += FString::Printf(TEXT("========== GRAPH: %s ==========\n"), *Graph->GetName());
		NodesText += GraphText;
		NodesText += TEXT("\n\n");
	}
	Root->SetArrayField(TEXT("graphs"), GraphsJson);

	SaveJson(Root, OutDir / AssetName + TEXT(".graph.json"));
	FFileHelper::SaveStringToFile(NodesText, *(OutDir / AssetName + TEXT(".nodes.txt")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	UE_LOG(LogBlueprintOracle, Display, TEXT("  wrote graph.json + nodes.txt (%d graphs)"), GraphsJson.Num());
}

void UBlueprintOracleCommandlet::WriteDisasm(UBlueprint* Blueprint, const FString& OutPath)
{
	UClass* GenClass = Blueprint->GeneratedClass;
	if (!GenClass)
	{
		UE_LOG(LogBlueprintOracle, Warning, TEXT("No generated class for %s; skipping disasm."), *Blueprint->GetName());
		return;
	}

	FStringOutputDevice Out;
	FKismetBytecodeDisassembler Disassembler(Out);
	for (TFieldIterator<UFunction> It(GenClass, EFieldIteratorFlags::ExcludeSuper); It; ++It)
	{
		UFunction* Function = *It;
		Out.Logf(TEXT("========== FUNCTION: %s =========="), *Function->GetName());
		Disassembler.DisassembleStructure(Function);
		Out.Logf(TEXT(""));
	}

	FFileHelper::SaveStringToFile(static_cast<const FString&>(Out), *OutPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	UE_LOG(LogBlueprintOracle, Display, TEXT("  wrote disasm.txt"));
}

void UBlueprintOracleCommandlet::WriteStruct(UUserDefinedStruct* Struct, const FString& OutPath)
{
	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("name"), Struct->GetName());
	Root->SetStringField(TEXT("kind"), TEXT("UserDefinedStruct"));

	TArray<TSharedPtr<FJsonValue>> Fields;
	for (TFieldIterator<FProperty> It(Struct); It; ++It)
	{
		FProperty* Property = *It;
		TSharedRef<FJsonObject> F = MakeShared<FJsonObject>();
		F->SetStringField(TEXT("name"), Property->GetAuthoredName());
		F->SetStringField(TEXT("cppType"), Property->GetCPPType());
		F->SetStringField(TEXT("propertyClass"), Property->GetClass()->GetName());
		// For container/struct/enum/object properties, record the referenced type.
		FString Inner;
		if (const FStructProperty* SP = CastField<FStructProperty>(Property))
		{
			Inner = SP->Struct ? SP->Struct->GetName() : TEXT("");
		}
		else if (const FArrayProperty* AP = CastField<FArrayProperty>(Property))
		{
			Inner = AP->Inner ? AP->Inner->GetCPPType() : TEXT("");
		}
		else if (const FByteProperty* BP = CastField<FByteProperty>(Property))
		{
			Inner = BP->Enum ? BP->Enum->GetName() : TEXT("");
		}
		else if (const FEnumProperty* EP = CastField<FEnumProperty>(Property))
		{
			Inner = EP->GetEnum() ? EP->GetEnum()->GetName() : TEXT("");
		}
		if (!Inner.IsEmpty())
		{
			F->SetStringField(TEXT("innerType"), Inner);
		}
		Fields.Add(MakeShared<FJsonValueObject>(F));
	}
	Root->SetArrayField(TEXT("fields"), Fields);

	SaveJson(Root, OutPath);
	UE_LOG(LogBlueprintOracle, Display, TEXT("  wrote struct.json (%d fields)"), Fields.Num());
}

void UBlueprintOracleCommandlet::WriteEnum(UUserDefinedEnum* Enum, const FString& OutPath)
{
	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("name"), Enum->GetName());
	Root->SetStringField(TEXT("kind"), TEXT("UserDefinedEnum"));

	TArray<TSharedPtr<FJsonValue>> Entries;
	// NumEnums() includes the implicit _MAX entry; skip it.
	const int32 Num = Enum->NumEnums();
	for (int32 i = 0; i < Num; ++i)
	{
		const FString Name = Enum->GetNameStringByIndex(i);
		if (Name.EndsWith(TEXT("_MAX")))
		{
			continue;
		}
		TSharedRef<FJsonObject> E = MakeShared<FJsonObject>();
		E->SetNumberField(TEXT("index"), i);
		E->SetStringField(TEXT("name"), Name);  // authored name, e.g. "NewEnumerator7"
		E->SetStringField(TEXT("displayName"), Enum->GetDisplayNameTextByIndex(i).ToString());
		E->SetNumberField(TEXT("value"), (double)Enum->GetValueByIndex(i));
		Entries.Add(MakeShared<FJsonValueObject>(E));
	}
	Root->SetArrayField(TEXT("entries"), Entries);

	SaveJson(Root, OutPath);
	UE_LOG(LogBlueprintOracle, Display, TEXT("  wrote enum.json (%d entries)"), Entries.Num());
}

namespace
{
	// Resolve a class by full path ("/Script/Module.ClassName") or generated-class path.
	UClass* ResolveClass(const FString& Path)
	{
		if (Path.IsEmpty())
		{
			return nullptr;
		}
		if (UClass* Found = FindObject<UClass>(nullptr, *Path))
		{
			return Found;
		}
		return LoadObject<UClass>(nullptr, *Path);
	}

	UScriptStruct* ResolveStruct(const FString& Path)
	{
		if (Path.IsEmpty()) { return nullptr; }
		if (UScriptStruct* Found = FindObject<UScriptStruct>(nullptr, *Path)) { return Found; }
		return LoadObject<UScriptStruct>(nullptr, *Path);
	}

	// A UserDefinedStruct member pin is named "<Field>_<Index>_<32 hex GUID>". Return the
	// authored "<Field>" so specs can reference clean field names instead of the GUID soup.
	FString StripStructPinGuid(const FString& PinName)
	{
		int32 Last = INDEX_NONE;
		if (!PinName.FindLastChar(TEXT('_'), Last)) { return PinName; }
		const FString Tail = PinName.Mid(Last + 1);
		bool bHex = Tail.Len() == 32;
		for (int32 i = 0; bHex && i < Tail.Len(); ++i) { bHex = FChar::IsHexDigit(Tail[i]); }
		if (!bHex) { return PinName; }
		const FString Rest = PinName.Left(Last);             // "<Field>_<Index>"
		int32 Last2 = INDEX_NONE;
		if (!Rest.FindLastChar(TEXT('_'), Last2)) { return PinName; }
		if (!Rest.Mid(Last2 + 1).IsNumeric()) { return PinName; }
		return Rest.Left(Last2);                              // "<Field>"
	}

	// Resolve a "nodeRef:pinName" token (nodeRef in NodeMap, e.g. "$entry"/"$result"/an id)
	// to a pin of the given direction. Logs the available pins if the name isn't found.
	UEdGraphPin* ResolvePin(const TMap<FString, UEdGraphNode*>& NodeMap, const FString& Token,
		EEdGraphPinDirection Dir)
	{
		FString Ref, PinName;
		if (!Token.Split(TEXT(":"), &Ref, &PinName))
		{
			UE_LOG(LogBlueprintOracle, Error, TEXT("    buildBody: bad pin token '%s' (want nodeRef:pin)"), *Token);
			return nullptr;
		}
		UEdGraphNode* const* Node = NodeMap.Find(Ref);
		if (!Node || !*Node)
		{
			UE_LOG(LogBlueprintOracle, Error, TEXT("    buildBody: unknown node ref '%s'"), *Ref);
			return nullptr;
		}
		// DynamicCast: its result object pin is named "As<ClassName>" (awkward to spell in a spec),
		// so accept "result"/"out" for the cast-result output and "Object"/"in" for the source input.
		if (UK2Node_DynamicCast* DC = Cast<UK2Node_DynamicCast>(*Node))
		{
			if (Dir == EGPD_Output && (PinName == TEXT("result") || PinName == TEXT("out") || PinName == TEXT("ReturnValue")))
			{
				return DC->GetCastResultPin();
			}
			if (Dir == EGPD_Input && (PinName == TEXT("Object") || PinName == TEXT("in")))
			{
				return DC->GetCastSourcePin();
			}
		}
		if (UEdGraphPin* Pin = (*Node)->FindPin(FName(*PinName), Dir))
		{
			return Pin;
		}
		// Fallback: match a struct member pin by its authored field-name prefix.
		for (UEdGraphPin* P : (*Node)->Pins)
		{
			if (P && P->Direction == Dir && StripStructPinGuid(P->PinName.ToString()) == PinName)
			{
				return P;
			}
		}
		// Helpful diagnostics: dump the candidate pins for that direction.
		FString Avail;
		for (UEdGraphPin* P : (*Node)->Pins)
		{
			if (P && P->Direction == Dir) { Avail += P->PinName.ToString() + TEXT(", "); }
		}
		UE_LOG(LogBlueprintOracle, Error,
			TEXT("    buildBody: pin '%s' (%s) not found on node '%s'. Available: [%s]"),
			*PinName, Dir == EGPD_Output ? TEXT("out") : TEXT("in"), *Ref, *Avail);
		return nullptr;
	}

	// Find a function graph (or any authored graph) on a blueprint by name.
	UEdGraph* FindGraphByName(UBlueprint* Blueprint, const FString& Name)
	{
		auto Search = [&Name](const TArray<UEdGraph*>& Graphs) -> UEdGraph*
		{
			for (UEdGraph* G : Graphs)
			{
				if (G && G->GetName() == Name)
				{
					return G;
				}
			}
			return nullptr;
		};
		if (UEdGraph* G = Search(Blueprint->FunctionGraphs)) { return G; }
		if (UEdGraph* G = Search(Blueprint->UbergraphPages)) { return G; }
		if (UEdGraph* G = Search(Blueprint->MacroGraphs)) { return G; }
		return nullptr;
	}
}

bool UBlueprintOracleCommandlet::ApplyMigrationOp(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& Op)
{
	FString OpName;
	if (!Op->TryGetStringField(TEXT("op"), OpName))
	{
		UE_LOG(LogBlueprintOracle, Error, TEXT("    op object missing 'op' field"));
		return false;
	}

	if (OpName == TEXT("reparent"))
	{
		const FString NewParent = Op->GetStringField(TEXT("newParent"));
		UClass* ParentClass = ResolveClass(NewParent);
		if (!ParentClass)
		{
			UE_LOG(LogBlueprintOracle, Error, TEXT("    reparent: cannot resolve class '%s'"), *NewParent);
			return false;
		}
		Blueprint->ParentClass = ParentClass;
		FBlueprintEditorUtils::RefreshAllNodes(Blueprint);
		UE_LOG(LogBlueprintOracle, Display, TEXT("    reparent -> %s"), *ParentClass->GetPathName());
		return true;
	}

	if (OpName == TEXT("removeGraph"))
	{
		const FString GraphName = Op->GetStringField(TEXT("name"));
		UEdGraph* Graph = FindGraphByName(Blueprint, GraphName);
		if (!Graph)
		{
			// Not fatal: the graph may already be gone (idempotent re-run).
			UE_LOG(LogBlueprintOracle, Warning, TEXT("    removeGraph: '%s' not found (skipping)"), *GraphName);
			return true;
		}
		FBlueprintEditorUtils::RemoveGraph(Blueprint, Graph, EGraphRemoveFlags::Default);
		UE_LOG(LogBlueprintOracle, Display, TEXT("    removeGraph '%s'"), *GraphName);
		return true;
	}

	if (OpName == TEXT("removeVar"))
	{
		const FString VarName = Op->GetStringField(TEXT("name"));
		FBlueprintEditorUtils::RemoveMemberVariable(Blueprint, FName(*VarName));
		UE_LOG(LogBlueprintOracle, Display, TEXT("    removeVar '%s'"), *VarName);
		return true;
	}

	if (OpName == TEXT("replaceVarRefs"))
	{
		const FString From = Op->GetStringField(TEXT("from"));
		const FString To = Op->GetStringField(TEXT("to"));
		FBlueprintEditorUtils::ReplaceVariableReferences(Blueprint, FName(*From), FName(*To));
		UE_LOG(LogBlueprintOracle, Display, TEXT("    replaceVarRefs '%s' -> '%s'"), *From, *To);
		return true;
	}

	if (OpName == TEXT("redirectCall"))
	{
		// Redirect every CallFunction node matching 'fromMember' to 'toMember' on
		// 'toClass' (Move 1 auto-rewire). Optional 'graph' restricts to one graph.
		const FString FromMember = Op->GetStringField(TEXT("fromMember"));
		const FString ToMember = Op->GetStringField(TEXT("toMember"));
		const FString ToClassPath = Op->GetStringField(TEXT("toClass"));
		UClass* ToClass = ResolveClass(ToClassPath);
		if (!ToClass)
		{
			UE_LOG(LogBlueprintOracle, Error, TEXT("    redirectCall: cannot resolve toClass '%s'"), *ToClassPath);
			return false;
		}
		FString OnlyGraph;
		Op->TryGetStringField(TEXT("graph"), OnlyGraph);

		TArray<UEdGraph*> Graphs;
		Graphs.Append(Blueprint->UbergraphPages);
		Graphs.Append(Blueprint->FunctionGraphs);
		Graphs.Append(Blueprint->MacroGraphs);

		int32 Count = 0;
		for (UEdGraph* Graph : Graphs)
		{
			if (!Graph || (!OnlyGraph.IsEmpty() && Graph->GetName() != OnlyGraph))
			{
				continue;
			}
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(Node);
				if (Call && Call->FunctionReference.GetMemberName() == FName(*FromMember))
				{
					Call->FunctionReference.SetExternalMember(FName(*ToMember), ToClass);
					Call->ReconstructNode();
					++Count;
				}
			}
		}
		UE_LOG(LogBlueprintOracle, Display, TEXT("    redirectCall '%s' -> %s::%s (%d node(s))"),
			*FromMember, *ToClass->GetName(), *ToMember, Count);
		return true;
	}

	if (OpName == TEXT("setCallPinDefault"))
	{
		// Recover/repair a literal default on a CallFunction input pin. Used when a
		// param rename (a BP pin name not reproducible as a C++ identifier, e.g.
		// "Items Lost (Percent)" -> ItemsLostPercent) leaves the new pin at its type
		// default and the old value stranded on an orphaned pin. ReconstructNode drops
		// the orphan; we then write the intended value onto the canonical pin.
		const FString Member = Op->GetStringField(TEXT("member"));
		const FString PinName = Op->GetStringField(TEXT("pin"));
		const FString Value = Op->GetStringField(TEXT("value"));
		FString OnlyGraph;
		Op->TryGetStringField(TEXT("graph"), OnlyGraph);

		TArray<UEdGraph*> Graphs;
		Graphs.Append(Blueprint->UbergraphPages);
		Graphs.Append(Blueprint->FunctionGraphs);
		Graphs.Append(Blueprint->MacroGraphs);

		int32 Count = 0;
		for (UEdGraph* Graph : Graphs)
		{
			if (!Graph || (!OnlyGraph.IsEmpty() && Graph->GetName() != OnlyGraph))
			{
				continue;
			}
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(Node);
				if (Call && Call->FunctionReference.GetMemberName() == FName(*Member))
				{
					Call->ReconstructNode();
					if (UEdGraphPin* Pin = Call->FindPin(FName(*PinName), EGPD_Input))
					{
						Pin->Modify();
						if (const UEdGraphSchema* Schema = Pin->GetSchema())
						{
							Schema->TrySetDefaultValue(*Pin, Value);
						}
						else
						{
							Pin->DefaultValue = Value;
						}
						++Count;
					}
					else
					{
						UE_LOG(LogBlueprintOracle, Warning,
							TEXT("    setCallPinDefault: pin '%s' not found on a '%s' node"), *PinName, *Member);
					}

					// ReconstructNode retains orphaned pins (stale name + value) so users
					// don't silently lose data; once we've transferred the value, strip
					// them, else they emit a "pin no longer exists" warning forever.
					TArray<UEdGraphPin*> Orphans;
					for (UEdGraphPin* P : Call->Pins)
					{
						if (P && P->bOrphanedPin)
						{
							Orphans.Add(P);
						}
					}
					for (UEdGraphPin* P : Orphans)
					{
						Call->RemovePin(P);
					}
				}
			}
		}
		UE_LOG(LogBlueprintOracle, Display, TEXT("    setCallPinDefault %s.%s = '%s' (%d node(s))"),
			*Member, *PinName, *Value, Count);
		return true;
	}

	if (OpName == TEXT("buildBody"))
	{
		// Replace a function graph's body. Keeps the FunctionEntry/Result, removes every
		// other node, then builds the spec's nodes and links. A small graph-authoring
		// engine: kinds = callFunction / breakStruct / makeStruct / callDelegate; node
		// refs in links use "$entry"/"$result"/<id>; pins by "ref:pinName".
		const FString GraphName = Op->GetStringField(TEXT("graph"));
		UEdGraph* Graph = FindGraphByName(Blueprint, GraphName);
		if (!Graph)
		{
			UE_LOG(LogBlueprintOracle, Error, TEXT("    buildBody: graph '%s' not found"), *GraphName);
			return false;
		}

		UK2Node_FunctionEntry* Entry = nullptr;
		UK2Node_FunctionResult* Result = nullptr;
		for (UEdGraphNode* N : Graph->Nodes)
		{
			if (UK2Node_FunctionEntry* E = Cast<UK2Node_FunctionEntry>(N)) { Entry = E; }
			if (UK2Node_FunctionResult* R = Cast<UK2Node_FunctionResult>(N)) { Result = R; }
		}

		// "append": true keeps the existing body and adds nodes to it (links may reference the
		// existing nodes by their 32-char GUID, as in graph.json); default REPLACES the body.
		bool bAppend = false;
		Op->TryGetBoolField(TEXT("append"), bAppend);
		if (!bAppend)
		{
			// Strip the old body (everything but entry/result).
			TArray<UEdGraphNode*> ToRemove;
			for (UEdGraphNode* N : Graph->Nodes)
			{
				if (N && N != Entry && N != Result) { ToRemove.Add(N); }
			}
			for (UEdGraphNode* N : ToRemove)
			{
				FBlueprintEditorUtils::RemoveNode(Blueprint, N, /*bDontRecompile*/ true);
			}
		}

		TMap<FString, UEdGraphNode*> NodeMap;
		if (Entry) { NodeMap.Add(TEXT("$entry"), Entry); }
		if (Result) { NodeMap.Add(TEXT("$result"), Result); }
		if (bAppend)
		{
			// Let links target the preserved nodes by GUID (e.g. wire an existing tail node's
			// "then" into the appended chain).
			for (UEdGraphNode* N : Graph->Nodes)
			{
				if (N) { NodeMap.Add(N->NodeGuid.ToString(EGuidFormats::Digits), N); }
			}
		}

		UClass* SkelClass = Blueprint->SkeletonGeneratedClass ? Blueprint->SkeletonGeneratedClass : Blueprint->GeneratedClass;

		const TArray<TSharedPtr<FJsonValue>>* NodeSpecs = nullptr;
		Op->TryGetArrayField(TEXT("nodes"), NodeSpecs);
		int32 PosY = 0;
		if (NodeSpecs)
		{
			for (const TSharedPtr<FJsonValue>& NV : *NodeSpecs)
			{
				const TSharedPtr<FJsonObject> NObj = NV->AsObject();
				if (!NObj.IsValid()) { continue; }
				const FString Id = NObj->GetStringField(TEXT("id"));
				const FString Kind = NObj->GetStringField(TEXT("kind"));
				UEdGraphNode* Created = nullptr;

				if (Kind == TEXT("callFunction"))
				{
					FGraphNodeCreator<UK2Node_CallFunction> C(*Graph);
					UK2Node_CallFunction* Node = C.CreateNode();
					const FString Member = NObj->GetStringField(TEXT("member"));
					bool bSelf = false;
					NObj->TryGetBoolField(TEXT("self"), bSelf);
					if (bSelf)
					{
						Node->FunctionReference.SetSelfMember(FName(*Member));
					}
					else
					{
						UClass* Cls = ResolveClass(NObj->GetStringField(TEXT("class")));
						Node->FunctionReference.SetExternalMember(FName(*Member), Cls);
					}
					Node->NodePosX = 300; Node->NodePosY = PosY;
					C.Finalize();
					Created = Node;
				}
				else if (Kind == TEXT("breakStruct") || Kind == TEXT("makeStruct"))
				{
					UScriptStruct* S = ResolveStruct(NObj->GetStringField(TEXT("struct")));
					if (!S)
					{
						UE_LOG(LogBlueprintOracle, Error, TEXT("    buildBody: cannot resolve struct for node '%s'"), *Id);
						return false;
					}
					if (Kind == TEXT("breakStruct"))
					{
						FGraphNodeCreator<UK2Node_BreakStruct> C(*Graph);
						UK2Node_BreakStruct* Node = C.CreateNode();
						Node->StructType = S;
						Node->NodePosX = 150; Node->NodePosY = PosY;
						C.Finalize();
						Created = Node;
					}
					else
					{
						FGraphNodeCreator<UK2Node_MakeStruct> C(*Graph);
						UK2Node_MakeStruct* Node = C.CreateNode();
						Node->StructType = S;
						Node->NodePosX = 450; Node->NodePosY = PosY;
						C.Finalize();
						Created = Node;
					}
				}
				else if (Kind == TEXT("cast"))
				{
					// Pure DynamicCast (no exec pins) for a data-path downcast, e.g. a C++ AActor*
					// return -> a BP-typed (WR_ItemMaster) pin. "to" = target class path. Link the
					// object in via "<id>:Object" and the result out via "<id>:result".
					UClass* ToCls = ResolveClass(NObj->GetStringField(TEXT("to")));
					if (!ToCls)
					{
						UE_LOG(LogBlueprintOracle, Error, TEXT("    buildBody: cannot resolve cast target class for node '%s'"), *Id);
						return false;
					}
					FGraphNodeCreator<UK2Node_DynamicCast> C(*Graph);
					UK2Node_DynamicCast* Node = C.CreateNode();
					Node->TargetType = ToCls;
					Node->SetPurity(true); // pure (data-only) cast - no exec pins
					Node->NodePosX = 400; Node->NodePosY = PosY;
					C.Finalize();
					Created = Node;
				}
				else if (Kind == TEXT("variableGet"))
				{
					// Read a member variable (self). Output pin is named after the variable, e.g.
					// {"kind":"variableGet","var":"ItemID"} -> link "<id>:ItemID".
					FGraphNodeCreator<UK2Node_VariableGet> C(*Graph);
					UK2Node_VariableGet* Node = C.CreateNode();
					Node->VariableReference.SetSelfMember(FName(*NObj->GetStringField(TEXT("var"))));
					Node->NodePosX = 100; Node->NodePosY = PosY;
					C.Finalize();
					Created = Node;
				}
				else if (Kind == TEXT("branch"))
				{
					// K2Node_IfThenElse. Pins: execute (in exec), Condition (in bool),
					// then / else (out exec). Standard names - no special pin resolution needed.
					FGraphNodeCreator<UK2Node_IfThenElse> C(*Graph);
					UK2Node_IfThenElse* Node = C.CreateNode();
					Node->NodePosX = 400; Node->NodePosY = PosY;
					C.Finalize();
					Created = Node;
				}
				else if (Kind == TEXT("callDelegate"))
				{
					const FName DName(*NObj->GetStringField(TEXT("delegate")));
					FMulticastDelegateProperty* DProp =
						FindFProperty<FMulticastDelegateProperty>(SkelClass, DName);
					FGraphNodeCreator<UK2Node_CallDelegate> C(*Graph);
					UK2Node_CallDelegate* Node = C.CreateNode();
					if (DProp)
					{
						Node->SetFromProperty(DProp, /*bSelfContext*/ true, SkelClass);
					}
					else
					{
						UE_LOG(LogBlueprintOracle, Warning, TEXT("    buildBody: delegate '%s' not found"), *DName.ToString());
					}
					Node->NodePosX = 600; Node->NodePosY = PosY;
					C.Finalize();
					Created = Node;
				}
				else
				{
					UE_LOG(LogBlueprintOracle, Error, TEXT("    buildBody: unknown node kind '%s'"), *Kind);
					return false;
				}

				// Optional pin literal defaults: "defaults": [ {"pin": "...", "value": "..."} ].
				const TArray<TSharedPtr<FJsonValue>>* Defaults = nullptr;
				if (Created && NObj->TryGetArrayField(TEXT("defaults"), Defaults))
				{
					for (const TSharedPtr<FJsonValue>& DV : *Defaults)
					{
						const TSharedPtr<FJsonObject> DObj = DV->AsObject();
						if (!DObj.IsValid()) { continue; }
						const FString PinName = DObj->GetStringField(TEXT("pin"));
						const FString Value = DObj->GetStringField(TEXT("value"));
						if (UEdGraphPin* Pin = Created->FindPin(FName(*PinName), EGPD_Input))
						{
							if (const UEdGraphSchema* Schema = Pin->GetSchema())
							{
								Schema->TrySetDefaultValue(*Pin, Value);
							}
							else
							{
								Pin->DefaultValue = Value;
							}
						}
						else
						{
							UE_LOG(LogBlueprintOracle, Warning,
								TEXT("    buildBody: default pin '%s' not found on node '%s'"), *PinName, *Id);
						}
					}
				}

				NodeMap.Add(Id, Created);
				PosY += 200;
			}
		}

		// Wire links.
		int32 LinkOk = 0, LinkFail = 0;
		const TArray<TSharedPtr<FJsonValue>>* LinkSpecs = nullptr;
		Op->TryGetArrayField(TEXT("links"), LinkSpecs);
		if (LinkSpecs)
		{
			for (const TSharedPtr<FJsonValue>& LV : *LinkSpecs)
			{
				const TSharedPtr<FJsonObject> LObj = LV->AsObject();
				if (!LObj.IsValid()) { continue; }
				UEdGraphPin* FromPin = ResolvePin(NodeMap, LObj->GetStringField(TEXT("from")), EGPD_Output);
				UEdGraphPin* ToPin = ResolvePin(NodeMap, LObj->GetStringField(TEXT("to")), EGPD_Input);
				if (FromPin && ToPin)
				{
					FromPin->MakeLinkTo(ToPin);
					++LinkOk;
				}
				else
				{
					++LinkFail;
				}
			}
		}

		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
		UE_LOG(LogBlueprintOracle, Display, TEXT("    buildBody '%s': %d node(s), %d link(s) ok, %d failed"),
			*GraphName, NodeMap.Num() - 2, LinkOk, LinkFail);
		return LinkFail == 0;
	}

	if (OpName == TEXT("spliceCall"))
	{
		// Move 2 (general converter splice): create a CallFunction node and use its output to
		// DRIVE an existing input pin ("into"), replacing whatever currently feeds that pin; wire
		// the new call's inputs from existing node pins. Existing nodes are identified by GUID
		// (as emitted in graph.json), pins by name. Used to gate/transform a wire without
		// rebuilding the whole graph (which buildBody would).
		const FString GraphName = Op->GetStringField(TEXT("graph"));
		UEdGraph* Graph = FindGraphByName(Blueprint, GraphName);
		if (!Graph)
		{
			UE_LOG(LogBlueprintOracle, Error, TEXT("    spliceCall: graph '%s' not found"), *GraphName);
			return false;
		}

		// GUID -> node map for this graph.
		TMap<FString, UEdGraphNode*> ByGuid;
		for (UEdGraphNode* N : Graph->Nodes)
		{
			if (N) { ByGuid.Add(N->NodeGuid.ToString(EGuidFormats::Digits), N); }
		}
		auto FindNode = [&ByGuid](const FString& Guid) -> UEdGraphNode*
		{
			UEdGraphNode* const* P = ByGuid.Find(Guid);
			return P ? *P : nullptr;
		};
		// FindPin by name with the struct-member authored-prefix fallback (mirrors ResolvePin).
		auto FindPinByName = [](UEdGraphNode* Node, const FString& PinName, EEdGraphPinDirection Dir) -> UEdGraphPin*
		{
			if (!Node) { return nullptr; }
			if (UEdGraphPin* Pin = Node->FindPin(FName(*PinName), Dir)) { return Pin; }
			for (UEdGraphPin* P : Node->Pins)
			{
				if (P && P->Direction == Dir && StripStructPinGuid(P->PinName.ToString()) == PinName) { return P; }
			}
			return nullptr;
		};

		// Create the call node.
		FGraphNodeCreator<UK2Node_CallFunction> C(*Graph);
		UK2Node_CallFunction* Call = C.CreateNode();
		const FString Member = Op->GetStringField(TEXT("member"));
		bool bSelf = false;
		Op->TryGetBoolField(TEXT("self"), bSelf);
		if (bSelf)
		{
			Call->FunctionReference.SetSelfMember(FName(*Member));
		}
		else
		{
			Call->FunctionReference.SetExternalMember(FName(*Member), ResolveClass(Op->GetStringField(TEXT("class"))));
		}
		C.Finalize();

		FString OutPinName = TEXT("ReturnValue");
		Op->TryGetStringField(TEXT("out"), OutPinName);
		UEdGraphPin* OutPin = FindPinByName(Call, OutPinName, EGPD_Output);
		if (!OutPin)
		{
			UE_LOG(LogBlueprintOracle, Error, TEXT("    spliceCall: function '%s' has no output pin '%s'"), *Member, *OutPinName);
			return false;
		}

		// Drive the target input pin, breaking its current source link(s) first.
		const TSharedPtr<FJsonObject> IntoObj = Op->GetObjectField(TEXT("into"));
		UEdGraphNode* IntoNode = FindNode(IntoObj->GetStringField(TEXT("node")));
		UEdGraphPin* IntoPin = FindPinByName(IntoNode, IntoObj->GetStringField(TEXT("pin")), EGPD_Input);
		if (!IntoPin)
		{
			UE_LOG(LogBlueprintOracle, Error, TEXT("    spliceCall: 'into' pin '%s' not found on node '%s'"),
				*IntoObj->GetStringField(TEXT("pin")), *IntoObj->GetStringField(TEXT("node")));
			return false;
		}
		IntoPin->BreakAllPinLinks();
		OutPin->MakeLinkTo(IntoPin);

		// Wire the call's inputs from existing node output pins.
		int32 InOk = 0, InFail = 0;
		const TArray<TSharedPtr<FJsonValue>>* Inputs = nullptr;
		if (Op->TryGetArrayField(TEXT("inputs"), Inputs))
		{
			for (const TSharedPtr<FJsonValue>& IV : *Inputs)
			{
				const TSharedPtr<FJsonObject> IObj = IV->AsObject();
				if (!IObj.IsValid()) { continue; }
				UEdGraphPin* DstPin = FindPinByName(Call, IObj->GetStringField(TEXT("pin")), EGPD_Input);
				UEdGraphNode* SrcNode = FindNode(IObj->GetStringField(TEXT("node")));
				UEdGraphPin* SrcPin = FindPinByName(SrcNode, IObj->GetStringField(TEXT("fromPin")), EGPD_Output);
				if (DstPin && SrcPin) { SrcPin->MakeLinkTo(DstPin); ++InOk; }
				else
				{
					++InFail;
					UE_LOG(LogBlueprintOracle, Error, TEXT("    spliceCall: could not wire input '%s' (dst=%d src=%d)"),
						*IObj->GetStringField(TEXT("pin")), DstPin != nullptr, SrcPin != nullptr);
				}
			}
		}

		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
		UE_LOG(LogBlueprintOracle, Display,
			TEXT("    spliceCall '%s' -> %s.%s : %d input(s) wired, %d failed"),
			*Member, *IntoObj->GetStringField(TEXT("node")), *IntoObj->GetStringField(TEXT("pin")), InOk, InFail);
		return InFail == 0;
	}

	if (OpName == TEXT("connectPins"))
	{
		// Connect two existing pins (exec or data), breaking the target pin's current links first.
		// from/to = {node:<guid>, pin}. Used to rewire exec flow (e.g. bypass a removed loop).
		const FString GraphName = Op->GetStringField(TEXT("graph"));
		UEdGraph* Graph = FindGraphByName(Blueprint, GraphName);
		if (!Graph)
		{
			UE_LOG(LogBlueprintOracle, Error, TEXT("    connectPins: graph '%s' not found"), *GraphName);
			return false;
		}
		TMap<FString, UEdGraphNode*> ByGuid;
		for (UEdGraphNode* N : Graph->Nodes) { if (N) { ByGuid.Add(N->NodeGuid.ToString(EGuidFormats::Digits), N); } }
		auto FindPinByName = [&ByGuid](const TSharedPtr<FJsonObject>& Spec, EEdGraphPinDirection Dir) -> UEdGraphPin*
		{
			UEdGraphNode* const* NP = ByGuid.Find(Spec->GetStringField(TEXT("node")));
			if (!NP || !*NP) { return nullptr; }
			const FString PinName = Spec->GetStringField(TEXT("pin"));
			if (UEdGraphPin* P = (*NP)->FindPin(FName(*PinName), Dir)) { return P; }
			for (UEdGraphPin* P : (*NP)->Pins)
			{
				if (P && P->Direction == Dir && StripStructPinGuid(P->PinName.ToString()) == PinName) { return P; }
			}
			return nullptr;
		};
		UEdGraphPin* From = FindPinByName(Op->GetObjectField(TEXT("from")), EGPD_Output);
		UEdGraphPin* To = FindPinByName(Op->GetObjectField(TEXT("to")), EGPD_Input);
		if (!From || !To)
		{
			UE_LOG(LogBlueprintOracle, Error, TEXT("    connectPins: pin not found (from=%d to=%d)"), From != nullptr, To != nullptr);
			return false;
		}
		To->BreakAllPinLinks();
		From->MakeLinkTo(To);
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
		UE_LOG(LogBlueprintOracle, Display, TEXT("    connectPins ok"));
		return true;
	}

	// Build an FEdGraphPinType from a spec object's {category, struct?/class?/sub?, container?}.
	auto BuildPinType = [this](const TSharedPtr<FJsonObject>& Spec, FEdGraphPinType& T) -> bool
	{
		const FString Category = Spec->GetStringField(TEXT("category"));
		if (Category == TEXT("struct")) { T.PinCategory = UEdGraphSchema_K2::PC_Struct; T.PinSubCategoryObject = ResolveStruct(Spec->GetStringField(TEXT("struct"))); }
		else if (Category == TEXT("object")) { T.PinCategory = UEdGraphSchema_K2::PC_Object; T.PinSubCategoryObject = ResolveClass(Spec->GetStringField(TEXT("class"))); }
		else if (Category == TEXT("class")) { T.PinCategory = UEdGraphSchema_K2::PC_Class; T.PinSubCategoryObject = ResolveClass(Spec->GetStringField(TEXT("class"))); }
		else if (Category == TEXT("real")) { FString S; T.PinCategory = UEdGraphSchema_K2::PC_Real; T.PinSubCategory = (Spec->TryGetStringField(TEXT("sub"), S) && S == TEXT("float")) ? UEdGraphSchema_K2::PC_Float : UEdGraphSchema_K2::PC_Double; }
		else if (Category == TEXT("bool")) { T.PinCategory = UEdGraphSchema_K2::PC_Boolean; }
		else if (Category == TEXT("int")) { T.PinCategory = UEdGraphSchema_K2::PC_Int; }
		else if (Category == TEXT("byte")) { T.PinCategory = UEdGraphSchema_K2::PC_Byte; }
		else if (Category == TEXT("name")) { T.PinCategory = UEdGraphSchema_K2::PC_Name; }
		else if (Category == TEXT("string")) { T.PinCategory = UEdGraphSchema_K2::PC_String; }
		else if (Category == TEXT("text")) { T.PinCategory = UEdGraphSchema_K2::PC_Text; }
		else { UE_LOG(LogBlueprintOracle, Error, TEXT("    unsupported category '%s'"), *Category); return false; }
		FString Container;
		if (Spec->TryGetStringField(TEXT("container"), Container))
		{
			if (Container == TEXT("Array")) { T.ContainerType = EPinContainerType::Array; }
			else if (Container == TEXT("Set")) { T.ContainerType = EPinContainerType::Set; }
		}
		return true;
	};

	if (OpName == TEXT("retypeParam"))
	{
		// Change a BP function's parameter type: locate the FunctionEntry (inputs) / FunctionResult
		// (outputs) editable pins by name, set the new type, ReconstructNode. NOTE: this changes the
		// function signature - the function's CALL SITES then need their (now type-mismatched) wires
		// fixed (splice converters/accessors) in the same migration for a clean compile.
		const FString GraphName = Op->GetStringField(TEXT("graph"));
		UEdGraph* Graph = FindGraphByName(Blueprint, GraphName);
		if (!Graph) { UE_LOG(LogBlueprintOracle, Error, TEXT("    retypeParam: graph '%s' not found"), *GraphName); return false; }
		const FName ParamName(*Op->GetStringField(TEXT("param")));
		FEdGraphPinType T;
		if (!BuildPinType(Op, T)) { return false; }
		int32 Changed = 0;
		for (UEdGraphNode* N : Graph->Nodes)
		{
			UK2Node_EditablePinBase* Editable = Cast<UK2Node_EditablePinBase>(N);
			if (!Editable) { continue; }
			bool bThisNode = false;
			for (TSharedPtr<FUserPinInfo>& P : Editable->UserDefinedPins)
			{
				if (P.IsValid() && P->PinName == ParamName) { P->PinType = T; ++Changed; bThisNode = true; }
			}
			if (bThisNode) { Editable->ReconstructNode(); }
		}
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
		UE_LOG(LogBlueprintOracle, Display, TEXT("    retypeParam '%s' in '%s': %d pin(s) retyped"), *ParamName.ToString(), *GraphName, Changed);
		return Changed > 0;
	}

	if (OpName == TEXT("addVar"))
	{
		// Add a member variable. Build the pin type from {category, struct?/class?, container?}.
		// category: bool/int/byte/real(+sub float/double)/name/string/text/struct/object/class/enum.
		const FString VarName = Op->GetStringField(TEXT("name"));
		const FString Category = Op->GetStringField(TEXT("category"));
		FEdGraphPinType T;
		if (Category == TEXT("struct"))
		{
			T.PinCategory = UEdGraphSchema_K2::PC_Struct;
			T.PinSubCategoryObject = ResolveStruct(Op->GetStringField(TEXT("struct")));
		}
		else if (Category == TEXT("object"))
		{
			T.PinCategory = UEdGraphSchema_K2::PC_Object;
			T.PinSubCategoryObject = ResolveClass(Op->GetStringField(TEXT("class")));
		}
		else if (Category == TEXT("class"))
		{
			T.PinCategory = UEdGraphSchema_K2::PC_Class;
			T.PinSubCategoryObject = ResolveClass(Op->GetStringField(TEXT("class")));
		}
		else if (Category == TEXT("real"))
		{
			T.PinCategory = UEdGraphSchema_K2::PC_Real;
			FString Sub; T.PinSubCategory = Op->TryGetStringField(TEXT("sub"), Sub) && Sub == TEXT("float")
				? UEdGraphSchema_K2::PC_Float : UEdGraphSchema_K2::PC_Double;
		}
		else if (Category == TEXT("bool")) { T.PinCategory = UEdGraphSchema_K2::PC_Boolean; }
		else if (Category == TEXT("int")) { T.PinCategory = UEdGraphSchema_K2::PC_Int; }
		else if (Category == TEXT("byte")) { T.PinCategory = UEdGraphSchema_K2::PC_Byte; }
		else if (Category == TEXT("name")) { T.PinCategory = UEdGraphSchema_K2::PC_Name; }
		else if (Category == TEXT("string")) { T.PinCategory = UEdGraphSchema_K2::PC_String; }
		else if (Category == TEXT("text")) { T.PinCategory = UEdGraphSchema_K2::PC_Text; }
		else
		{
			UE_LOG(LogBlueprintOracle, Error, TEXT("    addVar: unsupported category '%s'"), *Category);
			return false;
		}
		FString Container;
		if (Op->TryGetStringField(TEXT("container"), Container))
		{
			if (Container == TEXT("Array")) { T.ContainerType = EPinContainerType::Array; }
			else if (Container == TEXT("Set")) { T.ContainerType = EPinContainerType::Set; }
		}
		const bool bOk = FBlueprintEditorUtils::AddMemberVariable(Blueprint, FName(*VarName), T);
		// Optionally mark the new variable Expose-on-Spawn so it surfaces as a pin on
		// CreateWidget/SpawnActor/ConstructObject callers (instance-editable + CPF_ExposeOnSpawn +
		// the "ExposeOnSpawn" metadata, mirroring what the BP var editor's checkbox does).
		bool bExposeOnSpawn = false;
		if (bOk && Op->TryGetBoolField(TEXT("exposeOnSpawn"), bExposeOnSpawn) && bExposeOnSpawn)
		{
			const int32 VarIndex = FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, FName(*VarName));
			if (VarIndex != INDEX_NONE)
			{
				Blueprint->NewVariables[VarIndex].PropertyFlags &= ~CPF_DisableEditOnInstance;
				Blueprint->NewVariables[VarIndex].PropertyFlags |= CPF_ExposeOnSpawn | CPF_BlueprintVisible;
			}
			FBlueprintEditorUtils::SetBlueprintVariableMetaData(
				Blueprint, FName(*VarName), nullptr, FName(TEXT("ExposeOnSpawn")), TEXT("true"));
		}
		UE_LOG(LogBlueprintOracle, Display, TEXT("    addVar '%s' (%s%s%s) -> %s"),
			*VarName, *Category, T.ContainerType == EPinContainerType::Array ? TEXT("[]") : TEXT(""),
			bExposeOnSpawn ? TEXT(", exposeOnSpawn") : TEXT(""),
			bOk ? TEXT("ok") : TEXT("FAILED"));
		return bOk;
	}

	if (OpName == TEXT("removeNode") || OpName == TEXT("retargetVarRef"))
	{
		// Both locate a node by GUID within a named graph.
		const FString GraphName = Op->GetStringField(TEXT("graph"));
		UEdGraph* Graph = FindGraphByName(Blueprint, GraphName);
		if (!Graph)
		{
			UE_LOG(LogBlueprintOracle, Error, TEXT("    %s: graph '%s' not found"), *OpName, *GraphName);
			return false;
		}
		const FString NodeGuid = Op->GetStringField(TEXT("node"));
		UEdGraphNode* Target = nullptr;
		for (UEdGraphNode* N : Graph->Nodes)
		{
			if (N && N->NodeGuid.ToString(EGuidFormats::Digits) == NodeGuid) { Target = N; break; }
		}
		if (!Target)
		{
			UE_LOG(LogBlueprintOracle, Error, TEXT("    %s: node '%s' not found in '%s'"), *OpName, *NodeGuid, *GraphName);
			return false;
		}

		if (OpName == TEXT("removeNode"))
		{
			Graph->RemoveNode(Target);
			UE_LOG(LogBlueprintOracle, Display, TEXT("    removeNode '%s' from '%s'"), *NodeGuid, *GraphName);
			return true;
		}

		// retargetVarRef: point a VariableGet/Set at a different member (e.g. InventoryItem_A -> Items),
		// then ReconstructNode so the value pin takes the new variable's type. Downstream wildcard
		// nodes (GetArrayItem) follow via their own reconstruct on link change.
		UK2Node_Variable* VarNode = Cast<UK2Node_Variable>(Target);
		if (!VarNode)
		{
			UE_LOG(LogBlueprintOracle, Error, TEXT("    retargetVarRef: node '%s' is not a variable node"), *NodeGuid);
			return false;
		}
		const FString ToVar = Op->GetStringField(TEXT("toVar"));
		FString ToClassPath;
		if (Op->TryGetStringField(TEXT("toClass"), ToClassPath) && !ToClassPath.IsEmpty())
		{
			VarNode->VariableReference.SetExternalMember(FName(*ToVar), ResolveClass(ToClassPath));
		}
		else
		{
			VarNode->VariableReference.SetSelfMember(FName(*ToVar));
		}
		VarNode->ReconstructNode();
		UE_LOG(LogBlueprintOracle, Display, TEXT("    retargetVarRef '%s' -> %s"), *NodeGuid, *ToVar);
		return true;
	}

	UE_LOG(LogBlueprintOracle, Error, TEXT("    unknown op '%s'"), *OpName);
	return false;
}

int32 UBlueprintOracleCommandlet::RunMigration(const FString& SpecPath, bool bCommit)
{
	UE_LOG(LogBlueprintOracle, Display, TEXT("BlueprintOracle migration: spec=%s  mode=%s"),
		*SpecPath, bCommit ? TEXT("COMMIT") : TEXT("DRY-RUN"));

	FString SpecText;
	if (!FFileHelper::LoadFileToString(SpecText, *SpecPath))
	{
		UE_LOG(LogBlueprintOracle, Error, TEXT("Cannot read spec file %s"), *SpecPath);
		return 1;
	}

	TSharedPtr<FJsonObject> SpecRoot;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(SpecText);
	if (!FJsonSerializer::Deserialize(Reader, SpecRoot) || !SpecRoot.IsValid())
	{
		UE_LOG(LogBlueprintOracle, Error, TEXT("Spec file %s is not valid JSON"), *SpecPath);
		return 1;
	}

	const TArray<TSharedPtr<FJsonValue>>* Assets = nullptr;
	if (!SpecRoot->TryGetArrayField(TEXT("assets"), Assets))
	{
		UE_LOG(LogBlueprintOracle, Error, TEXT("Spec has no 'assets' array"));
		return 1;
	}

	int32 FailedAssets = 0;
	int32 SavedAssets = 0;

	for (const TSharedPtr<FJsonValue>& AssetVal : *Assets)
	{
		const TSharedPtr<FJsonObject> AssetObj = AssetVal->AsObject();
		if (!AssetObj.IsValid())
		{
			continue;
		}
		const FString PackageName = AssetObj->GetStringField(TEXT("package"));
		UE_LOG(LogBlueprintOracle, Display, TEXT("--- %s ---"), *PackageName);

		UPackage* Package = LoadPackage(nullptr, *PackageName, LOAD_None);
		if (!Package)
		{
			UE_LOG(LogBlueprintOracle, Error, TEXT("  could not load package"));
			++FailedAssets;
			continue;
		}
		Package->FullyLoad();

		UBlueprint* Blueprint = nullptr;
		{
			TArray<UObject*> Objects;
			GetObjectsWithOuter(Package, Objects, /*bIncludeNestedObjects*/ false);
			for (UObject* Obj : Objects)
			{
				Blueprint = Cast<UBlueprint>(Obj);
				if (Blueprint) { break; }
			}
		}
		if (!Blueprint)
		{
			UE_LOG(LogBlueprintOracle, Error, TEXT("  no blueprint in package"));
			++FailedAssets;
			continue;
		}

		// Apply ops in order.
		bool bOpsOk = true;
		const TArray<TSharedPtr<FJsonValue>>* Ops = nullptr;
		if (AssetObj->TryGetArrayField(TEXT("ops"), Ops))
		{
			for (const TSharedPtr<FJsonValue>& OpVal : *Ops)
			{
				const TSharedPtr<FJsonObject> Op = OpVal->AsObject();
				if (!Op.IsValid()) { continue; }
				if (!ApplyMigrationOp(Blueprint, Op))
				{
					bOpsOk = false;
					break;
				}
			}
		}

		if (!bOpsOk)
		{
			UE_LOG(LogBlueprintOracle, Error, TEXT("  op application failed; not saving"));
			++FailedAssets;
			continue;
		}

		// Compile-gate.
		FBlueprintEditorUtils::RefreshAllNodes(Blueprint);
		FCompilerResultsLog Results;
		FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::None, &Results);
		const bool bClean = (Blueprint->Status == BS_UpToDate || Blueprint->Status == BS_UpToDateWithWarnings)
			&& Results.NumErrors == 0;

		UE_LOG(LogBlueprintOracle, Display, TEXT("  compile: %d error(s), %d warning(s) -> %s"),
			Results.NumErrors, Results.NumWarnings, bClean ? TEXT("CLEAN") : TEXT("FAILED"));
		for (const TSharedRef<FTokenizedMessage>& Msg : Results.Messages)
		{
			UE_LOG(LogBlueprintOracle, Warning, TEXT("    [%s] %s"),
				Msg->GetSeverity() == EMessageSeverity::Error ? TEXT("ERR") : TEXT("WARN"),
				*Msg->ToText().ToString());
		}

		if (!bClean)
		{
			++FailedAssets;
			continue;
		}

		if (bCommit)
		{
			Package->MarkPackageDirty();
			const FString FileName = FPackageName::LongPackageNameToFilename(
				Package->GetName(), FPackageName::GetAssetPackageExtension());
			// Content is often read-only (source-control working copy); clear it so the
			// save can overwrite. The caller opted into mutation via -commit.
			FPlatformFileManager::Get().GetPlatformFile().SetReadOnly(*FileName, false);
			FSavePackageArgs SaveArgs;
			SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
			SaveArgs.SaveFlags = SAVE_NoError;
			const FSavePackageResultStruct Result = UPackage::Save(Package, nullptr, *FileName, SaveArgs);
			if (Result.IsSuccessful())
			{
				UE_LOG(LogBlueprintOracle, Display, TEXT("  SAVED %s"), *FileName);
				++SavedAssets;
			}
			else
			{
				UE_LOG(LogBlueprintOracle, Error, TEXT("  SAVE FAILED %s"), *FileName);
				++FailedAssets;
			}
		}
		else
		{
			UE_LOG(LogBlueprintOracle, Display, TEXT("  dry-run: compiled clean (not saved)"));
		}
	}

	UE_LOG(LogBlueprintOracle, Display, TEXT("Migration done: %d asset(s) failed, %d saved."),
		FailedAssets, SavedAssets);
	return FailedAssets == 0 ? 0 : 1;
}

int32 UBlueprintOracleCommandlet::RunSelfTest(const FString& OutDir)
{
	int32 Failures = 0;
	auto Check = [&Failures](bool bCond, const TCHAR* Desc)
	{
		if (bCond)
		{
			UE_LOG(LogBlueprintOracle, Display, TEXT("  [PASS] %s"), Desc);
		}
		else
		{
			UE_LOG(LogBlueprintOracle, Error, TEXT("  [FAIL] %s"), Desc);
			++Failures;
		}
	};

	UE_LOG(LogBlueprintOracle, Display, TEXT("BlueprintOracle self-test (programmatic editing) ..."));

	// 1. Create a disposable blueprint (transient package, never saved to disk).
	UPackage* Pkg = CreatePackage(TEXT("/Temp/__BPOracleSelfTest"));
	Pkg->SetFlags(RF_Transient);
	UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(
		AActor::StaticClass(), Pkg, FName("BP_OracleSelfTest"),
		BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
	Check(Blueprint != nullptr, TEXT("create blueprint"));
	if (!Blueprint)
	{
		return 1;
	}

	// 2. Add a float member variable.
	FEdGraphPinType FloatType;
	FloatType.PinCategory = UEdGraphSchema_K2::PC_Real;
	FloatType.PinSubCategory = UEdGraphSchema_K2::PC_Float;
	const bool bAddedVar = FBlueprintEditorUtils::AddMemberVariable(Blueprint, FName("TestFloat"), FloatType);
	Check(bAddedVar, TEXT("add member variable 'TestFloat'"));

	// 3. Reparent AActor -> APawn.
	Blueprint->ParentClass = APawn::StaticClass();
	FBlueprintEditorUtils::RefreshAllNodes(Blueprint);
	Check(Blueprint->ParentClass == APawn::StaticClass(), TEXT("reparent to APawn"));

	// 4. Spawn a VariableGet(TestFloat) and a CallFunction(Abs), wire get -> Abs.A.
	UEdGraph* EventGraph = (Blueprint->UbergraphPages.Num() > 0) ? Blueprint->UbergraphPages[0] : nullptr;
	Check(EventGraph != nullptr, TEXT("event graph exists"));
	if (!EventGraph)
	{
		return 1;
	}

	UK2Node_VariableGet* GetNode = nullptr;
	{
		FGraphNodeCreator<UK2Node_VariableGet> Creator(*EventGraph);
		GetNode = Creator.CreateNode();
		GetNode->VariableReference.SetSelfMember(FName("TestFloat"));
		GetNode->NodePosX = 0;
		GetNode->NodePosY = 400;
		Creator.Finalize();
	}
	UK2Node_CallFunction* CallNode = nullptr;
	{
		FGraphNodeCreator<UK2Node_CallFunction> Creator(*EventGraph);
		CallNode = Creator.CreateNode();
		CallNode->FunctionReference.SetExternalMember(FName("Abs"), UKismetMathLibrary::StaticClass());
		CallNode->NodePosX = 300;
		CallNode->NodePosY = 400;
		Creator.Finalize();
	}
	Check(GetNode != nullptr && CallNode != nullptr, TEXT("spawn VariableGet + CallFunction nodes"));

	UEdGraphPin* GetOut = GetNode ? GetNode->FindPin(FName("TestFloat"), EGPD_Output) : nullptr;
	UEdGraphPin* AbsA = CallNode ? CallNode->FindPin(FName("A"), EGPD_Input) : nullptr;
	Check(GetOut != nullptr && AbsA != nullptr, TEXT("locate value pins (TestFloat out, Abs.A in)"));
	if (GetOut && AbsA)
	{
		GetOut->MakeLinkTo(AbsA);
		Check(AbsA->LinkedTo.Contains(GetOut), TEXT("wire TestFloat -> Abs.A"));
	}

	// 5. THE AUTO-REWIRE: redirect the call Abs -> Sqrt (same param 'A'),
	//    reconstruct, and confirm the wire reconnected purely by pin name.
	if (CallNode)
	{
		CallNode->FunctionReference.SetExternalMember(FName("Sqrt"), UKismetMathLibrary::StaticClass());
		CallNode->ReconstructNode();
		UEdGraphPin* SqrtA = CallNode->FindPin(FName("A"), EGPD_Input);
		UEdGraphPin* GetOutAfter = GetNode ? GetNode->FindPin(FName("TestFloat"), EGPD_Output) : nullptr;
		Check(SqrtA != nullptr && GetOutAfter != nullptr && SqrtA->LinkedTo.Contains(GetOutAfter),
			TEXT("AUTO-REWIRE: TestFloat -> A wire survived Abs->Sqrt redirect + ReconstructNode"));
		Check(CallNode->FunctionReference.GetMemberName() == FName("Sqrt"),
			TEXT("call node now targets Sqrt"));
	}

	// 6. Compile.
	FKismetEditorUtilities::CompileBlueprint(Blueprint);
	Check(Blueprint->Status == BS_UpToDate || Blueprint->Status == BS_UpToDateWithWarnings,
		TEXT("compile blueprint clean"));

	// 7. Read it back through the oracle's own graph dump (closes the edit->read loop).
	WriteGraphs(Blueprint, TEXT("BP_OracleSelfTest"), OutDir);
	FString GraphJson;
	const bool bRead = FFileHelper::LoadFileToString(GraphJson, *(OutDir / TEXT("BP_OracleSelfTest.graph.json")));
	Check(bRead, TEXT("read back graph.json"));
	Check(GraphJson.Contains(TEXT("Pawn")), TEXT("read-back reflects reparent to Pawn"));
	Check(GraphJson.Contains(TEXT("Sqrt")), TEXT("read-back reflects redirected call (Sqrt)"));

	UE_LOG(LogBlueprintOracle, Display, TEXT("Self-test complete: %d failure(s)."), Failures);
	return Failures == 0 ? 0 : 1;
}
