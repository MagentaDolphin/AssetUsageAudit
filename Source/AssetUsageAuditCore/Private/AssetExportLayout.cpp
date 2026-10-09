// MagentaDolphin 2026. Asset Usage Audit.

#include "AssetExportLayout.h"

#include "AssetTypeFilter.h"
#include "AssetUsageAuditCoreModule.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Misc/PackageName.h"
#include "Misc/PathViews.h"

namespace AssetExportLayout
{
    namespace
    {
        /** Folder for anything no preset covers. Never empty - see the header. */
        const TCHAR* OtherFolderName = TEXT("Other");

        /**
         * Make a package name safe to use as a folder name.
         *
         * Asset names are already constrained by Unreal, but a folder name is not the same thing as
         * an asset name and the target may be any filesystem. FPaths::MakeValidFileName replaces
         * what Windows rejects; the empty check catches a name that was nothing but invalid
         * characters, which would otherwise produce a group writing into the export root.
         */
        FString MakeFolderName(FName PackageName)
        {
            FString Name = FPackageName::GetShortName(PackageName.ToString());
            Name = FPaths::MakeValidFileName(Name, TEXT('_'));

            return Name.IsEmpty() ? FString(TEXT("Asset")) : Name;
        }
    }

    FString FStats::Summarise() const
    {
        TArray<FString> Parts;

        Parts.Add(FString::Printf(TEXT("%d group(s)"), GroupCount));
        Parts.Add(FString::Printf(TEXT("%d distinct package(s)"), DistinctPackages));

        // Always stated, even when equal to DistinctPackages: under FolderPerAsset the gap between
        // the two is the surprise, and a reader should not have to infer that they match.
        Parts.Add(FString::Printf(TEXT("%d file placement(s)"), FilePlacements));

        if (RenamedFolders > 0)
        {
            Parts.Add(FString::Printf(TEXT("%d folder name(s) made unique"), RenamedFolders));
        }

        return FString::Join(Parts, TEXT(", "));
    }

    TMap<FTopLevelAssetPath, FString> BuildTypeFolderMap(IAssetRegistry& Registry)
    {
        TMap<FTopLevelAssetPath, FString> FolderByClass;

        for (EAssetTypePreset Preset : GetAllAssetTypePresets())
        {
            const TArray<FTopLevelAssetPath> Seeds = GetPresetClassPaths(Preset);
            if (Seeds.IsEmpty())
            {
                continue;
            }

            const FString FolderName = LexToString(Preset);

            TSet<FTopLevelAssetPath> Derived;
            Registry.GetDerivedClassNames(Seeds, TSet<FTopLevelAssetPath>(), Derived);

            // Seeds added explicitly: GetDerivedClassNames returns strict descendants in some
            // builds and includes the seeds in others. The same reason FAssetTypeFilter::Compile
            // does it, and the same consequence if it is forgotten - Material would match every
            // material instance but not Material itself.
            for (const FTopLevelAssetPath& ClassPath : Seeds)
            {
                FolderByClass.FindOrAdd(ClassPath, FolderName);
            }

            for (const FTopLevelAssetPath& ClassPath : Derived)
            {
                // FindOrAdd, not Add: presets overlap (a Niagara system is also a UObject subclass
                // reachable from more than one seed) and the first preset to claim a class wins.
                // Overwriting would make the folder depend on preset iteration order.
                FolderByClass.FindOrAdd(ClassPath, FolderName);
            }
        }

        return FolderByClass;
    }

    FString TypeFolderForPackage(IAssetRegistry& Registry, FName PackageName, const TMap<FTopLevelAssetPath, FString>& FolderByClass)
    {
        TArray<FAssetData> Assets;
        Registry.GetAssetsByPackageName(PackageName, Assets, /*bIncludeOnlyOnDiskAssets=*/true);

        for (const FAssetData& AssetData : Assets)
        {
            if (const FString* Folder = FolderByClass.Find(AssetData.AssetClassPath))
            {
                return *Folder;
            }
        }

        return OtherFolderName;
    }

