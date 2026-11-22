using UnrealBuildTool;

public class SerializeSubsystem : ModuleRules
{
	public SerializeSubsystem(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"CoreUObject",
			"Engine",
			"DeveloperSettings",
		});

		// Tombstoned replicated actors store their destruction with the Iris bridge (SerializeSubsystem.cpp).
		SetupIrisSupport(Target);

		if (Target.Type == TargetType.Editor)
		{
			PrivateDependencyModuleNames.AddRange(new string[]
			{
				"UnrealEd",
			});
		}
	}
}