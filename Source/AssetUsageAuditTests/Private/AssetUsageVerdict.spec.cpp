// MagentaDolphin 2026. Asset Usage Audit.

#include "AssetUsageAuditor.h"
#include "AssetUsageGraph.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

BEGIN_DEFINE_SPEC(AssetUsageVerdictSpec,
    "AssetUsageAudit.Verdict",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
END_DEFINE_SPEC(AssetUsageVerdictSpec)

void AssetUsageVerdictSpec::Define()
{
    Describe("ClassifyVerdict", [this]()
    {
        It("reports UsedOnLevel when a level reaches the asset", [this]()
        {
            const EAssetUsageVerdict Verdict = FAssetUsageAuditor::ClassifyVerdict(
                /*bReachableFromLevel=*/true,
                /*bHasReferencers=*/true,
                /*bFoundInConfigOrSource=*/false,
                /*bIsBlindSpot=*/false);

            TestEqual(TEXT("Verdict"), Verdict, EAssetUsageVerdict::UsedOnLevel);
        });

        It("reports UsedOnLevel even when the asset is also named in config", [this]()
        {
            // Level reachability is the strongest signal and must win. Otherwise a mesh that is
            // both placed on a level and mentioned in an ini would be filed under the weaker
            // config verdict and vanish from a per-level report.
            const EAssetUsageVerdict Verdict = FAssetUsageAuditor::ClassifyVerdict(true, true, true, false);

            TestEqual(TEXT("Verdict"), Verdict, EAssetUsageVerdict::UsedOnLevel);
        });

        It("reports UsedByAssetsOnly when referenced but no level reaches it", [this]()
        {
            const EAssetUsageVerdict Verdict = FAssetUsageAuditor::ClassifyVerdict(false, true, false, false);

            TestEqual(TEXT("Verdict"), Verdict, EAssetUsageVerdict::UsedByAssetsOnly);
        });

        It("reports ReferencedFromConfigOrSource when only a text literal names it", [this]()
        {
            // This is the BP_GameMode case: no asset references it, only
            // Config/DefaultEngine.ini does. Reporting it Unreferenced would be a real defect.
            const EAssetUsageVerdict Verdict = FAssetUsageAuditor::ClassifyVerdict(false, false, true, false);

            TestEqual(TEXT("Verdict"), Verdict, EAssetUsageVerdict::ReferencedFromConfigOrSource);
        });

        It("prefers the config verdict over UsedByAssetsOnly when both apply", [this]()
        {
            // Config provenance is more actionable than "some asset points at it" - it tells the
            // user the reference is invisible to the registry and where to look for it.
            const EAssetUsageVerdict Verdict = FAssetUsageAuditor::ClassifyVerdict(false, true, true, false);

            TestEqual(TEXT("Verdict"), Verdict, EAssetUsageVerdict::ReferencedFromConfigOrSource);
        });

        It("reports Unreferenced only when nothing at all points at it", [this]()
        {
            const EAssetUsageVerdict Verdict = FAssetUsageAuditor::ClassifyVerdict(false, false, false, false);

            TestEqual(TEXT("Verdict"), Verdict, EAssetUsageVerdict::Unreferenced);
        });

        It("never reports Unreferenced for a registry blind spot", [this]()
        {
            // An FMOD asset with zero referencers tells us nothing, because FMOD resolves events
            // by string path outside the UObject graph. Unknown is the honest answer, and the one
            // that stops someone deleting the audio bank.
            const EAssetUsageVerdict Verdict = FAssetUsageAuditor::ClassifyVerdict(false, false, false, true);

            TestEqual(TEXT("Verdict"), Verdict, EAssetUsageVerdict::Unknown);
            TestNotEqual(TEXT("Must not be Unreferenced"), Verdict, EAssetUsageVerdict::Unreferenced);
        });

        It("still reports UsedOnLevel for a blind-spot asset that a level does reach", [this]()
        {
            // A positive result is trustworthy even for a blind spot: we found a real edge.
            // Only the absence of evidence is uninformative.
            const EAssetUsageVerdict Verdict = FAssetUsageAuditor::ClassifyVerdict(true, false, false, true);

            TestEqual(TEXT("Verdict"), Verdict, EAssetUsageVerdict::UsedOnLevel);
        });

        It("produces a distinct string for every verdict", [this]()
        {
            // The report is parsed by other tools; two verdicts sharing a label would silently
            // merge distinct populations.
            TSet<FString> Seen;
            const TArray<EAssetUsageVerdict> All = {
                EAssetUsageVerdict::UsedOnLevel,
                EAssetUsageVerdict::UsedByAssetsOnly,
                EAssetUsageVerdict::ReferencedFromConfigOrSource,
                EAssetUsageVerdict::Unreferenced,
                EAssetUsageVerdict::Unknown
            };

            for (EAssetUsageVerdict Verdict : All)
            {
                const FString Label = LexToString(Verdict);
                TestFalse(FString::Printf(TEXT("Label '%s' must be unique"), *Label), Seen.Contains(Label));
                TestFalse(TEXT("Label must not be empty"), Label.IsEmpty());
                Seen.Add(Label);
            }
        });
    });

    Describe("IsRegistryBlindSpot", [this]()
    {
        It("flags FMOD assets", [this]()
        {
            FAssetUsageNode Node;
            Node.PackageName = FName(TEXT("/Game/FMOD/Events/Explosion"));
            Node.ClassPath = FTopLevelAssetPath(TEXT("/Script/FMODStudio.FMODEvent"));

            TestTrue(TEXT("FMOD asset is a blind spot"), FAssetUsageAuditor::IsRegistryBlindSpot(Node));
        });

        It("flags DataTables, whose rows the registry cannot attribute", [this]()
        {
            // A level depends on the whole table, so every row looks used even when none is.
            FAssetUsageNode Node;
            Node.PackageName = FName(TEXT("/Game/Data/DT_Weapons"));
            Node.ClassPath = FTopLevelAssetPath(TEXT("/Script/Engine.DataTable"));

            TestTrue(TEXT("DataTable is a blind spot"), FAssetUsageAuditor::IsRegistryBlindSpot(Node));
        });

        It("does not flag an ordinary static mesh", [this]()
        {
            FAssetUsageNode Node;
            Node.PackageName = FName(TEXT("/Game/Art/SM_Rock"));
            Node.ClassPath = FTopLevelAssetPath(TEXT("/Script/Engine.StaticMesh"));

            TestFalse(TEXT("StaticMesh is not a blind spot"), FAssetUsageAuditor::IsRegistryBlindSpot(Node));
        });
    });
}

#endif // WITH_DEV_AUTOMATION_TESTS
