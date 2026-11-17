using UnrealBuildTool;
public class Suzie : ModuleRules
{
	public Suzie(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;
		OptimizeCode = CodeOptimization.Never;

		PublicIncludePaths.AddRange(new string[] {}); 
		
		PrivateIncludePaths.AddRange(new string[] {});


		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
			}
			);

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"CoreUObject",
				"Engine",
				"Blutility",
				"Json",
				"GameplayTags",
				"UnrealEd",
				"Projects",
				"BlueprintGraph",
				"zlib",
				"AssetRegistry",
			}
			);

		DynamicallyLoadedModuleNames.AddRange(new string[] {});
	}
}
