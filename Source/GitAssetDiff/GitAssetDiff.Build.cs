using UnrealBuildTool;

public sealed class GitAssetDiff : ModuleRules
{
	public GitAssetDiff(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange([
			"Core",
			"CoreUObject"
		]);

		PrivateDependencyModuleNames.AddRange([
			"AssetDefinition",
			"AssetRegistry",
			"AssetTools",
			"ContentBrowser",
			"Engine",
			"Slate",
			"SlateCore",
			"UnrealEd"
		]);
	}
}
