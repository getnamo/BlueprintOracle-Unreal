# BlueprintOracle — agent guide: converting a Blueprint to C++

This plugin turns a binary Blueprint into clean JSON/text you can read and reason
about. This document is the playbook for an AI agent (or a human) to go from a
`.uasset` Blueprint to an equivalent C++ class.

The golden rule: **the engine already did the hard part.** BlueprintOracle loads
each Blueprint with Unreal itself, so the graph it gives you is correct and
version-accurate. Do not try to parse `.uasset` bytes yourself — run the oracle and
read its output.

---

## Step 0 — Build & run the oracle

It's an `UncookedOnly` editor module, so it compiles with your editor target.

```
UnrealEditor-Cmd.exe <YourProject>.uproject -run=BlueprintOracle \
    -dir=/Game/Path/To/Blueprints \
    -out="<some output dir>" \
    -unattended -nullrhi -nosplash -nopause -log
```

- `-dir=/MountPoint` — every Blueprint under a content path (recursive, registry-filtered).
- `-asset=/Game/Foo/BP_A,/Game/Bar/BP_B` — specific blueprints (package paths, comma-separated).
- Editor cold-start is ~1–2 min; each Blueprint then takes ~0.3–1 s. **Batch many per run.**

For each `<Asset>` you get four UTF-8 files. Use them in this priority:

| File | Use it for |
|------|-----------|
| `<Asset>.graph.json` | **Everything.** Class shape + full per-graph logic. This is what you read. |
| `<Asset>.disasm.txt` | Cross-check exact runtime behavior / operator semantics when a graph node is ambiguous. |
| `<Asset>.nodes.txt`  | Human eyeball of a single graph (T3D), for spot checks. |
| `<Asset>.layout.json`| Rarely needed; low-level linker tables. |

---

## Step 1 — Read the class shape from `graph.json`

```jsonc
{
  "asset": "BP_Foo",
  "parentClass": "/Script/Engine.Actor",          // C++ base to inherit from
  "generatedClass": "BP_Foo_C",
  "interfaces": ["/Script/Mod.SomeInterface"],     // -> public ISomeInterface
  "variables": [
    { "name": "Health", "category": "Stats",
      "type": { "category": "float", "container": "None" }, "defaultValue": "100.0" }
  ],
  "components": [
    { "variableName": "Mesh", "componentClass": "/Script/Engine.StaticMeshComponent",
      "parentAttachment": "" }
  ],
  "graphs": [ { "name": "...", "nodes": [ ... ] } ]
}
```

Map these directly:

- **`parentClass`** → the C++ `class AYourClass : public <ParentCpp>`. If the parent is
  itself a Blueprint (`.../X_C`), port that first or derive from its eventual C++ base.
- **`interfaces`** → add the `I...` interface(s) to the class and implement their members.
- **`variables[]`** → `UPROPERTY()` members. Translate `type`:
  - `category` is the pin category: `bool`, `int`, `int64`, `real`(+`subCategory` `float`/`double`),
    `byte`, `name`, `string`, `text`, `struct`, `object`/`class`/`softobject`(+`subCategoryObject` = the type),
    `enum`.
  - `container`: `None` → scalar, `Array` → `TArray<>`, `Set` → `TSet<>`, `Map` → `TMap<>`.
  - `subCategoryObject` is the fully-qualified type for struct/object/enum pins.
  - A `mcdelegate` variable → `DECLARE_DYNAMIC_MULTICAST_DELEGATE` + `UPROPERTY(BlueprintAssignable)`.
- **`components[]`** → `CreateDefaultSubobject<...>(TEXT("<variableName>"))` in the constructor;
  `parentAttachment` tells you what to `SetupAttachment` to (empty = root).

---

## Step 2 — Reconstruct each function/graph

Every graph in `graphs[]` is a node list. Each node:

```jsonc
{
  "guid": "….",               // unique id; pins reference nodes by this
  "class": "K2Node_CallFunction",
  "title": "Set Static Mesh",
  "memberName": "SetStaticMesh", // function/variable/event name, when applicable
  "posX": 0, "posY": 0, "comment": "",
  "pins": [
    { "id": "…", "name": "Mesh", "direction": "Input",
      "type": { "category": "object", "subCategoryObject": "/Script/Engine.StaticMesh" },
      "defaultValue": "", "defaultObject": "", "defaultText": "",
      "linkedTo": [ { "node": "<other node guid>", "pin": "<other pin id>" } ] }
  ]
}
```

**Connectivity model:** a wire is `linkedTo` — it names the *other* node's guid and
the *other* pin's id. Build two indexes once: `node_by_guid` and `pin_by_id`.

**Control flow (execution):** pins with `type.category == "exec"`.
1. Find the entry node: `class` in `K2Node_FunctionEntry` (functions), `K2Node_Event` /
   `K2Node_CustomEvent` (event graph). Its Output exec pin starts the chain.
