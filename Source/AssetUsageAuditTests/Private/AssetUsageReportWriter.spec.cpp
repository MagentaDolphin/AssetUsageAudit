// MagentaDolphin 2026. Asset Usage Audit.

#include "AssetUsageReportWriter.h"
#include "AssetUsageAuditor.h"
#include "AssetUsageAuditTypes.h"
#include "Dom/JsonObject.h"
#include "Misc/AutomationTest.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
    FAssetUsageAuditResult MakeSampleResult()
    {
        FAssetUsageAuditResult Result;

        Result.Header.GeneratedAt = FDateTime(2026, 9, 2, 12, 0, 0);
        Result.Header.ProjectName = TEXT("MyProject");
        Result.Header.EngineVersion = TEXT("5.6.1");
        Result.Header.ToolVersion = TEXT("0.1");
        Result.Header.LevelsScanned = 2;
        Result.Header.AssetsScanned = 3;
        Result.Header.AppliedFilters.Add(TEXT("Types: StaticMesh"));

        FAssetUsageRow& Used = Result.Rows.AddDefaulted_GetRef();
        Used.PackageName = FName(TEXT("/Game/Art/SM_Rock"));
        Used.AssetName = FName(TEXT("SM_Rock"));
        Used.ClassPath = FTopLevelAssetPath(TEXT("/Script/Engine.StaticMesh"));
        Used.PathFromProjectRoot = TEXT("Content/Art/SM_Rock.uasset");
        Used.Verdict = EAssetUsageVerdict::UsedOnLevel;
        Used.Levels = { FName(TEXT("/Game/Maps/WP_Example")), FName(TEXT("/Game/Maps/L_3i_Hub")) };
        Used.HardReferenceCount = 3;
        Used.SoftReferenceCount = 1;
        Used.Provenance = EAssetUsageProvenance::HardReference | EAssetUsageProvenance::ExternalActor;
        Used.Route = TEXT("WP_Example -> BP_Rock -> SM_Rock");

        FAssetUsageRow& Unused = Result.Rows.AddDefaulted_GetRef();
        Unused.PackageName = FName(TEXT("/Game/Art/SM_Orphan"));
        Unused.AssetName = FName(TEXT("SM_Orphan"));
        Unused.ClassPath = FTopLevelAssetPath(TEXT("/Script/Engine.StaticMesh"));
        Unused.PathFromProjectRoot = TEXT("Content/Art/SM_Orphan.uasset");
        Unused.Verdict = EAssetUsageVerdict::Unreferenced;

        return Result;
    }
}

