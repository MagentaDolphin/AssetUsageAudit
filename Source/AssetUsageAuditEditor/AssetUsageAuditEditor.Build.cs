// MagentaDolphin 2026. Asset Usage Audit.

using UnrealBuildTool;

public class AssetUsageAuditEditor : ModuleRules
{
    public AssetUsageAuditEditor(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

        PublicDependencyModuleNames.AddRange(new string[]
        {
            "Core",
            "AssetUsageAuditCore"
        });

        // Everything editor-only and UI-shaped belongs here and nowhere else. Keeping this list
        // out of AssetUsageAuditCore is what lets the same analysis run from a commandlet later.
        PrivateDependencyModuleNames.AddRange(new string[]
        {
            "CoreUObject",
            "Engine",
            "AssetRegistry",
            "AssetTools",               // MigratePackages - the only path that keeps references working
            "DeveloperSettings",
            "DesktopPlatform",          // native folder picker for the export destination
            "EditorFramework",
            "InputCore",
            "Projects",
            "Slate",
            "SlateCore",
            "ToolMenus",
            "ToolWidgets",              // SSearchBox, SSegmentedControl
            "UnrealEd",
            "WorkspaceMenuStructure"    // places the tab under Tools in the Window menu
        });
    }
}
