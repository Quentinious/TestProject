// Copyright Epic Games, Inc. All Rights Reserved.

#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS

#include "PCG/PCGExportSyntheticPointCloudSettings.h"
#include "PCGContext.h"
#include "PCGComponent.h"
#include "Data/PCGPointArrayData.h"
#include "Dom/JsonObject.h"
#include "HAL/PlatformFileManager.h"
#include "Metadata/PCGMetadata.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/StrongObjectPtr.h"
#include "Tests/GroundTruthBoundsTestUtils.h"

namespace GroundTruthExportTests
{
	UPCGPointArrayData* MakeInstances(const TArray<int32>& Classes, const TArray<int32>& Ids)
	{
		check(Classes.Num() == Ids.Num());
		UPCGPointArrayData* Data = NewObject<UPCGPointArrayData>();
		Data->SetNumPoints(Classes.Num());
		UPCGMetadata* Metadata = Data->MutableMetadata();
		auto* Mesh = Metadata->CreateAttribute<FSoftObjectPath>(TEXT("Mesh"), FSoftObjectPath(TEXT("/Engine/BasicShapes/Cube.Cube")), false, false);
		auto* Class = Metadata->CreateAttribute<int32>(TEXT("ClassId"), 0, false, false);
		auto* Instance = Metadata->CreateAttribute<int32>(TEXT("InstanceId"), 0, false, false);
		Metadata->CreateAttribute<int32>(TEXT("LayerIndex"), 0, false, false);
		auto Entries = Data->GetMetadataEntryValueRange();
		auto Transforms = Data->GetTransformValueRange();
		for (int32 Index = 0; Index < Classes.Num(); ++Index)
		{
			Entries[Index] = Metadata->AddEntry();
			Class->SetValue(Entries[Index], Classes[Index]);
			Instance->SetValue(Entries[Index], Ids[Index]);
			Mesh->SetValue(Entries[Index], FSoftObjectPath(TEXT("/Engine/BasicShapes/Cube.Cube")));
			Transforms[Index] = FTransform(FVector(Index * 200.0, 0, 0));
		}
		return Data;
	}

	TUniquePtr<FPCGContext> Export(UPCGExportSyntheticPointCloudSettings* Settings, const TArray<UPCGBasePointData*>& Batches, UPCGComponent* Component = nullptr)
	{
		FPCGDataCollection Input;
		FPCGTaggedData& SettingsInput = Input.TaggedData.Emplace_GetRef();
		SettingsInput.Data = Settings;
		SettingsInput.Pin = TEXT("Settings");
		for (UPCGBasePointData* Batch : Batches)
		{
			FPCGTaggedData& Item = Input.TaggedData.Emplace_GetRef();
			Item.Data = Batch;
			Item.Pin = PCGPinConstants::DefaultInputLabel;
		}
		Input.ComputeCrcs(false);
		const FPCGElementPtr Element = Settings->GetElement();
		TUniquePtr<FPCGContext> Context(Element->Initialize(FPCGInitializeElementParams(&Input, Component, nullptr)));
		Context->InitializeSettings();
		Context->AsyncState.NumAvailableTasks = 1;
		while (!Element->Execute(Context.Get())) {}
		return Context;
	}

