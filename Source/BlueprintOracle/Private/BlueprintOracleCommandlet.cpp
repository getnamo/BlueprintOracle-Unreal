// Copyright 2026-current Getnamo. All Rights Reserved.

#include "BlueprintOracleCommandlet.h"

#include "Engine/Blueprint.h"
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

// Write-side (programmatic blueprint editing) APIs.
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "EdGraphSchema_K2.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Kismet/KismetMathLibrary.h"
#include "GameFramework/Pawn.h"

#include "ScriptDisassembler.h"

#include "UObject/Linker.h"
#include "UObject/LinkerLoad.h"
#include "UObject/UObjectHash.h"
#include "UObject/UnrealType.h"

#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/PackageName.h"
#include "HAL/FileManager.h"

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
