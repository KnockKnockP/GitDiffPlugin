#pragma once

#include "Modules/ModuleInterface.h"
#include "Modules/ModuleManager.h"
#include "Delegates/Delegate.h"
#include "Internationalization/Text.h"

class FExtender;
class FMenuBuilder;
class UObject;
struct FAssetData;

/**
 * Diffs Unreal assets against a local Git revision without touching the working tree.
 *
 * Adds "Diff > Against Git Revision..." to the Content Browser asset context menu, and
 * exposes the same functionality to other editor modules (for example LLMBlueprintBridge).
 */
class GITASSETDIFF_API FGitAssetDiffModule final : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

	static FGitAssetDiffModule& Get()
	{
		return FModuleManager::LoadModuleChecked<FGitAssetDiffModule>(TEXT("GitAssetDiff"));
	}

	/** Exports the asset at Revision and opens Unreal's native diff against the current asset. */
	bool DiffAssetAgainstGitRevision(
		FAssetData AssetData,
		const FString& Revision,
		FText& OutError,
		FString* OutRepoRoot = nullptr,
		FString* OutRelativeFilename = nullptr,
		FString* OutTempAssetFilename = nullptr);

	/** Exports the asset at Revision into the diff temp dir and loads it as a diff-only package. */
	UObject* LoadAssetFromGitRevision(
		FAssetData AssetData,
		const FString& Revision,
		FText& OutError,
		FString* OutRepoRoot = nullptr,
		FString* OutRelativeFilename = nullptr,
		FString* OutTempAssetFilename = nullptr);

private:
	void RegisterContentBrowserExtensions();
	void UnregisterContentBrowserExtensions();
	TSharedRef<FExtender> ExtendAssetContextMenu(const TArray<FAssetData>& SelectedAssets);
	void AddAssetDiffMenu(FMenuBuilder& MenuBuilder, TArray<FAssetData> SelectedAssets);
	void ExecuteDiffAgainstGitRevision(FAssetData AssetData);

	FDelegateHandle AssetContextMenuExtenderHandle;
};
