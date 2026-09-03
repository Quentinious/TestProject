// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "PCGSettings.h"

#include "PCGExportSyntheticPointCloudSettings.generated.h"

/** Exports semantic surface samples from PCG static-mesh instances while passing the input points through unchanged. */
UCLASS(BlueprintType, ClassGroup = (Procedural), meta = (Keywords = "export synthetic point cloud semantic txt ply"))
class TESTPROJECT_API UPCGExportSyntheticPointCloudSettings : public UPCGSettings
{
	GENERATED_BODY()

public:
	//~Begin UPCGSettings interface
#if WITH_EDITOR
	virtual FName GetDefaultNodeName() const override { return FName(TEXT("ExportSyntheticPointCloud")); }
	virtual FText GetDefaultNodeTitle() const override { return NSLOCTEXT("PCGExportSyntheticPointCloud", "NodeTitle", "Export Synthetic Point Cloud"); }
	virtual FText GetNodeTooltipText() const override { return NSLOCTEXT("PCGExportSyntheticPointCloud", "NodeTooltip", "Exports deterministic semantic surface samples from LOD 0 static-mesh geometry and passes the input points through unchanged."); }
	virtual EPCGSettingsType GetType() const override { return EPCGSettingsType::InputOutput; }
#endif // WITH_EDITOR
	virtual bool UseSeed() const override { return false; }

	/** Explicit safety switch because PCG graphs may regenerate automatically in the Editor. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Export")
	bool bExportEnabled = false;

	/** Exact number of surface samples written to each enabled output. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Sampling", meta = (ClampMin = "1", UIMin = "1"))
	int32 TargetPointCount = 200000;

	/** Seed for the exporter's private random stream; it does not affect PCG point seeds. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Sampling")
	int32 SamplingSeed = 12345;

	/** Output directory relative to the project's Saved directory. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Output")
	FString OutputSubdirectory = TEXT("SyntheticDataset");

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Output")
	FString BaseFileName = TEXT("scene_0001");

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Output")
	bool bWriteTxt = true;

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Output")
	bool bWritePly = true;

protected:
	virtual TArray<FPCGPinProperties> InputPinProperties() const override { return Super::DefaultPointInputPinProperties(); }
	virtual TArray<FPCGPinProperties> OutputPinProperties() const override { return Super::DefaultPointOutputPinProperties(); }
	virtual FPCGElementPtr CreateElement() const override;
	//~End UPCGSettings interface
};

class FPCGExportSyntheticPointCloudElement : public IPCGElement
{
protected:
	virtual bool ExecuteInternal(FPCGContext* Context) const override;
	virtual bool CanExecuteOnlyOnMainThread(FPCGContext* Context) const override { return true; }
	virtual bool IsCacheable(const UPCGSettings* InSettings) const override { return false; }
	virtual bool SupportsBasePointDataInputs(FPCGContext* InContext) const override { return true; }
};
