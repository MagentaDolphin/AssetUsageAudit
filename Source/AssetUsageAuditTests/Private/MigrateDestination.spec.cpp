// MagentaDolphin 2026. Asset Usage Audit.

#include "AssetUsagePaths.h"

#include "HAL/FileManager.h"
#include "Misc/App.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * The Migrate destination check.
 *
 * Exists because the engine validates the destination only after MigratePackages has been called,
 * and reports the refusal to the Output Log alone - so a bad folder presented as a button that did
 * nothing at all. These specs pin the two rules to real folders on disk rather than to a reading of
 * the engine source.
 */
BEGIN_DEFINE_SPEC(MigrateDestinationSpec,
    "AssetUsageAudit.MigrateDestination",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

    FString ScratchDir;

    void MakeScratchDir(const TCHAR* Suffix)
    {
        ScratchDir = FPaths::ConvertRelativePathToFull(
            FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("AssetUsageAuditTests"), Suffix));

        IFileManager::Get().DeleteDirectory(*ScratchDir, false, true);
        IFileManager::Get().MakeDirectory(*ScratchDir, true);
    }

    void RemoveScratchDir()
    {
        if (!ScratchDir.IsEmpty())
        {
            IFileManager::Get().DeleteDirectory(*ScratchDir, false, true);
        }
    }

    /** Build <Scratch>/<ProjectName>/Content plus a marker file beside it. */
    FString MakeFakeProject(const TCHAR* MarkerFileName)
    {
        const FString ProjectRoot = FPaths::Combine(ScratchDir, TEXT("FakeProject"));
        const FString ContentDir = FPaths::Combine(ProjectRoot, TEXT("Content"));

        IFileManager::Get().MakeDirectory(*ContentDir, true);

        if (MarkerFileName)
        {
            FFileHelper::SaveStringToFile(TEXT("{}"), *FPaths::Combine(ProjectRoot, MarkerFileName));
        }

        return ContentDir;
    }

END_DEFINE_SPEC(MigrateDestinationSpec)

void MigrateDestinationSpec::Define()
{
    Describe("Rejecting a destination Migrate would refuse", [this]()
    {
        AfterEach([this]() { RemoveScratchDir(); });

        It("rejects an empty path", [this]()
        {
            TestFalse(TEXT("has a reason"), AssetUsagePaths::ValidateMigrateDestination(FString()).IsEmpty());
        });

        It("rejects a folder that is not called Content", [this]()
        {
            // The project's own Saved/AssetUsageAudit is the default for every other layout, so
            // this is the exact path a user arrives with after switching to Migrate.
            MakeScratchDir(TEXT("MigrateNotContent"));

            const FString Reason = AssetUsagePaths::ValidateMigrateDestination(ScratchDir);

            TestFalse(TEXT("rejected"), Reason.IsEmpty());
            TestTrue(TEXT("says the folder must be Content"), Reason.Contains(TEXT("Content")));
        });

        It("rejects a Content folder with no project or plugin beside it", [this]()
        {
            // Passes the name check and still aborts: Migrate derives the destination mount point
            // from what sits next to the Content folder.
            MakeScratchDir(TEXT("MigrateOrphanContent"));

            const FString ContentDir = MakeFakeProject(nullptr);

            TestFalse(TEXT("rejected"), AssetUsagePaths::ValidateMigrateDestination(ContentDir).IsEmpty());
        });

        It("rejects a Content folder whose parent holds several plugins", [this]()
        {
            MakeScratchDir(TEXT("MigrateManyPlugins"));

            const FString ContentDir = MakeFakeProject(TEXT("First.uplugin"));
            FFileHelper::SaveStringToFile(TEXT("{}"),
                *FPaths::Combine(FPaths::GetPath(ContentDir), TEXT("Second.uplugin")));

            const FString Reason = AssetUsagePaths::ValidateMigrateDestination(ContentDir);

            TestFalse(TEXT("rejected"), Reason.IsEmpty());
            TestTrue(TEXT("explains the ambiguity"), Reason.Contains(TEXT("several")));
        });
    });

    Describe("Accepting a destination Migrate would use", [this]()
    {
        AfterEach([this]() { RemoveScratchDir(); });

        It("accepts a Content folder next to a .uproject", [this]()
        {
            MakeScratchDir(TEXT("MigrateGoodProject"));

            const FString ContentDir = MakeFakeProject(TEXT("FakeProject.uproject"));

            TestTrue(TEXT("accepted"), AssetUsagePaths::ValidateMigrateDestination(ContentDir).IsEmpty());
        });

        It("accepts a Content folder next to exactly one .uplugin", [this]()
        {
            // Plugin content is a legitimate migrate target; the engine derives the mount point
            // from the plugin name.
            MakeScratchDir(TEXT("MigrateGoodPlugin"));

            const FString ContentDir = MakeFakeProject(TEXT("Only.uplugin"));

            TestTrue(TEXT("accepted"), AssetUsagePaths::ValidateMigrateDestination(ContentDir).IsEmpty());
        });

        It("accepts a trailing slash and backslashes alike", [this]()
        {
            // A path pasted from Explorer arrives with backslashes; one typed by hand often ends
            // in a slash. Neither should read as "not a Content folder".
            MakeScratchDir(TEXT("MigrateSlashes"));

            const FString ContentDir = MakeFakeProject(TEXT("FakeProject.uproject"));

            TestTrue(TEXT("trailing slash"), AssetUsagePaths::ValidateMigrateDestination(ContentDir + TEXT("/")).IsEmpty());
            TestTrue(TEXT("backslashes"), AssetUsagePaths::ValidateMigrateDestination(ContentDir.Replace(TEXT("/"), TEXT("\\"))).IsEmpty());
        });

        It("accepts this project's own Content folder", [this]()
        {
            // The strongest available check that the rule matches reality: the running project is
            // a real, valid migrate target by construction.
            const FString ProjectContent = FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir());

            TestTrue(TEXT("accepted"), AssetUsagePaths::ValidateMigrateDestination(ProjectContent).IsEmpty());
        });
    });
}

#endif // WITH_DEV_AUTOMATION_TESTS
