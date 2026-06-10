// Copyright 2026-current Getnamo. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "BlueprintOracleCommandlet.generated.h"

class UBlueprint;
class UEdGraph;
class UPackage;
class UUserDefinedStruct;
class UUserDefinedEnum;

/**
 * BlueprintOracle. For each requested Blueprint asset, emits four ground-truth artifacts to a chosen
 * output dir for static analysis, documentation, and agent-assisted Blueprint-to-C++ conversion:
 *
 *   <Asset>.layout.json  - linker export/import table as the engine sees it (name, class, SerialOffset,
 *                          SerialSize) so raw .uasset bytes can be aligned to decoded objects.
 *   <Asset>.graph.json   - walk of every UEdGraph: nodes (guid/class/title/func ref) + pins
 *                          (name/dir/type/default/LinkedTo guids), plus variables, components, parent,
 *                          interfaces. This is the primary structured-logic output.
 *   <Asset>.nodes.txt    - FEdGraphUtilities::ExportNodesToText per graph (clipboard T3D).
 *   <Asset>.disasm.txt   - FKismetBytecodeDisassembler per UFunction (compiled bytecode).
 *
 * Run headless, e.g.:
 *   UnrealEditor-Cmd.exe <project> -run=BlueprintOracle -dir=/Game/Blueprints
 *       -out="C:/path/to/out" -unattended -nullrhi -nosplash -nopause -log
 *
 *   -dir=/MountPoint        enumerate every Blueprint under a content path (via the asset registry).
 *   -asset=/Path/A,/Path/B  process specific blueprint package paths.
 * At least one of -dir or -asset is required. Output is UTF-8.
 */
UCLASS()
class UBlueprintOracleCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UBlueprintOracleCommandlet();

	//~ Begin UCommandlet interface
	virtual int32 Main(const FString& Params) override;
	//~ End UCommandlet interface

private:
	/** Load one package, find its UBlueprint, and emit all artifacts. */
	void ProcessAsset(const FString& PackageName, const FString& OutDir);

	/** Dump the linker export/import tables (with byte offsets) for a loaded package. */
	void WriteLayout(UPackage* Package, const FString& OutPath);

	/** Dump per-graph node/pin JSON and ExportNodesToText. */
	void WriteGraphs(UBlueprint* Blueprint, const FString& AssetName, const FString& OutDir);

	/** Dump Kismet bytecode disassembly for every UFunction in the generated class. */
	void WriteDisasm(UBlueprint* Blueprint, const FString& OutPath);

	/** Dump a UserDefinedStruct's fields (name + cpp type + referenced type). */
	void WriteStruct(UUserDefinedStruct* Struct, const FString& OutPath);

	/** Dump a UserDefinedEnum's entries (authored name, display name, value). */
	void WriteEnum(UUserDefinedEnum* Enum, const FString& OutPath);
};
