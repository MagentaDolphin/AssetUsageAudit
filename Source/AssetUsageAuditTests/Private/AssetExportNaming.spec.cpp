// MagentaDolphin 2026. Asset Usage Audit.

#include "AssetExportNaming.h"
#include "AssetUsageAuditTypes.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
    /** Builds an Exists predicate over a fixed set of filenames, so no filesystem is involved. */
    struct FFakeFolder
    {
        TSet<FString> Files;

        explicit FFakeFolder(std::initializer_list<const TCHAR*> InFiles)
        {
            for (const TCHAR* File : InFiles)
            {
                Files.Add(FString(File));
            }
        }

        bool Contains(const FString& FileName) const
        {
            return Files.Contains(FileName);
        }
    };
}

BEGIN_DEFINE_SPEC(AssetExportNamingSpec,
    "AssetUsageAudit.ExportNaming",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
END_DEFINE_SPEC(AssetExportNamingSpec)

void AssetExportNamingSpec::Define()
{
    Describe("SplitTrailingIndex", [this]()
    {
        It("reports no index for a plain name", [this]()
        {
            FString Stem;
            int32 Index = 0;
            int32 PadWidth = 0;
            AssetExportNaming::SplitTrailingIndex(TEXT("SM_Rock"), Stem, Index, PadWidth);

            TestEqual(TEXT("Stem"), Stem, FString(TEXT("SM_Rock")));
            TestEqual(TEXT("Index"), Index, (int32)INDEX_NONE);
            TestEqual(TEXT("PadWidth"), PadWidth, 0);
        });

        It("splits a single-digit trailing index", [this]()
        {
            FString Stem;
            int32 Index = 0;
            int32 PadWidth = 0;
            AssetExportNaming::SplitTrailingIndex(TEXT("SM_Rock_7"), Stem, Index, PadWidth);

            TestEqual(TEXT("Stem"), Stem, FString(TEXT("SM_Rock")));
            TestEqual(TEXT("Index"), Index, 7);
            TestEqual(TEXT("PadWidth"), PadWidth, 1);
        });

        It("preserves zero padding width", [this]()
        {
            FString Stem;
            int32 Index = 0;
            int32 PadWidth = 0;
            AssetExportNaming::SplitTrailingIndex(TEXT("SM_Rock_007"), Stem, Index, PadWidth);

            TestEqual(TEXT("Stem"), Stem, FString(TEXT("SM_Rock")));
            TestEqual(TEXT("Index"), Index, 7);
            TestEqual(TEXT("PadWidth"), PadWidth, 3);
        });

        It("treats a non-numeric suffix as part of the name", [this]()
        {
            // The trap this whole function exists for: _v3 must not be read as an index,
            // and the _02 in the middle must not be touched.
            FString Stem;
            int32 Index = 0;
            int32 PadWidth = 0;
            AssetExportNaming::SplitTrailingIndex(TEXT("SM_Rock_02_v3"), Stem, Index, PadWidth);

            TestEqual(TEXT("Stem"), Stem, FString(TEXT("SM_Rock_02_v3")));
            TestEqual(TEXT("Index"), Index, (int32)INDEX_NONE);
        });

        It("splits a mid-name numeric group only when it is trailing", [this]()
        {
            FString Stem;
            int32 Index = 0;
            int32 PadWidth = 0;
            AssetExportNaming::SplitTrailingIndex(TEXT("SM_Rock_02"), Stem, Index, PadWidth);

            TestEqual(TEXT("Stem"), Stem, FString(TEXT("SM_Rock")));
            TestEqual(TEXT("Index"), Index, 2);
            TestEqual(TEXT("PadWidth"), PadWidth, 2);
        });

        It("treats an empty suffix as no index", [this]()
        {
            FString Stem;
            int32 Index = 0;
            int32 PadWidth = 0;
            AssetExportNaming::SplitTrailingIndex(TEXT("SM_Rock_"), Stem, Index, PadWidth);

            TestEqual(TEXT("Stem"), Stem, FString(TEXT("SM_Rock_")));
            TestEqual(TEXT("Index"), Index, (int32)INDEX_NONE);
        });

        It("does not treat a leading underscore as a separator", [this]()
        {
            FString Stem;
            int32 Index = 0;
            int32 PadWidth = 0;
            AssetExportNaming::SplitTrailingIndex(TEXT("_1"), Stem, Index, PadWidth);

            TestEqual(TEXT("Stem"), Stem, FString(TEXT("_1")));
            TestEqual(TEXT("Index"), Index, (int32)INDEX_NONE);
        });
    });

    Describe("ComposeIndexedName", [this]()
    {
        It("returns the bare stem for INDEX_NONE", [this]()
        {
            TestEqual(TEXT("Composed"), AssetExportNaming::ComposeIndexedName(TEXT("Foo"), INDEX_NONE, 0), FString(TEXT("Foo")));
        });

        It("appends an unpadded index", [this]()
        {
            TestEqual(TEXT("Composed"), AssetExportNaming::ComposeIndexedName(TEXT("Foo"), 1, 1), FString(TEXT("Foo_1")));
        });

        It("restores zero padding", [this]()
        {
            TestEqual(TEXT("Composed"), AssetExportNaming::ComposeIndexedName(TEXT("Foo"), 8, 3), FString(TEXT("Foo_008")));
        });

        It("widens rather than truncates when the number outgrows the padding", [this]()
        {
            TestEqual(TEXT("Composed"), AssetExportNaming::ComposeIndexedName(TEXT("Foo"), 100, 2), FString(TEXT("Foo_100")));
        });
    });

    Describe("ResolveCollision under Overwrite", [this]()
    {
        It("returns the desired name even when it is taken", [this]()
        {
            const FFakeFolder Folder({ TEXT("SM_Rock.uasset") });
            const FString Result = AssetExportNaming::ResolveCollision(
                TEXT("SM_Rock.uasset"),
                EExportCollisionPolicy::Overwrite,
                [&Folder](const FString& Name) { return Folder.Contains(Name); });

            TestEqual(TEXT("Result"), Result, FString(TEXT("SM_Rock.uasset")));
        });
    });

    Describe("ResolveCollision under Index", [this]()
    {
        It("returns the desired name when the folder is empty", [this]()
        {
            const FFakeFolder Folder({});
            const FString Result = AssetExportNaming::ResolveCollision(
                TEXT("SM_Rock.uasset"),
                EExportCollisionPolicy::Index,
                [&Folder](const FString& Name) { return Folder.Contains(Name); });

            TestEqual(TEXT("Result"), Result, FString(TEXT("SM_Rock.uasset")));
        });

        It("appends _1 on the first collision", [this]()
        {
            const FFakeFolder Folder({ TEXT("SM_Rock.uasset") });
            const FString Result = AssetExportNaming::ResolveCollision(
                TEXT("SM_Rock.uasset"),
                EExportCollisionPolicy::Index,
                [&Folder](const FString& Name) { return Folder.Contains(Name); });

            TestEqual(TEXT("Result"), Result, FString(TEXT("SM_Rock_1.uasset")));
        });

        It("advances to _2 when _1 is also taken", [this]()
        {
            const FFakeFolder Folder({ TEXT("SM_Rock.uasset"), TEXT("SM_Rock_1.uasset") });
            const FString Result = AssetExportNaming::ResolveCollision(
                TEXT("SM_Rock.uasset"),
                EExportCollisionPolicy::Index,
                [&Folder](const FString& Name) { return Folder.Contains(Name); });

            TestEqual(TEXT("Result"), Result, FString(TEXT("SM_Rock_2.uasset")));
        });

        It("increments an existing index instead of appending a second one", [this]()
        {
            // The explicit requirement: Foo_7 must become Foo_8, never Foo_7_1.
            const FFakeFolder Folder({ TEXT("SM_Rock_7.uasset") });
            const FString Result = AssetExportNaming::ResolveCollision(
                TEXT("SM_Rock_7.uasset"),
                EExportCollisionPolicy::Index,
                [&Folder](const FString& Name) { return Folder.Contains(Name); });

            TestEqual(TEXT("Result"), Result, FString(TEXT("SM_Rock_8.uasset")));
            TestFalse(TEXT("Must not contain a doubled index"), Result.Contains(TEXT("_7_")));
        });

        It("keeps zero padding when incrementing", [this]()
        {
            const FFakeFolder Folder({ TEXT("SM_Rock_007.uasset") });
            const FString Result = AssetExportNaming::ResolveCollision(
                TEXT("SM_Rock_007.uasset"),
                EExportCollisionPolicy::Index,
                [&Folder](const FString& Name) { return Folder.Contains(Name); });

            TestEqual(TEXT("Result"), Result, FString(TEXT("SM_Rock_008.uasset")));
        });

        It("widens padding when the increment carries", [this]()
        {
            const FFakeFolder Folder({ TEXT("SM_Rock_99.uasset") });
            const FString Result = AssetExportNaming::ResolveCollision(
                TEXT("SM_Rock_99.uasset"),
                EExportCollisionPolicy::Index,
                [&Folder](const FString& Name) { return Folder.Contains(Name); });

            TestEqual(TEXT("Result"), Result, FString(TEXT("SM_Rock_100.uasset")));
        });

        It("does not mistake a version suffix for an index", [this]()
        {
            const FFakeFolder Folder({ TEXT("SM_Rock_02_v3.uasset") });
            const FString Result = AssetExportNaming::ResolveCollision(
                TEXT("SM_Rock_02_v3.uasset"),
                EExportCollisionPolicy::Index,
                [&Folder](const FString& Name) { return Folder.Contains(Name); });

            TestEqual(TEXT("Result"), Result, FString(TEXT("SM_Rock_02_v3_1.uasset")));
        });

        It("preserves the extension", [this]()
        {
            const FFakeFolder Folder({ TEXT("SM_Rock.fbx") });
            const FString Result = AssetExportNaming::ResolveCollision(
                TEXT("SM_Rock.fbx"),
                EExportCollisionPolicy::Index,
                [&Folder](const FString& Name) { return Folder.Contains(Name); });

            TestEqual(TEXT("Result"), Result, FString(TEXT("SM_Rock_1.fbx")));
        });

        It("handles a name with no extension", [this]()
        {
            const FFakeFolder Folder({ TEXT("SM_Rock") });
            const FString Result = AssetExportNaming::ResolveCollision(
                TEXT("SM_Rock"),
                EExportCollisionPolicy::Index,
                [&Folder](const FString& Name) { return Folder.Contains(Name); });

            TestEqual(TEXT("Result"), Result, FString(TEXT("SM_Rock_1")));
        });

        It("skips over a gap-free run of taken indices", [this]()
        {
            const FFakeFolder Folder({
                TEXT("T_Wall.png"), TEXT("T_Wall_1.png"), TEXT("T_Wall_2.png"), TEXT("T_Wall_3.png") });
            const FString Result = AssetExportNaming::ResolveCollision(
                TEXT("T_Wall.png"),
                EExportCollisionPolicy::Index,
                [&Folder](const FString& Name) { return Folder.Contains(Name); });

            TestEqual(TEXT("Result"), Result, FString(TEXT("T_Wall_4.png")));
        });

        It("is idempotent for a name that does not collide", [this]()
        {
            const FFakeFolder Folder({ TEXT("Other.uasset") });
            const auto Predicate = [&Folder](const FString& Name) { return Folder.Contains(Name); };

            const FString First = AssetExportNaming::ResolveCollision(TEXT("SM_Rock.uasset"), EExportCollisionPolicy::Index, Predicate);
            const FString Second = AssetExportNaming::ResolveCollision(First, EExportCollisionPolicy::Index, Predicate);

            TestEqual(TEXT("Stable"), Second, First);
        });
    });
}

#endif  // WITH_DEV_AUTOMATION_TESTS
