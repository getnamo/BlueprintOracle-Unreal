# BlueprintOracle-Unreal

An Unreal Engine editor commandlet that dumps **ground-truth Blueprint data to JSON/text** for static
analysis, documentation, and **agent-assisted Blueprint-to-C++ conversion**.

Blueprints are binary visual graphs, which are awkward to diff, review, or feed to an LLM. BlueprintOracle
loads each Blueprint with the engine itself (so deserialization is always correct and version-accurate) and
exports a clean, text-friendly representation of its full logic.

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

## Converting a Blueprint to C++

See **[AGENTS.md](AGENTS.md)** for the step-by-step playbook (for an AI agent or a
human): how to run the oracle, read `graph.json`, map nodes/pins/variables/components to
C++, and the gotchas (Core Redirects, unserialized defaults, macros, latent flow).

## Install

Drop into your project's `Plugins/` folder (or add as a submodule) and enable it for the Editor target.
It depends only on engine modules (`UnrealEd`, `Kismet`, `BlueprintGraph`, `ScriptDisassembler`,
`AssetRegistry`, `Json`).

## License

MIT — see [LICENSE](LICENSE).
