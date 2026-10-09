// Copyright Epic Games, Inc. All Rights Reserved.

#include "PCG/PCGExportSyntheticPointCloudSettings.h"

#include "PCGContext.h"
#include "PCGComponent.h"
#include "Data/PCGBasePointData.h"
#include "Dom/JsonObject.h"
#include "Engine/StaticMesh.h"
#include "HAL/PlatformFileManager.h"
#include "Metadata/Accessors/PCGAttributeAccessorHelpers.h"
#include "Metadata/PCGMetadataAttributeTraits.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Rendering/PositionVertexBuffer.h"
#include "Serialization/JsonSerializer.h"
#include "StaticMeshResources.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(PCGExportSyntheticPointCloudSettings)

#define LOCTEXT_NAMESPACE "PCGExportSyntheticPointCloudElement"

namespace PCGExportSyntheticPointCloud
{
	namespace Constants
	{
		const FName MeshAttributeName = TEXT("Mesh");
		const FName ClassIdAttributeName = TEXT("ClassId");
		const FName InstanceIdAttributeName = TEXT("InstanceId");
		const FName LayerIndexAttributeName = TEXT("LayerIndex");
		// /Game/DIPLOMA: AddAttribute_15 assigns the unique brick_stack dataset class.
		constexpr int32 BrickClassId = 3;
		constexpr double SquareCentimetersToSquareMeters = 0.0001;
		constexpr double CentimetersToMeters = 0.01;
		constexpr double MinTriangleAreaSquareCentimeters = 1.0e-12;
	}

	struct FTriangle
	{
		FVector A = FVector::ZeroVector;
		FVector B = FVector::ZeroVector;
		FVector C = FVector::ZeroVector;
		double CumulativeAreaSquareCentimeters = 0.0;
		int32 ClassId = 0;
		int32 InstanceId = 0;
		int32 LayerIndex = 0;
	};

	struct FSample
	{
		FVector PositionMeters = FVector::ZeroVector;
		int32 ClassId = 0;
		int32 InstanceId = 0;
		int32 LayerIndex = 0;
	};

	struct FPendingOutput
	{
		FString FinalPath;
		FString TemporaryPath;
		FString BackupPath;
		bool bHadOriginal = false;
		bool bCommitted = false;
	};

	bool IsFiniteVector(const FVector& Value)
	{
		return FMath::IsFinite(Value.X) && FMath::IsFinite(Value.Y) && FMath::IsFinite(Value.Z);
	}

	template <typename T>
	bool ExtractRequiredAttribute(
		const UPCGBasePointData* PointData,
		const FName AttributeName,
		const TCHAR* ExpectedType,
		TArray<T>& OutValues,
		FString& OutError)
	{
		const FPCGAttributePropertySelector Selector = FPCGAttributePropertySelector::CreateAttributeSelector(AttributeName);
		const TUniquePtr<const IPCGAttributeAccessor> Accessor = PCGAttributeAccessorHelpers::CreateConstAccessor(PointData, Selector);
		const TUniquePtr<const IPCGAttributeAccessorKeys> Keys = PCGAttributeAccessorHelpers::CreateConstKeys(PointData, Selector);

		if (!Accessor || !Keys)
		{
			OutError = FString::Printf(TEXT("Required attribute '%s' is missing."), *AttributeName.ToString());
			return false;
		}

		if (Accessor->GetUnderlyingType() != PCG::Private::MetadataTypes<T>::Id)
		{
			OutError = FString::Printf(TEXT("Required attribute '%s' must have type %s."), *AttributeName.ToString(), ExpectedType);
			return false;
		}

		if (Keys->GetNum() != PointData->GetNumPoints())
		{
			OutError = FString::Printf(TEXT("Required attribute '%s' does not provide exactly one value per point."), *AttributeName.ToString());
			return false;
		}

		if (!PCGAttributeAccessorHelpers::ExtractAllValues(Accessor.Get(), Keys.Get(), OutValues, EPCGAttributeAccessorFlags::StrictType))
		{
			OutError = FString::Printf(TEXT("Failed to read required attribute '%s'."), *AttributeName.ToString());
			return false;
		}

		return OutValues.Num() == PointData->GetNumPoints();
	}