	TSharedPtr<FJsonObject> ReadSummary(const FString& Directory)
	{
		FString Text;
		TSharedPtr<FJsonObject> Summary;
		if (FFileHelper::LoadFileToString(Text, *FPaths::Combine(FPaths::ProjectSavedDir(), Directory, TEXT("summary.json"))))
		{
			FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Summary);
		}
		return Summary;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGroundTruthExportCountsTest, "TestProject.GroundTruth.Export.CountsAndFormats", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGroundTruthExportCountsTest::RunTest(const FString& Parameters)
{
	using namespace GroundTruthExportTests;
	TStrongObjectPtr<UPCGExportSyntheticPointCloudSettings> Settings(NewObject<UPCGExportSyntheticPointCloudSettings>());
	Settings->bExportEnabled = true;
	Settings->OutputSubdirectory = TEXT("Automation/GroundTruthExports/") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
	TStrongObjectPtr<UPCGPointArrayData> BatchA(MakeInstances({0, 3, 3}, {0, 1, 2}));
	TStrongObjectPtr<UPCGPointArrayData> BatchB(MakeInstances({3, 7}, {3, 4}));
	for (const int32 SampleCount : {1, 257})
	{
		Settings->TargetPointCount = SampleCount;
		const auto Context = Export(Settings.Get(), {BatchA.Get(), BatchB.Get()});
		TestEqual(TEXT("Input batches pass through"), Context->OutputData.TaggedData.Num(), 2);
		TestTrue(TEXT("Input object identity preserved"), Context->OutputData.TaggedData[0].Data == BatchA.Get());
		const TSharedPtr<FJsonObject> Summary = ReadSummary(Settings->OutputSubdirectory);
		if (!TestTrue(TEXT("summary.json is valid JSON"), Summary.IsValid())) { return false; }
		TestTrue(TEXT("Unknown scene seed is null"), Summary->Values[TEXT("scene_seed")]->IsNull());
		const auto Classes = Summary->GetObjectField(TEXT("classes"));
		const auto Brick = Classes->GetObjectField(TEXT("brick"));
		TestEqual(TEXT("Actual brick ClassId"), Brick->GetIntegerField(TEXT("class_id")), 3);
		TestEqual(TEXT("Brick count independent of samples and batches"), Brick->GetIntegerField(TEXT("instance_count")), 3);
		TestEqual(TEXT("Unique brick IDs match object count"), Brick->GetIntegerField(TEXT("unique_instance_count")), 3);
		TestTrue(TEXT("Brick identity invariant holds"), Brick->GetBoolField(TEXT("instance_ids_unique")));
		TestEqual(TEXT("Other objects not counted as bricks"), Classes->GetObjectField(TEXT("class_0"))->GetIntegerField(TEXT("instance_count")), 1);
		TestEqual(TEXT("Unknown classes retained"), Classes->GetObjectField(TEXT("class_7"))->GetIntegerField(TEXT("instance_count")), 1);

		const FString Directory = FPaths::Combine(FPaths::ProjectSavedDir(), Settings->OutputSubdirectory);
		TArray<GroundTruthBoundsTests::FRow> Rows;
		if (!GroundTruthBoundsTests::Validate(*this, Directory, {}, 3, 3, Rows)) { return false; }
		FString Txt;
		TestTrue(TEXT("TXT written"), FFileHelper::LoadFileToString(Txt, *FPaths::Combine(Directory, TEXT("scene_0001_points.txt"))));
		TArray<FString> Lines;
		Txt.ParseIntoArrayLines(Lines);
		TestEqual(TEXT("Unchanged TXT schema"), Lines[0], FString(TEXT("X,Y,Z,ClassId,InstanceId,LayerIndex")));
		TestEqual(TEXT("TXT sample count"), Lines.Num(), SampleCount + 1);
		TArray<uint8> Ply;
		TestTrue(TEXT("PLY written"), FFileHelper::LoadFileToArray(Ply, *FPaths::Combine(Directory, TEXT("scene_0001_points.ply"))));
		const FString Header = FString::Printf(TEXT("ply\nformat binary_little_endian 1.0\ncomment generated by TestProject Unreal PCG exporter\nelement vertex %d\nproperty float x\nproperty float y\nproperty float z\nproperty int class_id\nproperty int instance_id\nproperty int layer_index\nend_header\n"), SampleCount);
		FTCHARToUTF8 HeaderUtf8(*Header);
		TestEqual(TEXT("PLY header and binary record size"), Ply.Num(), HeaderUtf8.Length() + SampleCount * 24);
		TestTrue(TEXT("Unchanged PLY schema"), Ply.Num() >= HeaderUtf8.Length() && FMemory::Memcmp(Ply.GetData(), HeaderUtf8.Get(), HeaderUtf8.Length()) == 0);
		Export(Settings.Get(), {BatchA.Get(), BatchB.Get()});
		TArray<uint8> RepeatedPly;
		FFileHelper::LoadFileToArray(RepeatedPly, *FPaths::Combine(Directory, TEXT("scene_0001_points.ply")));
		TestTrue(TEXT("Repeat export remains deterministic"), Ply == RepeatedPly);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGroundTruthExportDuplicatesTest, "TestProject.GroundTruth.Export.DuplicatesAndZeroBricks", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGroundTruthExportDuplicatesTest::RunTest(const FString& Parameters)
{
	using namespace GroundTruthExportTests;
	TStrongObjectPtr<UPCGExportSyntheticPointCloudSettings> Settings(NewObject<UPCGExportSyntheticPointCloudSettings>());
	Settings->bExportEnabled = true;
	Settings->TargetPointCount = 1;
	Settings->OutputSubdirectory = TEXT("Automation/GroundTruthExports/") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
	TStrongObjectPtr<UPCGPointArrayData> Batch(MakeInstances({3, 3, 3}, {10, 10, 11}));
	Export(Settings.Get(), {Batch.Get()});
	auto Summary = ReadSummary(Settings->OutputSubdirectory);
	if (!TestTrue(TEXT("Duplicate IDs still permit existing export"), Summary.IsValid())) { return false; }
	auto Brick = Summary->GetObjectField(TEXT("classes"))->GetObjectField(TEXT("brick"));
	TestEqual(TEXT("Objects are not silently deduplicated"), Brick->GetIntegerField(TEXT("instance_count")), 3);
	TestEqual(TEXT("Unique IDs counted separately"), Brick->GetIntegerField(TEXT("unique_instance_count")), 2);
	TestFalse(TEXT("Broken identity invariant is reported"), Brick->GetBoolField(TEXT("instance_ids_unique")));
	const FString Directory = FPaths::Combine(FPaths::ProjectSavedDir(), Settings->OutputSubdirectory);
	TArray<GroundTruthBoundsTests::FRow> Rows;
	if (!GroundTruthBoundsTests::Validate(*this, Directory, {}, 3, 2, Rows)) { return false; }
	TStrongObjectPtr<UPCGPointArrayData> NoBricks(MakeInstances({0}, {0}));
	Export(Settings.Get(), {NoBricks.Get()});
	Summary = ReadSummary(Settings->OutputSubdirectory);
	if (!TestTrue(TEXT("Replacement summary is readable"), Summary.IsValid())) { return false; }
	Brick = Summary->GetObjectField(TEXT("classes"))->GetObjectField(TEXT("brick"));
	TestEqual(TEXT("No bricks reports zero"), Brick->GetIntegerField(TEXT("instance_count")), 0);
	TestEqual(TEXT("No brick IDs reports zero"), Brick->GetIntegerField(TEXT("unique_instance_count")), 0);
	if (!GroundTruthBoundsTests::Validate(*this, Directory, {}, 0, 0, Rows)) { return false; }

#if PLATFORM_WINDOWS
	// Locked object metadata must roll all four outputs back together.
	TArray<TArray<uint8>> Originals;
	const TArray<const TCHAR*> Names = {TEXT("scene_0001_points.txt"), TEXT("scene_0001_points.ply"), TEXT("instances.csv"), TEXT("summary.json")};
	for (const TCHAR* Name : Names)
	{
		FFileHelper::LoadFileToArray(Originals.Emplace_GetRef(), *FPaths::Combine(Directory, Name));
	}
	for (const TCHAR* LockedName : {TEXT("instances.csv"), TEXT("summary.json")})
	{
		{
			TUniquePtr<IFileHandle> Lock(FPlatformFileManager::Get().GetPlatformFile().OpenRead(*FPaths::Combine(Directory, LockedName)));
			if (!TestTrue(TEXT("Existing metadata locked for rollback test"), Lock.IsValid())) { return false; }
			Settings->TargetPointCount = 2;
			AddExpectedError(TEXT("Could not stage existing output"), EAutomationExpectedErrorFlags::Contains, 1);
			Export(Settings.Get(), {Batch.Get()});
		}
		int32 FileIndex = 0;
		for (const TCHAR* Name : Names)
		{
			TArray<uint8> Current;
			FFileHelper::LoadFileToArray(Current, *FPaths::Combine(Directory, Name));
			TestTrue(FString::Printf(TEXT("Failed metadata replacement preserves %s"), Name), Current == Originals[FileIndex++]);
		}
	}
#endif

	Settings->bExportEnabled = false;
	Export(Settings.Get(), {Batch.Get()});
	TestEqual(TEXT("Disabled export preserves previous summary"), ReadSummary(Settings->OutputSubdirectory)->GetObjectField(TEXT("classes"))->GetObjectField(TEXT("brick"))->GetIntegerField(TEXT("instance_count")), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGroundTruthComponentSeedTest, "TestProject.GroundTruth.Export.ComponentSeed", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGroundTruthComponentSeedTest::RunTest(const FString& Parameters)
{
	using namespace GroundTruthExportTests;
	TStrongObjectPtr<UPCGExportSyntheticPointCloudSettings> Settings(NewObject<UPCGExportSyntheticPointCloudSettings>());
	Settings->bExportEnabled = true;
	Settings->TargetPointCount = 1;
	Settings->Seed = 987654;
	Settings->SamplingSeed = 123456;
	Settings->OutputSubdirectory = TEXT("Automation/GroundTruthExports/") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
	TStrongObjectPtr<UPCGPointArrayData> Batch(MakeInstances({3}, {99}));
	TStrongObjectPtr<UPCGComponent> Component(NewObject<UPCGComponent>());
	for (const int32 Seed : {66, 0, -17})
	{
		Component->Seed = Seed;
		const auto Context = Export(Settings.Get(), {Batch.Get()}, Component.Get());
		TestTrue(TEXT("Execution context retains the supplied component"), Context->ExecutionSource.Get() == Component.Get());
		const auto Summary = ReadSummary(Settings->OutputSubdirectory);
		if (!TestTrue(TEXT("Component-context summary readable"), Summary.IsValid())) { return false; }
		TestEqual(TEXT("Component seed serialized exactly, including zero and negative values"), Summary->GetIntegerField(TEXT("scene_seed")), Seed);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGroundTruthBoundingVolumesTest, "TestProject.GroundTruth.Export.BoundingVolumes", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGroundTruthBoundingVolumesTest::RunTest(const FString& Parameters)
{
	using namespace GroundTruthExportTests;
	TStrongObjectPtr<UPCGExportSyntheticPointCloudSettings> Settings(NewObject<UPCGExportSyntheticPointCloudSettings>());
	Settings->bExportEnabled = true;
	Settings->OutputSubdirectory = TEXT("Automation/GroundTruthExports/") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
	TStrongObjectPtr<UPCGPointArrayData> Batch(MakeInstances({3, 3, 3, 3, 0}, {10, 20, 30, 40, 50}));
	Batch->AllocateProperties(EPCGPointNativeProperties::BoundsMin | EPCGPointNativeProperties::BoundsMax);
	auto Transforms = Batch->GetTransformValueRange();
	Transforms[1] = FTransform(FQuat::Identity, FVector(12345, -6789, 400), FVector(2, 3, 4));
	Transforms[2] = FTransform(FRotator(0, 90, 0), FVector(1000, 2000, 3000), FVector(-2, 3, 0.5));
	Transforms[3] = FTransform(FRotator(0, 45, 0), FVector(-2000, 3000, -4000), FVector(2, 1, 3));
	// Deliberately unrelated spacing/pruning bounds: geometry must still be the 100 cm engine cube.
	auto BoundsMin = Batch->GetBoundsMinValueRange();
	auto BoundsMax = Batch->GetBoundsMaxValueRange();
	for (int32 Index = 0; Index < Batch->GetNumPoints(); ++Index)
	{
		BoundsMin[Index] = FVector(-100000); BoundsMax[Index] = FVector(100000);
	}
	TStrongObjectPtr<UPCGComponent> Component(NewObject<UPCGComponent>());
	Component->Seed = 66;
	const FString Directory = FPaths::Combine(FPaths::ProjectSavedDir(), Settings->OutputSubdirectory);
	const double Diagonal = 300.0 / FMath::Sqrt(2.0);
	const TArray<FVector> ExpectedSizes = {FVector(100), FVector(200, 300, 400), FVector(300, 200, 50), FVector(Diagonal, Diagonal, 300)};
	FString OriginalCsv;
	for (int32 Samples : {1, 257})
	{
		Settings->TargetPointCount = Samples;
		Export(Settings.Get(), {Batch.Get()}, Component.Get());
		TArray<GroundTruthBoundsTests::FRow> Rows;
		if (!GroundTruthBoundsTests::Validate(*this, Directory, 66, 4, 4, Rows) || Rows.Num() != 4) { return false; }
		for (int32 Index = 0; Index < Rows.Num(); ++Index)
		{
			TestEqual(TEXT("InstanceId unchanged"), Rows[Index].InstanceId, (Index + 1) * 10);
			TestTrue(TEXT("Analytic world AABB of cube: translation, rotation, non-uniform/negative scale; no PCG padding"), Rows[Index].SizeCm.Equals(ExpectedSizes[Index], 1.e-8));
		}
		FString Csv;
		FFileHelper::LoadFileToString(Csv, *FPaths::Combine(Directory, TEXT("instances.csv")));
		if (Samples == 1) { OriginalCsv = Csv; }
		else { TestEqual(TEXT("Object metadata independent of surface sampling"), Csv, OriginalCsv); }
	}
	// Flattened geometry can have positive surface area, but cannot supply a positive 3D box.
	Batch->GetTransformValueRange()[0] = FTransform(FQuat::Identity, FVector::ZeroVector, FVector(1, 1, 0));
	AddExpectedError(TEXT("invalid/non-positive geometric bounding dimensions or volume"), EAutomationExpectedErrorFlags::Contains, 1);
	Export(Settings.Get(), {Batch.Get()}, Component.Get());
	FString AfterFailure;
	FFileHelper::LoadFileToString(AfterFailure, *FPaths::Combine(Directory, TEXT("instances.csv")));
	TestEqual(TEXT("Invalid bounding volume abort preserves previous metadata"), AfterFailure, OriginalCsv);
	return true;
}

#endif
