// MagentaDolphin 2026. Asset Usage Audit.

#include "AssetUsagePaths.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "Misc/StringBuilder.h"
#include "String/Find.h"

namespace AssetUsagePaths
{
    namespace Private
    {
        // These two literals match FPackagePath's folder names (CoreUObject PackagePath.cpp).
        // They are used only to CLASSIFY an existing path. Producing an external-actor path is
        // ULevel::GetExternalActorsPaths's job, because that form has /CB/ and /EDL/ variants and
        // is extensible by plugin delegates - string building it here would be silently wrong.
        static const TCHAR* ExternalActorsFolder = TEXT("__ExternalActors__");
        static const TCHAR* ExternalObjectsFolder = TEXT("__ExternalObjects__");

        static const TCHAR* GameRoot = TEXT("/Game");
        static const TCHAR* ContentPrefix = TEXT("Content");

        /** Prefix match that only accepts a whole path segment, so /Game/Art misses /Game/ArtSource. */
        static bool StartsWithSegment(FStringView Path, FStringView Prefix)
        {
            if (Prefix.IsEmpty() || Path.Len() < Prefix.Len())
            {
                return false;
            }

            if (!Path.Left(Prefix.Len()).Equals(Prefix, ESearchCase::IgnoreCase))
            {
                return false;
            }

            return Path.Len() == Prefix.Len() || Path[Prefix.Len()] == TEXT('/');
        }
    }

    bool IsExternalPackage(FStringView PackagePath)
    {
        return UE::String::FindFirst(PackagePath, Private::ExternalActorsFolder, ESearchCase::IgnoreCase) != INDEX_NONE
            || UE::String::FindFirst(PackagePath, Private::ExternalObjectsFolder, ESearchCase::IgnoreCase) != INDEX_NONE;
    }

    bool IsExternalPackage(FName PackageName)
    {
        TStringBuilder<256> Builder;
        PackageName.ToString(Builder);
        return IsExternalPackage(Builder.ToView());
    }

    bool IsScriptPackage(FStringView PackagePath)
    {
        return Private::StartsWithSegment(PackagePath, TEXTVIEW("/Script"));
    }

    bool IsScriptPackage(FName PackageName)
    {
        TStringBuilder<256> Builder;
        PackageName.ToString(Builder);
        return IsScriptPackage(Builder.ToView());
    }

    bool IsEngineOrTempPackage(FStringView PackagePath)
    {
        return Private::StartsWithSegment(PackagePath, TEXTVIEW("/Engine"))
            || Private::StartsWithSegment(PackagePath, TEXTVIEW("/Temp"));
    }

    FString NormalizeExclusionPrefix(const FString& Prefix)
    {
        FString Working = Prefix;
        Working.TrimStartAndEndInline();
        Working.ReplaceInline(TEXT("\\"), TEXT("/"), ESearchCase::CaseSensitive);

        while (Working.EndsWith(TEXT("/"), ESearchCase::CaseSensitive))
        {
            Working.LeftChopInline(1, EAllowShrinking::No);
        }

        if (Working.IsEmpty())
        {
            return FString();
        }

        // Content-relative form, which is what the UI shows and what settings files accumulate.
        if (Private::StartsWithSegment(Working, Private::ContentPrefix))
        {
            const FString Remainder = Working.RightChop(FCString::Strlen(Private::ContentPrefix));
            return FString(Private::GameRoot) + Remainder;
        }

        if (!Working.StartsWith(TEXT("/"), ESearchCase::CaseSensitive))
        {
            Working.InsertAt(0, TEXT("/"));
        }

        return Working;
    }

    bool IsPathExcluded(FStringView PackagePath, const TArray<FString>& ExcludedPrefixes)
    {
        for (const FString& RawPrefix : ExcludedPrefixes)
        {
            const FString Normalized = NormalizeExclusionPrefix(RawPrefix);
            if (!Normalized.IsEmpty() && Private::StartsWithSegment(PackagePath, Normalized))
            {
                return true;
            }
        }

        return false;
    }

    bool IsPathExcluded(FName PackageName, const TArray<FString>& ExcludedPrefixes)
    {
        if (ExcludedPrefixes.IsEmpty())
        {
            return false;
        }

        TStringBuilder<256> Builder;
        PackageName.ToString(Builder);
        return IsPathExcluded(Builder.ToView(), ExcludedPrefixes);
    }

    FString ToProjectRelativePath(FName PackageName, bool bIsLevel)
    {
        TStringBuilder<256> Builder;
        PackageName.ToString(Builder);
        const FStringView PackagePath = Builder.ToView();

        if (!Private::StartsWithSegment(PackagePath, Private::GameRoot))
        {
            // /Script, /Engine and plugin mounts have no project-relative form. Reporting an
            // invented path for them would send an artist looking for a file that is not there.
            return FString();
        }

        const FStringView Remainder = PackagePath.RightChop(FCString::Strlen(Private::GameRoot));
        const TCHAR* Extension = bIsLevel ? TEXT(".umap") : TEXT(".uasset");

        return FString::Printf(TEXT("Content%.*s%s"), Remainder.Len(), Remainder.GetData(), Extension);
    }

    FString ValidateMigrateDestination(const FString& Directory)
    {
        if (Directory.IsEmpty())
        {
            return TEXT("Choose the destination project's Content folder.");
        }

        FString Normalised = Directory;
        FPaths::NormalizeDirectoryName(Normalised);
        Normalised += TEXT("/");

        if (!Normalised.EndsWith(TEXT("/Content/")))
        {
            return TEXT("Migrate needs a folder named Content - pick the destination project's Content folder itself, not the project root.");
        }

        // Second rule. A folder merely named Content passes the first check and Migrate still
        // aborts: the destination mount point is derived from what sits beside it.
        const FString Parent = Normalised + TEXT("../");

        TArray<FString> Found;
        IFileManager::Get().FindFiles(Found, *Parent, TEXT("uproject"));

        if (!Found.IsEmpty())
        {
            return FString();
        }

        IFileManager::Get().FindFiles(Found, *Parent, TEXT("uplugin"));

        if (Found.Num() == 1)
        {
            return FString();
        }

        return Found.Num() > 1
            ? TEXT("That folder's parent holds several .uplugin files, so Unreal cannot tell which mount point to migrate into.")
            : TEXT("No .uproject or .uplugin next to that Content folder, so Unreal has no mount point to migrate into.");
    }
}
