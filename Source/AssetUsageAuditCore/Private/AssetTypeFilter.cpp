// MagentaDolphin 2026. Asset Usage Audit.

#include "AssetTypeFilter.h"

#include "AssetUsageAuditCoreModule.h"
#include "AssetUsageGraph.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "UObject/Class.h"
#include "UObject/UObjectGlobals.h"

const TCHAR* LexToString(EAssetTypePreset Preset)
{
    switch (Preset)
    {
        case EAssetTypePreset::StaticMesh:   return TEXT("StaticMesh");
        case EAssetTypePreset::SkeletalMesh: return TEXT("SkeletalMesh");
        case EAssetTypePreset::Material:     return TEXT("Material");
        case EAssetTypePreset::Texture:      return TEXT("Texture");
        case EAssetTypePreset::VFX:          return TEXT("VFX");
        case EAssetTypePreset::Sound:        return TEXT("Sound");
        case EAssetTypePreset::Blueprint:    return TEXT("Blueprint");
        case EAssetTypePreset::Level:        return TEXT("Level");
        case EAssetTypePreset::DataAsset:    return TEXT("DataAsset");
        case EAssetTypePreset::Animation:    return TEXT("Animation");
    }

    return TEXT("Unknown");
}

TArray<EAssetTypePreset> GetAllAssetTypePresets()
{
    return {
        EAssetTypePreset::StaticMesh,
        EAssetTypePreset::SkeletalMesh,
        EAssetTypePreset::Material,
        EAssetTypePreset::Texture,
        EAssetTypePreset::VFX,
        EAssetTypePreset::Sound,
        EAssetTypePreset::Blueprint,
        EAssetTypePreset::Level,
        EAssetTypePreset::DataAsset,
        EAssetTypePreset::Animation
    };
}

TArray<FTopLevelAssetPath> GetPresetClassPaths(EAssetTypePreset Preset)
{
    const auto Path = [](const TCHAR* InPath)
    {
        return FTopLevelAssetPath(InPath);
    };

    switch (Preset)
    {
        case EAssetTypePreset::StaticMesh:
            return { Path(TEXT("/Script/Engine.StaticMesh")) };

        case EAssetTypePreset::SkeletalMesh:
            return { Path(TEXT("/Script/Engine.SkeletalMesh")) };

        case EAssetTypePreset::Material:
            // MaterialInterface covers Material, MaterialInstanceConstant and MaterialInstanceDynamic;
            // material functions are a separate hierarchy and must be named explicitly.
            return {
                Path(TEXT("/Script/Engine.MaterialInterface")),
                Path(TEXT("/Script/Engine.MaterialFunctionInterface"))
            };

        case EAssetTypePreset::Texture:
            return { Path(TEXT("/Script/Engine.Texture")) };

        case EAssetTypePreset::VFX:
            // Niagara classes live in the Niagara module. They resolve only when the plugin is
            // enabled; unresolved entries are reported rather than silently dropped.
            return {
                Path(TEXT("/Script/Niagara.NiagaraSystem")),
                Path(TEXT("/Script/Niagara.NiagaraEmitter")),
                Path(TEXT("/Script/Engine.ParticleSystem"))
            };

        case EAssetTypePreset::Sound:
            // Audio middleware (FMOD) is a representative case: its assets are included for
            // completeness, but it resolves events by string path outside the UObject graph, so
            // the usage verdict is unreliable by construction - the report marks them Unknown.
            return {
                Path(TEXT("/Script/Engine.SoundBase")),
                Path(TEXT("/Script/FMODStudio.FMODAsset"))
            };

        case EAssetTypePreset::Blueprint:
            return { Path(TEXT("/Script/Engine.Blueprint")) };

        case EAssetTypePreset::Level:
            return { Path(TEXT("/Script/Engine.World")) };

        case EAssetTypePreset::DataAsset:
            return {
                Path(TEXT("/Script/Engine.DataAsset")),
                Path(TEXT("/Script/Engine.DataTable"))
            };

        case EAssetTypePreset::Animation:
            return {
                Path(TEXT("/Script/Engine.AnimationAsset")),
                Path(TEXT("/Script/Engine.AnimBlueprint"))
            };
    }

    return {};
}

void FAssetTypeFilter::AddPreset(EAssetTypePreset Preset)
{
    for (const FTopLevelAssetPath& ClassPath : GetPresetClassPaths(Preset))
    {
        RequestedClasses.AddUnique(ClassPath);
    }

    RequestedDescriptions.AddUnique(LexToString(Preset));
    bCompiled = false;
}

void FAssetTypeFilter::AddRawClass(const FString& ClassPathOrName)
{
    const FString Trimmed = ClassPathOrName.TrimStartAndEnd();
    if (Trimmed.IsEmpty())
    {
        return;
    }

    FTopLevelAssetPath Resolved;

    if (Trimmed.StartsWith(TEXT("/")))
    {
        Resolved.TrySetPath(Trimmed);
    }
    else
    {
        // A short name like "StaticMesh". TryFindType walks loaded classes, which is the only way
        // to accept the shorthand the UI offers without making the user type /Script/Module.Class.
        if (const UClass* Found = UClass::TryFindTypeSlow<UClass>(Trimmed, EFindFirstObjectOptions::ExactClass))
        {
            Resolved = Found->GetClassPathName();
        }
    }

    if (Resolved.IsNull())
    {
        UnresolvedClasses.AddUnique(Trimmed);
        UE_LOG(LogAssetUsageAudit, Warning, TEXT("Class filter '%s' did not resolve to a class; ignoring it."), *Trimmed);
        return;
    }

    RequestedClasses.AddUnique(Resolved);
    RequestedDescriptions.AddUnique(Trimmed);
    bCompiled = false;
}

void FAssetTypeFilter::Compile(IAssetRegistry& AssetRegistry)
{
    ExpandedClasses.Reset();

    if (RequestedClasses.IsEmpty())
    {
        bCompiled = true;
        return;
    }

    TSet<FTopLevelAssetPath> Derived;
    AssetRegistry.GetDerivedClassNames(RequestedClasses, TSet<FTopLevelAssetPath>(), Derived);

    // GetDerivedClassNames returns strict descendants in some builds and includes the seeds in
    // others. Adding the seeds explicitly makes the result the same either way.
    for (const FTopLevelAssetPath& ClassPath : RequestedClasses)
    {
        ExpandedClasses.Add(ClassPath);
    }

    ExpandedClasses.Append(Derived);

    bCompiled = true;
}

bool FAssetTypeFilter::Matches(const FAssetUsageNode& Node) const
{
    if (RequestedClasses.IsEmpty())
    {
        return true;
    }

    ensureMsgf(bCompiled, TEXT("FAssetTypeFilter::Matches called before Compile; subclasses will be missed."));

    if (!Node.ClassPath.IsNull() && ExpandedClasses.Contains(Node.ClassPath))
    {
        return true;
    }

    // The Blueprint case. Asking "is this a StaticMeshActor" of a BP asset only works through the
    // generated class, because the asset's own class is /Script/Engine.Blueprint.
    if (!Node.GeneratedClassPath.IsNull() && ExpandedClasses.Contains(Node.GeneratedClassPath))
    {
        return true;
    }

    return false;
}

FString FAssetTypeFilter::Describe() const
{
    if (RequestedDescriptions.IsEmpty())
    {
        return TEXT("All types");
    }

    return FString::Join(RequestedDescriptions, TEXT(", "));
}