2. Follow the Output exec pin's `linkedTo` to the next node's Input exec pin; emit that
   node as a statement; repeat from its Output exec pin.
3. Branches (`K2Node_IfThenElse`) have `Then`/`Else` exec outputs; `K2Node_ExecutionSequence`
   has `Then0..N` (run in order); loops feed an exec pin back upstream.

**Data flow (expressions):** non-exec pins. To get an Input pin's value:
- If it has `linkedTo`, recurse into the source Output pin's node and build an expression.
- Else use the literal: `defaultObject` (asset/object), else `defaultValue`, else `defaultText`.

**Common node classes → C++:**

| K2Node class | Meaning |
|---|---|
| `K2Node_CallFunction` / `CallParentFunction` | `memberName(args...)` (args = non-exec input pins). Parent call → `Super::memberName(...)`. |
| `K2Node_VariableGet` / `VariableSet` | read / `memberName = value;` |
| `K2Node_Event` / `CustomEvent` | an entry point; map to a `UFUNCTION` (override or `BlueprintImplementableEvent`/native event). |
| `K2Node_IfThenElse` | `if (Condition) {Then} else {Else}` |
| `K2Node_ExecutionSequence` | run `Then0`, `Then1`, … in order |
| `K2Node_DynamicCast` | `Cast<T>(obj)` + branch on success |
| `K2Node_Knot` | reroute — pass straight through (follow its single input) |
| `K2Node_MakeStruct` / `BreakStruct` | struct literal / member access (`s.Field`) |
| `K2Node_VariableGet` of a component | the component pointer |
| `K2Node_CallDelegate` | `Delegate.Broadcast(...)` |
| `K2Node_GetArrayItem` / `CallArrayFunction` | array indexing / `UKismetArrayLibrary` calls |
| `K2Node_Self` | `this` |
| `K2Node_Timeline`, `K2Node_*Delay`, latent calls | latent/async — needs timers/timelines/latent actions in C++, not a straight line. Flag these. |

The function's signature comes from the entry node's Output pins (parameters) and the
`K2Node_FunctionResult` node's Input pins (return values / out-params).

---

## Step 3 — Worked example

Take a `BP_Lamp` actor (parent `Actor`, a `PointLightComponent` named `LampLight`, a
`bool bIsPowered`, and a struct variable `LampSettings`). Its `SetPowered(bool NewState)`
function `graph.json` reduces (entry → exec chain, data pins resolved through Knots and a
BreakStruct) to:

```
bIsPowered = NewState;
LampLight->SetIntensity(LampSettings.Brightness);
LampLight->SetVisibility(NewState);
```

→ C++:

```cpp
void ABP_Lamp::SetPowered(bool NewState)
{
    bIsPowered = NewState;
    LampLight->SetIntensity(LampSettings.Brightness);
    LampLight->SetVisibility(NewState);
}
```

A small reference reconstructor that walks `graph.json` into pseudocode like the above
is straightforward (~150 lines): index pins by id, follow exec pins for statements and
data pins for expressions, special-casing the node table above. Build one once and reuse
it across every Blueprint.

---

## Step 4 — Verify

- **Compile** the generated C++ against your editor target; fix types/signatures.
- For any node whose meaning you're unsure of (operators, conversions, enum equality),
  open `<Asset>.disasm.txt` and read the bytecode for that function — it's the exact
  executed semantics.
- Behaviorally diff against the original Blueprint in-editor where feasible.

---

## Gotchas (read before trusting a name)

- **Core Redirects.** A `.uasset` stores the names it had when last saved. The engine
  applies `+ClassRedirects` / `+FunctionRedirects` / `+PropertyRedirects` from
  `Config/*.ini` at load time. The oracle output reflects the **post-load** (current)
  names, which is what you want — but if you also read raw bytes elsewhere, resolve
  through the project's redirects first.
- **Defaults aren't serialized.** A variable/pin only appears with a value if it differs
  from the class default; absent = default (0 / empty / false).
- **Pure vs impure nodes.** Pure nodes (no exec pins — most getters, math, casts-as-pure)
  are pulled in as expressions wherever their output is used, possibly multiple times.
- **Macros are expanded.** Blueprint macros (e.g. `ForEachLoop`, `IsValid`) appear as
  their expanded `K2Node_*` nodes (Knots, branches, sequences), not a single call.
- **Latent / timeline / event-driven flow** does not map to a straight function body —
  flag it for manual handling (timers, timelines, latent actions, delegates).
- **Multiple graphs share state** via member variables; reconstruct per-graph, then the
  class holds the variables.

---

## Write side — scripting the migration (editing blueprints)

