// MagentaDolphin 2026. Asset Usage Audit.

#include "AssetUsageAuditor.h"

#include "AssetUsageAuditCoreModule.h"
#include "AssetUsagePaths.h"
#include "IndirectReferenceScanner.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "HAL/PlatformTime.h"
#include "Misc/App.h"
#include "Misc/EngineVersion.h"

namespace
{
    /** Package roots whose assets the registry cannot meaningfully track. */
    bool IsBlindSpotPackage(FName PackageName)
    {
        TStringBuilder<256> Builder;
        PackageName.ToString(Builder);
        const FStringView Path = Builder.ToView();

        // FMOD banks and events are addressed by string path through the FMOD Studio runtime.
        // Nothing in the UObject graph references them, so "no referencers" means nothing here.
        return UE::String::FindFirst(Path, TEXTVIEW("/FMOD/"), ESearchCase::IgnoreCase) != INDEX_NONE;
    }

    bool IsBlindSpotClass(const FTopLevelAssetPath& ClassPath)
    {
        if (ClassPath.IsNull())
        {
            return false;
        }

        // Anything from the FMOD module: events and banks are addressed by string, never referenced.
        if (ClassPath.GetPackageName().ToString().StartsWith(TEXT("/Script/FMODStudio"), ESearchCase::IgnoreCase))
        {
            return true;
        }

        // Data tables and curve tables are a subtler blind spot, and it cuts both ways. A consumer
        // depends on the whole table, so every row inside it looks used even when nothing reads it;
        // and rows holding paths as plain strings rather than TSoftObjectPtr produce no edge at all.
        // Neither "used" nor "unused" is defensible for the table's contents.
        static const TSet<FTopLevelAssetPath> BlindSpotClasses = {
            FTopLevelAssetPath(TEXT("/Script/Engine.DataTable")),
            FTopLevelAssetPath(TEXT("/Script/Engine.CurveTable"))
        };

        return BlindSpotClasses.Contains(ClassPath);
    }
}

int32 FAssetUsageAuditResult::CountByVerdict(EAssetUsageVerdict Verdict) const
{
    int32 Count = 0;
    for (const FAssetUsageRow& Row : Rows)
    {
        if (Row.Verdict == Verdict)
        {
            ++Count;
        }
    }
    return Count;
}

bool FAssetUsageAuditor::IsRegistryBlindSpot(const FAssetUsageNode& Node)
{
    return IsBlindSpotPackage(Node.PackageName) || IsBlindSpotClass(Node.ClassPath);
}

EAssetUsageVerdict FAssetUsageAuditor::ClassifyVerdict(bool bReachableFromLevel, bool bHasReferencers, bool bFoundInConfigOrSource, bool bIsBlindSpot)
{
    // Order matters, and it is chosen so the tool never overstates.
    //
    // Positive evidence wins over blind-spot status: if a level demonstrably reaches an FMOD bank,
    // saying "Unknown" would throw away a fact we actually have.
    if (bReachableFromLevel)
    {
        return EAssetUsageVerdict::UsedOnLevel;
    }

    if (bFoundInConfigOrSource)
    {
        return EAssetUsageVerdict::ReferencedFromConfigOrSource;
    }

    if (bHasReferencers)
    {
        return EAssetUsageVerdict::UsedByAssetsOnly;
    }

    // Absence of evidence only becomes Unreferenced when absence is actually informative.
    // For a blind spot it is not, so the honest answer is Unknown.
    if (bIsBlindSpot)
    {
        return EAssetUsageVerdict::Unknown;
    }

    return EAssetUsageVerdict::Unreferenced;
}

