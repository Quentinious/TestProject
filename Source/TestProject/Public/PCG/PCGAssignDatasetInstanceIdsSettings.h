// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "PCGSettings.h"

#include "PCGAssignDatasetInstanceIdsSettings.generated.h"

/** Assigns a deterministic, zero-based InstanceId attribute in input point order. */
UCLASS(BlueprintType, ClassGroup = (Procedural), meta = (Keywords = "dataset instance id metadata"))
class TESTPROJECT_API UPCGAssignDatasetInstanceIdsSettings : public UPCGSettings
{
	GENERATED_BODY()

public:
	//~Begin UPCGSettings interface
#if WITH_EDITOR
	virtual FName GetDefaultNodeName() const override { return FName(TEXT("AssignDatasetInstanceIds")); }
	virtual FText GetDefaultNodeTitle() const override { return NSLOCTEXT("PCGAssignDatasetInstanceIds", "NodeTitle", "Assign Dataset Instance IDs"); }
	virtual FText GetNodeTooltipText() const override { return NSLOCTEXT("PCGAssignDatasetInstanceIds", "NodeTooltip", "Assigns InstanceId values from zero to the point count minus one, preserving input point order."); }
	virtual EPCGSettingsType GetType() const override { return EPCGSettingsType::Metadata; }
#endif // WITH_EDITOR
	virtual bool UseSeed() const override { return false; }

protected:
	virtual TArray<FPCGPinProperties> InputPinProperties() const override { return Super::DefaultPointInputPinProperties(); }
	virtual TArray<FPCGPinProperties> OutputPinProperties() const override { return Super::DefaultPointOutputPinProperties(); }
	virtual FPCGElementPtr CreateElement() const override;
	//~End UPCGSettings interface
};

class FPCGAssignDatasetInstanceIdsElement : public IPCGElement
{
protected:
	virtual bool ExecuteInternal(FPCGContext* Context) const override;
	virtual bool SupportsBasePointDataInputs(FPCGContext* InContext) const override { return true; }
};