	bool ExtractMeshPaths(
		const UPCGBasePointData* PointData,
		TArray<FSoftObjectPath>& OutValues,
		FString& OutTypeName,
		FString& OutError)
	{
		const FName AttributeName = Constants::MeshAttributeName;
		const FPCGAttributePropertySelector Selector = FPCGAttributePropertySelector::CreateAttributeSelector(AttributeName);
		const TUniquePtr<const IPCGAttributeAccessor> Accessor = PCGAttributeAccessorHelpers::CreateConstAccessor(PointData, Selector);
		const TUniquePtr<const IPCGAttributeAccessorKeys> Keys = PCGAttributeAccessorHelpers::CreateConstKeys(PointData, Selector);

		if (!Accessor || !Keys)
		{
			OutError = FString::Printf(TEXT("Required attribute '%s' is missing."), *AttributeName.ToString());
			return false;
		}

		const int16 UnderlyingType = Accessor->GetUnderlyingType();
		OutTypeName = PCG::Private::GetTypeName(UnderlyingType);
		const bool bIsSoftObjectPath = UnderlyingType == PCG::Private::MetadataTypes<FSoftObjectPath>::Id;
		const bool bIsString = UnderlyingType == PCG::Private::MetadataTypes<FString>::Id;
		if (!bIsSoftObjectPath && !bIsString)
		{
			OutError = FString::Printf(
				TEXT("Mesh attribute '%s' has unsupported type '%s'. Expected SoftObjectPath or String."),
				*AttributeName.ToString(),
				*OutTypeName);
			return false;
		}

		if (Keys->GetNum() != PointData->GetNumPoints())
		{
			OutError = FString::Printf(
				TEXT("Mesh attribute '%s' (type '%s') does not provide exactly one value per point."),
				*AttributeName.ToString(),
				*OutTypeName);
			return false;
		}

		const EPCGAttributeAccessorFlags ReadFlags = bIsString
			? EPCGAttributeAccessorFlags::AllowConstructible
			: EPCGAttributeAccessorFlags::StrictType;
		if (!PCGAttributeAccessorHelpers::ExtractAllValues(Accessor.Get(), Keys.Get(), OutValues, ReadFlags) || OutValues.Num() != PointData->GetNumPoints())
		{
			OutError = FString::Printf(
				TEXT("Failed to normalize Mesh attribute '%s' from type '%s' to SoftObjectPath."),
				*AttributeName.ToString(),
				*OutTypeName);
			return false;
		}

		return true;
	}

	bool ValidateAndBuildOutputDirectory(const FString& OutputSubdirectory, FString& OutDirectory, FString& OutError)
	{
		FString RelativeDirectory = OutputSubdirectory;
		RelativeDirectory.ReplaceInline(TEXT("\\"), TEXT("/"));
		RelativeDirectory.TrimStartAndEndInline();

		if (RelativeDirectory.IsEmpty() || !FPaths::IsRelative(RelativeDirectory))
		{
			OutError = TEXT("OutputSubdirectory must be a non-empty relative path under the project's Saved directory.");
			return false;
		}

		TArray<FString> PathParts;
		RelativeDirectory.ParseIntoArray(PathParts, TEXT("/"), true);
		for (const FString& PathPart : PathParts)
		{
			if (PathPart == TEXT(".") || PathPart == TEXT(".."))
			{
				OutError = TEXT("OutputSubdirectory may not contain '.' or '..' path components.");
				return false;
			}
		}

		FText PathReason;
		if (!FPaths::ValidatePath(RelativeDirectory, &PathReason))
		{
			OutError = FString::Printf(TEXT("OutputSubdirectory is invalid: %s"), *PathReason.ToString());
			return false;
		}

		FString SavedDirectory = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir());
		FPaths::NormalizeDirectoryName(SavedDirectory);
		FString CandidateDirectory = FPaths::ConvertRelativePathToFull(FPaths::Combine(SavedDirectory, RelativeDirectory));
		FPaths::NormalizeDirectoryName(CandidateDirectory);

		const FString SavedPrefix = SavedDirectory + TEXT("/");
		if (!CandidateDirectory.Equals(SavedDirectory, ESearchCase::IgnoreCase) && !CandidateDirectory.StartsWith(SavedPrefix, ESearchCase::IgnoreCase))
		{
			OutError = TEXT("OutputSubdirectory resolves outside the project's Saved directory.");
			return false;
		}