    TArray<FGroup> BuildGroups(IAssetRegistry& Registry, const TArray<AssetDependencyClosure::FSeedClosure>& SeedClosures, const FOptions& Options, FStats& OutStats)
    {
        OutStats = FStats();

        TArray<FGroup> Groups;
        TSet<FName> DistinctPackages;

        // Flat and MirrorTree do not care which seed pulled a package in, so they collapse to one
        // group and let the exporter's own bFlatten decide the path. FolderPerAsset is the only
        // layout that needs the per-seed structure, and it is the only one that pays for it.
        if (Options.Layout != EExportLayout::FolderPerAsset)
        {
            FGroup& Group = Groups.AddDefaulted_GetRef();

            for (const AssetDependencyClosure::FSeedClosure& Closure : SeedClosures)
            {
                for (FName Package : Closure.Packages)
                {
                    if (!DistinctPackages.Contains(Package))
                    {
                        DistinctPackages.Add(Package);
                        Group.Packages.Add(Package);
                    }
                }
            }

            OutStats.GroupCount = Groups.Num();
            OutStats.DistinctPackages = DistinctPackages.Num();
            OutStats.FilePlacements = Group.Packages.Num();

            UE_LOG(LogAssetUsageAudit, Log, TEXT("Export layout %s: %s"), LexToString(Options.Layout), *OutStats.Summarise());

            return Groups;
        }

        // Folder names must be unique across the export, and asset names are not: SM_Rock can exist
        // in three different folders in /Game. Two seeds sharing a short name would otherwise write
        // into one folder and interleave their dependencies.
        TSet<FString> UsedFolderNames;

        const TMap<FTopLevelAssetPath, FString> FolderByClass = Options.bGroupDependenciesByType
            ? BuildTypeFolderMap(Registry)
            : TMap<FTopLevelAssetPath, FString>();

        for (const AssetDependencyClosure::FSeedClosure& Closure : SeedClosures)
        {
            if (Closure.Packages.IsEmpty())
            {
                continue;
            }

            FString FolderName = MakeFolderName(Closure.Seed);

            if (UsedFolderNames.Contains(FolderName))
            {
                const FString Base = FolderName;
                int32 Suffix = 1;

                while (UsedFolderNames.Contains(FolderName))
                {
                    FolderName = FString::Printf(TEXT("%s_%d"), *Base, Suffix++);
                }

                ++OutStats.RenamedFolders;
            }

            UsedFolderNames.Add(FolderName);

            // The seed sits at the root of its own folder; dependencies go beside it, or into
            // per-type subfolders when asked. Grouping the seed too would bury the subject of the
            // folder among the things it needs.
            FGroup& SeedGroup = Groups.AddDefaulted_GetRef();
            SeedGroup.RelativeDir = FolderName;
            SeedGroup.Packages.Add(Closure.Seed);

            DistinctPackages.Add(Closure.Seed);
            ++OutStats.FilePlacements;

            // One group per type subfolder, built lazily so a folder is only created for a type
            // that actually has files in it.
            TMap<FString, int32> GroupIndexByDir;

            for (FName Package : Closure.Packages)
            {
                if (Package == Closure.Seed)
                {
                    continue;
                }

                DistinctPackages.Add(Package);
                ++OutStats.FilePlacements;

                FString RelativeDir = FolderName;

                if (Options.bGroupDependenciesByType)
                {
                    RelativeDir = FPaths::Combine(FolderName, TypeFolderForPackage(Registry, Package, FolderByClass));
                }

                if (int32* ExistingIndex = GroupIndexByDir.Find(RelativeDir))
                {
                    Groups[*ExistingIndex].Packages.Add(Package);
                    continue;
                }

                // The dependency group for the seed's own folder is the one already created above;
                // reuse it rather than adding a second group with the same RelativeDir, which the
                // exporter would treat as a separate destination.
                if (RelativeDir == FolderName)
                {
                    SeedGroup.Packages.Add(Package);
                    GroupIndexByDir.Add(RelativeDir, Groups.Num() - 1);
                    continue;
                }

                FGroup& TypeGroup = Groups.AddDefaulted_GetRef();
                TypeGroup.RelativeDir = RelativeDir;
                TypeGroup.Packages.Add(Package);

                GroupIndexByDir.Add(RelativeDir, Groups.Num() - 1);
            }
        }

        OutStats.GroupCount = Groups.Num();
        OutStats.DistinctPackages = DistinctPackages.Num();

        UE_LOG(LogAssetUsageAudit, Log, TEXT("Export layout %s: %s"), LexToString(Options.Layout), *OutStats.Summarise());

        return Groups;
    }
}
