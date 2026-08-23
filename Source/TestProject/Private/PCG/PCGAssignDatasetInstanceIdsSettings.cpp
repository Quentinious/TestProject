// Copyright Epic Games, Inc. All Rights Reserved.

#include "PCG/PCGAssignDatasetInstanceIdsSettings.h"

#include "PCGContext.h"
#include "Data/PCGBasePointData.h"
#include "Elements/Metadata/PCGMetadataElementCommon.h"
#include "Metadata/Accessors/PCGAttributeAccessorHelpers.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(PCGAssignDatasetInstanceIdsSettings)

#define LOCTEXT_NAMESPACE "PCGAssignDatasetInstanceIdsElement"

namespace PCGAssignDatasetInstanceIds::Constants
{
	const FName InstanceIdAttributeName = TEXT("InstanceId");
}

FPCGElementPtr UPCGAssignDatasetInstanceIdsSettings::CreateElement() const
{
	return MakeShared<FPCGAssignDatasetInstanceIdsElement>();
}

bool FPCGAssignDatasetInstanceIdsElement::ExecuteInternal(FPCGContext* Context) const
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FPCGAssignDatasetInstanceIdsElement::Execute);

	check(Context);

	const TArray<FPCGTaggedData> Inputs = Context->InputData.GetInputsByPin(PCGPinConstants::DefaultInputLabel);

	for (const FPCGTaggedData& Input : Inputs)
	{
		const UPCGBasePointData* InputPointData = Cast<UPCGBasePointData>(Input.Data);
		if (!InputPointData)
		{
			PCGLog::LogErrorOnGraph(LOCTEXT("UnsupportedInput", "Assign Dataset Instance IDs requires Point Data input."), Context);
			continue;
		}

		UPCGBasePointData* OutputPointData = Cast<UPCGBasePointData>(InputPointData->DuplicateData(Context));
		if (!ensure(OutputPointData))
		{
			continue;
		}

		const FPCGAttributePropertySelector InstanceIdSelector = FPCGAttributePropertySelector::CreateAttributeSelector(PCGAssignDatasetInstanceIds::Constants::InstanceIdAttributeName);
		if (!PCGMetadataElementCommon::ClearOrCreateAttribute<int32>(OutputPointData->MutableMetadata(), InstanceIdSelector, 0, Context))
		{
			PCGLog::Metadata::LogFailToCreateAttributeError<int32>(PCGAssignDatasetInstanceIds::Constants::InstanceIdAttributeName, Context);
			continue;
		}

		const int32 NumPoints = OutputPointData->GetNumPoints();
		if (NumPoints == 0)
		{
			FPCGTaggedData& Output = Context->OutputData.TaggedData.Emplace_GetRef(Input);
			Output.Pin = PCGPinConstants::DefaultOutputLabel;
			Output.Data = OutputPointData;
			continue;
		}

		const TUniquePtr<IPCGAttributeAccessor> OutputAccessor = PCGAttributeAccessorHelpers::CreateAccessor(OutputPointData, InstanceIdSelector);
		const TUniquePtr<IPCGAttributeAccessorKeys> OutputKeys = PCGAttributeAccessorHelpers::CreateKeys(OutputPointData, InstanceIdSelector);
		if (!OutputAccessor || !OutputKeys)
		{
			PCGLog::Metadata::LogFailToCreateAccessorError(InstanceIdSelector, Context);
			continue;
		}

		if (OutputKeys->GetNum() != NumPoints)
		{
			PCGLog::LogErrorOnGraph(LOCTEXT("PointKeyCountMismatch", "Assign Dataset Instance IDs could not map one metadata key per point."), Context);
			continue;
		}

		TArray<int32> InstanceIds;
		InstanceIds.SetNumUninitialized(NumPoints);
		for (int32 PointIndex = 0; PointIndex < NumPoints; ++PointIndex)
		{
			InstanceIds[PointIndex] = PointIndex;
		}

		if (!OutputAccessor->SetRange(MakeConstArrayView(InstanceIds), 0, *OutputKeys))
		{
			PCGLog::Metadata::LogFailToSetAttributeError<int32>(InstanceIdSelector, OutputAccessor.Get(), Context);
			continue;
		}

		FPCGTaggedData& Output = Context->OutputData.TaggedData.Emplace_GetRef(Input);
		Output.Pin = PCGPinConstants::DefaultOutputLabel;
		Output.Data = OutputPointData;
	}

	return true;
}

#undef LOCTEXT_NAMESPACE