FAssetUsageAuditResult FAssetUsageAuditor::Run(IAssetRegistry& AssetRegistry, FAssetUsageAuditRequest& Request)
{
    FAssetUsageAuditResult Result;

    const double StartTime = FPlatformTime::Seconds();

    // --- 1. Graph -----------------------------------------------------------------------------
    FAssetUsageGraphBuildOptions GraphOptions;
    GraphOptions.IncludePackagePaths = Request.IncludePackagePaths;
    GraphOptions.ExcludePackagePaths = Request.ExcludePackagePaths;
    GraphOptions.bGatherDependencies = true;

    FAssetUsageGraph Graph;
    Graph.Build(AssetRegistry, GraphOptions);

    Result.GraphStats = Graph.GetStats();

    if (Graph.Num() == 0)
    {
        UE_LOG(LogAssetUsageAudit, Warning, TEXT("Asset graph is empty. Is the Asset Registry still scanning?"));
        return Result;
    }

    // --- 2. Level reachability ----------------------------------------------------------------
    FLevelUsageResolveOptions ResolveOptions = Request.ResolveOptions;
    ResolveOptions.LevelPackages = Request.LevelPackages;

    FLevelUsageResolver Resolver(Graph, AssetRegistry);
    const FLevelUsageResult LevelResult = Resolver.Resolve(ResolveOptions);

    Result.LevelStats = LevelResult.Stats;

    // --- 3. Indirect references ---------------------------------------------------------------
    TMap<FName, FString> IndirectProvenance;
    if (Request.bScanIndirectReferences)
    {
        for (const FIndirectReference& Reference : FIndirectReferenceScanner::Scan(FIndirectReferenceScanner::MakeDefaultOptions()))
        {
            FString& Existing = IndirectProvenance.FindOrAdd(Reference.PackageName);
            if (Existing.IsEmpty())
            {
                Existing = Reference.ToProvenanceString();
            }
        }
    }

    // --- 4. Rows ------------------------------------------------------------------------------
    Request.TypeFilter.Compile(AssetRegistry);

    // Levels-per-asset the cheap way: invert once, rather than calling GetLevelsForAsset per row,
    // which would rescan every level bitset for every asset.
    TArray<TArray<FName>> LevelsPerAsset;
    LevelsPerAsset.SetNum(Graph.Num());

    for (int32 LevelOrdinal = 0; LevelOrdinal < LevelResult.LevelReachability.Num(); ++LevelOrdinal)
    {
        const TBitArray<>& Bits = LevelResult.LevelReachability[LevelOrdinal];
        const FName LevelName = LevelResult.LevelPackageNames[LevelOrdinal];

        for (TConstSetBitIterator<> It(Bits); It; ++It)
        {
            LevelsPerAsset[It.GetIndex()].Add(LevelName);
        }
    }

    Result.Rows.Reserve(Graph.Num() / 2);

    for (int32 Index = 0; Index < Graph.Num(); ++Index)
    {
        const FAssetUsageNode& Node = Graph.GetNode(Index);

        if (Request.bHideExternalPackages && Node.bIsExternalPackage)
        {
            continue;
        }

        if (Request.bHideRedirectors && Node.bIsRedirector)
        {
            continue;
        }

        // Only project content can be exported or acted on; engine and plugin packages are noise.
        const FString RelativePath = AssetUsagePaths::ToProjectRelativePath(Node.PackageName, Node.bIsLevel);
        if (RelativePath.IsEmpty())
        {
            continue;
        }

        if (!Request.TypeFilter.Matches(Node))
        {
            continue;
        }

        const bool bReachable = LevelResult.IsReachableFromAnyLevel(Index);
        const bool bHasReferencers = Graph.GetReferencers(Index).Num() > 0;
        const FString* IndirectDetail = IndirectProvenance.Find(Node.PackageName);
        const bool bIsBlindSpot = IsRegistryBlindSpot(Node);

        const EAssetUsageVerdict Verdict = ClassifyVerdict(bReachable, bHasReferencers, IndirectDetail != nullptr, bIsBlindSpot);

        if (Request.bOnlyUnusedAssets && Verdict == EAssetUsageVerdict::UsedOnLevel)
        {
            continue;
        }

        FAssetUsageRow& Row = Result.Rows.AddDefaulted_GetRef();
        Row.PackageName = Node.PackageName;
        Row.AssetName = Node.AssetName;
        Row.ClassPath = Node.ClassPath;
        Row.GeneratedClassPath = Node.GeneratedClassPath;
        Row.PathFromProjectRoot = RelativePath;
        Row.Verdict = Verdict;
        Row.Levels = MoveTemp(LevelsPerAsset[Index]);

        if (LevelResult.Provenance.IsValidIndex(Index))
        {
            Row.Provenance = LevelResult.Provenance[Index];
        }

        if (LevelResult.HardReferenceCounts.IsValidIndex(Index))
        {
            Row.HardReferenceCount = LevelResult.HardReferenceCounts[Index];
            Row.SoftReferenceCount = LevelResult.SoftReferenceCounts[Index];
        }

        if (const FString* Route = LevelResult.Routes.Find(Index))
        {
            Row.Route = *Route;
        }

        if (IndirectDetail)
        {
            Row.ProvenanceDetail = *IndirectDetail;
            Row.Provenance |= EAssetUsageProvenance::ConfigFile;
        }
    }

    // --- 5. Header ----------------------------------------------------------------------------
    Result.Header.GeneratedAt = FDateTime::Now();
    Result.Header.EngineVersion = FEngineVersion::Current().ToString();
    Result.Header.ToolVersion = TEXT("0.1");
    Result.Header.ProjectName = FApp::GetProjectName();
    Result.Header.LevelsScanned = LevelResult.Stats.LevelsScanned;
    Result.Header.AssetsScanned = Graph.Num();
    Result.Header.ScanDurationSeconds = FPlatformTime::Seconds() - StartTime;

    Result.Header.AppliedFilters.Add(FString::Printf(TEXT("Types: %s"), *Request.TypeFilter.Describe()));
    Result.Header.AppliedFilters.Add(FString::Printf(TEXT("Include paths: %s"),
        Request.IncludePackagePaths.IsEmpty() ? TEXT("/Game") : *FString::Join(Request.IncludePackagePaths, TEXT(", "))));

    if (!Request.ExcludePackagePaths.IsEmpty())
    {
        Result.Header.AppliedFilters.Add(FString::Printf(TEXT("Excluded paths: %s"), *FString::Join(Request.ExcludePackagePaths, TEXT(", "))));
    }

    Result.Header.AppliedFilters.Add(FString::Printf(TEXT("Levels: %s"),
        Request.LevelPackages.IsEmpty() ? TEXT("all") : *FString::Printf(TEXT("%d selected"), Request.LevelPackages.Num())));
    Result.Header.AppliedFilters.Add(FString::Printf(TEXT("Indirect scan: %s"), Request.bScanIndirectReferences ? TEXT("on") : TEXT("off")));
    Result.Header.AppliedFilters.Add(FString::Printf(TEXT("Only unused: %s"), Request.bOnlyUnusedAssets ? TEXT("yes") : TEXT("no")));

    UE_LOG(LogAssetUsageAudit, Log,
        TEXT("Audit complete in %.2fs: %d rows (%d on level, %d assets-only, %d config/source, %d unreferenced, %d unknown) across %d levels."),
        Result.Header.ScanDurationSeconds,
        Result.Rows.Num(),
        Result.CountByVerdict(EAssetUsageVerdict::UsedOnLevel),
        Result.CountByVerdict(EAssetUsageVerdict::UsedByAssetsOnly),
        Result.CountByVerdict(EAssetUsageVerdict::ReferencedFromConfigOrSource),
        Result.CountByVerdict(EAssetUsageVerdict::Unreferenced),
        Result.CountByVerdict(EAssetUsageVerdict::Unknown),
        Result.Header.LevelsScanned);

    return Result;
}
