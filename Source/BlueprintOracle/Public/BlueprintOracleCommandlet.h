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

	/**
	 * Apply a JSON migration spec: a list of assets, each with an ordered list of
	 * structural edit ops (reparent / removeGraph / removeVar / redirectCall /
	 * replaceVarRefs). Each asset is edited in memory, RefreshAllNodes'd, then
	 * compile-gated: only on a clean compile (and only when bCommit is true) is the
	 * package saved to disk. With bCommit=false it is a dry run - it reports the
	 * compile result of every asset without touching any .uasset. Returns 0 if every
	 * asset compiled clean, non-zero otherwise.
	 */
	int32 RunMigration(const FString& SpecPath, bool bCommit);

	/** Apply one op object to an already-loaded blueprint. Returns false on a hard error. */
	bool ApplyMigrationOp(UBlueprint* Blueprint, const TSharedPtr<class FJsonObject>& Op);

	/**
	 * Proof-of-concept for programmatic blueprint editing. Builds a disposable
	 * in-memory blueprint and exercises the write-side APIs end-to-end:
	 * create, add variable, reparent, spawn + wire a function-call node, redirect
	 * the call to a different function (auto-rewire via ReconstructNode), compile,
	 * and read it back via the oracle's own graph dump. Touches no live assets.
	 * Returns 0 if every check passes, 1 otherwise.
	 */
	int32 RunSelfTest(const FString& OutDir);
};