BEGIN_DEFINE_SPEC(AssetUsageReportWriterSpec,
    "AssetUsageAudit.ReportWriter",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
END_DEFINE_SPEC(AssetUsageReportWriterSpec)

void AssetUsageReportWriterSpec::Define()
{
    Describe("EscapeCsvField", [this]()
    {
        It("leaves a plain field untouched", [this]()
        {
            TestEqual(TEXT("plain"), FAssetUsageReportWriter::EscapeCsvField(TEXT("SM_Rock"), TEXT(';')), FString(TEXT("SM_Rock")));
        });

        It("quotes a field containing the delimiter", [this]()
        {
            TestEqual(TEXT("delimiter"),
                FAssetUsageReportWriter::EscapeCsvField(TEXT("A;B"), TEXT(';')), FString(TEXT("\"A;B\"")));
        });

        It("does not quote a field containing a different delimiter", [this]()
        {
            TestEqual(TEXT("other delimiter"),
                FAssetUsageReportWriter::EscapeCsvField(TEXT("A,B"), TEXT(';')), FString(TEXT("A,B")));
        });

        It("doubles embedded quotes", [this]()
        {
            TestEqual(TEXT("quotes"),
                FAssetUsageReportWriter::EscapeCsvField(TEXT("say \"hi\""), TEXT(';')), FString(TEXT("\"say \"\"hi\"\"\"")));
        });

        It("quotes a field containing a newline", [this]()
        {
            // Route strings are built from node titles, and Blueprint node titles really do
            // contain newlines. An unquoted one would split the row and corrupt the whole file.
            TestEqual(TEXT("newline"),
                FAssetUsageReportWriter::EscapeCsvField(TEXT("A\nB"), TEXT(';')), FString(TEXT("\"A\nB\"")));
        });
    });

    Describe("BuildCsv", [this]()
    {
        It("emits one header row and one row per asset", [this]()
        {
            const FAssetUsageAuditResult Sample = MakeSampleResult();
            FAssetUsageReportWriter::FOptions Options;

            const FString Csv = FAssetUsageReportWriter::BuildCsv(Sample, Options);

            TArray<FString> Lines;
            Csv.ParseIntoArrayLines(Lines);

            const int32 CommentLines = Lines.FilterByPredicate([](const FString& Line) { return Line.StartsWith(TEXT("#")); }).Num();

            TestTrue(TEXT("has comment header"), CommentLines > 0);
            TestEqual(TEXT("comments + header + 2 rows"), Lines.Num(), CommentLines + 1 + 2);
        });

        It("records the filters that produced the report", [this]()
        {
            const FAssetUsageAuditResult Sample = MakeSampleResult();
            const FString Csv = FAssetUsageReportWriter::BuildCsv(Sample, FAssetUsageReportWriter::FOptions());

            TestTrue(TEXT("filter recorded"), Csv.Contains(TEXT("# Filter: Types: StaticMesh")));
        });

        It("joins multiple levels into one cell with the configured separator", [this]()
        {
            const FAssetUsageAuditResult Sample = MakeSampleResult();
            FAssetUsageReportWriter::FOptions Options;
            Options.CsvMultiValueSeparator = TEXT("|");

            const FString Csv = FAssetUsageReportWriter::BuildCsv(Sample, Options);

            TestTrue(TEXT("levels joined"),
                Csv.Contains(TEXT("/Game/Maps/WP_Example|/Game/Maps/L_3i_Hub")));
        });

        It("carries the verdict verbatim", [this]()
        {
            const FString Csv = FAssetUsageReportWriter::BuildCsv(MakeSampleResult(), FAssetUsageReportWriter::FOptions());

            TestTrue(TEXT("UsedOnLevel present"), Csv.Contains(TEXT("UsedOnLevel")));
            TestTrue(TEXT("Unreferenced present"), Csv.Contains(TEXT("Unreferenced")));
        });

        It("warns in the header that Unknown does not mean unused", [this]()
        {
            // The single most dangerous misreading of this report. It must be stated in the file
            // itself, because the file outlives the UI that explained it.
            const FString Csv = FAssetUsageReportWriter::BuildCsv(MakeSampleResult(), FAssetUsageReportWriter::FOptions());

            TestTrue(TEXT("warning present"), Csv.Contains(TEXT("does NOT mean unused")));
        });
    });

    Describe("BuildJson", [this]()
    {
        It("produces valid, parseable JSON", [this]()
        {
            const FString Json = FAssetUsageReportWriter::BuildJson(MakeSampleResult());

            TSharedPtr<FJsonObject> Root;
            const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);

            TestTrue(TEXT("parses"), FJsonSerializer::Deserialize(Reader, Root) && Root.IsValid());
        });

        It("carries the header, verdict counts and one entry per asset", [this]()
        {
            const FString Json = FAssetUsageReportWriter::BuildJson(MakeSampleResult());

            TSharedPtr<FJsonObject> Root;
            const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
            if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
            {
                AddError(TEXT("JSON did not parse"));
                return;
            }

            const TSharedPtr<FJsonObject>* Header = nullptr;
            TestTrue(TEXT("header object"), Root->TryGetObjectField(TEXT("header"), Header));
            if (Header)
            {
                TestEqual(TEXT("project"), (*Header)->GetStringField(TEXT("project")), FString(TEXT("MyProject")));
                TestEqual(TEXT("levelsScanned"), (*Header)->GetIntegerField(TEXT("levelsScanned")), 2);
            }

            const TSharedPtr<FJsonObject>* Counts = nullptr;
            TestTrue(TEXT("verdictCounts object"), Root->TryGetObjectField(TEXT("verdictCounts"), Counts));
            if (Counts)
            {
                TestEqual(TEXT("usedOnLevel"), (*Counts)->GetIntegerField(TEXT("usedOnLevel")), 1);
                TestEqual(TEXT("unreferenced"), (*Counts)->GetIntegerField(TEXT("unreferenced")), 1);
            }

            const TArray<TSharedPtr<FJsonValue>>* Assets = nullptr;
            TestTrue(TEXT("assets array"), Root->TryGetArrayField(TEXT("assets"), Assets));
            if (Assets)
            {
                TestEqual(TEXT("two assets"), Assets->Num(), 2);
            }
        });

        It("keeps the level list nested rather than flattened to a string", [this]()
        {
            // The whole point of shipping JSON alongside CSV: a consumer should not have to
            // re-split a delimited cell to learn which levels use an asset.
            const FString Json = FAssetUsageReportWriter::BuildJson(MakeSampleResult());

            TSharedPtr<FJsonObject> Root;
            const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
            if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
            {
                AddError(TEXT("JSON did not parse"));
                return;
            }

            const TArray<TSharedPtr<FJsonValue>>* Assets = nullptr;
            if (!Root->TryGetArrayField(TEXT("assets"), Assets) || Assets->Num() == 0)
            {
                AddError(TEXT("assets array missing"));
                return;
            }

            const TSharedPtr<FJsonObject> First = (*Assets)[0]->AsObject();
            const TArray<TSharedPtr<FJsonValue>>* Levels = nullptr;

            TestTrue(TEXT("levels is an array"), First->TryGetArrayField(TEXT("levels"), Levels));
            if (Levels)
            {
                TestEqual(TEXT("two levels"), Levels->Num(), 2);
            }
        });
    });
}

#endif // WITH_DEV_AUTOMATION_TESTS
