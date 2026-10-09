using UnrealBuildTool;

public class AssetUsageAuditCore : ModuleRules
{
    public AssetUsageAuditCore(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

        // INVARIANT: this module must stay free of UI and of the editor-only asset pipeline.
        // Do NOT add: UnrealEd, AssetTools, ToolMenus, Slate, SlateCore, EditorSubsystem.
        // AssetTools is editor-only transitively (it puts UnrealEd in PublicDependencyModuleNames),
        // so pulling it in here would silently make the whole analysis undrivable from a commandlet.
        PublicDependencyModuleNames.AddRange(new string[]
        {
            "Core",
            "CoreUObject",
            "AssetRegistry"
        });

        PrivateDependencyModuleNames.AddRange(new string[]
        {
            // Engine is a Runtime module and is available in commandlets, so it does not
            // compromise the headless goal. It is needed for ULevel::GetExternalActorsPaths and
            // ScanLevelAssets - the OFPA folder layout must never be string-built, because
            // Content Bundles inject /CB/<Guid>/, External Data Layers inject /EDL/<UID>/, and
            // plugins can register further roots through delegates.
            "Engine",
            "Json",
            "Projects"
        });
    }
}
