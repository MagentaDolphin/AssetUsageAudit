// MagentaDolphin 2026. Asset Usage Audit.

#include "AssetDeletionScan.h"

#include "AssetRegistry/IAssetRegistry.h"
#include "AssetUsageAuditTypes.h"
#include "AssetUsagePaths.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "Misc/PackageName.h"

#define LOCTEXT_NAMESPACE "AssetDeletionScan"

namespace AssetDeletionScan
{
    const TCHAR* LexToString(ERefusal Refusal)
    {
        switch (Refusal)
        {
        case ERefusal::None:              return TEXT("None");
        case ERefusal::IsLevel:           return TEXT("IsLevel");
        case ERefusal::IsExternalPackage: return TEXT("IsExternalPackage");
        case ERefusal::NotProjectContent: return TEXT("NotProjectContent");
        case ERefusal::Missing:           return TEXT("Missing");
        }

        return TEXT("Unknown");
    }

    FText DescribeRefusal(ERefusal Refusal)
    {
        switch (Refusal)
        {
        case ERefusal::None:
            return FText::GetEmpty();

        case ERefusal::IsLevel:
            return LOCTEXT("RefusalLevel",
                "Levels are never deleted by this tool. Delete a map from the Content Browser if you really mean to.");

        case ERefusal::IsExternalPackage:
            return LOCTEXT("RefusalExternal",
                "This is a single actor stored outside its map (One File Per Actor). Delete it in the level editor, where it can be undone.");

        case ERefusal::NotProjectContent:
            return LOCTEXT("RefusalNotProject",
                "Engine or plugin content - not part of this project.");

        case ERefusal::Missing:
            return LOCTEXT("RefusalMissing",
                "No asset found under this package name; nothing to delete.");
        }

        return FText::GetEmpty();
    }

    FString FStats::Summarise() const
    {
        return FString::Printf(
            TEXT("%d ticked: %d deletable, %d refused (levels %d, external actors %d, engine %d, missing %d); ")
            TEXT("%d still referenced from outside the set, %d read-only on disk."),
            Requested,
            Deletable,
            Requested - Deletable,
            RefusedLevels,
            RefusedExternal,
            RefusedNotProjectContent,
            RefusedMissing,
            WithOutsideReferencers,
            ReadOnly);
    }

