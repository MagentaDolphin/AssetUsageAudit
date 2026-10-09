// MagentaDolphin 2026. Asset Usage Audit.

#pragma once

#include "CoreMinimal.h"

/**
 * One /Game path literal found in a text file, with enough context to check it by hand.
 */
struct FIndirectReference
{
    /** Package name the literal resolved to, e.g. /Game/Core/GameModes/BP_GameMode. */
    FName PackageName;

    /** Absolute path of the file the literal was found in. */
    FString SourceFile;

    /** 1-based line number. */
    int32 LineNumber = 0;

    /** True when the hit came from Config/, false when from Source/. */
    bool bFromConfig = false;

    FString ToProvenanceString() const;
};

/**
 * Finds asset paths referenced from text rather than from a package.
 *
 * This exists because of a concrete, measured failure mode on this project: Config/DefaultEngine.ini
 * names BP_GameMode, BP_MenuGameInstance, RefinedMenuMap and Gyms_Geoda as plain
 * strings. No asset references them, so the Asset Registry reports zero referencers and a naive
 * audit calls the project's GameMode unused.
 *
 * It is a mitigation, not a solution. It cannot see a path assembled at runtime by concatenation,
 * an OpenLevel(FName) call, or an FMOD event path - those stay Unknown, deliberately.
 */
class ASSETUSAGEAUDITCORE_API FIndirectReferenceScanner
{
public:
    struct FOptions
    {
        /** Absolute directories to scan. Defaults to <Project>/Config and <Project>/Source. */
        TArray<FString> Directories;

        /** File extensions to read, lowercase, with the dot. */
        TArray<FString> Extensions = { TEXT(".ini"), TEXT(".cpp"), TEXT(".h"), TEXT(".cs") };

        /** Skip files larger than this; a multi-megabyte generated file is never a reference site. */
        int64 MaxFileSizeBytes = 8 * 1024 * 1024;
    };

    /** Scan and return every distinct package path found, with provenance. */
    static TArray<FIndirectReference> Scan(const FOptions& Options);

    /** Default options: <Project>/Config and <Project>/Source. */
    static FOptions MakeDefaultOptions();

    /**
     * Extract /Game path literals from one line of text.
     *
     * Exposed for testing: the trailing-_C strip and the delimiter set are where this goes wrong.
     * "/Game/X/BP_Y.BP_Y_C" and "/Game/X/BP_Y" must both yield the package /Game/X/BP_Y.
     */
    static void ExtractGamePathsFromLine(FStringView Line, TArray<FString>& OutPackageNames);
};
