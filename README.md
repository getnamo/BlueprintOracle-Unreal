# BlueprintOracle-Unreal

An Unreal Engine editor commandlet that dumps **ground-truth Blueprint data to JSON/text** for static
analysis, documentation, and **agent-assisted Blueprint-to-C++ conversion**.

Blueprints are binary visual graphs, which are awkward to diff, review, or feed to an LLM. BlueprintOracle
loads each Blueprint with the engine itself (so deserialization is always correct and version-accurate) and
exports a clean, text-friendly representation of its full logic.

[Discord Server](https://discord.gg/qfJUyxaW4s)

## What it emits

Per Blueprint, into your chosen output directory (all UTF-8):

| File | Contents |
|------|----------|
| `<Asset>.graph.json` | **Primary.** Every graph's nodes + pins + full `LinkedTo` connectivity, plus parent class, implemented interfaces, typed variables, and components. The structured logic source. |
| `<Asset>.layout.json` | Linker export/import tables with byte offsets (for low-level tooling / cross-referencing the raw `.uasset`). |
| `<Asset>.nodes.txt` | `FEdGraphUtilities::ExportNodesToText` per graph — the clipboard T3D representation. |
| `<Asset>.disasm.txt` | `FKismetBytecodeDisassembler` output per compiled `UFunction` — the executed bytecode. |

## Usage

It's an `UncookedOnly` editor module, so it builds with your editor target and runs headless:

```
UnrealEditor-Cmd.exe <YourProject>.uproject -run=BlueprintOracle ^
    -dir=/Game/Blueprints ^
    -out="C:/path/to/output" ^
    -unattended -nullrhi -nosplash -nopause -log
```

Arguments:

- `-dir=/MountPoint` — enumerate every Blueprint under a content path via the asset registry (skips
  textures/meshes/data assets automatically). Recursive.
- `-asset=/Path/A,/Path/B` — process specific Blueprint package paths.
- `-out=<dir>` — output directory (defaults to `<Project>/Saved/BlueprintOracle`).

At least one of `-dir` / `-asset` is required.

Performance: editor cold-start dominates (~1–2 min once); each Blueprint then extracts in roughly
0.3–1 s, so batch as many as possible into a single invocation.

It also dumps **UserDefinedStruct** and **UserDefinedEnum** assets (`<Asset>.struct.json` /
`<Asset>.enum.json`) — field names + C++ types for structs, and authored name + display name +
value for enum entries (which resolves Blueprint's opaque `NewEnumeratorN` names).

## Converting a Blueprint to C++

See **[AGENTS.md](AGENTS.md)** for the step-by-step playbook (for an AI agent or a
human): how to run the oracle, read `graph.json`, map nodes/pins/variables/components to
C++, and the gotchas (Core Redirects, unserialized defaults, macros, latent flow).

## Editing blueprints programmatically (write side)

The oracle runs in full editor context, so it can also **edit** blueprints the way the editor
does — driven from a commandlet instead of by hand. This makes bulk refactors and BP→C++
migrations scriptable and verifiable: edit, compile, then re-run the read path and diff the
`graph.json` against the intended result.

Run the built-in proof end-to-end (creates a disposable in-memory blueprint, exercises the
APIs, and reads it back — touches no live assets):

```
UnrealEditor-Cmd.exe <Project>.uproject -run=BlueprintOracle -selftest -unattended -nullrhi -nosplash -nopause -log
```

It reports `[PASS]/[FAIL]` for each step and returns non-zero on any failure.

### Declarative migrations (`-migrate`)

The practical interface for editing is a **compile-gated, declarative migration spec** — a JSON list of
assets, each with an ordered list of ops. Each asset is loaded, its ops applied, `RefreshAllNodes`'d, then
compiled; it is saved only on **0 errors** and only with `-commit`. Dry-run (the default) reports the
compile result without touching disk.

```
UnrealEditor-Cmd.exe <Project>.uproject -run=BlueprintOracle -migrate -spec="<path>.json" \
    -unattended -nullrhi -nosplash -nopause -log        # dry run: edit in memory, compile, report
    ... -migrate -spec="<path>.json" -commit             # write .uasset(s), only if they compiled clean
```

```jsonc
{ "assets": [ { "package": "/Game/Blueprints/BP_MyComponent", "ops": [
    { "op": "removeGraph", "name": "DoThingBP" },
    { "op": "reparent", "newParent": "/Script/MyModule.MyComponentBase" }
] } ] }
```

Ops include: `reparent`, `removeGraph`, `removeVar`, `replaceVarRefs`, `redirectCall`, `setCallPinDefault`,
`addVar`, `removeNode`, `retargetVarRef`, `connectPins`, `retypeParam`, `spliceCall` (drive a pin from a
new call), and `buildBody` (author/extend a function body from a node+link spec — kinds `callFunction` /
`breakStruct` / `makeStruct` / `callDelegate` / `cast` / `branch` / `variableGet`, with a replace or
`append` mode). A separate `-settablefield` mode edits a DataTable row's field. **The full op reference,
spec format, and the read→edit→verify workflow are in [`AGENTS.md`](AGENTS.md).**

### Verified editing API (UE 5.5)

| Operation | API |
|---|---|
| Create a blueprint | `FKismetEditorUtilities::CreateBlueprint` |
| Reparent to a new base class | set `Blueprint->ParentClass`, then `FBlueprintEditorUtils::RefreshAllNodes` + `CompileBlueprint` |
| Add / remove a member variable | `FBlueprintEditorUtils::AddMemberVariable` / `RemoveMemberVariable` |
| Bulk re-point variable references | `FBlueprintEditorUtils::ReplaceVariableReferences` |
| Add / remove an interface | `FBlueprintEditorUtils::ImplementNewInterface` / `RemoveInterface` |
| Delete a function graph / a node | `FBlueprintEditorUtils::RemoveGraph` / `RemoveNode` |
| Spawn a node | `FGraphNodeCreator<NodeType>` (or `FEdGraphUtilities::ImportNodesFromText`, the inverse of the read path's `ExportNodesToText`) |
| Connect / disconnect pins | `UEdGraphPin::MakeLinkTo` / `BreakLinkTo`, or `UEdGraphSchema::TryCreateConnection` (validated) |
| **Redirect a call to a different function (auto-rewire)** | `Node->FunctionReference.SetExternalMember(NewFunc, NewClass)` + `Node->ReconstructNode()` |
| Compile | `FKismetEditorUtilities::CompileBlueprint` |
| Save | `FEditorFileUtils::SavePackages` |

### Auto-rewire

`ReconstructNode()` rebuilds a node's pins from its (new) function and **reconnects existing
wires to pins of the same name**. So redirecting a `K2Node_CallFunction` from a Blueprint
function to a C++ function reconnects the pins automatically *when the C++ signature uses the
same parameter names* — no manual rewiring. The self-test proves this by redirecting a live,
wired call from `Abs` to `Sqrt` and confirming the input wire survives.

### Guardrails

- Run on a branch; commit per asset/op batch (edits mutate real `.uasset`s).
- **Compile-gate every asset** — never `SavePackages` a blueprint that failed `CompileBlueprint`.
- Use only the non-UI APIs above (avoid the `Open*Menu` variants) so it runs headless.

## Install

Drop into your project's `Plugins/` folder (or add as a submodule) and enable it for the Editor target.
It depends only on engine modules (`UnrealEd`, `Kismet`, `BlueprintGraph`, `ScriptDisassembler`,
`AssetRegistry`, `Json`).

## License

MIT — see [LICENSE](LICENSE).
