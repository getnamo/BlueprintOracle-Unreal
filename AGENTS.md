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
