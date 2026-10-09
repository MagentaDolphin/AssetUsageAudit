// MagentaDolphin 2026. Asset Usage Audit.

#include "AssetUsagePaths.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

BEGIN_DEFINE_SPEC(AssetUsagePathsSpec,
    "AssetUsageAudit.Paths",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
END_DEFINE_SPEC(AssetUsagePathsSpec)

void AssetUsagePathsSpec::Define()
{
    Describe("IsExternalPackage", [this]()
    {
        It("detects an OFPA external actor package", [this]()
        {
            // Real shape from a live project: /Game/__ExternalActors__/Maps/WP_Example/8/AB/<22 chars>
            TestTrue(TEXT("external actor"),
                AssetUsagePaths::IsExternalPackage(FName(TEXT("/Game/__ExternalActors__/Maps/WP_Example/8/AB/QWERTYUIOPASDFGHJKLZXC"))));
        });

        It("detects an external objects package", [this]()
        {
            TestTrue(TEXT("external object"),
                AssetUsagePaths::IsExternalPackage(FName(TEXT("/Game/__ExternalObjects__/Maps/L_3i_Hub/1/2A/ABC"))));
        });

        It("does not flag an ordinary content package", [this]()
        {
            TestFalse(TEXT("ordinary asset"),
                AssetUsagePaths::IsExternalPackage(FName(TEXT("/Game/Art/SM_Rock"))));
        });

        It("does not flag a level that merely lives next to external actors", [this]()
        {
            TestFalse(TEXT("the level itself is not external"),
                AssetUsagePaths::IsExternalPackage(FName(TEXT("/Game/Maps/WP_Example"))));
        });
    });

    Describe("IsScriptPackage", [this]()
    {
        It("flags /Script packages", [this]()
        {
            TestTrue(TEXT("script"), AssetUsagePaths::IsScriptPackage(FName(TEXT("/Script/MyGame"))));
        });

        It("does not flag content that starts with the same letters", [this]()
        {
            TestFalse(TEXT("not script"), AssetUsagePaths::IsScriptPackage(FName(TEXT("/Scripted/Foo"))));
        });
    });

    Describe("NormalizeExclusionPrefix", [this]()
    {
        It("converts content-relative form to package form", [this]()
        {
            TestEqual(TEXT("Content/3rdParty"),
                AssetUsagePaths::NormalizeExclusionPrefix(TEXT("Content/3rdParty")), FString(TEXT("/Game/3rdParty")));
        });

        It("strips a trailing slash", [this]()
        {
            TestEqual(TEXT("trailing slash"),
                AssetUsagePaths::NormalizeExclusionPrefix(TEXT("Content/3rdParty/")), FString(TEXT("/Game/3rdParty")));
        });

        It("leaves package form untouched", [this]()
        {
            TestEqual(TEXT("already package form"),
                AssetUsagePaths::NormalizeExclusionPrefix(TEXT("/Game/Zombie")), FString(TEXT("/Game/Zombie")));
        });

        It("maps bare Content to the game root", [this]()
        {
            TestEqual(TEXT("bare Content"),
                AssetUsagePaths::NormalizeExclusionPrefix(TEXT("Content")), FString(TEXT("/Game")));
        });

        It("normalizes backslashes pasted from Explorer", [this]()
        {
            TestEqual(TEXT("backslashes"),
                AssetUsagePaths::NormalizeExclusionPrefix(TEXT("Content\\3rdParty\\Fab")), FString(TEXT("/Game/3rdParty/Fab")));
        });

        It("returns empty for an empty or slash-only prefix", [this]()
        {
            TestTrue(TEXT("empty"), AssetUsagePaths::NormalizeExclusionPrefix(TEXT("")).IsEmpty());
            TestTrue(TEXT("slashes only"), AssetUsagePaths::NormalizeExclusionPrefix(TEXT("///")).IsEmpty());
        });
    });

    Describe("IsPathExcluded", [this]()
    {
        It("excludes a package under the prefix", [this]()
        {
            const TArray<FString> Excluded = { TEXT("Content/3rdParty") };
            TestTrue(TEXT("under prefix"),
                AssetUsagePaths::IsPathExcluded(FName(TEXT("/Game/3rdParty/Galaxy/SM_Star")), Excluded));
        });

        It("does not exclude a sibling folder sharing a name prefix", [this]()
        {
            // The bug this guards: a naive StartsWith would exclude /Game/ArtSource when the user
            // only asked to exclude /Game/Art.
            const TArray<FString> Excluded = { TEXT("/Game/Art") };
            TestFalse(TEXT("ArtSource must survive"),
                AssetUsagePaths::IsPathExcluded(FName(TEXT("/Game/ArtSource/SM_Rock")), Excluded));
            TestTrue(TEXT("Art itself is excluded"),
                AssetUsagePaths::IsPathExcluded(FName(TEXT("/Game/Art/SM_Rock")), Excluded));
        });

        It("matches case-insensitively", [this]()
        {
            const TArray<FString> Excluded = { TEXT("content/ZOMBIE") };
            TestTrue(TEXT("case insensitive"),
                AssetUsagePaths::IsPathExcluded(FName(TEXT("/Game/Zombie/SK_Walker")), Excluded));
        });

        It("returns false for an empty exclusion list", [this]()
        {
            const TArray<FString> Excluded;
            TestFalse(TEXT("nothing excluded"),
                AssetUsagePaths::IsPathExcluded(FName(TEXT("/Game/Art/SM_Rock")), Excluded));
        });
    });

    Describe("ToProjectRelativePath", [this]()
    {
        It("converts an asset package", [this]()
        {
            TestEqual(TEXT("asset"),
                AssetUsagePaths::ToProjectRelativePath(FName(TEXT("/Game/Art/SM_Rock")), false),
                FString(TEXT("Content/Art/SM_Rock.uasset")));
        });

        It("uses the umap extension for levels", [this]()
        {
            TestEqual(TEXT("level"),
                AssetUsagePaths::ToProjectRelativePath(FName(TEXT("/Game/Maps/WP_Example")), true),
                FString(TEXT("Content/Maps/WP_Example.umap")));
        });

        It("returns empty for script and engine packages rather than inventing a path", [this]()
        {
            TestTrue(TEXT("script"),
                AssetUsagePaths::ToProjectRelativePath(FName(TEXT("/Script/MyGame")), false).IsEmpty());
            TestTrue(TEXT("engine"),
                AssetUsagePaths::ToProjectRelativePath(FName(TEXT("/Engine/BasicShapes/Cube")), false).IsEmpty());
        });

        It("converts an external actor package, which is still under /Game", [this]()
        {
            TestEqual(TEXT("external actor"),
                AssetUsagePaths::ToProjectRelativePath(FName(TEXT("/Game/__ExternalActors__/Maps/WP_Example/8/AB/ABC")), false),
                FString(TEXT("Content/__ExternalActors__/Maps/WP_Example/8/AB/ABC.uasset")));
        });
    });
}

#endif // WITH_DEV_AUTOMATION_TESTS