Reading a BP gets you the C++. The other half of a migration is *changing the blueprints* that
call it — reattaching call sites to the new C++, remapping params, reparenting, deleting graphs
that C++ now owns. The oracle can do all of this programmatically (the verified API + the
`-selftest` proof are in the README). These are the load-bearing moves. Each runs in the editor
commandlet; **compile-gate and save after each asset** (see the end).

### Move 1 — Reattach a BP function call to a C++ function (auto-rewire)

The most common move: a graph calls a Blueprint function (or BP library) whose logic now lives in
C++. If the C++ function uses the **same parameter names and compatible pin types**, the wires
reconnect themselves — no manual rewiring:

```cpp
// Walk every graph of the blueprint; redirect matching call nodes.
if (auto* Call = Cast<UK2Node_CallFunction>(Node))
    if (Call->FunctionReference.GetMemberName() == TEXT("DoThingBP"))
    {
        Call->FunctionReference.SetExternalMember(TEXT("DoThing"), UMyComponentBase::StaticClass());
        Call->ReconstructNode();   // rebuilds pins from the new function; reconnects wires by name
    }
```

Implication for the *C++* side: **name the new C++ functions and parameters to mirror the BP ones**
during migration. That single discipline turns every call-site rewire into the one-liner above.
(Renames can come later via `+FunctionRedirects` once the dust settles.)

### Move 2 — Redirect across a changed signature / struct param (pin remap + converters)

When the signature differs — e.g. the BP function took `S_LegacyData` but the C++ one
takes `FMyData` — `ReconstructNode` drops the mismatched pins. Remap explicitly, splicing a
**converter node** where a *type* changed:

```cpp
// After redirect+reconstruct, matching pins are reconnected; the changed param pin is empty.
// Insert a converter between the original source and the new param:
FGraphNodeCreator<UK2Node_CallFunction> C(*Graph);
UK2Node_CallFunction* Conv = C.CreateNode();
Conv->FunctionReference.SetExternalMember(TEXT("MakeData"), UMyCompatLibrary::StaticClass());
C.Finalize();
OldSourcePin->MakeLinkTo(Conv->FindPin(TEXT("LegacyField"), EGPD_Input));          // feed the converter
Conv->FindPin(TEXT("ReturnValue"), EGPD_Output)->MakeLinkTo(NewCall->FindPin(TEXT("Item"), EGPD_Input));
```

Converter choices for this project's item migration:
- **Enum param changed** (e.g. `E_Foo` pin → an `EFoo` pin): splice the matching bridge
  in `UMyCompatLibrary` (`LegacyToNew`, `LegacyToNew`, …).
- **Whole item struct changed** (`S_LegacyData` → `FMyData`): the BP shim *breaks*
  the legacy struct (BP UserDefinedStructs have no C++ header, so they can't be passed whole to
  C++) and feeds the fields into `UMyCompatLibrary::MakeData` + the `Make*TypeData`
  builders. The reverse (`FMyData` → legacy) is a native BP BreakStruct (it *is* a C++
  BlueprintType) plus the enum bridges.

### Move 3 — Reparent onto a C++ base + strip migrated graphs

Once C++ owns a function, delete its BP graph; once a BP should derive from a C++ base, reparent:

```cpp
Blueprint->ParentClass = UMyComponentBase::StaticClass();   // Phase 3: BP_MyActor onto C++ base
FBlueprintEditorUtils::RefreshAllNodes(Blueprint);
FBlueprintEditorUtils::RemoveGraph(Blueprint, MigratedFunctionGraph);   // drop the now-C++ function
FKismetEditorUtilities::CompileBlueprint(Blueprint);
```

### Move 4 — Swap a member variable's type / storage

```cpp
FEdGraphPinType T;
T.PinCategory = UEdGraphSchema_K2::PC_Struct;
T.PinSubCategoryObject = FMyData::StaticStruct();
T.ContainerType = EPinContainerType::Array;
FBlueprintEditorUtils::RemoveMemberVariable(Blueprint, TEXT("LegacyArray"));  // S_LegacyData[]
FBlueprintEditorUtils::AddMemberVariable(Blueprint, TEXT("Items"), T);            // FMyData[]
FBlueprintEditorUtils::ReplaceVariableReferences(Blueprint, TEXT("LegacyArray"), TEXT("Items"));
```
(References whose pin *type* also changed still need Move-2 converters at their use sites.)

### Always — verify the edit (closed loop)

An edit is "done" only when both hold:
1. `FKismetEditorUtilities::CompileBlueprint` is clean — **a failed compile is a hard stop; never
   `SavePackages` a non-compiling blueprint.**
2. Re-run the *read* path on the same BP and diff the new `graph.json` against the intended result.

Run on a branch and commit per asset (or per op batch) so any bad edit is trivially reverted. Use
only the non-UI APIs (avoid `Open*Menu` variants) so it stays headless.