    TArray<FCandidate> Scan(
        IAssetRegistry& Registry,
        const TArray<FName>& Packages,
        const FOptions& Options,
        FStats& OutStats)
    {
        OutStats = FStats();

        TArray<FCandidate> Candidates;
        Candidates.Reserve(Packages.Num());

        // Membership is needed while classifying referencers, so it is built first and in full.
        // Deleting A and B where B is A's only referencer must not report A as still in use.
        TSet<FName> DeleteSet(Packages);

        // Answering "is this a map" costs a registry query that allocates; the same package shows up
        // as a referencer of many candidates, so the answer is cached across the whole scan.
        TMap<FName, bool> LevelCache;

        const auto IsLevelPackage = [&Registry, &LevelCache](FName PackageName) -> bool
        {
            if (const bool* Cached = LevelCache.Find(PackageName))
            {
                return *Cached;
            }

            TArray<FAssetData> Assets;
            Registry.GetAssetsByPackageName(PackageName, Assets, /*bIncludeOnlyOnDiskAssets=*/true);

            bool bIsLevel = false;
            for (const FAssetData& AssetData : Assets)
            {
                if (AssetData.AssetClassPath == UWorld::StaticClass()->GetClassPathName())
                {
                    bIsLevel = true;
                    break;
                }
            }

            LevelCache.Add(PackageName, bIsLevel);
            return bIsLevel;
        };

        TArray<FName> Referencers;
        TArray<FAssetData> Assets;

        for (FName PackageName : Packages)
        {
            if (PackageName.IsNone())
            {
                continue;
            }

            ++OutStats.Requested;

            FCandidate Candidate;
            Candidate.PackageName = PackageName;

            const FString PackageString = PackageName.ToString();

            // Filled from the path before any refusal, so a refused row still shows a name and a
            // path. A row that says only "refused" with an empty name is not an explanation.
            Candidate.AssetName = FName(*FPackageName::GetShortName(PackageString));
            Candidate.PathFromProjectRoot = AssetUsagePaths::ToProjectRelativePath(PackageName, /*bIsLevel=*/false);

            // Path-shaped refusals come first: they need no registry lookup, and two of them
            // (script and engine content) would not produce a sensible FAssetData anyway.
            if (AssetUsagePaths::IsScriptPackage(PackageName) || AssetUsagePaths::IsEngineOrTempPackage(PackageString))
            {
                Candidate.Refusal = ERefusal::NotProjectContent;
                ++OutStats.RefusedNotProjectContent;
                Candidates.Add(MoveTemp(Candidate));
                continue;
            }

            if (AssetUsagePaths::IsExternalPackage(PackageName))
            {
                Candidate.Refusal = ERefusal::IsExternalPackage;
                ++OutStats.RefusedExternal;
                Candidates.Add(MoveTemp(Candidate));
                continue;
            }

            Assets.Reset();
            Registry.GetAssetsByPackageName(PackageName, Assets, /*bIncludeOnlyOnDiskAssets=*/true);

            if (Assets.Num() == 0)
            {
                Candidate.Refusal = ERefusal::Missing;
                ++OutStats.RefusedMissing;
                Candidates.Add(MoveTemp(Candidate));
                continue;
            }

            // The package's own asset, preferring the one the package is named after. A package
            // usually holds exactly one, but the first entry is not guaranteed to be the UAsset.
            const FAssetData* Primary = Assets.FindByPredicate([](const FAssetData& Data) { return Data.IsUAsset(); });
            const FAssetData& AssetData = Primary ? *Primary : Assets[0];

            Candidate.AssetName = AssetData.AssetName;
            Candidate.ClassPath = AssetData.AssetClassPath;

            const bool bIsLevel = AssetData.AssetClassPath == UWorld::StaticClass()->GetClassPathName();
            LevelCache.Add(PackageName, bIsLevel);

            Candidate.PathFromProjectRoot = AssetUsagePaths::ToProjectRelativePath(PackageName, bIsLevel);

            if (bIsLevel)
            {
                Candidate.Refusal = ERefusal::IsLevel;
                ++OutStats.RefusedLevels;
                Candidates.Add(MoveTemp(Candidate));
                continue;
            }

            // From here the candidate is deletable, and the remaining work is describing the
            // consequences rather than deciding them.
            Referencers.Reset();
            Registry.GetReferencers(
                PackageName, Referencers,
                AssetUsageAudit::MakeTraversalCategory(), AssetUsageAudit::MakeTraversalQuery());

            for (FName Referencer : Referencers)
            {
                if (Referencer == PackageName || DeleteSet.Contains(Referencer))
                {
                    continue;
                }

                ++Candidate.OutsideReferencerCount;

                // An OFPA package is counted as a level reference without a registry lookup: it
                // exists only to hold an actor placed in some map, so a reference from one means
                // the asset is in a level right now.
                if (AssetUsagePaths::IsExternalPackage(Referencer) || IsLevelPackage(Referencer))
                {
                    ++Candidate.LevelReferencerCount;
                }

                if (Candidate.OutsideReferencers.Num() < Options.MaxReferencersListed)
                {
                    Candidate.OutsideReferencers.Add(Referencer);
                }
            }

            FString Filename;
            if (FPackageName::DoesPackageExist(PackageString, &Filename))
            {
                Candidate.bReadOnlyOnDisk = IFileManager::Get().IsReadOnly(*Filename);
            }

            ++OutStats.Deletable;

            if (Candidate.OutsideReferencerCount > 0)
            {
                ++OutStats.WithOutsideReferencers;
            }

            if (Candidate.bReadOnlyOnDisk)
            {
                ++OutStats.ReadOnly;
            }

            Candidates.Add(MoveTemp(Candidate));
        }

        return Candidates;
    }
}

#undef LOCTEXT_NAMESPACE
