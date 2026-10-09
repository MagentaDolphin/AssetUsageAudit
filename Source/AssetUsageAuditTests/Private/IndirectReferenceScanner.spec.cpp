// MagentaDolphin 2026. Asset Usage Audit.

#include "IndirectReferenceScanner.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

BEGIN_DEFINE_SPEC(IndirectReferenceScannerSpec,
    "AssetUsageAudit.IndirectReferences",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

    TArray<FString> Extract(const TCHAR* Line)
    {
        TArray<FString> Result;
        FIndirectReferenceScanner::ExtractGamePathsFromLine(Line, Result);
        return Result;
    }

END_DEFINE_SPEC(IndirectReferenceScannerSpec)

void IndirectReferenceScannerSpec::Define()
{
    Describe("ExtractGamePathsFromLine", [this]()
    {
        // These four lines are copied from this project's Config/DefaultEngine.ini. They are the
        // whole reason this scanner exists: nothing in Content references them, so without this
        // pass the project's GameMode and GameInstance are reported as unused assets.
        It("finds the GameMode from DefaultEngine.ini and strips the _C class suffix", [this]()
        {
            const TArray<FString> Found = Extract(
                TEXT("GlobalDefaultGameMode=/Game/Core/GameModes/BP_GameMode.BP_GameMode_C"));

            TestEqual(TEXT("one hit"), Found.Num(), 1);
            if (Found.Num() == 1)
            {
                TestEqual(TEXT("package"), Found[0], FString(TEXT("/Game/Core/GameModes/BP_GameMode")));
            }
        });

        It("finds the GameInstance from a purchased pack", [this]()
        {
            const TArray<FString> Found = Extract(
                TEXT("GameInstanceClass=/Game/UIPack/Blueprints/GameFramework/BP_MenuGameInstance.BP_MenuGameInstance_C"));

            TestEqual(TEXT("one hit"), Found.Num(), 1);
            if (Found.Num() == 1)
            {
                TestEqual(TEXT("package"), Found[0],
                    FString(TEXT("/Game/UIPack/Blueprints/GameFramework/BP_MenuGameInstance")));
            }
        });

        It("finds a map reference with an object suffix", [this]()
        {
            const TArray<FString> Found = Extract(TEXT("EditorStartupMap=/Game/Maps/Gyms/Gyms_Geoda.Gyms_Geoda"));

            TestEqual(TEXT("one hit"), Found.Num(), 1);
            if (Found.Num() == 1)
            {
                TestEqual(TEXT("package"), Found[0], FString(TEXT("/Game/Maps/Gyms/Gyms_Geoda")));
            }
        });

        It("finds a bare package path with no object suffix", [this]()
        {
            const TArray<FString> Found = Extract(TEXT("GameDefaultMap=/Game/Maps/RefinedMenuMap"));

            TestEqual(TEXT("one hit"), Found.Num(), 1);
            if (Found.Num() == 1)
            {
                TestEqual(TEXT("package"), Found[0], FString(TEXT("/Game/Maps/RefinedMenuMap")));
            }
        });

        It("strips surrounding quotes and parentheses from an ini struct value", [this]()
        {
            const TArray<FString> Found = Extract(TEXT("+DirectoriesToAlwaysCook=(Path=\"/Game/Art\")"));

            TestEqual(TEXT("one hit"), Found.Num(), 1);
            if (Found.Num() == 1)
            {
                TestEqual(TEXT("package"), Found[0], FString(TEXT("/Game/Art")));
            }
        });

        It("handles a C++ literal wrapped in TEXT()", [this]()
        {
            const TArray<FString> Found = Extract(
                TEXT("static const FSoftObjectPath Path(TEXT(\"/Game/Art/SM_Rock\"));"));

            TestEqual(TEXT("one hit"), Found.Num(), 1);
            if (Found.Num() == 1)
            {
                TestEqual(TEXT("package"), Found[0], FString(TEXT("/Game/Art/SM_Rock")));
            }
        });

        It("finds several distinct paths on one line", [this]()
        {
            const TArray<FString> Found = Extract(
                TEXT("Redirect=(From=\"/Game/Old/SM_A\", To=\"/Game/New/SM_B\")"));

            TestEqual(TEXT("two hits"), Found.Num(), 2);
            if (Found.Num() == 2)
            {
                TestEqual(TEXT("first"), Found[0], FString(TEXT("/Game/Old/SM_A")));
                TestEqual(TEXT("second"), Found[1], FString(TEXT("/Game/New/SM_B")));
            }
        });

        It("returns nothing for a line with no /Game path", [this]()
        {
            TestEqual(TEXT("no hits"), Extract(TEXT("bDynamicLevelStreaming=False")).Num(), 0);
            TestEqual(TEXT("no hits"), Extract(TEXT("")).Num(), 0);
        });

        It("ignores a bare /Game root with no asset under it", [this]()
        {
            // "/Game" alone is a mount point, not a reference. Recording it would mark the entire
            // project as referenced-from-config and make the verdict column meaningless.
            TestEqual(TEXT("no hits"), Extract(TEXT("PackagePath=/Game")).Num(), 0);
        });

        It("does not match a path that merely contains the word Game", [this]()
        {
            TestEqual(TEXT("no hits"), Extract(TEXT("Path=/MiniGames2/Foo/SM_Bar")).Num(), 0);
        });
    });
}

#endif // WITH_DEV_AUTOMATION_TESTS
