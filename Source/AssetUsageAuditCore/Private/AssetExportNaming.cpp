// MagentaDolphin 2026. Asset Usage Audit.

#include "AssetExportNaming.h"
#include "AssetUsageAuditCoreModule.h"

namespace AssetExportNaming
{
    namespace
    {
        /** Guard against a pathological folder where every candidate is taken. */
        constexpr int32 MaxCollisionAttempts = 100000;

        bool IsAllDigits(const FString& Value)
        {
            if (Value.IsEmpty())
            {
                return false;
            }

            for (const TCHAR Char : Value)
            {
                if (!FChar::IsDigit(Char))
                {
                    return false;
                }
            }

            return true;
        }
    }

    void SplitTrailingIndex(const FString& BaseName, FString& OutStem, int32& OutIndex, int32& OutPadWidth)
    {
        OutStem = BaseName;
        OutIndex = INDEX_NONE;
        OutPadWidth = 0;

        int32 SeparatorPos = INDEX_NONE;
        if (!BaseName.FindLastChar(TEXT('_'), SeparatorPos))
        {
            return;
        }

        // A leading underscore is part of the name, not a separator: "_1" has no stem.
        if (SeparatorPos <= 0)
        {
            return;
        }

        const FString Suffix = BaseName.Mid(SeparatorPos + 1);

        // "Foo_" has an empty suffix, "SM_Rock_02_v3" has a non-numeric one. Neither is an index.
        if (!IsAllDigits(Suffix))
        {
            return;
        }

        OutStem = BaseName.Left(SeparatorPos);
        OutIndex = FCString::Atoi(*Suffix);
        OutPadWidth = Suffix.Len();
    }

    FString ComposeIndexedName(const FString& Stem, int32 Index, int32 PadWidth)
    {
        if (Index == INDEX_NONE)
        {
            return Stem;
        }

        FString Digits = FString::FromInt(Index);

        // Preserve the original zero padding, but never truncate: Foo_09 -> Foo_10, Foo_99 -> Foo_100.
        while (Digits.Len() < PadWidth)
        {
            Digits.InsertAt(0, TEXT('0'));
        }

        return FString::Printf(TEXT("%s_%s"), *Stem, *Digits);
    }

    FString ResolveCollision(const FString& DesiredFileName, EExportCollisionPolicy Policy, FExistsPredicate Exists)
    {
        if (Policy == EExportCollisionPolicy::Overwrite)
        {
            return DesiredFileName;
        }

        if (!Exists(DesiredFileName))
        {
            return DesiredFileName;
        }

        // Split off the extension so the index lands before it: SM_Rock.uasset -> SM_Rock_1.uasset.
        // Note this splits on the LAST dot, so a name like "M_Glass.v2.uasset" keeps "M_Glass.v2" as the base.
        FString BaseName = DesiredFileName;
        FString Extension;

        int32 DotPos = INDEX_NONE;
        if (DesiredFileName.FindLastChar(TEXT('.'), DotPos) && DotPos > 0)
        {
            BaseName = DesiredFileName.Left(DotPos);
            Extension = DesiredFileName.Mid(DotPos);  // includes the dot
        }

        FString Stem;
        int32 Index = INDEX_NONE;
        int32 PadWidth = 0;
        SplitTrailingIndex(BaseName, Stem, Index, PadWidth);

        // No trailing index yet: start at _1. Otherwise increment what is already there,
        // rather than appending a second suffix and producing Foo_1_1.
        int32 NextIndex = (Index == INDEX_NONE) ? 1 : Index + 1;
        const int32 EffectivePadWidth = (Index == INDEX_NONE) ? 1 : PadWidth;

        for (int32 Attempt = 0; Attempt < MaxCollisionAttempts; ++Attempt, ++NextIndex)
        {
            const FString Candidate = ComposeIndexedName(Stem, NextIndex, EffectivePadWidth) + Extension;
            if (!Exists(Candidate))
            {
                return Candidate;
            }
        }

        // Every candidate taken. Returning the desired name would silently overwrite, which is the
        // one outcome the Index policy exists to prevent - so fail loudly instead.
        UE_LOG(LogAssetUsageAudit, Error,
            TEXT("Could not find a free filename for '%s' after %d attempts. Export of this asset will be skipped."),
            *DesiredFileName, MaxCollisionAttempts);

        return FString();
    }
}
