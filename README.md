# Git Asset Diff

Unreal Engine 5.7 editor-only plugin that diffs any Content Browser asset against a local Git revision — a branch, tag, `HEAD~1`, or commit hash — using Unreal's built-in diff tool. **Nothing is checked out**: your working tree, branch, and index are never touched.

Works with Git LFS. Packaged builds exclude it, because the module is `Editor` only.

## Features

- **Content Browser menu**: right-click a single asset → **Diff → Against Git Revision...**, type a revision (`main`, `origin/main`, `HEAD~1`, `9f3c2ab`) and Unreal's native diff window opens with *revision vs. current*.
- **C++ API** for other editor modules (for example [LLMBlueprintBridge](https://github.com/KnockKnockP/LLMBridgePlugin)):

```cpp
#include "GitAssetDiffModule.h"

FText Error;
FString RepoRoot, RelativeFilename, TempAssetFilename;

// Open Unreal's native diff window.
FGitAssetDiffModule::Get().DiffAssetAgainstGitRevision(FAssetData(Asset), TEXT("main"), Error);

// Or just load the revision copy as a diff-only package and inspect it yourself.
UObject* OldAsset = FGitAssetDiffModule::Get().LoadAssetFromGitRevision(
	FAssetData(Asset), TEXT("HEAD~1"), Error, &RepoRoot, &RelativeFilename, &TempAssetFilename);
```

## Installation

1. Clone or copy this repository into your project as `Plugins/GitAssetDiff/`:

   ```
   git clone https://github.com/KnockKnockP/GitDiffPlugin.git Plugins/GitAssetDiff
   ```

2. Enable it in your `.uproject` (or through **Edit → Plugins**):

   ```json
   { "Name": "GitAssetDiff", "Enabled": true }
   ```

3. Regenerate project files and build the editor target.

Requirements:

- Unreal Engine 5.7
- `git` on `PATH`
- Git LFS installed and configured, if your assets are stored in LFS

## How it works

1. Resolve the selected asset to its package file on disk. `/Game` is anchored to the `.uproject`'s `Content/` folder; other mount points are made absolute.
2. Find the repository root (`git rev-parse --show-toplevel`) and compute the path relative to the repository.
3. `git cat-file --filters <rev>:<path>` exports the revision copy into `FPaths::DiffDir()` (`Saved/Diff`). Because filters run, LFS pointers are smudged to the real binary.
4. Export the sidecar files `.uexp`, `.ubulk` and `.uptnl` when the revision has them.
5. Validate the exported header. The tool fails with a clear message for an LFS pointer, an LFS merge-conflict text, or a non-Unreal file.
6. `DiffUtils::LoadPackageForDiff` loads the copy, mapped to the current package, and `IAssetTools::DiffAssets` opens the diff.

## Safety

- It never runs checkout, switch, reset or stash, and never changes the working tree.
- The revision copy loads only from the diff temp directory.
- Revision input is limited to simple commit-ish characters (`A-Z a-z 0-9 . _ - / @ { } ^ ~`).
- Git runs with `-c safe.directory=<repo root>`.

## Limitations

- The asset's package file must exist on disk inside a Git working tree.
- The revision must contain a valid binary Unreal package at the same path. Renamed or moved assets are not followed.
- Dependencies (parent classes, referenced assets) resolve from the **current** working tree, not from the revision.
- You can diff only one asset at a time from the menu.

## Used by

- **[LLMBlueprintBridge](https://github.com/KnockKnockP/LLMBridgePlugin)**: its `diff_asset_against_git_revision`, `inspect_blueprint_git_revision` and `diff_blueprint_graph_against_git_revision` MCP tools are built on this plugin's API.

## License

MIT. See [LICENSE](LICENSE).