		OutDirectory = MoveTemp(CandidateDirectory);
		return true;
	}

	bool ValidateBaseFileName(const FString& BaseFileName, FString& OutError)
	{
		if (BaseFileName.IsEmpty() || BaseFileName == TEXT(".") || BaseFileName == TEXT("..") || BaseFileName.Contains(TEXT("/")) || BaseFileName.Contains(TEXT("\\")))
		{
			OutError = TEXT("BaseFileName must be a non-empty file name without path components.");
			return false;
		}

		const FString InvalidCharacters = FPaths::GetInvalidFileSystemChars();
		for (const TCHAR Character : BaseFileName)
		{
			if (InvalidCharacters.Contains(FString::Chr(Character)))
			{
				OutError = TEXT("BaseFileName contains an invalid filesystem character.");
				return false;
			}
		}

		return true;
	}

	FString FormatCoordinate(const double Value)
	{
		FString Result = FString::Printf(TEXT("%.6f"), Value);
		Result.ReplaceInline(TEXT(","), TEXT("."));
		return Result;
	}

	void AppendUInt32LittleEndian(TArray64<uint8>& OutBytes, const uint32 Value)
	{
		OutBytes.Add(static_cast<uint8>(Value & 0xffu));
		OutBytes.Add(static_cast<uint8>((Value >> 8u) & 0xffu));
		OutBytes.Add(static_cast<uint8>((Value >> 16u) & 0xffu));
		OutBytes.Add(static_cast<uint8>((Value >> 24u) & 0xffu));
	}

	void AppendFloatLittleEndian(TArray64<uint8>& OutBytes, const float Value)
	{
		static_assert(sizeof(float) == sizeof(uint32));
		uint32 Bits = 0;
		FMemory::Memcpy(&Bits, &Value, sizeof(float));
		AppendUInt32LittleEndian(OutBytes, Bits);
	}

	bool WriteTxtTemporaryFile(const FString& Path, const TArray<FSample>& Samples)
	{
		FString Text;
		Text.Reserve(44 + Samples.Num() * 80);
		Text.Append(TEXT("X,Y,Z,ClassId,InstanceId,LayerIndex\n"));
		for (const FSample& Sample : Samples)
		{
			Text.Append(FormatCoordinate(Sample.PositionMeters.X));
			Text.AppendChar(TEXT(','));
			Text.Append(FormatCoordinate(Sample.PositionMeters.Y));
			Text.AppendChar(TEXT(','));
			Text.Append(FormatCoordinate(Sample.PositionMeters.Z));
			Text.Appendf(TEXT(",%d,%d,%d\n"), Sample.ClassId, Sample.InstanceId, Sample.LayerIndex);
		}

		return FFileHelper::SaveStringToFile(Text, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	}

	bool WritePlyTemporaryFile(const FString& Path, const TArray<FSample>& Samples)
	{
		const FString Header = FString::Printf(
			TEXT("ply\n")
			TEXT("format binary_little_endian 1.0\n")
			TEXT("comment generated by TestProject Unreal PCG exporter\n")
			TEXT("element vertex %d\n")
			TEXT("property float x\n")
			TEXT("property float y\n")
			TEXT("property float z\n")
			TEXT("property int class_id\n")
			TEXT("property int instance_id\n")
			TEXT("property int layer_index\n")
			TEXT("end_header\n"),
			Samples.Num());

		FTCHARToUTF8 HeaderUtf8(*Header);
		TArray64<uint8> Bytes;
		Bytes.Reserve(static_cast<int64>(HeaderUtf8.Length()) + static_cast<int64>(Samples.Num()) * 24);
		Bytes.Append(reinterpret_cast<const uint8*>(HeaderUtf8.Get()), HeaderUtf8.Length());

		for (const FSample& Sample : Samples)
		{
			AppendFloatLittleEndian(Bytes, static_cast<float>(Sample.PositionMeters.X));
			AppendFloatLittleEndian(Bytes, static_cast<float>(Sample.PositionMeters.Y));
			AppendFloatLittleEndian(Bytes, static_cast<float>(Sample.PositionMeters.Z));
			AppendUInt32LittleEndian(Bytes, static_cast<uint32>(Sample.ClassId));
			AppendUInt32LittleEndian(Bytes, static_cast<uint32>(Sample.InstanceId));
			AppendUInt32LittleEndian(Bytes, static_cast<uint32>(Sample.LayerIndex));
		}

		return FFileHelper::SaveArrayToFile(MakeArrayView(Bytes), *Path);
	}

	bool WriteSummaryTemporaryFile(
		const FString& Path,
		const TOptional<int32>& SceneSeed,
		const TMap<int32, int32>& InstanceCountsByClass,
		const TMap<int32, TSet<int32>>& UniqueInstanceIdsByClass)
	{
		TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		if (SceneSeed.IsSet())
		{
			Root->SetNumberField(TEXT("scene_seed"), SceneSeed.GetValue());
		}
		else
		{
			// Standalone execution without a PCG component has no scene seed.
			Root->SetField(TEXT("scene_seed"), MakeShared<FJsonValueNull>());
		}
		TSharedRef<FJsonObject> Classes = MakeShared<FJsonObject>();
		TArray<int32> ClassIds;
		InstanceCountsByClass.GetKeys(ClassIds);
		ClassIds.AddUnique(Constants::BrickClassId); // Include zero bricks if this class is absent.
		ClassIds.Sort();
		for (const int32 ClassId : ClassIds)
		{
			const int32 InstanceCount = InstanceCountsByClass.FindRef(ClassId);
			const TSet<int32>* UniqueIds = UniqueInstanceIdsByClass.Find(ClassId);
			const int32 UniqueCount = UniqueIds ? UniqueIds->Num() : 0;
			TSharedRef<FJsonObject> Class = MakeShared<FJsonObject>();
			Class->SetNumberField(TEXT("class_id"), ClassId);
			Class->SetNumberField(TEXT("instance_count"), InstanceCount);
			Class->SetNumberField(TEXT("unique_instance_count"), UniqueCount);
			Class->SetBoolField(TEXT("instance_ids_unique"), InstanceCount == UniqueCount);
			Classes->SetObjectField(ClassId == Constants::BrickClassId ? TEXT("brick") : FString::Printf(TEXT("class_%d"), ClassId), Class);
		}
		Root->SetObjectField(TEXT("classes"), Classes);
		FString Json;
		return FJsonSerializer::Serialize(Root, TJsonWriterFactory<>::Create(&Json)) &&
			FFileHelper::SaveStringToFile(Json, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	}

	void CleanupTemporaryFiles(IPlatformFile& PlatformFile, TArray<FPendingOutput>& Outputs)
	{
		for (FPendingOutput& Output : Outputs)
		{
			if (PlatformFile.FileExists(*Output.TemporaryPath))
			{
				PlatformFile.DeleteFile(*Output.TemporaryPath);
			}
		}
	}

	bool RestoreOriginals(IPlatformFile& PlatformFile, TArray<FPendingOutput>& Outputs)
	{
		bool bRestoredAll = true;
		for (FPendingOutput& Output : Outputs)
		{
			if (Output.bCommitted && PlatformFile.FileExists(*Output.FinalPath))
			{
				bRestoredAll &= PlatformFile.DeleteFile(*Output.FinalPath);
			}

			if (Output.bHadOriginal && PlatformFile.FileExists(*Output.BackupPath))
			{
				bRestoredAll &= PlatformFile.MoveFile(*Output.FinalPath, *Output.BackupPath);
			}
		}

		CleanupTemporaryFiles(PlatformFile, Outputs);
		return bRestoredAll;
	}

	bool CommitOutputs(IPlatformFile& PlatformFile, TArray<FPendingOutput>& Outputs, FString& OutError, bool& bOutRollbackSucceeded)
	{
		bOutRollbackSucceeded = true;

		for (FPendingOutput& Output : Outputs)
		{
			if (PlatformFile.FileExists(*Output.FinalPath))
			{
				Output.bHadOriginal = true;
				if (!PlatformFile.MoveFile(*Output.BackupPath, *Output.FinalPath))
				{
					OutError = FString::Printf(TEXT("Could not stage existing output '%s' for replacement."), *Output.FinalPath);
					bOutRollbackSucceeded = RestoreOriginals(PlatformFile, Outputs);
					return false;
				}
			}
		}

		for (FPendingOutput& Output : Outputs)
		{
			if (!PlatformFile.MoveFile(*Output.FinalPath, *Output.TemporaryPath))
			{
				OutError = FString::Printf(TEXT("Could not atomically move temporary output into '%s'."), *Output.FinalPath);
				bOutRollbackSucceeded = RestoreOriginals(PlatformFile, Outputs);
				return false;
			}
			Output.bCommitted = true;
		}

		for (FPendingOutput& Output : Outputs)
		{
			if (Output.bHadOriginal && PlatformFile.FileExists(*Output.BackupPath))
			{
				PlatformFile.DeleteFile(*Output.BackupPath);
			}
		}

		return true;
	}

	FString FormatClassCounts(const TMap<int32, int32>& Counts)
	{
		TArray<int32> ClassIds;
		Counts.GetKeys(ClassIds);
		ClassIds.Sort();

		FString Result;
		for (const int32 ClassId : ClassIds)
		{
			Result.Appendf(TEXT("\nClassId %d = %d"), ClassId, Counts.FindChecked(ClassId));
		}
		return Result;
	}
}

FPCGElementPtr UPCGExportSyntheticPointCloudSettings::CreateElement() const
{
	return MakeShared<FPCGExportSyntheticPointCloudElement>();
}

bool FPCGExportSyntheticPointCloudElement::ExecuteInternal(FPCGContext* Context) const
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FPCGExportSyntheticPointCloudElement::Execute);

	check(Context);
	const UPCGExportSyntheticPointCloudSettings* Settings = Context->GetInputSettings<UPCGExportSyntheticPointCloudSettings>();
	check(Settings);

	const TArray<FPCGTaggedData> Inputs = Context->InputData.GetInputsByPin(PCGPinConstants::DefaultInputLabel);
	for (const FPCGTaggedData& Input : Inputs)
	{
		FPCGTaggedData& Output = Context->OutputData.TaggedData.Emplace_GetRef(Input);
		Output.Pin = PCGPinConstants::DefaultOutputLabel;
	}

	if (!Settings->bExportEnabled)
	{
		return true;
	}

	TOptional<int32> SceneSeed;
	if (const UPCGComponent* Component = Cast<UPCGComponent>(Context->ExecutionSource.Get()))
	{
		if (const UPCGComponent* OriginalComponent = Component->GetOriginalComponent())
		{
			// This is the editable component Seed, not Context->GetSeed() (node seed).
			// Resolve partition-local execution back to the user's original component.
			SceneSeed = OriginalComponent->Seed;
		}
	}

	auto AbortExport = [Context](const FString& Message)
	{
		PCGLog::LogErrorOnGraph(FText::FromString(FString::Printf(TEXT("Export Synthetic Point Cloud aborted: %s"), *Message)), Context);
		return true;
	};

	if (Settings->TargetPointCount < 1)
	{
		return AbortExport(TEXT("TargetPointCount must be at least 1."));
	}

	if (!Settings->bWriteTxt && !Settings->bWritePly)
	{
		return AbortExport(TEXT("At least one output writer (TXT or PLY) must be enabled."));
	}

	FString OutputDirectory;
	FString ValidationError;
	if (!PCGExportSyntheticPointCloud::ValidateAndBuildOutputDirectory(Settings->OutputSubdirectory, OutputDirectory, ValidationError) ||
		!PCGExportSyntheticPointCloud::ValidateBaseFileName(Settings->BaseFileName, ValidationError))
	{
		return AbortExport(ValidationError);
	}

	TArray<PCGExportSyntheticPointCloud::FTriangle> Triangles;
	TMap<int32, int32> InstanceCountsByClass;
	TMap<int32, TSet<int32>> UniqueInstanceIdsByClass;
	TSet<int32> UniqueInstanceIds;
	bool bFoundDuplicateInstanceId = false;
	int32 InputInstanceCount = 0;
	double TotalAreaSquareCentimeters = 0.0;

	for (const FPCGTaggedData& Input : Inputs)
	{
		const UPCGBasePointData* PointData = Cast<UPCGBasePointData>(Input.Data);
		if (!PointData)
		{
			return AbortExport(TEXT("The default input must contain Point Data only."));
		}

		const int32 NumPoints = PointData->GetNumPoints();
		InputInstanceCount += NumPoints;

		TArray<FSoftObjectPath> MeshPaths;
		TArray<int32> ClassIds;
		TArray<int32> InstanceIds;
		TArray<int32> LayerIndices;
		FString MeshAttributeType;
		if (!PCGExportSyntheticPointCloud::ExtractRequiredAttribute(PointData, PCGExportSyntheticPointCloud::Constants::InstanceIdAttributeName, TEXT("int32"), InstanceIds, ValidationError) ||
			!PCGExportSyntheticPointCloud::ExtractRequiredAttribute(PointData, PCGExportSyntheticPointCloud::Constants::ClassIdAttributeName, TEXT("int32"), ClassIds, ValidationError) ||
			!PCGExportSyntheticPointCloud::ExtractRequiredAttribute(PointData, PCGExportSyntheticPointCloud::Constants::LayerIndexAttributeName, TEXT("int32"), LayerIndices, ValidationError) ||
			!PCGExportSyntheticPointCloud::ExtractMeshPaths(PointData, MeshPaths, MeshAttributeType, ValidationError))
		{
			return AbortExport(ValidationError);
		}

		const auto PointTransforms = PointData->GetConstTransformValueRange();
		if (PointTransforms.Num() != NumPoints)
		{
			return AbortExport(TEXT("Point Transform property does not provide exactly one transform per point."));
		}

		for (int32 PointIndex = 0; PointIndex < NumPoints; ++PointIndex)
		{
			const FSoftObjectPath& MeshPath = MeshPaths[PointIndex];
			const int32 ClassId = ClassIds[PointIndex];
			const int32 InstanceId = InstanceIds[PointIndex];
			const int32 LayerIndex = LayerIndices[PointIndex];
			const FTransform& PointTransform = PointTransforms[PointIndex];
			const FString MeshContext = FString::Printf(
				TEXT("Mesh attribute '%s' (type '%s'), InstanceId %d, point %d, value '%s'"),
				*PCGExportSyntheticPointCloud::Constants::MeshAttributeName.ToString(),
				*MeshAttributeType,
				InstanceId,
				PointIndex,
				*MeshPath.ToString());

			InstanceCountsByClass.FindOrAdd(ClassId)++;
			UniqueInstanceIdsByClass.FindOrAdd(ClassId).Add(InstanceId);
			if (UniqueInstanceIds.Contains(InstanceId))
			{
				bFoundDuplicateInstanceId = true;
			}
			else
			{
				UniqueInstanceIds.Add(InstanceId);
			}

			if (MeshPath.IsNull() || !MeshPath.IsValid())
			{
				return AbortExport(MeshContext + TEXT(" is empty or is not a valid SoftObjectPath."));
			}

			UObject* MeshObject = MeshPath.ResolveObject();
			if (!MeshObject)
			{
				MeshObject = MeshPath.TryLoad();
			}

			UStaticMesh* StaticMesh = Cast<UStaticMesh>(MeshObject);
			if (!StaticMesh)
			{
				return AbortExport(MeshContext + TEXT(" could not be resolved/loaded as UStaticMesh."));
			}

			const FStaticMeshRenderData* RenderData = StaticMesh->GetRenderData();
			if (!RenderData || RenderData->LODResources.IsEmpty())
			{
				return AbortExport(MeshContext + TEXT(" has no render data or LOD 0."));
			}

			const FStaticMeshLODResources& LOD = RenderData->LODResources[0];
			const int32 NumVertices = LOD.VertexBuffers.PositionVertexBuffer.GetNumVertices();
			const FIndexArrayView Indices = LOD.IndexBuffer.GetArrayView();
			if (NumVertices <= 0 || Indices.Num() < 3 || Indices.Num() % 3 != 0)
			{
				return AbortExport(MeshContext + TEXT(" has no valid LOD 0 triangle index buffer."));
			}

			const int32 TrianglesBeforeInstance = Triangles.Num();
			for (int32 IndexOffset = 0; IndexOffset < Indices.Num(); IndexOffset += 3)
			{
				const uint32 IndexA = Indices[IndexOffset];
				const uint32 IndexB = Indices[IndexOffset + 1];
				const uint32 IndexC = Indices[IndexOffset + 2];
				if (IndexA >= static_cast<uint32>(NumVertices) || IndexB >= static_cast<uint32>(NumVertices) || IndexC >= static_cast<uint32>(NumVertices))
				{
					return AbortExport(MeshContext + TEXT(" has an out-of-range LOD 0 vertex index."));
				}

				const FVector A = PointTransform.TransformPosition(FVector(LOD.VertexBuffers.PositionVertexBuffer.VertexPosition(IndexA)));
				const FVector B = PointTransform.TransformPosition(FVector(LOD.VertexBuffers.PositionVertexBuffer.VertexPosition(IndexB)));
				const FVector C = PointTransform.TransformPosition(FVector(LOD.VertexBuffers.PositionVertexBuffer.VertexPosition(IndexC)));
				if (!PCGExportSyntheticPointCloud::IsFiniteVector(A) || !PCGExportSyntheticPointCloud::IsFiniteVector(B) || !PCGExportSyntheticPointCloud::IsFiniteVector(C))
				{
					return AbortExport(MeshContext + TEXT(" produced a non-finite world-space vertex."));
				}

				const double Area = 0.5 * FVector::CrossProduct(B - A, C - A).Length();
				if (!FMath::IsFinite(Area))
				{
					return AbortExport(MeshContext + TEXT(" produced a non-finite triangle area."));
				}
				if (Area <= PCGExportSyntheticPointCloud::Constants::MinTriangleAreaSquareCentimeters)
				{
					continue;
				}

				TotalAreaSquareCentimeters += Area;
				if (!FMath::IsFinite(TotalAreaSquareCentimeters))
				{
					return AbortExport(TEXT("The accumulated world-space surface area is non-finite."));
				}

				PCGExportSyntheticPointCloud::FTriangle& Triangle = Triangles.Emplace_GetRef();
				Triangle.A = A;
				Triangle.B = B;
				Triangle.C = C;
				Triangle.CumulativeAreaSquareCentimeters = TotalAreaSquareCentimeters;
				Triangle.ClassId = ClassId;
				Triangle.InstanceId = InstanceId;
				Triangle.LayerIndex = LayerIndex;
			}

			if (Triangles.Num() == TrianglesBeforeInstance)
			{
				return AbortExport(MeshContext + TEXT(" has no non-degenerate LOD 0 triangles after applying its point transform."));
			}
		}
	}

	if (InputInstanceCount == 0 || Triangles.IsEmpty() || TotalAreaSquareCentimeters <= 0.0)
	{
		return AbortExport(TEXT("Input contains no mesh instances with positive surface area."));
	}

	if (bFoundDuplicateInstanceId)
	{
		PCGLog::LogWarningOnGraph(LOCTEXT("DuplicateInstanceIds", "Export Synthetic Point Cloud found duplicate InstanceId values. Export will continue, but instance semantics are not unique."), Context);
	}

	FRandomStream SamplingStream(Settings->SamplingSeed);
	TArray<PCGExportSyntheticPointCloud::FSample> Samples;
	Samples.Reserve(Settings->TargetPointCount);
	TMap<int32, int32> PointCountsByClass;
	FBox BoundsMeters(ForceInit);

	for (int32 SampleIndex = 0; SampleIndex < Settings->TargetPointCount; ++SampleIndex)
	{
		const double AreaSelection = static_cast<double>(SamplingStream.GetFraction()) * TotalAreaSquareCentimeters;
		int32 Lower = 0;
		int32 Upper = Triangles.Num();
		while (Lower < Upper)
		{
			const int32 Middle = Lower + (Upper - Lower) / 2;
			if (AreaSelection < Triangles[Middle].CumulativeAreaSquareCentimeters)
			{
				Upper = Middle;
			}
			else
			{
				Lower = Middle + 1;
			}
		}

		const PCGExportSyntheticPointCloud::FTriangle& Triangle = Triangles[FMath::Min(Lower, Triangles.Num() - 1)];
		const double R1 = FMath::Sqrt(static_cast<double>(SamplingStream.GetFraction()));
		const double R2 = static_cast<double>(SamplingStream.GetFraction());
		const FVector PositionCentimeters = (1.0 - R1) * Triangle.A + R1 * (1.0 - R2) * Triangle.B + R1 * R2 * Triangle.C;

		PCGExportSyntheticPointCloud::FSample& Sample = Samples.Emplace_GetRef();
		Sample.PositionMeters = PositionCentimeters * PCGExportSyntheticPointCloud::Constants::CentimetersToMeters;
		Sample.ClassId = Triangle.ClassId;
		Sample.InstanceId = Triangle.InstanceId;
		Sample.LayerIndex = Triangle.LayerIndex;
		if (!PCGExportSyntheticPointCloud::IsFiniteVector(Sample.PositionMeters))
		{
			return AbortExport(TEXT("Sampling produced a non-finite output position."));
		}

		BoundsMeters += Sample.PositionMeters;
		PointCountsByClass.FindOrAdd(Sample.ClassId)++;
	}

	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	if (!PlatformFile.CreateDirectoryTree(*OutputDirectory))
	{
		return AbortExport(FString::Printf(TEXT("Could not create output directory '%s'."), *OutputDirectory));
	}

	const FString TransactionId = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString TxtPath = FPaths::Combine(OutputDirectory, Settings->BaseFileName + TEXT("_points.txt"));
	const FString PlyPath = FPaths::Combine(OutputDirectory, Settings->BaseFileName + TEXT("_points.ply"));
	const FString SummaryPath = FPaths::Combine(OutputDirectory, TEXT("summary.json"));
	TArray<PCGExportSyntheticPointCloud::FPendingOutput> PendingOutputs;

	if (Settings->bWriteTxt)
	{
		PCGExportSyntheticPointCloud::FPendingOutput& Output = PendingOutputs.Emplace_GetRef();
		Output.FinalPath = TxtPath;
		Output.TemporaryPath = TxtPath + TEXT(".tmp.") + TransactionId;
		Output.BackupPath = TxtPath + TEXT(".bak.") + TransactionId;
		if (!PCGExportSyntheticPointCloud::WriteTxtTemporaryFile(Output.TemporaryPath, Samples))
		{
			PCGExportSyntheticPointCloud::CleanupTemporaryFiles(PlatformFile, PendingOutputs);
			return AbortExport(FString::Printf(TEXT("Failed to write temporary TXT output '%s'."), *Output.TemporaryPath));
		}
	}

	if (Settings->bWritePly)
	{
		PCGExportSyntheticPointCloud::FPendingOutput& Output = PendingOutputs.Emplace_GetRef();
		Output.FinalPath = PlyPath;
		Output.TemporaryPath = PlyPath + TEXT(".tmp.") + TransactionId;
		Output.BackupPath = PlyPath + TEXT(".bak.") + TransactionId;
		if (!PCGExportSyntheticPointCloud::WritePlyTemporaryFile(Output.TemporaryPath, Samples))
		{
			PCGExportSyntheticPointCloud::CleanupTemporaryFiles(PlatformFile, PendingOutputs);
			return AbortExport(FString::Printf(TEXT("Failed to write temporary PLY output '%s'."), *Output.TemporaryPath));
		}
	}

	PCGExportSyntheticPointCloud::FPendingOutput& SummaryOutput = PendingOutputs.Emplace_GetRef();
	SummaryOutput.FinalPath = SummaryPath;
	SummaryOutput.TemporaryPath = SummaryPath + TEXT(".tmp.") + TransactionId;
	SummaryOutput.BackupPath = SummaryPath + TEXT(".bak.") + TransactionId;
	if (!PCGExportSyntheticPointCloud::WriteSummaryTemporaryFile(SummaryOutput.TemporaryPath, SceneSeed, InstanceCountsByClass, UniqueInstanceIdsByClass))
	{
		PCGExportSyntheticPointCloud::CleanupTemporaryFiles(PlatformFile, PendingOutputs);
		return AbortExport(FString::Printf(TEXT("Failed to write temporary summary output '%s'."), *SummaryOutput.TemporaryPath));
	}

	FString CommitError;
	bool bRollbackSucceeded = true;
	if (!PCGExportSyntheticPointCloud::CommitOutputs(PlatformFile, PendingOutputs, CommitError, bRollbackSucceeded))
	{
		if (!bRollbackSucceeded)
		{
			PCGLog::LogWarningOnGraph(LOCTEXT("RollbackFailed", "Export Synthetic Point Cloud could not fully restore the previous output files after a commit failure. Inspect the output directory and transaction backup files."), Context);
		}
		return AbortExport(CommitError);
	}

	const FString TotalAreaMetersText = PCGExportSyntheticPointCloud::FormatCoordinate(
		TotalAreaSquareCentimeters * PCGExportSyntheticPointCloud::Constants::SquareCentimetersToSquareMeters);
	const FString BoundsMinXText = PCGExportSyntheticPointCloud::FormatCoordinate(BoundsMeters.Min.X);
	const FString BoundsMaxXText = PCGExportSyntheticPointCloud::FormatCoordinate(BoundsMeters.Max.X);
	const FString BoundsMinYText = PCGExportSyntheticPointCloud::FormatCoordinate(BoundsMeters.Min.Y);
	const FString BoundsMaxYText = PCGExportSyntheticPointCloud::FormatCoordinate(BoundsMeters.Max.Y);
	const FString BoundsMinZText = PCGExportSyntheticPointCloud::FormatCoordinate(BoundsMeters.Min.Z);
	const FString BoundsMaxZText = PCGExportSyntheticPointCloud::FormatCoordinate(BoundsMeters.Max.Z);
	const FString Summary = FString::Printf(
		TEXT("Export Synthetic Point Cloud completed\n")
		TEXT("InputInstances = %d\n")
		TEXT("ValidTriangles = %d\n")
		TEXT("TotalSurfaceAreaM2 = %s\n")
		TEXT("GeneratedPoints = %d\n")
		TEXT("SamplingSeed = %d\n")
		TEXT("BoundsMeters:\n")
		TEXT("X=%s..%s\n")
		TEXT("Y=%s..%s\n")
		TEXT("Z=%s..%s\n")
		TEXT("Class counts by INSTANCE:%s\n")
		TEXT("Point counts by ClassId:%s\n")
		TEXT("TXT = %s\n")
		TEXT("PLY = %s\n")
		TEXT("Summary = %s"),
		InputInstanceCount,
		Triangles.Num(),
		*TotalAreaMetersText,
		Samples.Num(),
		Settings->SamplingSeed,
		*BoundsMinXText, *BoundsMaxXText,
		*BoundsMinYText, *BoundsMaxYText,
		*BoundsMinZText, *BoundsMaxZText,
		*PCGExportSyntheticPointCloud::FormatClassCounts(InstanceCountsByClass),
		*PCGExportSyntheticPointCloud::FormatClassCounts(PointCountsByClass),
		Settings->bWriteTxt ? *TxtPath : TEXT("disabled"),
		Settings->bWritePly ? *PlyPath : TEXT("disabled"),
		*SummaryPath);
	PCGE_LOG_C(Display, GraphAndLog, Context, FText::FromString(Summary));

	return true;
}

#undef LOCTEXT_NAMESPACE
