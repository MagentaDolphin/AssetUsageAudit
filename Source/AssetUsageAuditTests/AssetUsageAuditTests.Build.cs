// MagentaDolphin 2026. Asset Usage Audit.

using UnrealBuildTool;

public class AssetUsageAuditTests : ModuleRules
{
    public AssetUsageAuditTests(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

        PublicDependencyModuleNames.AddRange(new string[]
        {
            "Core",
            "CoreUObject",
            "Engine"
        });

        PrivateDependencyModuleNames.AddRange(new string[]
        {
            "AssetRegistry",
            "AssetUsageAuditCore",
            // Only the Migrate live spec needs these, and only ever under EditorContext. The Core
            // module still must not link them - that invariant is about the analysis being runnable
            // headless, and a test module is not part of that path.
            "AssetTools",
            "UnrealEd",
            "Json",
            "Projects"
        });
    }
}
